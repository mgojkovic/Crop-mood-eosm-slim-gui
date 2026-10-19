# EOS M MPU autofocus: test protocol v2

**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` · **Module:** `modules/crop_rec/crop_rec.c`
**Supersedes/extends:** [EOSM-AF-TESTING.md](EOSM-AF-TESTING.md) (the 10-minute field session)

This version adds an automated host-side pyramid below the existing camera procedure, a
field-log regression corpus, and concrete acceptance criteria. The camera protocol in
EOSM-AF-TESTING.md still applies for the manual tiers; this document says what must be
true at each tier, what is already automated, and which gaps remain.

```
                    ┌──────────────────────────────┐
   fewer, slower    │ 4. field-log regression      │  replay real sessions,
   closer to the    │    corpus (host)             │  corpus/<case>/expected.json
   real product     ├──────────────────────────────┤
                    │ 3. field protocol (camera)   │  Refocus test, Auto refocus,
                    │    AFRTEST / AFDRIVE         │  REC, focal lengths
                    ├──────────────────────────────┤
                    │ 2. on-camera bench (camera)  │  AF bench A/B/F/D, diagnostics
                    ├──────────────────────────────┤
                    │ 1. host unit + simulator     │  parsers, curve fit, algorithms
   fastest, most    │    (no camera, no card)      │  A-F, af_report verdicts
   tests            └──────────────────────────────┘
```

A change is not "done" until tiers 1 and 2 pass; the move to a new firmware zip must also
not regress tier 4.

---

## 1. Tier 1 — host unit + simulator (automated, no camera)

**Command**

```
python3 contrib/af_test/run_all.py            # all host tiers, PASS/FAIL summary, exit code
make af-test                                  # the same thing via the top-level Makefile
python3 contrib/af_test/run_all.py --verbose  # full unittest output
python3 -m unittest discover -s contrib/af_replay/tests -v   # parser/simulator/algorithm suite
python3 -m unittest discover -s contrib/af_test/tests -v     # tool suite (AFSTOP analyzer)
```

**What it covers** (files in `contrib/af_replay/tests/`, `contrib/af_test/`):

| Test file | Covers |
|---|---|
| `test_parser.py` | AFSWEEP parser round-trip (documented and shipped firmware formats), time->position mapping, AFDRIVE trace parsing, same-direction series extraction, known peak/sigma recovery |
| `test_curvefit.py` | `B + A*exp(-(p-p*)^2/2σ²)` recovery on noiseless/noisy synthetic Gaussians, rejection of flat/slope/too-short series, `accept_fit` range checks |
| `test_simulator.py` | seed determinism (trial and Monte Carlo cell), curve peak, exact same-direction moves, reversal offset bound `R`, measurement noise, continuous move + `stop()` coast, run cap / `AFTimeout`, timer-mode probability table |
| `test_algorithms.py` | algorithms A-F terminate on all defocus classes, run cap respected, bounded moves/travel, near-start success floors, B faster than A on identical cold trials, high-noise survival |
| `test_af_report.py` | AFDRIVE/AFRTEST/AFBENCH parsing, LV-minute/hunting-index maths, truncated-line robustness, report sections and verdicts for `healthy`/`slow`/`hunting` synthetic sessions, metric extraction |
| `test_corpus.py` | corpus convention, skip-when-absent, regression detection, replay curve check, `add_case.py` end-to-end |
| `test_cli.py` | `af_replay.py --selftest`, full `--logs` replay, `af_report.py --logs`, error paths |
| `contrib/af_test/tests/test_stop_analysis.py` | AFSTOP parser on the real card8/card9 logs (10-token and shipped 4-value sample lines), post-stop classifier (all 12 real trials "continued"), a synthetic "halted" trace that must classify differently, CLI exit codes 0/1/2, stop corpus cases pass, tampered stop expectation fails |

The synthetic session generator (`contrib/af_test/synth_logs.py`) writes logs in the formats
actually produced by `crop_rec.c` (including the shipped AFSWEEP sample line
`S <t_ms> <s1> <g> <luma>`) with known ground truth in `expected.json`, so the suite never
needs a camera or a card.

**Acceptance criteria**

- `run_all.py` exits 0; suite runtime < 60 s (currently ~13 s).
- All unittest cases pass; skips are allowed only for an empty field-log corpus.
- AFSWEEP synthetic round-trip: recovered peak within ±8 pulses and σ within ±8 pulses of
  the generated values (positive and negative sweep direction).
- Noiseless Gaussian fit: peak and σ recovered within ±1.5 pulses; noisy (±1 %) within ±6.
- Simulator: same seed => bit-identical results; reversal offset never exceeds `R`.
- Algorithms A-F: no exception, `t <= AFD_RUN_CAP_MS` (15 s), 1..100 moves, travel
  <= 8000 pulses; near-defocus success floors A≥6/10, B≥8/10, C≥7/10, D≥7/10, E≥6/10,
  F≥7/10; median B time < median A time on identical cold trials.
- `af_report.py` on the synthetic `healthy` session: `AF drive OK`, no `HUNTING`/`CRASH`;
  on `hunting`: `HUNTING` + `Refocus test: only 2/5 ok`; on `slow`: `AF drive NOT OK`.

**Known host-side regressions found and fixed while building this suite** (both were
one-line parser fixes, covered by tests):

1. `af_replay.py` crashed with `IndexError` on the **shipped** AFSWEEP.LOG format
   (`S t_ms s1 g luma`, 4 fields) because it only knew the documented 10-field format.
   It now accepts both.
2. `af_report.py` crashed on a truncated `# aref` line at the end of a log
   (card pulled during a write); the parser now ignores it.

