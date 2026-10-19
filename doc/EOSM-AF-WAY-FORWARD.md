# EOS M autofocus — team review and way forward

**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` (pushed to `fork/eosm-slim-gui`, commit `b5d6262`) · **Base HEAD:** `043d914` (AF code unchanged since `47d8589`)
**Scope:** five parallel workstreams — current-state audit, testing, algorithms, out-of-the-box ideas, and a firmware patch for the top open problem.
**Status:** everything below is **offline / compile-checked only**. No camera was attached, so no patch in this tree has been field-validated. The firmware patch set (patches 1-5 + the AFDIAG feed logging) is frozen: full build exit 0 twice, `crop_rec.mo` md5 `ebb45f8b53ba4cd7b53036ca5a8a4e8f` deterministic, host suite 81/81 tests and 6/6 real corpus cases PASS, and the final adversarial review concludes **safe to flash, no blocking findings**.

Companion documents produced by this review:

| Document | What it is |
|---|---|
| [AF-STATUS-REVIEW.md](AF-STATUS-REVIEW.md) | HEAD-verified status matrix, code map, 21 doc/code discrepancies, 17-item risk register, top-5 actions |
| [AF-PATCH-FIELD-PLAN.md](AF-PATCH-FIELD-PLAN.md) | Patch 1: behaviour, expected log signatures, field-test sequence, rollback |
| [AF-PATCH2-FIELD-PLAN.md](AF-PATCH2-FIELD-PLAN.md) | Patches 2–3: integrity/timing/reference logging, stop_check, safety guards, review must-fixes |
| [AF-PATCH-REVIEW.md](AF-PATCH-REVIEW.md) | Independent adversarial review of patch 1 ("flash after fixes"; must-fixes now closed) |
| [AF-PATCH-REVIEW-2.md](AF-PATCH-REVIEW-2.md) | Independent adversarial review of the patch 2/3 delta (four state must-fixes, closed by patch 5) |
| [AF-PATCH-REVIEW-3.md](AF-PATCH-REVIEW-3.md) | Final verification of patch 5: **safe to flash, no blocking findings** |
| [AF-HUNT-FOREVER.md](AF-HUNT-FOREVER.md) | Design of the persistent never-give-up Auto-refocus hunt (patch 9: state machine, backoff, strategy rotation, safety) |
| [AF-PATCH-REVIEW-4.md](AF-PATCH-REVIEW-4.md) | Review of patch 9: safe to flash, should-fixes applied in patch 9.1 |
| [AF-ROI-X10.md](AF-ROI-X10.md) | Sharpness ROI analysis: centre 25%x25%, x5 vs x10 scene coverage, why Canon x10 wins, ROI-selector/x10 experiment |
| [EOSM-AF-TESTING-V2.md](EOSM-AF-TESTING-V2.md) | Four-tier test strategy, acceptance criteria, coverage matrix, protocol gaps, proposed log fields |
| [../contrib/af_replay/REPORT_LAB.md](../contrib/af_replay/REPORT_LAB.md) | Nine search algorithms simulated on real fitted curves; ranked recommendation |
| [EOSM-AF-NOVEL.md](EOSM-AF-NOVEL.md) | 15 hardware-grounded ideas, scored; top-3 one-session experiments; killed ideas |
| [EOSM-AF-STOP-ANALYSIS.md](EOSM-AF-STOP-ANALYSIS.md) | Offline proof from the recovered traces that `(9,3,1)` does not halt a `(9,1)` move, and why B's position model is therefore biased |
| [EOSM-AF-FOCUSFEED.md](EOSM-AF-FOCUSFEED.md) | Offline mining of Canon's `(9,0x17,32)` feed: log mining is exhausted and negative; the dedicated fixed-exposure protocol is next |
| [EOSM-AF-HANDOVER.md](EOSM-AF-HANDOVER.md) | The previous handover (pre-review facts and rules) |

---

## 1. Bottom line

1. **The "3/5 Refocus test" is weaker than it looks.** With the test's reference anchored (which
   the patch now does), the latest session is **2/5**, not 3/5: two trials never triggered at all
   and were still scored because the reference had already ratcheted down. The previous session
   was 2/5 and shows the handover's 115k → 77k erosion directly in the raw log.
2. **Both open problems were reference-definition bugs as much as search bugs.** The local search
   accepts a soft picture via its in-focus exit, and the Auto-refocus bookkeeping redefines the
   scene reference from each post-landing sample. A patch addressing both is now in the tree
   (committed, compile-clean): wide ±4×fine probe + drop-path guard + per-zoom high-water mark
   + a reference floor + an anchored Refocus-test reference.
3. **The cheap, safe shippable algorithm remains B (the default).** The simulator confirms
   G (predictive braking) and C (stop-on-threshold) can beat B *only if* `(9,3,1)` halts a running
   `(9,1)` move — and the recovered `AFSTOP.LOG` traces now settle that: **it does not.**
   In all 12 recorded stop trials the picture kept changing for 300–500 ms after the stop command,
   monotonically in the travel direction (median post-stop |slope| ≈ 43%/s; 0/12 halted). See
   [EOSM-AF-STOP-ANALYSIS.md](EOSM-AF-STOP-ANALYSIS.md). Do not port C/G; **B, with a better
   landing/acceptance rule, is the robust choice** — and B's own sweep position model is now suspect
   because it assumes the stop shortened the travel (see §8, P1).
4. **The two highest-leverage unknowns are physical, not algorithmic:** (a) does any command halt a
   running move, and (b) what is the real per-reversal offset R? Local search is near coin-flip
   at the modelled R = 60; measuring R is worth more than any threshold change.
5. **The real field logs were recovered** (the handover feared they were lost). They are now locked
   into an automated regression corpus, and the algorithm simulator runs on 48 real fitted curves,
   2 real sweep files and the measured erosion ratios.

---

## 2. Verified current state (evidence, not docs)

Full detail and line references: [AF-STATUS-REVIEW.md](AF-STATUS-REVIEW.md). The short version:

| Capability | Verdict | Key evidence |
|---|---|---|
| Engine B (sweep + stepped landing), default | **partly** — least-bad structure, claims unverified | bench B07 6/6 on card17, but the bench's +defocus half barely defocuses (see §4) |
| Engine A / D / E / F | **partly / unverified** | A slow; D needs a focal-length-dependent sigma; E depends on the erodible last landing; F inferior in simulation |
| Auto-refocus local search | **broken** (soft acceptance) | real log: `# local s0=82352 plus=36539 minus=86291 dir=0` → `# local in focus`; 82% of the last landing |
| Auto-refocus reference handling | **broken** (ratchet) | real `ref_s` 97338 → 84320 → 70812 and 115624 → 94755 → 85927 → 76595; per-landing ratios 0.82–0.91 |
| Trigger gates 0–12 | **works/partly** | 0 false triggers in both recovering sessions, but calibrated on a handful of 18–20 mm scenes |
| Recording path | **unverified/partly** | one good run in the handover; true dropped frames not measurable |
| Speed | **fails its own target** | bench B median 3.0–5.1 s; AFDRIVE `total_ms` 5.8 s includes a ~1.5–2 s epilogue that bench timing excludes |
| Focal-length coverage | **unverified / likely broken** | only the fine step adapts to focal length; probes, margins and gates are 18–20 mm constants |
| Safety/observability | **partly** | menu SELECT handlers can `task_create` before the recording check; `afd_must_abort` ignores continuous AF; the epilogue is single-exit by convention; a NODONE timeout drops chunks without a stop |

