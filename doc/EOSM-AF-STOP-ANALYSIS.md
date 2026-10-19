# Does `(9,3,1)` stop halt a running `(9,1)` move? — recovered-log analysis

**Date:** 2026-10-19 · **Source:** real `AFSTOP.LOG` from the 2026-10-19 field session (recovered from the
Claude scratchpad; copies preserved by the test corpus work as `contrib/af_test/corpus/real-20261019/card8-stop/`
and `card9-stop/`) · **No camera was used; this is an offline analysis of recorded traces.**

Companion tools: `contrib/af_test/stop_analysis.py` (per-trial table, regenerates this evidence).

## 1. Why this question matters

The whole "fast AF" branch depends on it. If `(9,3,1)` halts a running `(9,1)` relative move, then
algorithm C (continuous sweep + stop-on-threshold) and G (predictive braking) are viable and beat B in
simulation (C 86%, G 90% vs B 85%). If it does not, C/G are dead, and **B's own sweep position model is
biased**, because `afd_sweep()` currently computes the travel end as `stop_ms + AFD_SW_COAST_MS` and the
peak position from that time.

The previous handover stated "`(9,3,1)` stop does not halt a `(9,1)` move" from the same session. The
recovered logs let us test that claim directly: the stop test records **per-frame sharpness** after the
stop command (`afw_log_samples`), not just timestamps.

## 2. How the traces were read

For every `# stop_plan k=.. at=..` block, the `S <t_ms> <frame> <s1> ...` samples that follow were split at
`t_stop` from the `# stop` line. For the samples at/after the stop, we compute:

- `s@stop` — first sample at or after the stop command,
- `s_end` — last recorded sample,
- `delta% = (s_end − s@stop) / s@stop`,
- `slope` — least-squares slope of `s1(t)` over the post-stop samples, in %/s.

If the stop worked, the lens would coast for at most `AFD_SW_COAST_MS` (~40 ms) plus the stop latency
(tens of ms), and then the static scene's sharpness would be flat: `slope ≈ 0`, `delta` within the
measured reading noise (~1%). Camera luma is constant in these blocks, so exposure/AE is not the driver.

## 3. Result: sharpness never freezes after the stop

12 stop trials across the two recovered files:

| card | k | planned stop (ms) | t_stop | t_done | s@stop | s_end | delta% | post-stop slope %/s |
|---|---|---|---|---|---|---|---|---|
| card9 | 1 | 80 | 91 | 506 | 6238 | 16923 | **+171.3** | **+230.7** |
| card9 | 2 | 80 | 126 | 502 | 10100 | 8014 | −20.7 | −95.5 |
| card9 | 3 | 160 | 200 | 504 | 7471 | 12624 | +69.0 | +51.1 |
| card9 | 4 | 160 | 161 | 507 | 11867 | 8260 | −30.4 | −147.3 |
| card8 | 1 | 150 | 154 | 510 | 21361 | 25047 | +17.3 | +35.4 |
| card8 | 2 | 250 | 289 | 508 | 21984 | 20688 | −5.9 | −19.3 |
| card8 | 3 | 350 | 363 | 509 | 22018 | 23297 | +5.8 | +33.3 |
| card8 | 4 | 450 | 483 | 503 | 21280 | 21605 | +1.5 | +29.0 |
| card8 (2nd run) | 1 | 150 | 161 | 523 | 18001 | 22248 | +23.6 | +55.7 |
| card8 (2nd run) | 2 | 250 | 255 | 544 | 19393 | 18040 | −7.0 | −20.0 |
| card8 (2nd run) | 3 | 350 | 380 | 501 | 20234 | 20643 | +2.0 | +58.7 |
| card8 (2nd run) | 4 | 450 | 469 | 506 | 18740 | 18550 | −1.0 | −8.3 |

