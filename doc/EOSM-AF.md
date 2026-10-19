# EOS M autofocus work: features, findings and firmware notes

Camera: Canon EOS M, firmware 2.0.2, EF-M 18-55 STM.
Code: branch `eosm-slim-gui` of this fork (`modules/crop_rec/crop_rec.c` unless noted).
Period: October 2026.

This document records what was built, what was tested on the camera, and what was learned
about how the EOS M drives its lens. The negative results are as important as the positive
ones: they explain why the obvious approaches do not work on this camera, so they do not need
to be tried again.

---

## 1. Summary

| Goal | Result |
|---|---|
| Reliable one-shot AF with a crop preset | **Works.** Shutter zoom "AF": zoom to x10, Canon AF, back to x5. |
| Continuous AF | **Works as "Auto refocus"**: ML watches sharpness and triggers Canon's AF when the picture goes soft. Not smooth tracking. |
| Canon Movie Servo AF under crop presets | **Not possible without patching firmware.** Canon disables servo in every magnified view by design, and presets need x5. |
| ML-driven lens movement (native contrast AF) | **Not achieved yet.** ML's lens command reaches the MPU but the MPU ignores it on this camera. The MPU protocol investigation (see [EOSM-MPU.md](EOSM-MPU.md)) is the active route. |
| ISO/shutter shortcuts while recording | **Fixed.** They were always blocked by a forced flag. |
| EOS M "LV guard" stability code | **Shelved.** It had never run before; on the camera it froze buttons and broke zoom/recording. |

---

## 2. Features built

### 2.1 Shutter zoom "AF" (one-shot AF assist)

Settings → Shutter zoom → **AF**.

- A half-press with an AF lens zooms to x10, runs one Canon AF, and returns to the x5 preset view. With a manual lens it acts like Shutter zoom ON.
- x10 is Canon's unpatched view, where Canon's AF is accurate. At x5 under a preset, Canon AF is much less reliable (see 3.4).
- Implementation: state machine `slim_af_*` (IDLE, SETTLE, WAIT_START, RUNNING, RELEASE, HOLD), run from the top of `crop_rec_polling_cbr`. It presses a remote SW1 (`slim_af_sw1`). It ends on `PROP_LV_AF_RESULT` (0x80050029, payload 0 = OK, 1 = failed), with a 1.5 s blind fallback.
- Not active while recording, because the zoom switch would wreck the recording.

### 2.2 Auto refocus (continuous AF)

Settings → **Auto refocus**: OFF / ON / ON + REC.

- ML samples a sharpness value (section 4) every 200 ms at x5. After each AF it stores the sharpness as the reference.
- When sharpness stays well below the reference for a short time, and the picture has stopped changing, ML triggers AF:
  - **ON, not recording:** the x10 one-shot AF above. There is a brief visible jump to x10 and back.
  - **ON + REC, recording:** AF in place at x5 (request value 2, `slim_af_inplace`), without zooming. It starts 2 s after recording begins. It needs Small Hacks OFF.
- Protections:
  - cooldown after every AF
  - back-off after a failed AF
  - one quick retry after a failed AF while recording
  - pause after 3 consecutive failures, resuming when sharpness recovers
  - no action with Movie Servo on, a menu open, a physical half-press, an LV transition, or a lens test running
- Field results before tuning:

  | | Not recording | Recording |
  |---|---|---|
  | AF success | 6/7 | 4/6 |
  | Blur to trigger (median) | 1.4 s | 1.9 s |

- User verdict: "ON works well", "REC AF pretty good, not perfect but ok".
- Known limits:
  - Recording AF is Canon's x5 AF, which favours far subjects.
  - A failed AF while recording usually parks the lens at the near end stop (about 25 cm). Canon does this.
  - The first version was not sensitive enough: clearly blurry scenes produced no attempts. Replaying the logs showed why. The reference was re-taken from an already-blurry picture (after a menu, zoom change, failed AF or recording start), and a 40000 absolute floor disabled triggering for blurry references. A drop below 60% was fine: real blur is 10–35% of the recent best.
