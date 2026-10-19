# af_replay

Offline replay and simulation of autofocus algorithms for the Canon EOS M ML project (crop_rec "AF drive (MPU)"),
so an algorithm idea can be compared against real focus curves without a camera round trip.

Python 3, standard library only (numpy is not needed and not used).

```
python3 af_replay.py --logs <dir> [--trials N] [--report path] [--seed S]
python3 af_replay.py --selftest        # only the AFSWEEP parser test
```

`--logs` is searched recursively for `AFDRIVE.LOG`, `AFCALIB.LOG`, `AFPROBE.LOG`, `AFSPEED.LOG`, `MPUDRIVE.LOG`,
`AFSEARCH.LOG` and `AFSWEEP.LOG` (e.g. the `card*/` copies). Default 300 trials per defocus class, about 15 s.
The report goes to `REPORT.md` next to the script unless `--report` is given.

## What it does

1. **Parses** the logs and cuts the move lines (`ms phase cmd pos s g luma ...`) into same-direction series
   (positions are only comparable inside one, because every direction reversal adds an unknown offset).
2. **Fits** `B + A*exp(-(p-p*)^2/(2 sigma^2))` to s and to g for each series and reports the distribution of sigma and A/B.
   Also fits the FOCUS_DONE time model (`a + b*|n|`, fast mode and the ~510 ms timer mode), per-move overheads and the reading noise.
3. **Simulates** a lens (relative pulses, no position feedback, random reversal offset in +-R, move timing, noise,
   continuous moves with per-frame sampling and `stop()` latency) and runs six algorithms over a small camera API
   (`move`, `measure`, `begin`/`next_sample`/`stop`):
   A baseline (mirrors `afd_run`), B continuous sweep + stepped retrace, C continuous sweep + continuous retrace with stop on threshold,
   D model jump, E warm start, F predictive stop in the stepped climb.
4. **Reports** success rate (final s >= 90 % of the true peak), median/p90 time, moves and travel per algorithm and start-defocus class,
   plus sensitivity to R, stop latency, sweep speed, image lag, noise and curve family, and a recommendation.

## AFSWEEP.LOG

```
# AFSWEEP t=.. lens=".." zoom=x5 frame=42
# sweep k=1 cmd=600 speed=7 dir=1 [t=<ms of the send>]
S <t_ms> <frame> <s1> <s2> <s4> <g> <q> <band> <luma>
# done k=1 t=<ms of FOCUS_DONE> val=<hex> n=<samples>
```

Position from time: `pos = cmd*(t - t_start - lag)/(t_done - t_start)`; `t_start` is the `t=` of the sweep line if present, else the first S line;
`lag` defaults to 42 ms (`sweep_series(..., lag_ms=)`); `cmd` is negated when `dir=-1` and `cmd` is positive. `s1` is used as s.
`selftest_parser()` generates a synthetic sweep with a known peak (`synthetic_afsweep`), parses it and checks that the fitted peak position and
width come back; it runs at every start. When a real AFSWEEP.LOG is in the log folder its series are added to the fit statistics and the curve pool.

## Layout of `af_replay.py`

Part 1 parsers and fits; Part 2 `Params` (all model assumptions in one place), `Cam` (the simulated camera) and the algorithms;
Part 3 Monte Carlo and the report writer. To try a new idea: write `def algo_X(cam, P)` using `Ctl`, `retrace_chain`, `fine_and_retrace`, `sweep_find`
as building blocks, add it to `ALGOS`, rerun.

The model assumptions that are *not* measured (reversal range, sweep speed, stop latency, curve tails) are listed with their uncertainty
in section 5 of `REPORT.md`; treat the results as a ranking of structures, not as predicted camera numbers.
