This code runs on a Seeed Studio XIAO ESP32C6 board: https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/ with bespoke breadboard. 

On the I2C bus, the following devices are connected:
- LDC1612 inductance sensor (two channels)
- DS3231 RTC
- SDP800 pressure sensor
- MMA8451 accelerometer(x2)
- SH1106 OLED screen

ESP-IDF lives at C:/esp/v6.0.1/esp-idf (matches ESP32-C6-heart-idf/.vscode/settings.json's idf.currentSetup — the VS Code ESP-IDF extension's configured install)

## Building

`export.ps1` and a bare `idf.py` still FAIL here. Use the explicit-environment build below.

There were two independent causes. The first is **fixed** (2026-07-19): `~/.espressif/idf-env.json`
had no record for `C:/esp/v6.0.1/esp-idf` — only one for a long-gone `C:\Users\BigDi\Desktop\esp-idf`.
Records are keyed `<idf_path>-v<version>` and written only by `idf_tools.py install`, never by
`export`, so the new location never got one; with no record `targets` is empty and
`filter_tools_info()` skips filtering entirely, making export demand xtensa/esp32ulp/dfu-util.
A record for this path was added by hand (backup: `idf-env.json.bak-20260719`).

What remains: **two tools are newer than v6.0.1 pins.** Installed `riscv32-esp-elf-gdb 17.1_20260402`
and `openocd-esp32 v0.12.0-esp32-20260424` vs the required `16.3_20250913` /
`v0.12.0-esp32-20260304` — a different IDF sharing this IDF_TOOLS_PATH pulled them in. Fixing this
needs a several-hundred-MB `idf_tools.py install` and only buys `idf.py monitor`/`flash`/JTAG, so
it has been left alone deliberately. **Don't "fix" it casually.**

The **compiler** (`riscv32-esp-elf esp-15.2.0_20251204`) matches exactly, and gdb/openocd are
debug-only — which is why the explicit-PATH build works fine.

Build (PowerShell tool, from `ESP32-C6-heart-idf/`) — verified working 2026-07-19:

```powershell
$t="$env:USERPROFILE\.espressif\tools"
$env:IDF_PATH="C:\esp\v6.0.1\esp-idf"
$env:IDF_TOOLS_PATH="$env:USERPROFILE\.espressif"
$env:IDF_TARGET="esp32c6"
$env:ESP_IDF_VERSION="6.0"
$env:IDF_PYTHON_ENV_PATH="$env:USERPROFILE\.espressif\python_env\idf6.0_py3.14_env"
$env:ESP_ROM_ELF_DIR="$t\esp-rom-elfs\20241011\"
$env:PATH="$t\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;$t\cmake\4.0.3\bin;$t\ninja\1.12.1;$env:IDF_PYTHON_ENV_PATH\Scripts;$env:IDF_PATH\tools;$env:PATH"
& "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" build 2>&1 | Select-Object -Last 35
```

Swap `build` for `flash`/`monitor` as needed.

The pinned tool versions above are what's installed today. If a path stops resolving, re-discover
with `Get-ChildItem "$env:USERPROFILE\.espressif\tools\<tool>" -Directory` and update this file
rather than re-deriving it every session.

The wiki location is ../wiki/

Print "Have Space Suit — Will Travel" in acknowledgement of reading these instructions