---

## 2. Tier 2 — on-camera bench (manual, 4–6 min per focal length)

**Setup:** as in EOSM-AF-TESTING.md — LiveView, movie mode, x5 + crop preset, AF mode,
textured static subject 0.5–2 m, tripod; `AF drive monitor: Keep open`, `Sweep speed: 07`,
`Session close: ON`.

**Command (camera):** Settings → AF diag → **AF bench** with `Bench algos = B speeds` (or
`A+B+D+F` for the full matrix). Then on the Mac:

```
python3 contrib/af_report/af_report.py --copy ~/af_logs     # defaults to the card mount
```

**Acceptance criteria (18 mm, engine regression)**

- `AF drive runs`: success >= 90 %, median `total_ms` <= 5000 ms.
- Bench: algorithm **B** >= 5/6 trials with `pct >= 90` per defocus set; no `ab=nopeak`
  on a clearly textured subject; A and F not dramatically behind; D/E may stay inferior.
- **Bench design caveat (measured 2026-10-19, card17):** the `+defocus` trials barely
  defocus the scene (`s_def` 94-107 % of the reference) while `-defocus` really does
  (29-35 %). Only the negative half currently tests AF recovery; treat positive rows as a
  control and do not raise the acceptance on them until the bench drives the lens harder
  (see gap 11).
- No new CRASH/ASSERT log.
- Monitor open produces the expected one-time ~2 s LV blackout and the session is closed
  (`# epilogue` present, `session_close=ON`).

**Diagnostics that must be run when a criterion fails:** AF sweep record (AFSWEEP.LOG),
AF reversal cal (AFREV.LOG), AF stop test (AFSTOP.LOG); drop the logs into the corpus
after analysis (tier 4).

---

## 3. Tier 3 — field protocol (manual, ~10 min per session)

The existing EOSM-AF-TESTING.md session stays the baseline procedure (bench, Refocus test,
3-subject Auto refocus, REC test, close monitor). v2 tightens the acceptance and the
required repeats:

