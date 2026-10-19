# AF patch 11: closed-loop "re-catch" landing

**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` · **Module:** `modules/crop_rec/crop_rec.c`
**Evidence:** field session `~/af_logs/af-20261019-230401` (patch 10 build, md5 f587be48…), 55 mm Refocus test 2/5.

## 1. What the field logs show

User report: "it goes from out-of-focus to out-of-focus skipping focus in between, then moves far out
of focus, then ping-pongs fully out of focus and gives up".

Every 55 mm failure in the session has the same shape:

| AFDRIVE run (t=) | sweep max seen (`smax_ex`) | reference | result |
|---|---|---|---|
| 139612 (+150 trial) | **119,524** | 125,144 | timeout, 37 moves, land 0 |
| 189043 (−150 trial) | **129,520** (1F 107,790) | 125,144 | nopeak, 24 moves |
| 119500 | **128,079** | — | nopeak, land flat |
| 724201 | **120,143** | — | timeout, 49 moves |
| 1161863 | **98,239** | — | timeout, 36 moves |

**The sweep finds focus every time. The landing never gets back to it.**

1. **Open-loop return.** After the sweep passes the peak, B makes an unmeasured jump to
   `pk − dir·AFD_SW_MARGIN` and walks `AFD_FINE` steps toward the estimated peak. The estimate (time →
   pulses, `AFD_SW_LAT_MS`/`MSPP`) is off by 20–60 pulses even in successful runs (68154: est −449, focus
   at ~−400; 351123: est −91, focus beyond −123). At 55 mm the in-focus window is only ~30 pulses wide
   (68154 retrace: 30k → 69k → 120k in three 10-pulse steps).
2. **Premature flat abort.** `afd_retrace` declares a "flat floor" after any `AFD_FLAT_RT_N` = 4 steps,
   regardless of how far the walk still is from the estimate. Run 139612: start −247, estimate −327,
   abort at −297 after 5 steps, never reaching the estimate.
3. **Open-loop recovery.** After the flat abort, patch 10's full 12-chunk resweep, the "other way" jump
   (+680) and the 1F coarse `pk−300` jump (−537) move the lens hundreds of pulses on dead reckoning;
   each lands somewhere unexpected, runs into end stops (`0x1000` refusals) and the deadline. That is
   the ping-pong.
4. **Learned fine step** (patch 10/C) uses the full width of the sweep peak (span ≥ 70 % of max,
   motion-blurred) as the *step*: values 4–80 for the same lens and focal length in one session.

## 2. The fix: never jump after the peak has been seen

When a sweep has crossed a peak, the lens is just past it, in the sweep direction, by `over` pulses.
Instead of jumping to a computed position, **reverse from where the lens is and watch s continuously**,
in tracked chunks that shrink each time the peak is crossed again. Every decision is made on measured s,
not on the position model. Short reversals are reliable (±40/±80 return 95–103 %), and short tracked
chunks bound the overrun (`(9,3,1)` stop does not halt, so the stop callback is not used).

### Algorithm `afd_land_recatch(dir, P, over)`

Inputs: `dir` = direction of the sweep that found the peak, `P` = the sweep peak (`smax_ex`, ex scale),
`over` = `|afd_pos − pk|` (estimated distance past the peak). `thr = afd_rt_thr1` % of P (tolerance-aware,
90 by default). All s values compared in the ex scale (`afd_move_ex` returns ex; tracked samples `m.s1`
must be converted with `afd_to_ex`, or compare `w.smax_ex`).

```
d = -dir; L = AFD_RC_L1 (50); cap = over + AFD_SW_ERR (100) + L; pass = 1
seen_max = 0
loop while pass <= AFD_RC_PASSES (4):
    covered = 0; pass_max = 0
    loop:
        tracked sweep afd_sweep("rc", d, L, AFD_SPEED (07), NULL, &w)     # no stop callback
        covered += w.moved; pass_max = max(pass_max, w.smax_ex)
        s_rest = afd_move_ex("i", 0, 1, 0)                                 # lens at rest
        log "# recatch pass=.. d=.. L=.. covered=.. cap=.. smax_ex=.. s_rest=.. thr=.. -> <decision>"
        if s_rest >= thr:                         -> LANDED: afd_land_top(d); return s
        if pass_max >= P*AFD_RC_SEEN_PCT(50)% and s_rest < pass_max*85%:
                                                  -> PASSED: the top is behind
            d = -d; cap = (pulses since the sample of pass_max, from w.over if it was in this chunk,
                           else covered-to-max) + L; L = max(L/2, AFD_RC_LMIN (2*AFD_FINE)); pass++; break
        if covered >= cap:
            if pass_max < P*AFD_RC_LOST_PCT(30)%  -> LOST: break out of both loops
            if not extended yet: cap += cap/2; extended = 1; continue  # rising flank, not reached yet
            -> LOST
    if L == AFD_RC_LMIN and pass > 2: final fine walk (below), return
final fine walk: AFD_FINE steps in d (afd_move_ex("r", d*AFD_FINE, 2, 0)), max cap/AFD_FINE + 3 steps;
    s >= thr -> afd_land_top(d), LANDED; rise then fall below 85 % of the walk max -> one step back, LANDED
    with that s if >= 80 % of P else LOST.
