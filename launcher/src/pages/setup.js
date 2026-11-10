import { listen } from '@tauri-apps/api/event';
import { api, esc, icon, refreshIcons, toast, errText } from '../util.js';
import { t } from '../i18n.js';
import { checkText, dumpText, stepTitle } from '../rusttext.js';

// one listener for the session; the page being shown sets the handler
let nupHandler = null;
listen('nup-progress', (ev) => nupHandler && nupHandler(ev.payload));
const nupPct = (p) => (p && p.total ? Math.min(100, Math.floor((100 * p.done) / p.total)) : 0);
const nupMsg = (p) =>
  p && p.total
    ? t(p.file ? 'setup.unpackingNupFile' : 'setup.unpackingNup', { pct: nupPct(p), file: p.file || '' })
    : t('setup.readingNup');

const MAX_LOG = 600;

/*
 * The step titles, the check labels and the dump messages come from the Rust side (setup.rs, dump.rs) in English and are
 * translated in rusttext.js; `checkText` keeps a label it does not know as it came.
 */
function checkLabel(label) {
  const text = checkText(label);
  return text === label ? (label === 'exefs/main is decrypted' || /^supported version \(/.test(label) ? text : null) : text;
}
const dumpError = (s) => dumpText(s);
const progressMsg = (m) => dumpText(m);

function candidateHtml(c, chosen) {
  const title = c.kind === 'base' ? t('common.baseGame') : c.kind === 'update' ? t('common.update', { version: c.version }) : t('common.folder');
  const used = chosen && chosen.path === c.path;
  const unneeded = chosen && !used && !c.ok;
  const chip = used
    ? `<span class="chip ok">${icon('circle-check')}${esc(t('setup.willBeUsed'))}</span>`
    : c.ok ? `<span class="chip">${esc(t('setup.valid'))}</span>`
    : unneeded ? `<span class="chip">${esc(t('setup.notNeeded'))}</span>`
    : `<span class="chip danger">${icon('circle-x')}${esc(t('setup.notUsable'))}</span>`;
  return `
    <div class="candidate ${used ? 'chosen' : ''}">
      <div class="row between"><h3>${esc(title)}</h3>${chip}</div>
      <div class="path">${esc(c.path)}</div>
      ${unneeded ? '' : `<ul class="checks">${c.checks.map((k) => `<li class="${k.ok ? 'ok' : 'bad'}">${icon(k.ok ? 'circle-check' : 'circle-x')}${esc(checkLabel(k.label) || k.label)}</li>`).join('')}</ul>`}
    </div>`;
}

function toolsHtml(tools) {
  const missing = tools.filter((tool) => !tool.ok);
  const tail = !missing.length
    ? t('setup.toolsAllFound')
    : missing.length === 1
      ? t('setup.toolsMissingOne')
      : t('setup.toolsMissingMany', { count: missing.length });
  return `
    <div class="card">
      <h2>${icon('list-checks')}${esc(t('setup.toolsTitle'))}</h2>
      <p class="dim">${esc(t('setup.toolsIntro') + tail)}</p>
      <div class="tools">
        ${tools.map((tool) => `<span class="${tool.ok ? 'ok' : 'bad'}">${icon(tool.ok ? 'circle-check' : 'circle-x')} ${esc(tool.name)}</span><span class="${tool.ok ? 'faint' : 'dim'}">${esc(tool.ok ? tool.found : tool.hint)}</span>`).join('')}
      </div>
    </div>`;
}


function stepsHtml(s) {
  const p = s.progress;
  const running = p.state === 'running';
  const pct = p.total ? Math.floor((100 * p.done) / p.total) : 0;
  const label = s.complete ? t('setup.checkAgain') : s.steps.some((x) => x.state === 'done') ? t('setup.continueSetup') : t('setup.setUp');
  const stepIcon = (x) => (x.state === 'done' ? 'circle-check' : x.state === 'running' ? 'loader-circle' : 'circle');
  return `
    <div class="card" id="steps-card">
      <h2>${icon('hammer')}${esc(t('setup.setUp'))}</h2>
      <div class="stack">
        <ul class="steps">${s.steps.map((x) => `<li class="${x.state}">${icon(stepIcon(x), x.state === 'running' ? 'spin' : '')}${esc(stepTitle(x))}</li>`).join('')}</ul>
        ${running ? `<p class="dim">${esc(progressMsg(p.message))}</p><progress max="100" ${p.total ? `value="${pct}"` : ''}></progress>
          <p class="faint">${esc((p.total ? t('setup.progressDone', { done: p.done, total: p.total }) : '') + t('setup.pauseHint'))}</p>` : ''}
        ${p.state === 'error' ? `<div class="banner danger">${icon('circle-x')}<div style="white-space: pre-wrap">${esc(progressMsg(p.message))}</div></div>` : ''}
        ${p.state === 'cancelled' || p.state === 'done' ? `<p class="dim">${esc(progressMsg(p.message))}</p>` : ''}
        ${!running && !s.complete && p.state !== 'error' ? `<p class="faint">${esc(t('setup.firstRunHint', { jobs: s.jobs }))}</p>` : ''}
        <div class="row">
          ${running
            ? `<button class="btn" id="setup-cancel">${icon('circle-x')}${esc(t('setup.pause'))}</button>`
            : `<button class="btn ${s.complete ? '' : 'primary'}" id="setup-run" ${s.cpu_problem ? 'disabled' : ''}>${icon('hammer')}${esc(label)}</button>`}
          ${s.complete && !running ? `<button class="btn primary" id="to-play">${icon('play')}${esc(t('setup.goToPlay'))}</button>` : ''}
          <button class="btn quiet" id="open-log">${icon('file-text')}${esc(t('setup.openFullLog'))}</button>
        </div>
      </div>
    </div>`;
}

export async function render(el, ctx) {
  const [cfg, ov] = await Promise.all([api('get_config'), api('get_overview')]);
  let path = cfg.dump_dir || '';
  let saved = path;
  let report = null;
  let status = null;
  let log = [];
  let nupBusy = false;
  let nupProgress = null;
  const refresh = async () => {
    report = path && ov.repo_ok ? await api('validate_dump', { path }) : null;
    status = ov.repo_ok ? await api('setup_status') : null;
    if (status) log = status.log.slice();
  };
  await refresh();

  function drawLog() {
    const box = el.querySelector('#log');
    if (!box) return;
    const atEnd = box.scrollTop + box.clientHeight >= box.scrollHeight - 8;
    box.textContent = log.join('\n');
    if (atEnd) box.scrollTop = box.scrollHeight;
  }

  function bindSteps() {
    el.querySelector('#setup-run')?.addEventListener('click', async () => {
      try {
        await api('start_setup');
        status = await api('setup_status');
        draw();
      } catch (e) {
        toast(errText(e), 'error');
      }
    });
    el.querySelector('#setup-cancel')?.addEventListener('click', () => api('cancel_setup'));
    el.querySelector('#to-play')?.addEventListener('click', () => ctx.go('play'));
    el.querySelector('#open-log')?.addEventListener('click', () => api('open_setup_log').catch((e) => toast(errText(e), 'error')));
  }

  function draw() {
    const ok = report && report.chosen;
    if (!ov.repo_ok) {
      el.innerHTML = `<div class="page"><div class="hero"><h1>${esc(t('setup.title'))}</h1></div>
        <div class="banner danger">${icon('circle-x')}<div>${esc(t('setup.repoMissing'))}</div></div></div>`;
      refreshIcons();
      return;
    }
    el.innerHTML = `
      <div class="page">
        <div class="hero">
          <h1>${esc(ctx.firstRun ? t('setup.titleFirstRun') : t('setup.title'))}</h1>
          <p class="dim">${esc(t('setup.intro'))}</p>
        </div>

        <div class="card">
          <h2>${icon('folder-search')}${esc(t('setup.yourDump'))}</h2>
          <div class="stack">
            <div class="row">
              <input type="text" id="path" spellcheck="false" placeholder="${esc(t('setup.pathPlaceholder'))}" value="${esc(path)}" />
              <button class="btn" id="browse" ${nupBusy ? 'disabled' : ''}>${icon('folder-open')}${esc(t('common.browse'))}</button>
              <button class="btn" id="browse-nup" ${nupBusy ? 'disabled' : ''}>${icon('file-archive')}${esc(t('setup.nup'))}</button>
            </div>
            ${nupBusy ? `<p class="dim" id="nup-msg">${esc(nupMsg(nupProgress))}</p><progress max="100" value="${nupPct(nupProgress)}"></progress>` : ''}
            <p class="faint">${esc(t('setup.nupHint'))}</p>
            <p class="faint">${esc(t('setup.dumpHint'))}</p>
            ${status.package ? `<p class="faint">${esc(t('setup.packageComplete'))}</p>` : ''}
          </div>
        </div>

        ${report && report.errors.length ? `<div class="banner danger">${icon('circle-x')}<div>${report.errors.map((e) => esc(dumpError(e))).join('<br>')}</div></div>` : ''}
        ${report && report.candidates.length ? `<div class="stack">${report.candidates.map((c) => candidateHtml(c, report.chosen)).join('')}</div>` : ''}
        ${report && report.dlc && report.dlc.length ? `<div class="banner info">${icon('package')}<div>${esc(t('setup.dlcFound', { list: report.dlc.join(', ') }))}</div></div>` : ''}

        <div class="row">
          <button class="btn ${ok && saved !== path ? 'primary' : ''}" id="use" ${ok && status.progress.state !== 'running' ? '' : 'disabled'}>${icon('circle-check')}${esc(t('setup.useFiles'))}</button>
        </div>

        ${status.cpu_problem ? `<div class="banner danger" id="cpu-problem">${icon('circle-x')}<div><strong>${esc(t('setup.cpuProblem'))}</strong> ${esc(status.cpu_problem)}</div></div>` : ''}

        ${status.package ? '' : toolsHtml(status.tools)}
        ${ok && saved === path ? stepsHtml(status) : ''}

        <div class="card">
          <h2>${icon('terminal')}${esc(t('setup.log'))}</h2>
          <div class="logbox" id="log"></div>
        </div>
      </div>`;
    refreshIcons();
    drawLog();
    const input = el.querySelector('#path');
    const check = async () => {
      path = input.value.trim();
      report = path ? await api('validate_dump', { path }).catch((e) => ({ errors: [errText(e)], candidates: [], chosen: null })) : null;
      draw();
    };
    input.addEventListener('change', check);
    input.addEventListener('keydown', (e) => e.key === 'Enter' && check());
    el.querySelector('#browse').addEventListener('click', async () => {
      const picked = await api('pick_folder', { title: t('setup.chooseDumpTitle'), start: input.value || null });
      if (picked) {
        input.value = picked;
        check();
      }
    });
    el.querySelector('#browse-nup').addEventListener('click', async () => {
      const picked = await api('pick_nup', { start: null });
      if (!picked) return;
      nupBusy = true;
      nupProgress = null;
      draw();
      try {
        path = await api('import_nup', { path: picked });
        report = await api('validate_dump', { path });
      } catch (e) {
        toast(errText(e), 'error');
      }
      nupBusy = false;
      draw();
    });
    el.querySelector('#use').addEventListener('click', async () => {
      try {
        await api('save_dump', { path });
        saved = path;
        status = await api('setup_status');
        toast(t('setup.filesSaved'), 'ok');
        draw();
      } catch (e) {
        toast(errText(e), 'error');
      }
    });
    bindSteps();
  }

  nupHandler = (p) => {
    nupProgress = p;
    const msg = el.querySelector('#nup-msg');
    if (msg) {
      msg.textContent = nupMsg(p);
      const bar = msg.nextElementSibling;
      if (bar) bar.value = nupPct(p);
    }
  };
  ctx.onLog((lines) => {
    log.push(...lines);
    if (log.length > MAX_LOG) log = log.slice(log.length - MAX_LOG);
    drawLog();
  });
  ctx.onSetup(async (p) => {
    if (p.state === 'running' && status && status.progress.state === 'running' && status.progress.step === p.step) {
      status = { ...status, progress: p };
    } else {
      status = await api('setup_status');
    }
    const card = el.querySelector('#steps-card');
    if (card) {
      card.outerHTML = stepsHtml(status);
      refreshIcons();
      bindSteps();
    } else {
      draw();
    }
  });
  draw();
}
