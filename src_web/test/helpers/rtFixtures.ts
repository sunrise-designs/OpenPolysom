import type { RtChannelDef, RtHello } from '../../src/rt_types';
import { RT_PROTOCOL } from '../../src/rt_types';

/**
 * The device's channel table, transcribed from the `SigDef` array in
 * `ESP32-C6-heart-idf/components/logger/logger.cpp` — the same rows, the same
 * order, the same digital/physical ranges the EDF+ header declares.
 *
 * Kept verbatim on purpose: this is the fixture the protocol tests check the
 * affine map against, so it has to be the firmware's numbers rather than
 * convenient round ones. If `SigDef` changes, this must change with it, and the
 * scaling assertions in `rt_protocol.test.ts` are what will notice.
 */
export const EDF_CHANNELS: readonly RtChannelDef[] = [
  { name: 'thoracic', edf_label: 'Thoracic', transducer: 'LDC1612 CH0', unit: 'counts', sample_rate_hz: 50, digital_min: -32767, digital_max: 32767, physical_min: -1e6, physical_max: 1e6 },
  { name: 'abdomen', edf_label: 'Abdomen', transducer: 'LDC1612 CH1', unit: 'counts', sample_rate_hz: 50, digital_min: -32767, digital_max: 32767, physical_min: -1e6, physical_max: 1e6 },
  { name: 'flow', edf_label: 'Flow', transducer: 'SDP800-125Pa', unit: 'mbar', sample_rate_hz: 50, digital_min: -32767, digital_max: 32767, physical_min: -100, physical_max: 100 },
  { name: 'ecg', edf_label: 'ECG', transducer: 'AD8232 ADC0', unit: 'ADC', sample_rate_hz: 100, digital_min: 0, digital_max: 4095, physical_min: 0, physical_max: 4095 },
  { name: 'accel0_x', edf_label: 'Accel0X', transducer: 'MMA8451 ch0', unit: 'mg', sample_rate_hz: 50, digital_min: -8192, digital_max: 8191, physical_min: -2000, physical_max: 2000 },
  { name: 'accel0_y', edf_label: 'Accel0Y', transducer: 'MMA8451 ch0', unit: 'mg', sample_rate_hz: 50, digital_min: -8192, digital_max: 8191, physical_min: -2000, physical_max: 2000 },
  { name: 'accel0_z', edf_label: 'Accel0Z', transducer: 'MMA8451 ch0', unit: 'mg', sample_rate_hz: 50, digital_min: -8192, digital_max: 8191, physical_min: -2000, physical_max: 2000 },
  { name: 'accel1_x', edf_label: 'Accel1X', transducer: 'MMA8451 ch1', unit: 'mg', sample_rate_hz: 50, digital_min: -8192, digital_max: 8191, physical_min: -2000, physical_max: 2000 },
  { name: 'accel1_y', edf_label: 'Accel1Y', transducer: 'MMA8451 ch1', unit: 'mg', sample_rate_hz: 50, digital_min: -8192, digital_max: 8191, physical_min: -2000, physical_max: 2000 },
  { name: 'accel1_z', edf_label: 'Accel1Z', transducer: 'MMA8451 ch1', unit: 'mg', sample_rate_hz: 50, digital_min: -8192, digital_max: 8191, physical_min: -2000, physical_max: 2000 },
  { name: 'rr', edf_label: 'RR', transducer: 'N/A', unit: 'ms', sample_rate_hz: 2.5, digital_min: 0, digital_max: 2000, physical_min: 0, physical_max: 2000 },
];

export const helloFixture = (over: Partial<RtHello> = {}): RtHello => ({
  type: 'hello',
  protocol: RT_PROTOCOL,
  device_uid: 'A1B2C3D4E5F6',
  recording_start_iso: '2026-08-09T23:00:00',
  timezone: 'GMT0BST,M3.5.0/1,M10.5.0',
  recording_active: true,
  record_duration_s: 10,
  channels: EDF_CHANNELS,
  ...over,
});

export const channel = (name: string): RtChannelDef => {
  const found = EDF_CHANNELS.find((c) => c.name === name);
  if (found === undefined) throw new Error(`no such fixture channel: ${name}`);
  return found;
};
