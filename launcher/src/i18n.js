import { invoke } from '@tauri-apps/api/core';
import { en } from './locales/en.js';
import { zh } from './locales/zh.js';

/*
 * Interface language.
 *
 * English is the source table: every key exists there and any key a translation
 * leaves out falls back to it, so an incomplete pack shows English instead of a
 * raw key. Chinese is built in as well.
 *
 * Third-party packs are plain JSON files in locales/ next to the launcher:
 *   { "code": "de", "name": "Deutsch", "strings": { "nav.play": "Spielen", ... } }
 * The Rust side only reads and parses them (list_locales / read_locale); a pack
 * can add or override strings and nothing else, so a broken or hostile file can
 * never change how the launcher works. A file that does not parse is skipped.
 */

const BUILTIN = { en, zh };

let current = 'en';
let table = en.strings;
let langs = [{ code: 'en', name: en.name }, { code: 'zh', name: zh.name }];

/** The active language code. */
export const getLang = () => current;

/** Every language the launcher can switch to: the built-in ones, then the packs in locales/. */
export const listLangs = () => langs;

/** The name to show for a language code. */
export const langName = (code) => (langs.find((l) => l.code === code) || {}).name || code;

/**
 * A piece of interface text. `vars` fills {placeholders}.
 * Falls back to English, then to the key itself, so a missing entry is visible but harmless.
 */
export function t(key, vars) {
  let s = table[key];
  if (s === undefined) s = en.strings[key];
  if (s === undefined) return key;
  if (vars) s = s.replace(/\{(\w+)\}/g, (m, k) => (vars[k] === undefined ? m : String(vars[k])));
  return s;
}

/** True when the active language needs a wider font stack (Chinese, Japanese, Korean). */
const wide = (code) => /^(zh|ja|ko)/i.test(code);

/**
 * Switches the interface language and redraws what is on screen. `redraw` is
 * called after the table is loaded, so the caller decides what to repaint
 * (navigation labels and the current page).
 */
export async function setLang(code, redraw) {
  const known = langs.some((l) => l.code === code);
  current = known ? code : 'en';
  if (BUILTIN[current]) {
    table = BUILTIN[current].strings;
  } else {
    table = {};
    try {
      const pack = await invoke('read_locale', { code: current });
      table = (pack && pack.strings) || {};
    } catch {
      table = {};
    }
  }
  document.documentElement.lang = current === 'zh' ? 'zh-CN' : current;
  document.documentElement.classList.toggle('cjk', wide(current));
  if (typeof redraw === 'function') redraw();
}

/**
 * Reads the saved language and every pack in locales/, then applies the saved one.
 * Called once at startup, before the first page is drawn. `saved` is the language
 * from the configuration when the caller already has it, which saves one round trip.
 */
export async function initLang(redraw, saved) {
  let code = saved || 'en';
  if (!saved) {
    try {
      code = (await invoke('get_config')).lang || 'en';
    } catch {
      code = 'en';
    }
  }
  try {
    const packs = await invoke('list_locales');
    for (const p of packs) {
      if (p && p.code && !BUILTIN[p.code] && !langs.some((l) => l.code === p.code)) {
        langs.push({ code: p.code, name: p.name || p.code });
      }
    }
  } catch {
    // no locales/ folder: the built-in languages still work
  }
  await setLang(code, redraw);
}