- Reworked trigger (commit 480e704 plus a follow-up):
  - **Peak memory:** compares against the best sharpness of the last 12–24 s (two 12 s buckets), fed with the median of 3 samples so single-frame spikes can't poison it. It survives failed AF, menus, half-press and recording start. It is cleared on a zoom change, a setting toggle, a Shutter zoom AF, or a detected scene change.
  - **No absolute floor:** a texture check on the peak (`emax` ≥ 70) replaces it.
  - **AF "OK" but still blurry counts as a failure** (an "OK" at the end stop is common). If two OK AFs in a row land at the same sharpness (±25–33%), it is treated as a new, less detailed scene and the old peak is forgotten, instead of hunting.
  - **The failure pause ends after 20 s** with one more attempt.
  - **Absolute fallback** when no usable peak exists: q = 100 × Laplacian / gradient sum, one attempt per blur episode. Its thresholds are guesses until calibrated from logs.
  - **Field run 27 fixes:**
    - The peak stored the edge strength of a different sample than its value, so real peaks were rejected as "no texture" (gate 4 was the most common blocker). It now keeps the strongest edge seen.
    - The peak minimums were lowered: blur sits at 16–25k, noise at 10–14k.
    - The absolute test was inverted. **q goes up with blur**: blurry 100–132 with max edge 16–45, sharp 44–81 with max edge 240–380. Blur = q ≥ 105 (Medium) / 95 (High) and max edge ≤ 45 / 55.
    - It stops deciding after a scene-change reset.
    - A post-AF judgement older than 2 s is skipped.
  - **Refocus sensitivity** (Low / Medium / High; peak minimums now 30000 / 20000 / 16000):

    | | Low | Medium | High |
    |---|---|---|---|
    | Fires below % of peak | 45 | 60 | 70 |
    | Dwell, not recording (ms) | 1000 | 700 | 500 |
    | Dwell, recording (ms) | 600 | 350 | 250 |
    | Peak minimum | 45000 | 30000 | 24000 |
    | Absolute q ≤ | off | 15 | 25 |

  - Decision logging (AF diag log on): `@REFOCUS` kind 10, once per second and at each trigger, records the gate that blocked it (0 fired, 1 not low, 2 dwell, 3 cooldown, 4 no usable peak, 5 unsteady, 6 paused, 8 history). Kind 11 records why it is ineligible, as a bitmask (1 mode, 2 AF busy, 4 MF/servo, 8 menu/display, 16 transition/raw, 32 half-press, 64 zoom/REC rule).
- Code: block "EOS M slim: Auto refocus", `aref_step()`, `#define AREF_*`.
- Diagnostics: `@REFOCUS` events in the AF diag log. w0 kinds:

  | w0 | Meaning |
  |---|---|
  | 1 | trigger |
  | 2 | AF OK |
  | 3 | AF failed |
  | 4 | request not started |
  | 5 | paused |
  | 6 | resumed |
  | 7 | reference taken |
  | 8 | AF frame record |
  | 9 | AF OK but still blurry |
  | 10 | periodic decision record |
  | 11 | ineligible gates |
  | 12 | new scene, peak forgotten |

### 2.3 ISO/shutter/aperture shortcuts while recording

The Up/Down and Left/Right shortcuts were ignored during every recording. They were gated on crop_rec's own "More Hacks" flag, which is forced on at startup on the EOS M. They are now blocked only when mlv_lite's "More" small hacks has actually suspended Canon's AeWb task (`slim_expo_frozen()`). With Small Hacks OFF, ISO can be changed while recording.

### 2.4 Focus and Modules tiles in the slim menu grid

The slim GUI's grid offered only Exposure, Monitoring, Movie and Settings. ML's Focus menu was not even registered in slim builds. The grid is now 3x2 and adds **Focus** and **Modules** (`src/menu-grid.c`; `src/focus.c` registers the Focus menu).

### 2.5 Diagnostics (temporary, all OFF after restart)

All of these are in Settings.

