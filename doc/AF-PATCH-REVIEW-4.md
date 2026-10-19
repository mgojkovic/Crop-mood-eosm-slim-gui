# AF patch review 4: patch 9 — persistent never-give-up hunt (safety/regression)

**Status:** adversarial review, **offline (no camera)**. Read-only; I did not edit
`crop_rec.c` and did not run `make`.
**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` · **Base:** HEAD `043d914`.
**Artifact reviewed:** `/tmp/af_patch9_full.diff`, md5
`5d8bb74af449c1fca840b324ef50c3ff` (full diff vs HEAD, patches 1–9), pinned
identical to `git diff modules/crop_rec/crop_rec.c` at the start **and end** of
this review (reconstructed under `/tmp/snap9`; all line refs below are from that
snapshot). Read: [AF-HUNT-FOREVER.md](AF-HUNT-FOREVER.md),
[AF-PATCH2-FIELD-PLAN.md](AF-PATCH2-FIELD-PLAN.md) §11.15, and my previous reviews
[1](AF-PATCH-REVIEW.md) / [2](AF-PATCH-REVIEW-2.md) / [3](AF-PATCH-REVIEW-3.md).
**Build/host evidence:** in-tree `modules/crop_rec/crop_rec.mo` md5
`3d5f18e345b14f3daaf6aacad31dd556` matches the patch-9 plan's deterministic build;
I cannot rebuild. I re-ran `python3 contrib/af_test/run_all.py` myself → **84/84**
unit tests, 4/4 tiers, **6/6** corpus cases, `RESULT: PASS`.

---

## 0. Verdict

**Safe to flash. No blocking defect in patch 9 itself.** The Normal-mode contract
holds (static proof, §2), the never-stop invariant is structurally closed (§3),
the judge arithmetic is sound and non-self-referential (§4), the engine override
is HUNT-flag-scoped and cannot leak into Normal/menu/bench runs (§5), and the
hunt introduces no task, no monitor operation of its own and no card write while
recording (§6).

Two caveats, neither a flash blocker for the planned (non-recording) session:

1. **Fix one line before the ON+REC recording spot test.** The *patch-8*
   auto-zoom poll (`aref_step`, 15175–15204) is **not gated on recording**: with
   Auto refocus ON+REC and the Keep-open monitor, it runs during RAW recording,
   blocks the polling callback up to 250 ms once a second when the MPU does not
   answer (`afd_lens_refresh`, 8137–8155), and, when it *does* answer, writes a
   `# zoom probe` note about once a second → the 2 KB `afd_note` buffer is
   disabled from flushing while recording and fills in ≈40 s, after which the
   `# hunt` lines of the recording test are silently dropped. That undermines
   plan acceptance item 7 ("no monitor ops / `# hunt` log complete") and can
   disturb the take. This is a patch-8 × patch-9 interaction; a one-line guard
   (`!RECORDING && !RECORDING_RAW && !mlv_raw_rec_busy()` in the auto-zoom block)
   fixes most of it; also log the probe only when mm changes and actually stop
   polling after `aref_zoom_noted` (the plan says it stops; the code does not).
2. **Persistent is the default** (`Auto_refocus_persist = 1`). This is intended
   and documented, and Normal is an exact patches-1–8 rollback (verified), but it
   is a behaviour change for every user who already runs Auto refocus — the field
   plan's A/B (Persistent vs Normal, store A/B) is the right control.

Nothing else found rises to "fix first": the remaining findings are should-fix /
nit (§7) and are either documented residuals or small log/label issues.

---

## 1. What patch 9 adds (verified from code, not only from the plan)

- Setting `Auto_refocus_persist` (CONFIG_INT 132, menu 6003, MODULE_CONFIG
  17292), default 1.
- Hunt state machine + helpers: declarations 13796–13874, helper block
  14648–15115 (`aref_hunt_ref/abs_sharp/is_sharp/med/delay/mpu_ok/available/
  order/pick/log_allowed/close/reset/pause/begin/launch/schedule/fail/
  finish_attempt/settle/judge/step`). One call site: `aref_step` 15408–15413.
- Engine override: `AFD_F_HUNT` (7543), `afd_hunt_algo/speed/dir` (7618–7622),
  cleared for non-HUNT runs (10480–10486) and at engine end (10810–10812),
  applied only for `mode == 0 && AFD_F_HUNT` (10499–10507); forced B direction in
  `afd_run_sweep` after the probe (9229–9240).
