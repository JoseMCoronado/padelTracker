# Radio protocol

Transport: ESP-NOW encrypted unicast (court <-> remote), application-layer ACK
on top. This document covers the wire format implemented in
`components/protocol`. Framing is explicit byte-by-byte little-endian
serialization with a CRC16 trailer — never raw struct memory.

- Protocol version: 1
- Magic: ASCII `PS` (0x50 0x53)
- CRC: CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF), computed over all bytes
  before the CRC field.
- Intent identity: `(remote_id, boot_id, sequence)`. Retries reuse the same
  identity. `boot_id` is random per remote boot.

## POINT_INTENT (remote -> court), 31 bytes

| Field | Size | Notes |
|---|---|---|
| magic | 2 | "PS" |
| protocol_version | 1 | 1 |
| message_type | 1 | 0x01 |
| court_id | 2 | LE |
| remote_id | 4 | LE |
| boot_id | 4 | LE |
| sequence | 4 | LE |
| team_id | 1 | 1 = A, 2 = B |
| action | 1 | 0x01 = AWARD_POINT |
| button_duration_ms | 2 | LE |
| battery_mv | 2 | LE, 0 = unknown |
| monotonic_ms | 4 | LE |
| flags | 1 | reserved, 0 |
| crc16 | 2 | LE |

## ACK (court -> remote), 31 bytes

| Field | Size | Notes |
|---|---|---|
| magic | 2 | "PS" |
| protocol_version | 1 | 1 |
| message_type | 1 | 0x02 |
| court_id | 2 | LE |
| remote_id | 4 | LE, echoed |
| boot_id | 4 | LE, echoed |
| sequence | 4 | LE, echoed |
| ack_status | 1 | see below |
| state_revision | 8 | LE |
| team_a_display_code | 1 | compact score projection (diagnostic) |
| team_b_display_code | 1 | compact score projection (diagnostic) |
| crc16 | 2 | LE |

### AckStatus

| Value | Name |
|---|---|
| 1 | Accepted |
| 2 | DuplicateAccepted |
| 3 | RejectedNotInMatch |
| 4 | RejectedWrongTeam |
| 5 | RejectedUnpaired |
| 6 | RejectedPaused |
| 7 | RejectedConflict |
| 8 | RejectedInvalidPacket |
| 9 | ErrorStorage |

### Display codes

Compact one-byte projection for remote diagnostics (authoritative display is
the court screen): 0-3 = raw points 0/15/30/40, 40 = advantage, 41 = golden
point pending, 100 + n = tiebreak points n (capped at 155), 255 = not playing.

## Parsing rules

Parsers validate, in order: minimum header length, magic, version, message
type, exact length for the type (short = Truncated, long = Oversized), CRC,
then field enums. Any failure yields a typed `ProtocolError`; malformed input
never produces a packet object.

## Deduplication

See ADR-0007. Per-remote watermark of highest accepted sequence with wrap-safe
serial arithmetic; equal or up to 64 behind = Duplicate (re-ACK), further
behind = Stale (reject), new boot_id resets the entry. Watermarks are
serializable for persistence so a court reboot cannot double-apply a retry.

## Retry policy (remote side, implemented in M4 firmware)

```yaml
ack_timeout_ms: 450
max_attempts: 5
backoff_ms: [0, 80, 180, 350, 650]
```

The remote never allocates a new sequence while an intent is pending; a repeat
press during the pending window retransmits the same identity.