- **0 of 12 trials show a halted trace.** The final tool (`contrib/af_test/stop_analysis.py`,
  slope-primary rule: halted iff |least-squares slope| ≤ 2 %/s AND |max excursion| ≤ 1 %) reports
  12/12 continued, all in the commanded direction; combined median post-stop |slope| **48.7 %/s**
  (min 8.3), median |max excursion| 14.8%. Endpoint delta alone is a trap: card8 session-1 k4 has a
  net delta of only +0.06% because the peak turns inside the window, yet its post-stop slope is
  +28.8%/s — which is why the classifier is slope-primary.
- The sign follows the commanded direction (`dir=+1` rises, `dir=−1` falls), i.e. the image keeps
  changing as if the lens were **still travelling**.
- The `# done t=..` values are all ~500–544 ms (the FOCUS_DONE timer), not the motion end — consistent
  with the handover's motion model.
- Example (card8, 400-pulse sweep, stop at 154 ms): `s1` is 21361 at the stop and keeps rising
  monotonically to 25047 at 540 ms, with luma 146→144. A halted lens would have frozen within ~1%.
  If the planned stop had worked, the rise would have ended by ~200 ms.

## 4. What this establishes and what it does not

**Established:** in every recovered stop trial, the picture kept changing well past the stop command; the
only consistent explanation is continued lens travel. `(9,3,1)` does not halt a `(9,1)` move.
**Caveats:** the test logs have no position feedback, so "continued travel" is inferred from sharpness
(static scene + constant luma). Short 100-pulse moves can also finish naturally around the same time, so
their post-stop tail is weaker evidence than the 400-pulse card8 trials and the early-stopped card9
trials. The dedicated stop test should use a long move (400 pulses) and start on a clearly sloped flank,
with samples for at least 500 ms after the stop; the new `# stop_check` line in patch 2 is designed to
make each new run self-interpreting.

## 5. Consequences for the roadmap

1. **C and G are not shippable.** Stop-on-threshold and predictive braking both assume the stop fires.
   The simulator already showed the cost: with a dead stop, C falls 86% → 65% and G 90% → 84%. Do not
   port them; keep B.
2. **B's sweep position model is biased, and this is a real, cheap next experiment.** In
   `afd_sweep()` (`crop_rec.c`), when a stop was sent the code takes `end_t = stop_ms + COAST` and
   computes `moved = (end_t − LAT) × 10 / mspp`, then `afd_pos` and the peak position from it. If the
   lens ignores the stop and travels the full chunk, `moved` is **underestimated**, so `afd_pos` and
   `pk` are wrong and the feedback landing starts from the wrong place. The retrace often recovers, but
   this is a plausible contributor to hard-scene B failures and to the soft landings that are currently
   blamed on the local search.
   - Candidate fix (to be tried as a separate, field-tested change, not in the reference patch): when a
     stop was sent, prefer the **s-stability end time** (`t_stab`, already computed in `afd_sweep`) over
     `stop_ms + COAST`; if no stable window is found, assume the **full chunk** (`t_exp`, `moved = len`).
     That is the correct model for both a working and a non-working stop, and it uses data already
     sampled.
   - Test: one field session with the current B and the stability-based model, same bench/Refocus set;
     look for shorter `travel`, fewer `# land flat -> resweep`, and a lower landing-position error in the
     sweep record. Do this **after** the reference patch has been validated, so the deltas are
     attributable.
3. **The stop test itself becomes a regression fixture.** The real traces are now in the corpus and
   `stop_analysis.py` flags a future build whose stops actually halt — which would reopen C/G and the
   position model.
4. **Do not use `(9,3,1)` for safety-critical abort.** It is still worth sending (better than nothing) and
   the engine does, but it must not be relied on to stop motion; chunking remains the real safety
   mechanism.

## 6. Reproduce

```
python3 contrib/af_test/stop_analysis.py contrib/af_test/corpus/real-20261019/card8-stop/AFSTOP.LOG \
                                          contrib/af_test/corpus/real-20261019/card9-stop/AFSTOP.LOG
```

The table above was produced by the same parsing rule (least-squares `s1(t)` slope over the samples at/
after `t_stop`); numbers can differ by a few percent if a different stop-sample window is chosen, but the
sign and the "0/12 halted" conclusion are robust to the window.