- Hooks: `aref_mpu_exec` publishes `aref_mpu_esdir/nopeak/lowc/travel/dir`
  (13924–13932); `aref_mpu_settle` calls `aref_hunt_settle` (14207);
  `aref_sw_push` scene reset (14221); `aref_dec.emax` for the absolute judge
  (14321); all reset/pause hooks in `aref_step` (15127–15398); RT test resets
  (15840/15847).

---

## 2. Normal-mode contract: `Auto_refocus_persist == 0` is a no-op for patch 9

I audited every patch-9 execution site on the Normal path. The only additions
are calls that return immediately in `HUNT_IDLE`, or one global write that no
pre-patch-9 decision reads:

| Site | Normal-mode effect |
|---|---|
| `if (!Auto_refocus_persist) aref_hunt_reset("off")` (15127) | `aref_hunt_reset` calls `aref_hunt_close` only if `state != HUNT_IDLE` (14850–14853); state is IDLE unless the user just switched from Persistent, in which case the reset is the intended "off" end. No clock call, no log, no state write otherwise. |
| diag/`toggle`/`method`/`rawrec`/`zoom`/`user_af`/`rtest` resets | same `HUNT_IDLE` early-return; no log, no lens/AF side effect. |
| `aref_hunt_pause` in the `blk` branch (15378) | first line returns when IDLE (14860). |
| `aref_hunt_settle` at the end of `aref_mpu_settle` (14207) | returns unless `state == HUNT_JUDGE && active && attempt_mpu` (15024). |
| `if (Auto_refocus_persist) { aref_hunt_step(...); return; }` (15408–15413) | Normal falls through to the **unchanged** legacy launch block below. |
| engine override clear (10480–10486, 10810–10812) | writes three globals that are only *read* when `flags & AFD_F_HUNT` (10499–10507) / `afd_hunt_dir` in the sweep (9232) — and a non-HUNT engine call clears them before any sweep. No patch-1–8 run can observe them (hunt launch is the only setter). |
| `aref_dec.emax = emax` (14321) | new struct field; grep confirms it is read only inside the hunt judge (14710/14712/14715/14717/15079). |
| `aref_mpu_exec` publishes extra volatile facts, `aref_hunt_cold_t = now` (15398) | writes only; no decision reads them in Normal. |

No new lens moves, property requests, logs, timers or AF launches occur on the
Normal path, and the engine's forced-direction branch is dead when
`afd_hunt_dir == 0` (guaranteed for non-HUNT runs by the entry clear). **Normal
mode is exactly patches 1–8** (camera A/B still recommended as the plan says).

---

## 3. Never-stop invariant: no idle escape, one attempt in flight

State transitions audited from the assignment sites (only `aref_hunt_close` sets
`HUNT_IDLE`, 14827; the only other writer is the static initialiser 13841):

- **IDLE** → begins only on a trigger path, gate 6/7/11, gate 4 with a usable
  peak, or the 8 s cold start (15071–15083); `aref_hunt_begin` sets
  `next_at = now` and falls straight into the ARM launch logic. So an episode
  cannot be "created idle".
- **ARM**: each poll either (a) sharp-check closes it (15041–15046), (b) waits
  for `next_at`, (c) waits 500 ms for a settled scene / a busy AF machinery path
  (15087–15098) — timer refreshed, no failure consumed — or (d) launches one
  attempt (15100–15114). `next_at` is always finite and ≤ `now + 36 s` when set
  by a failure (`aref_hunt_schedule` 14982–14984, delay clamped 3–36 s,
  14728–14743). No attempt-count cap anywhere (`aref_hunt_n` only feeds the
  backoff index, rotation and logs).
- **ATTEMPT**: completion is handled by the phase machinery before the hunt is
  consulted: MPU finish → `HUNT_JUDGE` (pct ≥ 0), taxonomy failure (nopeak/lowc/
  endstop/dark), or abort-pause + retry in 2 s (15236–15264); Canon finish →
  judge or `canon_fail` failure (15343–15358); request dropped / worker not
  responding → `HUNT_ARM` + 2 s / 4 s retry (15222–15229, 15281–15289). The 30 s
  `HUNT_ATTEMPT_TIMEOUT_MS` watchdog (15052–15069) is a backstop; because
  `aref_step` returns from the phase branches before `aref_hunt_step`, that
  watchdog only actually runs if the phase has already fallen back to MON — the
  real liveness comes from the phase handlers plus the pre-existing 1.5 s
  worker-pickup watchdog and the engine caps. No stuck-ATTEMPT path found
  (see nit N1 for the over-strong prose in the plan).
