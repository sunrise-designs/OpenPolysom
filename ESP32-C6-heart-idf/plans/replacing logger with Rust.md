# Design sketch: `logger.cpp` ported to Rust on esp-idf-svc (std)

## Context

This is a design exploration, not an implementation task: the user wants to see what
`components/logger/logger.cpp` (SD mount, timestamped EDF file, biosignal buffering,
and the ESP_LOG-to-SD capture added earlier this session) would look like rewritten in
idiomatic Rust, targeting the ESP32-C6 (RISC-V) via the `esp-idf-svc`/`esp-idf-hal`
"std" support (real `std::sync`, `std::fs`, heap, etc. on top of ESP-IDF's newlib/FreeRTOS).
Per the user's answer, this is designed as a **clean idiomatic Rust module**, not
constrained to match the current C `extern "C"` ABI — i.e. as if this component were
being written fresh in Rust, not linked back into today's C++ `main.cpp`.

Nothing in the actual repo changes as part of this plan. The deliverable is this
document (with embedded Rust source) describing the design; nothing will be built,
compiled, or committed. Real edflib/SD-card semantics (signal defs, sample rates,
sd_mount flow, timestamp format, log-buffer flush cadence) are carried over unchanged
from the current C++ so the behavior stays equivalent — only the language/idioms change.

Research grounding for the trickiest part (hooking `esp_log_set_vprintf` from Rust,
which requires handling a C `va_list`) is cited inline; this is a known hard problem
in the esp-rs community, not something with an obvious textbook answer.

## Toolchain / crate choices

- Target: `riscv32imac-esp-espidf` (ESP32-C6 is RISC-V, so it builds with **upstream
  Rust + nightly**, not the Xtensa esp-rs LLVM fork — relevant because the log-capture
  design below needs the nightly `c_variadic` feature).
- Crates: `esp-idf-svc` (pulls in `esp-idf-hal`/`esp-idf-sys`, `std` feature enabled),
  `printf-compat` (safe-Rust `vsnprintf`-equivalent for the log hook, see below).
- `esp-idf-svc` re-exports the C bindings as `esp_idf_svc::sys::*`, and already wraps:
  - `esp_idf_svc::log::EspLogger` — binds the `log` crate facade to ESP-IDF logging,
    so Rust code calls `log::info!()` and it prints exactly like `ESP_LOGI` today.
  - `esp_idf_svc::sntp::EspSntp` — safe wrapper for SNTP (relevant to `wifi_ntp.c`, out
    of scope here but worth noting for a full port).
  - `esp_idf_svc::fs::fatfs::Fatfs` + `esp_idf_svc::io::vfs::MountedFatfs` — safe FAT
    mount wrapper. The published example (`examples/sd_mmc.rs` in `esp-idf-svc`) mounts
    over the 4-bit SDMMC bus; this board uses **SPI mode** (shares the display's SPI bus,
    per `sdspi_host.h`/`SDSPI_HOST_DEFAULT()` in the current C code), so the plan below
    uses the SPI-mode counterparts (`SdSpiHostDriver` / `SdCardDriver::new_spi(...)`,
    mirroring `esp_vfs_fat_sdspi_mount`). **Verify exact type names against the pinned
    `esp-idf-hal` version** — the SDMMC path is confirmed from the official example;
    the SPI path is inferred by symmetry with the C API and should be checked before
    actually implementing.
  - Once `MountedFatfs` is alive, `/sdcard` is a real POSIX-ish mount: `std::fs::File`,
    `Write`, and `File::sync_all()` (→ `fsync`) all just work — this replaces essentially
    all of the manual `fopen`/`fwrite`/`fflush`/`fileno`/`fsync` FFI from the C++ version.
  - EDF file writing itself: no Rust EDF crate exists. Per this project's own
    instruction to reuse the reference implementation with minimal adaptation, the plan
    keeps `components/edflib` as-is and calls it through `bindgen`-generated `unsafe
    extern "C"` bindings (same approach the C++ code already uses, just from Rust).

## The hard part: capturing all `ESP_LOG` output (the mutex/lock code)

### Why this is genuinely tricky in Rust

`esp_log_set_vprintf` wants a callback shaped like C's `vprintf`:
`fn(*const c_char, va_list) -> c_int`. In the C++ version this was trivial
(`va_copy` + `vsnprintf`). In Rust, **`va_list` has no portable safe representation on
stable Rust** — this is a known open problem in the esp-rs community itself (see
[esp-idf logger redirection: vprintf variadic function](https://users.rust-lang.org/t/esp-idf-logger-redirection-vprintf-variadic-function/95568),
which hit exactly this wall and didn't reach a clean resolution on stable).

The practical answer used by real Rust projects hooking C `vprintf`-style callbacks is
the [`printf-compat`](https://docs.rs/printf-compat/latest/printf_compat/) crate: a
`no_std` reimplementation of `printf`'s format-string parsing in safe Rust, built on top
of the *nightly* `c_variadic` feature (`core::ffi::VaList`). It's built for exactly this
use case — the crate's own docs call out "many C libraries provide a way to provide a
custom log callback... particularly useful in embedded systems like ESP-IDF projects."
Using it means: no hand-written C shim, but it does mean the crate is pinned to nightly
Rust (acceptable here since C6/RISC-V doesn't need the Xtensa fork anyway).

### The lock design

Only the **log buffer** needs synchronization — `ESP_LOG`/`log::info!` calls can come
from multiple FreeRTOS tasks concurrently (Wi-Fi, BLE, display, sensor loop), same as
in the C++ version. `std::sync::Mutex::new` and `Vec::new` are both `const fn`, so
(unlike the C++ code's `xSemaphoreCreateMutex()` + null-check dance in `logger_log_init`)
the mutex can just be a `static` with no runtime init step and no "is it created yet"
guard anywhere:

```rust
// log_capture.rs
use std::ffi::{c_char, c_int, VaList};
use std::fs::File;
use std::io::Write;
use std::sync::Mutex;
use printf_compat::{format, output};

const LOG_BUF_CAP: usize = 4096;

struct LogState {
    buf: Vec<u8>,
    file: Option<File>,
}

// No init-order dependency: this line alone replaces log_mutex/log_buf/log_buf_len/
// log_file from the C++ version, plus the "if (!log_mutex) return" guards everywhere.
static LOG_STATE: Mutex<LogState> = Mutex::new(LogState { buf: Vec::new(), file: None });

/// Caller must hold the lock. Mirrors log_flush_locked() in logger.cpp.
fn flush_locked(state: &mut LogState) {
    if state.buf.is_empty() {
        return;
    }
    if let Some(file) = state.file.as_mut() {
        let _ = file.write_all(&state.buf);
        let _ = file.sync_all(); // fsync — only touches the card on an explicit flush
    }
    state.buf.clear();
}

/// Mirrors logger_flush() — called at the same "safe" checkpoints as before
/// (every 10th EDF record, and on close), not on every log line.
pub fn flush() {
    let mut state = LOG_STATE.lock().unwrap();
    flush_locked(&mut state);
}

/// Mirrors log_open_file() — opens the SD log file next to the EDF file and
/// flushes whatever was buffered during boot before the card was mounted.
pub fn open_file(path: &std::path::Path) {
    let file = File::create(path).ok();
    let mut state = LOG_STATE.lock().unwrap();
    state.file = file;
    flush_locked(&mut state);
    // Any `let file_failed = file.is_none()` check + log::error! call must happen
    // *after* this function returns and the lock is dropped: logging from inside
    // the locked section would recurse into the vprintf hook below, which also
    // wants LOG_STATE's lock — same non-reentrancy hazard as the C++ mutex had.
}

/// Mirrors logger_close()'s log_close_file().
pub fn close_file() {
    let mut state = LOG_STATE.lock().unwrap();
    flush_locked(&mut state);
    state.file = None; // dropping the File closes the fd
}

/// The actual vprintf-like hook, installed once via esp_log_set_vprintf.
/// `printf_compat::format` parses `fmt`/`args` (the nightly VaList) into
/// anything implementing `fmt::Write` — here, a small on-stack formatter that
/// mirrors the C version's `char line[256]` + vsnprintf.
pub unsafe extern "C" fn log_capture_vprintf(fmt: *const c_char, args: VaList) -> c_int {
    let mut line = output::LineWriter::<256>::new(); // fixed-capacity fmt::Write sink
    let n = format(fmt, args, output::fmt_write(&mut line));

    if n > 0 {
        let bytes = line.as_bytes();
        if let Ok(mut state) = LOG_STATE.try_lock() {
            if state.buf.len() + bytes.len() > LOG_BUF_CAP {
                flush_locked(&mut state);
            }
            if bytes.len() <= LOG_BUF_CAP {
                state.buf.extend_from_slice(bytes);
            }
        }
        // try_lock (not a blocking timeout like the C++ pdMS_TO_TICKS(50)) — if
        // another task is mid-flush, this line is dropped rather than blocking
        // whatever FreeRTOS task is logging. Same trade-off as before, just
        // expressed as "skip if contended" instead of "wait 50ms then skip".
    }

    n as c_int
}

/// Mirrors logger_log_init() — call once, first thing, before any other init,
/// so boot logs (Wi-Fi/NTP) are buffered even before the SD card is mounted.
/// Note: printf_compat's callback still needs to also emit to the console
/// (the C++ version chained to `prev_vprintf`); that means holding onto
/// whatever esp_log_set_vprintf returns as the previous function pointer and
/// invoking it too, same as `prev_vprintf` in the C++ code — omitted here for
/// brevity but is a straightforward line-for-line port of that part.
pub fn init() {
    unsafe {
        esp_idf_svc::sys::esp_log_set_vprintf(Some(log_capture_vprintf));
    }
}
```

Compared to the C++ version, this removes: the `SemaphoreHandle_t` global + its
`if (!log_mutex) return` guards scattered across four functions, the manual
`xSemaphoreTake`/`xSemaphoreGive` pairing (a `MutexGuard`'s `Drop` does this
automatically, so there's no way to forget a `Give` on an early return), and the fixed
`char log_buf[4096]` + manual length bookkeeping (replaced by `Vec<u8>`, which still
never grows past 4096 in practice because `flush_locked` is called before it would).

## `logger.rs` — the rest of the port

```rust
// logger.rs
use esp_idf_svc::hal::gpio::AnyOutputPin;
use esp_idf_svc::hal::spi::SpiDriver;
use esp_idf_svc::fs::fatfs::Fatfs;
use esp_idf_svc::io::vfs::MountedFatfs;
use esp_idf_svc::sys::EspError;
use std::time::SystemTime;
use crate::edflib_sys as edf; // bindgen output for components/edflib/edflib.h

pub const RECORD_DURATION_S: i32 = 10;
pub const SAMPLES_50HZ: usize = 500;
pub const SAMPLES_1HZ: usize = 10;
const NUM_SIGNALS: usize = 11;
const EDF_FILE_DIR: &str = "/sdcard";
const EDF_FILE_PREFIX: &str = "biometric";

struct SigDef {
    label: &'static str,
    transducer: &'static str,
    dim: &'static str,
    rate: i32,
    dmax: i32,
    dmin: i32,
    pmax: f64,
    pmin: f64,
}

const SIGS: [SigDef; NUM_SIGNALS] = [
    SigDef { label: "Thoracic", transducer: "LDC1612 CH0", dim: "counts", rate: 50, dmax: 32767, dmin: -32767, pmax: 1e6, pmin: -1e6 },
    SigDef { label: "Abdomen",  transducer: "LDC1612 CH1", dim: "counts", rate: 50, dmax: 32767, dmin: -32767, pmax: 1e6, pmin: -1e6 },
    SigDef { label: "Flow",     transducer: "SDP800-125Pa", dim: "mbar", rate: 50, dmax: 32767, dmin: -32767, pmax: 2.0, pmin: -2.0 },
    SigDef { label: "ECG",      transducer: "AD8232 ADC",  dim: "ADC",  rate: 50, dmax: 4095,  dmin: 0,      pmax: 4095.0, pmin: 0.0 },
    SigDef { label: "Accel0X",  transducer: "MMA8451 ch0", dim: "mg",   rate: 50, dmax: 8191,  dmin: -8192,  pmax: 2000.0, pmin: -2000.0 },
    // ... Accel0Y/Z, Accel1X/Y/Z identical shape, RR at the end (rate: 1) —
    // straight data, no logic worth spelling out further here.
];

pub struct Samples {
    thoracic: [i32; SAMPLES_50HZ],
    abdomen: [i32; SAMPLES_50HZ],
    flow: [i32; SAMPLES_50HZ],
    ecg: [i32; SAMPLES_50HZ],
    accel: [[i32; SAMPLES_50HZ]; 6], // a0x,a0y,a0z,a1x,a1y,a1z
    rr: [i32; SAMPLES_1HZ],
}

pub struct Logger {
    _mounted_fatfs: MountedFatfs<Fatfs<SpiSdCard>>, // unmounts on Drop
    edf_handle: i32,
    samples: Samples,
    sample_idx: usize,
    rr_idx: usize,
    record_count: u32,
    ldc0_baseline: u32,
    ldc1_baseline: u32,
    baseline_ok: bool,
    log_file_path: std::path::PathBuf,
}

impl Logger {
    /// Mirrors sd_mount() + open_edf() + logger_init().
    pub fn init(spi: SpiDriver, cs: AnyOutputPin) -> Result<Self, EspError> {
        // SPI-mode SD mount (see "verify exact type names" note above); shares
        // the SPI bus display_init() already created, matching the C comment.
        let sd_card = esp_idf_svc::hal::sd::SdCardDriver::new_spi(
            esp_idf_svc::hal::sd::spi::SdSpiHostDriver::new(spi, cs, /* ... */)?,
        )?;
        let mounted_fatfs = MountedFatfs::mount(
            Fatfs::new_sdcard(0, sd_card)?, "/sdcard", /* max_files */ 4,
        )?;

        let now = SystemTime::now();
        let t = local_broken_down_time(now); // thin wrapper around libc localtime_r,
                                              // see "time formatting" note below

        let edf_path = format!(
            "{EDF_FILE_DIR}/{EDF_FILE_PREFIX}_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}.edf",
            t.year, t.month, t.day, t.hour, t.minute, t.second,
        );
        let log_file_path = format!(
            "{EDF_FILE_DIR}/{EDF_FILE_PREFIX}_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}.log",
            t.year, t.month, t.day, t.hour, t.minute, t.second,
        ).into();

        crate::log_capture::open_file(&log_file_path);

        let edf_handle = unsafe {
            edf::edfopen_file_writeonly(
                std::ffi::CString::new(edf_path.as_str()).unwrap().as_ptr(),
                edf::EDFLIB_FILETYPE_EDFPLUS as i32,
                NUM_SIGNALS as i32,
            )
        };
        if edf_handle < 0 {
            log::error!("edfopen failed: {edf_handle}");
            return Err(EspError::from_infallible::<{ esp_idf_svc::sys::ESP_FAIL }>());
        }

        unsafe {
            edf::edf_set_datarecord_duration(edf_handle, 1_000_000);
            for (i, sig) in SIGS.iter().enumerate() {
                edf::edf_set_label(edf_handle, i as i32, cstr(sig.label));
                edf::edf_set_transducer(edf_handle, i as i32, cstr(sig.transducer));
                edf::edf_set_samplefrequency(edf_handle, i as i32, sig.rate);
                edf::edf_set_digital_maximum(edf_handle, i as i32, sig.dmax);
                edf::edf_set_digital_minimum(edf_handle, i as i32, sig.dmin);
                edf::edf_set_physical_maximum(edf_handle, i as i32, sig.pmax);
                edf::edf_set_physical_minimum(edf_handle, i as i32, sig.pmin);
                edf::edf_set_physical_dimension(edf_handle, i as i32, cstr(sig.dim));
            }
            edf::edf_set_startdatetime(edf_handle, t.year, t.month, t.day, t.hour, t.minute, t.second);
        }

        log::info!("EDF recording started: {edf_path}");

        Ok(Self {
            _mounted_fatfs: mounted_fatfs,
            edf_handle,
            samples: Samples::zeroed(),
            sample_idx: 0,
            rr_idx: 0,
            record_count: 0,
            ldc0_baseline: 0,
            ldc1_baseline: 0,
            baseline_ok: false,
            log_file_path,
        })
    }

    /// Mirrors logger_record(). Only ever called from the sensor task, so no
    /// lock needed here (unlike the log buffer) — same assumption the C++
    /// code already relies on implicitly.
    pub fn record(&mut self, a0: [i16; 3], a1: [i16; 3], ldc0: u32, ldc1: u32, ecg: u16, pressure_mbar: f32, rr_ms: u16) {
        if !self.baseline_ok && ldc0 != 0 {
            self.ldc0_baseline = ldc0;
            self.ldc1_baseline = ldc1;
            self.baseline_ok = true;
        }

        let d0 = ldc0 as i32 - self.ldc0_baseline as i32;
        let d1 = ldc1 as i32 - self.ldc1_baseline as i32;
        self.samples.thoracic[self.sample_idx] = (d0 / 30).clamp(-32767, 32767);
        self.samples.abdomen[self.sample_idx] = (d1 / 30).clamp(-32767, 32767);
        self.samples.flow[self.sample_idx] = ((pressure_mbar * 32767.0 / 2.0) as i32).clamp(-32767, 32767);
        self.samples.ecg[self.sample_idx] = ecg as i32;
        self.samples.accel[0][self.sample_idx] = a0[0] as i32;
        self.samples.accel[1][self.sample_idx] = a0[1] as i32;
        self.samples.accel[2][self.sample_idx] = a0[2] as i32;
        self.samples.accel[3][self.sample_idx] = a1[0] as i32;
        self.samples.accel[4][self.sample_idx] = a1[1] as i32;
        self.samples.accel[5][self.sample_idx] = a1[2] as i32;

        if self.sample_idx % 50 == 49 && self.rr_idx < SAMPLES_1HZ {
            self.samples.rr[self.rr_idx] = rr_ms as i32;
            self.rr_idx += 1;
        }
        self.sample_idx += 1;

        if self.sample_idx >= SAMPLES_50HZ {
            unsafe {
                edf::edfwrite_digital_samples(self.edf_handle, self.samples.thoracic.as_ptr());
                edf::edfwrite_digital_samples(self.edf_handle, self.samples.abdomen.as_ptr());
                edf::edfwrite_digital_samples(self.edf_handle, self.samples.flow.as_ptr());
                edf::edfwrite_digital_samples(self.edf_handle, self.samples.ecg.as_ptr());
                for ch in &self.samples.accel {
                    edf::edfwrite_digital_samples(self.edf_handle, ch.as_ptr());
                }
                edf::edfwrite_digital_samples(self.edf_handle, self.samples.rr.as_ptr());
            }
            self.sample_idx = 0;
            self.rr_idx = 0;
            self.record_count += 1;

            if self.record_count % 10 == 0 {
                unsafe { edf::edfflush_file(self.edf_handle, self.record_count as i32) };
                log::info!(
                    "EDF: {} records ({:.1} min)",
                    self.record_count,
                    self.record_count as f32 * RECORD_DURATION_S as f32 / 60.0,
                );
                crate::log_capture::flush(); // piggyback on the EDF flush cadence
            }
        }
    }

    pub fn ldc0_baseline(&self) -> u32 { self.ldc0_baseline }
    pub fn ldc1_baseline(&self) -> u32 { self.ldc1_baseline }
    pub fn baseline_ok(&self) -> bool { self.baseline_ok }

    /// Mirrors logger_format_sd(). Consumes self (closes EDF + unmounts FAT)
    /// since formatting needs the card unmounted; caller re-inits afterward.
    pub fn format_sd(self) -> Result<(), EspError> {
        let raw_card = /* re-borrow the underlying SdCardDriver from self before drop */;
        drop(self); // runs Drop below: flushes+closes EDF and the log file
        unsafe { esp_idf_svc::sys::esp_vfs_fat_sdcard_format(cstr("/sdcard"), raw_card) };
        log::info!("SD card formatted");
        Ok(())
    }
}

impl Drop for Logger {
    /// Mirrors logger_close(). RAII replaces the C++ pattern of having to
    /// remember to call logger_close() before deep sleep / reformatting —
    /// here it happens automatically whenever a Logger goes out of scope,
    /// with an explicit `drop(logger)` before deep sleep if you want it eager.
    fn drop(&mut self) {
        unsafe { edf::edfclose_file(self.edf_handle) };
        crate::log_capture::close_file();
    }
}
```

Notes on the sketch above:

- `Samples::zeroed()` / the accel-channel-as-`[[i32; N]; 6]` grouping is a minor
  cleanup over the six separate `a0x_buf`/`a0y_buf`/... arrays in the C++ version —
  same memory layout, less repetition. Not required, easy to keep 1:1 with six
  named fields instead if a closer C++ mirror is preferred.
- `local_broken_down_time()` is a thin FFI wrapper around libc's `time()` +
  `localtime_r()` (via `esp_idf_svc::sys::{time, localtime_r, tm}`), same as the C++
  code. `chrono`/`chrono-tz` were considered and rejected: those crates expect a
  desktop-like `/etc/localtime`/IANA tzdata environment that doesn't exist on
  ESP-IDF, whereas the existing `setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1); tzset();`
  approach (added earlier this session in `wifi_ntp.c`) keeps working unchanged
  through the same libc calls.
- Error handling throughout uses `Result<_, EspError>` + `?` instead of
  `if (r != ESP_OK) { ESP_LOGE(...); return false; }` — `EspError` already wraps
  `esp_err_t` 1:1, so this is a very direct translation, not a redesign.
- The `format_sd()` sketch has a rough edge (getting the raw SD card handle back out
  before drop, so it can be reformatted) — this is exactly the kind of ownership
  wrinkle that's easy to hand-wave in a sketch but would need to be worked out for
  real; flagged here rather than papered over.

## What's out of scope / unverified

- Wi-Fi + SNTP (`wifi_ntp.c`) would port to `esp_idf_svc::wifi::EspWifi` +
  `esp_idf_svc::sntp::EspSntp` — not sketched here since the user's ask was
  specifically `logger.cpp` + the lock code, but it's the natural next file.
- Exact `esp-idf-hal` SPI-mode SD-card type names (`SdSpiHostDriver` etc.) are
  inferred by symmetry with the confirmed SDMMC example and the current C code's
  use of `sdspi_host.h`; would need a `cargo doc`/docs.rs check against the pinned
  crate version before writing real code.
- `printf-compat`'s exact API (`output::LineWriter`, `output::fmt_write`) is
  illustrative of the shape (safe `fmt::Write` sink fed by the crate's C-format
  parser) rather than verified against the crate's current API surface — confirm
  against `docs.rs/printf-compat` before implementing.
- This whole file assumes `edflib` stays a C library called via bindgen. If a
  "no unsafe FFI anywhere" version were wanted instead, EDF writing itself
  (a fairly small binary format) could be reimplemented natively in Rust — bigger
  scope, not attempted here since it contradicts the project's own "minimal
  adaptations from the reference implementation" guidance.

## Verification (if this were ever actually implemented)

Since this plan produces no code changes, there's nothing to verify now. If someone
picks this design up for real:

1. `cargo build --release` against `riscv32imac-esp-espidf` with `+nightly` (for
   `printf-compat`'s `c_variadic` dependency) must succeed, including the
   `edflib.h` bindgen step.
2. Flash + serial monitor: confirm boot-time Wi-Fi/NTP logs appear on the console
   (unchanged behavior) and, after SD mount, appear in `/sdcard/biometric_*.log`
   once pulled off the card.
3. Confirm `/sdcard/biometric_*.edf` opens correctly in an EDF viewer, matching
   today's C++ output bit-for-bit for the same recorded input.
4. Deliberately contend the log mutex (log from two tasks rapidly) and confirm no
   panics/deadlocks — the `try_lock`-and-drop-if-contended behavior in
   `log_capture_vprintf` should never block a FreeRTOS task's log call.
