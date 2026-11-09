import { listen } from '@tauri-apps/api/event';
import { api, esc, icon, refreshIcons, toast, errText } from '../util.js';

// one listener for the session; the page being shown sets the handler
let nupHandler = null;
listen('nup-progress', (ev) => nupHandler && nupHandler(ev.payload));
listen('xci-progress', (ev) => nupHandler && nupHandler(ev.payload));
const nupPct = (p) => (p && p.total ? Math.min(100, Math.floor((100 * p.done) / p.total)) : 0);
const nupMsg = (p) => p?.message || (p && p.total ? `Unpacking the .nup: ${nupPct(p)}%${p.file ? ` (${p.file})` : ''}` : 'Reading the .nup...');

const MAX_LOG = 600;

function candidateHtml(c, chosen) {
  const title = c.kind === 'base' ? 'Base game' : c.kind === 'update' ? `Update ${esc(c.version)}` : 'This folder';
  const used = chosen && chosen.path === c.path;
  const unneeded = chosen && !used && !c.ok;
  const chip = used
    ? `<span class="chip ok">${icon('circle-check')}Will be used</span>`
    : c.ok ? '<span class="chip">Valid</span>'
    : unneeded ? '<span class="chip">Not needed</span>'
    : `<span class="chip danger">${icon('circle-x')}Not usable</span>`;
  return `
    <div class="candidate ${used ? 'chosen' : ''}">
      <div class="row between"><h3>${title}</h3>${chip}</div>
      <div class="path">${esc(c.path)}</div>
      ${unneeded ? '' : `<ul class="checks">${c.checks.map((k) => `<li class="${k.ok ? 'ok' : 'bad'}">${icon(k.ok ? 'circle-check' : 'circle-x')}${esc(k.label)}</li>`).join('')}</ul>`}
    </div>`;
}

function toolsHtml(tools) {
  const missing = tools.filter((t) => !t.ok);
  return `
    <div class="card">
      <h2>${icon('list-checks')}Build tools on this PC</h2>
      <p class="dim">Setup compiles the game on your PC from your own dump, so it needs a C++ toolchain. ${missing.length ? `${missing.length} missing: install ${missing.length > 1 ? 'them' : 'it'}, then come back.` : 'Everything was found.'}</p>
      <div class="tools">
        ${tools.map((t) => `<span class="${t.ok ? 'ok' : 'bad'}">${icon(t.ok ? 'circle-check' : 'circle-x')} ${esc(t.name)}</span><span class="${t.ok ? 'faint' : 'dim'}">${esc(t.ok ? t.found : t.hint)}</span>`).join('')}
      </div>
    </div>`;
}

