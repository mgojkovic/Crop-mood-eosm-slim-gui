# AF hunt forever — persistent Auto-refocus design (patch 9)

**Status:** design only. **No code was changed, no build, no camera, no commit.**
**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` (local) · **Implementation target:** `modules/crop_rec/crop_rec.c`
**Requirement (user):** *"I don't want autofocus to ever stop hunting if blurry. No giving up ever.
Think of a foolproof retry strategy that'll work like a broken watch at least (right twice a day)."*
**Code basis:** working-tree snapshot of `modules/crop_rec/crop_rec.c`, **16 599 lines**,
md5 `746df3ebe5f68467a1aeebb4f6b03a54` (taken 2026-10-19 ~20:35, includes uncommitted patches 1–8).
The tree was still being edited while this design was written; **all line refs are snapshot refs and
may drift** — verify by the symbol names given next to each ref. Design against function names, not
line numbers.

Companion reading: [EOSM-AF-HANDOVER.md](EOSM-AF-HANDOVER.md), [EOSM-AF-WAY-FORWARD.md](EOSM-AF-WAY-FORWARD.md),
[AF-PATCH-REVIEW-3.md](AF-PATCH-REVIEW-3.md), [AF-PATCH2-FIELD-PLAN.md](AF-PATCH2-FIELD-PLAN.md),
[AF-FIELD-TEST-20261019.md](AF-FIELD-TEST-20261019.md), [AF-FIELD-TEST-RETEST-PATCH6.md](AF-FIELD-TEST-RETEST-PATCH6.md).

---

## 0. What "never give up" means (and does not mean)

No contrast-AF system can guarantee focus on a texture-less, dark, or genuinely soft scene: the
information is not in the image. "Never gives up" therefore has one testable meaning:

> **While Auto refocus is ON, the setting is Persistent, the camera is in an eligible state, and the
> picture is not verifiably sharp, the controller always schedules another bounded attempt — forever,
> with no permanent pause, no attempt-count cap, and no state from which it cannot return.**

That is the broken-watch property: periodic, policy-diverse attempts sample the reachable lens
positions over time, so a scene that *can* be focused will eventually be focused (by a later attempt
or after the user moves the camera), and a scene that cannot be focused keeps being retried at a
bounded, cheap rate instead of being abandoned.

Normal mode (the current build) is preserved byte-for-byte in behaviour: Persistent is an *additional*
scheduler layered on top of the existing gates; it does not replace the search algorithms, thresholds,
landing verification, or the trigger logic.

---

## 1. Inventory: every current path that can make the camera "stop trying"

Gate definitions are in `aref_eval` (`snapshot:14159`); the gate code is logged in `aref_dec.gate`
and in the AFDIAG `@REFOCUS` decision record (`afdiag_record(AFD_EV_REFOCUS, 16, 10 | ...)`,
`snapshot:14400`).

| # | Mechanism | Snapshot ref (symbol) | Current behaviour | Can look like "gave up"? | Persistent-mode disposition |
|---|---|---|---|---|---|
| G1 | **Failure pause**, gate 6 | `AREF_MAX_FAILS 3` `snapshot:13578`, `AREF_PAUSE_MS 20000` `snapshot:13579`; set in `aref_mpu_finish` `14443`, post-check `14287`, Canon failure `14622`; resume `14303-14327` | after 3 failures: 20 s of no triggers; resume with `aref_fails = AREF_MAX_FAILS-1`, so one more failure re-pauses → sustained blur is retried roughly once per ~21 s, not abandoned | yes: user sees long stretches with no attempt | **replace with bounded backoff** (4→30 s). The pause variables keep working for Normal mode; Persistent ignores gate 6 and runs its own timer. |
| G2 | **Failed-scene memory**, gate 11 | `AREF_FAILED_MS 20000` `snapshot:13726`; set `14475`; gate `14383-14393`; cleared by luma/g change or timeout `14208-14220` | after a `nopeak`/`lowtexture` run, no new run on a visually identical scene for up to 20 s | yes (immediate retry suppressed) | keep the memory as a **strategy hint** (same taxonomy), but retry on the Persistent backoff timer. |
| G3 | **Absolute-path cap**, gate 4 | `AREF_ABS_MAX 3` `snapshot:13610`; `aref_abs_used` `14340`; re-arm `14489-14493` and `14254-14259` | path 2 (absolute blur) is available only up to 3 times per blur episode; re-armed only by a sharp frame, scene change, pause resume or a new episode | yes: after 3 failed absolute attempts a fresh blurry scene with no peak can stay silent until something else changes | Persistent re-arms the absolute path on **every** hunt attempt when its taxonomy needs it; the `AREF_ABS_MAX` counter stays for Normal mode. |
| G4 | **No usable peak**, gate 4 | `aref_eval` `14332-14338` | with no peak and no absolute-blur signal (e.g. `Auto_refocus_sens = Low`, whose `aref_abs_q[0] = 0` = absolute path off `snapshot:13606`) the trigger can stay silent indefinitely on a fresh/dark scene | yes: strongest "never tried at all" case | Persistent **cold-start probe**: if no usable reference is learned within `HUNT_COLD_START_MS` (8 s) of becoming eligible, start a hunt episode anyway (cheap strategies first). |
| G5 | **Hold / penalty**, gate 7 | `AREF_MPU_PAUSE_MS 10000` `snapshot:13692`; set `14061-14064`; gate `14361`; penalty `pct = MAX(20, pct - pen)` `14250` | after two identical landings: a 10 s gate 7 hold and a 10–25-point lower trigger threshold; the penalty decays 45 s after the hold | yes (10 s dead time; lower threshold makes normal triggers rarer) | Persistent ignores gate 7 for hunt scheduling (its own backoff still runs); the hold/penalty still throttle the *normal* path. |
| G6 | **Cooldowns**, gate 3 and 2 | `AREF_COOLDOWN_MS 2000`, `AREF_FAIL_BACKOFF_MS 4000`, `AREF_MPU_MIN_GAP_MS 4000`, `AREF_REC_RETRY_MS 1500` `snapshot:13576-13581`, `13690` | 1.5–4 s between attempts; dwell gate 2 | no (short, healthy) | keep, and make the Persistent backoff never *shorter* than these after a failure. |
| G7 | **Eligibility gates** | `aref_blk_mask` `snapshot:14138-14150` | bit 1 not movie/x5/preset; 2 AF busy/worker; 4 MF/Movie Servo; 8 menu/overlay; 16 LV transition/RAW start; 32 half-press; 64 not x5 (or not ON+REC while recording) | only if the user forgets a state; these are **legitimate user/camera intent** | always honoured; the hunt episode is paused, not reset (resumes on return). |
| G8 | **Worker watchdog** | `snapshot:14557-14577` | if the worker does not pick the request in 1.5 s: drop it, `afd_wk_ok = 0`, 4 s cooldown, later triggers fall back to Canon AF | yes if the worker stays dead | Persistent calls `afd_worker_ensure()` before every MPU strategy and logs `worker`; if it cannot be created (recording), it uses Canon strategies and keeps retrying. |
| G9 | **Run gives up internally** | `afd_nopeak`, `afd_lowcontrast`, `AFD_RUN_CAP_MS/B_MS` `snapshot:7711-7712`; return `-1` `afd_engine` | an attempt that finds nothing is a failure, and the next attempt waits for the gates above | indirect | becomes taxonomy input + next strategy; per-attempt caps are unchanged (travel bounded by `AFD_MAX_TOTAL 9000`, `AFD_SW_MAX_CHUNKS 12`). |
| G10 | **Recording restrictions** | `aref_blk_mask` bit 64; `aref_step` MPU/Canon split `snapshot:14693-14756` | during recording, Auto refocus runs only with `Auto_refocus = ON + REC`; MPU only with `MPU drive while recording` ON, monitor already open and worker alive; otherwise Canon in-place | no (deliberate safety) | persistent but **throttled**: in-place Canon or pre-opened MPU only, ≥10 s between attempts, never open/close the monitor, never `task_create`, never write the card (logs stay in RAM). Never permanently stops. |
| G11 | **Canon-method / monitor-OFF** | `aref_step` `14688-14756` | with `Refocus method = Canon` or monitor OFF, MPU strategies are impossible | no (configuration) | Persistent rotates only the Canon strategies, logs `why=canon_only`, keeps retrying. |
| G12 | **Auto refocus OFF** | `Auto_refocus` config `snapshot:127` | nothing runs | no (user intent) | no hunt. |
| G13 | **Refocus test sandbox** | `aref_test_task` `snapshot:15082` | the RT test holds `afdiag_afdrive_busy` and resets state before/after | no | hunt is reset around the test (like the anchor/ref-max clears) and never runs inside it. |

**Conclusion:** the current design does not literally have an "OFF forever" switch (except G4/G3/G12),
but it has several throttles that a user perceives as giving up (G1/G2/G3/G5/G8), and one true silent
state (G4). Patch 9 removes all of them **in Persistent mode only**, replacing them with one bounded
scheduler.

---

## 2. New setting: Persistent / Normal (default Persistent)

Add a module setting next to the existing Auto-refocus ones:

| item | value |
|---|---|
| config | `CONFIG_INT("crop.auto_refocus_persist", Auto_refocus_persist, 1);` (default **1 = Persistent**) |
| menu | Settings → INFO/Up-Down/Shutter zoom menu (`slim_info_button_menu[]`, entry after **MPU drive while recording**, `snapshot:5902/5991`): `.name = "Refocus persistence"`, `.max = 1`, `.choices = CHOICES("Normal", "Persistent")`, `.help` = "Persistent: never stop retrying while the picture is blurry. Normal: current bounded retries." |
| registration | `MODULE_CONFIG(Auto_refocus_persist)` after `MODULE_CONFIG(Auto_refocus_mpu_rec)` (`snapshot:16565`) |
| semantics | only meaningful while `Auto_refocus` is ON/ON+REC. `Normal` = exactly today's behaviour (all patches 1–8). Switching to Normal mid-episode immediately resets the hunt and lets the existing pause/gates take over. |

Default is Persistent because the user asked for it; the help text must say that Persistent can keep
moving the lens on a scene that cannot be focused and that Normal is the battery/lens-wear-friendly
choice. (No new menu screens; the existing settings page is reused.)

---

## 3. The state machine

### 3.1 States

```
                 (Auto refocus ON, Persistent, eligible, not sharp)
   ┌──────────┐  blur evidence / cold start / provisional ref     ┌───────────┐
   │ HUNT_IDLE│ ────────────────────────────────────────────────► │ HUNT_ARM  │
   └──────────┘                                                   │ (episode  │
        ▲                                                         │  active,  │
        │                                                         │  backoff  │
        │                                                         │  timer)   │
        │                                                         └─────┬─────┘
        │                                     next_at due & eligible    │
        │                                                               ▼
        │    success => reset(sharp)                           ┌──────────────┐
        │    ┌──────────────────────────────────────────────── │ HUNT_ATTEMPT │
        │    │                                                 │ (one attempt │
        │    │                                                 │  in flight)  │
        │    │                                                 └──────┬───────┘
        │    │                                                        │ run returns
        │    │                                                 ┌──────▼───────┐
        │    └───────────────────────────────────────────────► │ HUNT_JUDGE   │
        │              (sharp: relative or absolute)           │ (settle)     │
        │                                                      └──────┬───────┘
        │                                                             │ still blurry
        │                                                             │ n++, k++
        │                                                             ▼
        │                                                      (schedule backoff,
        │                                                       back to HUNT_ARM)
        │
   ┌────┴───────┐
   │ HUNT_RESET │  also: scene change, user AF, zoom/mm, toggle/method, RT test, OFF
   └────────────┘
   (abort by user/camera: pause — stay in HUNT_ARM, do not reset, do not count a failure)
