import type { StudySummary } from './types';
import { escHtml as esc, formatDuration, formatRecordedAt } from './format';
import { PLMI_ABNORMAL_THRESHOLD } from './narrative';
import { LOGO } from './shell';

function studyCard(s: StudySummary): string {
  const flagged = s.plmi >= PLMI_ABNORMAL_THRESHOLD;
  const hrv = s.hrv_rmssd_overall;
  const href = `index.html?meta=${encodeURIComponent(s.meta_path)}`;
  return `
    <a class="card study-card" href="${esc(href)}">
      <div class="study-top">
        <span class="study-id">${esc(s.recording_id)}</span>
        <span class="badge ${flagged ? 'flag' : 'good'}">${flagged ? '▲ elevated PLMI' : '▼ typical'}</span>
      </div>
      <div class="study-when">${esc(formatRecordedAt(s.start_iso))} · ${esc(formatDuration(s.duration_s))}</div>
      <div class="study-stats">
        <div class="ss"><span class="v num">${esc(s.plmi.toFixed(1))}</span><span class="l">PLMI /hr</span></div>
        <div class="ss"><span class="v num">${esc(String(s.total_lms))}</span><span class="l">limb moves</span></div>
        <div class="ss"><span class="v num">${hrv === null ? '—' : esc(String(Math.round(hrv)))}</span><span class="l">HRV ms</span></div>
      </div>
      <div class="study-foot">Subject <b>${esc(s.subject_id)}</b> <span class="dim">· de-identified</span></div>
    </a>`;
}

/** Build the landing page shown at the site root (no `?meta=` param): every deployed study, newest first, linking through to its viewer. */
export function renderLanding(studies: readonly StudySummary[]): string {
  const sorted = [...studies].sort((a, b) => b.start_iso.localeCompare(a.start_iso));
  const count = sorted.length;
  const body = count === 0
    ? '<div class="card card-pad empty-state">No studies deployed yet. Run <code>deploy.py</code> after processing a recording.</div>'
    : `<div class="study-grid">${sorted.map(studyCard).join('')}</div>`;

  return `
  <div class="wrap">
    <header class="top">
      <div class="brand">${LOGO}<div class="wordmark">Proto<b>Som</b></div></div>
      <div class="spacer"></div>
      <span class="poc-tag">Proof of concept</span>
    </header>
    <div class="rec-line">${String(count)} stud${count === 1 ? 'y' : 'ies'} available</div>
    ${body}
  </div>`;
}
