# AF patch field plan: local-search soft-picture acceptance + reference erosion

**Status:** implemented, **compile-checked only — no camera attached, NOT field-validated.**
**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` (local, not committed) · **Module:** `modules/crop_rec/crop_rec.c`
**Fixes:** [EOSM-AF-HANDOVER.md](EOSM-AF-HANDOVER.md) §4.1 (local search accepts a soft picture, the 3/5 Refocus test) and §4.2 (reference erosion).
**Background:** [EOSM-AF.md](EOSM-AF.md) §3.8, [EOSM-AF-TESTING.md](EOSM-AF-TESTING.md).

The patch touches only the Auto refocus local path (`afd_run_local`) and new engine-side reference
bookkeeping. Algorithms A/B/C/D/E/F, the aref trigger thresholds, the engine epilogue and the A/B
landing code are unchanged; a default (menu algorithm B) run behaves exactly as before except for the
new `# local: ref max ...` bookkeeping lines.

---

## 1. What changed (behaviour)

### 1.1 Local-search reference (`modules/crop_rec/crop_rec.c:9271`)

`afd_run_local` now computes its acceptance/fallback reference as:

```
ref = per-zoom ref max (afd_ref_max_s)  when valid for the current lv_dispsize
    = afd_last_land_s                  otherwise (first run / after a zoom change)
```

`afd_last_land_s` keeps exactly its old meaning and is still what A/B/C/D/E and the aref trigger use.
The ref max is engine bookkeeping (see 1.2); it is deliberately **not** reset by
`aref_peak_clear()`/`aref_peak_update()`, which rewrite the aref peak after every landing (that rewrite
is the second half of the erosion).

Consequences inside `afd_run_local`:

- the "far below the reference -> B" checks (`AFD_L_FAR_PCT`, 50 %) use `ref` instead of the last
  landing (`crop_rec.c:9301`, `crop_rec.c:9376`);
- the existing 70 %-of-**last-landing** plateau guard is kept as it was (`crop_rec.c:9309`);
- the in-focus exit is now conditional (1.3).

### 1.2 Per-zoom reference high-water mark (`crop_rec.c:8569-8614`)

New state, in the engine block (before the aref code, so `aref_peak_clear` cannot touch it):

```c
static uint32_t afd_ref_max_s;   /* running max of VERIFIED landing sharpness, same zoom */
static int      afd_ref_max_zoom;
```

Updated **only** in `afd_run()`'s verified-landing block (`crop_rec.c:9438-9443`), i.e. after the
300 ms landing verification and any re-land, never on an aborted/nopeak/lowtexture run:

- new zoom -> set from that landing (`# local: ref max zoom set ...`);
- landing above the max -> raise (`# local: ref max rise ...`);
- landing below `AFD_REF_RESET_PCT` (75 %) of the max -> reset to it (scene / light change proxy,
  `# local: ref max reset deep landing ...`);
- otherwise unchanged: the max does **not** follow the landing down, so a locally soft landing
  (82-92 %) can no longer erode it;
- `afd_ref_max_reset("scene")` is called from the aref scene-change detector (a >25 % luma jump in
  `aref_sw_push`, `crop_rec.c:13478`) and logs `# local: ref reset why=scene ...` through `afd_note`
  (it runs outside an AF run), so it survives in AFDRIVE.LOG with the next run's notes.

No time-based decay: the explicit scene-change signal and the 75 % deep-landing reset are the only ways
down. The 75 % threshold is intentionally far below the 82-92 % soft band the patch is fixing, so a soft
local landing cannot reset (re-create) the erosion.

### 1.3 Soft-picture wide probe and the drop path (`crop_rec.c:9310-9355`)

When neither `±2×fine` side improves by `AFD_L_IMPROVE_PCT` (`dir == 0`) **and**

- the best probe is below `AFD_L_SOFT_PCT` (92 %) of `ref`, **or**
- the run was triggered by the Auto refocus **drop path**,

