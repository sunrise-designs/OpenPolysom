#!/usr/bin/env node
/**
 * Mock ProtoSom RT stream — replays a recorded EDF+ over the same WebSocket
 * protocol the device's `rt_stream` component serves, at wall-clock rate.
 *
 * This is the development and verification harness for the viewer's live mode:
 * it makes RT mode runnable without hardware, and — because it replays a real
 * recording rather than synthesising waveforms — it also makes the round-trip
 * checkable. A sample the viewer plots at elapsed time t must be the sample at
 * index t*rate in the file on disk.
 *
 *   node tools/mock_rt_server.mjs "../examples/17 July 2026/biometric_2026-07-16_23-00-00.edf"
 *   # then open index.html?rt=ws://localhost:8081/rt
 *
 * Options:
 *   --port <n>     listen port (default 8081)
 *   --speed <x>    replay rate multiplier (default 1; try 20 to fill charts fast)
 *   --start <s>    begin this many seconds into the recording (default 0)
 *   --drop <p>     drop this fraction of frames, 0..1, to exercise gap rendering
 *
 * No dependencies: Node ships no WebSocket *server*, so the ~40 lines of
 * RFC 6455 needed to serve text frames are inlined below rather than pulling in
 * `ws` for a dev-only tool.
 */

import { createServer } from 'node:http';
import { createHash } from 'node:crypto';
import { openSync, readSync, fstatSync, closeSync } from 'node:fs';
import { argv, exit } from 'node:process';

// ── args ─────────────────────────────────────────────────────────────────────

const args = argv.slice(2);
const FLAGS = new Set(['--port', '--speed', '--start', '--drop']);
const flag = (name, fallback) => {
  const i = args.indexOf(`--${name}`);
  return i === -1 ? fallback : args[i + 1];
};
const positional = [];
for (let i = 0; i < args.length; i++) {
  if (FLAGS.has(args[i])) { i++; continue; } // skip the flag and its value
  positional.push(args[i]);
}
const edfPath = positional[0];
const PORT = Number(flag('port', 8081));
const SPEED = Number(flag('speed', 1));
const START_S = Number(flag('start', 0));
const DROP = Number(flag('drop', 0));

if (edfPath === undefined) {
  console.error('usage: node tools/mock_rt_server.mjs <recording.edf> [--port n] [--speed x] [--start s] [--drop p]');
  exit(1);
}

// ── EDF+ reader ──────────────────────────────────────────────────────────────
// Only what a replay needs: the header's channel table, and record-by-record
// access to the interleaved int16 samples. Deliberately not a general EDF
// library — src_python/edf_reader.py is the real one.

const ascii = (buf, off, len) => buf.toString('ascii', off, off + len).trim();

