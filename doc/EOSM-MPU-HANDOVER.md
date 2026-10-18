# EOS M MPU autofocus — handover / continuation guide

**Date:** 2026-10-19
**Branch:** `eosm-slim-gui` (tracks `fork/eosm-slim-gui`)
**Camera:** Canon EOS M, firmware 2.0.2
**Goal:** drive the lens from ML for autofocus under a crop preset, without
Canon's x5 AF area restriction.

This document is for the next model/engineer continuing the work. It assumes
the MPU investigation is complete; it is now a **field-testing and integration**
task.

---

## 1. Executive status

The hard part is solved: **ML can start an MPU AF lens search with an ordinary
ICU→MPU message**, no UART and no serial monitor.

Confirmed on camera:

- `(8, 2, 32)` with payload `00 00 00 00 00 00 00 10` + 24 zero bytes,
- MPU calls `0x1417C[16] = 0x33794` (`AF_LENS_SEARCH`),
- the lens physically moves,
- the search is a **full near→infinity sweep**, not a focus-and-stop,
- `(9,3,1)` stops it; `FOCUS_DONE` arrives after a stop,
- starting the search with the lens already at infinity can **hang the camera**.

Two new actions exist:

1. `modules/mpu_lab` (Focus → MPU lab) — raw probes, safe guard, 2 s bounded
   search action, exposure logging.
2. `modules/crop_rec` (Settings → AF search (MPU)) — first sharpness-peak-stop
   autofocus attempt; **new and not yet camera-tested**.

The immediate next task is to run the `crop_rec` action, inspect
`ML/LOGS/AFSEARCH.LOG` and tune the peak-stop thresholds.

**Update 2026-10-19 00:45:** the action was run once on the camera (LASTSEL.CFG
= `Settings / AF search (MPU)`), but its result was lost: `afdiag_record()` is a
no-op while *AF diag log* is OFF (its default after restart), and that was the
only record. The action now always writes a per-sample trace to
`ML/LOGS/AFSEARCH.LOG` and arms the peak-stop only on the far leg (see §3).
`crop_rec.mo` = `d8ede812db9b4bf9213fe3a8e2e2d03c` flashed; previous card
modules backed up to `/tmp/mpu_lab_backup_20261019_004539/`.

---

## 2. Key facts and addresses

### MPU

- Renesas F74964A, SH2A big-endian; ROM dump 786,432 bytes (`0xC0000`).
- Valid dump: `card30/MPU-ROM.BIN` in the session scratchpad (not in repo).
- Monitor command table is **name-first at `0x147B0`** (name +0, handler +0x24,
  stride 0x28, 49 records); the old `[handler][name]` at `0x14824` is wrong.
- `AF_LENS_SEARCH = 0x33794`; `TVAFLOG = 0x6F81A`; `EXEC = 0x6F526`;
  `CHECKEXTFLASH = 0x74D3E`; `VLENS_SET = 0x6FCD2`;
  `VLENS_DRIVE = 0x6FDAC`; `EEPW = 0x6EB7C`; `EEPR = 0x6EAF4`;
  `UPBNY = 0x70574`.
- Monitor text parser is **serial-console-only**: task loop `0x718C4`
  (RTOS id 6), line editor `0x70A60`, tokenizer `0x71CAC`,
  dispatcher `0x71D2A`. No group/EXEC path into the parser.
- Serial monitor: channel 1, base `0xFFFE8800`, prompt `MON>>>`,
  banner `Debug Monitor (K269)` at `0x14725`.

### Normal-message AF route

- Group-8 receive dispatcher `0x54E82`:
  `[transfer_size][group][op][payload...]`.
  For group 8, op 0/1/2 it copies 32 payload bytes to RAM `0xFFF84CEB` and
  enqueues `[group,op]` via `0x556D0`.
- Consumer `0x503D2` → `0x559E0(r4=op)`. For **op 2**:
  - `index = be32(payload[4..7])`,
  - call `0x1417C[index](0xFFF80000)` via `0x55DA8`,
  - expected raw reply `08 08 02 <BE32 ret> 00`.
- **No bounds check**: only indices 0..127 are safe.
- `0x1417C[16] = 0x33794`.
- `0x33794` takes no input args; writes mailbox `0xFFF825DA = 17`, waits on
  OS flags, writes `0x10` to `*r4`. It does **not** call the old suspected
  “AF core” `0x1C1F8` (that is `0x74D3E`/CHECKEXTFLASH).
- Exact ICU bytes:

  ```
  send(8, 2, 32, payload)
  payload[0..3] = 00 00 00 00
  payload[4..7] = 00 00 00 10
  payload[8..31] = 00...
  wire = 24 08 02 00 00 00 00 00 00 00 10 + 24 zeros (35 bytes)
  ```

