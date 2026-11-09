import { api, esc, icon, refreshIcons, toast, errText } from '../util.js';

/*
 * Settings schema. Every key is described in runtime/docs/CONFIG.md.
 *   file: "game" (fefw.ini), "controls" (controls.ini, written by the game on its first start) or "config" (the launcher's own options)
 * Types: select, toggle, size, autonum, range, path, text, number. A toggle with `on` / `off` writes those words instead of 1 / 0 (`off`,
 * 0, false or no read as off, anything else as on).
 */
const SECTIONS = [
  {
    title: 'Picture', icon: 'monitor', file: 'game',
    fields: [
      { id: 'output', type: 'select', label: 'Output resolution', sec: 'fefw', key: 'output', def: 'console',
        options: [['console', 'Console (docked 1440p)'], ['720p', '720p'], ['1080p', '1080p'], ['1440p', '1440p'], ['2160p', '2160p (4K)'],
          ['2560x1080', '2560 x 1080 (21:9)'], ['3440x1440', '3440 x 1440 (21:9)'], ['5120x1440', '5120 x 1440 (32:9, drawn at 3840 x 1080)'], ['window', 'Window size at start']],
        hint: 'The size the game draws its picture, UI and effects at. Applies at the next start.' },
      { id: 'ultrawide', type: 'toggle', label: 'Ultrawide', sec: 'fefw', key: 'ultrawide', def: 'auto', on: 'auto', off: 'off',
        hint: 'With an output wider than 16:9 (a wide size above, or the window size at start in a wide window) the 3D view gets wider and the menus, HUD and movies stay in the centre at 16:9. Off stretches the 16:9 picture.' },
      { id: 'render_ratio', type: 'select', label: 'Render scale', sec: 'fefw', key: 'render_ratio', def: '0',
        options: [['0', "Game's own (50%)"], ['0.5', '50%'], ['0.67', '67%'], ['0.75', '75%'], ['1.0', '100% (native, sharpest)']],
        hint: '3D render size per side, as a share of the output. The upscaler fills in the rest.' },
      { id: 'upscaler', type: 'select', label: 'Upscaler', sec: 'dlss', key: 'upscaler', def: 'taau',
        options: [['taau', 'Built-in temporal (TAAU)'], ['fsr3', 'AMD FSR 3 upscaling (any GPU; files included)'], ['dlss', 'NVIDIA DLSS (RTX; needs the Streamline files)'], ['bilinear', 'Bilinear (no history)'], ['auto', 'Automatic (DLSS, else FSR 3, else TAAU)']],
        hint: "Stands in for the console's DLSS. TAAU needs no extra files. FSR 3 uses AMD's files in upscalers/fsr (they come with the release); DLSS needs NVIDIA's. If a file is missing the game says which in fefw.log and uses TAAU. Applies at the next start." },
      { id: 'fsr_dll', type: 'path', label: 'FSR 3 (AMD) folder', sec: 'upscale', key: 'fsr_dll', def: '', placeholder: 'upscalers/fsr next to the launcher', status: 'fsr',
        hint: 'Leave empty to use the files that come with the release. Upscaling: amd_fidelityfx_upscaler_vk.dll and amd_fidelityfx_loader_vk.dll (FSR SDK v2.4.0, signedbin). Frame generation: amd_fidelityfx_vk.dll (FidelityFX SDK v1.1.4).' },
      { id: 'dlss_dll', type: 'path', label: 'DLSS (Streamline) folder', sec: 'upscale', key: 'dlss_dll', def: '', placeholder: 'upscalers/streamline next to the launcher', status: 'dlss',
        hint: 'NVIDIA files, not included: sl.interposer.dll, sl.common.dll, sl.dlss.dll and nvngx_dlss.dll from the Streamline SDK (bin/x64); DLSS frame generation also needs sl.dlss_g.dll, sl.reflex.dll, sl.pcl.dll, nvngx_dlssg.dll and NvLowLatencyVk.dll. Leave empty for upscalers/streamline next to the launcher.' },
      { id: 'quality', type: 'select', label: 'Quality', sec: 'fefw', key: 'quality', def: 'console',
        options: [['console', 'As the console picks'], ['docked', 'Always docked quality']] },
      { id: 'shadow_size', type: 'select', label: 'Shadow map size', sec: 'fefw', key: 'shadow_size', def: '0',
        options: [['0', "Game's own"], ['2048', '2048'], ['4096', '4096'], ['8192', '8192']] },
    ],
  },
  {
    // [tweaks] language of controls.ini (runtime/docs/CONFIG.md). The names are the ones the runtime reads (services/common.cpp); the list is
    // the languages the game ships (games/fefw/fefw_data.cpp). A [system] language in fefw.ini wins over this.
    title: 'Language', icon: 'languages', file: 'controls',
    fields: [
      { id: 'language', type: 'select', label: 'Game language', sec: 'tweaks', key: 'language', def: 'auto',
        options: [
          ['auto', 'Automatic (Windows language)'],
          ['AmericanEnglish', 'English (US)'], ['BritishEnglish', 'English (UK)'], ['Japanese', '日本語 (Japanese)'],
          ['French', 'Français (French)'], ['CanadianFrench', 'Français (Canada)'], ['German', 'Deutsch (German)'], ['Italian', 'Italiano (Italian)'],
          ['Spanish', 'Español (Spain)'], ['LatinAmericanSpanish', 'Español (Latin America)'], ['Korean', '한국어 (Korean)'],
          ['SimplifiedChinese', '简体中文 (Simplified Chinese)'], ['TraditionalChinese', '繁體中文 (Traditional Chinese)'],
        ],
        hint: "The language of the game's text and voices. Automatic uses your Windows display language when the game has it, else English. Applies at the next start of the game." },
    ],
  },
  {
    title: 'Frame rate', icon: 'gauge', file: 'game',
    fields: [
      { id: 'fps60', type: 'toggle', label: '60 fps gameplay', sec: 'fefw', key: 'fps60', def: '0', hint: 'The game runs at 30 on the console. Dialogue text also types twice as fast.' },
      { id: 'fps120', type: 'toggle', label: '120 fps gameplay', sec: 'fefw', key: 'fps120', def: '0', hint: 'Wins over 60 fps. Needs a fast CPU. A frame rate limit below 120 is raised to 120 while this is on; a 60 Hz display, or vsync on a 60 Hz display, still shows 60.' },
      { id: 'smooth_step', type: 'toggle', label: 'Exact game clock', sec: 'fefw', key: 'smooth_step', def: '0', hint: 'Keeps game time exact when frame times vary. Unset, it is on with 60 and 120 fps gameplay and off at the original 30.' },
      { id: 'frame_gen', type: 'select', label: 'Frame generation', sec: 'dlss', key: 'frame_gen', def: '0',
        options: [['0', 'Off'], ['fsr3', 'AMD FSR 3 frame generation (any GPU; files included)'], ['dlss', 'NVIDIA DLSS frame generation (RTX 40 or newer; Streamline files)']],
        hint: 'One generated frame between real ones; the HUD and text come from the real frame. Best with VSync off on a 120 Hz or faster display. With VSync on the game renders at most half the refresh rate, and below twice the game frame rate generation pauses. FSR 3 uses amd_fidelityfx_vk.dll from the FSR 3 folder (Picture). DLSS needs Windows hardware-accelerated GPU scheduling and the Streamline frame generation files. Applies at the next start.' },
    ],
  },
  {
    title: 'Window', icon: 'monitor', file: 'controls',
    fields: [
      { id: 'window_mode', type: 'select', label: 'Window mode', sec: 'display', key: 'window_mode', def: 'windowed',
        options: [['windowed', 'Windowed'], ['borderless', 'Borderless fullscreen'], ['exclusive', 'Exclusive fullscreen']], hint: 'Alt+Enter switches while playing.' },
      { id: 'size', type: 'size', label: 'Window size', sec: 'display', keys: ['width', 'height'], def: ['1280', '720'] },
      { id: 'fps_limit', type: 'autonum', label: 'Frame rate limit', sec: 'display', key: 'fps_limit', def: 'auto',
        min: 0, max: 1000, numDefault: 60, unit: 'fps', autoLabel: 'Auto', hint: 'Auto follows the display refresh rate. 0 is unlimited. 60 and 120 fps gameplay raise a lower number to their own rate while they are on.' },
      { id: 'vsync', type: 'toggle', label: 'VSync', sec: 'display', key: 'vsync', def: '1' },
    ],
  },
  {
    title: 'Sound and controls', icon: 'volume-2', file: 'controls',
    fields: [
      { id: 'volume', type: 'range', label: 'Master volume', sec: 'audio', key: 'volume', def: '1.0' },
      { id: 'mute_unfocused', type: 'toggle', label: 'Mute when the window is in the background', sec: 'audio', key: 'mute_unfocused', def: '0' },
      { id: 'prompt_style', type: 'select', label: 'Button prompts', sec: 'prompts', key: 'style', def: 'auto',
        options: [['auto', 'Automatic (last device used)'], ['switch', "Switch (the game's own)"], ['xbox', 'Xbox'], ['playstation', 'PlayStation'], ['keyboard', 'Keyboard and mouse']],
        hint: 'Keyboard: WASD move, arrows or IJKL camera, Enter or Space A, Esc or Backspace B, R X, F Y, Q L, E R, Z ZL, C ZR, Tab +, M -, 1-4 d-pad. F1 > Controls lists them; change them in [bindings.all] of controls.ini.' },
    ],
  },
  {
    title: 'Input, saves and log', icon: 'folder', file: 'game',
    fields: [
      { id: 'jc2_mouse', type: 'select', label: 'Mouse controls (the game's Joy-Con 2 mouse)', sec: 'input', key: 'jc2_mouse', def: 'auto', options: [['auto', 'Automatic (last device used)'], ['on', 'Always the mouse'], ['off', 'Off']],
        hint: 'Moving or clicking the mouse switches the game to its mouse controls; a key or the controller switches back. Speeds: the game's own Settings.' },
      { id: 'save_dir', type: 'path', label: 'Save folder', sec: 'fs', key: 'save_dir', def: '', placeholder: 'save/ in the run folder' },
      { id: 'log_level', type: 'select', label: 'Log detail', sec: 'log', key: 'level', def: 'info', options: [['error', 'Errors'], ['warn', 'Warnings'], ['info', 'Normal'], ['debug', 'Debug']] },
    ],
  },
  {
    title: 'Launcher', icon: 'wrench', file: 'config',
    fields: [
      { id: 'repo_dir', type: 'path', label: 'Repository folder', sec: '', key: 'repo_dir', def: '', placeholder: "found from the launcher's location", hint: 'The checkout of this project that setup builds in.' },
      { id: 'game_dir', type: 'path', label: 'Run folder', sec: '', key: 'game_dir', def: '', placeholder: '<repository>/work/play', hint: 'Holds fefw.ini, controls.ini, fefw.log and the saves.' },
      { id: 'jobs', type: 'number', label: 'Build processes', sec: '', key: 'jobs', def: '0', min: 0, max: 256, hint: '0: one fewer than your CPU threads. Lower it if the PC runs out of memory while building.' },
      { id: 'minimize_on_play', type: 'toggle', label: 'Minimise while the game runs', sec: '', key: 'minimize_on_play', def: '0' },
    ],
  },
];

