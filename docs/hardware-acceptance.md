# Hardware acceptance checklist (B11)

Everything in this file is **outstanding**. No board has been connected, nothing has been
flashed, no card has been inserted. The software side of B11 is covered by the
`app_acceptance` host group; this file is the part that cannot be.

Each item states what to do, what to expect, and what evidence to keep. An item is only
done when the evidence exists.

## Rules for this checklist

- **Do not write eFuses, change secure-boot or flash-encryption configuration, or erase
  the whole flash.** Nothing here needs any of that.
- RF, provisioning and control measurements are only against a network and devices the
  operator owns or has explicitly authorised.
- Record the exact commit, the build command and the serial log with each result. A
  result without its commit is not evidence.
- "It seemed to work" is not a result. Each item names the observable that settles it.

## 0. Preparation

| | Item |
|---|---|
| 0.1 | Board: Waveshare ESP32-C6-Touch-LCD-1.9, connected over its USB serial port. Record the port (`COM…` / `/dev/tty…`). |
| 0.2 | A microSD card, FAT32, with `/nearby/db/` present. Record its capacity and free space. |
| 0.3 | `firmware/sdkconfig.defaults` unchanged; `idf.py set-target esp32c6`. |
| 0.4 | Build: `cd firmware && idf.py build`. Record the `one_os.bin` size **and** the `idf.py size` output. The binary size is not a RAM measurement. |
| 0.5 | Flash: `idf.py -p <PORT> flash monitor`. Record the boot log verbatim from reset. |
| 0.6 | Optional but decides B7: a second ESP32 running the ESP-IDF `bleprph` example (or a Linux host with BlueZ `btgatt-server`) as a controllable GATT peripheral. |
| 0.7 | Optional but decides B7: an ESPHome node reachable on the same LAN, with its API encryption key. |
| 0.8 | Optional but decides B8: a standard Zigbee device and, if the device is not already on a network, a way to put it into pairing mode. |

## 1. Boot and diagnostics

| | Check | Expected | Evidence |
|---|---|---|---|
| 1.1 | Boot with no SD card | Boot completes. `request 2 resources` reports `db_state=sd_missing`. Scan still runs. | serial log |
| 1.2 | `request 1 version` | Firmware identity, no credentials | serial log |
| 1.3 | `request 3 resources` | Every field populated; `db_state`, `db`, `db_path` present. **These are the first real heap/stack numbers this project will have.** | serial log |
| 1.4 | Insert the card, then `request 4 scan full` and `request 3 resources` again | `db_state=ready` **without a reboot** — the reopen before enrichment is what makes this work | serial log |
| 1.5 | Nothing in any response contains the STA password, the AP password or the session token | confirmed by reading the log | serial log |

## 2. Scan and device state

| | Check | Expected | Evidence |
|---|---|---|---|
| 2.1 | `request 4 scan full` with the card present | All stages reported with a terminal state; `partial`/`truncated` flags match what actually happened | serial log |
| 2.2 | `request 5 devices` | Real APs and BLE devices with names, protocol, recognition, availability, signal | serial log |
| 2.3 | `request 6 entities` | Read-only entities for the exact facts observed (signal, channel, last seen, tx power). No invented sensor. | serial log |
| 2.4 | Cancel a scan mid-flight (`request 7 cancel 4` style) | The scan ends CANCELED; devices are not swept; a later scan works | serial log |
| 2.5 | Two scans back to back | No duplicate devices; the device count is stable | serial log |
| 2.6 | Wi-Fi coverage: does a device that is present appear every round? | Record any device missed in one round and seen in the next — this is the input the two-round freshness rule is tuned against | serial log |
| 2.7 | Turn an AP off, then scan twice | First miss: still listed, `stale`. Second miss: gone. | serial log |

## 3. Recognition database

| | Check | Expected | Evidence |
|---|---|---|---|
| 3.1 | Card with no corpus on it | `db_state=file_missing`; devices still appear as generic | serial log |
| 3.2 | Copy the fixture corpus to `/nearby/db/devices.nbdb`, scan | `db_state=ready`, `db` shows version and profile count | serial log |
| 3.3 | Corrupt one byte of the corpus on the card, reopen (or scan) | `db_state=corrupt`; **no partial recognition**; devices stay generic | serial log |
| 3.4 | Replace the corpus while the device is running, then scan | The new version is reported without a reboot | serial log |
| 3.5 | Pull the card while running, then scan | `db_state=sd_missing`; the scan still completes; devices are not lost | serial log |
| 3.6 | Re-insert the card, scan | Recognition returns without a reboot | serial log |
| 3.7 | Measure the time from scan start to enrichment completion with the real corpus | Record it. The body checksum streams the whole file, so this is the SD read time. | serial log with timestamps |

