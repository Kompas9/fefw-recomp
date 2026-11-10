import { invoke } from '@tauri-apps/api/core';
import {
  createIcons,
  Play, Settings, FolderOpen, Info, CircleCheck, CircleX, TriangleAlert, CircleAlert, FileText,
  Save, Undo2, Folder, Monitor, Volume2, Mouse, Gamepad2, LoaderCircle, FolderSearch, Wrench,
  ShieldCheck, Keyboard, HardDrive, FileArchive, Terminal, ListChecks, Hammer, Circle, Sparkles, Gauge,
  Download, RefreshCw, RotateCw, CircleArrowUp, Languages, Package,
} from 'lucide';

const ICONS = {
  Play, Settings, FolderOpen, Info, CircleCheck, CircleX, TriangleAlert, CircleAlert, FileText,
  Save, Undo2, Folder, Monitor, Volume2, Mouse, Gamepad2, LoaderCircle, FolderSearch, Wrench,
  ShieldCheck, Keyboard, HardDrive, FileArchive, Terminal, ListChecks, Hammer, Circle, Sparkles, Gauge,
  Download, RefreshCw, RotateCw, CircleArrowUp, Languages, Package,
};

/** Calls a Rust command. */
export const api = (cmd, args) => invoke(cmd, args);

export function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

/** Markup for a Lucide icon; call refreshIcons() after inserting it. */
export const icon = (name, cls = '') => `<i data-lucide="${name}" class="${cls}"></i>`;

export function refreshIcons() {
  createIcons({ icons: ICONS });
}

export function toast(message, kind = '') {
  const host = document.getElementById('toast');
  const el = document.createElement('div');
  el.className = `toast ${kind}`;
  el.textContent = message;
  host.appendChild(el);
  setTimeout(() => el.remove(), kind === 'error' ? 7000 : 3500);
}

export const errText = (e) => (typeof e === 'string' ? e : e?.message ?? String(e));