const truthy = (v) => ['1', 'true', 'yes', 'on'].includes(String(v).trim().toLowerCase());
const clamp = (n, lo, hi) => Math.min(hi, Math.max(lo, n));
const fwd = (s) => String(s).trim().replace(/\\/g, '/');

export async function render(el, ctx) {
  const [cfg, status] = await Promise.all([api('get_config'), api('game_status')]);
  const data = await api('read_settings').catch(() => ({ game_dir: '', game: {}, controls: {}, controls_exists: false }));
  const files = { game: data.game, controls: data.controls, config: { repo_dir: cfg.repo_dir, game_dir: cfg.game_dir, jobs: String(cfg.jobs || 0), minimize_on_play: cfg.minimize_on_play ? '1' : '0' } };
  const exists = { game: true, controls: data.controls_exists, config: true };
  const keyOf = (f, k) => (f.file === 'config' ? k : `${f.sec}.${k}`);
  const get = (f, k, def) => files[f.file][keyOf(f, k)] ?? def;

  // ----- model: one UI value per field -----
  const READ = {
    select: (f) => get(f, f.key, f.def),
    toggle: (f) => (f.off !== undefined ? ![f.off, '0', 'false', 'no'].includes(String(get(f, f.key, f.def)).trim().toLowerCase()) : truthy(get(f, f.key, f.def))),
    size: (f) => ({ w: get(f, f.keys[0], f.def[0]), h: get(f, f.keys[1], f.def[1]) }),
    autonum: (f) => {
      const v = String(get(f, f.key, f.def));
      return v.toLowerCase() === 'auto' ? { auto: true, num: String(f.numDefault) } : { auto: false, num: v };
    },
    range: (f) => Math.round(clamp(parseFloat(get(f, f.key, f.def)) || 0, 0, 1) * 100),
    path: (f) => get(f, f.key, f.def),
    text: (f) => get(f, f.key, f.def),
    number: (f) => String(get(f, f.key, f.def)),
  };
  const WRITE = {
    select: (f, x) => [[f.sec, f.key, x]],
    toggle: (f, x) => [[f.sec, f.key, x ? (f.on ?? '1') : (f.off ?? '0')]],
    size: (f, x) => [
      [f.sec, f.keys[0], String(clamp(parseInt(x.w, 10) || 1280, 320, 16384))],
      [f.sec, f.keys[1], String(clamp(parseInt(x.h, 10) || 720, 320, 16384))],
    ],
    autonum: (f, x) => [[f.sec, f.key, x.auto ? 'auto' : String(clamp(Math.round(parseFloat(x.num)) || f.numDefault, f.min, f.max))]],
    range: (f, x) => [[f.sec, f.key, (x / 100).toFixed(2)]],
    path: (f, x) => [[f.sec, f.key, fwd(x)]],
    text: (f, x) => [[f.sec, f.key, String(x).trim()]],
    number: (f, x) => [[f.sec, f.key, String(clamp(parseInt(x, 10) || 0, f.min, f.max))]],
  };

  const all = SECTIONS.flatMap((s) => s.fields.map((f) => ({ ...f, file: s.file })));
  const model = {};
  const orig = {};
  for (const f of all) {
    model[f.id] = READ[f.type](f);
    orig[f.id] = JSON.stringify(model[f.id]);
  }
  const dirtyFields = () => all.filter((f) => JSON.stringify(model[f.id]) !== orig[f.id]);

  // ----- markup -----
  const dis = (f) => (exists[f.file] ? '' : 'disabled');
  const CONTROL = {
    select: (f, x) => {
      const opts = f.options.some(([v]) => v === x) ? f.options : [...f.options, [x, x]];
      return `<select data-f="${f.id}" ${dis(f)}>${opts.map(([v, l]) => `<option value="${esc(v)}" ${v === x ? 'selected' : ''}>${esc(l)}</option>`).join('')}</select>`;
    },
    toggle: (f, x) => `<label class="switch"><input type="checkbox" data-f="${f.id}" ${x ? 'checked' : ''} ${dis(f)}>${x ? 'On' : 'Off'}</label>`,
    size: (f, x) => `<input type="number" data-f="${f.id}" data-part="w" min="320" max="16384" value="${esc(x.w)}" ${dis(f)}><span class="unit">x</span><input type="number" data-f="${f.id}" data-part="h" min="320" max="16384" value="${esc(x.h)}" ${dis(f)}>`,
    autonum: (f, x) => `<label class="switch"><input type="checkbox" data-f="${f.id}" data-part="auto" ${x.auto ? 'checked' : ''} ${dis(f)}>${esc(f.autoLabel)}</label><input type="number" data-f="${f.id}" data-part="num" min="${f.min}" max="${f.max}" value="${esc(x.num)}" ${x.auto || !exists[f.file] ? 'disabled' : ''}><span class="unit">${esc(f.unit)}</span>`,
    range: (f, x) => `<input type="range" data-f="${f.id}" min="0" max="100" step="1" value="${x}" ${dis(f)}><span class="unit" data-readout="${f.id}" style="min-width: 3.5ch; text-align: right">${x}%</span>`,
    text: (f, x) => `<input type="text" data-f="${f.id}" spellcheck="false" placeholder="${esc(f.placeholder || '')}" value="${esc(x)}" ${dis(f)}>`,
    number: (f, x) => `<input type="number" data-f="${f.id}" min="${f.min}" max="${f.max}" value="${esc(x)}" ${dis(f)}>`,
    path: (f, x) => `<input type="text" data-f="${f.id}" spellcheck="false" placeholder="${esc(f.placeholder || '')}" value="${esc(x)}" ${dis(f)}><button class="btn" data-browse="${f.id}" ${dis(f)}>${icon('folder-open')}Browse</button>`,
  };

  const fieldHtml = (f) => `
    <div class="field">
      <label class="name">${esc(f.label)}${f.hint ? `<span class="hint">${esc(f.hint)}</span>` : ''}${f.status ? `<span class="hint" data-status="${f.status}">Checking the files...</span>` : ''}</label>
      <div class="control">${CONTROL[f.type](f, model[f.id])}</div>
    </div>`;

  function draw() {
    const n = dirtyFields().length;
    el.innerHTML = `
      <div class="page">
        <div class="hero">
          <h1>Settings</h1>
          <p class="dim">These edit fefw.ini and controls.ini in the run folder (<code>${esc(data.game_dir)}</code>). Other keys and comments in those files are left alone. Every key: runtime/docs/CONFIG.md.</p>
        </div>
        ${status.state === 'running' ? `<div class="banner warn">${icon('triangle-alert')}<div>The game is running. Most changes apply at the next start, and the game may overwrite controls.ini when it exits.</div></div>` : ''}
        ${!data.controls_exists ? `<div class="banner info">${icon('info')}<div>controls.ini does not exist yet in <code>${esc(data.game_dir)}</code>. The game creates it on its first start, so Window and Sound and controls become editable after that. The other options work now.</div></div>` : ''}
        ${SECTIONS.map((s) => `<div class="card"><h2>${icon(s.icon)}${esc(s.title)}</h2>${s.fields.map((f) => fieldHtml({ ...f, file: s.file })).join('')}</div>`).join('')}
        <div class="savebar">
          <span class="dim">${n ? `${n} unsaved change${n > 1 ? 's' : ''}` : 'No changes'}</span>
          <div class="row">
            <button class="btn quiet" id="revert" ${n ? '' : 'disabled'}>${icon('undo-2')}Revert</button>
            <button class="btn primary" id="save" ${n ? '' : 'disabled'}>${icon('save')}Save</button>
          </div>
        </div>
      </div>`;
    refreshIcons();
    bind();
    checkFiles();
  }

  // The upscaler files the game will find with the folders as typed (found only when every file a feature needs is in one folder).
  let checkSeq = 0;
  async function checkFiles() {
    const seq = ++checkSeq;
    const r = await api('upscaler_files', { fsrDir: fwd(model.fsr_dll || ''), dlssDir: fwd(model.dlss_dll || '') }).catch(() => null);
    if (seq !== checkSeq || !r) return;
    const line = (name, x) => `${name}: ${x.found ? `found (${x.detail})` : `not found, ${x.detail}`}`;
    const put = (which, text) => el.querySelectorAll(`[data-status="${which}"]`).forEach((n) => { n.textContent = text; });
    put('fsr', `${line('Upscaling', r.fsr_upscaling)}. ${line('Frame generation', r.fsr_framegen)}.`);
    put('dlss', `${line('DLSS', r.dlss)}. ${line('DLSS frame generation', r.dlss_framegen)}.`);
  }
  let checkTimer = 0;

  function bind() {
    const byId = Object.fromEntries(all.map((f) => [f.id, f]));
    el.querySelectorAll('[data-f]').forEach((input) => {
      const f = byId[input.dataset.f];
      const part = input.dataset.part;
      const onChange = () => {
        const v = input.type === 'checkbox' ? input.checked : input.value;
        if (part) model[f.id] = { ...model[f.id], [part]: v };
        else model[f.id] = f.type === 'range' ? parseInt(v, 10) : v;
        if (f.type === 'range') {
          el.querySelector(`[data-readout="${f.id}"]`).textContent = `${model[f.id]}%`;
          updateBar();
        } else if (input.type === 'text' || input.type === 'number') {
          updateBar();
          if (f.status) {
            clearTimeout(checkTimer);
            checkTimer = setTimeout(checkFiles, 300);
          }
        } else {
          draw(); // toggles and selects change dependent markup (labels, disabled inputs)
        }
      };
      input.addEventListener(input.type === 'range' || input.type === 'text' || input.type === 'number' ? 'input' : 'change', onChange);
    });
    el.querySelectorAll('[data-browse]').forEach((b) =>
      b.addEventListener('click', async () => {
        const f = byId[b.dataset.browse];
        const picked = await api('pick_folder', { title: `Choose: ${f.label}`, start: model[f.id] || null });
        if (picked) {
          model[f.id] = picked;
          draw();
        }
      }),
    );
    el.querySelector('#revert').addEventListener('click', () => {
      for (const f of all) model[f.id] = JSON.parse(orig[f.id]);
      draw();
    });
    el.querySelector('#save').addEventListener('click', save);
  }

  function updateBar() {
    const n = dirtyFields().length;
    const bar = el.querySelector('.savebar');
    bar.querySelector('span').textContent = n ? `${n} unsaved change${n > 1 ? 's' : ''}` : 'No changes';
    bar.querySelector('#save').disabled = !n;
    bar.querySelector('#revert').disabled = !n;
  }

  async function save() {
    const edits = [];
    const cfgNew = { ...cfg };
    for (const f of dirtyFields()) {
      for (const [sec, key, value] of WRITE[f.type](f, model[f.id])) {
        if (f.file === 'config') {
          cfgNew[key] = key === 'minimize_on_play' ? value === '1' : key === 'jobs' ? parseInt(value, 10) || 0 : fwd(value);
        } else {
          edits.push({ file: f.file, section: sec, key, value });
        }
      }
    }
    try {
      if (edits.length) await api('write_settings', { edits });
      if (JSON.stringify(cfgNew) !== JSON.stringify(cfg)) await api('save_config', { config: cfgNew });
      toast('Settings saved.', 'ok');
      ctx.go('settings');
    } catch (e) {
      toast(errText(e), 'error');
    }
  }

  draw();
}
