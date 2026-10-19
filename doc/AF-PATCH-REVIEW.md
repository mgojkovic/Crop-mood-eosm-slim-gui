# AF patch review: local-search wide probe + reference integrity (task-5 / task-6)

**Status:** adversarial code review, **offline (no camera)**. Not a field validation.
**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` · **Base:** HEAD `043d914`.
**Artifact reviewed:** the uncommitted `modules/crop_rec/crop_rec.c` delta
`+169/−31`, snapshot `/tmp/af_patch_task5_6.diff`,
md5 `38450d67a2f47bba63ccd03e85439d6c` (verified identical to `git diff` when the
review started). Line numbers below refer to **that snapshot applied on HEAD**
(`git show HEAD:modules/crop_rec/crop_rec.c` + `/tmp/af_patch_task5_6.diff`).

> **Note — the working tree moved during the review.** A second diagnostics/safety
> patch (task-7: NODONE stop, servo abort, `# refmax`/`search_ms`/`# RTEST
> notrig/valid` logging, menu task-create guard) landed in `crop_rec.c` while this
> review was in progress. It is **not** part of this review. Its delta was read
> and is noted separately in §6; the reviewed wide-probe/ref-max/floor/RT code is
> textually unchanged by it (only shifted). Anchor mapping:
> `afd_ref_max_*` 8583/8590 → 8600/8607; `afd_aref_trig_*` 8614 → 8631;
> `afd_run_local` 9271 → 9288; `afd_ref_max_update(r)` 9443 → 9461;
> `aref_mpu_settle` 13443 → 13549; `afrt_reference` 14230 → 14338.

---

## 0. Verdict

**Flash after fixes.** The patch is structurally sound, does not introduce any
crash/UB/format-string/overflow/epilogue violation I can find, and even unfixed it
is a net improvement over HEAD (it can no longer *silently* redefine the local
reference to a soft landing; it just cannot always prevent a soft landing).

But three localized defects mean **the field logs cannot yet be used as evidence
that the two open problems are fixed**, and one of them can actively spoil the
planned field session:

| # | Sev | One-line |
|---|---|---|
| MF1 | must-fix | the soft/drop guard is only inside `if (!dir)`; a noisy probe can still pick a direction and the climb can still land at 50–92 % of the reference — the exact 82–83 % failure class |
| MF2 | must-fix | `afd_ref_max_s` is keyed only by `lv_dispsize` (digital zoom, **not focal length**) and is updated by bench / RT-prep / speed-test landings; the field plan benches at 35/50 mm then walks at 18 mm in one boot → contaminated reference |
| MF3 | must-fix | the `AREF_REF_FLOOR_PCT 95` floor is a **95 %-per-settle ratchet**, not an anchor (0.95^k), and the floored value is rotated out of the 12–24 s decaying peak, so the trigger reference still erodes; the plan's "new_ref does not drift down" acceptance is false |

If a rebuild before the field window is impossible: flashing as-is is still lower
risk than HEAD, but the tester must not accept `# aref done new_ref=` as proof of
anchoring, and should run the **walk before the bench** (or power-cycle between
them) to avoid MF2.

**What is verified good:** wide-probe geometry arithmetic (§2), all new early
returns vs abort/deadline/single-exit (§3), the ref-max update is gated on a
genuinely verified landing (§4), ML snprintf/format/overflow rules (§5), parser
tolerance of the new/renamed log lines (§5), and A/B/C/D/E/F result behaviour
(§5).

---

## 1. What the patch actually does (re-derived, not from the plan)

- `afd_run_local`: after the two narrow probes at +p1 and −p1 (p1 = 2×fine), when
  no side is > 105 % of s0 it may probe ±wo (wo = 2×p1 = 4×fine): P−p1 → P+wo
  (+p1+wo), P+wo → P−wo (−2wo), then return to P+p1 (dir +) or P−p1 (dir −) and
  run the existing climb/landing from there.
- A per-zoom static `afd_ref_max_s` is updated only in `afd_run`'s
  verified-landing block and used as the local reference `ref` when valid for the
  current `lv_dispsize`, else `afd_last_land_s` (old behaviour).
- `aref_mpu_settle`: saves `old_ref`, computes
  `floor = 95 % × MAX(old_ref, pre)`, adopts `MAX(settled, landing, floor)` and
  keeps the old texture metrics when the floor wins.