The review's top contradiction: the handover frames the local search as a plateau-threshold tuning
problem. In code it is an **acceptance-rule + self-referential-reference** problem. That changed the
patch design and the test baseline.

---

## 3. Real field data: what the recovered logs say

Recovered from the Claude scratchpad (`card3` … `card18`); the relevant files are now preserved in
`contrib/af_test/corpus/real-20261019/` (188 KB) with sha256 provenance.

**Refocus tests (the regression measure):**

| Session | ok | false_trig | ref_s sequence | per-trial final_pct | no-trigger trials |
|---|---|---|---|---|---|
| card18 `20261019-144447` | 3/5 | 0 | 97338 → 84320 → 70812 | 82, 92, 83, 96, 98 | def=150, def=400 |
| card17 `20261019-143547` | 2/5 | 0 | 115624 → 94755 → 85927 → 76595 | 88, 95, 77, 91, 87 | def=150, def=400 |

- Under an **anchored** reference (the patch keeps the session-start value unless the scene really
  gets sharper), card18's baseline is **2/5** — trial 5 lands at 69712/97338 = 71.6% and should
  have triggered. Post-patch target: **ok ≥ 4/5 with all 5 trials triggering**, not "3/5 → 4/5" on
  the old, shifting baseline.
- The engine's `pct` is `final_s / the run's own best`, so a soft local landing logs `pct≈98–101`.
  Do not use `pct` alone as an absolute focus measure.
- The actual failure trace in card18: the −40 probe returned **104.8%** of s0, just under the 105%
  improve threshold, so the local run chose "no side better" and took the in-focus exit at 82% of
  the previous landing.

**Bench:** card17 B07 is 6/6 (pct 103/100/100/101/100/108), B14 4/6, B1F 5/6. But in the +defocus
trials the measured `s_def` is 94.3–106.5% of the reference — the positive direction barely defocuses,
so the bench overstates reliability. This is now protocol gap #11 in the v2 testing doc.

**Actuator/motion:** `AFSTOP.LOG` planned stops at 150/250/350/450 ms all finished at the ~500 ms
FOCUS_DONE timer, and the per-frame sharpness traces keep changing for 300–500 ms **after** the stop
command, monotonically in the travel direction (12/12 trials; median post-stop |slope| ≈ 43%/s, 0/12
halted). `(9,3,1)` does **not** halt a running `(9,1)` move — see
[EOSM-AF-STOP-ANALYSIS.md](EOSM-AF-STOP-ANALYSIS.md). The only real `AFSWEEP.LOG` has 16 chunks/183
samples, 6/16 in the 0x0400 timer mode, and fits wider sweep sigmas (median 123 pulses vs ~19 for
stepped fits), which is why the new simulator pool gives different far-class numbers from the old
22-curve report.

---

## 4. The firmware patches now in the tree (committed, patches 1-10)

File: [modules/crop_rec/crop_rec.c](../modules/crop_rec/crop_rec.c) · ten stacked, committed
patches (reference/local-search, diagnostics/safety, and three rounds of review fixes) plus the
AFDIAG full-payload feed logging. Full `make -C platform/EOSM.202 zip` exits 0; `crop_rec.c`
compiles clean; `crop_rec.mo` md5 `ebb45f8b53ba4cd7b53036ca5a8a4e8f`, identical across forced
rebuilds. Field plan and exact log signatures: [AF-PATCH-FIELD-PLAN.md](AF-PATCH-FIELD-PLAN.md)
and [AF-PATCH2-FIELD-PLAN.md](AF-PATCH2-FIELD-PLAN.md) (§11 has the per-patch detail). Independent
reviews: [AF-PATCH-REVIEW.md](AF-PATCH-REVIEW.md) (first pass: "flash after fixes"),
[AF-PATCH-REVIEW-2.md](AF-PATCH-REVIEW-2.md) (four state must-fixes, closed by patch 5), and
[AF-PATCH-REVIEW-3.md](AF-PATCH-REVIEW-3.md) (**safe to flash, no blocking findings**; residual
non-blocking items in its §3: rare anchor texture-metric seeding, focal-length debounce call-count,
and stop_check integer-boundary truncation — all logged and self-recovering).

**Patch 1 — landing and reference integrity** (the original open-problem fix):

1. **Wide probe / drop-path guard** in `afd_run_local`: when no side improves and the best probe is
   < 92% of the local reference, or the trigger came from the real drop path, probe ±4×fine before
   any in-focus exit; still flat → fall back to B. Patch 3 adds the post-climb guard: a climb that
   stalls on a shoulder below 92% of the reference also falls back to B, so a noisy narrow probe
   cannot sneak a soft landing through.
2. **Reference high-water mark** `afd_ref_max_s`: rises only on 300 ms-verified landings. Patch 3
   keys it by digital zoom **and** focal length (10 mm bucket), feeds it only from engine mode 0,
   and resets it when RAW recording starts/stops, Auto refocus is toggled, a foreign AF runs, the
   scene changes, or the focal-length bucket changes.
3. **True Auto-refocus reference anchor** (patch 3 replaces the first pass's 95%-per-settle floor):
   `aref_ref_anchor` is seeded/raised from the old peak and the ref max, adopted exactly when the
   floor wins, and clamped into `aref_ref` so the 12–24 s peak rotation cannot erode the trigger
   reference. A genuine gradual contrast change is handled explicitly: three consecutive settles
   below 92% of the anchor reset it (`# aref ref anchor: confirmed soft scene -> reset`).
