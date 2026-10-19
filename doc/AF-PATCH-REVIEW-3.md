# AF patch review 3: final verification — patch 5 must-fix closure

**Status:** targeted verification review, **offline (no camera)**. Read-only; no
edit of `crop_rec.c`, no `make`.
**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` · **Base:** HEAD `043d914`.
**Artifact reviewed:** `/tmp/af_patch5_full.diff`, md5
`84c81956134bb53caf937a75230736be` — the full `modules/crop_rec/crop_rec.c`
diff vs HEAD (patches 1–5 + the task-12 `(9,0x17,32)` feed logging). Verified
identical to `git diff modules/crop_rec/crop_rec.c` at review start; all line
numbers below refer to that snapshot applied on HEAD (reconstructed under
`/tmp/snap5`). The working tree was not touched during this review.
**Build evidence:** I cannot rebuild (build owner), but the built artefact in the
tree matches the Lead's claim exactly: `modules/crop_rec/crop_rec.mo` md5
`ebb45f8b53ba4cd7b53036ca5a8a4e8f` (mtime 16:09, after patch 5).
**Host suite:** re-ran `python3 contrib/af_test/run_all.py` myself → 81/81 unit
tests, 4/4 tiers, 6/6 corpus cases, `RESULT: PASS`.

---

## 0. Verdict

**Safe to flash. No blocking findings.** All four review-2 must-fixes (MF1–MF4)
are closed by code change, and I verified each with line-level tracing plus two
small state models and a real-data check:

- **MF1 closed:** the anchor is seeded only when it does not exist; once valid it
  is raised only by a sharper *settled* `med`. A model of the exact
  seed/raise/soft/reset logic shows the stale-ref-max re-raise is gone, and the
  confirmed-soft reset lowers `afd_ref_max_s` with the anchor
  (115k → 95k, then constant; no re-raise).
- **MF2 closed:** seed metrics are `MAX(old_em, settle emax)` / `MAX(old_g, g)`,
  and the clamp restores metrics only when `anchor_em > 0`. The bucket/clamp model
  with a zero-metric seed now keeps `aref_ref_em` ≥ the settle sample's edge (gate
  4 cannot lock from a zero anchor).
- **MF3 closed:** `afd_ref_max_reset("rtest")` + `aref_anchor_clear("rtest")` run
  before and after `aref_test_run()`; HOLD1 starts from the RT subject's own
  picture, and the test-scene anchor is not left for normal Auto refocus.
- **MF4 closed:** `afd_last_land_mm` is part of `have_last`; `afd_last_land_s/zoom/mm`
  are written only by engine modes 0 and 8, so bench/speed/revcal no longer leak;
  the mode-8 prep keeps the RT first trial's reference (cleared before the test,
  rewritten by the reference landing inside it).

The review-2 should-fixes are also in: 2-failure anchor clear (with the 5 s
timeout and new-scene escapes no longer re-clamped), MPU-method-scoped clamp +
method-switch clear, `# stop_check` parity with `stop_analysis.py` (**12/12 real
recovered trials agree**, same `npost=`, same verdict), `afdiag_afsearch_select`
guarded, focal-length debounce/keep-last-valid, and `af_report.py` surfacing
`notrig/valid/false_pass`.

Residuals found are **non-blocking** (§3): one rare anchor-metrics edge, the
focal-length debounce being call-counted rather than sample-counted, stop-check
integer truncation at the 1 %/2 %-per-s boundary, a failed-run-streak abort gap,
and two plan-text nits. None changes the verdict for the first field session.

---

## 1. Closure verification (exact code, why it closes, evidence)

### MF1 — stale `afd_ref_max_s` no longer re-raises the anchor

**Code:** seed block is now entirely inside `if (!aok)` (`13778–13801`); the
former `else if (aok && seed > aref_ref_anchor)` raise is gone; the only raise is
`raw > aref_ref_anchor` (`13817–13825`), which resets `aref_ref_soft_n/best`; the
confirmed-soft reset (`13832–13863`) also lowers the ref max when the key matches
and it is above the new anchor:

```c
if (afd_ref_max_s && afd_ref_max_zoom == zoom && afd_ref_max_mm == mm && afd_ref_max_s > aref_ref_anchor)
{ afd_ref_max_s = aref_ref_anchor; afd_soft_land_n = afd_soft_best = 0;
  afd_note("# local: ref max reset confirmed soft old=%d new=%d t=%d\n", ...); }
```

**Model (replicates the snapshot logic):** anchor 115k, stale refmax 115k, three
soft settles at 95k →