the search first probes wider at `±AFD_L_WIDE_X × 2×fine` = `±4×fine`:
`P-p1 -> P+2p1` ("W"), then `P+2p1 -> P-2p1` ("W"), then it returns to the usual `±p1` probe point on the
better side and the existing climb/landing continues from there (so the band between `p1` and `2p1` is
still scanned by the fine climb). If the wide probe also shows no side better by
`AFD_L_IMPROVE_PCT`, the run falls back to algorithm B from where the lens is (`# local: wide still flat
-> B`) — B was already the local search's fallback and is the proven reliable algorithm.

The drop path is a real signal, not a heuristic: `aref_mpu_arm()` (`crop_rec.c:13789`) publishes
`afd_aref_trig_pct` (current sharpness as a % of the aref reference peak) and `afd_aref_trig_path`
(1 = relative drop, 2 = absolute / no usable peak) immediately before every `AFD_F_AREF` engine run;
both `aref_step` and the Refocus test call it. `drop` is `path == 2` or `path == 1 && pct <
AFD_L_TRIG_DROP_PCT` (90). On a drop-path trigger the in-focus exit is never taken, even when the local
probes look flat: this is exactly the handover's "82-83 % + flat flank + declared in focus" case.

Note the engine `pct = final_s*100/best` reported in `# RESULT` is **not** usable for this judgement on
the local path: `best` is the local climb max, so a soft local landing still reports pct ~100. All new
guards compare against the absolute per-zoom reference inside `afd_run_local`, as the lead noted.

### 1.4 Unchanged

- A/B/C/D/E/F default behaviour (only extra `# local:` bookkeeping lines in their logs).
- The aref trigger thresholds / dwell / settle / peak memory (`aref_eval`, `aref_peak_*`).
- `afd_last_land_s` / `afd_last_land_zoom` semantics.
- The single-exit epilogue in `afd_engine`, the abort/deadline checks: every new move goes through
  `afd_move_ex`, which checks `afd_must_abort()` and `afd_deadline` before every move and chunk.
- No new tasks, no card writes while recording (all new lines go into the normal RAM log buffers).

### 1.5 New constants (`crop_rec.c:8559-8566`)

| Constant | Value | Meaning / rationale |
|---|---|---|
| `AFD_L_SOFT_PCT` | 92 | No side improves and the best probe is below this % of `ref` -> still soft (field: the accepted soft landings were 82-83 % of ref; in-focus noise/sag stays above ~95 %). Wide probe first. |
| `AFD_L_WIDE_X` | 2 | Wide probe distance = `±AFD_L_WIDE_X * p1` = ±4×fine from the start (was ±2×fine). The handover's ±4×fine. |
| `AFD_L_TRIG_DROP_PCT` | 90 | An aref trigger below this % of its reference peak is the drop path. MPU sensitivity thresholds are 60/75/85 % (lowered by the hold penalty), so every relative trigger qualifies. |
| `AFD_REF_RESET_PCT` | 75 | A verified landing this far below the ref max is a different scene / light -> reset the max. Kept well below the 82-92 % soft band so soft landings cannot lower the max. |

`AFD_L_FAR_PCT` (50 %) is unchanged numerically, but now applies to `ref` rather than the last landing.

---

## 2. Expected log signatures

All new lines use only `%d` / `%x` / plain `%s` (ML snprintf). Lines from a local run go to
**AFDRIVE.LOG** (the engine's own log; the Refocus test's local runs are engine mode 0 inside the test,
so their `# local:` lines are in AFDRIVE.LOG, appended; correlate by time with `# RT_RUN` in
AFRTEST.LOG). In the AF bench the same text appears in **AFBENCH.LOG** via the bench buffer.

### 2.1 Per local run

```
# local s0=82000 plus=84000 minus=81500 dir=0 fine=20 last=96000 ref=115000 trig_pct=82 drop=1
# local: soft -> wide probe ref_pct=73
# local wide plus=91000 minus=80000 s0=82000
# local climb dir=1 max_s=101000 since=1 steps=2
# local in focus ...            (only when near ref / not a drop path)
```

Other possible lines:

```
# local: drop path -> wide probe ref_pct=0          (no usable ref, e.g. first run)
# local: wide still flat -> B                        (wide probe also flat: full search)
# local: far from the local reference -> B
# local: climb max far below the local reference -> B
# local: no peak within 8 steps -> B
# local: landing failed -> B
# local: flat but below 70pct of the last landing -> B   (unchanged guard)
```

### 2.2 Reference bookkeeping

```
# local: ref max zoom set zoom=5 s=115000          (first verified landing at this zoom)
# local: ref max rise 96000 -> 115000              (a sharper landing; expected when B recovers focus)
# local: ref max reset deep landing 115000 -> 82000 (a landing < 75 % of the max)
# local: ref reset why=scene s=115000              (afd_note line: >25 % luma jump, new scene)
```

### 2.3 Refocus test (AFRTEST.LOG)

Unchanged format; the new information is in the per-trial numbers:

```
# RT_TRIG def=60 run=1 ... pct=82 path=1 ...
# RT_RUN  def=60 run=1 pct=96 ms=3400 land_s=104000 moves=9 path=local nopeak=0 ...
# RT_TRIAL def=60 ... final_pct=96 settled=1 ok=1 ...
# RTEST false_trig=0 trig_ms_med=... ok=5/5 final_pct_med=95 runs_total=...
```

("fixed" = `ok 5/5`, `false_trig=0`, `final_pct_med >= 90`; the +60/+150 trials that used to land at
82-83 % should now show a `path=local` run that includes a `# local: ... wide probe ...` / `# local
wide ...` pair in AFDRIVE.LOG, or a `path=full` run after `# local: wide still flat -> B`, and
`final_pct >= 90`.)

### 2.4 What to accept as "working"

- Refocus test: **ok 5/5, false_trig 0** (target). Any trial with `final_pct < 90` is a failure and
  must be analysed: check whether it went `path=full` (B) or stayed `path=local` with a wide probe that
  found no slope.
- No `82-83 %` `# local in focus` / `land_s` results for a defocused trial.
- During a walk with several refocuses, `ref max` must not follow landings down: serial soft landings
  may still happen, but the reference should only drop via `ref reset` lines.
- Bench: A/B/C/D/E/F/B07/B14/B1F results should be statistically unchanged vs the baseline build
  (bookkeeping only); B remains the reliability reference. Extra `# local: ref max ...` lines are
  expected in AFBENCH.LOG and are harmless.
- Timing cost: a soft/flat local run now costs 2 extra measured moves (`# local wide ...`) before the
  decision, and a still-flat run continues into B (~5-8 s more). Expect occasional noticeably slower
  refocuses on hard/flat scenes; that is the intended trade for a correct landing.

---

## 3. First field-test sequence (no camera has run this yet)

Prerequisites: build+flash the zip from this branch, card writable, `AF diag` logging on, **Movie Servo
OFF** (patch 2 gates the `is_continuous_af()` abort to Auto-refocus engine runs, so a servo left ON makes
those runs abort instead of focus). Settings for Auto refocus: **monitor = Keep open**, **Refocus method =
MPU drive**, **Refocus AF = Local**, sensitivity **High/MPU (85 %)** for the trigger, x5 movie LV. Static,
textured subject; tripod or steady support for the Refocus test.

1. **Refocus test** (AF diag → Refocus test; 18 mm first). It runs the reference landing, a 20 s HOLD,
   the five defocus trials (+60, -60, +150, -150, +400), a 10 s HOLD and writes AFRTEST.LOG.
   Pass: **ok 5/5**, **false_trig 0**; inspect AFDRIVE.LOG around each `# RT_RUN` for the `# local`
   wide-probe lines and for `ref max` behaviour. Compare against the current baseline (3/5).
2. **AF bench at 18 / 35 / 50 mm** (AF bench, B speeds; monitor Keep open). This is the regression
   measure for the engine. Expect B unchanged; note that the bench's own B07 reference landings also
   feed the ref max (bookkeeping only).
