# Toolchain

## Native (host) build

- macOS, AppleClang 17 (Xcode command line tools), C++17
- CMake >= 3.20 (host build tested with Homebrew CMake 3.29.1)
- Catch2 v3.5.2, pinned via CMake FetchContent in `tests/CMakeLists.txt`
- One command: `tools/run_native_tests.sh`

## ESP-IDF (firmware)

| Item | Value |
|---|---|
| ESP-IDF | v5.4.4 (provisional pin, see ADR-0006) |
| Location | `~/esp/esp-idf` (shallow clone of tag v5.4.4) |
| Tools dir | `~/.espressif` |
| Python env | `idf5.4_py3.9_env` (system Python 3.9.6, arm64) |
| CMake | 3.30.2 (IDF-managed, universal binary) |
| Ninja | 1.12.1 (IDF-managed, universal binary) |
| Xtensa toolchain | xtensa-esp-elf esp-14.2.0_20260121 (ESP32-S3) |
| RISC-V toolchain | riscv32-esp-elf (ESP32-C3) |
| Installed targets | esp32s3 (court unit, DevKits), esp32c3 (remotes) |

The final version pin happens at M2 board bring-up: build the untouched official
Waveshare demo for the exact purchased board, record the IDF/LVGL versions it
proves, and update this file (spec section 5.1 version policy).

### Usage

```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3   # or esp32c3
idf.py build
idf.py -p <PORT> flash monitor
```

### Verification performed (2026-08-03)

- `examples/get-started/hello_world` builds clean for `esp32s3`.
- `examples/get-started/hello_world` builds clean for `esp32c3`.
- Flash/monitor not yet verified (hardware not delivered).

### Apple Silicon gotchas (hit during install, already fixed)

This machine runs arm64 with an Intel (x86_64) Homebrew under `/usr/local`,
which caused two Rosetta architecture mismatches:

1. `install.sh` created the IDF Python env with x86_64 wheels (`psutil`,
   `pydantic_core`, `cffi`, `bitarray`, `tibs`, `tree_sitter`, `PyYAML`),
   which fail to import from an arm64 shell. Fix applied: force-reinstall the
   affected packages with the venv's own pip (which selects arm64 wheels), and
   `psutil` was rebuilt from source as a universal binary.
2. IDF initially used the Homebrew x86_64 `cmake`, which re-spawned Python
   under Rosetta and failed against the now-arm64 venv. Fix applied: install
   IDF-managed universal cmake/ninja via
   `python tools/idf_tools.py install cmake ninja`; `export.sh` puts them
   first on PATH.

If the Python env is ever recreated (`install.sh` after an IDF update), expect
to repeat fix 1, or run the installer from an arm64-native context end to end.