- **AF diag log**: OFF / ON / ON + MPU. It writes `ML/LOGS/AFDIAG.LOG` (1 MB cap, then rotates to `AFDIAG.OLD`). The file is written only when not recording.
  - Line format: `<ms> <EVENT> n=<len> w=<w0>,<w1>,<w2>,<w3> z=<lv_dispsize> rr=<RECORDING_RAW> hs=<half-shutter> af=<slim_af_state> pos=<lens_info.focus_pos> dist=<focus distance cm, 65535 = infinity>`
  - Events:

    | Event | Content |
    |---|---|
    | `@SHARP` | sharpness samples |
    | `@AF_STATE` | AF state changes |
    | `@FOCUS_CHG` | focus changes |
    | `@REC` | recording start/stop |
    | `@LVLENS` | raw PROP_LV_LENS bytes |
    | `@LVOBJ` | Canon's LV AF state fields (3.3) |
    | `@REFOCUS` | Auto refocus decisions |
    | `@MPU` | messages to the MPU (ON + MPU only) |
    | `@MPUTEST`, `@SWEEP`, `@STEP_SHARP`, `@LENS_STEP` | lens tests |
    | property names | e.g. `LV_FOCUS_DONE`, `LV_AF_RESULT` |

  - **ON + MPU** hooks Canon's ICU→MPU packet builder at RAM 0x38C4. It is read-only and rate-limited per (group, op). The hook is installed once and never removed: `unpatch_memory()` wipes the stub immediately, and a task preempted inside it would crash.
- **AF lens test**: OFF / Remote / A/B / MPU / MPU+SW1. These are the lens-drive experiments of section 3. A choice starts only after it has been held for 1.5 s.

---

## 3. Findings

### 3.1 ML's lens drive does not move the EF-M lens on this camera

> **Superseded 2026-10-19, see §3.8.** The causes were the payload form (speed byte 0xFF) and the MPU
> monitor state, not "the MPU ignores (9,1)".

ML moves the focus through `lens_focus()` → `PROP_LV_LENS_DRIVE_REMOTE` (0x80050013).

Tested without any movement:
- movie and photo mode
- x1, x5 and x10
- step sizes 1, 2, 3 and 15
- `lens_focus()` timing, back-to-back commands, gaps of 0/40/100 ms, and 400-command sweeps
- AF and MF
- direct `PROP_LV_FOCUS` with ±50…±500 pulses, before and after Canon's AF-session message

Every command was acknowledged with `PROP_LV_FOCUS_DONE` (0x80050002, error bit 0x1000 clear) within 20–40 ms. The focus distance and the image sharpness did not change. `PROP_LV_FOCUS_PULSE_COUNT` (0x8005001E) never arrived.

Early "movements" in two logs (afdiag5/6) turned out to be unrelated. The same command produced focus changes 1–2 s later in opposite directions, and the image changed without any command. One "moved" result in the MPU test was the user touching the focus ring.

ML's own focus stacking refuses movie mode on every camera except the 5D2 (`src/focus.c`), and this build had already disabled Follow Focus for the EOS M. Earlier developers probably hit the same wall.

### 3.2 How the EOS M actually focuses: the MPU runs AF

The main processor (ICU, which runs ML) does not move the lens. A separate controller, the MPU, does. All ICU→MPU traffic goes through one packet builder. It was hooked and logged during Canon's own AF.

- **Canon's half-press AF** sends no move command at all:
  - (4,0x16) [01 00 00]: the remote/physical SW1 echo
  - (9,0x25,16) [02 00 …]: the AF-session message
  - (9,0x19,7) [01 3b 6b 26 60 1e 00]: an AF configuration block, identical every time

  The MPU then searches and drives the motor itself. At the end Canon sends (9,0x0B) [00 00] and (4,0x16) [00 00 00].
- Every frame the ICU only feeds the MPU image and AF data:
  - (9,0x17,32): AF frame geometry
  - (9,0x0A,14), (9,0x04,4), (9,0x1A,5), (9,0x00,1)
- ML's command becomes (9,1,5) with payload `[hi lo FF FF 00]`, a signed 16-bit relative pulse count, followed by (9,0x12,1). Canon itself never sends (9,1) during AF. The MPU acknowledges it and does nothing *in this form*: see §3.8.
- Requests through Canon's `_prop_request_change` for 0x80050003 (FOCUS_STOP), 0x8005003C (AF session), 0x80050027 (FOCUS_CMD) and 0x8005001D (LensSearchDriveStart) produced **no MPU message at all** in the earlier field test. Later ROM analysis (see [EOSM-MPU.md](EOSM-MPU.md)) showed why: **0x80050027 is not an outgoing property at all** (it is MPU→ICU only, `(9,0x18)`), while 0x8005003C and 0x8005001D have known handlers and lengths (15 and 4 bytes). The earlier failures are therefore not evidence that the MPU rejects relative moves inside a session; that hypothesis is still **untested**.

