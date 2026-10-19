# AF patch 2 field plan: test integrity, timing / reference logging, stop-check, safety guards

**Status:** implemented, **compile-checked only — no camera attached, NOT field-validated.**
**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` (local, uncommitted) · **Module:** `modules/crop_rec/crop_rec.c`
**Scope:** diagnostics and safety only. No search algorithm, trigger threshold, epilogue or landing change.
**Predecessors:** [AF-PATCH-FIELD-PLAN.md](AF-PATCH-FIELD-PLAN.md) (task-5/6: local-search soft-picture
fix, ref max, aref/Refocus-test reference floors — **left intact**), [AF-STATUS-REVIEW.md](AF-STATUS-REVIEW.md)
(actions A1/A3/A5, risks R3/R4/R7/R12).
**Diff:** `modules/crop_rec/crop_rec.c` +316/−53 vs `HEAD` (this includes the preserved pre-existing patch;
patch 2 alone adds ~150 net lines). `crop_rec.mo` md5 `664048be6a460578a6dfd62136a87f92`, deterministic
across two forced rebuilds.

This is a second, independent patch on the same file. The task-5/6 patch is untouched: the local search,
`afd_ref_max_s`, `AFRT_REF_FLOOR_PCT`, the trigger thresholds, the task-5 wide probe, the single-exit engine
epilogue and every landing path behave exactly as before.

> **Patch 3 update (task-13, §11).** The adversarial review found three must-fixes in the preserved patch;
> patch 3 implements them. `AREF_REF_FLOOR_PCT` **no longer exists** (replaced by the true reference anchor),
> `afd_ref_max_s` is now keyed by zoom + focal-length bucket and only mode-0 landings feed it, and the local
> search has a post-climb soft guard. Where this document still mentions `# aref ref floor` (§4.3, §7, §8)
> it describes the pre-patch-3 state: use §11 for the current behaviour and acceptance (the `# aref ref
> floor` line is now `# aref ref anchor: ...`).

---

## 1. Summary

| Item | What | Where | Status |
|---|---|---|---|
| A | Refocus-test integrity: a trial with `runs==0` is invalid, not a pass; false passes counted | `aref_test_run` `crop_rec.c:14358`, trial block `14449-14462`, summary `14484` | done |
| B | Timing separation: `search_ms` around `afd_run`, `epilogue_ms` measured explicitly, both in `# RESULT` | `afd_engine` `crop_rec.c:10214`, `10381`, `10437`, `10500`, `10517` | done |
| C | Reference observability: `# refmax`, `ref_max`/`land_vs_ref`/`anchor` in `# aref done`, `# aref ref anchor:` seed/raise/soft/reset lines (the patch-3/4/5 anchor replaced the old floor line) | `afd_run` verified-landing block, `aref_mpu_done_note`, `aref_mpu_settle` | done (anchor: patch 3, updated by patch 5 §11.11) |
| D | AFSTOP self-interpretation: inline `# stop_check` per trial (`halted=0/1`) | `afw_stop_check` `crop_rec.c:10059-10093`, call `10168` | done |
| E(i) | `task_create` guarded by "not recording" in every menu SELECT handler | `afd_menu_task_create` `crop_rec.c:10868`, 9 call sites | done |
| E(ii) | NODONE timeout in `afd_move_ex` sends an explicit `(9,3,1)` and logs it | `crop_rec.c:8120-8128` | done |
| E(iii) | `is_continuous_af()` abort — **gated to Auto-refocus engine runs only**, not unconditional | `crop_rec.c:7664`, `7701`, `10230` | done (gated); see §7 |

---

## 2. A — Refocus-test integrity (`AFRTEST.LOG`)

**Problem (status review 4.2 / D18, card18):** the `def=400` trial never triggered yet was scored
`ok=1` at `final_pct=98 %` of an already-eroded `ref_s`. The summary `ok=x/5` could not distinguish
"5 real refocuses" from "4 triggers + 1 stationary picture".

**Change** (`aref_test_run`, `crop_rec.c:14358`):
- after the trigger loop, `no_trigger = runs == 0`, `valid = 1 - no_trigger`;
- `ok` is only incremented for a **valid** trial (`valid && final_pct >= AFRT_OK_PCT && settled`);
- `false_pass = no_trigger && final_pct >= AFRT_OK_PCT` — shown in `# RT_TRIAL` and counted in the summary;
- `# RT_TRIAL` gains `no_trigger=… valid=… false_pass=…`; a no-trigger trial also emits a short
  `# RT_TRIAL_INVALID …` line;
- `# RTEST` gains `notrig=… valid=… false_pass=…` **after** the old fields, so `ok=x/5` stays where it was
  for existing parsers (`contrib/af_test/corpus.py:262`, `contrib/af_report/af_report.py:334` match a
  prefix, so the trailing fields are compatible);
- the final NotifyBox says "{n} no-trigger" so a field operator sees it without the card.

**Not changed:** trial defocus values (`60, -60, 150, -150, 400`), `AFRT_OK_PCT 90`, the trigger logic,
HOLD times, the reference anchor (`AFRT_REF_FLOOR_PCT`), and the re-reference decision
(`if (fin_pct < AFRT_OK_PCT) need_ref = 1;`).

**Log lines** (AFRTEST.LOG):
```
# RT_TRIAL def=400 s_def=… trig_ms=-1 runs=0 run_ms=0 … final_pct=98 settled=1 ok=0 ref_s=… no_trigger=1 valid=0 false_pass=1
# RT_TRIAL_INVALID def=400 reason=no_trigger valid=0 ok=0 false_pass=1
# RTEST false_trig=0 trig_ms_med=… ok=5/5 final_pct_med=… runs_total=… notrig=0 valid=5 false_pass=0
```
Reading:
- `notrig=0 valid=5` + `ok=5/5` = the only clean pass;
- `notrig=1 valid=4 ok=4/5` = one invalid trial (was previously possibly `ok=5/5`): treat the session as
  "4/5 with 1 invalid", **not** 5/5;
- `false_pass=1` = the invalid trial's picture was still >= 90 % of the reference; this is a test-setup
  problem (defocus did not produce a trigger: end stop, too little defocus at +400, or the aref gates
  blocked), **not** a successful refocus. Check `# RT_S` gate codes around it and `# RT_REF` reference.

---

## 3. B — Timing separation (`AFDRIVE.LOG` `# RESULT`)

**Problem (A3):** `total_ms` conflates the AF search with LV wait / session open (~300 ms) and the field
never knew how much of "6 s" was the epilogue or setup.

**Change** (`afd_engine`, `crop_rec.c:10214`):
- `search_ms` = wall time around the `afd_run(&best)` call only (`10381`), i.e. the algorithm itself,
  including its landing verification / re-land;
- `epilogue_ms` is **measured explicitly** (`t_epi` at `10437`, `epilogue_ms` at `10500`): stable pre-stop
  sample + `(9,3,1)` stop + 500 ms + sample + session close + 300 ms + sample + wait-to-1 s + sample
  (+ the optional restore landing), up to just before `# RESULT`;
- `total_ms` keeps its **old meaning**: `get_ms_clock() - afd_t0` captured *before* the epilogue. Wall time
  of a menu run is therefore `total_ms + epilogue_ms`. This was chosen over "total includes epilogue" so
  that new logs stay comparable with the existing field logs and with the bench (which times `afd_run`
  only). Both new fields are added after `total_ms` in the mode-0 `# RESULT` line
  (`crop_rec.c:10517`); the other modes' `# RESULT` lines are unchanged.

**Log line** (AFDRIVE.LOG, mode 0):
```
# RESULT aborted=0 best_pos=… … total_ms=5400 search_ms=3900 epilogue_ms=1540 end_pos=… travel=… algo=B
```
Reading:
- compare `search_ms` with the bench `median_ms` (bench times the search only): same-basis numbers;
- `total_ms - search_ms` = LV wait + session open (300 ms) + stale handling; a large value means the run
  spent time in the engine wrapper, not in the search;
- `epilogue_ms` should be stable ~1500-2100 ms; a much larger value means the optional "Restore" landing
  ran (`# restore …` line above it).

---

## 4. C — Reference observability (AFDRIVE.LOG / `afd_note`)

### 4.1 `# refmax` per verified landing (`afd_run`, `crop_rec.c:9457-9464`)

Logged inside the existing verified-landing block, after the 300 ms verify / re-land, before/after
`afd_ref_max_update`:

```
# refmax prev=115000 land=82000 now=115000 zoom=5 mm=20 track=1 last_track=1
```
`prev` = ref max before this landing (on a zoom change it belongs to the old zoom), `land` = the verified
landing that fed the local reference, `now` = ref max afterwards (`track=1` = a mode-0 landing fed it;
`last_track=1` = it also updated the local-search fallback landing, patch 5 / MF4). A soft landing is
visible as `land` ~82-92 % of `now` on one line. The pre-existing `# local: ref max zoom set/rise/reset`
lines are unchanged (plus `# local: ref max reset confirmed soft ...`, patch 5 / MF1).

### 4.2 `# aref done` absolute reference fields (`aref_mpu_done_note`, `crop_rec.c:13532-13540`)

`ref_max=` (0 = no valid ref max for this zoom) and `land_vs_ref=` (landing as % of `afd_ref_max_s` when
valid, else 0) are appended:

```
# aref done pct=96 land_s=104000 new_ref=109250 ms=3400 path=local mm=18 fine=20 post_close2_s=… ref_max=115000 land_vs_ref=90
```
Reading: `new_ref` is the reference actually adopted, `anchor=` the true reference anchor (patch 3+). A
soft landing is visible as `land_vs_ref` 82-92 with `new_ref`/`anchor` staying high: that is the anchor
keeping the reference, **not** a recovered one. `ref_max=0` / `anchor=0` means no reference at this
zoom/mm yet (first run / reset) — compare against `# local s0=… ref=0`.

### 4.3 `# aref ref anchor:` lines (`aref_mpu_settle`, patch 3/4/5)

The old `# aref ref floor ... adopt_pct=` line was removed in patch 3 (the 0.95-per-settle floor is gone).
The anchor reports through `afd_note`:

```
# aref ref anchor: seed zoom=5 mm=20 ref=115000 pre=41000 t=...
# aref ref anchor: raise ref=121000 zoom=5 mm=20 t=...
# aref ref anchor: soft anchor=115000 raw=95000 n=1 t=...
# aref ref anchor: confirmed soft scene -> reset ref=95000 n=3 t=...
# local: ref max reset confirmed soft old=115000 new=95000 t=...
# aref ref anchor: reset why=scene|toggle|rawrec|foreign|zoom|method|timeout|newscene|failed runs|rtest was=115000 t=...
```

Reading: `seed` = first anchor at this zoom/mm; `raise` = a genuinely sharper settled picture; `soft n=1/2`
= soft settles that did NOT lower it; after 3 (`AREF_REF_SOFT_N`) the anchor and (patch 5 / MF1) the
local-search ref max are lowered to the best of them. Any `reset` line is a deliberate restart (with its
reason). A `raise` immediately after `confirmed soft scene -> reset` would be the patch-3 MF1 bug and must
not appear after patch 5.

---

## 5. D — AFSTOP self-interpretation (AFSTOP.LOG, mode 5)

**Problem:** the recovered 2026-10-19 AFSTOP logs show the lens still moving for 300-500 ms after
`t_stop` (post-stop least-squares |slope| 8.3-406 %/s, typical ~43 %/s), but the raw log needed an
offline script (`contrib/af_test/stop_analysis.py`) to see that. Every new trial now carries the verdict
inline.

**Change:** `afw_stop_check` (`crop_rec.c:10059-10093`), called after `# done` for each of the four stop
trials (`crop_rec.c:10168`), computes from the recorded `s1` samples of that trial (integer math, `int64_t`
accumulators, **no floats, no allocation, no movement**):

- `t_stop` = `r.stop_ms` (the actual `(9,3,1)` send time, -1 if the move finished before the planned stop);
- `s_stop` = first sample with `t_ms >= t_stop` (what the lens showed right after the stop request);
- `s_end` = last sample of the trial;
- `delta_pct = (s_end - s_stop) * 100 / s_stop`;
- `slope_pct_s` = least-squares slope of `s1` over the post-stop samples, in % of `s_stop` per second;
- `npost` = post-stop samples used;
- `halted=1` when `|slope_pct_s| <= 2` **and** `|delta_pct| <= 5`; else `halted=0`.
  The 2 %/s bound deliberately matches `contrib/af_test/stop_analysis.py` (`HALT_SLOPE_PCT_S = 2.0`,
  chosen there because every real trace has |slope| >= 8.3 %/s). `insufficient=1` (+ all numbers 0/
  `t_stop=-1`) means no stop was sent or fewer than 3 post-stop samples exist — no verdict.

