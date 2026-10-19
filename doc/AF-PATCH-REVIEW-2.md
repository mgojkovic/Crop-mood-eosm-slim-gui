# AF patch review 2: diagnostics/safety patch 2 + review-fix patch 3

**Status:** adversarial code review, **offline (no camera)**. Not a field validation.
**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` · **Base:** HEAD `043d914`.
**Artifact reviewed:** `/tmp/af_patch3_full.diff`, md5
`a661d62933e055d5b40d74fd226b81b3` — the full `modules/crop_rec/crop_rec.c` diff
vs HEAD (patch 1 task-5/6 + patch 2 diagnostics/safety + patch 3 review fixes).
Verified identical to `git diff modules/crop_rec/crop_rec.c` when the review
started; **all line numbers below refer to that snapshot applied on HEAD**
(reconstructed under `/tmp/snap3`).

> **Pinning note.** Mid-review the working tree gained the task-12 logging-only
> `(9,0x17,32)` feed change (`AFD_EV_MPUF`, `feed=32B` header) — *not reviewed
> here*. Its delta is confined to the AFDIAG hook/flush/header code and does not
> textually touch anything cited below; it shifts the reviewed code by +62…+76
> lines (current tree: `afd_ref_max_update` 8672, `afw_stop_check` 10174,
> `afd_menu_task_create` 10991, `aref_peak_update` 13644, `aref_mpu_settle`
> 13709, `afrt_reference` 14574, `aref_test_run` 14606). Locate by symbol/quote.
> Read first: [AF-PATCH-REVIEW.md](AF-PATCH-REVIEW.md) (review 1),
> [AF-PATCH2-FIELD-PLAN.md](AF-PATCH2-FIELD-PLAN.md) §11 (patch-3 plan).

---

## 0. Verdict

**Fix first, then flash.** Patch 2 is diagnostics/safety plus one deliberate
behaviour gate and looks correct in the parts that matter; patch 3 genuinely
removes the 0.95-per-settle ratchet and the anchor clamp does stop the 12–24 s
peak decay from eroding the *trigger* reference. **But the anchor's own escape is
undone by the un-decayed `afd_ref_max_s`**, the anchor's texture metrics can be
seeded as zero (which silently disables the relative trigger path after ~24 s),
the Refocus test does not clear the pre-test anchor/ref-max, and the local
search's fallback reference still has no focal-length key / mode-0 gate. These
are all small, localised fixes, but without them the field logs cannot show that
reference erosion is fixed, and the Refocus test can report false triggers for a
reason unrelated to AF.

| # | Sev | One-line |
|---|---|---|
| MF1 | must-fix | stale `afd_ref_max_s` re-raises the anchor on the next settle, undoing every `confirmed soft scene -> reset`; the anchor is permanently high and the local search falls back to B on every trigger |
| MF2 | must-fix | the anchor can be seeded with `anchor_em = 0` (peak cleared, ref-max valid) and the clamp then forces `aref_ref_em = 0`; the `AREF_PEAK_EMAX_MIN` gate blocks path-1 triggers with no self-heal |
| MF3 | must-fix | the Refocus test neither clears the aref anchor nor the ref max at start/end, so a test run after a walk can register HOLD1 **false triggers** against the pre-test scene and leaves the test scene's anchor for normal Auto refocus |
| MF4 | must-fix | the local-search fallback `afd_last_land_s` has no focal-length key and is still written by bench / speed / RT-prep landings (`track` gates only the ref max) |

If a rebuild is impossible before the field window: flashing is **not unsafe**
(no crash/UB/format/epilogue issue), but do **not** run the Refocus test after a
walk and **do not** treat `# aref ref anchor: ...` as proof of anchoring — see
the checklist in §6.

**Verified good:** MF1 guard placement/order and the N4 direction argmax (§1.1);
MF2 zoom+mm key, mode-0 ref-max gating and the new resets (§1.2); the anchor's
replacement of the 0.95 ratchet and the peak clamp (§1.3); SF3 RT_REF parsing by
`contrib/af_test/corpus.py`/`af_report.py` (§1.4, checked against a synthetic
post-patch log); patch 2's notrig/valid/false_pass, search_ms/epilogue_ms,
NODONE stop, menu wrapper (9 handlers), servo gating and stop_check integer math
(§1.5); and the host test suite still passes (78 unit tests, 6 corpus cases,
`RESULT: PASS`).

---

## 1. What I verified as correct

### 1.1 Patch 3 / MF1 — post-climb soft guard (snapshot 9349, 9420–9427)