- `afrt_reference`: keeps the old `ref_s` when the fresh re-reference measurement
  is < 95 % of it, logs `# RT_REF anchor kept`.
- `aref_mpu_arm` publishes `afd_aref_trig_pct` / `afd_aref_trig_path` before the
  `AFD_F_AREF` engine run.

---

## 2. Wide-probe geometry — verified correct

Re-derived the relative positions (fine = 20 → p1 = 40, wo = 80; the arithmetic is
fine-independent):

| move | code line (snapshot) | from → to | result |
|---|---|---|---|
| `+p1` probe | 9289 | P → P+p1 | OK |
| `−2p1` probe | 9291 | P+p1 → P−p1 | OK |
| wide `+p1+wo` | 9324 | P−p1 → P+wo | OK |
| wide `−2wo` | 9326 | P+wo → P−wo | OK |
| return dir + | 9343 | P−wo → P+p1 (`wo+p1`) | OK |
| return dir − | 9348 | P−wo → P−p1 (`wo−p1`) | OK |
| climb | 9357-9366 | from ±p1 outward, 8 × fine | covers ±wo (2 steps) and ±200 pulses |

`fmax` starts as the return sample (so the wide samples themselves are not used as
the climb max, but the climb re-scans the wide band within 2 steps), `pmax`/`since`/
`below`/`steps` are initialised exactly as in HEAD at 9356, and the climb step cap
and the 18 s `AFD_RUN_CAP_B_MS` are unchanged (each wide move goes through
`afd_move_ex` → `afd_must_abort`/`afd_deadline`). **The geometry is not the
problem.**

Residual geometry caveats (see SF4):
- the wide samples are taken at the end of 120- and 160-pulse **reversals**
  (measured reversal range is ±40/±80; R is unmeasured, modelled ≈60), and the +
  sample (after a +120 reversal) and − sample (after a −160 reversal) do not have
  the same reversal history, so `wp`/`wm` can be biased and the "P±wo" positions
  can be closer to P than commanded;
- a refused end-stop chunk breaks the chunk loop but `afd_pos` has already been
  advanced by the full chunk (pre-existing in `afd_move_ex`), so after a refused
  wide move the subsequent "return to P±p1" can be off by up to one chunk
  (100 pulses = 2.5 × p1). Failure mode is a wrong landing start → B fallback
  (slow), not an unsafe move, because the landing is feedback-based.

---

## 3. Early returns / invariants — verified clean

Every new `afd_move_ex` is followed by `if (afd_aborted) return 0;` (9325, 9327,
9344, 9349). Every new "give up" path sets `afd_path_local = 0` before
`afd_run_sweep` (9334, 9302, 9310, 9371, 9377, 9392). `afd_run_sweep` re-samples
from the current position and is safe from any lens position; an abort returns 0
to `afd_run`, and the engine's single exit/epilogue (stop + session close) is not
bypassed — `afd_run` is called from inside `afd_engine`'s loop, which always falls
through to the epilogue. No new state is left un-restored (`afd_rt_thr1` is
restored; `afd_land_flat`/`afd_best_*` are only set on the landing path). A
timeout now costs the two extra wide moves, which can make a slow B fallback hit
the cap sooner — same failure handling as HEAD (timeout = aref failure), not a new
class.

---

## 4. Must-fix findings

### MF1 — the soft/drop guard does not cover a direction found by the narrow probes

**Where:** snapshot `crop_rec.c:9314-9338` (guard inside `if (!dir)`), `:9374-9379`
(only the 50 % far check after the climb), `:9380-9398` (unconditional landing).
Current tree: 9331-9355 / 9393-9398 / 9402-9417.

**Why it matters:** `dir` is non-zero when either narrow probe is > 105 % of s0.
On a soft start (s0 ≈ 82 % of `ref`), a noisy/interpolated probe at 106 % s0
(≈ 87 % of ref) sets `dir`, the wide probe is skipped entirely, and the climb lands
at 92 % of the *local climb max* — the only remaining guard is
`fmax < 50 % × ref`. So a climb that stalls on a shoulder at 60–90 % of `ref`
produces exactly the failure this patch was written to fix: a local landing well
below the true focus, reported as local "success" (the engine pct is
final/local-best and stays ≈100). The drop-path guard is currently a `dir == 0`
guard.