4. **Anchored Refocus-test reference** `afrt_reference`: a re-reference after a failed trial cannot
   lower `ref_s`; `# RT_REF` now logs both `meas=` and the adopted `ref_s=`, and a >15% luma change
   is treated as a scene change rather than an AF failure.

**Patch 2 — diagnostics and safety** (no algorithm change): a no-trigger Refocus-test trial is
`valid=0, ok=0` (with `notrig`/`false_pass` in the summary); `search_ms` and `epilogue_ms` are
logged separately from `total_ms`; `# refmax`/`# aref done ref_max/land_vs_ref`/`# aref ref
anchor` make reference behaviour visible; `# stop_check` summarises every stop-test trial; the
9 menu SELECT handlers check recording **before** `task_create`; a NODONE timeout sends an explicit
stop; Auto-refocus runs abort when Movie Servo is active (menu AF deliberately stays usable —
`is_continuous_af()` reports the menu setting, not an active servo).

**Patch 3 — review must-fixes** (as above) plus cheap hardening: wide-probe positions and focal
length in the logs, trigger-hint globals cleared for non-Auto-refocus runs, and the scene reset
clearing the last-landing reference together with the high-water mark.

**Patch 4 — confirmed-soft recovery**: three consecutive verified landings below 92% of the ref max
(a genuinely softer scene, not a one-off miss) reset the max to the best of those three, so the local
search cannot stay on the "wide probe → B" path for a whole session.