`soft0` is computed once from the pre-wide `smax` (9349) and the guard sits
**after** the climb and the "no peak within 8 steps → B" check and **before** the
50 % far check and the landing (9420–9427 after 9414–9419). Every branch falls
back to `afd_run_sweep` (a bounded, self-contained B search), so there is no
recursion/infinite loop inside a run: one trigger → at most one local attempt,
one B fallback. Because every MPU trigger sets `drop` (SF1 of review 1, now
documented in the plan), the guard is effectively "a local run may not land from
a climb max below 92 % of `ref`" — which closes the review-1 MF1 hole.
`soft0`'s value after the wide probe is irrelevant for correctness: it can only
*add* the guard; the `fmax` test is re-evaluated after the climb. The N4 argmax
(9377–9383) is safe: the wide entry requires `dir == 0`, so `sp/sm` are within
105 % of s0 and the fallback `dir = wp ? 1 : -1` is only reached when a wide
probe improved. Geometry of the return-to-P±p1 is unchanged from review 1
(verified there).

**Caveat for the plan, not the code:** the acceptance text "no local run may
land with `afd_land_s < 92 %` of `ref`" is stronger than the guarantee. The
guard bounds `fmax` (the climb max), and the landing retrace stops at
`AFD_L_LAND_PCT` = 92 % of `fmax` (the half-step top-climb then usually brings it
back up). So `land_s` can legitimately be ~85 % of `ref`. The field test should
judge `# local climb ... max_s=` against `ref=` (it is logged), not only
`land_s`.

### 1.2 Patch 3 / MF2 — ref-max key, gating, resets (snapshot 8586–8645, 9491–9504, 10269–10291, 14151–14219, 14283–14292)

- `afd_mm_bucket()` rounds `lens_info.focal_len` to 10 mm (0/implausible → 0)
  and the ref-max key is now `(lv_dispsize, mm)`: `have_ref` (9316) and
  `afd_ref_max_update` (8623–8645) both check it; a key change starts a new max
  and logs `zoom`/`mm`.
- `afd_ref_max_track` is set per engine call and only mode 0 feeds the max
  (8611, 10269, 10286). I checked every other `afd_run` caller: bench
  `afb_search` (mode 6), speed test (`afd_speed_run`, mode 3) and the Refocus
  prep `afb_land_a` (mode 8) all run with `track = 0`, so **the bench no longer
  pollutes the local-search reference** (acceptance: those landings log
  `# refmax ... track=0` and do not change `now=`). The RT's *trial* runs are
  mode 0 and do feed it — intended (same scene).
- Resets exist at all the aref forgetting points: scene luma jump
  (13754–13755), Auto-refocus toggle (14151–14152), RAW-rec start/stop
  (14163–14164), user/foreign AF (14218–14219), zoom/mm eligibility (14290–14291).
  The reset also clears `afd_last_land_s/_zoom` (N5) — see MF4 for the
  consequence.
- `afd_aref_trig_pct/_path` are snapshotted once into locals (9321–9323) and
  cleared for non-`AFD_F_AREF` runs (10287–10291); the review-1 N1 footgun is
  closed.

### 1.3 Patch 3 / MF3 — the anchor (snapshot 13336–13362, 13568–13602, 13633–13743)

- The 0.95-per-settle floor is gone. A settle adopts the anchor exactly
  (`adopt = anchor`, 13693), a genuinely sharper picture raises it (13680–13688),
  and the trigger reference is clamped to the anchor in `aref_peak_update`
  (13596–13601), so the 12–24 s bucket rotation can no longer erode it. This is
  the right construction.
- Seed: the pre-defocus peak (`old_ref`) and the engine ref max (`refmax`) seed
  a missing anchor (13641–13657); the seed/raise lines, the soft streak and the
  confirmed-soft reset are logged (`# aref ref anchor: seed/raise/soft/confirmed
  soft scene -> reset/reset`), and `# aref done ... anchor=` exposes the value.
- Confirmed-soft escape: 3 consecutive settles below `AFD_L_SOFT_PCT` (92 %) of
  the anchor reset it to the best of the three (13694–13711). With a *consistent*
  ref max this is a sane bounded adaptation.
- The anchor is per `(lv_dispsize, mm)` (`aref_anchor_ok`, 13341–13344) and is
  cleared by the same events as the ref max, incl. the new focal-length-change
  eligibility branch (14283–14292, new `aref_ref_mm` also resets the aref peak
  memory on an optical zoom).

**The two defects are in what feeds it and what it is seeded with — MF1/MF2
below.**

### 1.4 Patch 3 / SF3 — RT_REF (`# RT_REF ... meas= ref_s=`)