```
settle 1: soft n=1                      anchor=115000 refmax=115000
settle 2: soft n=2                      anchor=115000 refmax=115000
settle 3: soft n=3 +refmax->95000 CONFIRMED-SOFT  anchor=95000 refmax=95000
settle 4-6: hold                        anchor=95000 refmax=95000
```

No re-raise after the reset; the local `ref=` stays at the soft level, so the
"climb still soft -> B on every trigger" loop from review 2 is closed.
Counters: seed branch resets both; raise branch resets both; confirmed-soft
resets both; the `>= 92 %` branch resets both; `afd_ref_max_reset` resets both.
`aref_anchor_fail_n` is reset on every settle (`13875`).

### MF2 — the anchor can no longer be seeded with zero texture metrics

**Code:** seed uses `aref_ref_anchor_em = MAX(old_em, emax)` and
`aref_ref_anchor_g = MAX(old_g, g)` (`13795–13796`); the clamp requires
`aref_ref_anchor_em > 0` before restoring metrics (`13728–13736`). The two-bucket
model with a zero old metric → after the 24 s rotation `aref_ref_em` stays at the
settle sample's edge (≥ `AREF_PEAK_EMAX_MIN` for a textured scene), so gate 4 is
not self-locked. (Residual for a *low-but-nonzero* edge is §3/R1 — not the
review-2 zero case.)

### MF3 — Refocus test no longer inherits or leaves a scene reference

**Code:** `aref_test_task` (`14878–14885`):
`afd_ref_max_reset("rtest"); aref_anchor_clear("rtest"); aref_test_run();`
`afrt_state_reset(...); aref_forget=1; aref_eligible=0; afd_ref_max_reset("rtest");
aref_anchor_clear("rtest");`. The before-clear runs after the `aref_blk_mask`
gate, so it only fires when the test will actually run; `aref_test_run` always
returns (all abort paths `goto done`), so the after-clear always runs.
HOLD1 therefore starts with no anchor/ref max; its first settled picture is the RT
subject's own (the first settle has `old_ref=0, refmax=0` → no seed → adopt `med`;
later triggers use the RT peak), so a pre-test scene cannot clamp it. The after-clear
prevents the test scene's anchor/ref max from being inherited by the walk.
Log: `# local: ref reset why=rtest` / `# aref ref anchor: reset why=rtest` bracket
the test **when there was state to clear** (see R5).

### MF4 — last-landing fallback is focal-length keyed and mode-gated

**Code:** `afd_last_land_mm` (`8630–8633`); written only when
`afd_last_land_track` (`9598–9606`); the flag is `(mode == 0 || mode == 8)` at
engine entry (`10411`); `have_last` requires
`afd_last_land_mm == afd_mm_bucket()` (`9418`); `afd_ref_max_reset` clears
`afd_last_land_mm` and the soft counters (`8685–8698`). Bench (6), speed (3) and
reversal-cal (7) landings no longer write it; mode 8 does, which is what gives the
RT first trial its reference after the MF3 pre-test clear (the reference landing
goes through `afb_land_a` → `afb_search` → `afd_run`, and `afd_last_land_track` is
already 1 for the whole mode-8 engine call). `# refmax ... last_track=` exposes the
gating (`9610–9612`).

### Should-fixes from review 2

| Item | Status | Evidence |
|---|---|---|
| 2-failure anchor clear (SF2) | closed | `AREF_ANCHOR_FAIL_N 2` (`13531`), counter/reset `14258–14272`, settle resets it (`13875`); clear also `afd_ref_max_reset("failed runs")`; skipped runs no longer carry stale `afd_lowcontrast` (`13601`) |
| MPU-scoped clamp + escapes (SF1) | closed | clamp `Auto_refocus_method == 1` (`13728`); method switch clears the anchor (`14330–14335`); 5 s timeout (`14025–14026`) and "new scene" post-check (`14090–14091`) now clear anchor + ref max |
| seed-raise soft counters (SF3) | closed | seed-raise removed; the only raise resets the counters |
| RT first-trial reference (SF4) | closed | mode-8 last-landing write (MF4 above) |
| stop_check parity (SF6) | closed on real data | baseline = last sample ≤ `t_stop`, `i0` = first ≥, `s_stop = sp[ib]`, direction-aware max/min excursion, 1 % bound (`10211–10257`, call `10331` passes `dir`). Replayed the rule on both recovered AFSTOP logs: **12/12 trials same `npost`, same `halted=0`**, `slope`/`exc` equal to the offline values within integer truncation. Residual boundary nuance: R3 |
| afsearch task-create guard (SF7) | closed | forward declaration + wrapper `7422–7435` |
| focal-length debounce (SF5) | closed for the stated cases | keep-last-valid on `0`/bogus, two-observation candidate, no notes/changes on a stable lens, first boot handled (`8657–8683`). Residual call-counting nuance: R2 |
| `af_report.py` integrity fields (SF8) | closed | `notrig/valid/false_pass` printed + verdict line for `notrig>0` (`contrib/af_report/af_report.py:331–375`); host suite 81/81 |
| plan §11 acceptance corrections (SF9) | closed | §4.3 + §11.11 corrected; only §5 body text is stale (R7) |

