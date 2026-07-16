# Serial monitor cheatsheet (XIAO ESP32-C6, native USB)

The C6 uses its built-in USB Serial/JTAG controller (no CP210x/CH340 chip), which
is flaky on Windows when `idf_monitor` toggles DTR/RTS to reset the board. If you
see:

```
Cannot configure port, something went wrong. Original message: PermissionError(13,
'A device attached to the system is not functioning.', None, 31)
```

try, in order:
1. Use a data-capable USB-C cable/port directly on the PC (not a hub).
2. Monitor with `--no-reset` (see below) so it doesn't toggle DTR/RTS on open.
3. Device Manager → "USB JTAG/serial debug unit" → Power Management → uncheck
   "Allow the computer to turn off this device to save power."
4. Device Manager → View → Show hidden devices → remove stale/greyed-out
   "USB Serial Device (COMx)" ghost entries, then replug the board.

## Monitor without touching PowerShell execution policy

```powershell
& 'C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe' 'C:\esp\v6.0.1\esp-idf\tools\idf_monitor.py' -p COM7 -b 921600 --no-reset --toolchain-prefix riscv32-esp-elf- 'c:\Repos\ProtoSom\ESP32-C6-heart-idf\build\polysom.elf'
```

## Monitor via idf.py (needs export.ps1 first)

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
C:\esp\v6.0.1\esp-idf\export.ps1
idf.py -p COM7 monitor --no-reset
```

`Set-ExecutionPolicy -Scope Process` only affects the current terminal window
and reverts when you close it — nothing persists on the machine.

If `export.ps1` fails with `tool <name> has no installed versions`, the IDF
tools install is incomplete. Fix with:

```powershell
C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe C:\esp\v6.0.1\esp-idf\tools\idf_tools.py install
```

For more detail on an `export.ps1` failure, set `$env:ESP_IDF_EXPORT_DEBUG=1`
before running it again.