The sub-lines (`# RT_REF anchor kept`, `# RT_REF scene changed`) carry no
`ref_s=`, and the main line logs `meas=` (fresh) **and** `ref_s=` (adopted).
`contrib/af_test/corpus.py` `RT_REF_RE` (`^# RT_REF .*?ref_s=(\d+)`) therefore
extracts the **adopted** reference — verified with a synthetic post-patch
AFRTEST.LOG: the ref sequence came out as the anchored values
(`[97000, 97000]`, `ref_s_monotonic_decrease=false`), the `# RTEST ... notrig/
valid/false_pass` line matches the existing prefix regex, and
`af_report.py --logs <case>` runs clean and prints the new summary (it does not
surface the new fields, see SF8). The first-call behaviour is right
(`old == 0` → adopt fresh, set anchor luma); `afrt_ref_luma` is refreshed only
when the fresh value is adopted, i.e. it tracks the actually-adopted reference.
The luma guard (15 %) is a reasonable "don't turn a light change into 5
unexplained failures" escape; a *same-luma* subject change is still anchored
(deliberate, see checklist).

### 1.5 Patch 2 — diagnostics and safety

- **Trial integrity** (14614–14658): `no_trigger = runs == 0`, `valid = 1 -
  no_trigger`, `ok` only for valid trials, `false_pass` counted, `# RT_TRIAL`
  gains the three fields, `# RT_TRIAL_INVALID` is emitted, and `# RTEST` appends
  `notrig/valid/false_pass` **after** the old fields. Prefix parsers
  (`corpus.RTEST_SUM_RE`, `af_report.py:334`) still match; verified on a
  synthetic line. `need_ref` is unchanged (`fin_pct < 90`), which is correct:
  a no-trigger trial with a low final picture re-references, a false pass does
  not need to.
- **Timing** (10408, 10428–10430, 10484, 10547, 10564): `search_ms` wraps only
  `afd_run`; `total_ms` is captured before the epilogue; `epilogue_ms` is
  measured from `t_epi` through stop + session close + monitor close + the
  optional restore landing. Wall time = `total_ms + epilogue_ms`; the benchmark
  comparison (`search_ms` vs bench `median_ms`) is on the same basis. Other
  modes' `# RESULT` lines are untouched. (Nit N2.)
- **NODONE stop** (8116–8129): the explicit `(9,3,1)` is sent once on the NODONE
  branch, before the break, then the normal settle/sample continues; the move is
  still counted `nodone`, `afd_pos` keeps its existing commanded accounting,
  end-stop detection is untouched, and a second stop from the epilogue is
  harmless per the AFSTOP evidence. No chunk-accounting change.
- **Menu task-create guard** (10912–10924 + 9 call sites): the helper checks
  `RECORDING || RECORDING_RAW || mlv_raw_rec_busy()`, refuses with a NotifyBox +
  `# menu task refused ...` note and creates nothing; every AF-diagnostic menu
  SELECT now uses it. (One menu select is still unguarded — SF7.)
- **Servo gate** (7664, 7701, 10269, 10570): `afd_abort_on_servo` is set from
  `AFD_F_AREF` and reset at engine end; `is_continuous_af()` is indeed the
  persistent menu setting (`src/focus.c:901`), so menu AF/bench/stop/sweep are
  unaffected, and Auto refocus/RT are already blocked by `aref_blk_mask` gate 4;
  the gate only closes the race. Movie Servo must be OFF for the field session
  (already in the plan §11.7).
- **`# stop_check`** (10093–10132, call 10207): units are right — samples and
  `r->stop_ms` share the "ms since send" origin (`samples[0].t_ms = 0` at the
  send, `stop_ms = get_ms_clock() - t0`), the slope is `y/ms × 1000 / s_stop`
  (= % of `s_stop` per second) with `int64_t` accumulators, `den` and `s_stop`
  are guarded, and the text is `%d`-only. Realistic ranges fit `int64_t`.
  Threshold/verdict mismatch with the offline script: SF6.
- **Cross-task ordering:** the aref-trigger hints are written (pct then path)
  before `afd_wk_req` is posted (or synchronously in the RT) and read (path then
  pct) in the same order — safe direction; the engine never runs concurrently
  with `aref_eval` (phase/`afdiag_afdrive_busy`), so the new statics do not need
  `volatile` in practice (nit N6).
- **No regressions found** in the single-exit epilogue, abort/deadline handling,
  ML snprintf rules (`%d`/`%x`/plain `%s` only; arg counts checked on every new
  line), 64-bit products (all cast) or A–F search results (the ref-max change is
  deliberately invisible to them; see N5 for `afd_last_land_s`).

---

## 2. Must-fix findings

### MF1 — a stale `afd_ref_max_s` re-raises the anchor and undoes the confirmed-soft escape

**Where (snapshot):** `aref_mpu_settle` seed/raise `13641–13665` (esp.
`seed` 13642–13644 and `else if (aok && seed > aref_ref_anchor)` 13659–13665),
confirmed-soft reset `13694–13711`; `afd_ref_max_update` never lowers the max
below 75 % `8623–8645` (`AFD_REF_RESET_PCT 75`, 8581); the local guard uses `ref`
=`afd_ref_max_s` `9316–9317`, `9420–9427`.

