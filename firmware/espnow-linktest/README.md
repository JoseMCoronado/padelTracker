# ESP-NOW link test (two ESP32-S3 DevKitC-1)

Proves the real radio link before the court/remote hardware exists: peer
setup, channel config, the callback-enqueue pattern, retry policy, dedup
under induced ACK loss, and round-trip latency.

One binary, two roles (menuconfig): **sender** plays the remote, **receiver**
plays the court. Discovery is zero-config — the sender broadcasts until the
first ACK, then locks onto the receiver's MAC.

## Build and flash

```sh
source ~/esp/esp-idf/export.sh
cd firmware/espnow-linktest

# Board 1: receiver
idf.py set-target esp32s3
idf.py menuconfig        # ESP-NOW linktest configuration -> Role -> Receiver
idf.py -p /dev/cu.usbmodemXXX flash monitor

# Board 2: sender (second terminal; default role is Sender)
idf.py -p /dev/cu.usbmodemYYY flash monitor
```

Both boards must use the same Wi-Fi channel (default 6) and court id
(default 1). The receiver drops `LINKTEST_ACK_DROP_PCT` percent of ACKs
(default 20) to force sender retries.

## What happens

1. Sender waits 3 s, then fires a scripted burst of 500 presses
   (`LINKTEST_BURST_COUNT`), one intent identity per press, retrying per the
   protocol policy (450 ms ACK timeout, 5 attempts, 0/80/180/350/650 ms
   backoff).
2. After the burst, the BOOT button on the sender sends single debounced
   presses.

## Acceptance (spec M4 rehearsal)

- Receiver log: `applied` == 500 exactly (presses the sender reported as
  failed reduce this bound; with 20% ACK drop, failures should be zero).
- Receiver `duplicates` > 0 (proof the induced loss exercised dedup) while
  `applied` never double-counts.
- Sender log: `accepted + dup_accepted == 500`, `failed == 0`, and
  press-to-ACK latency min/avg/max printed every 50 presses and at the end.

Record the numbers in `STATUS.md` when the soak has been run on hardware.
