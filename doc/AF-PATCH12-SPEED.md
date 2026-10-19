# AF patch 12: speed ("quick and smooth")

**Date:** 2026-10-20 · **Base:** patch 11 + 11.1 (settled rest measurement, frame-synced) · **Module:** `modules/crop_rec/crop_rec.c`
**Evidence:** `contrib/af_test/time_budget.py` over all 2026-10-19 sessions (118 AF runs, 1218 s), field timeline of the
patch-11 −60 trial (11.9 s for ~650 pulses of real motion ≈ 2 s physical minimum).

## 1. Where the time goes (median per run)

| group | session+LV | Local | B probe | sweep | landing | epilogue | wall |
|---|---|---|---|---|---|---|---|
| Refocus trial (62) | 0.71 | 1.79 | 0.71 | 2.03 | 2.80 | 1.76 | **10.8 s** |
| Auto refocus, fast epilogue (18) | 0.60 | 1.28 | 0 | 0 | 0.14 | 0.62 | 4.3 s |

Hard facts used below:
- FOCUS_DONE: moves ≤ 30 pulses at 07 come back on completion (1–3 % timer); 31–100 pulses hit the ~510 ms timer
  43–64 % of the time (reversals almost always). Completion-driven done ≈ 153 + 1.33·|p| ms.
- LV frame ≈ 44 ms; at 07 the lens moves ~15 pulses per frame.
- Sweep chunk (07, 100 p): median idle 173 ms beyond the motion model; 109 ms dead gap after done.
- Local ends in "→ B" in 70 % of Refocus trials, costing a median 1.5 s before B starts.
- Epilogue: 1.78 s normal, 0.62 s fast (fast only for auto refocus/hunt today).
- Patch-11 rest measurement after each 50-pulse re-catch chunk: median 314 ms (p90 709) — as much as the motion.

## 2. Changes (each behind its own define, all on by default)

### S1 `AFD_P12_FAST_EPI` — fast epilogue for every run
`fast_ep = (afd_tol_run || AFD_P12_FAST_EPI) && sc != 2`. Landing verify (300 ms) skipped when `land_pct >= 90` for every
run, not only tolerance runs. Restore mode (sc == 2) keeps the full path. Saves ~1.2–1.5 s per run.

### S2 `AFD_P12_LOCAL_SKIP` — no Local probes when the start is deep blur
In `afd_run_local`, after `afd_start`: if `ref && afd_below(s0, ref, AFD_L_SKIP_PCT)` (30) → log
`# local: s0=%d below %d%% of ref=%d -> B (no probes)` and go to B immediately. (The existing post-probe
"far" rule is 50 %; the probes are ±2·fine, which cannot reach a peak that s0 says is > ~1.5 σ away; B's own probe
and sweep cover it.) Saves ~1.5 s on most deep refocuses.

