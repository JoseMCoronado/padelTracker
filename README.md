# Padel Smart Court

Offline-first padel scoring system: a 7-inch touchscreen court unit (Waveshare
ESP32-S3-Touch-LCD-7B) plus one clip-on wireless remote per team (Seeed XIAO
ESP32-C3, ESP-NOW). One press after a rally scores the point exactly once, with
haptic/LED confirmation on the remote.

The master specification lives in
[padel-smart-court-master-spec.md](padel-smart-court-master-spec.md).
Current progress is tracked in [STATUS.md](STATUS.md); architecture decisions in
[DECISIONS.md](DECISIONS.md).

## Repository layout

```text
components/
  common/     Typed identifiers, Result type — shared by everything
  domain/     Pure scoring engine: config, state, events, reducer, projection
  protocol/   Radio packet serialization, CRC16, intent deduplication
simulator/
  scorer-cli/ Interactive terminal scoreboard (no hardware needed)
tests/        Native unit tests (Catch2)
tools/        Build and test scripts
docs/         Scoring rules, radio protocol, toolchain notes
```

`components/` has zero ESP-IDF/LVGL dependencies and compiles natively on the
development machine. Firmware projects (court display, wearable remote) will be
added under `firmware/` at board bring-up and reuse the same components.

## Build and test (native, no hardware)

Requires CMake >= 3.20 and a C++17 compiler.

```bash
tools/run_native_tests.sh
```

This configures `build/native/`, builds everything, and runs the full test
suite.

## Score simulator

```bash
cmake -S . -B build/native && cmake --build build/native -j
./build/native/simulator/scorer-cli/scorer-cli
```

Commands: `a` / `b` award a point, `undo`, `state`, `new <preset>`, `reset`,
`presets`, `help`, `quit`. Presets: `standard`, `golden`, `club`,
`tiebreak-final`.

## Hardware (prototype)

| Part | Role |
|---|---|
| Waveshare ESP32-S3 7" touch LCD, 1024x600 | Court unit (scoring authority) |
| 2x Seeed XIAO ESP32-C3 | Team A / Team B wearable remotes |
| 3x ESP32-S3 DevKitC-1 N16R8 | Bench dev boards, ESP-NOW test harness |
| 5x 30 mm illuminated arcade buttons | Court backup/service controls |

Toolchain setup is documented in [docs/TOOLCHAIN.md](docs/TOOLCHAIN.md).
