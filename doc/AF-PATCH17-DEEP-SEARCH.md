# AF patch 17: deep refocus searches both ways; profile file never loses entries

**Date:** 2026-10-20 · **Base:** patch 16 (uncommitted, card `744b5e93`) · **Evidence:** `~/af_logs/af-20261020-030453`

## 1. Field result (EF50 + speedbooster)
- Profile ok twice: W50 137/139, floor 41 %, done word 0x100, honest 100 % → trusted, chunk 200, rc_l1 100, probe 200.
- **Refocus test 5/5, 3.3–6.1 s, 3–9 moves** (defocus 345/820/800 from the profile) — best result so far.
- **Auto refocus near → far failed** (user: "small steps in between, giving up before the ballpark").
  Hunt episode at AFDRIVE.LOG lines 20003–20368: focus was ~+1400 pulses away.
  1. Attempt 1: probe flat → coarse 1F scan `dir=-1`, 6 × 300 → `# sweep dir=-1: flat after 6 chunks … no other way`
     → nopeak. **The "flat cap" rule ended the search without trying the other direction.**
  2. Attempt 2 `full_b_alt`: tracked probe said `g_up` (+1) but the hunt forced `dir=-1` again — the direction attempt 1
     had just searched — 7 × 200 at 14 → "flat … no other way" again.
  3. Attempt 3 (after a 5.3 s backoff): sweep +1 found the peak at +1392 in 6 chunks, 5.8 s.
  Total 31.5 s, 10 060 pulses of travel, versus ~6 s with the right direction.
- **AFLENS.TXT lost the EF-M 18 mm entry**: before the session it held the EF-M line; after the EF50 profile it holds only
  the EF50 line. `afpr_load` logs nothing, so whether the load failed at init is unknown.

## 2. Changes
**H1 — no "flat, no other way" in deep searches.** In `afd_sweep_find`, the flat cap (`# sweep dir=…: flat after N
chunks …, no other way`) no longer ends the search: after the first direction comes up flat without a peak, the other
direction is always searched (existing "other way" path), unless that direction is a known end stop (range memory /
`afd_endstop_dir`). Applies to the coarse 1F scan and to the 07/14 sweep. Log `# sweep flat dir=… -> other way (H1)`.

**H2 — the hunt's alternate leg really alternates.** Record the direction each AF run actually searched first
(`afd_last_search_dir`: the coarse scan direction if there was one, else the first sweep direction). `full_b_alt` (and any
"reversed" leg) forces `-afd_last_search_dir` of the previous attempt, not a fixed/alternating value. Log
`# hunt alt dir=… prev_search_dir=…`.

**H3 — sweep range scales with the chunk.** With a trusted lens and a larger chunk (patch 15), the per-direction chunk
count stays `AFD_SW_MAX_CHUNKS` (12) instead of shrinking to keep 1200 pulses: range = 12 × chunk (2400 at chunk 200).
EF-M unchanged (12 × 100). Coarse 1F unchanged in chunk count.

**P1 — the profile file never loses entries.** `afpr_load` logs `# PROFILE load path=… size=… entries=… bad_lines=…`
(buffered and written to AFDRIVE.LOG at the first flush). `afpr_save` first re-reads the file on the card and keeps every
entry whose key is not in the RAM table (so a failed load can never erase stored profiles). The "Forget lens profiles"
action is the only way to remove entries. The Lens-profile menu shows the number of stored and built-in profiles.

## 3. Field script
EF50: auto refocus near → far and far → near, several times; expect one run that sweeps one way, then the other, and lands
(`# sweep flat dir=… -> other way (H1)`), no `no other way`. Check `# PROFILE load … entries=` after boot and that
AFLENS.TXT keeps both lenses after a new profile. EF-M: profile at 18 and 55 mm (patch 16 return fix still unverified).