```

- `HUNT_IDLE` — no episode. The normal `aref_eval` path is untouched.
- `HUNT_ARM` — episode active, next attempt time `next_at` is set; waits for eligibility and the
  timer. This is the only state from which attempts are launched.
- `HUNT_ATTEMPT` — exactly one attempt in flight (MPU worker or Canon request). The existing phase
  machine (`aref_phase` / `slim_af_state`) still owns the attempt; `aref_hunt_step` never launches a
  second one.
- `HUNT_JUDGE` — the attempt finished; wait `AREF_SETTLE_MS` + one median sample, then judge sharp.
- `HUNT_RESET` is a transition, not a state: logs `# hunt reset`/`# hunt summary`, zeroes the state.

`aref_hunt_step(now)` is called on **every** `aref_step` poll, not only when `aref_eval` returns a
path: the backoff timer, eligibility pause/resume and the judge must keep running while `aref_eval`
returns 0 (gate 6/11/4 or settling). Normal-mode `aref_fails`/pause bookkeeping still runs unchanged
for normal triggers; in Persistent mode it only gates the normal path — the hunt ignores gates 6/7/11
for its own scheduling (see §5). One completion watchdog bounds a stuck attempt:
`HUNT_ATTEMPT_TIMEOUT_MS = 30000` (engine caps are 15/18 s, Canon 4 s; the existing worker watchdog
handles a dead worker). If an attempt has not completed by then, log
`# hunt outcome … why=timeout`, do not touch the engine internals, and schedule the next attempt.

The state lives in three integers + counters (all `static` file-scope, zero-init):
`state`, `n` (attempts in this episode), `k` (strategy cursor), `next_at`, `started_t`,
`last_strategy`, `last_taxonomy`, `pre_s`, `pre_q`, `pre_emax`, `pre_luma`, `sharp`, `besteffort`,
`travel`, `strat_mask`, `rng`, `cold_t`, `prov_t`. About 60 bytes of RAM — negligible.

### 3.2 Episode-entry conditions (any one, in this order)

1. **Blur evidence** — `aref_eval` returned a path (`path == 1/2`), i.e. the normal detector fired but a
   give-up gate (6/7/11) suppressed it, or the attempt is about to run anyway. Enter with the normal
   trigger (strategy 0) immediately; this is not a new behaviour, just bookkeeping around the attempt.
2. **Suppressed blur** — `aref_dec.gate` is 4, 6, 7 or 11 AND there is blur evidence: gate 6/7/11 by
   definition follow a blur/attempt, gate 4 with `q >= aref_abs_q[sens] && emax <= aref_abs_em[sens]`
   (absolute blur) or with a previously valid peak (`aref_dec.peak >= aref_peak_min[sens]`), i.e. the
   peak exists but texture gate blocked the trigger.
3. **Cold start** — `aref_eligible` was just acquired and after `HUNT_COLD_START_MS = 8000` ms there is
   still no usable reference at all (`!aref_ref && !aref_anchor_ok()`). This covers "camera starts on a
   blurry/dark scene and the reference is never learned" (G4).
4. **Provisional reference** — the current trigger reference was learned from a picture that fails the
   absolute sharp test (`q >= AREF_ABS_Q_GOOD || emax < AREF_ABS_EM_GOOD`, `snapshot:13608-13609`).
   This is the "reference was defined from a blurry picture" case: the camera thinks it is in focus
   but is not. Persistent treats such a reference as **provisional**: it arms a slow hunt episode
   (`HUNT_PROV_MS = 30000`) until a settle is absolute-sharp, then the reference becomes trusted.
   Normal mode never does this. (The existing anchor/ref-max seeds `snapshot:13947-14120` remain the
   authority; this flag only decides whether Persistent distrusts them.)