function readEdf(path) {
  const fd = openSync(path, 'r');
  const head = Buffer.alloc(256);
  readSync(fd, head, 0, 256, 0);

  const headerBytes = Number(ascii(head, 184, 8));
  const nRecords = Number(ascii(head, 236, 8));
  const recordDurationS = Number(ascii(head, 244, 8));
  const nSignals = Number(ascii(head, 252, 4));
  const startDate = ascii(head, 168, 8); // dd.mm.yy
  const startTime = ascii(head, 176, 8); // hh.mm.ss

  const sigHead = Buffer.alloc(headerBytes - 256);
  readSync(fd, sigHead, 0, sigHead.length, 256);

  // Signal header fields are stored column-wise: all labels, then all
  // transducers, then all dimensions, and so on.
  const field = (offsetBlocks, width, i) => {
    const base = offsetBlocks.reduce((sum, [w]) => sum + w * nSignals, 0);
    return ascii(sigHead, base + i * width, width);
  };
  const blocks = [];
  const col = (width, i) => {
    const v = field(blocks, width, i);
    return v;
  };
  const take = (width) => {
    const values = Array.from({ length: nSignals }, (_, i) => col(width, i));
    blocks.push([width]);
    return values;
  };

  const labels = take(16);
  const transducers = take(80);
  const dimensions = take(8);
  const physMin = take(8).map(Number);
  const physMax = take(8).map(Number);
  const digMin = take(8).map(Number);
  const digMax = take(8).map(Number);
  take(80); // prefiltering, unused
  const samplesPerRecord = take(8).map(Number);

  const recordSamples = samplesPerRecord.reduce((a, b) => a + b, 0);
  const recordBytes = recordSamples * 2;
  const onDiskRecords = Math.floor((fstatSync(fd).size - headerBytes) / recordBytes);
  // A recording cut short (or still being written) has a header count that
  // over-states what is actually on the card; trust the file size.
  const records = nRecords > 0 ? Math.min(nRecords, onDiskRecords) : onDiskRecords;

  const offsets = samplesPerRecord.reduce((acc, n) => [...acc, acc[acc.length - 1] + n], [0]);

  const readRecord = (index) => {
    const buf = Buffer.alloc(recordBytes);
    readSync(fd, buf, 0, recordBytes, headerBytes + index * recordBytes);
    return Array.from({ length: nSignals }, (_, s) => {
      const out = new Int16Array(samplesPerRecord[s]);
      for (let k = 0; k < out.length; k++) out[k] = buf.readInt16LE((offsets[s] + k) * 2);
      return out;
    });
  };

  return {
    close: () => { closeSync(fd); },
    nSignals, records, recordDurationS, samplesPerRecord, readRecord,
    labels, transducers, dimensions, physMin, physMax, digMin, digMax,
    startIso: (() => {
      const [dd, mm, yy] = startDate.split('.');
      const [hh, mi, ss] = startTime.split('.');
      const year = Number(yy) >= 85 ? `19${yy}` : `20${yy}`;
      return `${year}-${mm}-${dd}T${hh}:${mi}:${ss}`;
    })(),
  };
}

/**
 * EDF label → canonical snake_case channel name (zarr-schema-spec §3.2). The
 * device's own `rt_stream` emits these names directly; here they are recovered
 * from the recorded header so a replay is indistinguishable on the wire.
 */
const CANONICAL = {
  Thoracic: 'thoracic', Abdomen: 'abdomen', Flow: 'flow', ECG: 'ecg',
  Accel0X: 'accel0_x', Accel0Y: 'accel0_y', Accel0Z: 'accel0_z',
  Accel1X: 'accel1_x', Accel1Y: 'accel1_y', Accel1Z: 'accel1_z',
  RR: 'rr',
};

// ── minimal RFC 6455 server frames ───────────────────────────────────────────

const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

/** Server→client text frame. No masking (servers must not mask), no fragmentation. */
function encodeTextFrame(text) {
  const payload = Buffer.from(text, 'utf8');
  const len = payload.length;
  const header = len < 126 ? Buffer.from([0x81, len])
    : len < 65536 ? Buffer.concat([Buffer.from([0x81, 126]), (() => { const b = Buffer.alloc(2); b.writeUInt16BE(len); return b; })()])
      : Buffer.concat([Buffer.from([0x81, 127]), (() => { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(len)); return b; })()]);
  return Buffer.concat([header, payload]);
}

// ── replay ───────────────────────────────────────────────────────────────────

const edf = readEdf(edfPath);
// EDF+ files carry a mandatory "EDF Annotations" signal holding TAL text, not
// samples. It is not a channel and must not be streamed as one.
const isAnnotation = (label) => label.toLowerCase().replace(/\s+/g, '') === 'edfannotations';
const signalIdx = Array.from({ length: edf.nSignals }, (_, i) => i).filter((i) => !isAnnotation(edf.labels[i]));
const channels = signalIdx.map((i) => ({
  name: CANONICAL[edf.labels[i]] ?? edf.labels[i].toLowerCase(),
  edf_label: edf.labels[i],
  transducer: edf.transducers[i],
  unit: edf.dimensions[i],
  sample_rate_hz: edf.samplesPerRecord[i] / edf.recordDurationS,
  digital_min: edf.digMin[i],
  digital_max: edf.digMax[i],
  physical_min: edf.physMin[i],
  physical_max: edf.physMax[i],
}));

