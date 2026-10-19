# AF field-log regression corpus

Real camera sessions and synthetic sessions, replayed on the host as regression tests. The
suite never needs a camera: an empty corpus is skipped, each case is replayed with the real
tools, and the curated 2026-10-19 dataset below runs as **tier 4** of
`python3 contrib/af_test/run_all.py` / `make af-test`.

```
python3 contrib/af_test/corpus.py -v       # replay only the corpus
python3 contrib/af_test/run_all.py         # all offline tiers, including the corpus
```

Each case is replayed with

```
python3 contrib/af_report/af_report.py --logs <case> --all --no-state   # human verdicts
python3 contrib/af_replay/af_replay.py --logs <case>                    # parser + curve fits
```

plus a raw-log fact check (below), so a change in the parser, the curve fit or the report
verdicts is caught against real data.

## Directory convention

```
contrib/af_test/corpus/
    <case-id>/                     a single case
        AFDRIVE.LOG                one or more AF logs as written by the camera
        AFRTEST.LOG                (any of AFDRIVE / AFRTEST / AFBENCH / AFSWEEP / AFSTOP /
        AFBENCH.LOG                 AFREV / AFCALIB / AFSPEED / AFSEARCH / MPUDRIVE /
        AFSWEEP.LOG                 AFPROBE)
        expected.json              regression expectation (see below)
    real-20261019/                 a *session group*: one directory per case
        card18-refocus/
        card17-refocus/
        card17-bench/
        card8-sweep/
        card8-stop/                AFSTOP: 8 stop trials, 2 runs
        card9-stop/                AFSTOP: 4 stop trials, s2/s4/band all zero
```

Both `corpus/<case>/` and `corpus/<group>/<case>/` are discovered; a directory with logs or
an `expected.json` directly inside is a case, anything else is treated as a group.

`<case-id>` should sort usefully: date first, then scope, e.g.
`2026-10-20-18mm-refocus` or `2026-10-20-35mm-50mm-bench`.

Crash/ASSERT logs are deliberately **not** copied: `af_report --all` treats every crash log
on the card as new, so a copied crash log would fail the case forever.

## The curated 2026-10-19 dataset (`real-20261019/`, ~193 KB)

Provenance: copies of the EOS M card from the 2026-10-19 field session, preserved in the
Claude scratchpad (`.../scratchpad/card18/20261019-144447/ML/LOGS`, `card17/20261019-143547`,
`card8`). Each `expected.json` records the source directory, the capture date, the build
(`pre-task-6`), and the **sha256 of every copied log**, so the fixture cannot silently
change; the test fails with a `provenance:` error if a log is edited or replaced.

| Case | Logs | What it locks (from the raw logs) | What a failure means |
|---|---|---|---|
| `card18-refocus` | AFDRIVE + AFRTEST (card18) | Latest Refocus test: `ok=3/5`, `false_trig=0`, `runs_total=4`, final_pct `82,92,83,96,98`; ref_s `97338 -> 84320 -> 70812` (erosion); trial `def=400` never triggered but counted ok (`98%` of the eroded ref); the `+60` local search accepted `s0=82352` = 80.8% of the previous landing `101880`; 51 fast / 6 timer FOCUS_DONE (10.5% timer) | Parser/curve-fit changed on real data; report verdict changed; the known 3/5 baseline moved (expected only after a new post-patch fixture is added); the local-search false "in focus" or reference erosion was hidden/removed without new data |
| `card17-refocus` | AFDRIVE + AFRTEST (card17) | Previous Refocus test: `ok=2/5`, `false_trig=0`, `runs_total=3`, final_pct `88,95,77,91,87`; ref_s `115624 -> 94755 -> 85927 -> 76595`; `def=150,400` never triggered; 1 `nopeak` abort in 6 AF runs; 42 fast / 6 timer moves | Same as above; also checks the documented 115k -> 77k reference-erosion claim from the handover |
| `card17-bench` | AFBENCH (card17) | B07 `6/6` pct `100-108`, B14 `4/6`, B1F `5/6`; **positive-defocus trials barely defocus** (s_def 94.3-106.5% of the reference) while negative ones do (28.9-34.9%) | Bench parsing/table changed; bench regression (B07 no longer 6/6); the bench loses its negative-defocus coverage |
| `card8-sweep` | AFSWEEP (card8) | The only real sweep log: 16 chunks, 183 samples, 6/16 in the 0x0400 timer mode; fitted s width sigma_s 37-239 pulses (median 123) across chunks; 11 sweep series, 9 usable fits | AFSWEEP parser/time->position mapping changed; sweep geometry (sigma, timer-mode share) regressed |
| `card8-stop` | AFSTOP (card8) | **Stop-halt evidence:** 2 runs x 4 planned stops (150/250/350/450 ms), 8/8 trials NOT halted; sharpness keeps changing for 300-500 ms after `t_stop`; least-squares post-stop slope 8.3-62 %/s, all excursions in the commanded direction; even the 350/450 ms stops do not freeze s | The AFSTOP parser/classifier changed; a stop that used to coast now looks frozen (which would invalidate the C/G/predictive-braking decision); the fixture was edited |
| `card9-stop` | AFSTOP (card9) | **Stop-halt evidence, early stops:** 1 run x 4 planned stops (80/80/160/160 ms), 4/4 trials NOT halted; the early stops show the largest change (68-171 % delta, 87-406 %/s slope); `s2/s4/band` are zero in every full-format sample (trimmed build), so `zero_columns` locks that shape and s1 carries the classification | Same as card8; also that the 10-column-but-trimmed sample format still parses and classifies |

