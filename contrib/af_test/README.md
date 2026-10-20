# af_test — offline test harness for EOS M AF

Everything here runs on the host, with Python 3 standard library only: no camera, no
memory card, no numpy/pytest. It covers the log parsers, the focus-curve fit, the lens
simulator and algorithms A-F in `contrib/af_replay/af_replay.py`, and the report/verdict
logic in `contrib/af_report/af_report.py`.

```
python3 contrib/af_test/run_all.py            # every tier, PASS/FAIL summary, exit code
make af-test                                  # same thing through the top-level Makefile
python3 contrib/af_test/run_all.py --verbose  # full unittest + CLI output
```

About 14 s on a laptop (tier 4 replays the real logs). Expected tail:

```
  PASS parser self-test (af_replay --selftest)         0.2s  AFSWEEP parser self-test: ok
  PASS unit + simulator + tool suite (2 test dirs)    12.6s  run=84 passed=84 failed=0 errors=0 skipped=0
  PASS synthetic-log CLI pipeline                      1.0s  6 runs replayed, 2 verdict(s): ...
  PASS field-log regression corpus                     0.7s  6 case(s): 6 pass, 0 fail, 0 skip
  ...
  RESULT: PASS
```

## Tiers

| Tier | What it verifies | Command |
|---|---|---|
| 1 parser self-test | AFSWEEP synthetic sweep -> parse -> fit recovers peak/sigma | `af_replay.py --selftest` |
| 2 unit + simulator + tools | parsers, curve fit, simulator determinism/physics, algorithms A-F, af_report parsing, corpus mechanics, CLI smoke, AFSTOP halt analyzer | `python3 -m unittest discover -s contrib/af_replay/tests -v && python3 -m unittest discover -s contrib/af_test/tests -v` |
| 3 synthetic-log CLI pipeline | generator -> `af_replay --logs` -> `af_report --logs` end to end | inside `run_all.py` |
| 4 field-log corpus | 6 curated 2026-10-19 sessions (including the two AFSTOP stop tests) replayed against `expected.json` limits + raw-log facts + sha256 provenance; skips cleanly when empty | `python3 contrib/af_test/corpus.py -v` |

## Files

| File | Purpose |
|---|---|
| `synth_logs.py` | Generates realistic AFDRIVE/AFRTEST/AFBENCH/AFSWEEP/AFSEARCH logs with known ground truth (`expected.json`). Profiles: `healthy`, `slow`, `hunting`. |
| `run_all.py` | One-command orchestrator for all tiers with PASS/FAIL counts. |
| `field_session.py` | One-command **post-field-session** checker: finds the newest AF logs on the card (or a copy), prints the Refocus-test integrity verdict (`notrig`/`valid`/`false_pass`), the stop-test verdict, drive timings (`search_ms`/`epilogue_ms`), bench rows, anchor/ref-max lines and crashes; `--copy` first copies the session to `~/af_logs/af-<timestamp>/`. Exit 0 pass, 1 hard failure, 2 incomplete/old build. |
| `corpus.py` | Field-log corpus convention, replay/check logic, raw-log fact extraction (`facts`), sha256 provenance (see `corpus/README.md`). |
| `stop_analysis.py` | AFSTOP halt analysis: parses `AFSTOP.LOG`, per-trial post-stop delta/slope/classification, summary + exit code (see "Stop-halt analysis" below). |
| `add_case.py` | After a field session: copy the logs into the corpus and write a baseline `expected.json`. |
| `corpus/real-20261019/` | Curated real field logs: card18/card17 Refocus tests, card17 bench, card8 AFSWEEP, card8/card9 AFSTOP stop tests (~193 KB). |
| `../af_replay/tests/` | The parser/simulator/algorithm unittest suite (stdlib `unittest`), including real-corpus fact and tamper tests. |
| `tests/` | Tool-level unittest suite: AFSTOP parser/classifier on the real logs, a synthetic halted trace, the CLI exit codes and the stop corpus tamper checks. |

## Synthetic session

```
python3 contrib/af_test/synth_logs.py --out /tmp/af_synth --profile healthy
python3 contrib/af_report/af_report.py --logs /tmp/af_synth --all --no-state
python3 contrib/af_replay/af_replay.py --logs /tmp/af_synth --trials 50 --report /tmp/REPORT.md
```

Profiles:

- `healthy` — 6/6 AF runs land, Refocus test 5/5, false_trig 0 → `AF drive OK`.
- `slow` — 5/8 runs land, median > 5 s → `AF drive NOT OK`.
- `hunting` — Refocus test 2/5, false_trig 2 → `HUNTING` verdict.

The generator writes the formats actually produced by `modules/crop_rec/crop_rec.c`,
including the shipped AFSWEEP sample line (`S <t_ms> <s1> <g> <luma>`) as well as the
documented longer AFSWEEP format.

## Real field-log corpus