---

## 2. General regression scan

- **Abort / deadline / epilogue:** the patch-4/5 delta touches only
  `afd_mm_bucket`, ref-max/soft-landing bookkeeping, the `afd_run` verified-landing
  block, `afw_stop_check`, the aref anchor/settle/finish/step code, the menu
  wrapper and the test task. `afd_must_abort`, `afd_move_ex`, the engine loop and
  the single-exit epilogue are textually unchanged; no new early return was added
  in the engine path. The new aref clears run in the same (single) task context as
  the settle/eval code.
- **ML snprintf:** every new/changed line uses only `%d`/`%x`/plain `%s`, no `%c`,
  no `%%`, no field widths; argument counts checked, including
  `# stop_check` (9 `%d` both branches), `# refmax` (7/7),
  `# local: ref max reset confirmed soft landings` (5/5), `# focal mm change` (2/2),
  `# aref ref anchor: reset` (1 `%s` + 2 `%d`).
- **Overflow:** new products are `int64_t` (`ext`, `sx/sy/sxx/sxy`, `bq`) and
  `ext*100` is bounded by the s scale; `exc_pct`/`delta_pct`/`slope` divisions are
  guarded (`s_stop != 0`, `den != 0`). The pre-existing theoretical `(int)` cast
  overflow for a tiny `s_stop` remains (review-2 N3), unchanged.
- **Statics:** all new globals are zero-initialised; `aref_prev_method = -1` is
  explicit; `afd_mm_bucket`'s `cand = -1` is explicit. No uninitialised use.
- **A–F behaviour:** algorithms themselves are untouched. The only shared-state
  change is that bench (6) and speed-test (3) landings no longer update
  `afd_last_land_s` (mode 0/8 only, MF4). In practice this does not change bench
  results: bench trials are deliberately defocused, so B's coarse-1F "in focus"
  shortcut (`9189–9192`) and E's warm shortcut (`9376`) would not fire there; the
  speed test performs no further `afd_run`. Menu AF drive (mode 0) still updates it
  as before; the review-2 residual that A/B/E read it with a zoom-only key is
  unchanged and documented (§11.11).
- **Parsers:** ran synthetic post-patch `AFDRIVE.LOG`/`AFSTOP.LOG` with every new
  line (`# focal mm change ...`, `# refmax ... last_track=`, `# stop_check ...
  exc_pct=`, `# local: ref max reset confirmed soft [landings] ...`, `# aref ref
  anchor: reset why=...`) through `corpus.parse_drive_facts`,
  `stop_analysis.analyze_file` and `af_report.py`: no crash, no lost facts,
  `# RTEST`/`# RT_TRIAL`/`# RESULT`/`# aref done` still parse. Full host suite
  independently re-run: 81/81 + 6/6 corpus, PASS.
- **Build artefact:** `crop_rec.mo` md5 matches the Lead's deterministic build
  (`ebb45f8b…`); I cannot rebuild (no make) — I rely on the build owner for the
  compile, and the static scan found no format/type/unused-variable issue.

---

## 3. Residual (non-blocking) findings

None of these change the verdict; all are logged/observable and self-recover via a
scene/zoom/toggle/RAW/user-AF reset. Ordered by interest, not severity.

### R1 — an anchor *raise* can still store low (1…69) texture metrics, re-opening a rare gate-4 lock

**Where:** raise branch `aref_ref_anchor_em = emax; aref_ref_anchor_g = g;`
(`13817–13822`); clamp guard `if (aref_ref_anchor_em > 0)` (`13731`).

The review-2 MF2 zero-metric path is closed, and the clamp no longer forces
`em=0`. But a raise can still *lower* the stored anchor edge: the 70 %-of-landing
branch (`13805–13812`) sets `raw = aref_mpu_land`, so a run whose picture settled
far below the landing can raise the anchor to the landing value while the settle
sample's `emax` is low (the "settled far below landing" situation is exactly a
low-edge frame). If that `emax` is 1…69, the anchor is stored with
`aref_ref_anchor_em < AREF_PEAK_EMAX_MIN`; ~24 s later the clamp restores it and
gate 4 blocks path-1 triggers until a reset. (With `emax == 0` the patch-5 guard
avoids it; the residual is only the low-nonzero band.)