Two important notes that are part of the fixture, not bugs:

1. **Historical fixture, not a post-patch expectation.** `expected.json` records what the
   *old build* did. Each refocus case also carries a `post_patch_expectation` block marked
   `"status": "TARGET, not current"`: after the anchored-reference fix (task-6,
   `AFRT_REF_FLOOR_PCT=95`) card18 becomes 2/5 (trial 5 is `69712/97338 = 71.6%`, should
   trigger and fail), so `ok >= 4/5 with all 5 trials triggered` is the **target**. Add a
   new post-patch field session as a separate case instead of rewriting this one.
2. **Bench design weakness.** The AF bench's `+defocus` phase barely changes the scene
   (s_def 94-107% of the reference), only `-defocus` really defocuses. The bench therefore
   tests recovery from negative defocus only; the v2 protocol doc flags this.

## expected.json

Every key is optional, but at least one check must be present. Checks are limits, not
equalities (except lists / `of`), so a case keeps passing while the code stays at least as
good as the recorded baseline.

```json
{
  "name": "real-20261019-card18-refocus",
  "historical_fixture": true,
  "session": "2026-10-19",
  "notes": "what this case is and why the numbers matter",
  "provenance": {
    "source_dir": ".../scratchpad/card18/20261019-144447/ML/LOGS",
    "copied": "2026-10-19",
    "build": "pre-task-6 ...",
    "files": {"AFDRIVE.LOG": {"source": "...", "bytes": 26906, "sha256": "..."}}
  },
  "verdicts": {
    "must_contain": ["AF drive NOT OK", "Refocus test: only 3/5 ok"],
    "must_not_contain": ["CRASH", "HUNTING"]
  },
  "afdrive": {"min_runs": 7, "min_success_pct": 100, "max_median_ms": 5900},
  "rtest":   {"ok_at_least": 3, "of": 5, "false_trig_max": 0},
  "bench":   {"algo": "B", "min_ok": 5, "of": 6},
  "replay":  {"min_runs": 7, "min_moves": 67, "min_series": 1,
              "min_curve_fits": 0, "min_sweeps": 16, "min_sweep_samples": 183},
  "facts": {
    "drive": {
      "results": 7, "aborted_total": 0,
      "fast_moves": 51, "timer_moves": 6,
      "timer_share_pct": {"min": 10.0, "max": 11.0},
      "aref_new_ref": [101420, 78001, 79418, 80285, 72947, 72466, 73361, 16564, 17718],
      "local_in_focus": [{"s0": 82352, "s": 83863, "last_landing": 101880,
                          "pct_of_last_s0": 80.8, "pct_of_last_s": 82.3}]
    },
    "rtest": {
      "summary": {"false_trig": 0, "ok": 3, "of": 5, "final_pct_med": 92, "runs_total": 4},
      "final_pct": [82, 92, 83, 96, 98],
      "ref_s": [97338, 84320, 70812],
      "ref_s_monotonic_decrease": true,
      "triggered_trials": 4,
      "never_triggered_def": [400],
      "trials": [{"defocus": 60, "s_def_pct": 82, "trig_ms": 1269, "runs": 1,
                  "moves": 3, "final_pct": 82, "ok": 0, "ref_s": 97338}]
    },
    "bench": {
      "defocus": [150, -150, 400, -400, 800, -800],
      "algos": {"B07": {"n": 6, "ok": 6, "pct": [103, 100, 100, 101, 100, 108],
                        "pct_min": 100, "pct_max": 108, "aborted": 0,
                        "summary": {"n": 6, "median_ms": 4131, "ok": 6}}},
      "s_def_pct_positive_min": {"min": 90, "max": 110},
      "s_def_pct_negative_max": {"max": 60}
    },
    "sweep": {
      "sweeps": 16, "total_samples": 183,
      "samples": [4, 16, 13, 13, 14, 10, 15, 8, 11, 16, 7, 10, 15, 12, 8, 11],
      "cmd": [600, -600], "speed": ["7", "7", "1f", "1f"],
      "timer_mode_sweeps": 6,
      "fitted_sweeps": {"min": 8, "max": 16},
      "sigma_s_median": {"min": 60.0, "max": 200.0}
    }
  },
  "post_patch_expectation": {
    "status": "TARGET, not current",
    "rtest": {"ok_at_least": 4, "of": 5, "all_trials_triggered": true},
    "note": "..."
  }
}
```

