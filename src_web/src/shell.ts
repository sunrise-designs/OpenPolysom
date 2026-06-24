import type { Meta } from './types';
import type { Narrative } from './narrative';
import { formatDuration, formatRecordedAt, shortSha } from './format';
import { PLMI_ABNORMAL_THRESHOLD } from './narrative';
import { CHANNEL_META } from './chart';

function esc(s: string): string {
  return s.replace(/[&<>"']/g, (c) =>
    c === '&' ? '&amp;' : c === '<' ? '&lt;' : c === '>' ? '&gt;' : c === '"' ? '&quot;' : '&#39;',
  );
}

function clockUTC(ms: number): string {
  const d = new Date(ms);
  return `${String(d.getUTCHours()).padStart(2, '0')}:${String(d.getUTCMinutes()).padStart(2, '0')}`;
}

/** Logo mark (moon + wave), static. */
const LOGO = `
  <svg width="40" height="40" viewBox="0 0 40 40" aria-hidden="true">
    <defs><linearGradient id="logoG" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#8ea2ff"/><stop offset="1" stop-color="#6ad6c6"/></linearGradient></defs>
    <rect x="1" y="1" width="38" height="38" rx="11" fill="#131c30" stroke="#2e3c59"/>
    <path d="M24.5 11.5a8.5 8.5 0 1 0 4 12.6 6.8 6.8 0 0 1-4-12.6z" fill="url(#logoG)"/>
    <path d="M8 27c2.2 0 2.2-3 4.4-3s2.2 3 4.4 3 2.2-3 4.4-3 2.2 3 4.4 3 2.2-3 4.4-3" fill="none" stroke="#6ad6c6" stroke-width="1.6" stroke-linecap="round" opacity=".85"/>
  </svg>`;

/** Aurora / moon hero illustration, static. */
const HERO_ART = `
  <div class="hero-art" aria-hidden="true">
    <svg viewBox="0 0 360 240" preserveAspectRatio="xMidYMid slice">
      <defs>
        <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#162038"/><stop offset="1" stop-color="#0c1426"/></linearGradient>
        <linearGradient id="aur1" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#6ad6c6" stop-opacity="0"/><stop offset=".5" stop-color="#6ad6c6" stop-opacity=".55"/><stop offset="1" stop-color="#6ad6c6" stop-opacity="0"/></linearGradient>
        <linearGradient id="aur2" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#8ea2ff" stop-opacity="0"/><stop offset=".5" stop-color="#8ea2ff" stop-opacity=".5"/><stop offset="1" stop-color="#8ea2ff" stop-opacity="0"/></linearGradient>
        <linearGradient id="aur3" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#c79bf0" stop-opacity="0"/><stop offset=".5" stop-color="#c79bf0" stop-opacity=".45"/><stop offset="1" stop-color="#c79bf0" stop-opacity="0"/></linearGradient>
        <radialGradient id="moonG" cx="35%" cy="35%" r="70%"><stop offset="0" stop-color="#fdf6e3"/><stop offset="1" stop-color="#d7c89b"/></radialGradient>
        <radialGradient id="glow" cx="50%" cy="50%" r="50%"><stop offset="0" stop-color="#fdf6e3" stop-opacity=".35"/><stop offset="1" stop-color="#fdf6e3" stop-opacity="0"/></radialGradient>
      </defs>
      <rect width="360" height="240" fill="url(#sky)"/>
      <g fill="#cdd7ef"><circle cx="40" cy="38" r="1.3"/><circle cx="92" cy="22" r="1"/><circle cx="150" cy="46" r="1.5"/><circle cx="210" cy="30" r="1"/><circle cx="300" cy="58" r="1.3"/><circle cx="330" cy="28" r="1"/><circle cx="64" cy="74" r="1"/><circle cx="120" cy="96" r="1.2"/><circle cx="250" cy="80" r="1"/><circle cx="278" cy="120" r="1.1"/><circle cx="22" cy="110" r="1"/><circle cx="340" cy="92" r="1.2"/></g>
      <path d="M-10 120 C 80 60, 150 150, 240 80 S 380 60, 380 60" fill="none" stroke="url(#aur1)" stroke-width="22" stroke-linecap="round" opacity=".7"/>
      <path d="M-10 150 C 90 110, 170 180, 260 110 S 380 100, 380 100" fill="none" stroke="url(#aur2)" stroke-width="18" stroke-linecap="round" opacity=".65"/>
      <path d="M-10 100 C 70 70, 160 120, 250 60 S 380 40, 380 40" fill="none" stroke="url(#aur3)" stroke-width="14" stroke-linecap="round" opacity=".5"/>
      <circle cx="268" cy="70" r="44" fill="url(#glow)"/>
      <circle cx="268" cy="70" r="30" fill="url(#moonG)"/>
      <circle cx="258" cy="60" r="6" fill="#cbb98a" opacity=".5"/><circle cx="280" cy="78" r="4" fill="#cbb98a" opacity=".45"/><circle cx="272" cy="56" r="3" fill="#cbb98a" opacity=".4"/>
      <path d="M0 240 V190 C 60 170, 120 200, 180 188 C 250 174, 310 198, 360 184 V240 Z" fill="#0e1830"/>
      <path d="M0 240 V210 C 80 198, 150 222, 230 206 C 300 192, 340 214, 360 206 V240 Z" fill="#0a1224"/>
      <g transform="translate(96,196)">
        <path d="M0 22 C 6 6, 40 2, 64 8 C 86 13, 96 18, 96 24 L96 30 L0 30 Z" fill="#1b2742"/>
        <circle cx="14" cy="14" r="9" fill="#243559"/>
        <path d="M9 12 a4 4 0 0 0 8 0" fill="none" stroke="#6ad6c6" stroke-width="1.4" stroke-linecap="round" opacity=".7"/>
        <text x="30" y="2" font-family="Inter, sans-serif" font-size="11" fill="#8ea2ff" opacity=".8">z</text>
        <text x="40" y="-6" font-family="Inter, sans-serif" font-size="8" fill="#8ea2ff" opacity=".6">z</text>
      </g>
    </svg>
  </div>`;

/** Data-driven PLMI gauge (semicircle): needle + active arc computed from the value. */
function plmiGauge(plmi: number, abnormal: boolean): string {
  const cx = 120;
  const cy = 130;
  const R = 90;
  const needle = 78;
  const f = Math.max(0, Math.min(1, plmi / 25));
  const a = Math.PI - f * Math.PI;
  const r1 = (n: number): string => n.toFixed(1);
  const ex = r1(cx + R * Math.cos(a));
  const ey = r1(cy - R * Math.sin(a));
  const nx = r1(cx + needle * Math.cos(a));
  const ny = r1(cy - needle * Math.sin(a));
  const col = abnormal ? '#ff7a7a' : '#4fd6a3';
  return `
    <div class="card-title">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" aria-hidden="true"><path d="M3 13a9 9 0 0 1 18 0" stroke="var(--movement)" stroke-width="2" stroke-linecap="round"/><path d="M12 13l4-3" stroke="var(--movement-2)" stroke-width="2" stroke-linecap="round"/></svg>
      Periodic Limb Movement Index
    </div>
    <div style="flex:1;display:flex;align-items:center;justify-content:center;margin:2px 0;">
      <svg width="240" height="150" viewBox="0 0 240 150" aria-hidden="true">
        <defs>
          <linearGradient id="gaugeArc" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#4fd6a3"/><stop offset=".55" stop-color="#f5b942"/><stop offset="1" stop-color="#ff7a7a"/></linearGradient>
          <radialGradient id="needleHub" cx="50%" cy="50%" r="50%"><stop offset="0" stop-color="#fdf6e3"/><stop offset="1" stop-color="#d7c89b"/></radialGradient>
        </defs>
        <path d="M30 130 A 90 90 0 0 1 210 130" fill="none" stroke="#243049" stroke-width="16" stroke-linecap="round"/>
        <path d="M30 130 A 90 90 0 0 1 210 130" fill="none" stroke="url(#gaugeArc)" stroke-width="16" stroke-linecap="round" opacity=".30"/>
        <path d="M30 130 A 90 90 0 0 1 ${ex} ${ey}" fill="none" stroke="${col}" stroke-width="16" stroke-linecap="round"/>
        <g stroke="#ff7a7a" stroke-width="2"><line x1="183" y1="63" x2="193" y2="55"/></g>
        <text x="196" y="50" font-family="Inter,sans-serif" font-size="9" fill="#ff7a7a">15+ flag</text>
        <text x="22" y="146" font-family="Inter,sans-serif" font-size="9" fill="#647193">0</text>
        <text x="206" y="146" font-family="Inter,sans-serif" font-size="9" fill="#647193">25</text>
        <g>
          <line x1="120" y1="130" x2="${nx}" y2="${ny}" stroke="${col}" stroke-width="4" stroke-linecap="round"/>
          <circle cx="120" cy="130" r="11" fill="url(#needleHub)"/>
          <path d="M124 130 a5 5 0 1 1 -4 -4.6 a4 4 0 0 0 4 4.6z" fill="#d7c89b"/>
        </g>
      </svg>
    </div>
    <div class="gauge-cap">
      <span class="g-val num">${esc(plmi.toFixed(1))}</span><span class="g-unit"> /hr</span>
      <div class="g-state${abnormal ? ' flag' : ''}">${abnormal ? `Elevated (&ge; ${String(PLMI_ABNORMAL_THRESHOLD)} /hr)` : `Within typical range (&lt; ${String(PLMI_ABNORMAL_THRESHOLD)} /hr)`}</div>
    </div>
    <div class="flag-preview">
      <svg width="44" height="30" viewBox="0 0 88 56" aria-hidden="true"><path d="M8 50 A 36 36 0 0 1 80 50" fill="none" stroke="#243049" stroke-width="8" stroke-linecap="round"/><path d="M8 50 A 36 36 0 0 1 78 38" fill="none" stroke="#ff7a7a" stroke-width="8" stroke-linecap="round"/><line x1="44" y1="50" x2="74" y2="34" stroke="#ff7a7a" stroke-width="3" stroke-linecap="round"/><circle cx="44" cy="50" r="5" fill="#ffb0c4"/></svg>
      <div class="fp-text">A flagged night (<b>&ge; 15 /hr</b>) turns the ring red and surfaces a &ldquo;discuss with a clinician&rdquo; note.</div>
    </div>`;
}

function kpiCards(meta: Meta, narrative: Narrative): string {
  const s = meta.stats;
  const hrv = s.hrv_rmssd_overall;
  const hrvVal = hrv === null ? '—' : `${String(Math.round(hrv))}<span class="u">ms</span>`;
  const startMs = new Date(meta.recording.start_iso).getTime();
  const endClock = Number.isNaN(startMs) ? '—' : clockUTC(startMs + meta.recording.duration_s * 1000);
  const startClock = Number.isNaN(startMs) ? '—' : clockUTC(startMs);
  const plmiBadge = narrative.plmiAbnormal
    ? '<span class="badge flag">▲ elevated</span>'
    : '<span class="badge good">▼ typical</span>';
  return `
    <div class="card kpi g-move">
      <div class="k-top"><span class="k-label">PLMI</span>
        <svg class="k-ico" width="20" height="20" viewBox="0 0 24 24" fill="none"><path d="M3 12c2 0 2-4 4-4s2 8 4 8 2-8 4-8 2 4 4 4" stroke="var(--movement)" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg></div>
      <div class="k-val num">${esc(s.plmi.toFixed(1))}<span class="u">/hr</span></div>
      <div class="k-foot">${plmiBadge}</div>
    </div>
    <div class="card kpi g-move">
      <div class="k-top"><span class="k-label">Limb moves</span>
        <svg class="k-ico" width="20" height="20" viewBox="0 0 24 24" fill="none"><path d="M5 18c2-1 3-3 4-6m6 6c-2-1-3-3-4-6" stroke="var(--movement)" stroke-width="1.8" stroke-linecap="round"/><circle cx="11" cy="5" r="2.4" stroke="var(--movement-2)" stroke-width="1.6"/></svg></div>
      <div class="k-val num">${esc(String(s.total_lms))}</div>
      <div class="k-foot"><span class="badge neutral">over ${esc(formatDuration(meta.recording.duration_s))}</span></div>
      <svg class="spark" width="56" height="22" viewBox="0 0 56 22" fill="none"><polyline points="0,16 8,12 14,17 22,9 30,14 38,7 46,13 56,10" stroke="var(--movement)" stroke-width="1.8" fill="none" stroke-linecap="round"/></svg>
    </div>
    <div class="card kpi g-move">
      <div class="k-top"><span class="k-label">Periodic LMs</span>
        <svg class="k-ico" width="20" height="20" viewBox="0 0 24 24" fill="none"><path d="M4 12h3l2-5 3 10 2-7 2 4h4" stroke="var(--movement)" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg></div>
      <div class="k-val num">${esc(String(s.total_plms))}</div>
      <div class="k-foot"><span class="badge ${s.total_plms === 0 ? 'good' : 'neutral'}">${s.total_plms === 0 ? 'none detected' : 'in PLM series'}</span></div>
    </div>
    <div class="card kpi g-hrv">
      <div class="k-top"><span class="k-label">HRV RMSSD</span>
        <svg class="k-ico" width="20" height="20" viewBox="0 0 24 24" fill="none"><path d="M3 13h4l2-6 3 12 2-8 2 4h5" stroke="var(--hrv)" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg></div>
      <div class="k-val num">${hrvVal}</div>
      <div class="k-foot">${hrv === null ? '<span class="badge neutral">not measured</span>' : '<span class="badge good">▲ healthy</span>'}</div>
      <svg class="spark" width="56" height="22" viewBox="0 0 56 22" fill="none"><polyline points="0,15 9,16 18,11 27,13 36,8 45,10 56,6" stroke="var(--hrv)" stroke-width="1.8" fill="none" stroke-linecap="round"/></svg>
    </div>
    <div class="card kpi g-time">
      <div class="k-top"><span class="k-label">Recording</span>
        <svg class="k-ico" width="20" height="20" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="8.5" stroke="var(--neuro)" stroke-width="1.8"/><path d="M12 7.5V12l3 2" stroke="var(--neuro)" stroke-width="1.8" stroke-linecap="round"/></svg></div>
      <div class="k-val num">${esc(formatDuration(meta.recording.duration_s))}</div>
      <div class="k-foot"><span class="badge neutral">${esc(startClock)} → ${esc(endClock)}</span></div>
    </div>
    <div class="card kpi future">
      <div class="k-top"><span class="k-label">AHI</span>
        <svg class="k-ico" width="20" height="20" viewBox="0 0 24 24" fill="none" opacity=".7"><path d="M5 15c0-4 3-6 3-9m8 9c0-4-3-6-3-9M9 13a3 3 0 0 0 6 0" stroke="var(--respir)" stroke-width="1.7" stroke-linecap="round"/></svg></div>
      <div class="k-val num">—</div><div class="k-foot"><span class="soon">coming soon</span></div>
    </div>
    <div class="card kpi future">
      <div class="k-top"><span class="k-label">Sleep efficiency</span>
        <svg class="k-ico" width="20" height="20" viewBox="0 0 24 24" fill="none" opacity=".7"><path d="M5 13a7 7 0 1 0 7-7" stroke="var(--neuro)" stroke-width="1.7" stroke-linecap="round"/><circle cx="12" cy="13" r="1.6" fill="var(--neuro)"/></svg></div>
      <div class="k-val num">—</div><div class="k-foot"><span class="soon">coming soon</span></div>
    </div>`;
}

const MONTAGE = `
  <div class="section-title">Montage <span class="st-line"></span></div>
  <div class="mont-list">
    <div class="mont">
      <div class="m-ico" style="background:rgba(240,120,154,.14)"><svg width="16" height="16" viewBox="0 0 24 24" fill="none"><path d="M12 20S3 14 3 8.5A4.5 4.5 0 0 1 12 6a4.5 4.5 0 0 1 9 2.5C21 14 12 20 12 20z" stroke="var(--cardiac)" stroke-width="1.7"/></svg></div>
      <div><div class="m-name">Cardiac</div><div class="m-sub">RR interval · live</div></div>
      <div class="m-right"><div class="toggle on"></div></div>
    </div>
    <div class="mont">
      <div class="m-ico" style="background:rgba(95,208,196,.14)"><svg width="16" height="16" viewBox="0 0 24 24" fill="none"><path d="M2 12c2 0 2-5 4-5s2 10 4 10 2-10 4-10 2 5 4 5" stroke="var(--movement)" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/></svg></div>
      <div><div class="m-name">Movement</div><div class="m-sub">Accel mag + X/Y/Z · live</div></div>
      <div class="m-right"><div class="toggle on"></div></div>
    </div>
    <div class="mont">
      <div class="m-ico" style="background:rgba(179,136,245,.14)"><svg width="16" height="16" viewBox="0 0 24 24" fill="none"><path d="M3 13h4l2-7 3 13 2-9 2 5h5" stroke="var(--hrv)" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/></svg></div>
      <div><div class="m-name">HRV trend</div><div class="m-sub">RMSSD · live</div></div>
      <div class="m-right"><div class="toggle on"></div></div>
    </div>
    <div class="mont dev">
      <div class="m-ico" style="background:rgba(247,178,103,.10)"><svg width="16" height="16" viewBox="0 0 24 24" fill="none"><path d="M12 4v7m0 0c0 4-3 5-4 5s-3-1-3-5V9m7 2c0 4 3 5 4 5s3-1 3-5V9" stroke="var(--respir)" stroke-width="1.6" stroke-linecap="round"/></svg></div>
      <div><div class="m-name">Respiratory</div><div class="m-sub">Thoracic · Abdomen · Flow — awaiting device</div></div>
      <div class="m-right"><div class="toggle off"></div></div>
    </div>
    <div class="mont dev">
      <div class="m-ico" style="background:rgba(240,120,154,.10)"><svg width="16" height="16" viewBox="0 0 24 24" fill="none"><path d="M2 14h6l2-6 3 12 2-8 2 4h7" stroke="var(--cardiac)" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg></div>
      <div><div class="m-name">ECG / HR</div><div class="m-sub">Awaiting device</div></div>
      <div class="m-right"><div class="toggle off"></div></div>
    </div>
    <div class="mont dev">
      <div class="m-ico" style="background:rgba(142,162,255,.10)"><svg width="16" height="16" viewBox="0 0 24 24" fill="none"><path d="M9 4a3 3 0 0 0-3 3 3 3 0 0 0-1 5 3 3 0 0 0 2 4 3 3 0 0 0 5 1 3 3 0 0 0 5-1 3 3 0 0 0 2-4 3 3 0 0 0-1-5 3 3 0 0 0-3-3 3 3 0 0 0-3-1 3 3 0 0 0-3 1z" stroke="var(--neuro)" stroke-width="1.4"/></svg></div>
      <div><div class="m-name">Neuro (EEG)</div><div class="m-sub">Awaiting device</div></div>
      <div class="m-right"><div class="toggle off"></div></div>
    </div>
  </div>`;

const SPECTRO = `
  <div class="section-title">Snore spectrogram <span class="st-line"></span></div>
  <div class="spectro">
    <svg class="sp-art" viewBox="0 0 300 120" preserveAspectRatio="none" aria-hidden="true">
      <defs><linearGradient id="spg" x1="0" y1="1" x2="0" y2="0"><stop offset="0" stop-color="#162038"/><stop offset=".5" stop-color="#2a3a63"/><stop offset="1" stop-color="#3a4d82"/></linearGradient></defs>
      <rect width="300" height="120" fill="#0f1830"/>
      <g opacity=".35"><rect x="10" y="60" width="8" height="60" fill="url(#spg)"/><rect x="22" y="40" width="8" height="80" fill="url(#spg)"/><rect x="34" y="75" width="8" height="45" fill="url(#spg)"/><rect x="46" y="30" width="8" height="90" fill="url(#spg)"/><rect x="58" y="55" width="8" height="65" fill="url(#spg)"/><rect x="70" y="80" width="8" height="40" fill="url(#spg)"/><rect x="82" y="45" width="8" height="75" fill="url(#spg)"/><rect x="94" y="65" width="8" height="55" fill="url(#spg)"/><rect x="200" y="50" width="8" height="70" fill="url(#spg)"/><rect x="212" y="70" width="8" height="50" fill="url(#spg)"/><rect x="224" y="35" width="8" height="85" fill="url(#spg)"/><rect x="236" y="60" width="8" height="60" fill="url(#spg)"/><rect x="248" y="78" width="8" height="42" fill="url(#spg)"/><rect x="260" y="48" width="8" height="72" fill="url(#spg)"/><rect x="272" y="66" width="8" height="54" fill="url(#spg)"/></g>
    </svg>
    <div class="coming">
      <svg width="34" height="34" viewBox="0 0 24 24" fill="none"><path d="M11 5L6 9H3v6h3l5 4V5z" stroke="var(--respir)" stroke-width="1.6" stroke-linejoin="round"/><path d="M16 8a5 5 0 0 1 0 8" stroke="var(--respir)" stroke-width="1.6" stroke-linecap="round" opacity=".7"/></svg>
      <div class="c-title">Snore detection — coming soon</div>
      <div class="c-sub">Audio-derived snore intensity bands will render here once a microphone channel is captured.</div>
    </div>
  </div>`;

/** Build the full Tremor-Illustrated viewer shell. `#chart` is left for ECharts. */
export function renderShell(meta: Meta, narrative: Narrative): string {
  const s = meta.stats;
  const git = meta.provenance.pipeline.git;
  const anchor = meta.layers.raw.biosignals[0]?.hash;
  const anchorStr = anchor === undefined ? 'n/a' : `${anchor.algorithm} ${anchor.value.substring(0, 8)}…`;
  const hrv = s.hrv_rmssd_overall;
  const periodic = s.total_plms > 0 ? `${esc(String(s.total_plms))} periodic` : 'none periodic';

  return `
  <div class="wrap">
    <header class="top">
      <div class="brand">${LOGO}<div class="wordmark">Proto<b>Som</b></div></div>
      <div class="spacer"></div>
      <span class="chip"><span class="dot"></span>Subject <b style="font-weight:600">${esc(meta.subject.subject_id)}</b><small>· de-identified</small></span>
    </header>
    <div class="rec-line">Recorded <b>${esc(formatRecordedAt(meta.recording.start_iso))}</b> · <b>${esc(formatDuration(meta.recording.duration_s))}</b> total</div>

    <div class="grid-hero">
      <div class="card hero">
        <div class="hero-inner">
          <div class="hero-copy">
            <span class="verdict${narrative.plmiAbnormal ? ' flag' : ''}">
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none"><path d="M5 12l4.5 4.5L19 7" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/></svg>
              ${esc(narrative.verdict)}
            </span>
            <h1>${esc(narrative.headline)} —<br/>we saw <span class="hl">${esc(String(s.total_lms))} leg movement${s.total_lms === 1 ? '' : 's'}</span>, ${periodic}.</h1>
            <p class="lead">${esc(narrative.lead)}</p>
            <div class="hero-stats">
              <div class="hs"><div class="v num">${hrv === null ? '—' : String(Math.round(hrv))}<span style="font-size:12px;color:var(--text-mut)"> ms</span></div><div class="l">HRV RMSSD</div></div>
              <div class="hs"><div class="v num">${esc(String(s.total_lms))}</div><div class="l">Limb moves</div></div>
              <div class="hs"><div class="v num">${esc(String(s.total_plms))}</div><div class="l">Periodic</div></div>
            </div>
          </div>
          ${HERO_ART}
        </div>
      </div>
      <div class="card card-pad" style="display:flex;flex-direction:column;">${plmiGauge(s.plmi, narrative.plmiAbnormal)}</div>
    </div>

    <div class="kpi-row">${kpiCards(meta, narrative)}</div>

    <div class="main-cols">
      <div style="display:flex;flex-direction:column;gap:18px;">
        <section class="signals">
          <div class="sig-toolbar">
            <div class="card-title" style="margin:0;">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none"><path d="M2 12h4l2-7 4 14 3-9 2 4h5" stroke="var(--movement-2)" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg>
              Signal viewer <span class="fs-hint">· tap a chart to expand ⤢</span>
            </div>
            <div class="legend">
              <span class="leg"><span class="sw" style="background:var(--good-soft);border:1px solid var(--good)"></span>Limb movement</span>
              <span class="leg"><span class="sw" style="background:rgba(95,208,196,.3);border:1px solid var(--movement)"></span>Periodic LM</span>
              <span class="leg off"><span class="sw"></span>Apnea</span>
              <span class="leg off"><span class="sw"></span>Hypopnea</span>
              <span class="leg off"><span class="sw"></span>Arousal</span>
              <span class="leg off"><span class="sw"></span>Desat</span>
            </div>
          </div>
          <div class="sig-grid">
            ${CHANNEL_META.map((m, i) => `<div class="card sig-card"><div class="sig-head"><span class="gdot" style="background:${m.color}"></span>${esc(m.name)}</div><div class="sig-plot" id="sig-${String(i)}"></div></div>`).join('')}
          </div>
          <div class="viewer-note">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none"><circle cx="12" cy="12" r="9" stroke="var(--text-dim)" stroke-width="1.6"/><path d="M12 8h.01M11 12h1v4h1" stroke="var(--text-dim)" stroke-width="1.6" stroke-linecap="round"/></svg>
            <span><b>5 device channels</b> (Thoracic, Abdomen, Flow, ECG/HR, EEG) await hardware for this study — listed in the Montage.</span>
          </div>
        </section>
      </div>

      <aside class="right-rail">
        <div class="card card-pad">${MONTAGE}</div>
        <div class="card card-pad">${SPECTRO}</div>
      </aside>
    </div>

    <footer class="prov">
      <span class="pv"><svg width="14" height="14" viewBox="0 0 24 24" fill="none"><path d="M6 9l6-5 6 5v6l-6 5-6-5z" stroke="var(--text-dim)" stroke-width="1.6" stroke-linejoin="round"/></svg>build <code>@ ${esc(shortSha(git.sha, git.dirty))}</code></span>
      <span class="pv">raw anchor <code>${esc(anchorStr)}</code></span>
      <span class="pv"><svg width="14" height="14" viewBox="0 0 24 24" fill="none"><path d="M4 7c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3z" stroke="var(--text-dim)" stroke-width="1.4"/><path d="M4 7v10c0 1.7 3.6 3 8 3s8-1.3 8-3V7" stroke="var(--text-dim)" stroke-width="1.4"/></svg>charts read from <code>Zarr v2</code> boundary</span>
      <span class="disc"><b>Not a medical device.</b> ProtoSom is a proof-of-concept screening aid and does not diagnose. Discuss any concern with a qualified clinician.</span>
    </footer>
  </div>`;
}