## 4. Provisioning (B6)

| | Check | Expected | Evidence |
|---|---|---|---|
| 4.1 | Start the portal | The AP SSID from the device's own presentation appears in a scan from a phone | phone screenshot |
| 4.2 | The AP password is only on the device screen / local serial output | It appears **nowhere** in the log or in `/api/status` | serial log + browser |
| 4.3 | `GET /api/status` | JSON as documented; no password or token field | browser / curl output |
| 4.4 | `POST /api/wifi/connect` with a wrong password | Reported as a failure; the stored credentials remain for retry | browser + serial log |
| 4.5 | Correct credentials | STA connects; `/api/status` shows `sta_state=connected` and an address from the real netif | browser |
| 4.6 | Reboot | The device reconnects **without** the portal, and without the portal being started | serial log |
| 4.7 | `POST /api/db/upload` with the corpus | Streams, validates, replaces; old corpus intact on failure | browser + card inspection on a PC |
| 4.8 | Interrupt an upload mid-way, then restart the device | Exactly one usable corpus at the path; the `.part` is gone | card inspection |
| 4.9 | Cut power during an upload, restart | Recovery restores or promotes exactly one usable corpus — the branch that `app_db_import` was written for and that only a board can actually exercise | card inspection |
| 4.10 | Scan while the portal is open | The portal holds the operation gate, so a scan is refused BUSY rather than the two fighting over the radio | serial log |

## 5. Control (B10)

The control loop is now the path the `control` command actually takes
(`firmware/main/app_runtime.c` calls `app_control_submit()`). With no backend
registered the correct answer is `NO_BACKEND`; the items below need a real
writable device.

| | Check | Expected | Evidence |
|---|---|---|---|
| 5.1 | `request 20 control switch.nothing turn_on` with no backend registered | refused with `unsupported`, detail `no_backend` — **not** `ok` and **not** `not_implemented` | serial log |
| 5.2 | `request 21 control <entity> <action>` for an entity that does not exist | refused `not_found`, detail `unknown_entity` | serial log |
| 5.3 | `request 22 control <entity> <bad-action>` for a known entity | refused `unsupported` | serial log |
| 5.4 | `request 23 control <entity> <action> <out-of-range value>` | refused `invalid_argument`, detail `out_of_range` | serial log |
| 5.5 | Two controls for one entity without the first completing | second refused `busy` | serial log |
| 5.6 | A real ESPHome or Zigbee device, with a writable recipe in the corpus | needed before anything below can run | — |
| 5.7 | Send a control | Accepted, and the response names the state as `pending` — not the requested state | serial log |
| 5.8 | The device confirms | The state changes only then | serial log |
| 5.9 | Send a control the device refuses | The state returns to the previous confirmed value; the error is reported | serial log |
| 5.10 | Send a control, then pull the device's power before it answers | The control times out and the previous state is restored | serial log |
| 5.11 | Scan again while a control is pending, and let the device vanish | `reconcile` fails the pending control rather than leaving it pending forever | serial log |

## 5b. BLE GATT central (B7)

The session module (`firmware/main/app_ble_gatt.c`) is host-tested with a
scripted backend, and the firmware adapter that binds it to `esphome_ble_gatt_*`
now exists (`firmware/main/app_ble_gatt_native.c`) with its own host group
(`app_ble_native`, 96 checks, passing locally and on CI). **Nothing below has run
against a radio**: the adapter has never been executed against a real peer, so
item 5b.1 still gates everything else.

| | Check | Expected | Evidence |
|---|---|---|---|
| 5b.1 | A second ESP32 running the ESP-IDF `bleprph` example, or a Linux host with BlueZ `btgatt-server` | needed before anything below can run | — |
| 5b.2 | Scan, then `request 5 devices`; compare the printed address with the peripheral's own | the byte order is the one the scan evidence uses — this settles the address-order question in the ledger. The adapter currently performs **no** reversal, so if this item fails the fix belongs in the adapter | serial log + peripheral log |
| 5b.3 | Open a GATT session | connect and discovery run exactly once; the radio is held for the session and handed back on close | serial log |
| 5b.4 | Compare the discovered service/characteristic/descriptor counts with nRF Connect's view | identical, and `truncated` set if the peer exceeds the session's bounds | screenshots + serial log |
| 5b.5 | Read a known characteristic, then one whose value exceeds the buffer | byte-exact value; the oversized read is refused, not truncated silently | serial log |
| 5b.6 | Write a characteristic with and without response; then write a read-only one | the peripheral's value changes; the refusal is a named error | serial log |
| 5b.7 | Subscribe, notify twice, then `cancel` mid-notification | both notifications delivered; **no callback after cancel returns**, and no crash | serial log |
| 5b.8 | Cut the peripheral's power mid-operation | the operation reports `peer_gone`, not success | serial log |
| 5b.9 | 100 × connect/discover/subscribe/notify/close against two peripherals, alternating | min free heap does not trend down; the second peripheral never appears pre-disconnected | serial log over 100 rounds |