Episode entry is logged once:
`# hunt begin why=blur|gate4|gate6|gate7|gate11|cold|provref gate=%d path=%d ref=%d anchor=%d rec=%d t=%d`.

### 3.3 Backoff (bounded, jittered, integer-only)

```
base_ms[6] = { 4000, 6000, 10000, 15000, 20000, 30000 }   /* HUNT_BACKOFF_MS_N = 6 */
delay = base_ms[min(n-1,5)] * (80 + (rng >> 16) % 41) / 100   /* ±20 %, rng = 32-bit LCG */
delay = COERCE(delay, 3000, 36000)
next_at = now + delay
```

- `n` increments on every **failed** attempt; `n` has no cap (an `int` of milliseconds/attempts
  overflows after hundreds of years of continuous failure).
- The LCG is private (`rng = rng * 1664525 + 1013904223`), seeded per episode from
  `get_ms_clock() ^ (afd_frame_cnt << 16) ^ luma`. No floats, no `rand()` dependency, cannot lock-step
  with a repeating scene/refresh artefact.
- The backoff never gets shorter after a failure; it is reset on episode end only. Two
  user-intent exceptions: after a half-press pause is released, and after a non-failure abort
  (menu/LV), the retry is scheduled after `AREF_SETTLE_MS` instead of the full backoff.
- The real attempt duration is additional, so the effective attempt-start spacing is
  `delay + attempt (+ settle)` ≈ 5–45 s at the cap.

**Never-stop proof (invariant):** every non-final transition out of `HUNT_ATTEMPT`/`HUNT_JUDGE` either
resets the episode (success/user/scene) or sets `next_at = now + delay ≤ 36 s` and returns to
`HUNT_ARM`. `HUNT_ARM` has no exit other than launching an attempt or an episode reset. No counter is
compared with a maximum anywhere in the state machine. Eligibility pauses do not change `next_at`
(the attempt is launched as soon as eligibility returns and the timer is due). Therefore:

> while the camera remains eligible and the picture is not verifiably sharp, an attempt starts at
> least every **~45 s** (36 s backoff + ≤8 s attempt/settle) without end.

The only stops are the user/camera states listed in §5 (menu, MF, Movie Servo, half-press, LV
transition, recording restrictions, Auto refocus OFF) and verifiable sharpness.

### 3.4 Judging an attempt (success vs failure)

`HUNT_JUDGE`, after `AREF_SETTLE_MS` (500 ms, `snapshot:13569`) + one 3-sample median:

```
sharp = 0
ref   = best of (anchor if aref_anchor_ok(), afd_ref_max_s if key matches, aref_ref)
if (ref && med*100 >= ref*HUNT_SHARP_PCT) sharp = 1;              /* HUNT_SHARP_PCT = 92 */
else if (!ref && absolute_sharp(q, emax)) sharp = 1;              /* q < AREF_ABS_Q_GOOD && emax >= AREF_ABS_EM_GOOD */
else if (ref && med*100 >= pre_s*(100 + HUNT_IMPROVE_PCT) && absolute_ok(q)) sharp = 1;
```

- Existing reference-integrity logic (anchor `snapshot:13947-14120`, `afd_ref_max_s` `8727`, the
  confirmed-soft escape, SF2 failed-run anchor clear) stays the **source of truth** for `ref`; the hunt
  does not duplicate or lower it. It only uses it to decide "is this picture good enough to stop?".
- A run that reports `pct >= 0` but settles below 92 % of the reference is a **failure** (the old
  self-referential `pct` cannot end a hunt). This is the core fix for the user complaint: a soft
  landing never ends the episode.
- An attempt aborted by user/camera (menu, MF, Movie Servo, LV transition, `afd_must_abort`) is **not
  a failure**: stay in the episode, wait for eligibility + `AREF_SETTLE_MS`, retry (backoff unchanged).
- The judge must not race the reference adoption: after a `pct >= 0` run `aref_mpu_newref` is pending
  and the first settled sample is consumed by `aref_mpu_settle` (`snapshot:13947`). `HUNT_JUDGE`
  waits for that settle (or the 5 s `aref_mpu_newref` timeout in `aref_eval`, `snapshot:14197-14205`)
  and uses the settled `med`/the adapter's decision — it never launches another attempt while a
  reference settle is pending.
- `aref_dec.emax` does not exist today (`snapshot:13750`); add one `int emax;` field to `aref_dec` and
  set it in `aref_eval` (`snapshot:14183`) — no behavioural change, needed by the judge.
- On success: `HUNT_RESET` with `why=sharp`, reset backoff, clear `besteffort`, log `# hunt summary`.
- On failure: `n++`, `k = (k+1) % HUNT_S_N`, classify taxonomy, set `next_at`, log.

### 3.5 Episode reset/pause conditions (exact hooks)

| Event | Hook (snapshot) | Action |
|---|---|---|
| verifiably sharp frame | `aref_step` after `aref_eval` (`14510`), judge uses `aref_dec` | `hunt_reset("sharp")` |
| scene change (>25 % luma jump) | `aref_sw_push` scene branch (`14075-14085`) | `hunt_reset("scene")` |
| user AF / Shutter zoom AF | `aref_step` `AREF_FOREIGN` branch (`14598-14607`) | `hunt_reset("user_af")` |
| zoom / focal-length bucket change | `aref_step` eligibility branch (`14670-14686`), patch-8 refresh event | `hunt_reset("zoom")` |
| Auto refocus toggled, method changed, RAW rec start/stop | `aref_step` (`14519-14552`) | `hunt_reset("toggle"/"method"/"rawrec")` |
| Refocus test start/end | `aref_test_task` (`15082-15110`) next to the patch-5 MF3 clears | `hunt_reset("rtest")` |
| menu, MF, Movie Servo, half-press, LV transition, AF busy | `aref_blk_mask != 0` (`14138`) | **pause**: keep state/next_at, no reset; log rate-limited `# hunt pause gate=%x` |
| not x5 / not ON+REC | `aref_blk_mask` bit 64 | pause (no attempt is possible) |
| recording (ON+REC) | see §4 | throttled attempts; never open/close monitor, never `task_create`, never card-write; logs buffered |
| Persistent OFF / Auto refocus OFF | `aref_step` entry | `hunt_reset("off")` |

Half-press deserves a note: gate 32 pauses the hunt (user is focusing). On release, if the picture is
still not sharp, the episode resumes with `next_at = now + AREF_SETTLE_MS` (immediate retry) rather
than the full backoff — the user has just shown intent.

---

## 4. Strategy portfolio (rotating, bounded, safety-aware)

`HUNT_S_N = 5` strategies; `k` advances by 1 on every failure and wraps. Strategies that are not
currently available are skipped (the cursor still advances; the skip is logged once).