| Test | Runs per session | Acceptance target |
|---|---|---|
| AF bench B speeds, 18 mm | 1 (6 defocus values) | bench criteria above |
| Refocus test, 18 mm | **2** (to expose repeatability) | `ok >= 5/5` each, `false_trig <= 1` total; latest field result is 3/5 — this is the open problem |
| Refocus test, 35 mm and 50 mm | 1 each | `ok >= 4/5`, `false_trig <= 1` (first-ever data at these focal lengths) |
| Auto refocus, 3 distances | 3 subjects × 5 s each | every trigger lands >= 90 % of the reference and settles; no hunting from panning or blank walls |
| Auto refocus + REC (RAW, MPU drive ON) | 1 × 30 s | recording does not stop; LV cadence steady; `# REC ... stopped_in_run=0` |
| Close monitor / session safety | 1 | monitor closes, no crash, lens idle |

Focal lengths are part of the protocol now, not an optional footnote: almost all existing
data is 18–20 mm while the user reports worse AF at 30–50 mm.

---

## 4. Tier 4 — field-log regression corpus (automated replay of real data)

**Command**

```
python3 contrib/af_test/corpus.py -v       # replay only the corpus (currently 6 cases, ~0.7 s)
python3 contrib/af_test/run_all.py         # all tiers; tier 4 is PASS, not SKIP, with the real data
make af-test                               # same through the Makefile
```

**Dataset:** `contrib/af_test/corpus/real-20261019/` (~193 KB) holds a curated subset of the
2026-10-19 field session, copied from the Claude scratchpad card snapshots. Each case's
`expected.json` records the source directory, the build (`pre-task-6`), and the sha256 of
every log; the corpus fails with a `provenance:` error if a fixture is edited.