- **JUDGE**: `judge_at = now + AREF_SETTLE_MS + 1500`; the settle hook or the
  timer always calls `aref_hunt_finish_attempt`, which either closes (sharp) or
  schedules (failure) — 15021–15032, 15003–15019.
- **One attempt in flight**: guarded by `attempt_active` (15086), by the phase
  check `aref_phase != AREF_MON || ...` (15094–15098), and by the state machine
  itself (IDLE→ARM→ATTEMPT→JUDGE/ARM). Launch sets the phase/worker request in
  the same call; no second launch can pass the guards. Reset paths during an
  in-flight attempt set `state = IDLE` and `attempt_active = 0`; the later
  completion is ignored (the `if (attempt_active && ...)` blocks) — no double
  judge, no double schedule.
- **Resets**: scene (14221), user AF (15305), zoom auto (15198) and
  eligibility zoom/mm (15395), toggle (15144), method (15152), RAW rec
  (15166), Refocus test (15840/15847), menu diag (15130), and Normal/off
  (15127). Auto refocus OFF goes through the toggle block (15133–15145). Sharp
  is the only success end.
- **Aborted attempts retry in 2 s** (15260, 15286) and do not consume the
  backoff; legitimate ineligibility pauses (menu/MF/servo/half-press/LV/busy)
  preserve state and timer (14857–14865, 15378).

**Verdict on the invariant:** structurally closed. Two practical caveats:
(i) a permanently unsettled scene keeps the hunt in ARM (waiting for
`aref_dec.settled`) without attempts — a legitimate "don't fight motion" pause,
not an idle escape; (ii) a *hung engine* is covered by the pre-existing worker
watchdog/engine caps, not by the hunt watchdog (N1). A pathological
timeout/travel-abort loop is S4.

---

## 4. Judge: frozen base, soft landing = failure, arithmetic

- `aref_hunt_ref()` (14683–14692) trusts only the patch-3 anchor, the per-zoom/mm
  engine ref max and the last verified landing — never the decaying `aref_ref`
  peak memory. `aref_hunt_base_ref` is frozen in `aref_hunt_begin` (14885), so a
  reference (re)learned from a soft landing during the episode cannot become the
  success bar. Confirmed this is the only input to the sharp check besides
  absolute metrics and the pre-attempt sample.
- `HUNT_SHARP_PCT 92`: `(uint64_t) med * 100 >= (uint64_t) ref * 92` — 64-bit, no
  overflow (both values are the same ex-scale sharpness); a run that reports
  `pct >= 0` but settles below this is a failure (`aref_hunt_finish_attempt`
  15003–15019: judge on the settled `med`, not on `aref_mpu_pct`). No
  self-referential `pct` acceptance anywhere in the hunt.
- Absolute-sharp: `q < AREF_ABS_Q_GOOD (85) && emax >= AREF_ABS_EM_GOOD (100)`
  (14694–14697), using `aref_dec.q/emax` from the latest sample — the same
  constants patch 6 uses for "sharp, not blur".
- +10 % path: `med >= pre_s * 1.10` **and** `emax >= AREF_PEAK_EMAX_MIN (70)` —
  the pre-attempt sample is a single sample; this is the lenient cold-start
  evidence (risk: S5).
- After 6 consecutive no-improvement fails → `HUNT_BESTEFFORT_N` → cheap
  Canon-only preference, but pass 2 falls back to any available strategy, so the
  state machine cannot stall; attempts continue at the 30 s cap (14788–14805,
  14973–14989).
- `aref_hunt_med()` = median of the last 3 samples (or current), the same
  measure the settle uses — consistent with `med` passed from
  `aref_mpu_settle` (14207).

---

## 5. Engine override / portfolio / Canon paths

- `AFD_F_HUNT` is applied **after** the menu algorithm and the
  AREF-local selection (10495–10507), only for `mode == 0`; the model/speed/
  direction are cleared for every non-HUNT engine run at entry (10480–10486) and
  at engine end (10810–10812), so menu AF, bench, speed, sweep, stop and
  reversal-cal cannot inherit a hunt strategy. `afd_hunt_dir` is read only in
  `afd_run_sweep` and is 0 for all non-HUNT runs.
