# AF patch 14: automatic lens profile + profile-driven Refocus test

**Date:** 2026-10-20 · **Base:** patch 13 (uncommitted, on the card as `24d6ad6f…`) · **Module:** `modules/crop_rec/crop_rec.c`
**User decisions:** profile on **lens swap and zoom change**, **on by default**; never while recording.

## 1. Why

- The fixed Refocus-test defocus list `afrt_defs = {60, −60, 150, −150, 400}` (pulses) fits the 18 mm (peak half-width
  ~25 pulses) but is ~5 half-widths on the 55 mm (~14) and only ~1 on the EF50 + speedbooster (~50): the same test
  is much harder on one lens than another, and 400 can run into an end stop.
- The fine step (`afd_fine_eff`) is guessed from `lens_info.focal_len`, which freezes while the MPU monitor is open;
  the patch-10 learned fine step was noise.
- "AF sweep record" (mode 4) only makes ±100 @07 / ±300 @1F blind moves and logs raw samples — no peak, no width, no
  feedback into AF state, nothing persisted.

## 2. The lens profile

**Key:** `(lens_info.name, afd_mm_bucket(), lv_dispsize)` — lens swap and zoom change both give a new key.
**Profile run** (new engine mode 10, `afw_run`-style, inside the normal session/monitor handling):
1. **Land:** a normal B re-catch AF run (mode-0 search code). nopeak / lowcontrast / abort → profile `fail why=…`.
2. **Width probe** from the landed position x0 (s = P, settled, frame-synced `afd_rc_rest`):
   measured static moves at 07: −10, −10, −20, −40, −80 (cumulative −160), then +160 back in measured chunks (80, 80),
   re-measure s at x0 (log `x0_return_pct`), then +10, +10, +20, +40, +80, then −160 back (80, 80). Every sample is a
   settled rest reading. ~16 moves, ~4–5 s.
3. **Derived:** half-width at 50 % of P on each side (linear interpolation between the bracketing offsets) `W50L`, `W50R`,
   `W50 = (W50L + W50R) / 2` (one-sided if the other side never fell below 50 %); `floor_pct` = mean s at ±160 / P;
   `x0_return_pct` (reversal repeatability); done statistics over all profile moves: the done word (e.g. `0x100` /
   `0x4e0000`), `honest_pct` (no 0x0400/0x1000), median done_ms per 10 pulses.
4. **Quality gate:** `ok` only if `P ≥ AFD_MIN_S·4`, both or one W50 found, `x0_return_pct ≥ 85`. Else `fail`.
5. Lens ends at x0 (in focus).

**Log:** `ML/LOGS/AFPROF.LOG` (raw moves + samples) and one summary line, also in AFDRIVE.LOG:
`# PROFILE lens="…" mm=… zoom=… ok=1 P=… W50L=… W50R=… W50=… floor_pct=… x0_return_pct=… fine=… done_word=… honest_pct=… done_ms_per10=… ms=…`

**Storage:** RAM table (16 entries, LRU) + persisted to `ML/SETTINGS/AFLENS.TXT`, one line per profile
(`v1|name|mm|zoom|W50|floor_pct|done_word|honest_pct|fine`), rewritten on change, loaded at module init (tolerant
parser: skip bad lines). Menu action "Forget lens profiles" deletes the file and the table.

## 3. Automatic trigger (menu `Auto lens profile`: ON default / OFF; CONFIG_INT persisted)

Checked from the existing AF worker loop (no new task while recording; `afd_worker_ensure` only when not recording).
Run when **all** hold for ≥ 3 s continuously: LV on, not RECORDING / RAW / mlv busy, no menu, `slim_af_state` idle,
not manual focus, `afdiag_afdrive_busy == 0`, no auto-refocus run pending, and the current key has no stored profile.
Notice on screen: "Profiling lens… (≈ 6 s)". Abort on recording/menu/half-shutter (`afd_must_abort`) → retry later.
Failure (no contrast etc.): retry after 30 s, at most 3 attempts per key per power-on, then stop trying for that key
(log `# PROFILE give up`). The zoom part relies on `afd_mm_bucket` (focal length may stay frozen while the monitor is
open: then no new key is seen and no profile runs — logged, acceptable; Zoom refresh still works manually).
Menu action **"Profile this lens now"** (forces a run, replaces the stored profile).

## 4. Uses of the profile

### 4a. Refocus test defocus (menu `Refocus defocus`: **Auto (profile)** default / Fixed 60-150-400)
With Auto: if the current key has no profile, the test first runs a profile (log `# RT_PROFILE run`); if that fails, it
falls back to Fixed (log why). Levels from W50 (pulses), each rounded to 5, min 20:
`near = 2.5·W50`, `mid = 6·W50`, `far = 15·W50` capped at 800; trials in order `+near, −near, +mid, −mid, +far`.
Log `# RT_DEFS source=profile W50=… defs=…`.
**End-stop clipping:** a defocus move that hits an end stop (afd_es_check) is kept as the trial's actual defocus
(log `# RT_DEF clipped want=… got=…`) and later trials in that direction are capped at `got − 20`.
The no-trigger retry (double the defocus, cap) stays.

### 4b. Fine step (define `AFD_P14_PROFILE_FINE`, default on)
With "Fine step = Auto" and a profile for the current key: `fine = clamp(round(0.7·W50), 6, 30)` (log
`# fine profile W50=… fine=…`), replacing the focal-length guess and the learned value. Manual Fine step still wins.

## 5. Safety
Same rules as every AF diag action: no task_create / monitor open-close while recording; abort checks after every move;
travel caps; bounded total profile time (deadline 20 s). The width probe never exceeds ±160 pulses from x0 and returns
there with measured moves.

## 6. Field script
1. Flash; Auto lens profile ON (default). Attach the 18-55 at 18 mm, LV idle 3 s → expect the notice and a `# PROFILE`
   line; zoom to 55 mm → a second profile (if the focal length updates); swap to the EF50 → a third.
2. Refocus test on each (Refocus defocus = Auto): check `# RT_DEFS` levels and results.
3. Power-cycle with the same lens → no new profile (loaded from AFLENS.TXT).