| k | strategy | implementation (existing primitive) | cost ≈ | when it is for | skipped when |
|---|---|---|---|---|---|
| 0 | **Local** | engine mode 0, `AFD_F_AREF \| AFD_F_HUNT`, algorithm `AFD_ALGO_LOCAL` (`snapshot:7528`, dispatch `9599`/`10467`) | 2–3 s, 5–10 moves | first attempt / small defocus / configured Refocus AF | monitor OFF; method Canon; recording without pre-opened monitor+worker |
| 1 | **Full B, configured speed (07)** | engine mode 0, force `afd_algo_cur = 1` (B), `afd_sw_speed = AFD_SPEED` (`snapshot:7450/7603/10464`), sweep `afd_run_sweep` (`9206`), deadline `AFD_RUN_CAP_B_MS` 18 s | 4–8 s, up to 12 chunks + landing | no usable local peak, deep defocus, soft landing | monitor OFF; method Canon; recording (unless MPU rec enabled + monitor open) |
| 2 | **Canon x10 detour** | `slim_af_request = 1` (`snapshot:447`, `slim_af_step` `500`): zoom to x10, Canon AF, return to x5; only non-recording | 1–4 s + x10 switch | texture exists but MPU cannot find a peak; Canon's own AF works on a different evaluation; the x10 image is sharper to evaluate | recording; menu/MF/servo; `slim_af_can_start()` false |
| 3 | **Canon in-place (x5)** | `slim_af_request = 2` (`snapshot:482-500`): Canon AF without the x10 switch, also allowed while recording | 0.5–4 s | recording-safe fallback; dark scene; after an x10 detour failure | `slim_af_can_start_inplace()` false |
| 4 | **Full B, alternate** | force B with `afd_sw_speed = 0x1F` (or 0x14) **and** a forced direction opposite to the previous attempt (`afd_forced_dir`, see §9.3); one shot through the whole range, bounded by `AFD_MAX_TOTAL` | 3–7 s | deep defocus, end stop, wrong direction, "broken watch" diversity | monitor OFF; method Canon; recording |

Rotation order is exactly `0 → 1 → 2 → 3 → 4 → 0 …` (the user's suggested order:
local → full B → Canon AF (x10/x5) → full B at the other speed/reversed → repeat).

Additional selection rules (applied on top of the rotation):

- **Monitor OFF / Canon method:** only k=2/3 are launched; the state machine logs `why=canon_only` once
  per episode and keeps cycling at the same backoff (never stops).
- **Recording:** only k=3 (in-place Canon) and, if `Auto_refocus_mpu_rec` is ON and the monitor is
  already open and the worker exists, k=0/1/4 with `AFD_F_REC`; no x10 (k=2 skipped). The attempt
  spacing while recording is at least `HUNT_REC_MIN_GAP_MS = 10000` and the per-episode strategy
  rotation keeps full B rare (at most 1 in 3).
- **Low texture** (taxonomy `lowtexture`, gate 10, `afd_lowcontrast`): MPU full B is unlikely to help;
  order becomes k=2 → k=3 → k=0 → k=1, and k=1 is attempted only once per two cycles.
- **End stop** (taxonomy `endstop`, `afd_endstop_dir` `snapshot:7633`): force the opposite direction
  (k=4 with `afd_forced_dir = -last_dir`), then k=3; never two same-direction full B in a row.
- **Dark** (`luma < HUNT_DARK_LUMA`, `HUNT_DARK_LUMA = 40`, heuristic/log-only): prefer k=2/k=3
  (Canon handles exposure/its own AF differently), MPU attempts at the 30 s cap only.
- **Best-effort guard:** after `HUNT_BESTEFFORT_N = 6` consecutive failed attempts with no measured
  improvement over `pre_s` (and the anchor has already been through the confirmed-soft path), set
  `besteffort = 1`: only k=2/k=3 are attempted, at the 30 s cap. **Attempts continue forever**; this
  only protects the lens motor and battery from full-range sweeps on a hopeless scene. Log
  `# hunt besteffort attempts=%d t=%d`.

Per-attempt bounds are the existing ones and must not be raised by patch 9:
`AFD_MAX_TOTAL 9000` pulses (`snapshot:7504`), `AFD_RUN_CAP_MS/B_MS` 15/18 s (`7711-7712`),
`AFD_SW_MAX_CHUNKS 12` (`8599`), Canon `SLIM_AF_MAX_MS 4000` (`snapshot:433`). The hunt also
accumulates `hunt.travel += afd_travel` per MPU attempt and reports it in `# hunt summary`.

---

## 5. Failure taxonomy and decision table

Taxonomy is computed once at `HUNT_JUDGE` from the last attempt + the settled sample
(`aref_mpu_*`, `afd_nopeak_last`, `afd_lowcontrast`, `afd_es_*`, `aref_dec`):

| taxonomy string | detector | likely cause | next strategies (in order) | note |
|---|---|---|---|---|
| `nopeak` | `afd_nopeak_last` (engine says no real peak) | deep defocus, wrong region, flat flank, end stop | 1 (full B 07), 4 (B alt/reverse), 0 (local) | `afd_nopeak` is returned as pct=-1; must not end the episode |
| `lowtexture` | `afd_lowcontrast` or gate 10 (`aref_ref_g < AREF_MIN_G`, `snapshot:14382`) | wall/sky/dark, texture below MPU sensitivity | 2 (Canon x10), 3 (Canon in-place), 0 | Canon's own AF may succeed where the MPU metric cannot; MPU full B rare |
| `soft_anchor` | run `pct >= 0` but settled `med` < `92 %` of anchor/ref max | soft landing, or anchor stale-high | 1, 4, 2, 3 | the anchor's own `confirmed soft scene` escape (`14015-14060`) still runs; if it fires and B still cannot improve, `besteffort` |
| `deep` | `nopeak` + available gradient `g >= AREF_MIN_G` + full sweep covered the range | subject far from start; sweep missed the peak / wrong direction | 4 (alt speed + reverse), 1, 2 | B alt is the key "other speed/reversed" leg |
| `endstop` | `afd_endstop_dir != 0` for the last run (`snapshot:7633/7676-7681`) | lens hit a hard stop during the attempt | 4 with forced opposite direction, 3, 2 | avoids repeated end-stop slams (wear) |
| `dark` | `luma < HUNT_DARK_LUMA` and/or `emax` low | underexposed scene | 2, 3, 0 | log-only heuristic; Canon handles exposure differently |
| `canon_fail` | `slim_af_last_result != 0` / result-count unchanged (`snapshot:14595-14646`) | Canon AF itself failed | 0, 1, 4 | next attempt is MPU; if recording, only 0/1/4 when allowed |
| `run_abort` | `afd_aborted`/`afd_timeout` without nopeak | menu/MF/servo/LV/recording change, watchdog, deadline | **not a failure** — pause, retry after settle | do not advance `n`/`k` |
| `moving` | gate 5/9 (`snapshot:14376/14393`) | pan / unstable scene | no attempt; wait for settle | not a failure; do not consume the backoff |
| `unknown` | no evidence | cold start / dark | 0 → 1 → 2 → 3 → 4 | maps to `cold`/`provref` entry reasons |

The taxonomy string goes into the `# hunt attempt/outcome` log and into `# hunt summary`
(`why=`). It is also the input for skipping strategies (§4).

---

## 6. Logging and the `# hunt` summary

All lines use ML `snprintf` rules only: `%d`, `%x`, plain `%s`; no floats, no `%c`, no `%%`, no field
widths. Text lines go through `afd_note` (RAM, flushed to AFDRIVE.LOG by `afd_note_flush`,
`snapshot:9878`) so they are safe while recording; per-attempt compact records go through
`afdiag_record(AFD_EV_REFOCUS, 16, 17, ...)`.

### 6.1 Lines (one per event, rate-limited)

```
# hunt begin why=%s gate=%d path=%d ref=%d anchor=%d rec=%d t=%d
# hunt attempt n=%d k=%d strategy=%s pre=%d ref=%d anchor=%d backoff_ms=%d t=%d
# hunt outcome n=%d k=%d strategy=%s pct=%d land=%d med=%d sharp=%d improved_pct=%d why=%s t=%d
# hunt skip n=%d k=%d strategy=%s reason=%s t=%d
# hunt pause gate=%x t=%d
# hunt besteffort attempts=%d t=%d
# hunt reset why=%s attempts=%d ok=%d strategies=%x max_backoff_ms=%d travel=%d duration_ms=%d sharp=%d t=%d
# hunt summary attempts=%d ok=%d fails=%d strategies=%x max_backoff_ms=%d duration_ms=%d travel=%d end=%s rec=%d
```