**Minimal fix** (after line 9379, before the landing, no geometry change):

```c
if (ref && afd_below(fmax, ref, AFD_L_SOFT_PCT) && (drop || soft0))
{
    afd_log("# local: climb still soft on the drop path -> B\n");
    afd_path_local = 0;
    return afd_run_sweep(best_out, 0);
}
```

where `soft0` is the `soft` expression computed once before the `if (!dir)` block.
(Alternatively force the wide probe when `soft0` regardless of `dir`; the geometry
still starts from P−p1, so it composes, but the post-climb check is the smaller
change and also covers a wide probe whose direction was noisy.)

**Verify:** no local run may end with `afd_land_s < 92 %` of the `ref=` printed on
its `# local s0=...` line while `drop=1`; in a simulated/replayed soft curve the
log must show either a wide probe or `-> B`, never a bare local landing.

### MF2 — `afd_ref_max_s` is keyed by digital zoom only and is fed by non-aref runs

**Where:** snapshot `:8578-8610` (state + update), `:9443` (unconditional call in
`afd_run`), `:13904-13913` (the aref-memory resets that do **not** reset it).
Current tree: 8595-8627, 9461, 13904-13913.

**Why it matters (three separate leaks):**

1. **Focal length is not part of the key.** `lv_dispsize` is the digital zoom
   (x5/x10); the lens focal length does not change it. `AFD-F-PLAN §3` benches at
   18/35/50 mm and then walks at 18/35/50 mm at x5 in one boot, and the handover
   says sharpness is focal-length dependent ("worse AF at 30–50 mm", fine step
   adapts by focal length). A 50 mm bench landing can therefore become the
   reference for the 18 mm walk (stale-high → every local run wide-probes and
   falls back to B; stale-low → the 92 %/50 % guards are weakened).
2. **Any engine mode feeds it.** `afd_ref_max_update` is called from the shared
   `afd_run`, so mode 6 (bench B07/B14/B1F landings), mode 3 (speed-test landing)
   and mode 8's prep (`afb_land_a` → `afb_search(6)` → `afd_run`)
   all write the local-search high-water mark. The field plan calls this
   "bookkeeping only"; it is not — it is the reference the local search uses.
3. **Aref's own forgetting events do not forget it.** The aref peak is cleared on
   Auto-refocus toggle, RAW-rec start/stop (crop/preview scale change) and
   user/foreign AF, but `afd_ref_max_s` survives all of these. Only a verified
   landing < 75 %, a >25 % luma jump or a digital-zoom change clears it.

Failure mode is "slow, not soft" (B fallback) — but on the planned single-session
test it is the most likely way the session looks *worse* than HEAD.

**Minimal fix:**

- add `static int afd_ref_max_mm;` and treat `lens_info.focal_len` (validity is
  already assumed by the aref logs, `mm=` in `# aref trig`) as part of the key:
  on a >10 % change, enter the "new zoom" branch (log
  `# local: ref max zoom set zoom=.. mm=..`);
- gate the call: in `afd_engine` set `afd_ref_max_track = (mode == 0)` and call
  `afd_ref_max_update(r)` only when set (mode 0 is the only mode that can run the
  local search; plain menu AF-drive landings are still genuine and useful);
- add `afd_ref_max_reset("rawrec")` next to the aref RAW-rec reset, and
  `afd_ref_max_reset("scene")` where `aref_forget` is set by the Auto-refocus
  toggle / foreign AF.

**Verify:** after the bench, the first walk landing must show
`# local ... ref max zoom set ...` (or a `ref=` equal to the field landing, not a
bench value); after a RAW-rec start/stop or a >10 % focal-length change the same;
`# refmax prev=... land=... now=... zoom=...` (task-7 delta) makes this easy.

### MF3 — the aref reference "floor" is a 5 %-per-settle ratchet and decays with the peak

**Where:** snapshot `:13450-13452` (`base = MAX(pre, old_ref)`,
`floor = 95 % × base`), `:13464-13474` (`adopt = floor`), `:13394-13421`
(`aref_peak_update` two-bucket 12–24 s memory), `AREF_PEAK_HOLD_MS 12000`
(`:13133`). Current tree: 13556-13558, 13570-13580, 13496 (peak update), 13234 (12 s hold).

