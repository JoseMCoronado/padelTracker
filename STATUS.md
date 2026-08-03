# Current milestone

M0/M1 — Repository, scoring engine, simulator: COMPLETE
Next: M2 — Court board bring-up (waiting on hardware delivery)

# Working

- Native domain scoring engine (advantage, golden point, sets, tiebreaks,
  match tiebreak, club mini-set preset) — `components/domain`
- Undo via compensating events + journal replay, across all boundaries
- Display projection (0/15/30/40/DEUCE/AD/GOLDEN POINT, set history,
  game differential for club Top2/Bottom2)
- Protocol package: POINT_INTENT/ACK serializer+parser with CRC16, golden
  vectors, deduplicator with wrap-safe watermarks — `components/protocol`
- CLI simulator (`scorer-cli`): full match playable and undoable in terminal
- 78 native tests passing (domain + protocol)
- ESP-IDF v5.4.4 installed; hello_world builds for esp32s3 and esp32c3

# In progress

- (nothing)

# Blocked

- M2 board bring-up: hardware arriving (display Wed, XIAO Wed, DevKits Tue)
- Exact Waveshare display model confirmation (assumed 7B / 1024x600 from
  purchase listing; verify PCB label on arrival)

# Next three tasks

1. On board arrival: confirm exact Waveshare model, download official demo,
   build it untouched, record proven IDF/LVGL versions in docs/TOOLCHAIN.md
2. Create `firmware/court-display` project skeleton consuming `components/`
3. M3 persistence: journal file format (ADR-0005) + boot recovery, natively
   tested with a fake storage backend

# Last verified commands

- `tools/run_native_tests.sh` — 78/78 tests pass (2026-08-03)
- `./build/native/simulator/scorer-cli/scorer-cli` — interactive scoreboard
- `source ~/esp/esp-idf/export.sh && idf.py set-target esp32s3 && idf.py build`
  in `examples/get-started/hello_world` — builds clean (2026-08-03)