### 5b-bis. BLE control (the chain behind `app_ctl_ble`)

The controller, its firmware binding and the drivability switch are all in place and
host-tested (`app_ctl_ble` 85 checks, `app_ctl_ble_gatt` 53 checks). What no test can
reach is a peripheral that actually answers, so these items are the ones that decide
whether the chain is real. **Item 5b.1 still gates them.**

| | Check | Expected | Evidence |
|---|---|---|---|
| 5b.10 | A peripheral exposing a **writable** characteristic, and a corpus recipe whose `write_target_id` names it | needed before anything below can run | — |
| 5b.11 | `request <id> entities`, then `request <id> control <entity> turn_on` | the response names the state `pending` — **never** the requested state — and the write reaches the peripheral | serial log + peripheral log |
| 5b.12 | Let the peripheral report the new value on a subscribed characteristic | the state moves to `confirmed` **only then**; before the report it is still pending | serial log |
| 5b.13 | Send the opposite control and have the peripheral **refuse** (report the old value) | the control ends `failed` at its deadline and the previously confirmed state is restored — the requested value is never published | serial log |
| 5b.14 | Send a control with the peripheral powered off or out of range | the control fails; it does **not** sit pending, and no write is attempted | serial log |
| 5b.15 | Point a control at a device that is **not** the connected peer | refused, and the connected peripheral's characteristic is untouched | serial log |
| 5b.16 | A recipe naming a **read-only** characteristic | refused before any write; the peripheral sees no ATT write | serial log + peripheral log |

Item 5b.16 is the one worth doing first: a wrong handle does not fail, it writes
successfully to the wrong attribute, and only the peripheral's own log shows it.

## 5c. ESPHome Native API (B7)

Noise is implemented from the specification and verified against RFC vectors,
pinned cross-implementation fixtures and a loopback responder. **None of it has
met a real ESPHome node**, and one end-to-end test failure in the host suite is
still open (ledger §4d), so these items are the only way to settle it.

| | Check | Expected | Evidence |
|---|---|---|---|
| 5c.1 | A real ESPHome node on the LAN plus its API encryption key | needed before anything below can run | — |
| 5c.2 | Connect with the correct key | handshake completes; the probe reports the node's name, model and version | serial log |
| 5c.3 | Connect with a wrong key | `auth_required` reported; **no plaintext fallback is attempted** | serial log + node log |
| 5c.4 | Connect with no key to a node that requires one | refused, never sent in the clear | serial log + node log |
| 5c.5 | Entity discovery | the node's entities appear with names, units and device classes | serial log |
| 5c.6 | Send a control, then change the state on the node itself | the command is accepted; observed state moves only from the node's report | serial log |
| 5c.7 | Restart the node mid-session | the session reports the disconnect and reconnects; no duplicate entities appear | serial log |

## 6. Resources

| | Check | Expected | Evidence |
|---|---|---|---|
| 6.1 | `request 3 resources` immediately after boot | free heap, min free heap, largest free block | serial log |
| 6.2 | After 100 scan rounds | **min free heap must not trend downward.** A downward trend is a leak, and this is the measurement that finds it. | serial log over 100 rounds |
| 6.3 | Worker and console stack high-water marks | recorded; both must stay clear of their stacks | serial log |
| 6.4 | The LVGL budget is intact | `CONFIG_LV_MEM_SIZE_KILOBYTES=32` unchanged and the display still works. **No screen or LVGL code may have been removed to make room.** | photo of the display |
| 6.5 | The same after the portal has run and been closed | resources return to the pre-portal level | serial log |

### What is deliberately not measured yet

`docs/recognition-budget.md` lists the computed software budget and marks every unmeasured
figure. Items 1.3, 6.1–6.3 and 6.5 above are where those become measured. Until they
exist, **no RAM or stack figure for this project may be quoted.**

## 7. What to do with the results

For each item: the outcome, the commit, the serial log, and any measurement taken. Items
that cannot be run (no GATT peripheral, no ESPHome node, no Zigbee device) stay marked as
blocked with the reason — not as passed, and not as failed.

Sections 5b and 5c are additionally blocked on code that does not exist yet, not only on
hardware: 5b needs the firmware adapter that binds `app_ble_gatt` to `esphome_ble_gatt_*`,
and 5c needs the same layer for the Native API session. Record them as blocked on that,
so the distinction between "needs hardware" and "needs work" stays visible.

The hardware column of the stage table in `docs/handover-ledger.md` is updated from
these results and from nothing else.
