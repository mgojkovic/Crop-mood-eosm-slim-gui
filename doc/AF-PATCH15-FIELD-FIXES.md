# AF patch 15: field fixes from the patch-13 session + profile-driven AF + shared profile DB + test logging

**Date:** 2026-10-20 · **Base:** patch 14 (lens profile) · **Evidence:** `~/af_logs/af-20261020-015523` (patch 13, build `24d6ad6f`)

## 1. Patch-13 field result
Refocus tests **10/10** (55 mm 6.0–10.3 s, EF50 5.8–13.4 s), all landings 97–103 %. Not faster than patch 12.
EF-M never trusted (correct). EF50 trusted 3×, lost it 5× (`why=fast`): short 6–10-pulse moves genuinely finish
< 60 ms on the EF50. Video recording on the EF50: could not refocus from very far to very near.

## 2. Bugs found in the logs (fix first)

**F1 — missed peak at the probe→sweep hand-off** (AFDRIVE t=361107, EF50, 39 moves / 12.7 s instead of ~8):
the tracked probe rose (s 37k → 50k, `g_up`), the first sweep chunk's **first sample** was the peak (211k), and the
found-guard `gint = imax > 0 || k > 0` rejected it (k = 0). The sweep went 12 chunks away, then 12 back.
Fix: when `afd_sweep_find` continues a measured rise in the same direction (tracked probe used, same `d`, no jump),
the direction's first sample is a continuation → treat as `k > 0` (flag set by `afd_run_sweep`, consumed once).

**F2 — position bookkeeping after a stop** (same run): `afd_sweep` computes `end_t = stop_ms + AFD_SW_COAST_MS` when
the stop callback fired, so chunks after the peak log `moved=0` while the lens moved the full 100 (stop never halts:
12/12 stop trials). `afd_pos` was ~1000 pulses off; the "other way from pos0" jump then went to the wrong place.
Fix: ignore `stop_ms` for `moved` (use `t_stab`/`t_exp` as for an unstopped chunk; trusted honest done → `len`);
keep end-stop handling as is. Log `stopped=1` on the chunk line.

**F3 — trust `why=fast` on short moves:** judge "fast" only for `|len| >= AFD_P13_TRUST_MIN_LEN` (50).

**F4 — deep blur on a new scene runs Local first, then waits a 10 s backoff** (recording, hunt t=457370:
local at s≈18k → full_b 16 s later → sharp 27.7 s after the begin). The patch-12 Local skip needs a reference; a new
scene has none. Fix: (a) Local skip also when `s0 <= floor_abs`, with `floor_abs = P_profile · floor_pct · 1.3` from
the lens profile (patch 14) when one exists, else when the first two Local probes and s0 are all within ±3 %
(flat floor) → B immediately (already the "wide still flat" path; make it skip the wide probe when no ref);
(b) hunt backoff after a failed attempt while recording: first retry after 2 s (not 10 s), then the existing schedule.

**F5 — scene change during a run grinds to the timeout** (AFDRIVE t=431403: focus found at 106k, re-catch at 101k vs
95.6k thr, then luma 110 → 7; lowluma waits ~1.2 s per sample until the 19.5 s timeout). Fix: track luma vs the run's
start luma; if it falls below 50 % or rises above 200 % for 3 consecutive samples → abort the run with
`# scene changed luma0=… luma=…` (`afd_aborted`, reason "scene") so the hunt restarts on the new scene. Inside
`afd_rc_rest` never use the lowluma wait.

**F6 — settle check in a changing scene:** `afd_rc_rest` took 11 frames (1.8 s) because the scene itself drifted.
Fix: `AFD_RC_REST_N` 12 → 6 while recording; compare consecutive frames on s normalised by luma (s·luma0/luma).

## 3. Profile-driven AF parameters (user request: "some lenses need bigger jumps, some smaller")
With a profile for the current key (and the define `AFD_P15_PROFILE_PARAMS`), derive per run and log one line
`# params source=profile W50=… fine=… probe=… chunk=… rc_l1=… local_probe=… half=…`:
- `fine = clamp(round(0.7·W50), 6, 30)` (patch 14), Local probes ±2·fine, top half step `max(fine/2, 6)` (unchanged formulas).
- Probe chunk (`AFD_COARSE`): `clamp(4·W50, 60, 200)`.
- Sweep chunk at 07: 100 on the EF-M (the timer); on a **trusted** lens `clamp(4·W50, 100, 200)`.
- Re-catch first chunk `AFD_RC_L1`: 30 on the EF-M (below the timer threshold); trusted lens `clamp(2·W50, 30, 100)`.
Without a profile: today's values (`source=default`).

## 4. Shared lens profile database (user request)
- **Repo file** `data/af_lens_profiles.txt` (same line format as `ML/SETTINGS/AFLENS.TXT`, plus `n=` sample count and
  `src=` contributor tag), the source of truth for shipped profiles.
- **Tool** `contrib/af_test/lens_db.py`: `import <AFLENS.TXT|card path>...` merges profiles into the repo file (same key →
  median W50/floor, n summed, outliers > 2× median flagged, not merged), `list`, `export-header` → generates
  `modules/crop_rec/af_lens_db.h` (a `static const` table compiled into the module). Unit tests.
- **Camera lookup order:** own measured profile (card) > built-in (shipped) > default. A built-in hit counts as
  "has a profile" for the auto-profile trigger (no automatic lens motion for known lenses); "Profile this lens now"
  still measures and stores an own profile. Log `# params source=card|builtin|default`.
- **Collection:** `field_session.py --copy` also copies `ML/SETTINGS/AFLENS.TXT`; README section "Sharing lens profiles".

## 5. All logging on by default (user request: no option, no build flag)
AF diag log defaults to ON, every AF log file is enabled by default, and the in-recording note buffer (`afd_note`,
2 KB today → recording runs are lost) is raised to 16 KB, allocated at module init (never during recording), flushed
after recording as now. No new setting or compile flag; the existing AF diag log menu entry can still turn it off.