- `# hunt begin` once per episode; `# hunt attempt` + `# hunt outcome` per attempt **while
  recording-off and at most once per strategy change while recording** (the note buffer is 2048 B,
  `snapshot:9859`; do not overflow it).
- `# hunt reset` + `# hunt summary` at episode end (success, user, scene, zoom, toggle, RT, OFF). The
  summary is the field's one-line verdict for the episode; `af_report.py` can be extended to print it
  (separate, small follow-up).
- `@REFOCUS` kind 17 compact payload (sharpness values are up to ~200k, so they cannot be packed
  into 16 bits): `w0 = 17 | (k << 8) | ((n & 0xFF) << 16) | (anchor_present << 24) | (sharp << 25)`,
  `w1 = cur`, `w2 = med`, `w3 = ref`. This keeps a per-attempt record in AFDIAG even when the text
  line is suppressed during recording; the backoff is in the text summary.
- Example of a healthy episode on a blurry subject:

```
# hunt begin why=blur gate=6 path=1 ref=115000 anchor=115000 rec=0 t=812340
# hunt attempt n=1 k=0 strategy=local pre=48000 ref=115000 anchor=115000 backoff_ms=0 t=812540
# hunt outcome n=1 k=0 strategy=local pct=98 land=82000 med=82000 sharp=0 improved_pct=0 why=soft_anchor t=815200
# hunt attempt n=2 k=1 strategy=full_b07 ... backoff_ms=4200 t=819400
# hunt outcome n=2 k=1 strategy=full_b07 pct=100 land=112000 med=113000 sharp=1 improved_pct=135 why=ok t=826100
# hunt reset why=sharp attempts=2 ok=1 strategies=3 max_backoff_ms=4200 duration_ms=13760 sharp=1 t=826300
# hunt summary attempts=2 ok=1 fails=1 strategies=3 max_backoff_ms=4200 duration_ms=13760 travel=2100 end=sharp rec=0
```

### 6.2 How to read the verdict

- `attempts` should keep growing while the scene is blurry; a `# hunt summary` with `end=user|scene|zoom`
  is a legit end, `end=sharp` is success.
- `max_backoff_ms` ≤ 36000 proves the backoff cap; `n` continuing past 10 proves "never gave up".
- On an unfocusable scene the normal expected pattern is `begin`, then attempts at ≈4/6/10/15/20/30/30 s
  (`why=nopeak|lowtexture|deep`), with occasional `besteffort`, and **no** `reset` until the scene
  changes — that is the requirement being met, not a bug.
- `# hunt pause gate=...` lines are expected for menu/MF/half-press; they must stop as soon as the gate
  clears.

---

## 7. Safety analysis

### 7.1 Battery

Worst case is a permanently unfocusable, eligible scene with Persistent ON. At the 30 s cap, one
attempt costs (from the current engine): local ≈2–3 s, full B07 ≈4–8 s, Canon x10 ≈1–4 s, in-place
≈0.5–4 s, plus the engine epilogue ≈1.5–2 s for MPU attempts. A full rotation averages ≈3–6 s of
lens activity per 5 attempts, at 30–45 s spacing → **motor duty ≈7–15 %**, plus LV sampling that the
camera already does. This is far below Movie Servo continuous AF (which is the camera's normal
movie-mode behaviour), but it is not free: expect several %/hour extra battery on a hopeless scene.
Mitigations: the best-effort guard restricts to cheap Canon strategies; the backoff cap; the menu
setting (Normal for long unattended takes); attempts pause while the camera is in menu/MF/servo/LV
transition; recording uses in-place Canon only.

### 7.2 Lens wear

- Per attempt travel is bounded by `AFD_MAX_TOTAL` 9000 pulses and per-run caps; the hunt never raises
  them.
- Full-range B sweeps are the main wear/heat item: the rotation puts local/Canon between B attempts,
  and the end-stop taxonomy forces a direction reversal so the lens does not slam the same hard stop
  repeatedly; best-effort removes B entirely after a hopeless run.
- The EF-M STM motors are designed for continuous Movie-Servo AF; the worst persistent case (~one full
  B per ~2–3 min at the cap, plus cheaper attempts) is within the same order of magnitude as normal
  video AF usage, not a continuous grind.
- The state machine never issues a command outside `afd_move_ex`/`slim_af_request`, so all existing
  end-stop, deadline and abort protections apply unchanged.

### 7.3 LV cadence / UI

- MPU attempts use the existing monitor session (Keep open); the first one may still cost the known
  ~2 s monitor-open blackout (once). Subsequent attempts cause the same per-run LV traffic as a normal
  Auto refocus; the new fields `search_ms`/`epilogue_ms` (patch 2) quantify it.
- Canon x10 detours visibly switch magnification for ~1–3 s. Mitigation: k=2 is skipped while
  recording, and it is one leg of five. If field testing shows it is too visible, the portfolio order
  can be changed without touching the state machine (the cursor order is a table).
- Persistent does not touch overlays, menus or the ML config; the only UI addition is the settings
  entry.

### 7.4 Recording safety

Rules are enforced by construction in §4:

- never `task_create` while recording (patch 2 wrapper already guards all menu handlers; the hunt only
  uses the pre-created worker or Canon in-place);
- never open/close the MPU monitor while recording (only use it if already open);
- never write the card while recording: `# hunt` text goes to the RAM note buffer and is flushed after
  recording; AFDRIVE logs are held (`afd_log_pending`/`afd_log_hold`);
- the attempt rate while recording is throttled (`HUNT_REC_MIN_GAP_MS = 10000`), x10 is not used, and
  full B is at most 1 in 3 attempts (only with MPU-rec enabled);
- Canon in-place AF while recording can itself hunt (documented camera behaviour) — that is why the
  setting is `ON + REC` and why `MPU drive while recording` remains the user's choice. If the take is
  more important than the refocus, `Auto refocus = ON` (not ON+REC) disables the whole path during
  recording.

### 7.5 CPU, tasks, RAM

- No new tasks, no new threads; the state machine runs inside `aref_step` (one call per polling
  callback) with a few integer comparisons and one timer check. The judge runs once per attempt.
- No new allocations; ~60 bytes of statics; the `# hunt` text is bounded per attempt.
- All engine loops keep their existing `afd_must_abort()`/`afd_deadline` checks; the hunt only chooses
  *which* existing entry point runs.

### 7.6 Not fighting the user

Gate 32 (half-press), gate 4 (MF/Movie Servo), gate 8 (menu/overlay), AF-busy, LV transitions and user
AF (`AREF_FOREIGN`) all pause or reset the hunt. A user half-press is never overridden; the episode
resumes after release with a short settle, not the full backoff.

---

## 8. Interaction with patches 1–8

- **Patch 1/3 (local search, wide probe, `afd_ref_max_s`, anchor):** the hunt uses these as the
  success reference and leaves their updates untouched. The local strategy is still exactly
  `afd_run_local` with its wide probe/drop-path guards; the hunt only adds another retry after its
  failure.
- **Patch 2 (diagnostics/safety):** `search_ms`/`epilogue_ms` in `# RESULT` provide the per-attempt
  cost analysis; the servo abort (`afd_abort_on_servo`, only `AFD_F_AREF` runs) applies to hunt
  attempts too; the menu `task_create` guard and NODONE stop are untouched.
- **Patch 3 (zoom/mm keys, clamps):** hunt resets on zoom/mm changes (same events that clear the
  anchor/ref max) and never bypasses the keys.
- **Patch 4/5 (confirmed-soft recovery, anchor/ref-max state fixes):** the hunt treats the anchor as
  authoritative; the confirmed-soft escape still fires (after 3 soft settles) and the SF2 failed-run
  clear still runs after 2 `nopeak/lowtexture` runs. Those are exactly the events the hunt uses to
  decide `besteffort`.