3. **Auto-refocus walk** (normal Auto refocus, not the test): start in focus at 18 mm, walk the camera
   across the scene / between subjects at different distances, let Auto refocus work for a few minutes,
   then repeat at 35 and 50 mm. Watch: fewer soft landings, `# aref done ... path=local/full` lines,
   `# local: ref max ...` / `ref reset ...`, and the refocus latency. Note any repeated
   `wide still flat -> B` pattern and whether `ref reset deep landing` / `ref reset why=scene` appear
   (a persistent stale-high ref shows up as every refocus going to B).
4. Optional A/B: flash the previous build and repeat the same Refocus test / walk, so the deltas are
   from this patch only.
5. Copy `ML/LOGS/AFDRIVE.LOG`, `AFRTEST.LOG`, `AFBENCH.LOG` and run
   `python3 contrib/af_report/af_report.py --copy <dir>`.

Keep the monitor **Keep open** for all of the above; the engine paths above do not open/close it, and
never do this while RAW recording.

---

## 4. Rollback

The patch is uncommitted on `eosm-slim-gui`.

- Full rollback: `git checkout -- modules/crop_rec/crop_rec.c` (this document can stay), rebuild:
  `make -C platform/EOSM.202 zip SHELL=/bin/bash PYTHON=python3`, flash the zip.
- If only the aggressive decisions must be disabled without losing the ref bookkeeping, set
  `AFD_L_SOFT_PCT 0` (no soft probe) and/or make `drop` 0 in `afd_run_local`; or set
  `AFD_L_WIDE_X` back to 1 (= ±2×fine, i.e. the old probe reach). Rebuild after any change.
- If the build deletes `modules/crop_rec/module_strings.h` (python2 missing), recreate the 3-line
  placeholder (see handover §5) and rebuild.

---

## 5. Residual risks / open items

- **Unvalidated.** This is a compile-only patch; the first field run may show the thresholds
  (`92`, `90`, `75`) need tuning. They are isolated `#define`s.
- **Stale-high ref max after a gradual light/contrast change** (< 25 % luma jump, so no scene reset and
  no <75 % landing): the local search then wide-probes and falls back to B on (nearly) every trigger.
  Failure mode is slower refocus, not a softer landing; a zoom change or a scene change resets it. Check
  for `ref reset why=scene` / `ref max reset deep landing` if Auto refocus gets slow.
- **False triggers are now more expensive.** Every relative aref trigger qualifies as the drop path, so
  a false trigger on an in-focus scene does the wide probe and then B if flat. The aref hold/penalty
  logic (`aref_mpu_settle`) still limits repeat false triggers.
- **Wide probe near an end stop.** The +wide move may be refused (end-stop detection) and only the
  other side is usable; the code treats a refused move's sample like any other and falls back to B if
  nothing improves.
- **Timing.** +2 measured moves (~1.2 s) on soft runs, plus a B run when still flat; the local run's
  `AFD_RUN_CAP_B_MS` (18 s) includes the B fallback. A B fallback that hits the cap reports a timeout
  and counts as an aref failure, as before.
- **Climb branch still trusts its own local max.** If a side improves > 5 % but the climb finds only a
  shoulder, the landing can still be below the ref; only the 50 % far checks and the 300 ms drift verify
  guard this path. Not changed here (out of scope of §4.1).
- **Ref max across camera restarts** is RAM only (static), like the rest of the engine state; a module
  reload / power cycle clears it.
- The aref peak-memory erosion (the reference is re-taken from the post-landing picture) is addressed by
  the follow-up reference floor in §7 (task-6), not by the local-search changes above: task-5 only made
  the local search ignore the eroding aref peak for acceptance via `afd_ref_max_s`. Retuning the trigger
  thresholds remains out of scope.

---

## 6. Build verification

```
export PATH=$HOME/gcc-arm-none-eabi-5_4-2016q3/bin:$PATH
cd /Users/mgojkovic/repos/Crop-mood-eosm-slim-gui
make -C platform/EOSM.202 zip SHELL=/bin/bash PYTHON=python3
```