**Why it matters:** two distinct things are wrong relative to the plan's claims
(AF-PATCH-FIELD-PLAN §7.1/§7.4 and EOSM-AF-WAY-FORWARD §1/§4):

1. `floor` is 95 % of the *old reference*, not the old reference. Every settle
   whose settled measurement is below `floor` adopts exactly `floor`, i.e. the
   trigger reference is multiplied by 0.95 each time:
   115 000 → 109 250 → 103 788 → 98 599 → … (≈77 % after 5 soft landings). It
   does **not** "keep the old peak"; it slows the erosion from the old ~0.86 per
   landing to 0.95 per landing. `# aref done ... new_ref=` will staircase down and
   the plan's acceptance criterion ("new_ref does not drift down") will fail.
2. Even that is temporary: `aref_peak_update` rotates the two 12 s buckets, so a
   floored bucket value with no >= it samples is replaced by the settled/blurry
   `med` within 2 × `AREF_PEAK_HOLD_MS` (≈24 s). The *trigger* reference
   (`# aref trig ... ref=`) therefore still erodes to the current soft picture
   within ~24 s; only the settle log line is anchored.

So the aref half of the "reference erosion" fix is a damper, not an anchor. Note
the tension deliberately: a **strict** anchor (`adopt = old_ref`) would make a
genuine gradual contrast fade (inside the 25 % luma gate) permanently
unattainable, replaced by a periodic refocus + hold cycle. The current 0.95
compromise accidentally provides slow adaptation. Decide explicitly — currently
the code implements §7.1's literal 95 % but the docs/acceptance promise §7.4's
"stays floored or rises".

**Minimal fix (pick one, then fix the docs to match):**

- **Anchor:** when the floor wins, `adopt = old_ref;` (not `floor`), and add a
  gradual-change escape: if N (e.g. 3) consecutive verified landings are all below
  `AFD_L_SOFT_PCT` of the reference, reset (log
  `# aref ref floor: scene change confirmed ...`) — soft landings cannot be a
  true peak, so this cannot recreate the erosion; or
- **Damper:** keep the code, change §7.4/§2.4 of the field plan to "at most 5 %
  down per soft landing, and the trigger peak still follows a genuine scene within
  ~24 s", and base the field acceptance on `# aref trig ref=` over time rather
  than on a non-staircasing `new_ref`.

**Verify:** five consecutive soft landings: snapshot build produces
0.95^k `new_ref`; anchored build produces a constant. Over ≥60 s of a static soft
scene, `# aref trig ... ref=` must stay floored in an anchored build (needs the
`aref_peak_update` clamp too) — in the snapshot build it will fall to `cur`.

---

## 5. Should-fix / nit findings

### SF1 — the "drop path" test is always true for every MPU trigger; the in-focus exit and `AFD_L_SOFT_PCT` are dead on the aref path

**Where:** snapshot `:9286-9288`, `:9314-9317`, thresholds
`aref_drop_pct_mpu[3] = {60, 75, 85}` (`:13155`), `AFD_L_TRIG_DROP_PCT 90`
(`:8562`). Current tree: 9303-9305, 9331-9334, 13256, 8579.

Path 1 is only reported when `cur < pct × peak` with `pct ≤ 85` (the hold penalty
can only lower it), so `trig_pct = cur/peak < 85 < 90` — every relative trigger
satisfies `drop`, and path 2 always does. Consequences: (a) `soft` can never be
the selector, so the documented `# local: soft -> wide probe` line never appears
(the tester will only ever see `drop path -> wide probe`); (b) the in-focus exit
`# local in focus` is unreachable for Auto refocus, so every false trigger on an
in-focus scene now pays 2 wide moves + a full B fallback (5–8 s) instead of 3
moves — bounded by the aref hold/penalty, but a real latency regression and a
change in local-search character the plan does not describe. If the intent was
"only a *deep* drop skips the in-focus exit", compare against the sensitivity
threshold actually in force (or set `AFD_L_TRIG_DROP_PCT 80` so High-sens 80–85 %
triggers keep the cheap exit and rely on the `soft` check for the 82–92 % band).

**Verify:** in the walk, `# local ... drop=1` for every trigger, `soft` never
logged; false triggers on a static scene leave `# local: wide still flat -> B`.

### SF2 — no safe recovery from a stale-high ref max in the 75–92 % band