### S3 `AFD_P12_TRACKED_PROBE` — B's direction probe is the first sweep chunk (re-catch landing only)
Today: untracked `p +100` (0.7 s), then an unmeasured jump back to `pos0 − d·60` (0.65 s), then the sweep re-covers it.
New (only when `rc`): the probe is a tracked 07 chunk `afd_sweep("probe", +1, 100, 07, afd_sw_stop, &w)`.
- If the chunk itself shows a peak (`w.peak`) → found: re-catch landing from here (dir = +1, P = w.smax_ex, over = w.over).
- Direction from g over the chunk: `g_last` vs `g_first` (the chunk's last and first samples) with the existing
  `AFD_PROBE_PCT` rule; also s rising (> 5 %) means +1. g/s falling → −1. Neither → probe flat (existing coarse-1F path).
- Then `afd_sweep_nopre = 1; afd_sweep_find(afd_pos, d, …)` — no jump back. For d = −1 the sweep re-covers the probed
  100 pulses on its way past pos0 (needed anyway).
- End stop during the probe → d = −1 (existing behaviour).
Saves ~0.7–1.3 s per B run.

### S4 `AFD_P12_RC_SHORT` — re-catch chunks below the timer threshold, no tracking pad
- `AFD_RC_L1` 50 → **30** (no 510 ms timer; done ≈ 190 ms). Halving floor stays `2·AFD_FINE`, but never above L.
- For "rc" chunks `afd_track_min_ms = t_exp` (no `AFD_SW_TRACK_PAD_MS`): the settled rest measurement that follows
  replaces the pad.
- Settled rest measurement (11.1): 3 frames in a row within 5 %, frame-synced (`afd_wait_frame`), ≤ 12 frames.
Target: rc chunk + rest ≈ 350 ms instead of ~1000 ms.

### S5 `AFD_P12_PHASE_LOG` — explicit phase markers
`# phase name=<session|local|probe|coarse|sweep|land|top|epilogue> t=<run ms>` at each phase start so
`time_budget.py` no longer has to infer phases. No behaviour change.

### E1 field experiment: chunk pipelining (diag action, not used by AF)
AF diag action **"AF pipe test"** (menu next to the other AF diag actions; monitor must be open; refused while recording):
for X in {−80, 0, +80} ms and 4 repetitions each, alternate direction, send a 100-pulse 07 chunk, then send the next
chunk at `t_exp + X` (t_exp = 120 + 290 ms) *without* waiting for FOCUS_DONE; track s/g per frame for both chunks.
Log to `AFPIPE.LOG`: per pair `X, done1_ms, val1, send2_ms, done2_ms, val2 (0x1000 = refused), s/g samples`, and a final
`# PIPE X=.. refused=k/4` summary. Total travel bounded (each pair returns to the start). This tells us whether the
~170 ms idle + 510 ms timer per chunk can be removed (the largest remaining lever).

## 3. Expected effect (Refocus trial median 10.8 s)

S1 −1.2 · S2 −1.5 (70 % of trials) · S3 −0.7…1.3 · S4 −1.5 → **≈ 5–6 s**, auto refocus deep runs similar.
The next step after E1 (if pipelining works) is ~−1.5 s more on the sweep.

## 4. Not changed (on purpose)
Session open 300 ms sleep and per-run session open/close (unknown MPU behaviour; a rejected first move aborts a run);
trigger dwell (false-trigger risk); sweep speed (07 samples ~15 pulses/frame; the 55 mm peak is ~30 wide).

## 5. As built (after simulation + review), build md5 `ee07e3ab93d0cb618837c15c3058c7a0`

Includes patch 11.1 (settled, frame-synced rest measurement: `afd_wait_frame`, `afd_rc_rest`; land only on a settled
reading; a settled reading counts toward the pass max; `land_top_rc` re-measures the start point).

Changes vs §2, from the simulator (`recatch_gtop_l30_tpg2`) and the code review:
- **S3 peak guard:** a probe chunk is a peak only with an interior max (`0 < imax < n−1`) and a first sample ≤ 85 % of the
  max. A flat first probe chunk gets a second tracked chunk before going to coarse 1F. Literal S3 lost the EF50 at
  defocus 60 (49 % vs 94 %); with the guard 98–99 %.
- **Sweep found-guard:** `afd_sweep_find` accepts a peak only if its max is not the very first sample of the sweep direction
  (`imax > 0 || k > 0`; a later chunk's first sample continues the previous chunk).
- **S3 nopeak:** a failed search returns the lens to its position before the probe.
- **S1:** fast epilogue only for clean (not aborted/timed out) AF runs (mode < 4); the 300 ms landing verify stays for
  the Refocus test and menu AF (skipped only for Auto refocus/hunt at ≥ 90 %, as before).
- **E1:** raw FOCUS_DONE events logged per pair (up to 3) and classified by time + 0x1000; 90 s total deadline.

Simulator (600 trials/cell, realistic errors), strict success / median time, recatch_gtop → as built:
55 mm d60 82 %/5.6 s → 94 %/2.7 s · 55 mm d150 90 %/5.0 s → 85 %/4.2 s · 18 mm d150 98 %/5.0 s → 98 %/4.1 s ·
EF50 d60 94 %/5.2 s → 99 %/2.5 s · EF50 d400 95 %/6.7 s → 95 %/5.9 s. (Local skip S2 and the fast epilogue are not in
the simulator; they are extra field savings.)

## 6. Field script

1. Flash; Movie Servo OFF; monitor Keep open; AF landing **Re-catch**; Fine step Auto; AF diag log ON.
2. **55 mm Refocus test** (same scene as the 2026-10-19 runs). Log: `# phase name=…`, `# local: s0=… -> B (no probes)`,
   `# probe dir=… tracked why=…`, `# recatch … st=1 … -> LANDED`, `# epilogue fast`.
3. **18 mm Refocus test**, then **EF50** if time allows.
4. **AF pipe test** once (AF diag menu, after AF stop test): focus on a textured subject first; ~1 min; writes AFPIPE.LOG.
5. Recording: auto refocus on a subject moving in depth; note any visible hunting or wrong landings (time of day).
6. Card back in → `python3 contrib/af_test/field_session.py --copy` and `contrib/af_test/time_budget.py`.