- **Patch 6 (`AREF_ABS_MAX 3` retry):** remains in Normal mode; Persistent re-arms the absolute path
  per attempt, so the cap is not a give-up path there. The `# aref abs re-arm` log is expected less
  often in Persistent because the hunt drives the attempts.
- **Patch 7 (Refocus-test adaptive defocus):** completely independent; the RT test is reset around the
  hunt and its `aref_eval` calls are not routed through the hunt (the test owns its own sandbox).
- **Patch 8 (monitor/zoom focal-length work):** hunt resets on the patch-8 zoom-refresh/`afd_mm_bucket`
  changes and never reads `lens_info.focal_len` for logic (only logs it).
- **Normal mode is a hard regression contract:** with `Auto_refocus_persist = 0`, `aref_hunt_step`
  returns immediately; all existing behaviour (pause, failed-scene memory, abs cap, hold, cooldowns)
  is unchanged.

---

## 9. Implementation order for patch 9 (exact functions to touch)

Do the phases in this order; each phase is buildable and independently revertible. No commits.

### Phase 0 — snapshot & offline model (no firmware change)

1. Freeze a pre-patch copy (`git diff` snapshot, md5) for A/B; note the line drift.
2. Add a small Python model of the state machine to `contrib/af_test/` (pure functions: backoff,
   rotation, never-stop simulation) and a unit test that runs 1 simulated hour on a "never focuses"
   scene and asserts `max_gap <= 45000` and `attempts > 60`, plus reset cases. This proves the
   invariant before touching the firmware. (No camera.)

### Phase 1 — setting + menu (no behaviour unless switched)

3. `modules/crop_rec/crop_rec.c`: `CONFIG_INT("crop.auto_refocus_persist", Auto_refocus_persist, 1)`
   after line 131 (`Auto_refocus_mpu_rec`).
4. New entry in `slim_info_button_menu[]` after "MPU drive while recording" (`snapshot:5991-6001`).
5. `MODULE_CONFIG(Auto_refocus_persist)` after `snapshot:16565`.
6. Verify in the menu and in `config` persistence; with Persistent ON and the state machine absent,
   nothing changes yet.

### Phase 2 — hunt core (state machine, entry, backoff, judge)

7. Add constants and the state near the aref globals (after `snapshot:13750`, `aref_dec`): extend
   `aref_dec` with `emax`; add `aref_hunt_*` statics and `enum { HUNT_IDLE, HUNT_ARM, HUNT_ATTEMPT,
   HUNT_JUDGE }`.
8. Add helpers before `aref_step` (`snapshot:14510`):
   `hunt_rng()`, `hunt_backoff(now)`, `hunt_best_ref()`, `hunt_abs_sharp(q, emax)`,
   `hunt_sharp(med)`, `hunt_classify()`, `hunt_begin()`, `hunt_reset()`, `hunt_schedule()`,
   `hunt_launch()` (MPU + Canon variants), `hunt_outcome()`, `hunt_step(now)`.
9. Call `hunt_step(now)` from `aref_step` at exactly two places:
   - after the normal `path = aref_eval(...)` handling (`snapshot:14689-14756`), so a normal trigger
     and a Persistent attempt share one launch path;
   - at the completion points: `AREF_MPU` branch after `aref_mpu_finish` (`14572-14576`), and the Canon
     `AREF_OURS`/`AREF_FOREIGN` completion (`14595-14646`), to feed `hunt_outcome`.
10. In Persistent mode, make gates 6/7/11 non-blocking for the hunt timer only (the state machine
    ignores `gate == 6/7/11` when deciding "due"); do **not** remove the gate assignments (Normal mode
    and the Refocus test still use them).
11. Judge sharp with the sharp detector; success → reset; failure → schedule; abort → pause.

### Phase 3 — engine strategy overrides

12. Add `#define AFD_F_HUNT 8` to the flags block (`snapshot:7527-7529`) and
    `static int afd_hunt_algo = -1, afd_hunt_speed = -1, afd_hunt_dir = 0;` near `afd_sw_speed`
    (`7603`).
13. `afd_engine` (`10433`): if `flags & AFD_F_HUNT`, after the normal algorithm/speed selection
    (`10464-10467`) apply `afd_algo_cur = afd_hunt_algo` (0 local / 1 B), `afd_sw_speed = afd_hunt_speed`,
    and pass the direction override; clear the overrides at the end of the run (next to
    `afd_abort_on_servo = 0`, `snapshot:10769`).
14. `afd_run_sweep` (`9206`) / `afd_probe_dir` (`9020`): honour `afd_hunt_dir` after the probe; if the
    forced direction immediately hits an end stop, fall back to the probe direction and log
    `# hunt forced dir refused` (bounded, one retry).
15. `aref_mpu_exec` (`13769`) / `afd_worker_task` (`13802`): `AFD_F_HUNT` is already carried in
    `afd_wk_flags`; publish `afd_es_dir`/`afd_endstop_dir`/`afd_nopeak_last`/`afd_lowcontrast` to the
    hunt before they are reset by the next run.
16. Keep the existing flags/deadline/travel caps; add `hunt.travel += afd_travel` after each MPU
    attempt.

### Phase 4 — Canon strategies

17. In `hunt_launch`, Canon variants use the existing request path (`snapshot:14734-14739`):
    set `aref_trig_ref`, `aref_count0`, `aref_phase = AREF_REQUESTED`, `slim_af_request = 1` (x10) or
    `2` (in-place), with `hunt.active_attempt = 1`.
18. In the completion branches (`14595-14646`) call `hunt_outcome(now, failed)` and skip the normal
    `aref_fails`/pause bookkeeping when the attempt was a hunt attempt (Normal-mode runs keep it).
19. Enforce `slim_af_can_start()`/`slim_af_can_start_inplace()` before launching; if false, skip the
    strategy and try the next one (log `# hunt skip reason=canon_state`).

### Phase 5 — reset hooks and recording throttle

20. Wire the reset hooks of §3.5: `aref_sw_push` scene branch, `aref_step` toggle/method/RAW/foreign/
    zoom branches, `aref_test_task` begin/end, `Auto_refocus_persist` change.
21. Add the recording branch: `HUNT_REC_MIN_GAP_MS 10000`, k=2 skipped, monitor already open required,
    in-place Canon always allowed, note lines suppressed to one per strategy change (note buffer).
22. Add the provisional-reference flag: when `aref_mpu_settle` seeds/adopts a reference whose sample
    fails the absolute sharp test, set `hunt.provref = 1` (or a dedicated `aref_ref_prov` next to the
    anchor, `snapshot:13631-13641`); clear on an absolute-sharp settle. This must not change the
    adopted value, only the trust flag.
23. Add the cold-start timer: store `hunt.cold_t` when eligibility is acquired, and enter the episode
    from §3.2(3).

### Phase 6 — logging, summary, parsers

24. Add the `# hunt` lines and the `@REFOCUS` kind-17 compact record (§6); rate-limit during
    recording; update `contrib/af_report/af_report.py` to print `# hunt summary` (small, optional,
    separate from the firmware patch).
25. Add a dropped-note counter in the summary path if the note buffer fills (`afd_note` has no return
    value today, `snapshot:9862`); at minimum log the episode summary even when attempts are skipped.

### Phase 7 — build, host tests, field plan

26. Build with the documented command
    `export PATH=$HOME/gcc-arm-none-eabi-5_4-2016q3/bin:$PATH; make -C platform/EOSM.202 zip SHELL=/bin/bash PYTHON=python3`,
    twice, determinism check, `git checkout -- modules/crop_rec/README.rst` if the build overwrites it,
    keep the `module_strings.h` placeholder.
