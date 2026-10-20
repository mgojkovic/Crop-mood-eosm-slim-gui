# AF patch 16: reliable lens profile on the EF-M (dropped long moves)

**Date:** 2026-10-20 · **Base:** `26e89ee` (patches 13-15) · **Evidence:** `~/af_logs/af-20261020-023259` (AFPROF.LOG, AFRTEST.LOG)

## 1. Evidence
Five profile runs on the EF-M 18-55 (18 mm ×3, 55 mm ×2):
- 3 failed with `x0_return_pct` 17–21 %: after the −10/−10/−20/−40/−80 side scan the return +80, +80 left the lens on
  the blur floor, and the + side scan then found the peak at **+80** (96–100 %): **one 80-pulse return move was dropped**.
- The 2 "ok" runs ended at `x0_end_pct` 24 / 29 %: the final −80, −80 return also lost a move; `ok` only checked the
  position counter (`afd_pos == x0`), not the picture.
- Consistent with the EF-M AF pipe test: moves of 40–100 pulses can be ignored (a "done" 60–120 ms after the send, no
  0x1000) even when sent after the previous move's done; moves ≤ 30 pulses complete honestly (done ≈ 153 + 1.33·|p| ms).
- The side scans themselves (same-direction moves of 10/10/20/40/80 starting from a verified x0) look consistent; W50 from
  runs with a good `x0_return_pct` is valid (18 mm: 30 and 40).

## 2. Changes (crop_rec.c, profile run `afpr_run` and the move layer)

**P1 — short moves on untrusted lenses.** In the profile run, when the lens is not trusted (`!afd_p13_trusted()`), every
move is split into measured sub-moves of at most 30 pulses (sample only at the listed offsets 10, 20, 40, 80, 160 and at
x0). Trusted lenses (EF50) keep the current moves.

**P2 — closed-loop returns.** After each return to x0 (mid and end), measure the settled s. If s < 85 % of P, recover:
walk ±fine steps (≤ 30 pulses) up to ±60 pulses around the expected x0, keep the best point, stop at ≥ 90 % of P
(`# PROF_RECOVER where=mid|end tries=… s=… pct=…`). A mid recovery that does not reach 85 % fails the profile
(`why=x0return`); `ok` additionally requires the final settled s ≥ 85 % of P (`why=x0end`) — never report ok with the
lens off focus. Deadline for the probe phase: landing end + 12 s.

**P3 — dropped-move detector (log-only, all AF moves).** In `afd_move_ex` and `afd_move_track`: on a lens with the EF-M
done word (`0x4e....`), for `|p| >= 40`, a done without 0x0400/0x1000 that arrives before 50 % of `153 + 1.33·|p|` ms →
log `# move suspect_dropped p=… done_ms=… val=…` and count it (`suspect_dropped=N` on the RESULT line). No behaviour
change yet — this gives the data to decide whether sweeps also lose chunks (the 11 % "done < 150 ms" chunks).

## 3. Field script
Profile the 18-55 at 18 mm and at 55 mm ("Profile this lens now" if the zoom is not detected), then the EF50. Expect
`ok=1` with `x0_return_pct` and the end value ≥ 85 %, few or no `PROF_RECOVER`, and count `suspect_dropped` in the
Refocus-test runs.