**Assessment:** rare (needs the observed "settled far below landing" case *and* a
low-edge settle sample), logged (`# aref settled far below landing` followed by
`# aref ref anchor: raise`), and not a regression against patches 1–4 (the raise
branch predates patch 5). **Not a blocker for this session.**

**Minimal fix for a future build (one line):** make the clamp threshold the gate
threshold instead of zero:
`if (aref_ref_anchor_em >= AREF_PEAK_EMAX_MIN) { aref_ref_em = ...; aref_ref_g = ...; }`,
and/or take `MAX(emax, aref_ref_anchor_em)` in the raise branch.

### R2 — the focal-length debounce counts *calls*, not samples (partial SF5 closure)

**Where:** `afd_mm_bucket` `8657–8683`; call sites `9418–9419` (local run),
`9605`/`9612` (landing block), `13753`/`13772` (done-note/settle), `14465`/`14470`/
`14477` (aref eligibility).

The function is stateful (static `last/seen/cand/cand_n`) and is called several
times per sample from some paths. A one-sample focal-length glitch is correctly
absorbed on the idle aref path (one call per step), but during an engine run or a
verified-landing block two or three calls arrive back-to-back and satisfy the
"two observations" test inside one sample — so a glitch during a run can still be
"confirmed" and reset the references, and within a single landing
`afd_last_land_mm` (call before `afd_ref_max_update`) and `afd_ref_max_mm` (call
inside) can be written with *different* buckets. Consequences are one or two
reference resets / one landing falling back to `ref=0`; no safety or lens issue.
Stable focal length and `focal_len == 0` behave exactly as claimed.

**Not a blocker.** Future fix: make the bucket pure (`raw` + "last valid") and do
the debounce once per sample (e.g. in `aref_step`/engine entry) into a global, or
gate the debounce on `get_ms_clock()` so two calls in the same tick cannot confirm.

### R3 — inline `# stop_check` can disagree with the offline script at the threshold boundary

**Where:** `10249–10257` (`exc_pct`, `slope_pct_s` are integer-truncated before the
1 % / 2 %-per-second comparisons).

On real data this is closed (12/12 recovered trials agree, incl. `npost`), and the
plan already says the offline script is authoritative. But a synthetic trace with
a 1.5 % excursion and a flat slope is `halted=1` inline (`exc_pct` truncates to 1)
while `stop_analysis.py` says `continued` (excursion 1.5 > 1.0). Real traces have
|slope| ≥ 8.3 %/s, so the field session will not hit this.
Future fix: compare in milli-percent (`ext * 100000 / s_stop` vs 1000, and slope
mille vs 2000) instead of truncating to int percent.

### R4 — the no-focus failure streak is not broken by an abort/skip between failures

**Where:** `14258–14272`.

Two `nopeak/lowtexture` runs separated by an aborted or skipped run (pct < 0,
no nopeak/lowtexture) still reach `AREF_ANCHOR_FAIL_N` and clear the anchor/ref
max. The clear is bounded/logged and the plan accepts the 2-failure reset; "two
*consecutive* no-focus runs" would be stricter. Optional fix: also reset
`aref_anchor_fail_n` on `pct < 0 && !(afd_nopeak_last || afd_lowcontrast)`.

### R5 — the RT clear log lines only appear when there was state to clear

`afd_ref_max_reset` / `aref_anchor_clear` early-return when there is nothing to
clear (`8687`, `13476`), so on a fresh boot (no prior walk) the plan's acceptance
"`# local: ref reset why=rtest` / `# aref ref anchor: reset why=rtest` brackets
around `# RTEST_BEGIN`" will not appear. Read this as "brackets appear when a
pre-test state existed", not as a failure. Same for
`# local: ref max reset confirmed soft` — it appears only when the ref max was
actually above the new anchor (`13848–13857`).

### R6 — plan §5 body still describes the pre-patch stop-check baseline

`doc/AF-PATCH2-FIELD-PLAN.md` §5 says "`s_stop` = first sample with
`t_ms >= t_stop`"; patch 5 changed it to the last sample at/before `t_stop`
(§11.11(d) is correct). Documentation-only; update §5 for the record.

### R7 — pre-existing cross-task statics (unchanged class)