The MPU turned out to have more layers than the group 9 property channel. The full ICU→MPU / MPU→ICU maps, the monitor channel (`MonRead`/`MonWrite`), the `(9,0x13)` LensSearchDriveStart command, the missing focus-position feedback `(9,0x14)`, and the `Msg 08 02` factory command table are documented in [EOSM-MPU.md](EOSM-MPU.md). A temporary `mpu_lab` module was built to dump the MPU and send raw messages; see section 9 of that document.

### 3.3 Why Movie Servo AF never runs under crop presets

Canon gates servo AF on an internal LV AF state object, `obj = *(0x4069C + 12)` (0x7388xx on the test camera). Proven by disassembly and confirmed on the camera (`@LVOBJ`):

| View | obj+0x184 (LV mode) | obj+0x710 (AF method/zoom tag) |
|---|---|---|
| x1 movie | 2 | 1 |
| x5 | 1 | 5 |
| x10 | 1 | 10 (0x81 = x5 with Single, from code) |

- Movie servo may start only when `+0xC7C == 1 && +0x184 == 2 && +0x710 == 1` (predicate at 0xFF18597C). Photo servo needs `+0xC54 == 1 && +0x184 == 0 && +0x710 == 1` (0xFF188360).
- The zoom routine (around 0xFF18EDB8) writes 5/0x81/10 into +0x710 and stops a running servo.
- Crop presets work only at x5, so servo can never start. crop_rec forcing FlexiZone Single is not the cause.
- The only route would be to write 1 into +0x710 at x5 or patch the predicate. That is untested and risky, because Canon uses that value to place the x5 AF area.

### 3.4 Canon AF at x5 under a preset favours far subjects

Manual half-presses during recording (x5) mostly locked only on far subjects (85–98 cm in the logs) and often failed. Failures ended with the lens parked at the near stop. At x10 the same AF mostly succeeded.

The likely cause is that Canon's x5 AF area is computed in Canon's own x5 geometry, while the preset shows a different sensor window. It is unproven. ML's AF-frame helper (`move_lv_afframe`) refuses this mode. `PROP_LV_AFFRAME` is now logged (`@REFOCUS` w0 = 8) to check frame position against failures.

### 3.5 Canon half-press AF during RAW recording

In-place AF at x5 while recording failed three times out of three in field run 27, with Canon reporting failure each time. The trigger can't fix that; x10 AF is only possible when not recording.

Short 1x1 recordings at 2880×1206 are not an AF issue. At about 6 MB per frame and 24 fps, the data rate far exceeds what the EOS M can write. mlv_lite fills its buffer (about 17 frames) and stops after a few seconds. Lower the resolution or bit depth for continuous recording.



It works only with Small Hacks OFF. With "More" small hacks, Canon's AeWb task is suspended, which also blocks exposure changes.

### 3.6 The EOS M "LV guard" (branch `eosm-guard-test`, shelved)

`CONFIG_EOSM` was never defined for module builds, so about 30 EOS M-only blocks in crop_rec and mlv_lite had never been compiled. Branch `eosm-guard` defines it (export MODEL; `-DCONFIG_$(MODEL)` in module Makefiles) and fixes the compile errors.

On the camera:
- **First run:** the guard stayed "busy" forever and swallowed SET, the arrows, INFO and touch.
- **Second run** (with a 3 s timeout and `@GUARD` logging): it timed out every time while still in its first state. Its readiness check needs the LV display buffer address to stay the same for 3 frames. Canon alternates display buffers every frame, so it never passes. Its forced x1→x5 rebuilds left Canon's path in x10 while the screen showed x5, so the camera got stuck in x5/x10. Recording was also unstable, consistent with the mlv_lite EOS M changes it enabled.

Decision: do not merge. Fixing it would need a redesign. The branch is kept for reference.

### 3.7 Misc

- `lens_info.focus_pos` is meaningless on the EOS M. `focus_dist` is coarse (cm, 65535 = infinity, and it jitters). `lv_focus_status` never reports 3.
- crop_rec forces the AF method to FlexiZone Single when a preset is active. Movie Servo + FlexiZone Single refocuses continuously in photo mode, but not in movie mode with a preset (see 3.3).
- `CRASH00/01.LOG` of 18 Oct 18:06–18:07 ("NULL PTR" in shoot_task) came from a mismatched `EOSM_202.sym` (see 6), not from the AF code.
- `ASSERT00.LOG` of 7 Oct (mlv_lite `reset_buffer_slots`) predates this work. It was attributed to too-high quality settings.

---