- The ICU packet builder is at RAM `0x38C4`:
  `int (*send)(int grp, int op, int len, const void *data)`.
- `MpuMonOpen` = `0xFF1DD098`, `MpuMonRead` = `0xFF1DD2B8`,
  `MpuMonClose` = `0xFF1DD118`.

### Camera behavior observed

- From 2.9 m the command drove the lens to ~50 cm, then to infinity.
- From 1.15 m it drove to infinity.
- 8 s capture: `dist=73->65535`, 27–29 `LV_LENS` updates, final lens bytes
  `3B A3 C0 00 40 10 40 00`, no `FOCUS_DONE` until a stop is sent.
- `FOCUS_DONE n=1` after `(9,3,1)` stop; value `[00 00 4E ...]` or
  `[00 10 4E ...]`.
- **Freeze:** starting the scan while the lens is already at infinity hung the
  camera once. Recover by pulling the battery.
- **Dark viewfinder:** observed during the long 8 s sweep only. LiveView
  shutter/aperture/ISO display did not change. The 2 s bounded action did not
  darken. It is likely optical/defocus during the near→infinity sweep (or an
  unlogged working-aperture change), not an exposure setting.
- The `EXP` logging in `mpu_lab` reads zero because its property handlers never
  receive initial values; do not treat those lines as exposure data.

---

## 3. What is implemented

### `modules/mpu_lab` (temporary probe)

Menu: **Focus → MPU lab**.

- Dump ROM/EEPROM/RAM/test via Canon's `MpuMon*`.
- Raw command list + sequences.
- Group-8 probes:
  - `E 8.2.32 AF_SEARCH idx16` — 6 s window;
  - **`AF search (MPU) 8s`** — long window, opens monitor, pre/post mailbox
    reads, **refuses if `focus_dist >= 60000`**, sends `(9,3,1)` after;
  - **`AF search (MPU) 2s + stop`** — safe bounded action;
  - no-op/control/index variants, 13-byte variants, `(8,1)` MonRead;
- Logs `CMD`, `EXP`, `MON`, `EXPECT`, `STOP`, `REPLY` to
  `ML/LOGS/MPULAB.LOG`.
- Module is non-core; requires `ML/SETTINGS/mpu_lab.en`.

### `modules/crop_rec` (new sharpness-stop action)

Menu: **Settings → AF search (MPU)** (after `AF lens test`).

Function: `afdiag_afsearch_task` near the AF diagnostics block.

Behavior:

- refuses if not EOSM, no LV, recording, MF, `slim_af_state != IDLE`, or
  `focus_dist >= 60000`;
- waits for the menu to close (2 s);
- `MpuMonOpen`, sends `(8,2,32)` index 16;
- samples centre sharpness every 50 ms for max **3 s**;
- **far-leg arming:** the sweep often goes near first, so sharpness falling on
  that leg is not "past the peak". The tracker arms (and resets the peak) only
  once `focus_dist` rises at least `max(3, dist_min/20)` above its minimum;
  fallback arm at 1500 ms if the distance never changes;
- stops with `(9,3,1)` when sharpness has been **<80% of peak for >120 ms**
  after arming;
- samples another 500 ms after the stop (overshoot: settled vs peak);
- `MpuMonClose`;
- **always** appends a trace to `ML/LOGS/AFSEARCH.LOG`: a header (lens, zoom,
  dist0, ret, thresholds), a `# result=` line (stop/timeout, armed_ms, peak,
  peak_ms, stop_ms) and one line per sample `ms s g luma dist phase`
  (`n` = near leg / not armed, `a` = armed, `p` = post-stop). `g` is the
  gradient sum (contrast normaliser); `luma` answers the dark-viewfinder
  question;
- logs `AFD_EV_MPUSRCH` (AFDIAG code **14**):
  `w0 = stopped`, `w1 = peak`, `w2 = elapsed ms`, `w3 = focus_dist`;
- NotifyBox: `AF search MPU: stop/timeout peak=... ms=... ret=...`.

**This action has not yet been tested on the camera as of this handover.**
The thresholds are first guesses. It is the main thing to test/tune next.

---

## 4. Build and flash

### Build

```
make -C platform/EOSM.202 zip SHELL=/bin/bash PYTHON=python3
```

Output: `platform/EOSM.202/magiclantern-crop_mood.2026Oct19.EOSM202.zip`.

Known build gotchas:

- `modules/readme2modulestrings.py` is Python 2 and `python2` is unavailable.
  Keep a valid/touched `modules/mpu_lab/module_strings.h` newer than
  `README.rst`; after `make clean` recreate the empty placeholders.
- Module `module_strings.h` files are generated, not tracked.
- Do not commit build artifacts (`*.o`, `*.mo`, `*.zip`, platform outputs).

### Flash

Card: `/Volumes/EOS_DIGITAL`. The card is often present but unmounted; use:

```
diskutil mount /dev/disk4s1
unzip -o <zip> -d /Volumes/EOS_DIGITAL
touch /Volumes/EOS_DIGITAL/ML/SETTINGS/mpu_lab.en
sync
diskutil unmount /Volumes/EOS_DIGITAL
```

Latest flashed module MD5s at handover:

- `crop_rec.mo` = `c842372e3c9e3757a8f237f7d21e2368`
- `mpu_lab.mo` = `d11dd7dd738ef09e6c67a1df9ad775bb`

Current card is unmounted.

Backups:

- `/tmp/mpu_lab_backup_20261018_203849/`
- `/tmp/mpu_lab_backup_20261018_235456/`

---

## 5. Test protocol / next steps

### Immediate: test the new `crop_rec` action

1. Boot in **AF mode**, LiveView, not recording, lens not at infinity.
2. **Settings → AF search (MPU)**.
3. Close the menu when prompted; let it run (max 3 s).
4. Observe: did it stop near the subject instead of infinity?
5. Return the card; inspect `ML/LOGS/AFSEARCH.LOG` (always written).
   The NotifyBox shows `peak`, `end%` (settled/peak sharpness), `arm`, `stop`
   ms and `d=dist0->dist`. AFDIAG code **14** is only logged if AF diag log
   is ON.
6. Compare with `ML/LOGS/MPULAB.LOG` / lens position.

If peak stop is too early or too late, tune in `afdiag_afsearch_task`:

- peak drop percentage (80),
- drop dwell (120 ms),
- max run (3000 ms),
- sample period (50 ms).

### If it works

- Repeat at different distances, lenses and scenes.
- Add hysteresis or a second pass: if the stop is before the peak, reverse
  search; if after, search the other way. Canon's MPU command has no known
  normal-message direction control yet.
- Integrate into Auto refocus: replace the half-press path with this bounded
  MPU search once reliable.
- Check whether the `2 s`/`3 s` scan can be shortened after peak detection.

### Direction / TVAFLOG (open)

- `TVAFLOG <0|1> <1|2|3> <0|1>` still sets direction/drive level, but it is
  serial-monitor-only. Use TXDMPU (3.3 V, 115200 8N1) + `EXEC 10` to test it.
- UART guide: `doc/EOSM-MPU-UART.md`.
- If a normal message can set the same state, direction control will remove the
  need for a full sweep.

### Do not do

- Do not start `AF search` at infinity (freeze; guard exists in both modules).
- Do not write EEPROM / `(8,0)` MpuMonWrite / MEMW yet.
- Do not run AF search while recording.
- Do not increase the scan past 3 s until peak stop is reliable.

---

## 6. Files to read, in order

1. `doc/EOSM-MPU.md` — main investigation narrative, protocol, all camera runs
   (§5.6–§5.13 are the test history).
2. `doc/EOSM-MPU-ROM.md` — ROM disassembly, monitor tables, group-8 route,
   exact handler behavior.
3. `doc/EOSM-MPU-UART.md` — TXDMPU/XSR6 service port, `MON>>>`, `EXEC 10`,
   TVAFLOG/VLENS scripts, read-only command set.
4. `modules/mpu_lab/mpu_lab.c` — raw probe implementation and logging.
5. `modules/crop_rec/crop_rec.c` — `afdiag_afsearch_task` and menu entry.
6. `doc/EOSM-AF.md` — earlier Auto refocus/lens-drive background.

---

## 7. Open questions

- Does the sharpness peak really coincide with the best focus at x5 / under
  crop? The first action will tell.
- Is the dark viewfinder purely defocus, or is the lens working aperture
  changing? UI settings did not change; a working-aperture read would settle it.
- Can `TVAFLOG` direction/level be replicated from a normal message?
- Is the `0x33794` mailbox `17` consumed by a task that could do a true
  contrast AF internally? A task-level RE of the 17 consumer was not finished.
- Does repeated `(8,2,32)` during a scan stop/restart it, or does it need
  `(9,3,1)`? Early data suggests explicit stop is required.

---

## 8. Handover checklist

- [x] MPU ROM dumped and validated.
- [x] Monitor parser located; serial-only.
- [x] Normal-message AF route confirmed on camera.
- [x] Lens movement verified end-to-end.
- [x] Infinity freeze characterized; guard added.
- [x] `(9,3,1)` stop and `FOCUS_DONE` verified.
- [x] 2 s bounded action works; no dark viewfinder.
- [x] `crop_rec` sharpness-stop action implemented and flashed.
- [x] Always-on AFSEARCH.LOG trace + far-leg arming (first run's data was lost).
- [ ] `crop_rec` action tested on camera with a trace.
- [ ] Peak thresholds tuned.
- [ ] Direction control (TVAFLOG / normal message).
- [ ] Auto refocus integration.