**Why it matters:** in exactly the scenario patch 3 targets (a gradual
contrast/light change inside the 25 % luma gate), a verified landing at
82–92 % of the old max does **not** reset the ref max (75 % rule). The anchor's
confirmed-soft reset then sets the anchor to the soft best on settle *k*, but on
settle *k+1* the seed is `MAX(old_ref, afd_ref_max_s)` = the stale max, and the
`seed > anchor` branch raises the anchor straight back. The anchor therefore
oscillates (high → reset → high → …), the trigger reference is high nearly all
the time, `# aref ref anchor: raise` appears immediately after
`confirmed soft scene -> reset` in the log, and:
- the trigger keeps firing on the (in-focus but lower-contrast) scene, so
  refocus bursts repeat every hold/penalty cycle;
- the local search keeps `ref` high, so the MF1 guard returns
  `# local: climb still soft on the drop path -> B` on **every** trigger — the
  "slow, not soft" residual of review 1 SF2 becomes permanent.
This is the "permanently unattainable reference" the review asked about, and it
is the only must-fix that directly defeats patch 3's headline fix.

**Minimal fix:** tie the ref max to the confirmed-soft escape. In the
`confirmed soft scene` block (13700–13711), before resetting the soft counters:

```c
if (afd_ref_max_zoom == zoom && afd_ref_max_mm == mm && afd_ref_max_s > aref_ref_anchor)
{
    afd_note("# local: ref max reset confirmed soft %d -> %d t=%d\n",
             (int) afd_ref_max_s, (int) aref_ref_anchor, now);
    afd_ref_max_s = aref_ref_anchor;   /* keep the key */
}
```

(A dedicated `afd_ref_max_lower(s)` helper is cleaner; a genuinely sharper
landing later raises both again through the normal rise path.) This also
implements review-1 SF2 for the local reference with the same evidence, and
cannot re-create erosion because a confirmed-soft value is a settled picture.

**Verify:** a static soft scene with a stale max must show
`confirmed soft scene -> reset` **without** an immediately following
`raise ref=<higher>`; `# local ... ref=` stops being the old max; the
`-- B` fallback stops being logged on every trigger. A model of the settle/seed
logic reproduces the re-raise deterministically.

### MF2 — anchor texture metrics can be seeded zero; the clamp then disables the relative trigger path

**Where (snapshot):** seed `aref_ref_anchor_em = old_em; aref_ref_anchor_g =
old_g;` `13652–13653`; clamp `13596–13601`; path-1 gate
`peak >= pmin && aref_ref_em >= AREF_PEAK_EMAX_MIN` `13984`
(`AREF_PEAK_EMAX_MIN 70`, 13282). Reachable seed with `old_em == 0`:
`afrt_state_reset` clears the peak but not the ref max/anchor (`14431–14452`),
`aref_mpu_newref` timeout clears the peak (`13868–13873`), or any
absolute-path (path 2) run where the old peak had `em < 70`.

**Why it matters:** when the anchor is seeded while the old peak is already
cleared (`old_ref == 0`, `refmax` valid), `old_em = old_g = 0`, so the anchor
carries zero texture metrics. The first settle still refreshes the peak bucket
with the settle's `emax`, so everything looks fine; but ~24 s later the
two-bucket rotation drops the anchor out of the buckets, `aref_ref` falls below
the anchor, and the clamp **restores `aref_ref_em = anchor_em = 0`**. The
`AREF_PEAK_EMAX_MIN` gate then forces gate 4 (no usable peak) on every sample:
path-1 triggers stop; only the "less certain" absolute path (q ≥ 95…) can still
fire. Because a settle is needed to refresh the anchor metrics and a trigger is
needed for a settle, this is self-locking until a scene/zoom/toggle/RAW/user-AF
reset. I modelled the bucket/clamp sequence and it reproduces exactly:
`ref=115000, em=0, gate_em_ok=false` from t≈24 s onward.

**Minimal fix:** seed the anchor metrics from the settle sample as well, and make
the clamp defensive:

```c
aref_ref_anchor_em = MAX(old_em, emax);   /* emax/g are the settle's own sample */
aref_ref_anchor_g  = MAX(old_g, (uint32_t) g);
...
/* in aref_peak_update's clamp */
if (aref_ref_anchor_em > 0) { aref_ref_em = aref_ref_anchor_em; aref_ref_g = aref_ref_anchor_g; }
```

Also reset the anchor when the RT test starts/ends (MF3) so a prior scene's
anchor cannot be inherited.

**Verify:** `# aref done ... anchor=` non-zero while
`# aref ref anchor: seed ...` was logged with `pre=0`; no long stretch of gate 4
in `# aref ...`/`# RT_S gate=4` after ~24 s on a textured scene; a synthetic
settle+rotation model shows `aref_ref_em` staying ≥ 70.

### MF3 — the Refocus test does not clear the pre-test anchor or ref max