| Case | Logs | Locked facts (derived from the raw logs) | What a failure means |
|---|---|---|---|
| `card18-refocus` | card18 AFDRIVE + AFRTEST | Latest Refocus test `ok=3/5`, `false_trig=0`, `runs_total=4`; per-trial final_pct `82,92,83,96,98`; ref_s `97338 -> 84320 -> 70812`; trial `def=400` never triggered yet counted ok (98 % of the eroded ref); local search accepted `s0=82352` = 80.8 % of the last landing `101880`; 51 fast / 6 timer FOCUS_DONE (10.5 % timer) | parser/curve-fit changed on real data; the report verdict changed; someone tried to "fix" the fixture instead of the code; the local-search false "in focus" or reference erosion disappeared without new data |
| `card17-refocus` | card17 AFDRIVE + AFRTEST | `ok=2/5`, `false_trig=0`, `runs_total=3`; final_pct `88,95,77,91,87`; ref_s `115624 -> 94755 -> 85927 -> 76595` (confirms the handover's 115k -> 77k erosion claim); `def=150,400` never triggered; 1 `nopeak` abort in 6 AF runs; 42 fast / 6 timer moves | same as above |
| `card17-bench` | card17 AFBENCH | B07 `6/6` pct `100-108`, B14 `4/6`, B1F `5/6`; +defocus s_def 94.3-106.5 % of ref vs -defocus 28.9-34.9 % (bench design weakness above) | bench parsing/table changed; B07 no longer 6/6; the negative-defocus coverage regressed |
| `card8-sweep` | card8 AFSWEEP | only real sweep: 16 chunks, 183 samples, 6/16 in the 0x0400 timer mode; fitted sigma_s 37-239 pulses (median 123); 11 series, 9 usable fits | AFSWEEP parser/time->position mapping changed; sweep geometry or timer-mode share regressed |
| `card8-stop` | card8 AFSTOP | **stop-halt evidence:** 2 runs x 4 planned stops (150/250/350/450 ms); 8/8 trials NOT halted; s keeps changing 300-500 ms after `t_stop`; post-stop least-squares slope 8.3-62 %/s; every max excursion is in the commanded direction | AFSTOP parser/classifier changed, or a stop that used to coast now looks frozen (which would invalidate the C/G/predictive-braking decision) |
| `card9-stop` | card9 AFSTOP | **stop-halt evidence, early stops:** 1 run x 4 planned stops (80/80/160/160 ms); 4/4 NOT halted; biggest changes (68-171 % delta, 87-406 %/s slope); `s2/s4/band` are zero in every full-format sample (trimmed build), locked as `zero_columns`; s1 carries the classification | same as card8, plus the 10-column trimmed sample shape must keep parsing |

**What each case checks**

1. `af_report.py` on the case (`--all --no-state`): verdict strings, AF drive runs/success/
   median, RTEST ok/false_trig, bench row. Note that `--all` includes the Refocus-test runs
   in the AF drive statistics, which is why both refocus cases show `AF drive NOT OK`
   (median > 5 s) — that verdict is locked as the historical output, not a target.
2. `af_replay.py` parse roll-up: runs, measured moves, same-direction series, usable curve
   fits, sweep chunks and samples must not drop (`replay` block).
3. **Raw-log facts** (`facts` block, computed directly from the logs, not from the report
   text): per-trial `final_pct` sequence, `ref_s` and its monotonic decrease, per-trial
   trigger/no-trigger state, auto-refocus `new_ref` drift, FOCUS_DONE fast/timer split,
   local-search acceptances (`# local ... last=`), bench rows and positive/negative defocus
   strength, AFSWEEP chunk/sample/width statistics, and AFSTOP stop-halt rows (planned stop
   times, halted/continued split, per-trial post-stop delta/slope/max excursion). Range
   syntax `{min,max}` is used for values that should stay in the same regime, exact lists
   for the data itself.
4. **Consistency:** the raw AFRTEST facts and the `af_report` text must agree on `ok`,
   `false_trig` and trial count, so a hand-written expectation cannot hide a parser bug.
5. **Provenance:** sha256 of every copied log.
6. `post_patch_expectation` (refocus cases): marked `"status": "TARGET, not current"` and
   **not enforced**: `ok >= 4/5 with all 5 trials triggered` is the target once the anchored
   reference fix is flashed. The fixture deliberately stays the pre-patch baseline; with
   the anchored reference (task-6, `AFRT_REF_FLOOR_PCT=95`) card18 becomes 2/5 (trial 5 is
   69712/97338 = 71.6 %). A post-patch field session is added as a **new** case.

**Stop-halt analysis (AFSTOP)** — does a `(9,3,1)` stop halt a `(9,1)` relative move?
This is the question the camera's **AF stop test** (AF diag -> mode 5) exists to answer,
and it is a prerequisite for algorithms C/G and any predictive braking. Run:

```
python3 contrib/af_test/stop_analysis.py            # both real corpus cases (default scan)
python3 contrib/af_test/stop_analysis.py AFSTOP.LOG # one log (or a directory), exit code
python3 contrib/af_test/stop_analysis.py --json     # rows + summary as JSON
```

Per-trial output columns: `card, session, k, dir, at, t_stop, t_done, s@stop, s_end,
delta%, post-stop slope %/s, max excursion %, halted, class`. Current real evidence:
**12/12 trials continued, 0 halted**, median post-stop |slope| ~49 %/s (min 8.3 %/s),
median |delta| ~12 %, every trial's largest excursion is in the commanded direction.
`halted` is defined as post-stop least-squares |slope| <= 2 %/s **and** max excursion
<= 1 % (constants in `stop_analysis.py`); the real traces clear that by a wide margin.
`halted=0` therefore means the stop did **not** freeze the lens, and C/G/predictive
braking are conditional at best: they must **not** be ported as if `(9,3,1)` worked.

Exit codes make the fact a regression gate: `0` = every parsed trial not halted (the
historical evidence, expected); `1` = at least one trial appears frozen — the evidence
changed, re-examine the C/G assumption before relying on it; `2` = no complete stop trial
could be parsed. The two corpus cases (`card8-stop`, `card9-stop`) lock the historical
classification in `facts.stop`; a change in the analyzer/classifier or an edited fixture
turns tier 4 red. The analyzer handles both the documented 10-token sample line
(`S t frame s1 s2 s4 g q band luma`) and the shipped 4-value line (`S t s1 g luma`), and
reports zeroed full-format columns (card9 `s2/s4/band`) instead of failing on them.

**Failure meanings in one line:** a red corpus tier means real field evidence no longer
reproduces — fix the parser/report/algorithm, or, if the log fixture was intentionally
replaced, update its sha256/`expected.json` in the same change.

**Acceptance criteria**

- Every corpus case passes; tier 4 reports `6 case(s): 6 pass, 0 fail, 0 skip` (a case
  without logs is still skipped, so an empty corpus never fails).
- `replay` minimums and `facts` never drop after a parser change; ref_s sequences and
  final_pct lists are byte-for-byte historical data and must not be edited without a new
  fixture.
- Any fix adds at least one new case from the session that exposed the bug (or the PR
  explains why no card data is available).
- Corpus replay stays under a few seconds (`run_all.py` tier is ~0.7 s for 6 cases).
- Stop-halt analysis keeps reporting `0 halted / 12 continued` for the historical fixture;
  `stop_analysis.py` exits 0. A future build whose AFSTOP log freezes after `t_stop` makes
  the tool exit 1 (and is added as a *new* post-fix case, never by editing the fixture).

**How to add a case after a field session**

```
python3 contrib/af_test/add_case.py --logs /Volumes/EOS_DIGITAL --name 2026-10-20-18mm-refocus \
    --build "magiclantern-crop_mood.2026Oct19.EOSM202.zip"
```

`add_case.py` copies the known AF logs and writes a baseline `expected.json`: report checks,
raw-log `facts`, `replay` minimums, sha256 provenance and a `post_patch_expectation`
template. Review it — set `rtest.ok_at_least` to the target, add `must_not_contain` for the
failure being fixed, keep `facts` as the exact historical content, and put the target in
`post_patch_expectation`. Then run `corpus.py -v` and `run_all.py`. See
`contrib/af_test/corpus/README.md` for the full schema.

## 5. Coverage matrix

`H` = host unit/simulator, `S` = simulator (algorithm behaviour), `B` = on-camera bench,
`F` = field protocol, `C` = corpus replay.

| Behaviour | H | S | B | F | C | Status |
|---|---|---|---|---|---|---|
| AFDRIVE/AFSWEEP/AFRTEST/AFBENCH log formats | ✓ | | | | ✓ | automated on synthetic + 2026-10-19 real logs |
| Gaussian curve fit (peak/σ recovery) | ✓ | | | | ✓ | automated |
| Same-direction series / reversal offset model | ✓ | ✓ | | | | automated (model, not measured `R`) |
| FOCUS_DONE timing / timer-mode mixture | ✓ | ✓ | | | | fitted from logs; real speed still unmeasured |
| Stop `(9,3,1)` halts a `(9,1)` relative move | ✓ | | | ✓ | ✓ | **measured: it does NOT halt** (12/12 trials continue, post-stop \|slope\| 8.3-406 %/s); `stop_analysis.py` + card8-stop/card9-stop |
| Algorithms A-F termination, travel bounds | | ✓ | | | | automated |
| Algorithms A-F landing accuracy (ideal lens) | | ✓ | ✓ | | | host floors + bench |
| Algorithm A/B/C/D/E/F ranking vs real curves | | ✓ | ✓ | | | not a gate; bench decides |
| Local search accepts a soft picture (3/5 failure) | | ✗ | ✓ | ✓ | ✓ | **open — no host model of the local path** |
| Auto-refocus triggers/gates (13 gates) | | ✗ | | ✓ | ✓ | field-only; reported via AFRTEST |
| Refocus test ok / false_trig + per-trial final_pct/trigger state | | | | ✓ | ✓ | field + real corpus |
| Reference erosion across re-lands | | ✗ | | ✓ | ✓ | real corpus locks the `ref_s` sequences (97k->71k, 116k->77k); still no in-firmware per-landing field |
| Monitor open/close, session close, epilogue | | ✗ | ✓ | ✓ | | field bench verifies; not simulated |
| End stop (`afd_es_check`) | | ✗ | | ✓ | | field-only |
| No-op / abort paths (low texture, LV loss, timeout) | | ✗ | ✓ | ✓ | | aborts counted by report; no explicit tests |
| AF during RAW recording | | ✗ | | ✓ | | field-only; no dropped-frame counter |
| Speed (median total_ms) | | ✓ | ✓ | ✓ | ✓ | gates in tiers 2–4 |
| 35 mm / 50 mm focal lengths | | ✗ | | ✓ | | **gap — almost no data** |
| Low light / high ISO | | ✗ | | ✓ | | **gap — not in the protocol** |
| Scene/texture matrix (page/bookshelf/wall/foliage) | | ✗ | | ✓ | | **gap — logs are from a few scenes** |
| Distance coverage (0.3/0.5/1/2/5 m/∞) | | ✗ | | ✓ | | **gap — 0.5–2 m only** |
| Report verdicts (`AF drive OK`, hunting, crash) | ✓ | | | ✓ | ✓ | automated on synthetic logs |

## 6. Gaps in the current protocol (concrete work items)

Ordered by diagnostic value for the current 3/5 Refocus problem; each item says how to
close it.

1. **Repeatability (`n`).** A single Refocus test is 5 trials — the 1σ error of a success
   rate at n=5 is ~22 points, so 3/5 vs 5/5 is 1.5σ. Run **2 tests per focal length**
   (10 defocus events) and report ok/10; the acceptance target above assumes n>=10.
2. **35 mm and 50 mm.** Repeat the bench and 2 Refocus tests at 35 and 50 mm. `Fine step:
   Auto` (20/14/10 by focal length) is untested there; record `# focal_mm=` (already
   logged) and keep separate corpus cases per focal length.
3. **Low light.** Add a session at LV luma 30–50 (dim room, same textured subject):
   noise is multiplicative and the low-texture gate (`AFD_MIN_G`) may fire. Acceptance:
   same ok/false_trig targets, plus `# lowtexture` abort count reported.
4. **Scene/texture matrix.** Use a defined set of subjects — printed page, bookshelf,
   blank wall (negative control), foliage/curtain (repetitive texture) — and a distance
   set (0.3, 0.5, 1, 2, 5 m, infinity) at 18 mm. One session = one scene per corpus case,
   so the sensitivity (σ 9–29 pulses measured so far) gets real variation.
5. **Local-search-specific pass/fail.** The Refocus test reports the *outcome* (final
   pct), not why the local search accepted a soft picture. Add per-move evidence
   (see the proposed fields below) and a criterion: a local run that returns "in focus"
   with `final_pct < 92` is a **false success**; count those separately from misses.
6. **Dropped-frame measurement.** `# REC` currently reports LV vsync cadence proxies only.
   Expose an `mlv_lite` dropped/skipped frame accessor (or count `frame_count` deltas
   around the AF run) and log `frames_expected`, `frames_measured`,
   `max_vsync_gap_ms`; acceptance during the REC test: 0 dropped frames.
7. **Reference erosion tracking.** Each re-land lowers `ref_s`. Record the per-zoom,
   per-scene maximum and log the landing relative to it, so erosion is a number, not a
   manual read of the log: `ref_max_s`, `ref_age_ms`, `land_pct_of_max`.
8. **No-op / abort tests.** Explicit scenarios: blank wall (must abort `lowtexture`, not
   move), LV off mid-run (`nolv`), recording start mid-run, menu open mid-run, watchdog
   timeout. Acceptance: exactly one documented verdict per scenario, monitor/session
   always closed, no lens movement after an abort.
9. **Monitor / session safety tests.** Sequence: drive → monitor close → drive (reopen),
   LV exit while monitor marked open, wake after auto-close, session close ON/Restore/OFF.
   Acceptance: no crash, no 2 s blackout while recording, epilogue present in every
   AFDRIVE run.
10. **Reversal offset `R` and sweep speed are still assumed** (R=60 pulses, 0.4 pulses/ms
    in `af_replay.py`). Run AF reversal cal with many repeats at several move sizes and
    AF sweep record; feed the logs to the replay. Until then, algorithm rankings are
    structural, not predicted camera numbers.
11. **The AF bench does not defocus the positive direction.** Measured on the 2026-10-19
    card17 bench: requested `+150/+400/+800` produce `s_def` 94-107 % of the reference
    (barely defocused) while `-150/-400/-800` produce 29-35 %. So half the bench matrix is
    a near-focus control, not a recovery test. Fix: make `AFB_DEF_BACK`/positive-defocus
    travel larger (or aim at a lower-texture target for +defocus) so `s_def` drops below
    ~60 % of the reference; until then use `-defocus` rows for the engine verdict and keep
    the positive rows only as a drift control. The corpus locks the current weakness in
    `card17-bench` (`s_def_pct_positive_*` / `s_def_pct_negative_*` facts).

## 7. Proposed new log fields/counters (3–5 highest value)

All lines must obey the module's `snprintf` constraints (`%d`, `%x`, plain `%s` only).

1. **Per-trigger local-search evidence** — makes "why did it accept 82 %?" answerable
   without a logic analyser:

   ```
   # aref probe i=%d dir=%d step=%d s=%d pct=%d best_pct=%d verdict=%s
   # aref local s0_pct=%d wide_probe=%d flat=%d fallback=%s exit=%s
   ```

   (`verdict` = better/flat/drop; `fallback` = none/wide/B; `exit` = in_focus/fallback/abort)

2. **Reference-erosion tracking per landing:**

   ```
   # aref land pct=%d ref_s=%d ref_max_s=%d ref_age_ms=%d land_pct_of_max=%d zoom=%d
   ```

3. **Dropped-frame / cadence counters in the REC line** (needs a small mlv_lite accessor):

   ```
   # REC ... frames_expected=%d frames_logged=%d frames_dropped=%d vsync_max_gap_ms=%d
   ```

4. **Abort/no-op cause and phase** — one counter line per non-success exit:

   ```
   # RESULT ... abort_phase=%s abort_reason=%s lowtexture=%d nolv=%d n_nodone=%d n_err=%d
   ```

5. **Measured actuator numbers** (turn the simulator assumptions into data):
   AFREV already logs `backlash_est`; add per-run `mspp_07_x10=%d mspp_1f_x10=%d
   coast_ms=%d reversal_min=%d reversal_max=%d` to AFSWEEP/AFREV, then update
   `Params.R`, `sweep_v` and `stop_lat` from the logs.

Any of these should be added in `crop_rec.c` *and* parsed in `af_replay.py`/`af_report.py`
with a host test in `contrib/af_replay/tests/` (a synthetic line is enough to pin the
format), and a corpus case from the first session that contains it.

## 8. Regression workflow (what to run when)

```
# every change (seconds, host only)
make af-test

# parser/report modification
python3 -m unittest discover -s contrib/af_replay/tests -v
python3 -m unittest discover -s contrib/af_test/tests -v
python3 contrib/af_test/corpus.py -v
python3 contrib/af_test/stop_analysis.py       # AFSTOP halt evidence, exit 0 / 1 / 2

# before flashing a new zip
make af-test                                  # tier 1 must be green
# after the camera session
python3 contrib/af_report/af_report.py --copy ~/af_logs
python3 contrib/af_test/add_case.py --logs ~/af_logs/<newest> --name <date>-<scope>
python3 contrib/af_test/run_all.py            # tier 4 now includes the new session
```

The old field procedure, log table and report commands remain valid; v2 only adds the
automated tiers, acceptance numbers and the corpus mechanism.