**Patch 6-9 (field-driven, 2026-10-19 evening)**:
- **Patch 6**: auto-refocus absolute fallback retries (up to 3) after a failed run; Refocus-test reference validation (`verify local better/kept`, `emax=`).
- **Patch 7**: Refocus-test adaptive trial defocus (a no-trigger trial retries with doubled defocus: `# RT_RETRY`, `retries=`), needed for high-gearing/adapted lenses; focal-freeze docs + `mm_pre/mm_post`.
- **Patch 8**: AF-diag **Zoom refresh (read lens mm)** action (request fresh PROP_LV_LENS, fall back to close/read/fine/reopen) and automatic ~1 s zoom detection (`# zoom probe`, `# zoom auto`); the MPU monitor freezes the LV focal-length property, EXIF is the ground truth.
- **Patch 10** (2026-10-19 late): hunt false-sharp bug fix (`aref_hunt_pre_s` stale -> no attempts), **Focus tolerance** setting (Strict 92 / Normal 88 / Relaxed 85, default Normal), learned fine step from the sweep peak width (fixes the too-small steps on adapted/EF lenses), fast epilogue + skipped landing verify for auto-refocus runs (speed), and 55 mm fallbacks (RT reference full-B retry when B07 fails; full-width resweep after a flat landing).
- **Patch 9/9.1**: persistent **never-give-up** Auto-refocus hunt (setting Persistent/Normal, default Persistent): a soft landing is a failure, a frozen episode reference judges sharpness, and a rotating portfolio (local -> full B -> Canon x10 -> Canon in-place -> full B alternate/reversed) keeps retrying forever with bounded jittered backoff; Normal = patches 1-8 unchanged.
**Patch 5 — second-review state fixes**: the anchor and ref max can no longer re-raise each other
after a confirmed-soft reset; anchor texture metrics are seeded from the settled sample (no
self-locking gate 4 after the 12–24 s rotation); the Refocus test clears anchor and ref max before
and after the test (no HOLD1 false triggers from a pre-test scene); the last-landing fallback gets a
focal-length key and no longer takes bench/speed-test landings; repeated no-focus runs clear a stale
anchor; the anchor clamp is scoped to the MPU refocus method; the inline `# stop_check` verdict now
matches the offline analyser exactly; the last unguarded menu `task_create` is wrapped; and the
focal-length bucket is debounced.