`contrib/af_test/corpus/real-20261019/` replays the 2026-10-19 field session: card18 and
card17 Refocus tests (3/5 and 2/5, reference erosion 97k->71k / 116k->77k), card17 AF bench
(B07 6/6) and the only real AFSWEEP.LOG (16 chunks, 183 samples), plus the two AFSTOP stop
tests (card8: 8 trials, card9: 4 trials; 0/12 classified halted). Each case has
`expected.json` with report checks, exact raw-log facts (`final_pct` sequences, `ref_s`
erosion, never-triggered defocus, FOCUS_DONE fast/timer split, bench rows, sweep geometry,
stop-halt classification), replay minimums, sha256 provenance and a `post_patch_expectation`
target that is **not** enforced for the refocus cases. See `corpus/README.md`.

## Stop-halt analysis (AFSTOP)

Does a `(9,3,1)` stop halt a running `(9,1)` relative move? This is the question the
camera's **AF stop test** (AF diag -> mode 5) exists to answer, because algorithms C/G
(and any predictive braking) assume the stop works. The real 2026-10-19 evidence says
it does **not**:

```
python3 contrib/af_test/stop_analysis.py            # analyze the two corpus AFSTOP cases
python3 contrib/af_test/stop_analysis.py AFSTOP.LOG # analyze one log or a directory
python3 contrib/af_test/stop_analysis.py --json     # rows + summary as JSON
```

Output: one row per stop trial (`card, session, k, dir, at, t_stop, t_done, s@stop,
s_end, delta%, post-stop slope %/s, max excursion %, halted, class`) plus a summary.
Current real data: **12/12 trials continued** (0 halted), median post-stop |slope|
~49 %/s, min 8.3 %/s, all 12 excursions in the commanded direction.

- `halted=0` means the sharpness kept changing after `t_stop`: the stop did not freeze the
  lens, so a stop-based brake (C, G, predictive landing) cannot be relied on and must not
  be ported as if `(9,3,1)` worked.
- A trial is `halted` when the post-stop least-squares |slope| is <= 2 %/s **and** the
  max excursion is <= 1 % (thresholds are module constants; the real traces clear them by
  a wide margin).
- Exit code `0` = every trial classified not halted (expected). Exit `1` = at least one
  trial appears frozen, i.e. the evidence changed and the C/G assumption must be
  re-examined. Exit `2` = no complete stop trial could be parsed.
- The analyzer accepts both the documented 10-token sample line
  (`S t frame s1 s2 s4 g q band luma`) and the shipped 4-value line (`S t s1 g luma`).
  card9's full-format file has `s2/s4/band` zero in every sample; s1 is used for the
  classification and the zero columns are reported, not treated as an error.

## Sharing lens profiles

The camera measures the focus-peak width (W50) of each lens / zoom once and keeps it in `ML/SETTINGS/AFLENS.TXT`
(menu "Auto lens profile", "Profile this lens now", "Forget lens profiles"). Profiles from many cards are pooled in
`data/af_lens_profiles.txt` and compiled into the module, so a known lens needs no measuring run.

1. After a session, `python3 contrib/af_test/field_session.py --copy` copies the logs and `ML/SETTINGS/AFLENS.TXT`
   to `~/af_logs/af-<timestamp>/` and lists the `# PROFILE`, `# RT_DEFS` and `# params` lines.
2. Merge them: `python3 contrib/af_test/lens_db.py import --src <your-tag> ~/af_logs/af-<timestamp>`
   (also accepts an `AFLENS.TXT` file or a card mount). Same key (lens name, 10 mm bucket, LV zoom) -> median W50 / floor,
   `n` summed, raw samples kept (last 32). A sample more than 2x away from the known median is reported as `OUTLIER`
   (exit code 1) and not merged - re-measure that lens before trusting it.
3. `python3 contrib/af_test/lens_db.py list` shows the table; `python3 contrib/af_test/lens_db.py export-header`
   regenerates `modules/crop_rec/af_lens_db.h` (a test fails when the header is stale). Commit both files and rebuild.

On the camera the lookup order is: own measured profile (card) > built-in > default (`# params source=card|builtin|default`).
A built-in hit suppresses the automatic profile run; "Profile this lens now" still measures and stores an own profile,
which then wins. File format: `v1|name|mm|zoom|W50|floor_pct|done_word_hex|honest_pct|fine[|n=..|src=..|w=..|f=..]`.

## Adding tests

- Unit/simulator test: add `contrib/af_replay/tests/test_<area>.py`, inherit
  `common.TempDirTestCase` when a temporary log folder is needed. `common` inserts
  `contrib/af_replay` and `contrib/af_test` on `sys.path`, so `import af_replay` and
  `import synth_logs` just work.
- Tool test for `contrib/af_test/*.py`: add `contrib/af_test/tests/test_<tool>.py`
  (stdlib `unittest`, no shared helper needed; put `contrib/af_test` on `sys.path`).
  Both test directories are discovered by tier 2.
- A behaviour that needs a camera run: add it to the corpus after the session
  (`add_case.py`) instead of hand-writing assertions.