- Forced reversed sweep (9229–9240): the override is applied *after* the probe;
  if the probe already saw an end stop in the forced direction the override is
  refused and logged; the sweep still starts from the recorded `pos0` and is
  bounded by `AFD_SW_MAX_CHUNKS`, `afd_travel`/`AFD_MAX_TOTAL` and the run caps,
  so a forced direction cannot exceed the normal per-attempt travel budget. It
  can be sent into the *other* end stop, which is handled by end-stop detection
  → `nopeak`/`endstop` taxonomy → reversed leg next (bounded, not a fight loop;
  but see S4 for the 2 s abort retry).
- Portfolio (14650–14805): five strategies, availability-filtered (MPU needs
  method MPU + monitor setting + worker; while recording ON+REC + Keep-open
  open + worker; Canon x10 non-recording only; Canon in-place recording-safe),
  taxonomy reorder table, best-effort two-pass so it never dead-ends. Local uses
  `Auto_refocus_af ? B : LOCAL` like the legacy path; the other MPU legs force B.
- Canon paths reuse the existing `slim_af_request` 1 (x10) / 2 (in-place)
  machinery (14917–14929): one request at a time, the existing
  REQUESTED/OURS/FOREIGN phase tracking judges completion, request-dropped and
  worker cases are handled; `slim_af_can_start*` enforce the same eligibility as
  the legacy Canon fallback (x10 also requires `lv_dispsize == 5` and excludes
  recording; the x10 detour is visual but pre-existing behaviour for Canon AF).
- `AFD_F_AREF` is kept on hunt MPU runs, so the patch-2 servo abort and the
  patch-1/3/5 local/aref bookkeeping apply unchanged; a hunt MPU run's verified
  landing still feeds the ref max/anchor (patch 4/5 recovery still runs), while
  the judge uses the frozen base.

---

## 6. Safety

- **Tasks:** the hunt creates none. It can trigger the pre-existing lazy worker
  creation through `afd_worker_ensure()` in `aref_hunt_mpu_ok` (14756) — the
  same call the legacy Auto-refocus path makes, and it returns 0 while
  recording. No new task priorities.
- **Monitor:** patch-9 code performs no monitor open/close. Caveat S3: with the
  monitor setting **ON** (not Keep open), every MPU hunt attempt opens and closes
  the monitor in the engine (~2 s LV blackout each time) — pre-existing per-run
  engine behaviour, but Persistent makes it recurring. The plan's "never
  opens/closes the monitor" is only true for the Keep-open setting.
- **Recording:** MPU hunt attempts are gated to ON+REC + `Auto_refocus_mpu_rec`
  + Keep-open monitor already open + existing worker (14747–14757); x10 is
  excluded while recording; in-place Canon is the recording-safe leg. No card
  writes (`# hunt` goes through `afd_note`; `afd_note_flush` refuses while
  recording, 9905–9911) and engine logs stay held. Caveat S1 (auto-zoom poll
  during recording + note-buffer flood) is the one recording interaction that
  needs attention.
- **Rate limits / 2 KB buffer:** while recording, attempt/outcome text is limited
  to strategy changes (14808–14815, used at 14909/14995/15008); begin/reset/
  summary are always logged (small and important). **Not** rate-limited:
  `# hunt pause` repeats every 2 s while the same `blk` persists (14857–14864),
  the timeout outcome (15065), and the patch-8 `# zoom probe` ~1/s. The 2 KB
  buffer drops silently when full (9889–9901); with S1 present a long ON+REC
  take can lose the hunt evidence. Fix S1/S6.
- **CPU / blocking:** no msleep or loop in the hunt state machine; the only
  blocking wait on the hunt path is the patch-8 auto-zoom `afd_lens_refresh(250)`
  (S1). No busy-wait; `afd_worker_ensure` is the pre-existing path.
- **Abort/deadline:** engine runs use the unchanged `afd_must_abort` /
  `afd_deadline` / caps; hunt aborts are classified and retried (2 s) rather
  than counted as scene failures, which is safe but see S4.