**Where:** snapshot `:8606-8610` (`AFD_REF_RESET_PCT 75` only).

A persistent 10–20 % softer scene/light (inside the 25 % luma scene gate, and
above the 75 % deep-landing reset) keeps the max high forever: every local
trigger wide-probes and falls back to B, and a B landing at 80–90 % of the max
does not reset it. The plan acknowledges this as "slower, not softer", but "every
refocus goes to B for the rest of the session" is a real field failure. Cheap
safe fix: count consecutive verified landings below `AFD_L_SOFT_PCT` of the max
and after N = 3 set the max to the best of them (log the reason) — soft band
landings cannot be a true peak, so this cannot re-create the erosion the patch
fixes.

**Verify:** drop the light ~15 % without a >25 % luma jump; expect a
`ref max reset ...` line within 3 refocuses, not permanent `-> B`.

### SF3 — the anchored RT reference can turn a genuine light fade into 5 failures, and `# RT_REF` still logs the fresh (not adopted) reference

**Where:** snapshot `:14238-14245`; log line `:14239`; corpus reader
`contrib/af_test/corpus.py:258` (`RT_REF_RE` reads `ref_s=`).

If the light/contrast genuinely fades >5 % mid-test, every re-reference is
anchored to the start value and all remaining trials fail; the test reports
"ok 0/5" with no way to distinguish "AF failed" from "the scene changed". Also,
`# RT_REF ... ref_s=` now means "fresh measurement" (and `# RT_REF anchor kept
old/new` carries the decision), but the corpus extracts the former as `ref_s`, so
a post-patch session will still show a monotonically decreasing `ref_s` sequence
while the *active* reference is anchored.

**Minimal fix:** log the fresh value as `meas=` and the adopted reference as
`ref_s=` on the `# RT_REF` line (corpus keeps working and means the right thing),
and add a luma/`g`-similarity check on the anchor: if `|luma - anchor_luma|` (or
`g`) moved a lot, log `# RT_REF scene changed ...` and mark the trial invalid
rather than failed. Store the anchor luma/g in `afrt_reference` (or in
`aref_test_run`).

**Verify:** synthetic `# RT_REF` lines through `corpus.py` / `af_report.py`; field:
anchor-kept only after a *failed* trial in a static scene, never twice in a row
with a large `luma=` delta.

### SF4 — wide-probe positions after long reversals are unverified; log enough to check them

**Where:** snapshot `:9324-9337`.

The wide samples follow 120/160-pulse reversals (measured reversal range ±40/±80;
the model's R = 60 is unmeasured and the way-forward doc calls it the dominant
error). The `+` and `−` wide samples have different reversal lengths, so the
`wp`/`wm` comparison and the nominal ±4×fine reach can be biased. The `# local
wide plus=.. minus=..` line lacks positions; the generic move lines (`# ms phase
cmd pos s ...`) do have `pos`, so it is reconstructable — add `pos=` to the wide
line (or an `mm=`/`fine=` field) and do not treat a single "wide still flat → B"
as proof that there is no slope. No code change strictly required before the field
run; this is a caveat for interpreting the session.

### N1 — the trigger-hint globals are never cleared (footgun, benign today)

`:8612-8615` / `:13827-13828`. They are only read in `afd_run_local`, which is
only reachable for `AFD_F_AREF` runs, and both callers (`aref_step`, the Refocus
test) call `aref_mpu_arm` immediately before the run, so a stale value cannot
reach a run today. But if a future path ever runs the local search without arming,
the run inherits the last trigger forever. Clear them in `afd_engine` when
`!(flags & AFD_F_AREF)`, and snapshot both into locals in `afd_run_local`
(`int tpath = afd_aref_trig_path; ...`) — that also removes the two separate
volatile reads. The write order (pct then path) vs read order (path then pct) is
the safe direction and the worker is only requested after both writes, so there is
no real race.

### N2 — `# aref ref floor` field naming

`:13469-13470` logs `adopt=%d` but passes `floor` (identical by construction, but
misleading; the plan documents `adopt=` as the adopted value). Log `floor=` and
the final `adopt` after the max, or keep one field and document it. The task-7
delta already adds `floor=`/`adopt_pct=`; keep that, it resolves this.

### N3 — ref-max log lines lack context