`afd_last_land_mm`, `afd_soft_land_n/best`, `afd_ref_max_*` are written in the AF
worker and read/cleared in the aref (main) context; `afd_mm_bucket`'s statics are
called from both. There is no concurrent access in the normal flow (the aref
evaluator is gated off during an MPU run, and the RT task is synchronous), and
single-core ordering is established via the volatile request/state flags, so this
is the same benign class as review 2's N6 — noted, not actionable now.

---

## 4. Field-tester checklist delta (what to trust)

The review-1/2 checklists still apply. New/changed lines:

1. **Refocus test (run it first, and after a walk too):**
   - HOLD1 `# RT_S ... ref=` must be the RT subject's own level even if the test
     is started after a walk on another subject (`false_trig=0` now expected);
   - `# local: ref reset why=rtest` / `# aref ref anchor: reset why=rtest` bracket
     the test **if state existed** (R5);
   - `# RTEST ... notrig/valid/false_pass` — `ok=5/5` is clean only with
     `valid=5`; `af_report.py` now also prints the integrity line/verdict.
2. **Anchor lines:**
   - `seed` once per scene/key; `raise` only when the picture genuinely gets
     sharper; `soft n=1/2` must not lower `anchor=`;
   - after `confirmed soft scene -> reset ref=…` there must be **no** immediately
     following `raise ref=<higher>`, and `# local: ref max reset confirmed soft
     old=… new=…` should appear if the max was above the anchor (MF1 closed);
   - `# aref done ... anchor=` / `ref_max=` non-zero in the walk after the first
     settle; `# local ... ref=` should follow the soft level after the reset.
   - a `raise` *after* `# aref settled far below landing` with a subsequent long
     gate-4 stretch (≥24 s) would be R1 — rare; record and recover by a scene/zoom
     reset.
3. **Focal length:** `# focal mm change pending/confirmed` only on a real zoom
   (a lone stray line is harmless, R2). After an optical zoom the first
   `# local s0=... ref=` must be 0 or the new-focal landing, never the old one
   (MF4).
4. **Refs / tracks:** `# refmax ... last_track=1` for mode-0/RT-prep landings,
   `last_track=0` for bench/speed/revcal; `track=1` only for mode 0.
5. **Stop test:** `# stop_check ... exc_pct=... halted=...` now mirrors
   `stop_analysis.py` on real traces; if in doubt run the script (authoritative).
6. **Timing:** `search_ms`/`epilogue_ms`/`total_ms` semantics unchanged from
   review 2; `total_ms + epilogue_ms` is wall time.
7. **Recovery lines to expect, not failures:** `# aref ref anchor: reset
   why=failed runs|timeout|newscene|method|rtest`, `# local: ref reset
   why=failed runs|timeout|newscene|rtest`, and `# local: ref max reset confirmed
   soft landings ...` (patch 4). If `why=failed runs` appears on a static scene,
   check the AF result (`nopeak`/`lowtexture`) rather than tuning constants.

---

## 5. What I could not verify

1. **No camera:** the closure is verified by code tracing, two state models and
   the recovered real-data stop logs; the field behaviour of the anchor escape and
   the RT HOLD1 ref remain unmeasured.
2. **No rebuild:** I did not run `make`; I verified the in-tree `crop_rec.mo` md5
   matches the Lead's build (`ebb45f8b…`) and re-ran the host suite, but cannot
   independently prove the .mo was compiled from this exact diff.
3. **`lens_info.focal_len` real-world stability** (the whole premise of the mm
   debounce): field logs show plausible mm values, but flicker behaviour during
   zoom/AF/exposure is not measured.
4. **The task-12 `(9,0x17,32)` feed logging** is out of this review's scope
   (only reviewed insofar as it does not touch the AF/aref code paths).
5. **A–F bench numbers** were not re-flashed; the argument that the last-landing
   gating does not change them is static (bench trials are defocused).

---

## 6. Confidence

**High** that the four review-2 must-fixes and the listed should-fixes are closed:
the seed/raise/soft logic and the bucket/clamp logic were replicated in models,
the stop-check rule agrees with the offline analyser on all 12 recovered trials,
the RT/mode-8 ordering was traced through the calls, and the patch-5 diff is small
and localised (616 lines delta vs patch 3, no engine/move/epilogue changes).
**Medium-high** on "no new regression" — host suite 81/81 + 6/6 corpus re-run,
all new log lines exercised through the parsers, format/arg/overflow scan clean,
and the in-tree build hash matches. The remaining unknowns are physical/field, not
code-structural. No blocker found: the tree is good to flash for the planned
session.