- **Cross-task:** `afd_hunt_algo/speed/dir` are written by the aref context
  before `afd_wk_req = 1` (volatile) and read by the worker after it observes
  the request — safe direction, same pattern as the patch-9 trigger hints.
  `aref_mpu_esdir/nopeak/lowc/travel/dir` are `volatile` and written in the
  worker before `aref_mpu_state = 2`. All hunt state itself is aref-context
  only. No new race found (the pre-existing `afd_note` advisory lock and
  cross-context statics remain as before, N-note).

---

## 7. Findings (severity-ranked; nothing here blocks the main field session)

### S1 (should-fix, do this before the ON+REC recording spot test) — the patch-8 auto-zoom poll runs during recording and floods/drops the note buffer

**Where:** `aref_step` auto-zoom block 15175–15204 (`afd_lens_refresh(250, &mm)`
15181); `afd_lens_refresh` 8137–8155 (20 ms poll loop up to `wait_ms`; logs
`# zoom probe` on every reply 8152); `afd_note` 2 KB buffer 9885–9901, flush
refused while recording 9905–9911.

The guard checks Auto refocus ON, MPU method, Keep-open monitor open, not
AF-busy, phase MON — but **not recording**. During ON+REC the monitor is open by
definition, so this poll runs once a second during RAW recording:
- if the MPU does not answer (the premise of patch 8), each poll blocks the
  polling callback up to 250 ms (documented as a patch-8 residual, but not for
  the recording case);
- if it answers, `# zoom probe ... reply=1` is written ~1/s; `afd_note_flush` is
  disabled during recording, so the 2 KB buffer fills in ≈40 s and then silently
  drops all following notes — exactly the `# hunt` begin/attempt/outcome/reset
  evidence the recording spot test (acceptance item 7) needs;
- `afdiag_request_lens_update()` (a `PROP_LV_LENS` change request) is also sent
  during recording, which patch 8 deliberately avoids for its manual action.
- The plan says the auto poll "stops polling" if the MPU never answers; the code
  only sets `aref_zoom_noted` (15184–15188) and keeps polling every second
  forever.

**Minimal fix:** add `!RECORDING && !RECORDING_RAW && !mlv_raw_rec_busy()` to the
auto-zoom guard; log the probe only when `old_mm != new_mm || first`;
optionally stop the poll after `aref_zoom_noted` (e.g. `aref_zoom_next = now +
3600000`) to match the plan. Verify: ON+REC run — no `# zoom probe` lines and
no 250 ms gaps; `# hunt` evidence intact in AFDRIVE after the take.

### S2 (should-fix) — a hunt episode can re-begin immediately after `reset why=sharp` (false-hunt loop)

**Where:** `aref_hunt_step` IDLE entry 15071–15083 (no sharp check before
`aref_hunt_begin` + immediate launch); `aref_hunt_close` 14819–14848 (does not
clear `aref_paused`/`aref_fails`, does not space the next trigger).

Two mechanisms: (a) if the episode began from a give-up gate (6/7/11) and the
picture is actually sharp, the hunt launches one attempt before any sharp check
(the sharp check at 15041 only runs when `state != IDLE`); (b) after the
`why=sharp` close, `aref_paused` may still be set and/or the aref trigger
reference may still be stale-high, so the next poll can begin a new episode and
launch again. In the worst case this repeats every attempt cycle (few seconds)
on an actually-in-focus picture — the "false hunting" residual #1 of the plan,
but with a cheap partial fix.

**Minimal fix:** in the sharp branch of `aref_hunt_finish_attempt`
(15012–15016) also clear the legacy throttle state (`aref_paused = 0;
aref_fails = 0; afd_note(...)`) and set `aref_next_at = now + AREF_MPU_MIN_GAP_MS`;
and in the IDLE entry block, before `aref_hunt_begin`, skip starting when the
current picture already meets the sharp criterion against the *trusted*
reference (`int cand = aref_hunt_ref(); if (cand && med*100 >= cand*92) return;`)
— keeping the frozen-base/first-attempt semantics for genuinely soft pictures.
Note: do **not** blanket-gate `aref_mpu_*` references; the decaying peak is
deliberately not used.

### S3 (should-fix, documentation/availability) — Persistent + monitor "ON" blinks LV every attempt