function stepsHtml(s) {
  const p = s.progress;
  const running = p.state === 'running';
  const pct = p.total ? Math.floor((100 * p.done) / p.total) : 0;
  const label = s.complete ? 'Check again' : s.steps.some((x) => x.state === 'done') ? 'Continue setup' : 'Set up';
  const stepIcon = (x) => (x.state === 'done' ? 'circle-check' : x.state === 'running' ? 'loader-circle' : 'circle');
  return `
    <div class="card" id="steps-card">
      <h2>${icon('hammer')}Set up</h2>
      <div class="stack">
        <ul class="steps">${s.steps.map((x) => `<li class="${x.state}">${icon(stepIcon(x), x.state === 'running' ? 'spin' : '')}${esc(x.title)}</li>`).join('')}</ul>
        ${running ? `<p class="dim">${esc(p.message)}</p><progress max="100" ${p.total ? `value="${pct}"` : ''}></progress>
          <p class="faint">${p.total ? `${p.done} of ${p.total}. ` : ''}Pause stops the tools; finished work is kept and Set up continues from there.</p>` : ''}
        ${p.state === 'error' ? `<div class="banner danger">${icon('circle-x')}<div style="white-space: pre-wrap">${esc(p.message)}</div></div>` : ''}
        ${p.state === 'cancelled' || p.state === 'done' ? `<p class="dim">${esc(p.message)}</p>` : ''}
        ${!running && !s.complete && p.state !== 'error' ? `<p class="faint">The first run takes a long time: the recompile of the game code is the biggest step (an hour or more on 8 cores, with ${s.jobs} build processes here) and needs about 30 GB free. Later runs only redo what changed.</p>` : ''}
        <div class="row">
          ${running
            ? `<button class="btn" id="setup-cancel">${icon('circle-x')}Pause</button>`
            : `<button class="btn ${s.complete ? '' : 'primary'}" id="setup-run">${icon('hammer')}${label}</button>`}
          ${s.complete && !running ? `<button class="btn primary" id="to-play">${icon('play')}Go to Play</button>` : ''}
          <button class="btn quiet" id="open-log">${icon('file-text')}Open full log</button>
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
  let importBusy = false;
  let importProgress = null;
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
      el.innerHTML = `<div class="page"><div class="hero"><h1>Setup</h1></div>
        <div class="banner danger">${icon('circle-x')}<div>The repository was not found next to the launcher. Build the launcher inside a checkout of this project (launcher/), or set the repository folder in Settings.</div></div></div>`;
      refreshIcons();
      return;
    }
    el.innerHTML = `
      <div class="page">
        <div class="hero">
          <h1>${ctx.firstRun ? 'Welcome. Set up from your own dump.' : 'Setup'}</h1>
          <p class="dim">This project contains no game files and no game code. Point it at your own legally dumped, decrypted copy of the game: setup reads it (it never writes to it), recompiles the game code on this PC and translates the shaders, all on this PC.</p>
        </div>

        <div class="card">
          <h2>${icon('folder-search')}Your dump</h2>
          <div class="stack">
            <div class="row">
              <input type="text" id="path" spellcheck="false" placeholder="folder with exefs/ and romfs/" value="${esc(path)}" />
              <button class="btn" id="browse" ${importBusy || status.progress.state === 'running' ? 'disabled' : ''}>${icon('folder-open')}Browse</button>
              <button class="btn" id="browse-nup" ${importBusy || status.progress.state === 'running' ? 'disabled' : ''}>${icon('file-archive')}.nup</button>
              <button class="btn" id="browse-xci" ${importBusy || status.progress.state === 'running' ? 'disabled' : ''}>${icon('file-archive')}.xci</button>
            </div>
            ${importBusy ? `<p class="dim" id="nup-msg">${esc(nupMsg(importProgress))}</p><progress max="100" ${importProgress?.total ? `value="${nupPct(importProgress)}"` : ''}></progress><button class="btn" id="cancel-import">Cancel import</button>` : ''}
            <p class="faint">Or pick a <code>.nup</code> package (the NUP format of github.com/a55uka/nup): it is unpacked once into a <code>nup_dump</code> folder and used from there, so it needs the free space of the whole game.</p>
            <p class="faint">XCI import needs your own <code>prod.keys</code> and a separately installed hactool that supports the archive. You may also select an update NSP. It creates a decrypted copy and needs space for the archive contents plus the full extracted game. No keys or extractor are downloaded.</p>
            <p class="faint">Either the folder holding <code>exefs/</code> and <code>romfs/</code> of the supported update, or a dump root with <code>base/</code> and <code>updates/&lt;version&gt;/</code>. The ExeFS needs main, sdk, subsdk0, subsdk1, subsdk2 and rtld. DLC you dumped goes in <code>dlc/&lt;title id&gt;/romfs</code> of the same folder (a <code>.nup</code> with add-ons is unpacked that way).</p>
            ${status.package ? '<p class="faint">Folder/NUP setup is self-contained. Optional XCI import requires hactool and your own keys.</p>' : ''}
          </div>
        </div>

        ${report && report.errors.length ? `<div class="banner danger">${icon('circle-x')}<div>${report.errors.map(esc).join('<br>')}</div></div>` : ''}
        ${report && report.candidates.length ? `<div class="stack">${report.candidates.map((c) => candidateHtml(c, report.chosen)).join('')}</div>` : ''}
        ${report && report.dlc && report.dlc.length ? `<div class="banner info">${icon('package')}<div>DLC from your dump: ${report.dlc.map(esc).join(', ')}</div></div>` : ''}

        <div class="row">
          <button class="btn ${ok && saved !== path ? 'primary' : ''}" id="use" ${ok && !importBusy && status.progress.state !== 'running' ? '' : 'disabled'}>${icon('circle-check')}Use these game files</button>
        </div>

        ${status.package ? "" : toolsHtml(status.tools)}
        ${ok && saved === path && !importBusy ? stepsHtml(status) : ''}

        <div class="card">
          <h2>${icon('terminal')}Log</h2>
          <div class="logbox" id="log"></div>
        </div>
      </div>`;
    refreshIcons();
    drawLog();
    el.querySelector('#cancel-import')?.addEventListener('click', () => api('cancel_setup'));
    const input = el.querySelector('#path');
    const check = async () => {
      path = input.value.trim();
      report = path ? await api('validate_dump', { path }).catch((e) => ({ errors: [errText(e)], candidates: [], chosen: null })) : null;
      draw();
    };
    input.addEventListener('change', check);
    input.addEventListener('keydown', (e) => e.key === 'Enter' && check());
    el.querySelector('#browse').addEventListener('click', async () => {
      const picked = await api('pick_folder', { title: 'Choose your decrypted dump folder', start: input.value || null });
      if (picked) {
        input.value = picked;
        check();
      }
    });
    el.querySelector('#browse-nup').addEventListener('click', async () => {
      const picked = await api('pick_nup', { start: null });
      if (!picked) return;
      importBusy = true;
      importProgress = null;
      draw();
      try {
        path = await api('import_nup', { path: picked });
        report = await api('validate_dump', { path });
      } catch (e) {
        toast(errText(e), 'error');
      }
      importBusy = false;
      draw();
    });
    el.querySelector('#browse-xci').addEventListener('click', async () => {
      try {
        const selection = await api('pick_xci');
        if (!selection) return;
        importBusy = true;
        importProgress = { message: 'Starting XCI import...' };
        draw();
        const imported = await api('import_xci', { selection });
        const checked = await api('validate_dump', { path: imported });
        path = imported;
        report = checked;
      } catch (e) {
        toast(errText(e), 'error');
      } finally {
        importBusy = false;
        draw();
      }
    });
    el.querySelector('#use').addEventListener('click', async () => {
      try {
        await api('save_dump', { path });
        saved = path;
        status = await api('setup_status');
        toast('Game files saved. Next: Set up.', 'ok');
        draw();
      } catch (e) {
        toast(errText(e), 'error');
      }
    });
    bindSteps();
  }

  nupHandler = (p) => {
    importProgress = p;
    const msg = el.querySelector('#nup-msg');
    if (msg) {
      msg.textContent = nupMsg(p);
      const bar = msg.nextElementSibling;
      if (bar && p?.total) bar.value = nupPct(p);
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