27. Host suite: `python3 contrib/af_test/run_all.py` must stay green (the existing 81/81 + 6/6 corpus,
    review 3).
28. Field session per §10.

**Do not touch in patch 9:** `afd_run_a/b/c/d/e/f`, `afd_run_local` internals, the aref trigger
thresholds/tables (`aref_drop_pct*`, `aref_abs_*`, `AREF_MPU_PEN_*` values), `aref_mpu_settle`'s
adoption policy, `afd_engine`'s epilogue/single-exit structure, `aref_blk_mask` gates, the
`task_create` wrapper, `afd_must_abort`.

---

## 10. Test plan

### 10.1 Host (offline, before flashing)

- Unit tests for the pure helpers: backoff table bounds (3.0–36.0 s), ±20 % jitter over 10 000 draws,
  rotation `0..4` wrap, taxonomy → strategy mapping, reset/pause transitions.
- **Never-stop simulation:** 1 simulated hour, eligible + never-sharp scene → attempts > 60,
  `max_gap <= 45000`, no state that stops scheduling; then scene becomes sharp after 30 min →
  episode resets on the first sharp sample. Repeat with random aborts/menus and assert no permanent
  stop and no attempt during pauses.
- Parser tests for the new `# hunt` lines and the kind-17 record; `af_report` must not crash on them.
- Normal-mode regression: with Persistent OFF the state machine is never entered (assert no new
  lines, existing corpus unchanged).

### 10.2 Camera: the 2-minute deliberately unfocusable scene (the key test)

Setup: movie mode, LV **x5** crop preset, **AF** lens, **Movie Servo OFF**, monitor **Keep open**,
`Refocus method = MPU drive`, `Refocus AF = Local`, sensitivity **High**, Auto refocus **ON**,
**Refocus persistence = Persistent**, not recording, camera on a tripod.

1. Point at a deliberately unfocusable, textured-but-impossible scene (a white wall with no detail
   in the centre, or a close-up of a lens cap/fabric at less than minimum focus distance). Start a
   2-minute timer.
2. **Acceptance:** `# hunt begin` appears; attempts continue for the full 120 s; `attempts >= 6`;
   consecutive attempt starts never more than **45 s** apart; backoff sequence grows to ≈30 s and is
   jittered; no `# hunt reset` while the scene is unchanged; no crash/assert; no permanent gate 6/11
   pause (you must see `attempts` grow). `why=` values should read `nopeak`/`lowtexture`/`deep`/
   `besteffort`. `max_backoff_ms <= 36000`.
3. **Then, without changing the settings, point at a well-lit textured subject** 0.5–2 m away.
   **Acceptance:** focus is found and the hunt ends: `# hunt reset why=sharp` (or `why=scene` followed
   by a normal trigger and landing) within **≤45 s**, expected <5 s because the luma/scene change
   resets the episode immediately; the picture is visibly sharp; no further `# hunt attempt` lines
   while the picture stays sharp.
4. **Control (Normal mode):** set `Refocus persistence = Normal`, repeat step 1 for 2 minutes. Expect
   the current behaviour: a few attempts, then gate 6/pause patterns and long dead stretches — this
   proves the difference and is not a failure.
5. **A/B regression:** with Normal mode, run the Refocus test and the AF bench B speeds; the numbers
   must match the pre-patch baseline (Persistent must not leak into them). Then with Persistent mode,
   the Refocus test must be **unchanged** (`notrig=0 valid=5 false_pass=0`, same `ref_s` behaviour;
   the hunt is bracketed by `hunt_reset("rtest")`).
6. **Walk:** A↔B subjects at 18 mm for a few minutes: every blur episode shows a `# hunt summary`
   ending in `sharp` or a user/scene reason; no episode > 3 attempts on a normal subject; `refmax` /
   anchor lines do not staircase (patches 1–8 contract).
7. **Recording spot test:** RAW recording, Auto refocus **ON + REC**, MPU drive while recording ON
   with monitor already open: hunt attempts during recording are in-place/pre-opened-MPU only, never
   open/close the monitor, no card write while recording, and `# hunt` text lines appear in AFDRIVE
   only after recording stops. Pull the card: the log is complete.
8. **Battery/lens sanity:** during the 2-minute unfocusable test, record the approximated motor-active
   time (from `# hunt summary travel=` and attempt durations); compare with one continuous Movie
   Servo run; flag if the number looks unexpectedly high.

### 10.3 Acceptance table

| check | pass |
|---|---|
| 2-min unfocusable scene | `attempts >= 6`, no gap > 45 s, no permanent stop, `max_backoff_ms <= 36000`, no crash |
| textured subject afterwards | sharp within ≤45 s (expected <5 s), `# hunt reset why=sharp` (or scene + landing), no further attempts |
| Normal mode control | current pause/gate behaviour unchanged |
| Refocus test (Persistent) | `notrig=0 valid=5 false_pass=0`, `ok >= 4/5`, same as without the hunt |
| AF bench B speeds | unchanged vs the pre-patch build |
| walk | every episode ends `sharp`/user/scene; ≤3 attempts on normal subjects; `ref_max`/anchor do not staircase |
| recording | no monitor open/close, no task_create, no card write while recording; logs complete after stop |
| safety | no crash/assert; no new warnings; `crop_rec.mo` builds deterministically |

---

## 11. Rollback

- **Soft rollback (no rebuild):** set `Refocus persistence = Normal` (or `Auto_refocus = OFF`). The
  hunt state machine is not entered; all patches 1–8 behaviour is exactly as today. This is the field
  escape hatch if the default Persistent is annoying.
- **Firmware rollback:** revert the patch-9 hunks only (Phase 1–6). The file is uncommitted together
  with patches 1–8, so there is no separate commit; keep a pre-patch-9 copy of `crop_rec.c` (and the
  `crop_rec.mo` md5) before flashing. Full rollback of the whole AF stack is
  `git checkout -- modules/crop_rec/crop_rec.c` + rebuild.
- **Per-feature revert:** menu/config (Phase 1), state machine (Phase 2), engine overrides (Phase 3),
  Canon strategies (Phase 4), reset hooks/recording (Phase 5), logs (Phase 6) are independent hunks;
  reverting Phase 3/4 leaves a Persistent mode that only retries the configured strategy (still never
  stops, less diverse).
- After any rollback: rebuild, re-run `contrib/af_test/run_all.py`, reflash.

---

## 12. Residual risks / open questions (ranked)

1. **Hunting on an actually-in-focus scene (false blur) — the big UX risk.** Persistent trusts the
   relative reference and the absolute metrics; a stale-high anchor or a low-contrast "sharp" scene
   can look blurry (patches 3–5 reduced but did not eliminate this). Mitigations: the hunt only
   escalates after real evidence or a provisional/cold reference; it honours half-press/MF/menu; the
   best-effort guard; Normal mode. The field test must report every episode that starts on a scene the
   operator considers **already in focus** (`# hunt begin` then a quick `# hunt reset why=sharp`, or a
   visible attempt on a good picture) — that is the tuning signal, not a reason to raise the trigger
   thresholds globally.
2. **Battery/lens wear on a permanently unfocusable scene.** Bounded but real (≈7–15 % motor duty at
   the cap). Best-effort + cheap strategies + Normal setting are the mitigations; the user explicitly
   asked for this default.
3. **Recording disruption.** Canon in-place AF while recording can visibly hunt and the in-place
   request can interact with the camera's own movie AF. The throttle, the no-x10 rule, the "monitor
   already open" rule and the existing ON+REC opt-in bound it; a take can still be ruined by a
   refocus. Documented; persistent recording is opt-in (`ON + REC`).