**Where:** `aref_hunt_mpu_ok` 14747–14757 accepts `afdiag_afdrive_mon != 0`;
the engine opens the monitor when not open and `mon == 1`, and closes it at the
end of every run (`mon == 1` branch ~10767), i.e. ~2 s LV blackout per attempt.
The hunt's own claims and §11.15 "never opens/closes the monitor" are only true
for **Keep open**. On a hopeless scene in Persistent this repeats every 4–30 s
forever. **Minimal fix:** require `afdiag_afdrive_mon == 2` in
`aref_hunt_mpu_ok` (monitor ON users then get Canon legs in Persistent), or
document "Keep open required for Persistent MPU hunting" in the menu help and
field plan. (No safety consequence beyond UX/battery.)

### S4 (should-fix) — aborted/timeout attempts retry after 2 s with no backoff growth

**Where:** 15255–15263 (engine-abort branch → `now + 2000`), 15281–15289
(Canon request dropped → 2 s), 15222–15229 (worker → 4 s).

The plan intends "aborted attempts retry in 2 s" for user/menu/LV aborts. But an
engine run that hits its own `AFD_RUN_CAP_B_MS`/`AFD_MAX_TOTAL` (timeout/travel
abort) also arrives here (`aref_mpu_pct < 0` with no nopeak/lowc) and retries
after 2 s **without growing the backoff**; a scene that reliably hits the 18 s
cap gives a ~20 s cycle (~90 % duty) with full lens travel. Not a hard safety
bug (bounded, rare on this lens per the bench), but it partially defeats the
backoff/battery protection. **Minimal fix:** in the abort branch, if
`afd_timeout` (published or add `aref_mpu_timeout = afd_timeout`) treat it as a
failure (backoff) instead of a pause; keep 2 s only for user/menu-type aborts.

### S5 (should-fix, judge tuning) — the +10 % success path can end an episode on a still-soft textured scene

**Where:** 14711–14717 (+10 % over the single pre-attempt sample with
`emax >= 70`).

The "2-minute unfocusable scene" acceptance expects the episode to keep going,
but on a textured-but-unfocusable scene (fabric closer than MFD, low-detail
wall with some edges) a lens move can improve the median >10 % over one noisy
pre-sample while `emax` still passes 70, and the hunt closes `why=sharp` on a
visibly soft picture. This is the plan's residual #1 turned into a false
*success*, which would make acceptance item 1 fail for the wrong reason.
**Minimal fix (tuning, no camera data yet):** require the +10 % path to also
clear a weak absolute floor (e.g. `med >= 0.6 * aref_hunt_ref()` when a base
exists, or require two consecutive samples above the +10 % level) — or accept
and document it in the checklist (report `# hunt reset why=sharp` with a
visibly soft picture).

### S6 (nit) — log rate limits don't match the plan

- `# hunt pause gate=...` is refreshed every 2 s for the same `blk` (14857–14864);
  during a long ineligible period it can fill the note buffer (~40–50 lines) and
  drop later lines. Log on `blk` change or ≥10 s.
- The timeout outcome (15065) is not gated by `aref_hunt_log_allowed` (the
  attempt/outcome rate-limit while recording).
- Patch-8 `# zoom probe` logs on every reply (S1) even when `mm` is unchanged.

### S7 (nit) — `aref_hunt_finish_attempt` tags a soft judge with the *previous* failure's taxonomy