Algorithms A/B/C/D/E/F, trigger thresholds, dwell/settle and the epilogue are unchanged.
Full build and determinism checks are repeated after every patch; the final md5 and the exact
acceptance additions are in [AF-PATCH2-FIELD-PLAN.md](AF-PATCH2-FIELD-PLAN.md) §11.

**Why this is the right first patch set:** it fixes both halves of the soft-landing problem
(search acceptance *and* reference definition) without new commands, makes the Refocus test
honest (`notrig=0`, `valid=5`), and logs enough to diagnose a failure. The simulator says the
acceptance-rule change alone cuts the 80–90%-of-reference landing band from 8% to 5–7%, but the
reversal offset R remains the dominant error source.

---

## 5. Better testing — now automated and real-data-backed

One command: `make af-test` or `python3 contrib/af_test/run_all.py` (~14 s, exit 0):

```
PASS parser self-test (af_replay --selftest)            0.2s
PASS unit + simulator + tool suite (2 test dirs)     12.7s  run=81 passed=81 failed=0 errors=0 skipped=0
PASS synthetic-log CLI pipeline                         0.9s  6 runs replayed, 2 verdicts
PASS field-log regression corpus                     0.7s  6 case(s): 6 pass, 0 fail, 0 skip
unit tests: 81 run, 81 passed; tiers: 4 pass, 0 fail, 0 skip; RESULT: PASS
```

Four tiers (details and acceptance criteria in [EOSM-AF-TESTING-V2.md](EOSM-AF-TESTING-V2.md)):

1. **Host unit + simulator** — AFSWEEP parser (both the documented and the shipped 4-field format),
   curve-fit recovery, simulator determinism, algorithms A–F termination/bounds, `af_report`
   parsing/verdicts, CLI smoke tests. Two real host crashes were found and fixed on the way:
   `af_replay.py` crashed on any real AFSWEEP.LOG, and `af_report.py` crashed on a truncated
   `# aref` line.
2. **On-camera bench** — B speeds and A+B+D+F, with per-focal-length acceptance targets.
3. **Field protocol** — Refocus test, auto-refocus walk, REC test, now with explicit pass/fail
   per tier and the no-trigger-trial trap called out.
4. **Field-log regression corpus** — the four real cases above, with raw-log facts (per-trial
   final_pct, ref_s sequences, aref new_ref drift, fast/timer FOCUS_DONE split, bench ±defocus
   strength, sweep geometry), sha256 provenance, and cross-checks against `af_report`.
   `post_patch_expectation` blocks are marked **TARGET, not current** and are not enforced.

