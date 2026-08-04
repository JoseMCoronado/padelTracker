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
  common/       Typed identifiers, Result type, structured log ring
  domain/       Pure scoring engine: config, state, events, reducer, projection
  protocol/     Radio packet serialization, CRC16, dedup, pairing packets
  application/  CourtService intent pipeline, conflict guard, PairingService
  persistence/  Durable CRC-framed event journal + crash recovery
  remote_core/  Portable remote state machine: debounce, retries, feedback
  ui/           LVGL screens + view models (runs natively and on device)
simulator/
  scorer-cli/   Interactive terminal scoreboard (no hardware needed)
  court-sim/    SDL desktop court: real UI + service + journal + RemoteCore
firmware/
  espnow-linktest/  ESP-NOW link proof for the DevKits (see its README)
  remote/           XIAO ESP32-C3 remote (esp32c3)
  court-display/    Waveshare 7B court unit (esp32s3)
tests/          Native unit tests (Catch2), incl. power-loss + lossy-radio sims
tools/          Build and test scripts
docs/           Scoring rules, protocol, pairing, pinout, bring-up runbooks
```

`components/` has zero ESP-IDF/LVGL dependencies and compiles natively on the
development machine; each component's CMakeLists also registers as an ESP-IDF
component for firmware builds. Firmware projects live under `firmware/` and
consume the same components via `EXTRA_COMPONENT_DIRS`.

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

## Court simulator (full UI, no hardware)

```bash
./build/native/simulator/court-sim/court-sim          # interactive
./build/native/simulator/court-sim/court-sim --tour   # screenshot every screen
```

The real 1024x600 LVGL UI in an SDL window, driven by the real
`CourtService`, journal, and `RemoteCore` remotes. Keys: `a`/`b` remote
press, `Shift+a`/`b` wired button, `l` packet loss, `p` pairing mode, `r`
power-cycle (journal recovery), `q` quit.

## Firmware builds

```bash
source ~/esp/esp-idf/export.sh
cd firmware/court-display && idf.py set-target esp32s3 && idf.py build
cd ../remote            && idf.py set-target esp32c3 && idf.py build
```

Per-app details: [firmware/court-display/README.md](firmware/court-display/README.md),
[firmware/remote/README.md](firmware/remote/README.md), and the DevKit link
soak in [firmware/espnow-linktest/README.md](firmware/espnow-linktest/README.md).
Pin assignments: [docs/HARDWARE_PINOUT.md](docs/HARDWARE_PINOUT.md); pairing:
[docs/PAIRING.md](docs/PAIRING.md).

## Hardware (prototype)

| Part | Role |
|---|---|
| Waveshare ESP32-S3 7" touch LCD, 1024x600 | Court unit (scoring authority) |
| 2x Seeed XIAO ESP32-C3 | Team A / Team B wearable remotes |
| 3x ESP32-S3 DevKitC-1 N16R8 | Bench dev boards, ESP-NOW test harness |
| 5x 30 mm illuminated arcade buttons | Court backup/service controls |

Toolchain setup is documented in [docs/TOOLCHAIN.md](docs/TOOLCHAIN.md).