**Where (snapshot):** `aref_test_run` start `14530`; `afrt_reference`
`14498–14527`; `afrt_state_reset` `14431–14452` (clears the peak only);
`aref_test_task` end `14694–14697` (`afrt_state_reset(...,0); aref_forget=1;`,
no anchor/ref-max clear).

**Why it matters:** the aref anchor (and `afd_ref_max_s`) survive the test's
state reset. If the test is run after a walk (the plan's normal follow-up, and
any repeated test), HOLD1 starts with `aref_ref` clamped to the **pre-test**
anchor. If the RT subject is >15 % softer than the pre-test scene (same
`lv_dispsize`/mm bucket, no >25 % luma jump), path 1 fires during the 20 s HOLD
and `false_trig` is counted — the test fails its `false_trig=0` acceptance for a
reason unrelated to AF. Conversely a pre-test *ref max* that is lower/higher also
seeds the RT anchor on its first settle. After the test, the test scene's anchor
(and a possibly zero-metric anchor, MF2) is left for normal Auto refocus, and the
code comment/plan statement "the state is reset before the test and again at its
end" is no longer true for the new state.

**Minimal fix:** in `aref_test_task`, next to `aref_test_run()`:

```c
afd_ref_max_reset("rtest");   /* -> afd_ref_max_reset(...) */
aref_anchor_clear("rtest");
aref_test_run();
afrt_state_reset(get_ms_clock(), 0);
aref_forget = 1;
afd_ref_max_reset("rtest");
aref_anchor_clear("rtest");
```

(`afd_ref_max_reset`/`aref_anchor_clear` are safe from the test task — it is the
same context as `aref_eval`.) This also makes the test's own trigger reference
start from the test scene rather than a previous one.

**Verify:** in a post-patch session, the first `# RT_S` HOLD1 lines must show
`ref=` at the RT subject's own sharpness, not the previous walk's; run an RT
immediately after a different-subject walk and confirm `false_trig=0`. The
AFDRIVE log should show `# aref ref anchor: reset why=rtest` and
`# local: ref reset why=rtest` around the test.

### MF4 — the local-search fallback reference (`afd_last_land_s`) has no focal-length key and is still fed by non-mode-0 landings

**Where (snapshot):** state `8586–8587`; written in the shared verified-landing
block `9495–9504` (unconditional; only `afd_ref_max_update` is `track`-gated at
9499); used by `have_last` `9315` and the 70 % guard `9340`; also by B's
in-focus shortcut `9089` and warm/E `9273`.

**Why it matters:** patch 3 added `mm` to the ref-max key but the fallback path
still compares landing sharpness across focal lengths: if the ref max is invalid
for the current `(zoom, mm)` — the normal situation on the first run after an
optical zoom or a bucket change — `have_last` is true (it checks only
`lv_dispsize`) and `ref` becomes the *old focal length's* landing, so the 50 %
far check, the `soft0` guard and the MF1 guard compare a 50 mm picture with an
18 mm high-water. Independently, `afd_last_land_s` is still written by bench
(mode 6), speed-test (mode 3) and RT-prep (mode 8) landings, so the first walk
local run after a bench can bootstrap from a chart/scene reference. This is a
partial leak of the same class patch 3 fixed for the ref max.

**Minimal fix:** add `static int afd_last_land_mm;`, set it next to
`afd_last_land_zoom` in `afd_run` (`afd_last_land_mm = afd_mm_bucket();`), and
require it in the local path:

```c
int have_last = afd_last_land_s && afd_last_land_zoom == lv_dispsize
                && afd_last_land_mm == afd_mm_bucket();
```

and set `afd_last_land_s` in the verified block only when
`afd_ref_max_track` (or keep it for A/B compatibility and add a separate
"last mode-0 landing" used by `afd_run_local`). Clear `afd_last_land_mm` in
`afd_ref_max_reset` as well.

**Verify:** after an optical zoom with the digital zoom unchanged, the first
`# local s0=... ref=` must be 0 (or the new-focal landing), not the old one;
after a bench, the first field local run must show
`# local: ref max zoom set ...` / `ref=0`, not a bench value.

---

## 3. Should-fix findings

### SF1 — the anchor clamp defeats deliberate peak-clearing escapes and is not MPU-scoped

**Where:** clamp `13596–13601`; `aref_anchor_ok()` `13341–13344` (no method
check); `aref_post_check` "new scene → forget the old peak" `13920–13936`
(clear at `13933`); newref-timeout clear `13868–13873`; no method-change reset in
`aref_step` (`14143–14153` only compares `Auto_refocus`, not
`Auto_refocus_method`).