What is still camera-only: the local-search gate logic, end-stop, monitor/session and abort paths,
dropped-frame counting (needs an mlv_lite accessor), and per-landing reference logging.

---

## 6. Algorithms — what the simulation says

`contrib/af_replay/REPORT_LAB.md`, cold near+mid+far on the real 48-curve pool, N=100–200/cell:

| Rank | Algorithm | Success | Median time | Moves | Verdict |
|---|---|---|---|---|---|
| 1 | **G** sweep + predictive braking | 90–91% | 4.1–4.2 s | 7 | **conditional on a working stop** |
| 2 | **J** two-sided slope probe | 86–87% | 5.4–5.6 s | 9 | cheap direction improvement, not a new structure |
| 3 | **C** sweep + stop-on-threshold | 86% | 4.3–4.4 s | 7 | **conditional on a working stop** |
| 4 | **B** sweep + stepped landing | 85–86% | 3.9–4.0 s | 6–7 | **shippable default** |
| 5 | H Gaussian fit + jump | 78–82% | 6.0 s | 10 | not competitive cold |
| 6 | F predictive fine pass | 77–78% | 9.8–9.9 s | 21 | no gain over B |
| 7 | I Brent/golden-section | 77–81% | 6.2–6.3 s | 10 | not competitive cold |
| 8 | A stepped baseline | 74–78% | 10.8–10.9 s | 23 | slow |
| 9 | K calibrated DFD jump | 73–74% | 5.7–6.3 s | 9–10 | not recommended (calibration + far/low-contrast failure) |

Robust across the real and synthetic curve families: **G, C, B only**. With the stop modelled as a
no-op, C falls 86% → 65% and G 90% → 84% (B 85% → 79%). Recommendation: **keep B**; take a
J-style direction probe only as a cheap experiment; do not build a new search on `stop()` before the
stop test exists.

**Auto refocus in simulation** (warm/tracked, measured field ratchet 0.866 per landing):

| Variant | Success | 80–90% of reference | Reference drift |
|---|---|---|---|
| current local (`v0`) | 43–45% | 7–8% | 1.00 → 0.49 |
| wide probe + drop guard (`v1`) | 49% | 7% | 1.00 → 0.49 |
| v1 + high-water mark (`v2`, this patch) | 47–48% | **5%** | **1.00** |
| full sweep B on the same triggers | 67–72% | 5–6% | 1.00 |

So the patch is cheap and correct, but **the local shortcut buys moves, not accuracy**. At the
modelled reversal offset R = 60, local v0 succeeds 42% and drops to 26% at R = 100, while B stays at
72%/41%. Once the patch is field-tested, **A/B test `Refocus AF = Full (B)` vs `Local`** — the
simulator says B is both more accurate and not slower for the small defocus that Auto refocus sees.

---

## 7. Out-of-the-box — the three ideas worth a session

Full scoring and 12 more ideas: [EOSM-AF-NOVEL.md](EOSM-AF-NOVEL.md).

1. **N1 — decode Canon's per-frame AF feed `(9,0x17,32)`.** The feed is real and live (≈4 Hz at the
   hook's rate limit; 643 messages in the Refocus log). `PROP_LV_FOCUS_DATA` never reaches ML on the
   EOS M, so the packet-builder hook is the only access path. **Offline log mining is now exhausted
   and negative:** across 26 unique AFDIAG logs (3,204 feed records, 59 static windows), no payload
   field is a usable defocus proxy. The only word ML does not already consume, A (bytes 4–7), is
   brightness-linked (ρ(A,luma)=+0.80) with no consistent sharpness relation; B is a
   contrast/sharpness evaluation (ρ(B,s)=+0.90) already used by Canon. Decisive limitation: there are
   **zero controlled speed-07 lens moves in any session that logged the feed** — the stimulus was
   always Canon's own AF. See [EOSM-AF-FOCUSFEED.md](EOSM-AF-FOCUSFEED.md).
   **The logging change is now implemented (task-12):** the AFDIAG `@MPU` hook logs all 32
   payload bytes of the feed (`feed=32B w4..w8`, ≤4 Hz; expect a 1 MB rotation in a 30-minute
   session, so copy both `AFDIAG.LOG` and `AFDIAG.OLD`). Next is the 30-minute fixed-exposure
   protocol:
   manual ISO/shutter, static chart, 10 Canon half-press runs, then known speed-07 moves
   +200/+100/+40/−40/−100/−200 at two distances, repeated, AF diag ON + MPU. Accept a field only if
   it is monotone with |commanded pulses|, survives fixed exposure and no half-press, has ρ(field,luma)
   < 0.3 and does not track frame geometry. Kill N1 if no field passes.