**Where:** 15006 `int tx = (aref_hunt_last_tax == HUNT_TX_ENDSTOP) ?
HUNT_TX_ENDSTOP : HUNT_TX_SOFT;`. `last_tax` is set by the previous
`aref_hunt_schedule`, so a soft landing that follows an end-stop failure is
recorded/reordered as `endstop`. Use `HUNT_TX_SOFT` here (or derive the current
attempt's end-stop fact from `aref_mpu_esdir`) — cosmetic/portfolio choice.

### Nits

- **N1:** the plan's "completion watchdog stops waiting after 30 s" is mostly
  inert: `aref_hunt_step` (and therefore the watchdog) is only reached when
  `aref_phase == AREF_MON`; during an in-flight attempt the phase branch returns
  first. Liveness is actually provided by the phase handlers, the 1.5 s
  worker-pickup watchdog and the engine caps. No action; wording only.
- **N2:** `aref_mpu_travel/dir/esdir` are read from engine globals even when the
  engine run was skipped, so the summary `travel` / reversed-leg direction can
  be stale after a skipped run. Cosmetic.
- **N3:** `# hunt outcome n=` is 0-based for fail/judge but 1-based for timeout;
  attempt lines are 1-based (`n+1`). Cosmetic log indexing.
- **N4:** `HUNT_ATTEMPT_TIMEOUT_MS 30000` vs worst-case engine duration
  (18 s B cap + ~1 s session + up to ~6 s epilogue/restore): an attempt could be
  declared "timeout" a moment before the engine finishes; its result is then
  discarded and the episode waits the full backoff. 45 s or an "engine busy"
  check would be safer. Low probability on this lens.
- **N5:** `HUNT_ATTEMPT_TIMEOUT_MS` doc says "engine caps are 15/18 s" but the
  epilogue is outside `AFD_RUN_CAP_*`, so the worst case is ~25 s — see N4.
- **N6:** `afd_worker_ensure()` is called from `aref_hunt_pick` on every poll
  while the worker is missing; if `task_create` fails it is retried at poll rate
  (pre-existing helper, rare). No action.
- **N7:** patch 7/A RT retry moves are relative (`afrt_prep`, 10257–10260), so a
  no-trigger retry doubles the defocus *from the already-defocused position*:
  attempt 2's effective defocus is ≈ 3×`def0` while the log and the
  `AFRT_DEF_CAP` check say 2×`def0`. Test-stimulus only; return to the reference
  before a retry (or log the actual commanded travel) if the numbers matter.
- **N8:** the plan's "consecutive attempt starts never more than 45 s apart" is
  not achievable with a 36 s backoff plus an attempt that can take up to ~30 s
  (watchdog) / ~25 s (engine+epilogue): the worst-case gap is ≈68 s. The field
  checklist must allow up to ~70 s, or the acceptance will flag a legitimate
  cap as a failure.
- **N9:** `aref_hunt_begin`'s `# hunt begin ... ref=` logs the decaying peak, not
  the frozen `base`; `base=` is logged next to it, so this is only a reading
  trap. Fine.

---

## 8. Logging formats and parsers

- Every new line was checked for ML snprintf rules: only `%d`, `%x` and plain
  `%s` (strategy/tx names and `lens_info.name`), no `%c`, no `%%`, no field
  widths. Argument counts verified per line, including the two long ones:
  `# hunt reset` (1 `%s` + 8 numeric, 9 args), `# hunt summary` (8 numeric +
  1 `%s`, 9 args), attempt (8), begin (8), fail/outcome (8/9), timeout (7),
  pause/skip/besteffort, and all `# zoom` lines.
- `@REFOCUS` kind 17: `w0 = 17 | strategy<<8 | (n&0xFF)<<16 | anchor<<24`,
  `w1 = cur`, `w2 = med`, `w3 = base`. `len=16`; no 16-bit sharpness packing.
  Host consumers: `contrib/af_test/field_session.py` only interprets kinds 1,
  10, 13 and ignores unknown kinds → tolerant; `af_report.py`/`corpus.py` ignore
  AFDIAG; new `# hunt`/`# zoom` lines start with `#` and are ignored by
  `af_replay.parse_move_log` and the corpus prompts. Verified with a synthetic
  post-patch `AFDRIVE.LOG`/`AFRTEST.LOG` (hunt/zoom/`RT_RETRY`/new `RT_TRIAL`
  fields): `corpus.parse_drive_facts`, `parse_rtest_facts` and `af_report.py`
  all run clean; `# RTEST ... retries=` still matches the prefix regex.
- Host suite: 84/84 unit tests, 6/6 corpus, PASS (re-run by me).

---

## 9. What I could not verify

1. **No camera:** the blocking callback impact (S1), the MPU's answer behaviour
   while the monitor is open (the whole patch-8 zoom premise), the Canon x10
   blink/recording interaction, actual attempt durations/backoff timing, and
   whether the +10 % judge (S5) ends the 2-minute unfocusable test are all
   field-measurable only.
2. **No rebuild:** I confirmed the in-tree `crop_rec.mo` md5 matches the plan's
   build (`3d5f18e3…`) and re-ran the host suite, but cannot prove the .mo was
   compiled from this exact diff.
3. **A/B/C/D/E/F camera behaviour**: patch 9 does not touch their code and the
   engine override is HUNT-scoped; bench A/B is still recommended.
4. **The AFDIAG kind-17 rendering** on the camera (AFDIAG text parser) was not
   exercised; field_session tolerance was checked statically.

---

## 10. Field-tester checklist delta (Persistent / Normal)

The review-1/2/3 checklists still apply. For patch 9:

1. **Settings for the persistent test:** Auto refocus ON, Refocus method MPU,
   **AF drive monitor = Keep open** (S3: not "ON"), MPU drive while recording
   OFF for the main test, Movie Servo OFF, x5 movie LV, tripod, **Refocus
   persistence = Persistent** (default; verify the menu shows it).
2. **2-minute deliberately unfocusable scene** (blank/low-detail wall or fabric
   inside MFD):
   - `# hunt begin why=cold|gate|blur` within ~1–10 s of eligibility;
   - `attempts >= 6` in 120 s, backoff grows 4→6→10→15→20→30 s jittered,
     `max_backoff_ms <= 36000`; **gaps between attempt starts up to ~70 s are
     legitimate** (N8), not a failure;
   - no `# hunt reset` while the scene is unchanged *except* a `why=sharp` on a
     visibly soft picture (S5 — report it) or a genuine `why=scene` from an
     exposure/luma step (legitimate end; attempts resume);
   - `why=` from the set `nopeak|lowtexture|deep|cold|gate|endstop|dark`;
     attempts continue after the 6th failure (`# hunt besteffort`).
3. **Then aim at a well-lit textured subject without changing settings:** focus
   found and episode ends within ≤~45 s (expected <5 s) with
   `# hunt reset why=sharp`; no further `# hunt attempt` while it stays sharp.
   Watch for the S2 signature `reset why=sharp` immediately followed by a new
   `begin` on the same scene (report).
4. **Normal-mode control:** repeat (2) with **Refocus persistence = Normal** for
   2 minutes; expect the patches-1–8 behaviour (few attempts then pause/gate 6
   dead time) — this is the A/B proof that Persistent is doing the work, and the
   soft rollback if the hunt is annoying.
5. **Refocus test (Persistent):** `notrig=0 valid=5 false_pass=0`, `ok>=4/5`,
   `false_trig=0`, `ref_s` not staircasing (the hunt is reset before/after the
   test; test numbers are unchanged from patch 7).
6. **AF bench B speeds:** unchanged vs the pre-patch build (hunt never runs in
   diag; Normal contract).
7. **Walk A↔B:** every blur episode ends `sharp`/user/scene; on normal subjects
   <=3 attempts per episode; no `# hunt begin` immediately followed by
   `reset why=sharp` with no visible blur (report either way).
8. **Recording spot test — do not run until S1 is fixed, or accept log loss:**
   ON+REC, MPU drive while recording ON, monitor Keep open already open. During
   recording no monitor open/close, no `task_create`, no card write; `# hunt`
   text appears in AFDRIVE after the take. If AFDRIVE's hunt/reset lines are
   missing after a >~40 s take, suspect the patch-8 `# zoom probe` note flood
   filling the 2 KB buffer (S1), not the hunt.
9. **Reading the logs:** every attempt line carries `n k strategy pre ref
   anchor backoff_ms`; every outcome `pct land med sharp why`; the reset carries
   `attempts ok strategies max_backoff_ms travel duration_ms sharp`; the
   summary `end=... rec=...`. `max_backoff_ms <= 36000` proves the cap;
   `attempts` growing past 6/10 proves never-give-up. `# hunt pause gate=...`
   and `reason=abort|request_dropped|worker` are legitimate pauses, not stops.
   `af_report.py`/`field_session.py` do not summarise `# hunt` yet — read
   AFDRIVE directly.
10. **Safety watch:** battery/lens temperature on a long hopeless scene (≈80
    attempts/h at the cap), `# hunt reset` storms (S2), LV blackout if the
    monitor setting is ON instead of Keep open (S3), and any recording cadence
    dip during the ON+REC test (S1).

---

## 11. Confidence

**High** that Normal mode is a no-op and that the state machine cannot idle
without a reset/success, and that the judge is frozen-base/non-self-referential:
these were proved from the assignment sites and the call graph. **Medium-high**
that no new crash/hang/recording-safety regression exists: the code is
integer-only, bounded, adds no tasks/monitor ops/card writes, host suite and
parsers are clean, and the reviewed engine/aref paths are otherwise unchanged;
the residual uncertainties are exactly the camera-only questions above (S1/S3/S5
and the Canon x10 legs). I found **no blocker**: flash for the main field
session; fix S1 (one line) before the ON+REC test, or accept that take's hunt log
may be lost.