A full build plus repeated incremental rebuilds (forced `touch` of `crop_rec.c`): all exited 0;
`modules/crop_rec/crop_rec.o`/`.mo` were byte-identical between consecutive rebuilds (deterministic). The only
build warnings are pre-existing/environmental: a `printf` format warning in the host tool
`build_tools/xor_chk.c` and the ignored copies of `doc/INSTALL.pdf` / `doc/UserGuide.pdf` /
`src/logo.bmp`; **crop_rec.c compiles clean** (no warnings/errors). `platform/EOSM.202/autoexec.bin`
differs between builds only in the build date (`version.c`), as before.

---

## 7. Follow-up (task-6): Auto-refocus reference integrity

**Status:** implemented, **compile-checked only** — same caveat as the rest of this document.
**Fixes:** the aref reference ratchet (status review [AF-STATUS-REVIEW.md](AF-STATUS-REVIEW.md) §4.2 /
action A1) and the Refocus test's self-re-basing reference (same review, ratchet 2 / action A2).

> **Patch 3 supersedes this section's reference floor.** The adversarial review
> ([AF-PATCH-REVIEW.md](AF-PATCH-REVIEW.md), MF3) showed the 95 % floor below was a 0.95-per-settle
> ratchet and was rotated out of the 12–24 s peak, so it did not stop the trigger-reference erosion. Patch 3
> (task-13) replaced it with a true anchored reference (`aref_ref_anchor`) with a confirmed-soft-scene
> escape. Everything in this section about the Refocus-test anchor (`AFRT_REF_FLOOR_PCT`, now also with a
> luma guard) still applies; read §7.1 item 1 / §7.2 / §7.5 as "task-6 state, superseded", and see
> [AF-PATCH2-FIELD-PLAN.md](AF-PATCH2-FIELD-PLAN.md) §11 for the current behaviour and acceptance.

The task-5 fix stopped *local-search acceptance* from following a soft landing down, but the Auto refocus
trigger reference was still redefined after every "successful" landing: `aref_mpu_settle()` cleared the
peak and adopted the first settled post-landing sample unless it was < 70 % of the landing's own measured
s (and a soft-but-stable landing is always within 70 % of itself). So each soft landing moved the
reference down and the next trigger was measured against a lower bar. Separately, the Refocus test
re-took `ref_s` from a fresh B07 landing after every failed trial, so `ok 5/5` could be five passes against
a falling baseline.

### 7.1 New behaviour

1. **aref reference floor** (`crop_rec.c:13443`, `AREF_REF_FLOOR_PCT`):
   - before `aref_peak_clear()` the settle handler saves `old_ref = aref_ref`, its texture metrics
     (`aref_ref_em`, `aref_ref_g`) and `pre = aref_mpu_pre` (pre-trigger sharpness);
   - when `old_ref` is valid and the zoom is unchanged, `floor = AREF_REF_FLOOR_PCT (95) % of
     MAX(old_ref, pre)`;
   - the adopted reference is `MAX(settled med, landing-if-med-was-<70 %-of-landing, floor)` and is logged
     with `# aref ref floor: ...` when the floor is what raised it;
   - the old peak's texture metrics are kept with a floored value, so `AREF_PEAK_EMAX_MIN` / `AREF_MIN_G`
     do not reject it just because the settled sample is soft;
   - `# aref done ... new_ref=` now reports the reference actually adopted;
   - the hold/penalty logic still uses the real settled `med`, so repeated soft landings now compare as
     "not improved" and reach the existing hold/penalty path instead of sliding the reference down;
   - a > 25 % luma scene change clears `aref_ref` before the next settle, so the floor is skipped there
     (existing escape hatch); a zoom change does the same; a run with no prior peak (`old_ref == 0`) keeps
     the old behaviour.
2. **Anchored Refocus-test reference** (`crop_rec.c:14230`, `AFRT_REF_FLOOR_PCT`):
   - `ref_s` is now in/out; the first call sets it (session-start anchor);
   - on a re-reference, if the fresh measurement is below `AFRT_REF_FLOOR_PCT (95) %` of the current
     `ref_s`, the old value is kept and `# RT_REF anchor kept old=.. new=..` is logged; a genuinely
     sharper scene still raises the anchor;
   - trial defocus values, `AFRT_OK_PCT` (90 %) and all trigger/threshold behaviour are unchanged.
