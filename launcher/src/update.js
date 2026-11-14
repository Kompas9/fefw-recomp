import { api, errText } from './util.js';

/** The launcher's update state, shared by the nav and the About page. */
/** The update check, the update card on the About page and the check at start are off for now (no release feed to read yet). */
export const UPDATES_ENABLED = false;

export const upd = {
  check: null, // the last check_update answer
  checking: false,
  error: '',
  status: { state: 'idle' }, // update-status
  listeners: new Set(),
};

export function notify() {
  upd.listeners.forEach((fn) => fn());
}

export async function checkNow() {
  upd.checking = true;
  upd.error = '';
  notify();
  try {
    upd.check = await api('check_update');
  } catch (e) {
    upd.error = errText(e);
  }
  upd.checking = false;
  notify();
}

export const available = () => !!(upd.check && upd.check.package && upd.check.newer && upd.check.latest);