**Log line** (AFSTOP.LOG), one per trial:
```
# stop_check k=1 t_stop=161 s_stop=101000 s_end=80000 delta_pct=-20 slope_pct_s=-43 halted=0 npost=11 insufficient=0
# stop_check k=3 t_stop=161 s_stop=101000 s_end=100800 delta_pct=0 slope_pct_s=1 halted=1 npost=12 insufficient=0
```
Reading:
- `halted=0` with a large `|slope_pct_s|` = **`(9,3,1)` did not halt the tracked `(9,1)` move** (the
  historical result; the B sweep's `moved`/`pos_pk` position model and C/G depend on this);
- `halted=1` on every trial = **the stop works**; this changes the assumptions of the B sweep position
  estimate and makes C/G/predictive braking viable. Re-run `python3 contrib/af_test/stop_analysis.py`
  on the log; the two verdicts should agree (inline uses the sample at/after stop, the script uses the
  last sample before it; a borderline trace can differ, the script is authoritative);
- `delta_pct` sign uses the raw `s1` change (up = sharper); combine with `dir` from `# stop_plan` to see
  whether the lens continued in the commanded direction;
- if `t_stop=-1`, the planned stop never fired because FOCUS_DONE arrived first (the tracker stops only
  while `done_ms < 0`); that trial says nothing about the stop.

Diagnostics only — no timing, stop, sample or landing code was changed.

---

## 6. E — Safety guards

### 6.1 E(i) — menu `task_create` cannot run while recording (`crop_rec.c:10868`)

**Problem (R3/D16):** the menu SELECT handlers called `task_create(...)` before `afd_task_common` checked
recording. mlv_lite owns almost all memory during RAW recording, so that task_create can fail or, worse,
break mlv_lite (the historical `compress_task` failure).

**Change:** one helper used by all nine AF-diagnostic menu actions (drive, calib, probe, speed, sweep,
stop, bench, reversal cal, Refocus test):

```c
static void afd_menu_task_create(const char *name, void (*entry)(void *), const char *label)
{
    if (RECORDING || RECORDING_RAW || mlv_raw_rec_busy())
    {
        NotifyBox(4000, "%s: not while recording", label);
        afd_note("# menu task refused name=%s reason=recording t=%d\n", name, get_ms_clock());
        return;
    }
    task_create(name, AFD_TASK_PRIO, 0x3000, entry, (void *) 0);
}
```

No task is created on refusal; the `afd_note` line is flushed with the next AFDRIVE log (RAM only, no
card write while recording). The AF REC test (`afd_rect`) and the monitor-close action already had their
own pre-checks and are unchanged. The engine's own post-menu-close recording checks are unchanged too.

Field signature if it ever fires (e.g. half-press/recording race):
```
# menu task refused name=afdiag_afdrive reason=recording t=…
```
plus the NotifyBox "AF drive: not while recording". Before this patch the action would have created the
task and only then failed in `afd_task_common`.

### 6.2 E(ii) — NODONE timeout sends an explicit stop (`crop_rec.c:8120-8128`)

**Problem (R7):** in `afd_move_ex`, when FOCUS_DONE times out (`done_ms < 0`) the remaining chunks are
dropped (correct: never send on top of a running motor) but **no stop was sent**, so the motor could
still be running while the caller sampled or sent the next command. The `afd_move_track` (sweep/stop test)
path already sent a stop; the engine path did not.

**Change:** on the NODONE branch, send `(9,3,1)` immediately and log
```
# nodone stop phase=<phase> n=<pulses of the chunk> t=<ms since run start>
```
The move is still counted as `n_nodone` in `# RESULT`; `afd_pos` keeps its existing commanded accounting;
nothing about end-stop detection or the rest of the move changes. The stop is harmless if the motor was
already stopped and is the same command the epilogue sends.

### 6.3 E(iii) — `is_continuous_af()` abort: **Auto-refocus runs only**, not unconditional

**Decision:** `is_continuous_af()` is **not** added unconditionally to `afd_must_abort` (R4). On this
platform it returns the persistent **menu setting** (`continuous_af_movie` / `continuous_af_photo`,
`src/focus.c:901`), not "a servo is actively driving the lens now". In movie mode (the mode every MPU AF
test runs in) Movie Servo is a normal setting, so an unconditional check would abort **every** menu AF
drive / AFSTOP / AFSWEEP / bench run at the first move — i.e. it would break the diagnostics this patch is
supposed to improve, including the A3 field session.

**Implemented instead:** a gated check that only applies to the Auto-refocus engine path:
- `afd_abort_on_servo` (`crop_rec.c:7664`), set from `flags & AFD_F_AREF` at `afd_engine` entry
  (`crop_rec.c:10230`) and cleared before return;
- `afd_must_abort` (`crop_rec.c:7692`) returns abort when `afd_abort_on_servo && is_continuous_af()`
  (`crop_rec.c:7701`).

Effect: menu AF (drive/calib/probe/speed/sweep/stop/bench/reversal/Refocus-test prep, and the REC test)
is unchanged. Auto-refocus runs already refuse to arm while servo is on (`aref_blk_mask` gate 4,
`crop_rec.c:13674`); the gate now also covers the race where servo is switched on **between** the trigger
and the engine run. Such a run aborts (result `-1`, counted as a failed refocus, normal cooldown/failure
path) instead of fighting the Canon servo. The Refocus test cannot start with servo on either
(`aref_blk_mask(0,1)` is checked at test start), so its numbers do not change.

---

## 7. Interaction with the existing patch (task-5/6)

- **No search/trigger/landing change.** `afd_run_local`, the wide probe, `afd_ref_max_s`,
  `AFRT_REF_FLOOR_PCT`, the aref gates, the epilogue and the single-exit rule were untouched by patch 2.
  (Patch 3-5 later revised the reference bookkeeping around them; see §11.)
- New `# refmax` / extended `# aref done` lines are **additional observability** for the reference
  bookkeeping. `ref_max` / `land_vs_ref` read `afd_ref_max_s` exactly as `afd_run_local` does (same
  zoom+mm check).
- `AFRT_REF_FLOOR_PCT` (Refocus-test anchor) and the aref anchor (`aref_ref_anchor`, patch 3+) are the
  reference-integrity controls; item A only stops an invalid trial from being scored.
- Item B changes no timing, only measures it; the epilogue block is untouched.
- Parsers: `contrib/af_report/af_report.py`, `contrib/af_test/corpus.py` parse the old fields by regex /
  prefix and ignore the appended fields (`ok=x/5` and all old names/order are preserved). The new
  `# stop_check` line is ignored by `contrib/af_test/stop_analysis.py` (it keys on `# stop k=`,
  `# stop_plan`, `# done k=` and `S ` lines). `# RT_TRIAL_INVALID` starts with `# RT_TRIAL` only as a
  prefix of a different string; corpus parsing filters on `# RT_TRIAL ` (with the space), so it is
  ignored.

---

## 8. Expected next field-session output (acceptance)

Run the same protocol as [AF-PATCH-FIELD-PLAN.md](AF-PATCH-FIELD-PLAN.md) §3 (Refocus test first, then
AF bench B speeds, then an Auto-refocus walk; monitor **Keep open**; never while recording). In addition:

1. **AFSTOP test** (AF diag → AF stop test) at 18 mm, one run. Accept as "understood", not "passed":
   ```
   # stop_plan k=1 dir=1 at=80 start_rel=-100 cmd=100 speed=7
   # stop k=1 t_stop=… t_done=… val=… sent=100
   # stop_check k=1 t_stop=… s_stop=… s_end=… delta_pct=-XX slope_pct_s=-43 halted=0 npost=…
   ```
   `halted=0` on all four trials with |slope| >> 2 %/s = the historical "stop does not halt" result is
   reproduced; then `python3 contrib/af_test/stop_analysis.py <dir>` must agree. If `halted=1` appears,
   re-run and check the samples: that would be new evidence that changes the C/G/predictive-braking
   assumptions.
2. **Refocus test** (card18-style, 18 mm, 5 trials) — expected post-patch shape:
   ```
   # RT_TRIAL def=60 … runs=1 final_pct=96 settled=1 ok=1 ref_s=115624 no_trigger=0 valid=1 false_pass=0
   # RT_TRIAL def=-60 … runs=1 final_pct=95 settled=1 ok=1 …
   # RT_TRIAL def=150 … runs=1 final_pct=≥90 settled=1 ok=1 … no_trigger=0 valid=1 false_pass=0
   # RT_TRIAL def=-150 … runs=1 final_pct=≥90 … no_trigger=0 valid=1 false_pass=0
   # RT_TRIAL def=400 … runs=1 final_pct=≥90 … no_trigger=0 valid=1 false_pass=0
   # RTEST false_trig=0 trig_ms_med=… ok=5/5 final_pct_med=… runs_total=5 notrig=0 valid=5 false_pass=0
   ```
   **Acceptance additions:** `notrig=0`, `valid=5`, `false_pass=0`, `ok=5/5` measured against a
   non-staircasing `ref_s` (the `# RT_REF anchor kept` lines only after genuinely failed trials), and
   every `# RT_TRIAL` with `runs>=1`. `notrig>0` or `false_pass>0` = the test setup, not the patch,
   needs fixing before the 5/5 number is trusted (a no-trigger trial is **not** a pass).
3. **AF drive** once (tripod, textured subject) with the epilogue instrumentation:
   ```
   # RESULT aborted=0 … total_ms=≈6xxx search_ms=≈4xxx epilogue_ms=≈15xx end_pos=… travel=… algo=B
   # refmax prev=… land=… now=… zoom=5
   ```
   **Acceptance additions:** `search_ms` and `epilogue_ms` both present, non-zero; `total_ms` comparable
   with older logs (same meaning); search time now directly comparable with the bench median.
4. **Auto-refocus walk**: in AFDRIVE.LOG each completed MPU run shows
   ```
   # aref done … new_ref=… ref_max=… land_vs_ref=…
   # refmax prev=… land=… now=…
   ```
   and, after repeated soft landings, `# aref ref anchor: soft … n=…`; after three in a row,
   `# aref ref anchor: confirmed soft scene -> reset` + `# local: ref max reset confirmed soft …` (patch
   5). Expected: `land_vs_ref` may still be 82-92 on a soft landing, but `new_ref` / `ref_max` /
   `anchor` do not staircase down; downward movement only via a confirmed-soft reset (3 settles), a scene
   reset, or a genuinely lower-but-verified landing. Record `land_vs_ref` values and the anchor lines.
5. **Recording safety spot-check:** with RAW recording running, select AF drive from the menu: expect
   "AF drive: not while recording", no task created, no crash, recording continues (the log line
   appears only if the note buffer is flushed later). Run the AF REC test once after stopping: it must
   still arm normally (it has its own pre-check).
6. If a NODONE occurs (possible on a long/fast move), `# nodone stop phase=… n=… t=…` must appear next to
   the move's `nodone` flag, and the next measured move must still sample a still lens.

---

## 9. Rollback

The patch is uncommitted on `eosm-slim-gui`. Everything is in `modules/crop_rec/crop_rec.c`.

- **Full rollback of patch 2 only:** restore the file from before this patch. There is no separate commit
  (the task-5/6 patch is uncommitted too), so `git checkout -- modules/crop_rec/crop_rec.c` reverts
  **both** patches. Rebuild + flash:
  `export PATH=$HOME/gcc-arm-none-eabi-5_4-2016q3/bin:$PATH; make -C platform/EOSM.202 zip SHELL=/bin/bash PYTHON=python3`
- **Per-feature rollback (rebuild after each):**
  - A: delete the `no_trigger/valid/false_pass` block and restore
    `tok = fin_pct >= AFRT_OK_PCT && settled;` in `aref_test_run` (the summary's extra fields may stay,
    they would read 0).
  - B: remove the `search_ms`/`epilogue_ms` fields (timing variables are harmless if left unused, but the
    compiler may warn; remove them with the fields).
  - C: the three log lines are pure formatting; remove the extra `# refmax` call / drop the appended
    fields. No state is involved.
  - D: remove the `afw_stop_check` call; the helper can stay unused (would warn `defined but not used` if
    `static` — remove it too).
  - E(i): replace `afd_menu_task_create(...)` with the old direct `task_create(...)` calls.
  - E(ii): remove the `send(9,3,1)` + `# nodone stop` log from the NODONE branch.
  - E(iii): set `afd_abort_on_servo = 0` in `afd_engine` (or delete the flag and the `afd_must_abort`
    line) to return to pre-patch behaviour for Auto refocus too. Menu AF is unaffected in any case.
- **Build side effects:** if the build deletes `modules/crop_rec/module_strings.h`, recreate the 3-line
  placeholder (see `doc/EOSM-AF-HANDOVER.md` §5); after building run
  `git checkout -- modules/crop_rec/README.rst` if the build overwrote it (it did not here).

---

## 10. Residual risks / open items

- **Unvalidated.** Compile-only; no camera attached. The new lines may reveal that the assumptions in this
  document are wrong (e.g. `epilogue_ms` much larger than 2 s when Restore is used; `halted=1`).
- **`search_ms` does not include the LV wait / session open** (those are inside `total_ms`); do not add it
  to bench medians without accounting for the bench's own warm-up.
- **`epilogue_ms` includes the optional Restore landing** (`Session close = Restore`) and the monitor
  close; it is a wall-clock diagnostic, not a constant. The split `total_ms` (pre-epilogue) / `epilogue_ms`
  is documented above; an analyst who adds them gets the wall time.
- **A no-trigger trial is now invalid, so `ok` can go DOWN.** A session that used to print `ok=5/5` may
  print `ok=4/5 notrig=1`. That is the intended correction, not a regression; the field report must read
  `valid`/`notrig`, not just `ok`.
- **`false_pass` is only a detection, not a fix.** If `def=400` still never triggers, the defocus value /
  end-stop / aref gates are the problem (out of scope here; `AFRT_NTRIAL`/`afrt_defs` are unchanged).
- **Stop check is a heuristic.** It uses the `s1` samples of the tracked move; a small post-stop sample
  count (`insufficient=1`), a very noisy scene, or a stop that fires before the first sample can give no
  verdict. The offline `contrib/af_test/stop_analysis.py` (full excursion model) remains authoritative.
- **NODONE stop assumes `(9,3,1)` is safe to send repeatedly / when idle.** It already is sent by the
  epilogue and by the tracker, so this adds no new command; but if the stop itself corrupts the MPU move
  state, this path now does that earlier. The event is logged (`# nodone stop`) so a regression is
  attributable.
- **Servo gating is best-effort.** It covers the trigger→run race for Auto refocus and leaves menu AF
  usable. It cannot detect an active Canon servo from code (only the setting); a servo that starts while
  a *menu* run is moving the lens is still unguarded by design (status review R4 remains partly open).
- **New log lines cost a little RAM** in already near-full buffers (`# refmax` ≈ 45 bytes per verified
  landing, `# stop_check` ≈ 110 bytes × 4, `# nodone stop` ≈ 50 bytes per NODONE, `# menu task refused`
  in the note buffer). AFSTOP's own 160×~40 B sample buffer dominates; no card writes change.
- **`crop_rec.mo` size** grows slightly (new strings + stop-check code); the zip still built and linked
  within the platform limits (441 KB `magiclantern.bin` region unchanged in shape).

---

## 11. Patch 3: review must-fixes (task-13)

**Status:** implemented, **compile-checked only — no camera attached, NOT field-validated.**
**Source:** [AF-PATCH-REVIEW.md](AF-PATCH-REVIEW.md) verdict "flash after fixes", must-fixes MF1–MF3 plus
should-fix SF1/SF3/SF4 and nit N1/N3/N5. **This section supersedes the reference-floor acceptance in
[AF-PATCH-FIELD-PLAN.md](AF-PATCH-FIELD-PLAN.md) §7** (the 95 %-per-settle floor is gone; it was a ratchet,
not an anchor). Patch 1 (task-5/6) and patch 2 remain functionally intact.

### 11.1 MF1 — post-climb soft guard (the 82–83 % class)

`afd_run_local` computed the soft selector only inside `if (!dir)`: a noisy ±2×fine probe could beat
105 % of `s0`, set a direction, skip the wide probe, and the climb could stall on a shoulder at 60–92 % of
the reference. Now `soft0` is computed once (`crop_rec.c:9349`) and, after the climb and the no-peak
check, before any landing (`crop_rec.c:9420`):

```c
if (ref && afd_below(fmax, ref, AFD_L_SOFT_PCT) && (drop || soft0))
    -> "# local: climb still soft on the drop path -> B"; afd_path_local = 0; return afd_run_sweep(...)
```

The existing 50 % far check stays (it is a subset, kept for log continuity). **Acceptance:** while
`drop=1`, no local run may reach the landing with the climb max below 92 % of the `ref=` printed on its
`# local s0=` line (`# local climb ... max_s=` >= 92 % of `ref=`; `land_s` may be slightly lower, the
guard is on `fmax`); a soft scene must show either the wide probe or `-> B`, never a bare local landing.

Also from the review (N4, cheap): the wide direction is now the argmax of `s0/sp/sm/swp/swm` instead of
the wide pair only (`crop_rec.c:9371-9386`).

### 11.2 MF2 — ref-max key and pollution

`afd_ref_max_s` was keyed only by `lv_dispsize` (digital zoom) and was written by every `afd_run`, so a
50 mm bench landing could become the 18 mm walk reference and bench / RT-prep / speed-test landings fed it.

- New key part `afd_ref_max_mm` = `afd_mm_bucket()` (`crop_rec.c:8602-8609`): `lens_info.focal_len`
  rounded to 10 mm; 0 / >400 mm = unknown. A zoom **or** focal-length-bucket change starts a new max
  (`# local: ref max zoom set zoom=.. mm=..`); rise/deep-reset lines also carry `zoom=`/`mm=`.
- Update is gated to **engine mode 0 only**: `afd_ref_max_track` is set in `afd_engine` and checked in
  `afd_ref_max_update`. Mode 6 (bench), mode 3 (speed), mode 7 (reversal cal) and mode 8 prep landings do
  not write the max. Mode 8's prep landing DOES write the local-search fallback `afd_last_land_s`
  (`afd_last_land_track`, patch 5 / MF4), so the Refocus test's first trial has a `ref=` from the RT
  reference landing; the trial runs themselves are mode 0 and update the max as usual.
- `# refmax ... zoom=%d mm=%d track=%d` (`crop_rec.c:9502`) shows whether a landing fed the max.
- Resets where the aref memory is forgotten: `afd_ref_max_reset("toggle")` (Auto-refocus toggle,
  `:14151`), `("rawrec")` (RAW-rec start/stop, `:14163`), `("foreign")` (user AF, `:14218`), plus the
  existing `("scene")` luma jump (`:13754`). The reset now also clears `afd_last_land_s`/`_zoom`
  (review N5, `crop_rec.c:8612-8622`), so both references restart together after a scene change.

**Acceptance:** after the bench, the first walk landing shows `# local: ref max zoom set ... mm=...` (or a
`ref=` equal to the walk landing); the same after a RAW-rec start/stop, an Auto-refocus toggle, a user AF,
or a > 10 % focal-length change. A bench/RT-prep landing shows `track=0` and does not change `now=`.

### 11.3 MF3 — true reference anchor (replaces the 95 % floor)

The `AREF_REF_FLOOR_PCT 95` floor was `0.95 × old_ref` **per settle** (0.95^k ratchet) and was rotated
out of the 12–24 s two-bucket peak, so the trigger reference still eroded. It has been removed and
replaced by a real anchor (new statics `crop_rec.c:13336-13360`, `AREF_REF_SOFT_N` at `:13402`):

- `aref_ref_anchor` (+ zoom, mm bucket, em, g), valid only for the current zoom/mm (`aref_anchor_ok()`).
- **Seed / raise:** at each settle, `MAX(old peak, afd_ref_max_s if valid for the same zoom+mm)` seeds a
  missing anchor (`# aref ref anchor: seed ...`) or raises an existing one (`... raise ...`). This extracts
  the anchor from the pre-defocus picture and from the engine high-water mark (which survives
  `aref_peak_clear`).
- **Floor = the anchor, not 95 % of it:** a settled picture below the anchor still adopts the anchor
  exactly (`adopt = anchor`), so five consecutive soft landings give a constant `new_ref` / `anchor=`, not
  0.95^k (`crop_rec.c:13678-13712`). A genuinely sharper settled picture raises the anchor.
- **Clamp:** `aref_peak_update` clamps the effective `aref_ref` up to the anchor (`crop_rec.c:13593-13600`)
  and restores the anchor's texture metrics, so the 12–24 s bucket rotation cannot erode the trigger
  reference (`# aref trig ... ref=`) either.
- **Gradual-change escape:** `AREF_REF_SOFT_N = 3` consecutive settles below `AFD_L_SOFT_PCT` (92 %) of
  the anchor reset the anchor to the best settle of the three and log
  `# aref ref anchor: confirmed soft scene -> reset ref=...` (`crop_rec.c:13700-13710`). Soft band
  landings cannot be a true peak, so this cannot re-create the erosion the patch fixes.
- **Resets:** scene change (`:13755`), Auto-refocus toggle (`:14152`), RAW-rec start/stop (`:14164`),
  user AF (`:14219`), and zoom/focal-length change in the eligibility check (`:14291`, with a new
  `aref_ref_mm` at `:13331` so the aref peak memory also restarts when the lens zoom ring moves).
- `# aref done ... anchor=` (`crop_rec.c:13620`) exposes the anchor next to `new_ref`, `ref_max`,
  `land_vs_ref`.

**Corrected acceptance (replaces AF-PATCH-FIELD-PLAN §7.4):** over a static soft scene of ≥60 s,
`# aref trig ... ref=` must stay at ~the anchor and `# aref done ... new_ref`/`anchor` must not
staircase. A downward move is only valid via `# aref ref anchor: confirmed soft scene -> reset` (after
3 confirmed-soft settles — this is the intended adaptation, not a failure) or a scene/zoom/mm/RAW/toggle
`reset`. Plain soft landings show `# aref ref anchor: soft ... n=1/2` and must NOT lower `anchor=`.

### 11.4 SF1 — the "drop path" is always true by design (documentation correction)

On the Auto-refocus / MPU path every relative trigger satisfies `drop` (trigger thresholds are 60/75/85 %,
`AFD_L_TRIG_DROP_PCT` is 90), and path 2 always does. This is intended: the in-focus exit is never taken
on an MPU trigger, and the `soft` selector is only used by the MF1 post-climb guard. Consequences for the
field tester that earlier text got wrong:

- `# local ... drop=1` on every aref-triggered local run is normal; `soft` is not expected to be the
  selector and the old `# local: soft -> wide probe` line will read `drop path -> wide probe`.
- A false trigger on an in-focus scene costs two wide moves and then a B fallback (≈5–8 s), bounded by
  the aref hold/penalty hysteresis; it never silently accepts the soft picture. This is a deliberate
  latency trade, not a bug.
- `AFD_L_TRIG_DROP_PCT` is unchanged.

### 11.5 SF3 — Refocus-test anchor diagnostics and luma guard

`# RT_REF` now logs both values (`crop_rec.c:14524`):

```
# RT_REF why=reland t=... land_s=... meas=95000 ref_s=115000 g=... luma=...
```

`meas=` is the fresh measurement, `ref_s=` the adopted (anchored) reference — which is what
`contrib/af_test/corpus.py` reads as `ref_s`. A re-reference below `AFRT_REF_FLOOR_PCT` is still kept,
but if its luma differs by more than `AFRT_REF_LUMA_PCT` (15 %) from the anchored reference's luma, the
fresh reference is accepted and `# RT_REF scene changed old=.. new=.. luma=.. anchor_luma=..` is logged
(`crop_rec.c:14510-14518`) instead of turning a light fade into five unexplained failures.

**Acceptance:** `# RT_REF anchor kept` only after a *failed* trial on a static scene; it must not appear
twice in a row with a large `luma=` delta; `ref_s=` in `# RT_REF` is the value the trials are scored
against.

### 11.6 SF4 / N1 / N3 / N5 (cheap review items)

- `# local wide plus=.. minus=.. s0=.. pos_plus=.. pos_minus=.. mm=..` now carries the sample positions
  (`crop_rec.c:9369`), so the nominal ±4×fine reach after 120/160-pulse reversals can be checked against
  the generic move lines; still treat one "wide still flat" as weak evidence (SF4).
- `afd_aref_trig_pct/_path` are snapshotted into locals in `afd_run_local` (`crop_rec.c:9301-9307`) and
  cleared in `afd_engine` whenever the run is not `AFD_F_AREF` (`crop_rec.c:10287-10291`).
- `# refmax` carries `mm=`/`track=` (`crop_rec.c:9502`); `# local: ref reset` carries
  `zoom=/mm=/last_clear=` (`crop_rec.c:8612`).

### 11.7 Field-plan corrections

- **Movie Servo OFF for every Auto-refocus run** (Refocus test and the walk). Patch 2 gates the
  `is_continuous_af()` abort to `AFD_F_AREF` engine runs; with Movie Servo ON those runs abort as a
  safety measure, so the aref/refocus tests would measure aborts, not AF.
- Expect `# local ... drop=1` and no `soft` selector on the aref path (§11.4).
- Expect `# aref ref anchor:` seed on the first landing after boot (and after each reset), `raise` when a
  landing genuinely improves, `soft n=1/2` on soft landings, and only after three in a row
  `confirmed soft scene -> reset`.
- A `# aref ref anchor: reset why=scene|toggle|rawrec|foreign|zoom` line is a correct restart, not a test
  failure; correlate it with the scene change.

### 11.8 Rollback

Everything is in `modules/crop_rec/crop_rec.c` (uncommitted). Full rollback:
`git checkout -- modules/crop_rec/crop_rec.c` (this returns to HEAD, i.e. drops patches 1–3). Patch-3-only
rollback has no single constant: the anchor replaced `AREF_REF_FLOOR_PCT` (the constant no longer exists);
`AFRT_REF_FLOOR_PCT 0` disables the Refocus-test anchor and `AFRT_REF_LUMA_PCT` can be raised to disable
the scene-changed escape. `afd_ref_max_track` is mode-driven; to keep the old "any run feeds it"
behaviour, call `afd_ref_max_update` unconditionally in `afd_run`. Rebuild and flash after any change.

### 11.9 Residual concerns (patch 3, SF2 updated by patch 4)

- **No field validation.** MF1's 92 % post-climb guard, the anchor escape count (3) and the luma guard
  (15 %) are reasoned from the review, not measured.
- **SF2 closed by patch 4/task-15 (§11.10).** The stale-high ref max in the 75–92 % band is now reset to
  the best of 3 consecutive soft landings (`AFD_SOFT_LAND_N`). Residual risk: a search that fails 3 times
  in a row lowers the reference to the soft level (bounded, logged by
  `# local: ref max reset confirmed soft landings`; see §11.10). MF2's mm key and resets still remove the
  cross-focal-length and cross-scene leaks.
- **Anchor/settle interaction with the hold logic.** Hold/penalty still uses the raw settled `med`, so a
  floored anchor plus repeated soft landings can cause a few refocuses before the hold engages; that is
  bounded (10 s hold, threshold −10 up to −25) and observable via `# aref hold scene in focus`.
- **The wide-probe geometry is still unverified** (SF4); positions are now logged so the field session can
  confirm or refute the ±4×fine reach and the R = 60 reversal model.
- **RT re-reference and the anchor are separate mechanisms:** the RT anchor (`AFRT_REF_FLOOR_PCT`) only
  scores trials; the aref anchor (`aref_ref_anchor`) drives triggers. A session where both lines disagree
  is expected (different picture states) but must be read together.

### 11.10 SF2 closed (patch 4, task-15): ref-max confirmed-soft-landing reset

**Status:** implemented, **compile-checked only**. The last open review finding
([AF-PATCH-REVIEW.md](AF-PATCH-REVIEW.md) SF2): a persistently 10–20 % softer scene (inside the 25 % luma
scene gate and above the 75 % deep-landing reset) kept `afd_ref_max_s` high forever, so every local refocus
wide-probed and fell back to B for the rest of the session.

**Change** (`afd_ref_max_update`, `crop_rec.c`): new counter `afd_soft_land_n` + `afd_soft_best`
(`AFD_SOFT_LAND_N = 3`).

- a verified landing below `AFD_L_SOFT_PCT` (92 %) of the current max (but above the 75 % deep reset)
  increments the counter and keeps the best (highest) landing of the run;
- a landing at/above 92 % of the max, a max rise, a zoom/mm bucket change or `afd_ref_max_reset()` resets
  the counter/best;
- at 3 consecutive soft landings the max is set to `afd_soft_best` (never below the best actual landing of
  the run), the counter restarts, and this line is logged:

```
# local: ref max reset confirmed soft landings n=3 best=81000 old=100000 zoom=5 mm=20
```

**Why it does not re-create the erosion patch 1 fixed:** one non-soft landing breaks the run; three
landings in a row in the 75–92 % band cannot be a true focus peak (and MF1 already prevents the local
search from landing there on a drop path), so only a genuinely softer scene can reach the reset. The reset
appears once, not on every run.

**Acceptance:** after 3 consecutive refocuses on a genuinely softer scene the local search stops falling
back to B forever and the next landing is compared against the new (soft) level; `# local: ref max reset
confirmed soft landings` appears once after those three runs, followed by ordinary `# local s0=... ref=`
values at the softer level. A `# local: ref max reset confirmed soft landings` appearing repeatedly on a
static, unchanged scene is a regression to report (it would mean the landings keep missing focus).

**Rollback:** set `AFD_SOFT_LAND_N` to a large value (e.g. 1000) to disable the reset without touching
anything else, or `git checkout -- modules/crop_rec/crop_rec.c` for a full rollback of patches 1–4.

**Residual risk (bounded, logged):** a search that fails 3 times in a row lowers the reference to the soft
level. That is exactly the intended recovery, but if the failures are AF failures rather than a scene
change, the local search then accepts pictures near the soft level (until a sharper landing or a
scene/zoom reset raises the max again). The reset line plus `# aref ref anchor: ...` make this visible; a
field session that shows it repeatedly on a static scene should be treated as an AF problem, not tuned by
raising `AFD_SOFT_LAND_N`.

### 11.11 Patch 5 (task-16): second-review fixes

**Status:** implemented, **compile-checked only** (no camera). Closes the four must-fixes and the cheap
should-fixes of [AF-PATCH-REVIEW-2.md](AF-PATCH-REVIEW-2.md). Patches 1–4, the task-12 feed logging, all
algorithms/trigger thresholds and the engine epilogue are unchanged.

**MF1 — the stale ref max no longer undoes the confirmed-soft reset.** The anchor may be seeded from
`old_ref`/`afd_ref_max_s` only when it does not exist yet; once valid it is raised only by a genuinely
sharper settled `med`. In the `confirmed soft scene -> reset` block the local-search ref max is lowered to
the new anchor as well (same zoom/mm key, `# local: ref max reset confirmed soft old=.. new=.. t=..`), and
the confirmed-soft counter is reset on every anchor raise. Result: `confirmed soft scene -> reset` is not
immediately followed by `anchor: raise ref=<higher>`, the local `ref=` stays at the soft level, and the
`# local: climb still soft on the drop path -> B` fallback stops happening on every trigger.

**MF2 — anchor texture metrics can no longer be zero.** The seed takes `MAX(old_em, settle emax)` and
`MAX(old_g, settle g)`, and the peak clamp restores em/g only when `aref_ref_anchor_em > 0`. So after the
~24 s two-bucket rotation the anchor can no longer restore `em=0`, and `AREF_PEAK_EMAX_MIN` cannot lock the
relative trigger path into gate 4.

**MF3 — the Refocus test no longer inherits or leaves a scene reference.** `aref_test_task` now calls
`afd_ref_max_reset("rtest")` + `aref_anchor_clear("rtest")` before `aref_test_run()` and again after it
(alongside the existing `afrt_state_reset`/`aref_forget`). HOLD1 therefore starts from the RT subject's
own picture (no pre-test `false_trig`) and normal Auto refocus does not inherit the test scene's anchor.
Log: `# local: ref reset why=rtest ...` and `# aref ref anchor: reset why=rtest ...` around the test.

**MF4 — the local-search fallback landing is focal-length keyed and mode-gated.** `afd_last_land_mm`
is part of the `have_last` key in `afd_run_local`, and `afd_last_land_s/_zoom/_mm` are written only by
engine modes 0 and 8 (`afd_last_land_track`; mode 8's prep seeds the RT first trial). Bench (6), speed (3)
and reversal-cal (7) landings no longer leak into the walk; `afd_ref_max_reset` clears the mm too.
`# refmax ... track=.. last_track=..` shows the gating.

**Cheap should-fixes.**
- (a) two consecutive no-focus runs (nopeak/lowtexture) clear the anchor + ref max:
  `# aref ref anchor: reset why=failed runs ...` (patch 5 / SF2); a real settle resets the streak.
  The same clear was added to the 5 s no-settle timeout (`why=timeout`) and the "two OK AFs landed at the
  same soft level = new scene" post-check (`why=newscene`), so those peak-clearing escapes can no longer be
  defeated by the clamp.
- (b) the anchor clamp is MPU-Auto-refocus-only (`Auto_refocus_method == 1`); switching Refocus method
  clears the anchor (`# aref ref anchor: reset why=method ...`), so a Canon-method run keeps its own
  peak-clearing behaviour and cannot inherit a stale MPU anchor.
- (c) the confirmed-soft counter is reset on every anchor raise (MF1).
- (d) the inline `# stop_check` now mirrors `contrib/af_test/stop_analysis.py`: baseline = last sample at
  or before `t_stop`, `exc_pct` = largest signed post-stop excursion in the move direction (%), and
  `halted=1` iff `|slope_pct_s| <= 2` **and** `|exc_pct| <= 1`. Log line keeps all old fields plus
  `exc_pct=`:
  `# stop_check k=.. t_stop=.. s_stop=.. s_end=.. delta_pct=.. exc_pct=.. slope_pct_s=.. halted=.. npost=.. insufficient=..`
- (e) the last unguarded menu SELECT (`afdiag_afsearch_select`) uses `afd_menu_task_create`, so it creates
  no task while recording and logs `# menu task refused name=afdiag_afsearch reason=recording ...`.
- (f) `afd_mm_bucket()` keeps the last valid bucket for an invalid focal length and debounces a changed
  bucket over two observations (`# focal mm change pending .. -> ..` / `confirmed .. -> ..`), so a
  flickering `lens_info.focal_len` cannot keep resetting the references. Note the two-sample (~400 ms)
  confirmation latency on a real zoom ring change.

**Corrected acceptance (supersedes the patch-3/4 items):**
- After `# aref ref anchor: confirmed soft scene -> reset` there must be **no** immediately following
  `# aref ref anchor: raise ref=<higher>` from the stale max; `# local: ref max reset confirmed soft
  old=.. new=..` must appear, and the local `ref=` on later runs must be at the soft level.
- On a textured static scene, gate 4 (`# RT_S ... gate=4`, `# aref` decision records) must not persist
  after ~24 s; `# aref done ... anchor=` must be non-zero once a landing has settled.
- RT start/end: `# local: ref reset why=rtest` and `# aref ref anchor: reset why=rtest` brackets around
  `# RTEST_BEGIN`/`# RTEST`; HOLD1 `ref=` is the RT subject's own level; `false_trig=0` even when the test
  is run right after a walk on a different subject.
- After an optical zoom with the same digital zoom, the first `# local s0=... ref=` is 0 or the new-focal
  landing, never the old focal length's landing.
- `# stop_check ... exc_pct=... halted=...` must agree with `contrib/af_test/stop_analysis.py` on the same
  `AFSTOP.LOG` (run both on the same session; a rising-then-returning trace is `halted=0` inline now).
- `# aref done ... anchor=` / `ref_max=` non-zero in the walk after the first settle; `# refmax ...
  last_track=` is 1 for mode-0 and RT-prep landings, 0 for bench/speed/revcal.

**Rollback:** all patch-5 behaviour is in `modules/crop_rec/crop_rec.c`. `AFD_SOFT_LAND_N`/`AREF_REF_SOFT_N`
and `AREF_ANCHOR_FAIL_N` are isolated constants; the last-landing gating is `afd_last_land_track`; the
method scoping is one condition in `aref_peak_update`; the mm debounce is inside `afd_mm_bucket`; the stop
verdict is the two-line rule in `afw_stop_check`. Full rollback of patches 1–5:
`git checkout -- modules/crop_rec/crop_rec.c`.

**Residual concerns (patch 5):**
- A confirmed-soft reset (after 3 soft settles) or a 2-failure anchor clear intentionally lowers the
  reference to what the lens actually achieves; if the failures are AF failures, the trigger then accepts
  that soft level until a sharper landing/scene reset raises it. Both are logged (`confirmed soft`,
  `reset why=failed runs`) — do not tune them away if the field log shows them on a static scene.
- The anchor clamp is MPU-only; the Canon method relies on its own 12–24 s peak memory (unchanged).
- `afd_last_land_s` stays written by mode-0 menu AF runs, and A/B/E still read it with a zoom-only key
  (the mm key is applied only in the Auto-refocus local search), so a bench run followed by an A/B/E menu
  run can still use the older landing in those algorithms — the patch-5 gate only protects the walk.
- The stop-check baseline changed from "first sample at/after t_stop" to "last sample at/before t_stop",
  so inline verdicts on old traces may differ slightly from earlier logs; the offline script is still
  authoritative for analysis.
- No camera validation: the interaction fixes are reasoned from the review's models, not measured.

### 11.12 Patch 6 (task-19): absolute-fallback retry + Refocus-test reference validation

**Status:** implemented, **compile-checked only** (no camera). Based on the 2026-10-19 evening field
session (`~/af_logs/af-20261019-193009/ML/LOGS/`); patches 1–5 and the feed logging are preserved.

**A — Auto-refocus absolute fallback is retried after a failed run.** Field evidence: 120 of 355 walk
decisions were gate 4 "no usable peak", and 44 of those satisfied the absolute-path criteria
(`q >= 95`, `emax <= 55`, `gn >= 2`) yet `path` stayed 0. Cause: `aref_abs_used` is one-shot per blur
episode; after a failed path-2 run the scene is still blurry (the peak memory is rebuilt from the blurred
picture with `em < 70`), so the relative path is unusable and the absolute path never fires again.

Change: new `AREF_ABS_MAX 3` and `aref_abs_n` (attempts in the current blur episode; incremented when
path 2 arms). On a failed MPU run after a real search (`aref_mpu_pct < 0 && afd_searched_last`, while
`0 < aref_abs_n < AREF_ABS_MAX`) the absolute path is re-armed and this line is logged:

```
# aref abs re-arm after failed run n=1 max=3 t=...
```

The counter resets on a sharp recovery (`q < 85 && emax >= 100`), on pause resume/expiry, on the
Auto-refocus toggle, on user/foreign AF, on a cleared failed-scene memory (new subject/scene), and in the
Refocus test's state reset. The existing 4 s cooldown, 20 s failed-scene memory, `AREF_MAX_FAILS` pause and
the texture gates are unchanged: at most 3 absolute attempts per blur episode, then the pause/hold applies.

**B — Refocus-test reference validation (the 55 mm false pass).** Field evidence: `# RT_REF
land_s=34269 meas=33991 ref_s=33991` but all five trials landed 30–48k (`final_pct` 90–141 %) with
`false_trig=5` in HOLD1 — the reference landing itself was soft, so the test scored against a bad
baseline. Change in the reference path only (`afrt_prep`, engine mode 8): after `afb_land_a()` the code now
runs one local-search verification pass (trigger-hint globals cleared, `afd_algo_cur = AFD_ALGO_LOCAL`,
`afd_run`). If the local result is more than 5 % better it becomes the reference and is logged; a failed
local pass cannot fail the test (its aborted/nopeak flags are cleared). `afrt_reference` takes the verified
reference landing as a floor for its measured `ref_s`. The five trials, defocus values and `AFRT_OK_PCT`
are unchanged. Logs:

```
# RT_REF verify local better ref=34269 local=45000
# RT_REF verify kept ref=34269 local=36000 aborted=0 nopeak=0 lowcontrast=0 timeout=0
# RT_REF why=start t=... land_s=45000 meas=44900 ref_s=45000 g=... luma=... emax=...
```

(`emax=` added to `# RT_REF` for diagnosis.) A soft reference now either shows `verify local better` and a
lower/honest `final_pct`, or the test still reports the real (soft) level instead of a false 5/5.

**D — focal length is not available on this camera.** The same session shows `lens_info.focal_len` stuck at
18–21 while the lens was at 35/55 mm (`fine=20` always; the camera UI does not show focal length either).
Patch 6 adds no focal-length-dependent logic. A once-per-session warning is logged if the value is outside
8..300 mm (`# focal length unavailable? mm=..`); the Fine step must be set manually for the next session.

**Acceptance:**
- After a failed absolute run the next decision shows `# aref abs re-arm after failed run n=.. max=3` and
  up to 3 absolute attempts are made before the failed-scene/pause logic blocks further ones (no endless
  4 s re-tries, no trigger storm).
- On a deliberately soft reference scenario the `# RT_REF` block shows either `verify local better` (and
  then the trials' `final_pct` is honest against the improved reference) or, if the scene truly has no
  better focus, the test no longer reports a clean 5/5 false pass; `false_trig` in HOLD1 is at the test
  subject's own level (patch 6 keeps the pre-test anchor/ref-max clears).
- `# RT_REF ... emax=` present; focal-length warning appears at most once per session.

**Rollback:** `AREF_ABS_MAX` (set to 1 to make the absolute path one-shot again), remove the
`afrt_prep` verification block, or `git checkout -- modules/crop_rec/crop_rec.c` for patches 1–6.

**Residual risks (patch 6):**
- The retry can add up to 2 extra full searches on a texture-less scene before the pause (each bounded by
  the 18 s run cap and the 4 s cooldown; the pause then holds the trigger).
- The reference verification adds ~2–5 s per Refocus-test reference (a handful of references per test).
- The local verification cannot create a better focus if the scene genuinely has none: in that case it
  keeps the B07 reference and the test can still pass at the soft level — the difference is now visible in
  `verify kept` plus the `# RT_REF` numbers, but it is not an automatic abort.
- `lens_info.focal_len` remaining invalid means the `mm=` reference bucket is effectively constant in the
  field: cross-focal-length contamination is not separable until Canon reports the focal length; the
  manual Fine step is the mitigation for the next session.

### 11.13 Patch 7 (task-20): adaptive Refocus-test trial defocus + focal-length freeze docs

**Status:** implemented, **compile-checked only** (no camera). Test-only patch from the 2026-10-19 50 mm +
0.71x speedbooster session (`~/af_logs/af-20261019-200208/ML/LOGS/`); patches 1–6 and the feed logging
are preserved, no production AF algorithm changes.

**A — adaptive trial defocus (the 50 mm test's real problem).** With the EF50/1.8 STM + speedbooster the
smallest Refocus-test defocus (`+-60` pulses) produced only a 4–6 % sharpness drop (`s_def_pct` 94–96 %),
below the 85 % MPU trigger threshold, so those trials never triggered and were marked invalid/false-pass;
`+-150` and `+-400` worked and landed 95–97 %. The AF itself was fine — the fixed test stimulus was too
small for that lens's gearing.

Change (`aref_test_run` trial loop): when a trial ends with **no trigger** and the defocus is still below
`AFRT_DEF_CAP` (2000 pulses), the trial is retried with the defocus **doubled** (60→120→240, 150→300→600,
400→800), up to `AFRT_RETRY_MAX` (2) retries. Logged before each retried prep:

```
# RT_RETRY def0=60 def=120 attempt=2
```

The `# RT_TRIAL` line now carries the defocus actually used plus the original and the attempt number
(`def=.. def0=.. attempt=..`); the `# RTEST` summary gains `retries=N`. The first-attempt defocus values,
the 90 % ok threshold, the HOLD phases and the reference validation are unchanged; only if every attempt
fails is the trial `no_trigger`/invalid.

**B — focal-length freeze (correct diagnosis).** EXIF shows the lens reports focal length correctly
(35 mm for the 50 mm + speedbooster, 18/35/55 mm for the 17–55); the user found that **once the MPU monitor
opens (`MpuMonOpen`), the LV_LENS focal-length property (and the camera UI) freezes at its pre-open value**.
A zoom changed afterwards is invisible to ML, so `focal_mm=`, the Auto fine step and the `mm` reference
bucket stay stale. ML cannot defeat this from code. Workarounds for the field:
- set the zoom **before** the first AF drive / monitor open (with Keep open this is one setting for the
  whole session);
- to change the zoom later: close the monitor (AF diag → "AF drive monitor" away from Keep open / the
  monitor auto-close), change the zoom, then run AF drive again to re-open and re-read;
- EXIF is the ground truth, not `focal_mm=`; set **Fine step manually** (18 mm → 20, 35 mm → 12, 55 mm → 8).
New logging (cheap, no behaviour): the run header gains `mm_pre=`, the `# epilogue` line gains `mm_post=`,
so a frozen value (`mm_pre == mm_post`) and a re-read after a monitor re-open are visible in AFDRIVE.LOG.
No new focal-length-dependent logic was added.

**Acceptance:**
- A 50 mm-style Refocus test with no trigger at `+-60` shows `# RT_RETRY def0=60 def=120 attempt=2` and
  the retried trial then triggers (the `# RT_TRIAL def=` is the doubled value, `retries>=1` in `# RTEST`);
  18 mm runs are unchanged (`retries=0`).
- A deliberately small first defocus still ends as a normal valid trial after the retry, or as
  `no_trigger` only when every attempt (including the doubled ones) fails.
- After a zoom changed while the monitor was open, `mm_pre`/`mm_post` show the stale value and EXIF differs;
  after the close-monitor → zoom → run-AF-drive workaround the run header shows the new mm.
- `# RTEST retries=` is visible; `notrig/valid/false_pass` semantics are unchanged.

**Rollback:** set `AFRT_RETRY_MAX` to 0 to disable the retries without touching anything else, or
`git checkout -- modules/crop_rec/crop_rec.c` for patches 1–7; the mm/new logging is display-only.

**Residual risks:** a no-trigger trial plus 2 retries can add up to ~45 s to the test (15 s trigger window
per attempt); the retried defocus is applied from the current (already defocused) lens position, so the
actual offset can exceed the nominal doubled value (only ever more defocus, which is the safe direction);
a scene with no focus contrast still ends `no_trigger` after the retries — the stimulus helps gearing, not
a texture-less subject; the focal-length freeze remains unfixable from ML and must be worked around in the
field.

### 11.14 Patch 8 (task-21): zoom refresh action + automatic zoom-change detection

**Status:** implemented, **compile-checked only** (no camera). Uses the existing (previously unused) lens-status
refresh path: `afdiag_request_lens_update()` resends the saved `PROP_LV_LENS` payload so the MPU can answer
with a fresh lens status; `PROP_HANDLER(PROP_LV_LENS)` already stores the raw payload/length and now also
counts every report (`afdiag_lvlens_count`). Patches 1–7 are preserved; no production AF algorithm change
beyond the zoom reset in the automatic path.

**A — refresh core** (`afd_lens_refresh(wait_ms, *mm_out)`, `crop_rec.c`): records the current
`lens_info.focal_len` and the report counter, calls `afdiag_request_lens_update()`, polls (20 ms) up to
`wait_ms` for a new `PROP_LV_LENS` event, updates `afd_mm_cache` when the value is plausible (8..300), logs

```
# zoom probe old_mm=18 new_mm=18 reply=1 t=...
```

and returns `reply` (1 = a fresh report arrived) and the observed focal length. The probe line is only
written when a report arrives or the value changed, so a 1 s poll cannot flood the note buffer.

**B — manual action: "Zoom refresh (read lens mm)"** (AF diag menu, next to "AF drive: close monitor";
runs in a task so the ~2 s reopen blackout is safe). Refuses while recording or AF busy. It tries the
request path first while the monitor is open; if the MPU returns no fresh report, or returns the same value
for a lens whose name looks like a zoom (`-` in `lens_info.name`), it falls back to the proven workaround:
close monitor → `msleep(300)` → request/read → update the cached mm/fine → `MpuMonOpen` → wait for LV
recovery. Every stage is logged with `afd_note` and the result is shown in a NotifyBox:

```
# zoom refresh begin old_mm=18 monitor=1 lens="EF-M 18-55mm" t=...
# zoom refresh: no usable fresh report reply=1 new_mm=18 zoom=1 -> close/reopen fallback
# zoom refresh: monitor closed read mm=35 t=...
# zoom refresh: monitor reopened e=0 lv_ok=1 t=...
# zoom refresh done old_mm=18 new_mm=35 reply=1 fine=12 monitor=1 t=...
```

It never closes/opens the monitor while recording (checked before and again before the fallback), leaves the
monitor closed rather than in a broken state if reopen fails (the next AF run reopens it), and restores
`afd_mon_is_open` on success. Prime lenses with a valid fresh value take the cheap path (no blackout).

**C — automatic detection.** While Auto refocus is ON with the MPU method and the monitor is "Keep open",
`aref_step` polls `afd_lens_refresh(250)` about once a second (only when no AF run is in flight). If the
reported focal length changed by >10 % (both values 8..300) it logs

```
# zoom auto old_mm=18 new_mm=35 fine=12
```

updates the cached mm, and resets the zoom-dependent state: `afd_ref_max_reset("zoom")`,
`aref_anchor_clear("zoom")`, `aref_drop_ref(..., forget=1)` and the absolute-attempt counters, so the next
decision runs on the new framing. Before every Auto-refocus engine run `afd_lens_refresh(150)` is also
called so `afd_fine_eff()` inside the engine sees the current value. If the property never produces a fresh
report while the monitor is open, it logs once

```
# zoom auto: no fresh report (monitor open)
```

stops polling and relies on the manual action — it never closes/reopens the monitor automatically (2 s LV
blackout). Consequence: **whether the MPU answers the refresh while the monitor is open is exactly what the
next field session measures**; if it does, automatic detection works; if it does not, the manual action is
the answer and the `no fresh report` line documents it.

**D — observability.** The AFDRIVE run header now carries `mm_cache=` (last plausible cached mm, in
addition to the existing `mm_pre`/`mm_post` from patch 7):

```
00001234 AFDRIVE t=... lens="EF-M 18-55mm" zoom=x5 ... algo=L mm_pre=18 mm_cache=35
```

**Acceptance:**
- Manual action after an optical zoom while the monitor is open: `# zoom probe`/`# zoom refresh` lines and a
  NotifyBox with the new mm and fine step; if the MPU did not answer while open, the close/reopen fallback
  runs once and the header/NotifyBox show the new value; monitor is left open/working as before.
- Automatic detection (if the MPU answers): `# zoom auto old_mm=.. new_mm=.. fine=..` within ~1–2 s of the
  zoom, followed by reference re-learning; a prime lens reporting a stable mm never triggers a spurious
  reset; with the monitor closed nothing changes (no poll, no reset).
- If the MPU does not answer: exactly one `# zoom auto: no fresh report (monitor open)` and no
  close/open blink; the manual action remains the documented workaround.

**Rollback:** the whole feature is additive. The menu action and the auto poll can be disabled by
commenting the `aref_step` poll block / removing the menu entry; `afd_mm_cache`/`mm_cache=` are display-only;
full rollback of patches 1–8: `git checkout -- modules/crop_rec/crop_rec.c`.

**Residual risks:** the auto poll blocks `aref_step` for up to 250 ms once a second when the MPU does not
answer (sampling cadence dips; no logical effect); the zoom heuristic for the manual fallback (`-` in the
lens name) is crude (a prime name with a dash would take the blackout path once; a zoom without a dash
would accept a stale value and still be fixed by the automatic path or a second press); the close/reopen
fallback costs the ~2 s LV blackout and can move the lens/landing state is untouched but focus is not
re-verified; the automatic path cannot recover the mm if the MPU never answers while the monitor is open —
that must be handled manually. No camera validation yet.

---

## 11.15 Patch 9 (task-24): persistent never-give-up Auto-refocus hunt

**Status:** implemented in `modules/crop_rec/crop_rec.c`, **compile-checked only (no camera), not field-validated.**
Design: [AF-HUNT-FOREVER.md](AF-HUNT-FOREVER.md) (task-22). Behaviour change, so the Normal contract is explicit below.
Line refs are from the working tree at patch 9; the tree is still uncommitted together with patches 1–8.

**Diff/build:** `modules/crop_rec/crop_rec.c` +1725/−139 vs `HEAD` (this includes patches 1–8; patch 9 alone adds
+693/−1 = ~692 net lines). Build exit 0 twice (forced `touch` between), `crop_rec.mo` md5
`3d5f18e345b14f3daaf6aacad31dd556` both times, `crop_rec.c` compiles clean (only the pre-existing `xor_chk.c`
host-tool warning and the ignored PDF/logo copy warnings). Host suite green: `python3 contrib/af_test/run_all.py`
→ 84/84 unit tests, 4/4 tiers, 6/6 corpus cases, `RESULT: PASS`. No commits.

### A — New setting (default Persistent)

```c
CONFIG_INT("crop.auto_refocus_persist", Auto_refocus_persist, 1);   /* crop_rec.c:132 */
```
Menu: Settings → INFO/Up-Down/Shutter zoom → **Refocus persistence** (`Normal` / `Persistent`, default
Persistent), entry after "MPU drive while recording" (`crop_rec.c:6003`); registered with
`MODULE_CONFIG(Auto_refocus_persist)` (`crop_rec.c:17285`). Only meaningful while Auto refocus is ON/ON+REC.

- **Normal** = patches 1–8 exactly: the hunt state machine is never entered, and all existing pause /
  failed-scene-memory / absolute-cap / hold / cooldown logic is untouched. The only additions on that path are
  no-op calls (`aref_hunt_*` early-return on the idle state), one extra `aref_dec.emax` assignment and the engine's
  defensive override clear, none of which change a decision.
- **Persistent** = the scheduler below owns all Auto-refocus attempt launches; switching to Normal mid-episode
  ends the episode (`aref_hunt_reset("off")`).

### B — State machine (implemented)

`enum { HUNT_IDLE, HUNT_ARM, HUNT_ATTEMPT, HUNT_JUDGE }`, state globals and constants near `crop_rec.c:13796–13875`;
helpers at `crop_rec.c:14648–15107`; the one call site is the persistent branch after `aref_eval`
(`crop_rec.c:15408`), i.e. `aref_hunt_step(now, path, mpu_m)` runs on every aref poll in Persistent mode.
`HUNT_ATTEMPT` is driven by the existing `aref_phase` / `slim_af_state` machinery; the hunt never launches a
second attempt while one is in flight.  `HUNT_ARM` also refuses to launch an MPU attempt while
`aref_dec.settled < AREF_SETTLED_MS` (a pan / fade / exposure step): it waits 500 ms and retries — waiting is
not a failure and does not consume the backoff (moves/samples during motion are meaningless; Canon method has no
settled value).

**Entry conditions** (`aref_hunt_step`, `crop_rec.c:15070–15081`):
1. `path != 0` — the normal aref trigger fired: begin the episode and use the configured strategy at once
   (`why=blur`).
2. `gate` is 6/7/11 (failure pause / MPU hold / same failed scene) — an existing give-up throttle is what
   suppressed the trigger; begin and retry on the hunt backoff (`why=gate`).
3. `gate` is 4 with a peak that passes the sensitivity peak minimum (`why=gate4`).
4. **Cold start:** eligible for `HUNT_COLD_START_MS` (8 s) with **no currently trusted reference**
   (`aref_hunt_ref()==0`) and blur evidence (`peak < aref_peak_min[sens]` or `q >= AREF_ABS_Q_GOOD` and
   `emax < AREF_ABS_EM_GOOD`) — the "camera booted on a blurry/low-texture scene, no AF run has produced a
   trusted landing yet" case (`why=cold`).

**Backoff** (`aref_hunt_delay`, `crop_rec.c:14728`): table `{4000,6000,10000,15000,20000,30000}` ms, indexed by the
failure count (after the 6th failure it stays at 30 s), multiplied by a private-LCG jitter of ±20 %
(`HUNT_JITTER_PCT`), clamped to 3–36 s; never shorter than 10 s while recording. There is **no attempt-count cap**:
`aref_hunt_n`/`aref_hunt_k` only grow the backoff/rotation.

**Never-stop invariant:** every failed attempt calls `aref_hunt_schedule()` (`crop_rec.c:14967`) which always sets
`aref_hunt_next_at = now + delay <= now + 36 s` and returns the state to `HUNT_ARM`. `HUNT_ARM` has no other exit
except launching an attempt or `aref_hunt_close()` (sharp / explicit reset). Therefore, while the camera is
eligible and the picture is not verifiably sharp, an attempt starts at least every ~70 s **worst case**
(36 s backoff cap + up to ~30 s attempt/settle; review-4 N8).  The invariant to check in the field is
`max_backoff_ms <= 36000`, not a 45 s gap: with a 36 s backoff plus a slow attempt, ~68 s start-to-start is
legitimate. The only stops are verifiable sharpness and the explicit resets/pauses in E.

**Judge** (`aref_hunt_is_sharp`, `crop_rec.c:14704`): 92 % of the reference that was trusted **at episode start**
(`aref_hunt_base_ref`, frozen in `aref_hunt_begin`), or absolute sharp (`q < AREF_ABS_Q_GOOD && emax >=
AREF_ABS_EM_GOOD`), or a clear +10 % improvement over the pre-attempt sharpness with real texture
(`emax >= AREF_PEAK_EMAX_MIN`). The frozen base is essential: a soft landing that seeds/raises the anchor or ref max
during the episode must not become its own success bar (the original erosion failure mode). A run that reports
`pct >= 0` but settles below the sharp criterion is a **failure**, not an end. Trusted references are the
patch-3 anchor, the per-zoom/mm engine ref max and the last verified landing; the decaying `aref_ref` peak memory is
deliberately not trusted.

MPU attempts are judged in `aref_mpu_settle` (`crop_rec.c:14207`; the settle hook calls `aref_hunt_settle`,
`crop_rec.c:15022`); Canon attempts and a missed settle are judged by the fallback timer
(`HUNT_ATTEMPT_TIMEOUT_MS`/`+AREF_SETTLE_MS`). A completion watchdog (`crop_rec.c:15051–15068`) stops waiting after
30 s and schedules the next attempt without touching engine internals.

**Strategy portfolio** (`aref_hunt_order`/`aref_hunt_pick`, `crop_rec.c:14768–14806`) rotates on every failure:
1. `local` — configured Refocus AF (`Auto_refocus_af ? B : AFD_ALGO_LOCAL`), engine `AFD_F_HUNT`;
2. `full_b` — B at 07 (`afd_hunt_algo=1`, `afd_hunt_speed=AFD_SPEED`);
3. `canon_x10` — existing `slim_af_request = 1` (x10 detour, non-recording only);
4. `canon_x5` — existing in-place request `slim_af_request = 2` (recording-safe);
5. `full_b_alt` — B at 0x14/0x1F alternating and the **opposite** sweep direction to the previous B attempt
   (`afd_hunt_dir`; forced direction refused if the probe already saw an end stop — `afd_run_sweep`,
   `crop_rec.c:9232–9240`).

Unavailable strategies are skipped (monitor OFF / Canon method → only Canon legs; recording → no x10, MPU only with
ON+REC + open monitor + worker; menu/MF/servo → the eligibility gate pauses first). The last taxonomy
(`nopeak`/`lowtexture`/`soft`/`deep`/`endstop`/`dark`/`canon_fail`/`abort`/`moving`) reorders the portfolio once;
an end stop forces the reversed B leg; **after 6 consecutive failures without a >=10 % improvement the best-effort
mode** (log `# hunt besteffort`) restricts the rotation to the cheap Canon legs at the 30 s cap —
attempts still never stop. Per-attempt travel/time caps are unchanged (`AFD_MAX_TOTAL 9000`, `AFD_RUN_CAP_MS/B_MS`
15/18 s, Canon `SLIM_AF_MAX_MS 4000`); the hunt accumulates `travel` for the summary only.

Engine override: `AFD_F_HUNT` flag (`crop_rec.c:7543`) plus `afd_hunt_algo/speed/dir` globals
(`crop_rec.c:7620–7622`), applied after the normal menu/config algorithm choice only for `mode == 0` hunt runs
(`crop_rec.c:10502–10507`) and cleared at engine end. Normal menu AF never sets or inherits them.

**Recording / diagnostics safety:** no new tasks; never opens/closes the monitor (MPU only with an already-open
Keep-open monitor); never writes the card while recording (`# hunt` goes to the RAM `afd_note` buffer, flushed after
recording; AFDRIVE logs stay held); x10 is skipped while recording; attempt spacing >=10 s while recording; menu
AF diagnostics (bench/speed/sweep/stop/reversal) and the Refocus test reset the hunt
(`aref_hunt_reset("diag")`, `crop_rec.c:15130`; `aref_hunt_reset("rtest")` in `aref_test_task`,
`crop_rec.c:15840/15847`).

### C — Resets, pauses, explicit ends

- **Explicit resets** (episode ends, summary logged): a verifiably sharp frame (`why=sharp`), scene change
  (`aref_sw_push`, `why=scene`), user AF / Shutter zoom AF (`why=user_af`), zoom/focal-length change
  (automatic patch-8 detection and the eligibility branch, `why=zoom`), Auto-refocus toggle / method change /
  RAW recording start-stop (`why=toggle|method|rawrec`), Refocus test (`why=rtest`), menu AF diagnostics
  (`why=diag`), Auto refocus OFF / persistence Normal (`why=off|toggle`).
- **Legitimate pauses** (episode state preserved, timer untouched, **no failure counted**): menu open, MF,
  Movie Servo, half-press, LV transition, AF busy (`aref_hunt_pause`, `crop_rec.c:15378`); an attempt aborted by
  such a state is `# hunt pause ... reason=abort` and retries after 2 s (not a backoff step).
- Half-press release does not reset the backoff; the episode resumes from its current timer (the next attempt
  may be immediate if the timer already expired).

### D — Log lines (`AFDRIVE.LOG` via `afd_note`, compact `@REFOCUS` kind 17)

```
# hunt begin why=blur|gate|gate4|cold gate=%d path=%d ref=%d base=%d anchor=%d rec=%d t=%d
# hunt attempt n=%d k=%d strategy=%s pre=%d ref=%d anchor=%d backoff_ms=%d t=%d
# hunt outcome n=%d k=%d strategy=%s pct=%d land=%d med=%d sharp=0|1 why=%s t=%d
# hunt pause gate=%x t=%d
# hunt pause n=%d strategy=%s reason=abort|request_dropped t=%d
# hunt pause n=%d reason=worker t=%d
# hunt skip n=%d reason=no_strategy t=%d
# hunt besteffort attempts=%d t=%d
# hunt reset why=%s attempts=%d ok=%d strategies=%x max_backoff_ms=%d travel=%d duration_ms=%d sharp=%d t=%d
# hunt summary attempts=%d ok=%d fails=%d strategies=%x max_backoff_ms=%d duration_ms=%d travel=%d end=%s rec=%d
```

`@REFOCUS` kind 17 (one per attempt, `crop_rec.c:14913–14916`): `w0 = 17 | (strategy << 8) | ((n & 0xFF) << 16) |
(anchor_present << 24)`, `w1 = cur`, `w2 = med`, `w3 = base ref` (sharpness needs the full 32 bits, so no 16-bit
packing). Text lines are rate-limited while recording (attempt/outcome only when the strategy changes) to protect
the 2 KB `afd_note` buffer; begin/reset/summary are always logged.

**Reading the verdict:** on an unfocusable scene expect `begin`, attempts at ~4/6/10/15/20/30/30 s
(`why=nopeak|lowtexture|deep`, then `# hunt besteffort`) and **no** `# hunt reset` until the scene changes — that is
the requirement being met. `max_backoff_ms <= 36000` proves the cap; `attempts` continuing past 6/10 proves
"never gave up". `end=sharp` is success; `end=scene|user_af|zoom|toggle|rawrec|rtest|off` are legitimate ends;
`# hunt pause gate=...` is legitimate ineligibility, not a stop.

### E — Deliberately deferred / implemented differently

- **Implemented:** all of the never-stop invariant, setting/menu, backoff+jitter, portfolio (including the reversed
  B leg), soft-landing-is-failure, frozen-base sharp judge, resets/pauses, best-effort, recording throttle,
  `# hunt` logging + kind 17, cold-start entry.
- **Deferred: the explicit provisional-reference trust flag** from the design (§3.2(4), `aref_ref_prov`). The
  implemented `aref_hunt_ref()` + frozen `base_ref` already prevents the worst case ("a reference learned from the
  blurry picture becomes the success bar *inside the same episode*") and the cold-start entry covers a fresh
  scene, but there is **no explicit flag that marks an anchor/ref-max seeded from a picture that fails the absolute
  sharp test**. Residual: if the camera boots on a blurry scene and one attempt records a *successful but soft*
  landing (pct >= 0), that soft value can become the anchor/ref max; a *later* episode (after a reset) could then
  accept 92 % of it as sharp. Documented as the top residual risk below; patch 10/field data should decide whether
  to add the trust flag.
- **Also deferred:** the x1 half-press detour (N10a, separate idea) and any tuning of `AREF_*` thresholds — patch 9
  adds no threshold changes.

### F — Interaction with patches 1–8

- Patches 1/3/5 anchor/ref-max/ref-integrity logic is untouched; the hunt uses those values as trusted references
  and never lowers them.
- Patch 2 diagnostics/safety (`search_ms`/`epilogue_ms`, servo abort, menu `task_create` guard, NODONE stop) apply
  to hunt attempts unchanged; the servo abort now also covers an Auto-refocus run whose servo was enabled between
  trigger and engine call.
- Patch 4/5/6 failure/anchor recovery still runs in Persistent mode; the hunt only adds retries after it.
- Patch 7 Refocus-test changes are independent (hunt reset before/after the test; test numbers unchanged).
- Patch 8 zoom refresh/detection resets the hunt on a real zoom; the hunt never reads `lens_info.focal_len` for
  logic.
- **Normal mode is the regression contract.** With `Refocus persistence = Normal` the state machine is never
  entered; `aref_eval`, the legacy launch path, the pause/failed-scene/absolute-cap/hold/cooldown logic and the
  engine are the patches 1–8 code. This is verified by code review and by the host suite (84/84, 6/6 corpus);
  **camera A/B is still required** because the tree pairs the change with patches 1–8 and no camera is attached.

### G — Acceptance for the next field session

1. **2-minute deliberately unfocusable scene** (movie x5, AF lens, Movie Servo OFF, monitor Keep open,
   MPU drive, Refocus AF Local, sensitivity High, Auto refocus ON, **Persistent**, not recording, on a tripod;
   point at a blank/low-detail wall or a fabric closer than the minimum focus distance):
   - `# hunt begin` appears (expected within ~1–10 s of eligibility);
   - `attempts >= 6` in 120 s; start-to-start gaps up to **~70 s are legitimate worst case** (36 s backoff +
     up to ~30 s attempt/settle; review-4 N8), backoff grows to ≈30 s and is jittered, and the invariant
     `max_backoff_ms <= 36000` holds;
   - **no `# hunt reset` while the scene is unchanged**, no crash/assert, and attempts keep coming after the 6th
     failure (best-effort allowed);
   - `why=` should be `nopeak`/`lowtexture`/`deep`/`cold`/`gate`.
2. **Then, without touching the settings, point at a well-lit textured subject** 0.5–2 m away:
   - focus is found and the episode ends (`# hunt reset why=sharp` or a scene reset followed by a landing) within
     **≤~70 s worst case, expected <5 s** (scene/blur change resets and re-triggers immediately; a pending
     36 s backoff plus a slow attempt is the worst case);
   - the picture is visibly sharp and no further `# hunt attempt` lines appear while it stays sharp.
3. **Normal-mode control:** repeat 1 with `Refocus persistence = Normal` for 2 minutes. Expect the current
   patches 1–8 behaviour (a few attempts, then pause/gate 6 dead stretches) — the difference proves the setting.
4. **Refocus test (Persistent):** must stay `notrig=0 valid=5 false_pass=0`, `ok >= 4/5`, `false_trig=0`, same
   `ref_s` behaviour (hunt reset around the test).
5. **AF bench B speeds:** unchanged vs the pre-patch build (the hunt does not run; Normal contract).
6. **Walk A↔B:** every blur episode ends `sharp`/user/scene; on normal subjects <=3 attempts per episode;
   `refmax`/anchor do not staircase; no false episode on an already-sharp picture (report any `# hunt begin`
   immediately followed by `# hunt reset why=sharp` with no visible blur).
7. **Recording spot test:** AUTO refocus ON+REC, MPU drive while recording ON, monitor already open: during
   recording only in-place/pre-opened-MPU attempts, no monitor open/close, no `task_create`, no card write;
   `# hunt` text appears in AFDRIVE after recording stops, log complete.
8. **Host:** `python3 contrib/af_test/run_all.py` must stay `RESULT: PASS` (it is: 84/84 + 6/6).

### H — Rollback

- **Soft rollback (no rebuild):** `Refocus persistence = Normal` (or Auto refocus OFF). Patches 1–8 behaviour
  returns immediately; this is the field escape hatch if Persistent is annoying.
- **Firmware rollback of patch 9 only:** revert the patch-9 hunks (setting/menu/config, hunt block, aref_step
  hooks, engine `AFD_F_HUNT` overrides, sweep direction, aref test/task resets). The file is uncommitted with
  patches 1–8, so keep a pre-patch-9 copy/`crop_rec.mo` md5 before flashing; full rollback of the whole AF stack is
  `git checkout -- modules/crop_rec/crop_rec.c` + rebuild + reflash.
- After any rollback: rebuild twice, re-run `contrib/af_test/run_all.py`, reflash.

### I — Residual risks (ranked)

1. **False hunting on an actually-in-focus scene.** Persistent trusts the frozen base reference / absolute metrics;
   a stale-high anchor/ref max or a low-contrast "sharp" scene can look blurry. Mitigations: entry needs real
   evidence or cold start; half-press/MF/menu/servo pause it; best-effort after 6 no-improvement fails; Normal
   mode. Expected symptoms are visible `# hunt attempt` on a good picture or a short episode that returns
   `why=sharp` with no visible blur — field reports must flag both; thresholds are deliberately not tuned yet.
2. **Provisional reference deferred (see E).** A soft-but-"successful" first landing on a camera that booted blurry
   can seed the anchor/ref max; a later episode can accept 92 % of it. The frozen episode base prevents the
   immediate self-acceptance; the cross-episode case needs the planned trust flag after field data.
3. **Battery / lens wear on a hopeless scene.** Bounded but real: roughly one attempt per ~30–70 s (≈50–80/h)
   at the cap,
   duty roughly 7–15 % on hopeless scenes; best-effort drops to cheap Canon legs, end-stop taxonomy forces
   reversal, per-attempt travel is capped, Normal setting exists.
4. **Recording disruption.** Canon in-place AF while recording can visibly hunt and interact with the camera's own
   movie AF; the throttle/no-x10/no-monitor-ops rules and the ON+REC opt-in bound it, but a take can still be
   disturbed. Persistent recording remains opt-in.
5. **Canon x10 detour visibility** (magnification blink): one of five legs, skipped while recording; portfolio order
   is a table if field feedback is negative.
6. **Cold-start heuristic** (`peak < pmin` or `q >= 85 && emax < 100`) is calibrated on a small number of 18–20 mm
   scenes; it could start an episode on a sharp but low-detail/dark scene. The episode then either finds focus or
   goes best-effort; `# hunt begin why=cold` with a visibly sharp picture is the signal to tighten it.
7. **Log volume / note buffer:** text lines are rate-limited while recording and the compact kind-17 record carries
   per-attempt data, but the 2 KB `afd_note` buffer can still drop a line in an extreme session (status review R8);
   the reset/summary pair is the fallback evidence.
8. **No camera validation of the new constants** (`HUNT_COLD_START_MS`, `HUNT_SHARP_PCT`, `HUNT_IMPROVE_PCT`,
   `HUNT_BESTEFFORT_N`, `HUNT_DARK_LUMA`); they are isolated defines and the Normal/Persistent A/B is the tuning
   path. No camera is attached for this patch.
9. **Interaction with the user's own AF:** `AREF_FOREIGN` resets the hunt and half-press pauses it; if a future
   Canon path re-enters, verify no ping-pong between the hunt and user AF.
10. **Extra focal-length bucket calls (patch-5 R2 residual).** `aref_hunt_ref()` uses `aref_anchor_ok()`, which
    calls the stateful/debounced `afd_mm_bucket()`, and the cold-start check calls it on idle polls; this adds
    calls to the already-known call-counted debounce (review-3 R2). It can only make a stray mm glitch "confirm"
    slightly sooner (a reference reset), not change a focus decision; the same residual class already exists on
    the patch 5 path and is non-blocking. A future patch should make the bucket pure/call-once per sample.

---

## 11.16 Patch 9.1 (task-26): review-4 should-fixes

**Status:** implemented, compile-checked only (no camera), not field-validated. Supersedes the patch-9 build in
§11.15 (behaviour of the hunt is unchanged except the fixes below; Normal mode is still patches 1–8).
**Review:** [AF-PATCH-REVIEW-4.md](AF-PATCH-REVIEW-4.md) §7 S1–S7. S5 (the +10 % judge path) is deliberately left
as a documented residual, not changed (no camera data to tune it).

**Diff/build:** `modules/crop_rec/crop_rec.c` +1784/−139 vs `HEAD` (patches 1–9.1; **9.1 alone +59/−0**). Build
exit 0 twice (forced `touch` between), `crop_rec.mo` md5 **`4b1bd45433e2f52d4e9e7ea15fd63e17`** both times,
crop_rec.c compiles clean; host suite `python3 contrib/af_test/run_all.py` → **84/84, 4/4 tiers, 6/6 corpus,
PASS**. No commits.

### S1 — patch-8 auto-zoom poll: no recording, no log flood, real stop

- `aref_step` auto-zoom guard (`crop_rec.c:15223–15226`) now also requires
  `!RECORDING && !RECORDING_RAW && !mlv_raw_rec_busy()`, so the 250 ms blocking `afd_lens_refresh` poll and the
  `PROP_LV_LENS` request never run during ON+REC (they previously ran ~1/s while recording, filling the 2 KB
  `afd_note` buffer with `# zoom probe` lines and dropping the `# hunt` evidence of the recording spot test).
- `afd_lens_refresh` (`crop_rec.c:8155`) logs `# zoom probe` **only when `old_mm != new_mm`** (a stale reply with
  an unchanged value no longer writes a line); the manual zoom-refresh action still shows its own result lines.
- After `aref_zoom_noted` is set (3 polls without a fresh report) the poll really stops
  (`aref_zoom_next = now + 3600000`, `crop_rec.c:15237`) instead of the old flag-only behaviour; the guard also
  includes `!aref_zoom_noted`.
- Effect on Normal mode: only the patch-8 zoom poll is affected (recording safety/less log volume); no AF
  decision changes.

### S2 — no immediate false-hunt re-begin on a sharp picture

- The sharp branch of `aref_hunt_close` (`crop_rec.c:14842–14849`) now clears `aref_paused` and `aref_fails` and
  sets `aref_next_at = now + AREF_MPU_MIN_GAP_MS` (4 s cooldown), so a `# hunt reset why=sharp` is followed by a
  normal cooldown instead of a new episode on the same in-focus scene (the sharp-close is used both by the
  between-attempt check and the judge).
- The IDLE entry block (`crop_rec.c:15118–15126`) skips starting an episode when the current median is already
  **≥ 92 % of the trusted reference** (`aref_hunt_ref()`; the decaying peak is not used, as before), logging at
  most once per 10 s:
  `# hunt skip why=sharp ref=%d med=%d t=%d`.

### S3 — Persistent MPU hunt requires monitor = Keep open

- `aref_hunt_mpu_ok` (`crop_rec.c:14760–14762`) now requires `afdiag_afdrive_mon == 2`. With monitor **ON** the
  engine would open/close the monitor around every attempt (~2 s LV blackout each, forever on a hopeless scene);
  Persistent then uses the Canon legs only.
- Logged once when Persistent is running with monitor ON (`crop_rec.c:15061–15067`):
  `# hunt skip why=monitor_mode t=%d`. The note re-arms if the setting is later changed back to Keep open.

### S4 — timeout/travel abort advances the backoff

- `aref_mpu_exec` publishes `aref_mpu_timeout = afd_timeout || afd_travel_cap` (`crop_rec.c:13943`); a new
  `afd_travel_cap` flag is set by the two `AFD_MAX_TOTAL` checks in `afd_move_track`/`afd_move_ex`
  (`crop_rec.c:7975/8169`) and cleared at engine start.
- In the MPU attempt completion, an engine run that ends `pct < 0` without nopeak/lowcontrast and has
  `aref_mpu_timeout` is now a **failure** (`HUNT_TX_ABORT`, backoff grows) instead of the old 2 s retry
  (`crop_rec.c:15308–15312`). User/menu/LV aborts (no timeout/travel cap) still retry after 2 s; a dead worker
  still retries after its 4 s watchdog.

### S6 — log volume

- `# hunt pause gate=...` is logged only when the eligibility mask changes (global `aref_hunt_pause_blk`;
  reset on episode start/end and when eligibility returns, `crop_rec.c:14882–14890`, `15444`), not every 2 s.
- The completion-watchdog `# hunt outcome ... why=timeout` goes through `aref_hunt_log_allowed` like every other
  attempt/outcome line (`crop_rec.c:15095–15099`).

### S7 — taxonomy

- A soft judge is now tagged `HUNT_TX_SOFT`, not the previous failure's taxonomy; only the current MPU attempt's
  own end-stop fact (`aref_mpu_esdir`) can tag `HUNT_TX_ENDSTOP` (`crop_rec.c:15030–15032`).

### Acceptance wording corrected (§11.15 B/G)

The maximum **start-to-start** gap is ~70 s worst case (36 s backoff cap + up to ~30 s attempt/settle; review-4
N8), **not 45 s**. The field invariant to check is `max_backoff_ms <= 36000`; gaps up to ~70 s are legitimate and
must not be flagged. The textured-subject success is likewise ≤~70 s worst case (expected <5 s thanks to the
scene/blur reset). §11.15 B, G1 and G2 have been corrected accordingly.

### Residual risk accepted after 9.1

- **S5 (+10 % success path)** is unchanged: on a textured-but-unfocusable scene a lens move can improve the
  median >10 % over the single pre-attempt sample with `emax >= 70`, and the hunt can end `why=sharp` on a
  visibly soft picture. Accepted for the next field session; report every such `# hunt reset why=sharp` with a
  visibly soft picture. A future patch can require two consecutive samples above the +10 % level or a weak
  absolute floor (`med >= 0.6 * trusted ref`), once field data exists.
- The provisional-reference trust flag remains deferred from patch 9 (see §11.15 E).
- `# hunt outcome`/attempt indexing (`n`) remains as in patch 9 (nits N3/N4/N9 in review 4, cosmetic).

### Rollback (9.1)

- The fixes are independent small hunks: revert them and rebuild to return to the §11.15 patch-9 build.
- Soft rollback remains `Refocus persistence = Normal` (patches 1–8 behaviour, including the recording-gated
  patch-8 zoom poll).
- Full rollback of the AF stack: `git checkout -- modules/crop_rec/crop_rec.c` + rebuild + reflash.

### 11.17 Patch 10 (task-27): hunt false-sharp fix, good-enough tolerance, learned fine step, faster runs, 55 mm fallbacks

**Status:** implemented, **compile-checked only** (no camera). From the 2026-10-19 late session
(`~/af_logs/af-20261019-222942/ML/LOGS/`): successful runs median `total_ms` 7433 + `epilogue_ms` 1773
(~9.2 s wall, ~15 moves at ~450 ms); hunt logs showed 111 episodes, 93 cold-begin, and 75 of them
`# hunt reset why=sharp attempts=0 travel=0` within 60–120 ms (only 19 attempts total). Patches 1–9.1 are
preserved; menu AF / bench / Refocus test and the Canon-method paths keep their old strict behaviour.

**A — hunt false-sharp bug (the "does not try when really out of focus" cause).** `aref_hunt_pre_s` was only
set in `aref_hunt_launch` and never cleared, so a cold/gate episode saw the previous episode's pre-attempt
sharpness and the +10 % improvement test in `aref_hunt_is_sharp()` closed it immediately as "sharp" with
0 attempts. Fixed:
- `aref_hunt_pre_s` is cleared in `aref_hunt_begin` and `aref_hunt_close`; the +10 % rule can only fire
  after an attempt actually started (`pre_s != 0`);
- after a sharp close no new episode may begin for `HUNT_SHARP_COOLDOWN_MS` (6 s); the wait is logged
  (rate-limited) as `# hunt skip why=cooldown remain_ms=.. t=..` (the old `# hunt skip why=sharp` remains
  for the 92 %/tolerance pre-check);
- the cold/gate4 entry conditions are unchanged in content (no trusted ref + absolute-blur evidence /
  gate4 with a usable peak), but now they actually get to run: a cold scene begins an episode and launches
  an attempt instead of being closed as sharp. `# hunt begin` also logs the tolerance (`tol=`).

**B — good-enough tolerance.** New setting **"Focus tolerance"** (Settings → Auto refocus group,
`CONFIG_INT crop.auto_refocus_tol`, default **Normal 88 %**): Strict 92 (old behaviour), Normal 88,
Relaxed 85. It is used by (i) the hunt sharp judge and its S2 pre-check, and (ii) the Auto-refocus/hunt
local search: the soft/post-climb guard threshold and the local landing retrace threshold
(`afd_rt_thr1`). Menu AF, bench and the Refocus test keep 92 (the Refocus test passes the new `AFD_F_RT`
flag so its `AFD_F_AREF` local runs stay strict). A soft landing is still a failure (the 50 % far checks
and B fallback are unchanged). The tolerance used is logged: `tol=` on `# hunt begin` and `# local s0=...`
lines, and `tol=` on the engine `# focal_mm=...` line. This is the "widen the window of good enough"
request; it does not raise the hunt's frozen reference, it only accepts a slightly softer picture as the
end of an episode.

**C — learned fine step.** After every B/C sweep with a clear peak (`afd_sweep`), the peak width is
measured from the s-curve: the time span where `s >= 70 %` of `smax` around the peak, converted to pulses
with that move's speed model (`mspp`), clamped 4..80, and stored per zoom + focal-length bucket
(`afd_fine_learned`); the line is
`# fine learned mm=.. zoom=.. val=.. span_ms=.. mspp_x10=..`. On a zoom/mm key change it is dropped
(`# fine learned reset ...`). When **Fine step = Auto** and the run is Auto-refocus/hunt, the engine uses
the learned value instead of the focal-length guess (`# fine learned mm=.. val=.. auto=1`); a manual Fine
step is never overridden, and without a sweep the focal-length behaviour is unchanged. Target: the
EF50 + speedbooster (which reports 35 mm and got fine=14) learns a much larger step (~40–80 pulses), so
the first local probes see a slope instead of falling back to a full B search.

**D — speed (Auto-refocus/hunt runs only; menu/bench/Refocus test unchanged, no safety check skipped).**
(i) **Fast epilogue** when `afd_tol_run` and Session close is not Restore: stop wait 150 ms instead of
500, session close, final sample after 150 ms, and no 1000 ms post-close wait / `post_close2` / restore
block; logs `# epilogue fast pre_stop_s=.. post_stop_s=.. post_close_s=..`. (ii) **Landing verify skip**:
for Auto-refocus/hunt runs with `land_pct >= 90` the 300 ms verify + re-land is skipped (the hunt judge
/ aref settle judges the picture anyway); logs `# verify skipped land_pct=..`. There is no move-count
change beyond that; all abort/deadline/travel checks stay. Expected ~1.5–1.9 s back per run.

**E — deep defocus / 55 mm.** (i) Refocus-test reference path: if the B07 reference landing fails
(nopeak/lowtexture), the test no longer aborts; it retries with a **full B sweep** (using the learned fine
step when Fine step = Auto) and only fails if that also fails. Logs:
`# RT_REF fallback B07 failed nopeak=.. lowcontrast=.. aborted=.. -> full B` and
`# RT_REF fallback full B ok ref=.. best=.. fine=..` (or `# RT_REF fallback failed ...`). (ii) The B
sweep's "land flat -> resweep" path now does a **full-width re-sweep** from the current position instead
of the old 4-chunk local re-sweep that missed the focus region; logs
`# land flat -> full resweep chunks=.. (was ..)`. Deadline / travel caps still bound it.

**Settings summary:** "Focus tolerance" Strict 92 / Normal 88 (default) / Relaxed 85 — Auto refocus/hunt
only. "Fine step" Auto now consumes the learned value; manual values unchanged.

**Acceptance:**
- (a) Deliberately blurry scene in Persistent mode: `# hunt begin` followed by `# hunt attempt`/`# hunt
  outcome` lines; no repeated `# hunt reset why=sharp attempts=0` within the first second. After a real
  sharp fix, `# hunt reset why=sharp` once, then `# hunt skip why=cooldown` (not an immediate re-begin).
- (b) With Normal tolerance, an episode closes when the settled picture is >= 88 % of the episode's frozen
  reference (visible in `# hunt begin ... ref=..` + `# hunt outcome ... sharp=1`); Strict reproduces the
  old 92 % behaviour; the tolerance is printed (`tol=88`).
- (c) After a B sweep at the EF50+SB position, `# fine learned ... val=..` appears with a value clearly
  above the old focal guess, and the next `# fine learned ... auto=1` line is the value used by local
  probes; `# local ... fine=` shows it; manual Fine step never shows the `auto=1` line.
- (d) Successful 18 mm Auto-refocus wall time: `# epilogue fast ...` instead of `# epilogue ...` for
  AREF runs, `# verify skipped land_pct=..` for good landings; expect ~30–45 % median reduction (target
  <=6.5 s median, warm runs <=3 s). `# RESULT ... epilogue_ms=` is clearly lower (~0.3–0.5 s).
- (e) 55 mm Refocus test that previously aborted on a failed B07 reference now shows the
  `# RT_REF fallback ...` lines and continues; only shows the abort when both attempts fail.
- (f) Menu AF drive, bench, speed/stop/sweep/reversal tests and the Refocus test (including its five
  trials) keep their strict thresholds and full epilogue; the tolerance/learned/verify changes are gated by
  `afd_tol_run` (AFD_F_AREF/hunt, not AFD_F_RT).

**Rollback:** the patch is split into independent hunks: `HUNT_SHARP_COOLDOWN_MS`/pre_s clears (A),
`aref_tol_pct()` + `afd_tol_run` gating (B), `afd_fine_learn*` (C), the two `# epilogue fast` /
`# verify skipped` branches (D), `# RT_REF fallback` and the full resweep (E). Removing the corresponding
hunks and rebuilding returns to the §11.15/§11.16 patch-9.1 build; `Focus tolerance = Strict` restores the
old 92 % thresholds without a code change, and the learned fine step is only used with Fine step = Auto.
Full rollback of the AF stack: `git checkout -- modules/crop_rec/crop_rec.c` + rebuild.

**Residual risks / deferred items:** no camera validation; the tolerance thresholds and the 6 s hunt
cooldown are reasoned from the session, not measured; the learned fine step needs one successful B/C sweep
at the current zoom/mm before it helps (cold boot keeps the focal guess), and learning from a coarse 1F
sweep is clamped; a full re-sweep after a flat landing can cost up to ~6 s within the existing deadline;
the fast epilogue removes the 1 s post-close observation sample for Auto-refocus runs (`post_close2_s` =
`post_close_s` there), so a lens that moves after session close is still caught by the hunt/settle judge but
not by that sample; the tolerance widens what counts as "good enough" but does not fix a soft reference
(the hunt judge still uses the frozen trusted reference and the absolute path); normal persistence mode is
structurally unchanged (A only affects Persistent).
