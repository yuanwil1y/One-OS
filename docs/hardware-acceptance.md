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

| | Check | Expected | Evidence |
|---|---|---|---|
| 5.1 | A real ESPHome or Zigbee device, with a writable recipe in the corpus | needed before anything below can run | — |
| 5.2 | Send a control | The entity goes **pending**, not straight to the requested state | serial log |
| 5.3 | The device confirms | The state changes then | serial log |
| 5.4 | Send a control the device refuses | The state returns to the previous confirmed value; the error is reported | serial log |
| 5.5 | Send a control, then pull the device's power before it answers | The control times out and the previous state is restored | serial log |
| 5.6 | Send two controls for one entity at once | The second is refused BUSY | serial log |

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
that cannot be run (no ESPHome node, no Zigbee device) stay marked as blocked with the
reason — not as passed, and not as failed.

The hardware column of the stage table in `docs/handover-ledger.md` is updated from
these results and from nothing else.