4. **Canon x10 detour visibility** (magnification blink) — one of five strategies and skipped while
   recording; reorder or skip via the portfolio table if field feedback is negative.
5. **Stale-high anchor vs "never give up"**: on a genuinely softer scene the anchor's confirmed-soft
   escape lowers the reference and the best-effort guard reduces activity, but if the operator
   disagrees with the camera's "soft is the best this scene has", the hunt keeps trying at 30 s
   intervals — which is the requirement, at a battery cost. No code fix; the Normal setting is the
   answer.
6. **Cold-start / provisional reference is a heuristic.** It uses the existing absolute calibration
   (`q`, `emax`, `AREF_ABS_*`), which was tuned on a small number of 18–20 mm scenes. Mis-trusting a
   valid reference would cause an episode on a normal scene; the first field session must collect
   `# hunt begin why=cold|provref` lines and correlate with the picture before trusting the default.
7. **Zoom/focal-length state (patch 8).** The monitor freezes `lens_info.focal_len`; zoom/mm changes
   reset the hunt (as they reset the anchor), but a missed refresh could leave the hunt cycling on the
   wrong bucket. Patch 8's refresh path is the mitigation; the hunt logs `# hunt reset why=zoom`.
8. **Log volume / note-buffer drops.** The 2 KB `afd_note` buffer (status review R8) can drop lines
   during long recording; the design rate-limits and uses the compact `@REFOCUS` kind-17 record, and
   the `# hunt summary` is emitted at episode end.
9. **No camera validation of thresholds.** `HUNT_COLD_START_MS`, `HUNT_SHARP_PCT`, `HUNT_IMPROVE_PCT`,
   `HUNT_BESTEFFORT_N`, `HUNT_DARK_LUMA` are reasoned, not measured; they are isolated `#define`s and
   the Normal/Persistent A/B is the tuning path.
10. **"Never stop" vs eligibility confusion in the field log.** A long `# hunt pause gate=...` stretch
    is legitimate (menu/MF/servo/LV) but looks like giving up; the summary includes `end=` and the log
    includes the gate, so the report must distinguish "paused by user/camera" from "stopped".
11. **Interaction with the user's own AF**: `AREF_FOREIGN` resets the hunt; a user half-press pause
    resumes immediately on release. If a future Canon firmware/half-press path re-enters, verify no
    ping-pong between hunt and user AF.

---

## Appendix A — snapshot line references used in this document

Snapshot: `/tmp/crop_rec_snapshot_hunt.c`, 16 599 lines, md5 `746df3ebe5f68467a1aeebb4f6b03a54`
(working tree 2026-10-19 ~20:35, patches 1–8). Re-verify by symbol if the tree has moved.

| symbol | snapshot line |
|---|---|
| `CONFIG_INT("crop.auto_refocus*")` block | 127–131 |
| `slim_zoom_to_x10` | 384 |
| `SLIM_AF_SETTLE/BLIND/MAX/RELEASE/INPLACE` | 431–435 |
| `slim_af_request` | 447 |
| `slim_af_can_start` / `_inplace` | 474 / 482 |
| `slim_af_step` | 500 |
| `Shutter_zoom` AF request | 631 |
| `AFD_SPEED`, `AFD_CHUNK_07`, `AFD_MAX_TOTAL` | 7450, 7461, 7504 |
| `AFD_F_QUIET/REC/AREF` | 7527–7529 |
| `afd_sw_speed` | 7603 |
| `afd_endstop_dir` / `afd_es_note` | 7633 / 7676–7681 |
| `AFD_RUN_CAP_MS/B_MS` | 7711–7712 |
| `afd_probe_dir` | 9020 |
| `afd_run_sweep` | 9206 |
| `afd_run_local` | 9436 |
| `afd_run` | 9587 |
| `afd_ref_max_update` | 8727 |
| `afd_engine` | 10433 |
| `aref` block start / `aref_phase` enum | 13560 / 13616 |
| `AREF_PERIOD/SETTLE/COOLDOWN/FAIL/PAUSE/RETRY` | 13568–13581 |
| `aref_abs_q/em`, `AREF_ABS_MAX` | 13606–13610 |
| `aref_anchor_*` | 13631–13660 |
| `AREF_MPU_*` constants | 13690–13705 |
| `AREF_FAILED_MS` etc. | 13726–13728 |
| `aref_mpu_hold_until`, `aref_dec`, `aref_fs_*` | 13737–13756 |
| `aref_mpu_exec` | 13769 |
| `afd_worker_task` / `afd_worker_ensure` | 13802 / 13820 |
| `aref_log` / `afd_note` / `afd_note_flush` | 13828 / 9862 / 9878 |
| `aref_peak_update` | 13876 |
| `aref_mpu_done_note` | 13925 |
| `aref_mpu_settle` | 13947 |
| `aref_sw_push` (scene change) | 14075 |
| `aref_blk_mask` | 14138 |
| `aref_eval` (gate table) | 14159 |
| `aref_mpu_arm` | 14414 |
| `aref_mpu_finish` | 14435 |
| `aref_step` | 14510 |
| `aref_test_run` / `aref_test_task` | 14927 / 15082 |
| `slim_info_button_menu[]` | 5902 |
| `Auto refocus` / `MPU drive while recording` entries | 5947 / 5991 |
| `MODULE_CONFIG(Auto_refocus*)` | 16561–16565 |

## Appendix B — new constants proposed by this design

| constant | value | meaning |
|---|---|---|
| `Auto_refocus_persist` | 1 default | menu setting, 0 Normal / 1 Persistent |
| `HUNT_BACKOFF_MS_N` | 6 | backoff table entries |
| `HUNT_BACKOFF_MS[6]` | 4000, 6000, 10000, 15000, 20000, 30000 | capped at 30 s |
| `HUNT_JITTER_PCT` | 20 | ±20 % jitter via private LCG |
| `HUNT_COLD_START_MS` | 8000 | no-reference eligible time before the cold probe |
| `HUNT_PROV_MS` | 30000 | retry period when the reference is provisional (blurry seed) |
| `HUNT_SHARP_PCT` | 92 | settled med % of the best reference that ends the episode |
| `HUNT_IMPROVE_PCT` | 10 | improvement over pre-attempt that also counts as success (with absolute sanity) |
| `HUNT_BESTEFFORT_N` | 6 | consecutive fails before cheap-only best-effort mode (attempts continue) |
| `HUNT_DARK_LUMA` | 40 | heuristic dark-scene threshold (log/strategy only) |
| `HUNT_REC_MIN_GAP_MS` | 10000 | minimum attempt spacing while recording |
| `HUNT_ATTEMPT_TIMEOUT_MS` | 30000 | completion watchdog (engine/Canon caps are shorter) |
| `HUNT_S_N` | 5 | strategy portfolio size |

## Appendix C — worst-case attempt rate (math)

With `delay ∈ [0.8×base, 1.2×base]` at the cap (`base = 30000`), start-to-start spacing including an
8 s attempt and 1 s settle is ≤ 36 + 8 + 1 = 45 s. Attempts per hour ≥ 3600/45 = **80**. The backoff
ramp from a fresh episode: attempt at t≈0, then ≈4/6/10/15/20/30/30 s (jittered), i.e. **7–8 attempts
in the first 2 minutes** (7 at worst jitter, 8–9 typical). These numbers are the field acceptance
bounds in §10 (the acceptance floor is 6 to allow for a long first attempt).

## Appendix D — deliverables / next step

This document is the input for **patch 9** (implementation, same file, uncommitted, build + field plan
as in §9–§10). Patch 9 should ship the setting defaulting to Persistent, the state machine, the
portfolio, the reset hooks and the `# hunt` logging; Normal mode must remain bit-for-bit behavioural.