3. Everything from task-5 stays as is; A/B/C/D/E/F, the aref sensitivity/dwell tables and the engine
   epilogue are untouched.

### 7.2 New constants and rollback

| Constant | Value | Meaning | Disable |
|---|---|---|---|
| ~~`AREF_REF_FLOOR_PCT`~~ | ~~95~~ | **superseded by patch 3**: removed, replaced by `aref_ref_anchor` (see AF-PATCH2 §11.3) | n/a |
| `AFRT_REF_FLOOR_PCT` (`crop_rec.c:14128`) | 95 | a Re-focus-test re-reference may not fall below this % of the previous `ref_s` | `0` = old behaviour; `100` = pin the first reference |

Per-feature rollback: set the constant to 0 and rebuild (no other code change needed). Full rollback:
`git checkout -- modules/crop_rec/crop_rec.c`, rebuild and flash.

### 7.3 Expected log lines

```
# aref ref floor: old_ref=115000 pre=41000 med=95000 land=96000 adopt=109250 t=...
# aref done pct=96 land_s=104000 new_ref=109250 ms=3400 path=local mm=18 fine=20 post_close2_s=...
# RT_REF why=reland t=... land_s=... ref_s=94000 g=... luma=...
# RT_REF anchor kept old=115000 new=94000
```

`# aref ref floor` / `# aref done new_ref` are `afd_note` lines (held in RAM, written with the next
AFDRIVE log); `# RT_REF anchor kept` goes to AFRTEST.LOG. In a normal successful refocus (landing at or
above the pre-trigger reference) the floor line does not appear.

### 7.4 What "fixed" looks like

- Refocus test: `# RT_REF anchor kept` appears only after failed-trial re-references; a run of
  `# RT_REF ... ref_s=` values no longer staircases downwards; `RTEST ... ok=x/5` is then measured against
  the session-start reference, so a pass really means `final_pct >= 90` vs the original picture.
- Auto-refocus walk: `# aref done ... new_ref=` does not drift down after soft landings; the reference
  either stays floored or rises. Downward movement should only come from `ref reset why=scene` /
  zoom-change resets, or from a landing that is genuinely at least 95 % of the old reference.

  > **Patch 3 correction:** with the anchor, `new_ref`/`anchor=` stay constant across soft landings and
  > move down only after `# aref ref anchor: confirmed soft scene -> reset` (3 consecutive settles below
  > 92 % of the anchor) or a scene/zoom/mm/RAW/toggle reset. The old 0.95-per-settle `new_ref` staircase is
  > exactly what patch 3 removed (review MF3).
- Accepted new signature: repeated soft landings now produce `# aref hold scene in focus ...` (hold +
  penalty) instead of silently redefining the reference — see 7.5.

### 7.5 Stale-floor failure mode (accepted, documented)

If the scene genuinely becomes lower-contrast through a **gradual** change that stays inside the 25 %
luma scene gate, the floor keeps the old (higher) reference, so:
- the next trigger fires against the old bar and the refocus can run again;
- after two landings at the same sharpness without improvement, the existing hold/penalty path pauses MPU
  triggers for 10 s and lowers the trigger threshold by 10 points (up to 25), which bounds the repeat rate;
- the floor is also skipped on zoom change, on a > 25 % luma jump and when there is no old peak, and a
  genuinely sharper landing still raises the reference.

Failure mode is "a few extra refocuses / slower, then hold", **not** accepting a soft picture. If a field
log shows repeated `# aref ref anchor: soft` / `# aref hold` on a static scene, check the anchor key
(zoom/mm) and the RT anchor luma guard, and read AF-PATCH2 §11.3 / §11.9 before treating it as a regression.

### 7.6 Build verification (task-6)

Same command as §6; after the task-6 edits it exits 0, `crop_rec.c` compiles clean and the only warnings
are the pre-existing host-tool `xor_chk.c` format warning and the ignored `INSTALL.pdf` /
`UserGuide.pdf` / `logo.bmp` copy failures. The final `crop_rec.mo` is deterministic across rebuilds
(final `crop_rec.mo` md5 `0b14b42f9b89e9dd64541dc1fbc92da5`).
