# AF patch 13: wait less after an honest FOCUS_DONE, 3-point top, shorter Local climb

**Date:** 2026-10-20 · **Base:** `ec8d5e9` (patches 11-12, field 15/15) · **Module:** `modules/crop_rec/crop_rec.c`

## 1. Evidence

**AF pipe test, EF50 + speedbooster** (`~/af_logs/af-20261020-012116`, AFPIPE.LOG): 12/12 second chunks accepted at
X = −80/0/+80 ms; every pair returns to the same s (~57k) and ends at the same s per direction → all 200 pulses ran.
FOCUS_DONE val `0x100`, at 150–210 ms for 100 pulses at 07, and the s trace is flat after it → **done is the motion end**;
the code still waits `t_exp + pad` = 560 ms per chunk (the EF-M model).

**AF pipe test, EF-M 18-55** (`~/af_logs/af-20261020-012642`): a second chunk sent before chunk 1's ~510 ms timer done
(val `0x4e0400`) is sometimes refused (`0x1000`), sometimes ignored or reported done 60–120 ms later, and the lens loses
its position (later pairs start on the blur floor; pair 8 starts while still moving). **Never send a move on the EF-M before
the previous one reported done.**

**Patch-12 field runs** (`time_budget.py`, 30 Refocus trials, median 8.3 s): sweep 2.6 s, landing 2.9 s, Local 1.3 s,
session 0.73 s, epilogue 0.69 s. The two-sided top climb (8/18 landings) costs 1.3–1.7 s (5–6 half steps of ~250 ms).
Local still ends in "→ B" in 3/5 runs it attempts, twice after the full 8-step climb.

## 2. Changes (each behind a define, default on; log each decision)

### T1 `AFD_P13_DONE_TRUST` — end a chunk at the real motion end
Applies to tracked chunks (`afd_move_track`) and measured moves (`afd_move_ex` modelled travel wait).
- **Honest done:** FOCUS_DONE arrived, `!(val & 0x0400)` (no timer), `!(val & 0x1000)` (not refused).
- **EF-M rule (always):** with an honest done, drop the safety pad: the chunk may end at
  `max(done_ms + 2 frames, t_exp)` with `t_exp = AFD_SW_LAT_MS + len·mspp/10` (no `AFD_SW_TRACK_PAD_MS`).
  A timer done (≥ ~505 ms) is already past t_exp: unchanged.
- **Trusted lens (learned per run, kept for the lens name until the lens changes):** after 2 honest dones on moves of
  ≥ 50 pulses with `done_ms·10 < t_exp·6` (done before 60 % of the EF-M model), log
  `# done trust on lens="…" done_ms=… t_exp=…` and from then on end chunks at `done_ms + 2 frames` and skip the modelled
  travel wait in `afd_move_ex` after an honest done. Any timer or refused done on that lens turns trust off
  (`# done trust off why=timer|refused`). The EF-M never qualifies (its honest dones come late or via the timer).
- **Never** send the next command before the previous move's FOCUS_DONE (unchanged; the EF-M pipe test).
- For trusted-lens moves, the time→pulse model uses the observed done time: `moved = len` when done is honest
  (the move completed), so `over`/`pk` estimates are not inflated by the EF-M speed model.
- Log per tracked chunk: append `trust=0|1 end=pad|done|timer` to the existing `# sweep`/`# rc`/`# probe` line.

### T2 `AFD_P13_TOP3` — 3-point parabolic top (re-catch only, replaces the two-sided climb)
In `afd_land_top_rc`, when the settled landing value r is below 95 % of P (the gate stays):
1. `s+ = move(+h)`, `s− = move(−2h)` (h = `MAX(AFD_FINE/2, 6)`; each measured after a settled frame).
2. Vertex offset from x0: `δ = h·(s− − s+) / (2·(s− − 2r + s+))` if the denominator is < 0, clamped to [−h, h];
   otherwise (no curvature) the best of the three points.
3. Move from x0−h to x0+δ, measure s*. If `s* < 97 %` of max(r, s+, s−), move to the best measured point instead.
4. Log `# recatch top3 r=… s+=… s-=… delta=… final=…`.
Expected: 3–4 short moves (~0.8–1.0 s) instead of 5–6 (~1.5 s), and a better-centred landing.

### T3 `AFD_P13_LOCAL_CLIMB4` — Local gives up after 4 flat climb steps
`AFD_L_CLIMB_MAX` 8 → 4 for the "no peak within N steps → B" decision.

### T4 phase parsing
`time_budget.py` uses `# phase name=… t=…` markers when present (fallback: current inference).

## 3. Expected effect
EF50: −0.35 s × ~6–8 chunks ≈ −2 s per refocus. EF-M: −0.15 s per honest-done chunk (~half the sweep chunks, most rc
chunks) ≈ −0.6–1 s. T2 −0.5 s in ~40 % of landings. T3 −0.8 s in the Local-fails case. Target median ≈ 5–6 s (EF-M), 4–5 s (EF50).

## 4. Field script
Same three Refocus tests (55 mm, 18 mm, EF50) with AF diag log ON. Check `# done trust on` appears only for the EF50,
`end=done` / `end=pad` counts, `# recatch top3`, and no `0x1000` refusals or end-stop detections that did not happen before.

## 5. As built, `crop_rec.mo` md5 `24d6ad6fb8a77fec295b466453afc986` (zip `magiclantern-crop_mood.2026Oct20.EOSM202.zip`)

- **T1 hardened after review.** The review replayed real EF-M logs and found early honest dones (`0x4e0000` at 9–280 ms on
  100-pulse moves, e.g. 244 then 129 ms in a row) that would have taught trust to the EF-M. Trust is now learned only from
  the exact EF-adapter word `0x100`, never on a lens named `EF-M…`, from **3 consecutive** qualifying dones (any other
  honest done resets the count); a done < 60 ms or any other word turns it off (`# done trust off why=refused|timer|val|efm|fast`).
  The early exit itself also requires `val == 0x100`. On the EF-M only the 150 ms pad is dropped after an honest done
  (chunk still ≥ t_exp).
- Known, not changed: `pos_pk`/`over` still use the EF-M time→pulse model on a trusted EF50 (over is overestimated → the
  re-catch cap covers more; harmless).
- **T2** simulator (gate 95, 600 trials): equal or better strict success in 14/18 cells (≤ 1.3 points worse elsewhere),
  ~0.2–0.4 s faster median, better-centred landing.
- **Known gap pinned in tests:** 55 mm / realistic / defocus 150 is ~6 points below the patch-11 landing in the simulator
  (87 → 81 %); the field Refocus test was 5/5 there. Watch ±150 at 55 mm.
- **Build gotcha:** the zip name carries the build date — after midnight a new zip is written; always take the newest zip and
  compare `unzip -p <zip> ML/modules/crop_rec.mo | md5` with the built module. `make zip` exits 0 even if crop_rec.c fails.