Check meanings:

- `verdicts` — substrings of the whole `af_report.py` output, usually the `== Verdict` lines.
- `afdrive` — `== AF drive runs`: run count, success %, median time of all runs.
- `rtest` — last `# RTEST ... ok=k/n false_trig=f` summary.
- `bench` — one row of the `== Bench` summary table.
- `replay` — minimum shape the real logs must still parse into with `af_replay.py`:
  runs, measured moves, same-direction series, usable curve fits, sweep chunks and samples.
- `facts` — raw-log facts, derived by `contrib/af_test/corpus.py` directly from the logs
  (not from the report text): Refocus test per-trial `final_pct`, trigger/no-trigger state,
  `ref_s` erosion sequence, auto-refocus `new_ref` drift, FOCUS_DONE fast/timer split,
  local-search acceptances (`# local ... last=`), bench rows and positive/negative defocus
  strength, AFSWEEP chunk/sample/width statistics, and AFSTOP stop-halt rows
  (`files, sessions, trials, planned_stop_ms, halted_trials, continued_trials,
  median_abs_slope_pct_s, zero_columns, rows[{card,session,k,at,dir,t_stop,t_done,
  s_at_stop,s_end,delta_pct,post_stop_slope_pct_s,max_excursion_pct,halted,continued}]`).
  A `{ "min": ..., "max": ... }` object is a range, a list is an exact sequence, a dict is
  a nested object. The stop facts come from `contrib/af_test/stop_analysis.py`; a case that
  carries an `AFSTOP.LOG` but yields no stop trials fails (`no stop trials could be
  parsed`).
- `provenance.files[*].sha256` — the fixture's exact content; a mismatch is a failure
  (re-copy the log deliberately and update the hash if a case is intentionally replaced).
- `post_patch_expectation` — a **target**, never enforced by the corpus run; reviewed after
  a fix is flashed and then replaced by a new post-patch case.

`corpus.py` also cross-checks that the raw AFRTEST facts and the `af_report` text agree
(`ok`, `false_trig`, trial count); a fixture that hides a parser regression behind a
hand-written expected.json fails with a `... disagree` error.

## Adding a case after a field session

0. For an AFSTOP stop test, review the analyzer output first and keep the historical
   classification in `facts.stop`:
   `python3 contrib/af_test/stop_analysis.py <case>/AFSTOP.LOG`
   (`halted=0` is the expected "stop does not halt the move" evidence; do not "fix" it
   unless a new build really freezes the lens, and then make that a new post-fix case).
1. Mount the card (or copy the logs anywhere) and run:

   ```
   python3 contrib/af_test/add_case.py --logs /Volumes/EOS_DIGITAL --name 2026-10-20-18mm-refocus \
       --build "magiclantern-crop_mood.2026Oct19.EOSM202.zip"
   ```

   This copies the known AF logs and writes a **baseline** `expected.json`: report checks,
   raw-log facts, replay minimums, sha256 provenance, and a `post_patch_expectation`
   template (marked TARGET, not enforced).

2. Review and tighten `expected.json`:
   - `rtest.ok_at_least` / `afdrive.min_success_pct`: the target, not merely today's number.
   - add `must_not_contain` for the failure the session exposed (e.g. `"Refocus test: only 3/5 ok"`).
   - keep `facts` as the exact historical content unless the logs are wrong.
   - write the target in `post_patch_expectation`, not in `facts`.
3. Run `python3 contrib/af_test/corpus.py -v` and `python3 contrib/af_test/run_all.py`.
4. Keep the folder small (one AFDRIVE + AFRTEST + AFBENCH + AFSWEEP when present). Do not
   commit whole card dumps, AFDIAG logs or crash logs.

A synthetic session from `synth_logs.py` can be dropped in as a case too: its
`expected.json` already carries the check keys at the top level (used by the suite's own
corpus-mechanism tests).
