# Pairing and key provisioning

How remotes get bound to a court (spec 10.8 / 14.5), and how ESP-NOW keys
are provisioned. The logic below is fully implemented and tested natively
(`components/application/pairing.*`, remote states in
`components/remote_core`); on hardware only the radio transport differs.

## Physical-presence pairing flow

```
organizer (court touchscreen)                remote
─────────────────────────────                ──────
Setup screen -> PAIR TEAM A/B
  opens 30 s pairing window                  hold button 5 s (while unpaired)
                                             -> PairingAdvertise: broadcasts
                                                PAIR_REQUEST every 500 ms
window shows short device id  <──────────────  (remote_id, boot_id, battery)
  "Remote 3F92 requests pairing"
organizer presses CONFIRM
  assignment persisted (ISettings)
  PAIR_ASSIGN broadcast        ──────────────>  filters by remote_id,
                                                stores court_id + team + channel
                                                (NVS), PairingSuccess feedback,
                                                -> Ready
```

Rules enforced by `PairingService`:

- Requests are ignored when no window is open.
- A remote currently assigned to the *other* team is never silently
  replaced; the organizer must unassign it first (diagnostics screen /
  future roster UI). Re-pairing to the same team refreshes in place.
- The newest advertising remote wins the candidate slot while the window is
  open; the window times out after 30 s.
- Confirmed assignments are persisted immediately and restored on boot
  (`load_assignments()`), so pairing survives power cycles.

Packets (`components/protocol`, CRC16-framed like all traffic):

| Packet | Direction | Fields |
|---|---|---|
| `PAIR_REQUEST` (0x03) | remote -> broadcast | remote_id, boot_id, fw_version, battery_mv |
| `PAIR_ASSIGN` (0x04) | court -> broadcast (filtered by remote_id) | court_id, remote_id, team, channel |

In the desktop simulator: `PAIR TEAM A/B` on the setup screen opens the
window, `p` puts that team's remote into pairing mode, `CONFIRM` completes
the flow. Pairings persist in `court-sim-data/pairings.txt`.

## ESP-NOW key provisioning (PMK / LMK)

ESP-NOW supports a global Primary Master Key (PMK) and a per-peer Local
Master Key (LMK); encrypted peers use both.

Scheme for this project:

- **PMK**: one per deployment (all courts of a venue share it). Compiled in
  from a gitignored header for P0.
- **LMK**: one per court, derived offline (not over the air) as
  `HMAC-SHA256(PMK, "court-" + court_id)[0:16]`. The remote receives the
  court's LMK during pairing *by provisioning, not broadcast*: for P0 the
  dev LMK ships in the same gitignored header; the polished flow moves LMK
  delivery into a wired/USB provisioning step.
- `PAIR_REQUEST` / `PAIR_ASSIGN` are sent unencrypted (they carry no
  secrets); point traffic uses the encrypted peer once paired.

Development keys live in `firmware/keys/dev_keys.h` (gitignored, see
`firmware/keys/dev_keys.example.h`). Never commit production keys; per spec
10.8 production provisioning is documented, deliberate, and per-venue.

Until encrypted peers are enabled during bring-up (M5), firmware runs
ESP-NOW unencrypted — same as the `espnow-linktest` proof — so the pairing
and scoring flows can be validated independently of key handling.