LOST: afd_land_flat = 1, return 0 (the caller's fallback).
```

`afd_land_top(d)` = the existing half-step "land top" climb at the end of `afd_land_steps`, factored out
unchanged.

### Fallback after LOST (replaces patch 10's full resweep)

One **bounded** re-sweep from the current position: `afd_sweep_find(afd_pos, dir, 07,
AFD_SW_RESWEEP_CHUNKS = 4, …)` with **no pre-jump** (`AFD_SW_PRE` not applied for this call) and, if it
finds a peak, `afd_land_recatch` again. If that also fails: stop where the lens is (`nopeak`/0); the
hunt takes over. No run to the end stops, no "other way" jump of more than 4 chunks.

### 1F coarse scan

After `coarse1f: peak region`, do not jump to `pk − 300`. Run the 07 sweep **from the current position in
the reverse direction** (`d = -cdir`, no pre-jump, one direction, `maxch = ceil((over_1f + 300) / 100)`,
max 8). It recrosses the peak; then `afd_land_recatch` as usual.

### Classic path fixes (kept behind the menu)

- `afd_retrace` flat detection only fires once the walk has passed the estimated peak:
  `(i + 1) * AFD_FINE >= afd_flat_min_travel`, set by the caller to the start→estimate distance + 40
  (B landing: `AFD_SW_MARGIN + 40` = 120). 0 = old behaviour (other callers).
- Learned fine step: `step = span_pulses / 3`, clamped `[6, 30]`; ignore the measurement when the span
  covers fewer than 3 samples. Log `# fine learned … span_pulses=.. val=..`.

### Menu

`AF landing` = **Re-catch** (default) / Classic. Classic = patch 10 landing + the two classic fixes
above. Persisted like the other AF settings. Re-catch applies to B (`cmode == 0`) wherever
`afd_run_sweep` is used (AF drive, Refocus, hunt). C (`cmode == 1`) is unchanged.

## 3. Expected field signature

- `# recatch … -> LANDED` on most runs, typically pass 1 or 2, 3–6 chunks.
- No `# land flat -> full resweep`, no `o` moves larger than one chunk after `# sweep peak` with Re-catch.
- 55 mm Refocus test ≥ 4/5, failures without end-stop ping-pong.
- Timing: similar to patch 10 for easy runs, much shorter for the formerly failing ones (no 17–20 s timeouts).

## 4. Rollback

Menu `AF landing` → Classic (no reflash). Compile-time: `AFD_RC_ENABLE 0`.

## 5. As built (after review + simulation)

**Build:** `crop_rec.mo` md5 `cf08510a57023a048d56ed5cf8e797ec`, zip
`platform/EOSM.202/magiclantern-crop_mood.2026Oct19.EOSM202.zip`. Not camera-verified.

Changes against §2, from the code review and the simulator (`contrib/af_replay/af_recatch.py`):

- **Gated two-sided top (`afd_land_top_rc`).** The plain `afd_land_top` after a re-catch LANDED cost strict
  success on the narrow 55 mm peak (a chunk often ends just past the top; the one-sided climb steps further
  away). Now: s at rest ≥ 95 % of the sweep peak → stay; else a half step in d, and if that goes down, try
  the other side and climb whichever rises. Classic keeps `afd_land_top` unchanged.
- `pass_max` ignores a chunk whose max is its start sample (no false PASSED on pass 1).
- The final fine walk goes back to its best step (also when that is step 0); one downhill-first reversal.
- `afd_run_dir` = the re-catch approach side, so the epilogue re-land approaches from the landing side.
- No fine-step learning from chunks shorter than 80 pulses; run header logs `landing=recatch|classic`.

**Simulator, strict success (final s ≥ 90 % of the true peak), defocus 150, 600 trials per cell:**

| lens | errors | classic | re-catch (spec) | re-catch gated (built) | median moves classic → gated |
|---|---|---|---|---|---|
| 55 mm | realistic | 84 | 74 | **88** | 16 → 8 |
| 55 mm | bad | 46 | 41 | **70** | 21 → 9 |
| 55 mm + tails | realistic | 91 | 81 | **92** | 16 → 8 |
| 55 mm + tails | bad | 70 | 50 | **74** | 17 → 9 |
| 18 mm | realistic | 85 | 88 | **99** | 12 → 9 |
| 18 mm | bad | 62 | 61 | **85** | 14 → 9 |
| EF50 wide | realistic | 96 | 94 | **98** | 15 → 7 |
| EF50 wide | bad | 88 | 88 | **92** | 17 → 8 |

Defocus 400 is not helped (the shared 1F coarse scan loses narrow peaks in the model). The 189043 pattern
(1F estimate off by ~800 pulses) is not reproduced by the model, so the 1F reverse sweep is field-only evidence.

## 6. Field checklist for this build

1. Flash, Movie Servo OFF, monitor Keep open, **AF landing = Re-catch**, Fine step Auto, AF diag log ON.
2. Refocus test at 55 mm (good light), then 18 mm, then the EF50. Expect ≥ 4/5 and no 15–20 s timeouts.
3. Repeat the 55 mm Refocus test with **AF landing = Classic** for an A/B on the same scene.
4. Deep-defocus walk at 55 mm (auto refocus): watch for ping-pong.
5. Logs to check: `landing=recatch`, `# recatch … -> LANDED` (which pass), `# recatch top:`,
   `# recatch LOST`, `# coarse1f: re-catch reverse`; no `# land flat -> full resweep` with Re-catch.