The clamp restores `aref_ref` to the anchor on the next `aref_peak_update`
after those `aref_peak_clear()` calls, so the "forget the old peak instead of
hunting" escape no longer sticks. This matters for the **Canon method**
(`mpu_m == 0`): a leftover MPU anchor is never lowered there (the confirmed-soft
counter is only fed by `aref_mpu_settle`), and `aref_sw_push`'s scene reset is
`mpu_m`-gated, so a stale MPU anchor can persist across a method switch. Fix:
gate the clamp with an "MPU anchor active" flag (or `Auto_refocus_method == 1`),
add `aref_anchor_clear("method")` on a method change, and in the post-check
new-scene branch also clear the anchor + reset the ref max (a new scene is a new
scene). Verify: switch Refocus method on a static scene; no persistent hunting
with `# aref trig ref=` stuck high.

### SF2 — the anchor is never reset when runs fail to settle

**Where:** `aref_mpu_finish` failure path `14085–14098`; confirmed-soft escape
only inside `aref_mpu_settle` (`13694–13711`); clamp `13596`.

`nopeak`/`lowtexture` runs set no `aref_mpu_newref`, so no settle runs and no
soft count; the clamp keeps the stale anchor and the peak decay cannot bring the
reference down. The result is an indefinite periodic failed-refocus loop (bounded
by the 20 s pause / failed-scene gate, but not self-healing) until a scene/zoom/
toggle/RAW/user-AF reset. Add a failure-streak counter in `aref_mpu_finish`
(`pct < 0 && (afd_nopeak_last || afd_lowcontrast)`, key-checked) and after N = 3
lower/clear the anchor and ref max (`aref_anchor_clear("fails")` +
`afd_ref_max_reset("fails")`), resetting the streak on any settle or scene
reset. Verify: force a low-contrast scene with a stale anchor; expect
`# aref ref anchor: reset why=fails` after ≤3 failed runs rather than a forever
loop.

### SF3 — a seed-raise does not reset the confirmed-soft streak

**Where:** `13659–13665` (raise from seed) does not touch `aref_ref_soft_n` /
`aref_ref_soft_best`, unlike the raise at `13680–13688`.

If the anchor is raised from the seed while a soft streak is in progress, the
same settle can immediately increment the streak and, at n = 3, reset the anchor
to the best of the *pre-raise* soft settles — potentially lowering the anchor
right after a genuine raise. Minimal fix: reset both counters in the seed-raise
branch. Verify: no `confirmed soft scene -> reset` immediately after a
`raise ref=` in the log.

### SF4 — the Refocus test's first trial local run has no local reference

**Where:** mode-8 prep is `track = 0` (`10269, 10286`), so the RT reference
landing does not seed `afd_ref_max_s`; after a test-start clear (MF3 fix) or on a
fresh boot, the first trial's local run has `ref = 0`, so the 50 %/92 % guards
and the MF1 guard are skipped. Not dangerous (the trial's `final_pct` scores the
result), but the plan's "nothing useful is lost" for mode 8 is not quite true.
Minimal fix: after `afrt_reference` succeeds, seed the ref max from the reference
landing (e.g. a small `afd_ref_max_seed(ref_s)` that sets the max for the current
key, bypassing `track`), or accept the one-run bootstrap and note it.

### SF5 — `afd_mm_bucket()` treats an invalid/0 focal length as a real bucket

**Where:** `8601–8609`; key comparisons `9316`, `8611`, and the eligibility
reset `14283–14292`.

A transient `lens_info.focal_len == 0` (Canon sets it to 0 for bogus values,
`src/lens.c:1739`) changes the bucket to 0, which clears the anchor/peak memory
and starts a new ref-max key; when the value returns, the key changes back and
resets again. One glitch = two resets + two settle waits; a flickering value
would keep Auto refocus permanently settling. Fix: treat `0` as "unknown" and
keep the last valid bucket (`static int last_mm;`), and/or require the changed
bucket to persist 2 samples before resetting in `aref_step`. Verify: no
`reset why=zoom` pairs without a real zoom.

### SF6 — inline `# stop_check` verdict does not match the offline analyzer's metric

**Where:** `AFW_STOP_HALT_DELTA_PCT 5` vs end-delta `10094–10131`; offline
`contrib/af_test/stop_analysis.py:59–60, 270–300` uses `HALT_SLOPE_PCT_S = 2.0`
**and** `HALT_EXCURSION_PCT = 1.0`, where the excursion is the *signed largest
change* after the stop (direction-aware), and the baseline is the last sample
**before** `t_stop`. The plan §5 acknowledges the baseline difference but says
the thresholds match, which is not true for the excursion (5 % end-delta vs 1 %
max excursion). A trace that rises 4 % then returns has `halted=1` inline but is
`continued`/`changed` offline. Options: mirror the script (baseline = last
sample `<= t_stop`, `max_excursion_pct`, 1 % bound) or relabel the inline line as
a coarse variant and keep the script authoritative. Verify: run both on the same
synthetic rising-then-returning trace and compare.