### 3.8 ML-driven lens moves via (9,1) (2026-10-19, field-confirmed)

**Actuator.** ICU→MPU `(9,1,5) [hi lo SPD 00 00]` through the packet builder at 0x38C4:

- SPD == 0xFF (ML's `lens_focus` form): the word is a selector; only ±1..±3 give
  lens-dependent pulses, anything else becomes 0 pulses. This is why every ±50..±500 test
  never moved.
- SPD == 0x07: a raw signed relative pulse count. It moves the EF-M 18-55 both ways at
  ±20..±300. SPD 0x01 and 0x00 did not move.
- `lens_info.focus_dist` does not update for these moves. The MPU's cached lens position
  (0xFFF8132E, lens opcode 0xC0) does not update in this mode either, so there is no
  position feedback.

**MPU monitor state is required.** `MpuMonOpen` (0xFF1DD098) = `send(8,3,1,{00})` plus a
wait for
the reply. On the MPU, OPEN (0xB5FF6, dispatcher 0xA2238) runs a lens-engine re-init
(0xFFFD8280 handshake). Without it, every move is rejected in 3-46 ms with FOCUS_DONE
`0x004a0000`. With it, moves complete in 75-550 ms (`0x004e0000`/`0x004e0400`). The open
blacks out LV (luma→1) from ~300 ms to ~2.5 s. Keeping the monitor open
(crop_rec "AF drive monitor: Keep open") pays that once; later runs need no re-open.

**Motion behaviour.**

- FOCUS_DONE marks completion: the lens busy bit (byte 0xFFF81BB9 bit2, copied from the
  lens status poll) clears at the same time.
- The MPU sends a 1-pulse opposite pre-step before every move >1 pulse and applies its own
  reversal handling. The effective offset after a direction reversal is not a stable
  backlash: it was seen with both signs.
- Same-size moves in the same direction are repeatable (calib passes peak at the same
  commanded position). Mixing sizes (e.g. -260 then +120 vs 20-pulse steps) breaks
  position consistency. AF must land by sharpness feedback, not by computed positions.
- At x5 the Laplacian sharpness peak is only ~40-60 pulses wide; the gradient sum rises over
  ~±120 pulses (use it for the coarse search).

**Working algorithm (commit 8917e0b, field run 2026-10-19: 4/4 landed at 93-108% of the
best sharpness, first retrace each time, 8.5-10.6 s per run).**

1. Coarse climb on g in 60-pulse steps after a direction probe.
2. Fine pass A on s: back move, then 20-pulse steps until s drops below 70% of the max twice.
3. Retrace: back move, then the same 20-pulse steps, stopping at the first s >= 90% of pass A's
   max (80% on a second retrace).

Next: speed (each move ~0.5 s incl. settle and stable sampling), wider distance coverage,
Auto refocus integration, and behaviour with the monitor open while recording.

**Other.** The LV blackout seen in the "AF search (MPU)" runs came from MpuMonOpen, not from
the (8,2,32) index-16 call. That call still always ends at infinity and is not used.

Tools: crop_rec Settings → AF drive (MPU) / AF drive monitor / AF drive calib / AF drive
probe / AF drive: close monitor; mpu_lab → Drive test (9,1). Logs: ML/LOGS/AFDRIVE.LOG,
AFCALIB.LOG, AFPROBE.LOG, MPUDRIVE.LOG.

## 4. The sharpness metric

`afdiag_sharpness()` computes a Laplacian edge sum on luma over the centre quarter of the LV display buffer (`get_yuv422_vram()`).

- It tracks focus well: in focus about 90–180k at x5 on a detailed scene, defocused about 15–20k.
- It works during recording.
- It depends on the scene and the zoom level (x5 and x10 values differ), so Auto refocus compares samples only at the same zoom.
- It is flat when focus is far off, so it is a poor ruler for hill-climbing from deep blur.

---

## 5. Firmware addresses (EOS M 2.0.2, ROM1 at 0xFF000000)

RAM copies: ROM address − 0xFFA69590 (`RAM_OFFSET` in `platform/EOSM.202/stubs.S`).

| Address | What |
|---|---|
| 0xFF17C8FC | AF property switch (big branch tree on property ID, no state checks) |
| 0xFF17EC94 | `PROP_LV_LENS_DRIVE_REMOTE` decoder: low nibble = signed step count, bit 0x8000 = sign |
| 0xFF184494 | sends `PROP_LV_FOCUS` {1, hi, lo, FF, FF} |
| 0xFF11FB04 | Canon `_prop_request_change` (copies data, posts to the property task, no length check) |
| 0xFF902EC8 | ICU→MPU property-ID table (80 entries; 0x80050001 at index 30) |
| 0xFF902BD8 | MPU→ICU property-ID table (FOCUS_DONE, PULSE_COUNT, LV_AF_RESULT, LV_LENS, …) |
| 0xFF324950 | property → MPU message callback |
| 0xFF32456C, 0xFF324100 | further [property][handler] tables (e.g. REMOTE_SW1 → 0xFF3229E4 → (4,0x16)) |
| 0xFF321C34 | handler building a 2-byte message `[data[0], 00]` (FOCUS_CMD → (9,0x0B)) |
| 0xFFA6CE54 / RAM 0x38C4 | ICU→MPU packet builder `(grp, op, len, data)` → `[len+3, grp, op, payload]`. It refuses len > 100 and does nothing if `[0x3E4E8+0x24] == 1`. First instruction `push {r4, r5, lr}` (0xE92D4030). |
| 0xFFA6CED8 / RAM 0x3948 | second send variant (group 7). Not covered by the hook. |
| 0xFFA872BC | raw ring-buffer sender (never call) |
| 0x4069C + 12 | pointer to the LV AF state object (literal at 0xFF1835F4) |
| 0xFF18597C / 0xFF188360 | movie / photo servo-allowed predicates |
| 0xFF18ED20–0xFF18EE80 | magnify toggle: writes +0x710 = 5 / 0x81 / 10 |
| 0xFF180440 | AF state machine event handler (event 22: "AF_STOP CONTI START/STOP/PAUSE") |
| 0xFF192130 / 0xFF1921C0 | gmtStartServoAF / gmtStopServoAF |

Property → MPU message map (emulated):

| Property | Message |
|---|---|
| 0x80050001 LV_FOCUS | (9,1,5) |
| 0x80050003 FOCUS_STOP | (9,3,1) |
| 0x80050004 | (9,4,4) |
| 0x80050009 FOCUS_STATE | (9,0x0B,2) |
| 0x80050015 DISPSIZE | (9,0x11,2) |
| 0x8005001D LensSearchDriveStart | (9,0x13,4); sent only if data[0] ≠ 0, payload [d1 d2 d3 00]; nothing in the firmware sends it |
| 0x80050020 | (9,0x15,14) (AEWB exposure parameters) |
| 0x80050026 | (9,0x17,33) (32 data bytes + 00) |
| 0x8005002A | (9,0x1A,5) |
| 0x80050034 | (9,0x1F,3) |
| 0x80050038 | (9,0x24,5) |
| 0x80050039 | (9,0x21,61) (60-byte AF configuration block + 00) |
| 0x8005003C | (9,0x25,16) (15 data bytes + 00) |
| 0x80050043 | (9,0x2C,2) |
| 0x80050047 | (9,0x2F,2) |
| 0x80020015 REMOTE_SW1 | (4,0x16,3) |

Note: `0x80050027` and `0x80050028` are **not** in this outgoing table; they are MPU→ICU properties only (`(9,0x18)` and `(9,0x19)`). The full, corrected tables and payloads are in [EOSM-MPU.md](EOSM-MPU.md).

MPU → ICU (corrected; full table in [EOSM-MPU.md](EOSM-MPU.md)):

| Message | Property |
|---|---|
| (9,0x0B) | 0x80050029 LV_AF_RESULT (also used as FOCUS_STATE in the other direction) |
| (9,0x18) | 0x80050027 FOCUS_CMD |
| (9,0x19) | 0x80050028 |
| (9,0x14) | 0x8005001E focus pulse count / lens focus position |
| (9,0x00) | 0x80050000 LV_LENS |
| (9,0x02) | 0x80050002 LV_FOCUS_DONE |
| (14,2) | 0x80050045 LV_FOCAL_DISTANCE |

The ROM dump, string lists and RE scripts are in the session scratchpad. They are not in the repo; the ROM is the user's camera firmware and stays local.

---

## 6. Build and install

- **Toolchain:** ARM GCC 5.4 2016q3 at `~/gcc-arm-none-eabi-5_4-2016q3` (it runs under Rosetta on Apple Silicon and is auto-detected by `Makefile.setup.toolchain`).
- **Build:** `make -C platform/EOSM.202 zip SHELL=/bin/bash PYTHON=python3`
  - Every `modules/*/` needs a `module_strings.h`. Create empty placeholders if missing: `for d in modules/*/; do [ -f "$d/module_strings.h" ] || printf '' > "$d/module_strings.h"; done`. A clean or failed build deletes them.
  - `make clean` deletes all zips.
  - `modules/checkdep.py` now runs under Python 2 or 3.
- **Install** (ML already on the card): copy **all three together** from the zip:
  1. `autoexec.bin` to the card root
  2. `ML/modules/EOSM_202.sym`
  3. `ML/modules/*.mo`

  The `.sym` file must come from the same build as `autoexec.bin`. Otherwise modules link against wrong addresses, fail to load or crash at boot, and ML then refuses to load modules. That is what caused CRASH00/01. A first install still needs Canon's firmware-update step.
- **ML gotchas met along the way:**
  - ML's snprintf does not support `%-20s` or `%u`.
  - Module property handlers fire only if listed with `MODULE_PROPHANDLER`.
  - ML's `prop_request_change` rejects properties whose Canon-table length is 0 (PROP_LV_FOCUS), hence Canon's `_prop_request_change`.

---

## 7. Field test log index

The logs were copied from `ML/LOGS` to the session scratchpad.

| Log | Test | Outcome |
|---|---|---|
| afdiag1–4 | one-shot AF, AF during recording, Small Hacks | one-shot AF at x10 works; half-press AF during REC needs Small Hacks OFF and hunts |
| afdiag5–6 | first lens_focus bursts | apparent movement, later shown unrelated |
| afdiag7–12 | remote sizes/gaps, direct PROP_LV_FOCUS | acknowledged, no movement; ML rejected PROP_LV_FOCUS (ASSERTs) until sent via Canon's function |
| afdiag13 | full-range sweeps | nothing moved |
| afdiag14 | A/B lens_focus vs size 15 | nothing moved |
| afdiag15 | photo vs movie, x1 vs x5 | nothing moved in any mode |
| afdiag16 | ML built-in Focus End Point | no lens commands logged |
| afdiag17 | step sizes 1/2/3 | nothing moved |
| afdiag19 | Auto refocus ON | 7 triggers, 6 OK |
| afdiag20 | Auto refocus ON + REC; MPU capture | REC never fired (bug, fixed); MPU traffic decoded |
| guardrun, guardrun2 | LV guard builds | stuck busy; timeouts in state 0 |
| afdiag25 | MPU experiment series, AF and MF | big moves acknowledged, no movement; session/stop/search messages never sent |

---

## 8. Branches and commits

- `eosm-slim-gui`: all features and diagnostics above. Key commits:

  | Commit | Change |
  |---|---|
  | 5a6274f | one-shot AF assist |
  | 4884e0c | end AF on Canon's result |
  | 0bb16fa | Auto refocus |
  | 616723a | ON + REC fix |
  | b7a47a3 / 229259e | tuning, AF-frame logging |
  | b42e29c | ISO while recording |
  | 4e0a14a | Focus/Modules tiles |
  | 3797c2f | MPU log |
  | c8d8fdc / c6df5c9 | MPU experiments and safety fixes |

- `eosm-guard`: enables `CONFIG_EOSM` for modules (commit 11c3eb8).
- `eosm-guard-test`: guard plus a 3 s timeout and diagnostics (def578f). Shelved.

---

## 9. Open items

1. **Auto refocus:** field-test the reworked trigger (section 2.2). Check for hunting at Medium and High on pans and scene changes. Calibrate the absolute q thresholds from kind 10 records.
2. **x5 AF area vs preset framing:** use the new `PROP_LV_AFFRAME` logs to see whether failures correlate with an off-centre frame.
3. **MPU route (active):** [EOSM-MPU.md](EOSM-MPU.md) documents the MPU protocol, the corrected property maps, the `(9,0x13)` LensSearchDriveStart command, the monitor channel, and the `Msg 08 02` debug table. Next: dump the MPU ROM with `mpu_lab`, disassemble it as SH2A, then recover the exact search-drive / focus-drive / AF-session payloads. Only after that try `(8,2)` LensCMD/FocusScan or EEPROM writes.
4. **Release build:** hide or remove the diagnostic menu entries (AF diag log, AF lens test, MPU lab) and the MPU hook.