2. **N2 — two-position differential multi-band jump.** The only non-Canon estimator that passed
   leave-one-scene-out validation on the real sweeps: `Δlog(s1/(s2+s4))` over one known move gives
   MAE 10–20 pulses and 100% within 40 pulses at d = 20–40 (do-nothing MAE 23–27). One small
   algorithm patch + a chart calibration session. Absolute single-frame band energy does **not**
   generalise (cross-scene spread ≈ its whole dynamic range).
3. **N10a — Canon x1 half-press detour for non-recording Auto refocus.** Drop the preset to x1, let
   Canon's Hybrid CMOS AF focus, return to x5; highest practical payoff for the 3/5 if the preset
   restore is clean. Kill on any state glitch.

**Killed by the prototypes:** 3-point dither vertex (worse than doing nothing at measured 1% noise),
edge-width as a fine metric (saturates below ~0.5 px blur), waiting for `0x8005001E`/`(9,0x14)`
(never arrive). **Best information leak:** N1 first, then a motor-step counter in MPU RAM
(`MpuMonRead` diff before/after known moves, `0xFFF81300`/`0xFFF8256C`), which costs one open
monitor and no new protocol.

---

## 8. Prioritized way forward

### P0 — next field session (one build, one sitting, ~15 min)

1. **Build + flash this tree** (`make -C platform/EOSM.202 zip SHELL=/bin/bash PYTHON=python3`),
   keeping the current zip as the A/B baseline. **Set Movie Servo OFF** for the Auto-refocus runs
   (patch 2 deliberately aborts Auto-refocus engine runs when the servo setting is on).
2. **Refocus test at 18 mm**, monitor Keep open, `Refocus method = MPU drive`,
   `Refocus AF = Local`, sensitivity High/MPU. Accept: **notrig=0, valid=5, false_pass=0,
   ok ≥ 4/5**, `false_trig 0`, no `82–83%` landing while `drop=1`, `# RT_REF anchor kept` only after
   a failed trial on a static scene, `# local ... wide probe` visible on the hard trials, and
   `# refmax`/`# aref done anchor=` not staircasing down (the anchor may drop only via
   `# aref ref anchor: confirmed soft scene -> reset`). If it fails, restore the old build and
   repeat once.
3. **Auto-refocus walk** across 3 subjects at 18 mm for a few minutes; watch `# refmax` (zoom/mm),
   `# aref done ... ref_max= land_vs_ref= anchor=`, latency (soft runs cost +2 moves ≈1.2 s; a
   still-flat run adds a B fallback ≈5–8 s), and whether the trigger reference holds for ≥60 s on a
   static soft scene.
4. Read `# RESULT ... total_ms= search_ms= epilogue_ms=` and stop conflating search time with the
   epilogue. Run **AF sweep record** and the new **AFSTOP** (`# stop_check halted=0`) on the same
   session.
5. Copy `AFDRIVE.LOG`, `AFRTEST.LOG`, `AFBENCH.LOG` off the card, run
   `python3 contrib/af_report/af_report.py --copy <dir>`, then add the session to the corpus with
   `python3 contrib/af_test/add_case.py`.