### SF7 — one menu SELECT handler is still unguarded (patch 2's R3 fix is incomplete)

**Where:** `afdiag_afsearch_select` `7386–7391` still calls
`task_create("afdiag_afsearch", ...)` directly; the new wrapper `10915` is used
by the other nine. Same historical risk (task created before the recording
check; the task itself returns on `RECORDING` at `7278`, but the task was already
created and the plan's premise is that the creation itself is the hazard). Fix:
use `afd_menu_task_create("afdiag_afsearch", afdiag_afsearch_task, "AF search")`.
The plan §6.1's "all menu task_create" claim then becomes accurate.

### SF8 — `af_report.py` does not surface the new RT integrity fields

**Where:** `contrib/af_report/af_report.py:332–352`. The verdict only checks
`false_trig` and `ok=a/b`; `notrig/valid/false_pass` are parsed nowhere, and the
raw `# RT_TRIAL` lines are truncated to 140 chars, cutting the new
`no_trigger/valid/false_pass` fields off. A session with one invalid trial prints
"Refocus test: only 4/5 ok" with no explanation. Fix: parse `notrig`/`valid`/
`false_pass` from the last `# RTEST` line (the `kv()` helper does) and add a
verdict line ("1 no-trigger trial (invalid)") / keep `ok` qualified; widen the
`# RT_TRIAL` truncation. Verify: a synthetic case with `notrig=1` reports the
invalid trial in the verdict.

### SF9 — plan/documentation corrections

- [AF-PATCH2-FIELD-PLAN.md](AF-PATCH2-FIELD-PLAN.md) §1 table row C and §4.3
  still describe the **removed** `AREF_REF_FLOOR_PCT` floor and its
  `# aref ref floor ... adopt_pct=` line; §4.2 still says "with the reference
  floor active". Patch 3 replaced them with the `# aref ref anchor:` lines —
  update §4 (the §11 note "supersedes" is easy to miss).
- §11.1 acceptance "no local run may land with `afd_land_s < 92 %` of `ref`" —
  the code guards `fmax`, not `land_s` (see §1.1); require
  `# local climb ... max_s=` ≥ 92 % of `ref=` and accept `land_s` slightly lower.
- §11.2 "nothing useful is lost" re mode 8 (see SF4).

---

## 4. Nits

- **N1 — `afd_abort_on_servo` can leak on an early engine return.** It is set at
  `10269`, before the `AFD_F_REC` precondition return `10273` and the monitor-open
  failures. A later `afd_must_abort` outside an engine run does not exist, so the
  leak is benign today, but set/reset it around the run body.
- **N2 — `search_ms` is overwritten by the monitor-stale retry** (`10408`,
  `10428–10430`): `total_ms` includes both searches, `search_ms` only the last
  attempt. Fine for the common case; note it when comparing to the bench.
- **N3 — `# stop_check` integer cast.** `(int) (bq * 100 / s_stop)` can overflow
  only for pathologically small `s_stop`/timestamp spacing; a
  `s_stop < 100 → insufficient` guard would remove the theoretical UB.
- **N4 — `final_pct_med` includes invalid trials** (`fin_v[nfin++]`, `14630`).
  A false-pass trial can raise the median; either exclude `no_trigger` trials
  from `fin_v` or say the median is "all trials".
- **N5 — `afd_last_land_s` is now cleared by events that also fire with Auto
  refocus OFF** (`aref_step` runs unconditionally, `14821`): user AF/toggle/RAW
  rec clear it, so A/B/E lose the "already in focus" shortcut after such an
  event. Harmless, but it is a behaviour change to A/B worth one line in the
  plan.
- **N6 — cross-task statics are not `volatile`:** `afd_ref_max_*` are written in
  the worker and now read in the aref context (seed/done/eligibility). Ordering
  is established through the volatile request/state flags and there is no
  concurrency in practice, so this is a note, not a defect.
- **N7 — `# refmax` logs for modes 6/8 go to the bench/test log buffer**
  (`afd_log_quiet = 2`), not AFDRIVE; that is consistent with the other engine
  lines but the plan should say where to look (`track=0` line in AFBENCH.LOG /
  AFRTEST.LOG).

---

## 5. What I could not verify

1. **Anything physical** (no camera): the anchor/ref-max behaviour on real
   scenes, the 3-settle escape, the 15 % luma guard, the MF1 92 % guard on a real
   end-stop flank, and whether `lens_info.focal_len` is stable enough for the mm
   key (a 0/glitch value is legal in `src/lens.c`).
2. **Compile:** no `make` (build owner) and no edit of `crop_rec.c`; this is a
   static review of the snapshot. The task-12 AFDIAG delta was not reviewed.
3. **The AFSTOP sequence on hardware:** the inline `# stop_check` math is
   verified against the code/struct origins and by unit reasoning; the real
   traces are only in the recovered corpus (the offline analyzer remains
   authoritative).
4. **A real post-patch session:** the ordering claims about HOLD1 false triggers
   under a stale anchor and the MF1 "always B" behaviour are derived from the
   code paths, not measured.
5. **A/B/C/D/E/F timing:** patch 2/3 do not change them beyond the notes above;
   no camera timing was measured.

---

## 6. Field-tester checklist delta (what the new lines mean)

Add these to the review-1 checklist; the old items still apply.

1. **Movie Servo OFF** for the RT test and every Auto-refocus run (patch 2
   aborts `AFD_F_AREF` engine runs on `is_continuous_af()`; the plan §11.7 covers
   this). A run that aborts with `pct=-1` on a servo setting is not an AF failure.
2. **Do not run the Refocus test after a walk on a different subject** until MF3
   is fixed (or power-cycle / toggle Auto refocus between them). Otherwise
   HOLD1 `false_trig` may be a stale pre-test anchor, not a real false trigger;
   check `# RT_S ... ref=` against the RT subject in the first HOLD lines.
3. **`# aref ref anchor:` lines** — trust this sequence:
   - `seed` on the first landing after boot/reset;
   - `raise` only when a genuinely sharper picture arrived;
   - `soft anchor=... raw=... n=1/2` on soft landings (does **not** lower the
     anchor);
   - `confirmed soft scene -> reset` after three in a row (intended adaptation);
   - **a `raise ref=<higher>` immediately after a `confirmed soft ... reset`, or
     `# local ... ref=` staying at the old max, is MF1** — record it and stop
     treating the anchor as fixed.
4. **`# aref ref anchor: reset why=scene|toggle|rawrec|foreign|zoom`** is a
   correct restart. If it appears without the matching event (esp. `zoom` with a
   steady lens), suspect a focal-length glitch (SF5).
5. **`# refmax ... track=`** — `track=0` for bench/RT-prep/speed-test landings
   (they must not change `now=`); `track=1` for field landings. A `track=0`
   landing followed by a field local run using it as `ref` is MF4.
6. **`# local: climb still soft on the drop path -> B`** on every refocus with a
   picture that looks sharp in the viewfinder is the stale-ref-max signature
   (MF1/SF2); record `# local ... ref=` and `# refmax` values.
7. **`# aref ref anchor: reset`/long stretches with `# RT_S ... gate=4`** after
   the picture has been sharp for >~24 s is the zero-metric anchor (MF2): note
   whether `# aref done ... anchor=` was seeded with `pre=0` before it.
8. **Refocus test:** read `# RTEST ... notrig=… valid=… false_pass=…`; `ok=5/5`
   is clean only with `valid=5`. `# RT_REF ref_s=` is the scoring reference,
   `meas=` the fresh measurement; `anchor kept` only after a failed static
   trial; `scene changed` on a luma change (>15 %) — note the luma, it is not an
   AF failure.
9. **`# RESULT ... search_ms=… epilogue_ms=…`**: compare `search_ms` with the
   bench `median_ms` (same basis); `total_ms + epilogue_ms` is the wall time.
   Ignore `search_ms` after a monitor-stale retry (N2).
10. **`# stop_check ... halted=…`**: the offline
    `python3 contrib/af_test/stop_analysis.py` verdict is authoritative; a
    borderline inline `halted=1` with a visible `s` excursion is expected (SF6).
11. **`# RT_TRIAL_INVALID`** — treat the session as "ok/5 with one invalid
    trial", not 5/5; `af_report.py` will not currently say so (SF8) — read
    AFRTEST.LOG directly for the integrity fields.

---

## 7. Independent checks performed

- Reconstructed the snapshot (`HEAD` + `/tmp/af_patch3_full.diff`) under
  `/tmp/snap3` and reviewed the applied file, not just the diff.
- Re-derived the MF1 guard ordering and the wide-probe return geometry against
  the review-1 model.
- Modelled `aref_peak_update`'s two buckets + anchor clamp (MF2): with a
  zero-metric anchor the clamp sets `aref_ref_em = 0` from t ≈ 24 s and keeps it
  there; with a valid anchor it does not.
- Traced the settle/seed/raise logic (MF1): a stale `afd_ref_max_s` re-raises the
  anchor on the next settle after every confirmed-soft reset.
- Built synthetic post-patch `AFDRIVE.LOG`/`AFRTEST.LOG` lines and ran
  `corpus.parse_rtest_facts`, `corpus.parse_drive_facts` and `af_report.py` on
  them: the adopted `ref_s` is parsed, the new RTEST/RT_TRIAL/RESULT/`# aref
  done` fields do not break the prefix/kv parsers, and the report runs clean.
- Ran `python3 contrib/af_test/run_all.py`: 78/78 unit tests, 4/4 tiers,
  6/6 corpus cases, `RESULT: PASS` (read-only; no camera/build).