`:8597/8603/8608` do not log `zoom`/`focal_len`; `# refmax ..., zoom=...` from the
task-7 delta helps. Add `mm=` for MF2 verification.

### N4 — the wide direction throws away the narrow evidence

`:9337` chooses `dir` from `swp/swm` only. If e.g. narrow `sm` was the best sample
seen (104.9 % s0, just under the 105 % bar) and the wide plus is 103 %, the code
climbs the + side. Combining s0/sp/sm/swp/swm (argmax) is strictly more robust and
free. Heuristic, not a blocker.

### N5 — after `afd_ref_max_reset("scene")` the first local run falls back to the stale last landing

`:9281-9283`: a scene reset zeroes the ref max but not `afd_last_land_s`, so the
immediate next local run uses the *old scene's* last landing as `ref`. This is the
HEAD behaviour (it cannot be worse), but the new `# local ... ref=` will show
`ref == last` and the soft/far decisions are against the old scene. Prefer
`afd_last_land_s = 0` (or `afd_last_land_zoom = 0`) inside `afd_ref_max_reset`
("scene") so both references restart together.

---

## 6. New delta noticed during the review (task-7, not part of this review)

Read only, not reviewed in depth; the wide-probe/ref-max/floor/RT code under
review is unchanged. Things the build owner should check:

- **New/renamed log fields**: `# refmax`, `# RESULT ... search_ms epilogue_ms`,
  `# aref done ... ref_max land_vs_ref`, `# aref ref floor ... floor adopt
  adopt_pct`, `# RT_TRIAL ... no_trigger valid false_pass`,
  `# RTEST ... notrig valid false_pass`. Spot-checked `contrib/af_test/corpus.py`
  (`RTEST_SUM_RE` / `RT_REF_RE` / `DRIVE_RESULT_RE` are prefix/kv based → tolerant)
  and `contrib/af_report/af_report.py` (`# RESULT` parsed by `kv`, RTEST by
  prefix) — no break found. `# aref ref floor` now parses as note kind `ref`
  (ignored); harmless.
- **`afd_abort_on_servo`** makes every `AFD_F_AREF` engine run abort while
  `is_continuous_af()` is true. Per the comment `is_continuous_af()` is the
  persistent *menu* setting; users who leave Movie Servo ON will find Auto
  refocus (MPU) silently aborting every run (it is logged as aborted, and the
  aref hold/backoff applies). That may be the intended safety, but the field plan
  should say "Movie Servo OFF" explicitly.
- **`# aref ref floor`** logs `floor=` and `adopt=` with the same value; harmless
  but does not expose a different adopt.
- **`afw_stop_check`** divides by `s_stop` (guarded by `!sp[i0].m.s1` → s_stop
  non-zero) and by `den` (guarded). Integer 64-bit accumulators; fine as read.
- **NODONE explicit stop** sends `(9,3,1)` even when the move may be running;
  handover says the stop does not halt a `(9,1)` move, but sending it is harmless
  by the stop-test finding (a second stop / already-stopped is a no-op).

---

## 7. Regression check: what is genuinely unchanged

- **A/B/C/D/E/F search decisions and results:** unchanged. The only shared code
  change is the `afd_ref_max_update` call (writes a static used solely by the
  local search) plus one log line; the landing verification / landing value is
  untouched. Bench numbers must be statistically identical apart from the extra
  log lines.
- **Engine pct semantics:** unchanged (`final_s × 100 / best`, local best is still
  the local climb max) — this is why `pct` must never be used to judge a local
  landing (the patch correctly uses `ref` internally).
- **`afd_last_land_s` / `afd_last_land_zoom`:** unchanged (still what A/B/C/D/E and
  the aref trigger use).
- **`aref` trigger thresholds/dwell/gates/peak memory:** thresholds/dwell/gates
  unchanged; `aref_mpu_settle` behaviour *is* changed (that is task-6 by design).
  Note the field plan §1.4 sentence listing "settle" as unchanged is wrong.
- **`afd_ref_max_s ≥ afd_last_land_s` at the same zoom** (both are set from the
  same verified `r`), so switching the far checks from the last landing to the ref
  max is strictly stricter, never looser — no soft acceptance can be introduced
  by that substitution.
