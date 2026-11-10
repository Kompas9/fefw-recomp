import { getVersion } from '@tauri-apps/api/app';
import { api, esc, icon, refreshIcons, toast, errText } from '../util.js';
import { upd, checkNow, available } from '../update.js';
import { t } from '../i18n.js';
import { updText } from '../rusttext.js';

const mib = (b) => (b / (1024 * 1024)).toFixed(0);

/* The update progress lines and the end-of-update sentence come from update.rs in English; rusttext.js translates them. */
const updMessage = (m) => (m ? updText({ message: m }) : '');

/** The release notes from their "Changes" part on (the install steps above it are for new players), as light HTML. */
function notesHtml(md) {
  const lines = md.split('\n');
  const from = lines.findIndex((l) => /^(#+\s*)?(\*\*)?(changes|what's new|new)\b/i.test(l.trim()));
  const inline = (s) =>
    esc(s)
      .replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>')
      .replace(/`([^`]+)`/g, '<code>$1</code>')
      .replace(/\[([^\]]+)\]\([^)]+\)/g, '$1');
  let html = '';
  let list = false;
  for (const raw of lines.slice(from >= 0 ? from : 0)) {
    const l = raw.trim();
    const item = l.match(/^[-*]\s+(.*)$/);
    if (item && !list) (html += '<ul>'), (list = true);
    if (!item && list) (html += '</ul>'), (list = false);
    if (item) html += `<li>${inline(item[1])}</li>`;
    else if (/^#+\s/.test(l)) html += `<p><strong>${inline(l.replace(/^#+\s*/, ''))}</strong></p>`;
    else if (l) html += `<p>${inline(l)}</p>`;
  }
  return html + (list ? '</ul>' : '');
}

function updateCard(cfg) {
  const c = upd.check;
  const s = upd.status;
  const running = s.state === 'running';
  let body;
  if (c && !c.package) {
    body = `<p class="dim">${esc(t('about.checkout'))}</p>`;
  } else if (s.state === 'done') {
    // The pack text is plain, and the emphasis is markup added here, so a pack can never inject markup.
    const reason = s.rebuild_reason ? ` (${s.rebuild_reason})` : '';
    const rebuild =
      s.rebuild === true
        ? `<div class="banner warn">${icon('hammer')}<div><strong>${esc(t('about.rebuildNeeded'))}</strong>${esc(
            t('about.rebuildNeededRest').replace('{reason}', reason),
          )}</div></div>`
        : s.rebuild === false
          ? `<div class="banner ok">${icon('circle-check')}<div>${esc(t('about.rebuildNotNeeded'))}</div></div>`
          : '';
    body = `<div class="banner ok">${icon('circle-check')}<div>${esc(updMessage(s.message))}</div></div>${rebuild}
      <div class="row"><button class="btn primary" id="restart">${icon('rotate-cw')}${esc(t('about.restartLauncher'))}</button></div>`;
  } else if (running) {
    const pct = s.total ? Math.floor((100 * s.done) / s.total) : null;
    body = `<p class="dim">${esc(updMessage(s.message))}</p><progress max="100" ${pct === null ? '' : `value="${pct}"`}></progress>
      <div class="row"><button class="btn quiet" id="cancel" ${s.phase === 'download' ? '' : 'disabled'}>${esc(t('common.cancel'))}</button></div>`;
  } else if (available()) {
    const r = c.latest;
    body = `<p><strong>${esc(t('about.available', { version: r.version }))}</strong> <span class="faint">${esc(t('about.availableMeta', { current: c.current, size: mib(r.size) }))}</span></p>
      ${r.notes ? `<div class="notes">${notesHtml(r.notes)}</div>` : ''}
      ${r.page ? `<p class="faint">${esc(t('about.fullNotes'))}${esc(r.page)}</p>` : ''}
      ${s.state === 'error' ? `<div class="banner danger">${icon('circle-x')}<div>${esc(updMessage(s.message))}</div></div>` : ''}
      <p class="faint">${esc(t('about.downloadHint'))}</p>
      <div class="row"><button class="btn primary" id="update">${icon('download')}${esc(t('about.updateTo', { version: r.version }))}</button></div>`;
  } else {
    const line = upd.checking
      ? `${icon('loader-circle', 'spin')} ${esc(t('about.checking'))}`
      : upd.error
        ? `<span class="chip danger">${icon('circle-alert')}${esc(upd.error)}</span>`
        : c
          ? esc(t('about.latest', { version: c.current }))
          : esc(t('about.notChecked'));
    body = `<p class="dim">${line}</p>`;
  }
  return `
    <div class="card" id="updates">
      <h2>${icon('circle-arrow-up')}${esc(t('about.updates'))}</h2>
      <div class="stack">${body}</div>
      <div class="row wrap update-foot">
        <button class="btn" id="check" ${upd.checking || running ? 'disabled' : ''}>${icon('refresh-cw')}${esc(t('about.checkNow'))}</button>
        <label class="switch"><input type="checkbox" id="auto" ${cfg.skip_update_check ? '' : 'checked'}> ${esc(t('about.checkOnStart'))}</label>
      </div>
    </div>`;
}

export async function render(el) {
  let version = '';
  try {
    version = await getVersion();
  } catch {
    version = 'dev';
  }
  let cfg = await api('get_config');
  upd.status = await api('update_status');

  function draw() {
    el.innerHTML = `
    <div class="page">
      <div class="hero">
        <h1>${esc(t('about.title'))}</h1>
        <p class="dim">${esc(t('about.intro'))}</p>
      </div>
      ${updateCard(cfg)}
      <div class="card">
        <h2>${icon('info')}${esc(t('brand.name'))} ${esc(t('about.launcherWord'))}</h2>

        <dl class="kv">
          <dt>${esc(t('about.version'))}</dt><dd>${esc(version)}</dd>
          <dt>${esc(t('about.licence'))}</dt><dd>GPL-3.0-or-later</dd>
          <dt>${esc(t('about.builtWith'))}</dt><dd>Tauri 2, Lucide icons (ISC)</dd>
          <dt>${esc(t('about.credits'))}</dt><dd>${esc(t('about.creditsText'))}</dd>
        </dl>
      </div>
      <div class="banner warn">
        ${icon('triangle-alert')}
        <div>${esc(t('about.legal'))}</div>
      </div>
    </div>`;
    refreshIcons();
    el.querySelector('#check')?.addEventListener('click', () => checkNow());
    el.querySelector('#auto')?.addEventListener('change', async (ev) => {
      cfg = { ...cfg, skip_update_check: !ev.target.checked };
      await api('save_config', { config: cfg }).catch((e) => toast(errText(e), 'error'));
    });
    el.querySelector('#update')?.addEventListener('click', () =>
      api('start_update', { release: upd.check.latest }).catch((e) => toast(errText(e), 'error')),
    );
    el.querySelector('#cancel')?.addEventListener('click', () => api('cancel_update'));
    el.querySelector('#restart')?.addEventListener('click', () => api('restart_launcher').catch((e) => toast(errText(e), 'error')));
  }

  // redraw on update events while this page is shown
  const listener = () => {
    if (!el.querySelector('#updates')) {
      upd.listeners.delete(listener);
      return;
    }
    const prog = el.querySelector('#updates progress');
    const s = upd.status;
    if (prog && s.state === 'running' && s.phase === 'download' && s.total) {
      prog.value = Math.floor((100 * s.done) / s.total); // no full redraw for every download tick
      prog.previousElementSibling.textContent = updMessage(s.message);
      return;
    }
    draw();
  };
  upd.listeners.add(listener);
  draw();
}