### P0 — motion-model session (same day if possible)

5. **AF stop test** on a long move (400 pulses) started on a sloped flank, with the new `# stop_check`
   summary — the recovered traces already show stop does not halt, but this makes it a first-class,
   repeatable measurement. Do **not** build C/G regardless of the outcome unless a different stop
   primitive is found.
6. **AF sweep record** at 07/14/1F and **AF reversal cal** with many repeats at several move sizes —
   this fixes the time→position model and measures R, the dominant error term.
7. Log `search_ms` separately from `total_ms` (the epilogue currently inflates the field "AF time") —
   patch 2 adds this.

### P1 — next two weeks

8. **Fix B's sweep position model** for the no-op stop. `afd_sweep()` currently takes
   `end_t = stop_ms + COAST` and derives `moved`/`afd_pos`/`pk` from it; if the lens ignores the stop,
   `moved` is underestimated and the feedback landing starts from the wrong place. Candidate fix: prefer
   the already-computed s-stability end time (`t_stab`), else assume the full chunk (`t_exp`,
   `moved = len`). Trial as a separate change after the reference patch is field-validated, so the delta
   is attributable; watch `travel`, `# land flat -> resweep`, and sweep-record position error.
9. **A/B test `Refocus AF = Full (B)` vs `Local`** on the Refocus test (simulator favours B).
10. **35 mm and 50 mm**: Refocus test + bench at each; confirm `lens_info.focal_len` is valid under
    crop presets. If it is, derive pulses-per-DOF and scale probe/landing constants instead of
    hand-tuning.
11. **N1 decode:** the full-32-byte logging is implemented (`feed=32B`, no behavior change); run the
    fixed-exposure known-move protocol from [EOSM-AF-FOCUSFEED.md](EOSM-AF-FOCUSFEED.md) §8 and
    analyse with `contrib/af_novel/analyze_focusfeed.py` (it now exposes `r['b32']`). If field
    mining stays negative, proceed with **N2** differential jump in parallel.

### P2 — debt and safety (before the next recording test)

12. Guard `task_create` in menu SELECT handlers with the recording precondition.
13. Add `is_continuous_af()` to `afd_must_abort`; send/log an explicit stop on a NODONE timeout
    (patch 2 does the latter; the former only if it cannot break menu AF).
14. Make the engine epilogue a real single cleanup path; log monitor/session state at entry/exit.
15. Log a per-landing reference field (old/new/landing) so reference erosion becomes a corpus fact
    rather than an aggregate (patch 2 adds `# refmax`).

### P3 — longer term

16. N10a x1 half-press detour; MPU-RAM step counter; N12/N13 actuator metrology.
17. Warm continuous refocus with deadband + Kalman once R and pulses-per-DOF are measured.
18. C/G/predictive braking **only if** a future stop primitive actually halts motion; otherwise keep B.

---

## 9. Caveats

- **Nothing here is field-validated.** The patch is compile-clean and reasoned from real logs, but
  the 92/95/90/75 thresholds are derived from a small number of 18–20 mm scenes.
- **Simulation is a ranking tool, not a camera model.** Its dominant uncertainty is the reversal
  offset R (uniform ±60 assumed, unmeasured) and the sweep stop latency; both tables say so.
- **Historical corpus ≠ post-patch expectation.** The four corpus cases encode the pre-patch build.
  A post-patch session must be added as a new case; the `post_patch_expectation` blocks are targets.
- **Build gotcha:** the build regenerates `modules/crop_rec/README.rst` from the empty
  `module_strings.h` placeholder and overwrites the hand-written file. `git checkout --
  modules/crop_rec/README.rst` after building. Keep the 3-line placeholder or the build fails
  (python2 is unavailable).
- **Do not change several algorithm parts in one field run.** The patch is one coherent change
  (landing/reference integrity); the field plan tests it in isolation before anything else lands.