- **ML format constraints:** all new format strings use only `%d`/`%x`/plain `%s`,
  no `%c`, no `%%`, no field widths; argument counts match (checked every new
  line, incl. `# aref ref floor` 6 args/6 `%d` and `# local wide` 3/3). New
  64-bit products are all cast to `uint64_t` before the multiply (9287-9288,
  9323, 9328-9329, 13452, 13478-13480, 14240); no new unchecked division
  (`ref`, `peak`, `old_ref`, `ref_s` are all guarded/non-zero at the point of
  use). No new unused variables were introduced (only pre-existing style as in
  HEAD).
- **Log buffering/recording:** new bookkeeping lines go to the normal engine RAM
  buffer (flushed after recording) or `afd_note`; no new card writes, no new
  tasks, no monitor operations.

---

## 8. What I could not verify

1. **Anything physical:** no camera. The wide-probe behaviour near an end stop, the
   real reversal offset R, refusal semantics and actual lens positions are reasoned
   from the code, not measured. My end-stop/position-error statements are code
   readings (`afd_move_ex` advances `afd_pos` per chunk before the end-stop break).
2. **Whether the 92/90/75/95 thresholds are right for the field.** They are derived
   from a handful of 18–20 mm scenes; the full ±4×fine reach is itself subject to
   the unmeasured reversal slack.
3. **`lens_info.focal_len` validity under every crop preset / RAW recording** — the
   aref logs print plausible `mm=18/20`, but MF2's focal-length key depends on it
   being reliable; the way-forward doc lists this as unverified.
4. **The 12–24 s peak-decay arithmetic in real cadence** — it follows from
   `AREF_PEAK_HOLD_MS 12000` and `aref_peak_update`, but sampling pauses (menu,
  half-press) could change the effective behaviour in the field.
5. **Compilation/warnings:** I did not run `make` (owned by another teammate) and
   did not edit `crop_rec.c`. The task-7 delta was in the tree during the review,
   so a diff-based line ref may shift before the build; locate by function/quote.
6. **Whether "all MPU triggers are the drop path" was intended.** It follows
   arithmetically from the threshold tables; if the author intended a narrower
   exception, the spec (not just the code) needs to say so.

---

## 9. Field-tester checklist (post-fix or as-is)

1. Build/flash the current tree only after confirming the three MF fixes (or accept
   the adjusted expectations below). Check `module_strings.h` placeholder and the
   zip `crop_rec.mo` md5 before flashing; the build owner runs `make`.
2. Settings: x5 movie LV, monitor **Keep open**, `Refocus method = MPU drive`,
   `Refocus AF = Local`, sensitivity High/MPU, **Movie Servo OFF** (see task-7
   note), no RAW recording.
3. **Order matters for MF2:** do the Refocus test and the walk **before** the
   bench, or power-cycle between bench and walk. If you bench at 35/50 mm and walk
   at 18 mm in one boot, expect slow/B-heavy refocuses from a contaminated
   `ref max` — record it, don't call it a regression.
4. Refocus test (18 mm first): accept ok ≥ 4/5 **with all 5 trials triggering**
   (`no_trigger`/`notrig` must be 0), `false_trig 0`, no trial `final_pct < 90`.
   In `AFDRIVE.LOG`, for each RT run confirm the `# local ... ref=...` line and
   that `ref` is the field-scene value (not a bench/chart value), and
   `# aref done new_ref=` does **not** drop by more than 5 % per settle
   (snapshot build: expect exactly 5 % steps; that is the MF3 defect, not
   success).
5. Walk: log `# local ... ref= trig_pct= drop=`; `soft` will not appear in the
   snapshot build (SF1 — expected); `wide still flat -> B` should appear only on
   genuinely flat scenes; record refocus latency and any repeated `-> B` pattern.
6. Any local run whose `land_s` (or the following `# aref done land_s=`) is below
   ~92 % of its `ref=` while a drop trigger: capture the surrounding AFDRIVE lines
   — that is MF1 in the field.
7. After the run, copy `AFDRIVE.LOG`, `AFRTEST.LOG`, `AFBENCH.LOG` and run
   `python3 contrib/af_report/af_report.py --copy <dir>`; add the session to the
   corpus. A post-patch RTEST summary must be compared with the **anchored**
   baseline (card18 becomes 2/5 under an anchored reference), not the old 3/5.
8. If `# aref trig ... ref=` drifts down on a static soft scene within ~1 min,
   that is the MF3 peak-decay path, not a trigger-threshold bug.