console.log(`[mock-rt] ${edfPath}`);
console.log(`[mock-rt] ${String(edf.records)} records x ${String(edf.recordDurationS)}s, ${String(channels.length)} channels, start ${edf.startIso}`);
channels.forEach((c) => { console.log(`[mock-rt]   ${c.name.padEnd(10)} ${String(c.sample_rate_hz).padStart(5)} Hz  ${c.unit}`); });

const server = createServer((_req, res) => { res.writeHead(426); res.end('websocket only'); });

server.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'];
  if (key === undefined) { socket.destroy(); return; }
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
    + `Sec-WebSocket-Accept: ${createHash('sha1').update(key + WS_GUID).digest('base64')}\r\n\r\n`,
  );
  console.log(`[mock-rt] client connected (${req.url ?? '/'})`);

  const send = (obj) => {
    if (!socket.destroyed) socket.write(encodeTextFrame(JSON.stringify(obj)));
  };

  send({
    type: 'hello',
    protocol: 'protosom.rt/1.0.0',
    device_uid: 'MOCKED0FFEE1',
    recording_start_iso: edf.startIso,
    timezone: 'GMT0BST,M3.5.0/1,M10.5.0',
    recording_active: true,
    record_duration_s: edf.recordDurationS,
    channels,
  });

  // Frames are 100 ms of every channel, the same cadence the firmware uses, cut
  // out of whichever EDF record covers that instant.
  const FRAME_MS = 100;
  const frameS = FRAME_MS / 1000;
  let elapsed = START_S;
  let seq = 0;
  let cachedIndex = -1;
  let cached = null;

  const record = (index) => {
    if (index !== cachedIndex) { cached = edf.readRecord(index); cachedIndex = index; }
    return cached;
  };

  const timer = setInterval(() => {
    if (elapsed >= edf.records * edf.recordDurationS) {
      console.log('[mock-rt] end of recording');
      clearInterval(timer);
      clearInterval(statusTimer);
      socket.end();
      return;
    }

    seq += 1;
    const drop = Math.random() < DROP;
    if (!drop) {
      const blocks = {};
      channels.forEach((ch, c) => {
        const s = signalIdx[c];
        const rate = ch.sample_rate_hz;
        const n0 = Math.round(elapsed * rate);
        const n1 = Math.round((elapsed + frameS) * rate);
        if (n1 <= n0) return; // slower than the frame period (RR at 2.5 Hz)

        const perRecord = edf.samplesPerRecord[s];
        const v = [];
        for (let n = n0; n < n1; n++) {
          const rec = Math.floor(n / perRecord);
          if (rec >= edf.records) break;
          v.push(record(rec)[s][n % perRecord]);
        }
        if (v.length > 0) blocks[ch.name] = { n0, v };
      });
      send({ type: 'samples', seq, blocks });
    }
    elapsed += frameS;
  }, FRAME_MS / SPEED);

  const statusTimer = setInterval(() => {
    send({
      type: 'status',
      recording: true,
      elapsed_s: Math.round(elapsed),
      batt_pct: 74,
      sd_error: false,
      dropped_frames: 0,
      clients: 1,
    });
  }, 1000);

  const stop = () => {
    clearInterval(timer);
    clearInterval(statusTimer);
    console.log('[mock-rt] client gone');
  };
  socket.on('close', stop);
  socket.on('error', stop);
});

server.listen(PORT, () => {
  console.log(`[mock-rt] listening on ws://localhost:${String(PORT)}/rt  (speed x${String(SPEED)}, drop ${String(DROP)})`);
  console.log(`[mock-rt] open  index.html?rt=ws://localhost:${String(PORT)}/rt`);
});
