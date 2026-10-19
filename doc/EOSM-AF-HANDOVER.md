# EOS M ML-driven autofocus: handover

**Date:** 2026-10-19 · **Branch:** `eosm-slim-gui` (local, not pushed) · **HEAD:** `47d8589`
**Camera:** Canon EOS M, fw 2.0.2, EF-M 18-55 f/3.5-5.6 IS STM · **Module:** `modules/crop_rec/crop_rec.c`

This handover is for the next engineer or model working on the MPU-driven autofocus. It
supersedes [EOSM-MPU-HANDOVER.md](EOSM-MPU-HANDOVER.md), which predates the working
lens drive. The detailed field history is in [EOSM-AF.md](EOSM-AF.md) §3.8; the test procedure
is in [EOSM-AF-TESTING.md](EOSM-AF-TESTING.md).

---

## 1. Status in one paragraph

ML can now move the EF-M lens itself and run its own contrast AF at x5 under crop presets.
**One-shot AF (algorithm B) is reliable:** the bench gives 6/6 in focus at ±150/±400/±800
pulses of defocus, with a median of 3-6 s. It also ran once during RAW recording without
disturbing it. **Auto refocus with the MPU drive is the open problem.**
- Hunting from panning and texture-less scenes is fixed: 0 false triggers in the Refocus test.
- Re-triggering on *mild* blur still fails. The local search sees no slope within ±40 pulses,
  declares "in focus", and leaves the picture at about 82%.
- Last Refocus test: **3/5 ok**, false_trig 0.

## 2. How it works (the facts that everything relies on)

### The actuator

- ICU→MPU `(9,1,5) [hi lo SPD 00 00]`, sent through the packet builder at `0x38C4`, inside a
  `(9,0x25,16)[02 …]` session.
  - SPD `0x07` is the measured stepping speed, about 0.35 pulses/ms.
  - `0x1F` is about 3× faster and is used for unmeasured repositioning.
  - `0xFF` (ML's `lens_focus` form) only allows ±1..3 pulses, which is why ML's focus never
    moved this lens.
- **The MPU monitor must be open.** `MpuMonOpen` = `send(8,3,1,{00})` re-initialises the
  MPU's lens engine.
  - Without it, every move is rejected in under 40 ms (FOCUS_DONE `0x4a….`).
  - Opening it blacks out LV for about 2 s. The "Keep open" mode pays that once.
- **No position feedback.** `focus_dist` does not update, and the MPU's lens-position cache
  (`0xFFF8132E`) is not refreshed in this mode.

### Motion behaviour (all measured)

- **FOCUS_DONE is not the end of the motion.**
  - At 07 it arrives at a fixed ~510 ms (bit `0x0400`) for long moves.
  - At 1F it can arrive at ~150 ms while the lens moves until ~590 ms.
  - So moves are chunked (100 pulses at 07, 300 at 1F) and wait for a modelled travel time.
  - After an unmeasured move, the code waits for two still frames.
  - Getting this wrong caused most of the "unpredictable" behaviour seen before.
- **Reversals are fine for short moves:** ±40 and ±80 out-and-back return to 95-103% of
  focus. The earlier "backlash" was really moves being cut short.
- **`(9,3,1)` stop does not halt a `(9,1)` move.** Algorithm C (stop-on-threshold) is dead.
- **End stop:** the MPU answers further moves in under 150 ms with bit `0x1000`. Two in a row
  are treated as an end stop (`afd_es_check`).
- **Session close does not move the lens.** The epilogue instrumentation proved it; it was a
  false lead.

### The focus measure

- Laplacian sum `s` and gradient sum `g` over the centre ¼×¼ (`afdiag_sharpness_ex`, stride 2,
  "ex" scale).
- The fast per-frame `afd_measure` uses stride 4 (about 4 ms per call). Convert between the
  two with `afd_to_ex`.
- At x5 on the 18-55 at 18 mm, the s peak is about 40-60 pulses wide (σ ≈ 21) and g about
  ±120.
- Blur floor ≈ 17k (ex-scale); detailed subjects in focus reach 80-180k.

## 3. Code map (`modules/crop_rec/crop_rec.c`)

| Area | Functions |
|---|---|
| Engine | `afd_engine(mode, flags)`: session, monitor, LV-recovery wait, epilogue (stop, close), logs. Modes: 0 AF drive, 1 calib, 2 probe, 3 speed, 4 sweep record, 5 stop test, 6 bench, 7 rev-cal, 8 Refocus-test prep. Flags: `AFD_F_QUIET`, `AFD_F_REC`, `AFD_F_AREF` |
| Moves | `afd_move_ex` (chunking, done wait, travel-time wait, stillness wait, end-stop check), `afd_move_track` (samples every frame during a move), `afd_goto` |
| Algorithms | `afd_run` dispatcher (deadline, landing verify), `afd_run_sweep` / `afd_sweep_find` (**B**, the default), `afd_run_local` (Auto refocus), `afd_run_a/d/e`, `afd_land_steps` (feedback retrace plus climb to the top) |
| Monitor | `afd_mon_open_raw`, `afd_mon_close_now`, self-heal on a stale monitor, re-open on wake, auto-close (default Never) |
| Auto refocus | `aref_step` → `aref_eval` (gates 0-12), `aref_mpu_arm`, `aref_mpu_exec` (in the persistent worker `afd_worker_task`), `aref_mpu_finish`, `aref_mpu_settle` |
| Tests | `afb_*` (AF bench), `aref_test_run` (Refocus test), `afw_*` (sweep record and stop test), `afd_rect_*` (REC test) |
| Safety | `afd_must_abort`, `afd_keep_awake`, `afd_log_flush` / `afd_note_flush` (never while recording) |

Menu: Settings → the AF diag entries (AF drive (MPU), AF drive monitor, AF algorithm, Sweep
speed, Fine step, Session close, AF bench, Refocus test, …) and the Auto refocus entries
(Auto refocus, Refocus sensitivity, Refocus method, Refocus AF, MPU drive while recording).

## 4. Open problems, in priority order

### 4.1 The local search accepts a soft picture (cause of the 3/5)

- **Evidence:** latest AFRTEST.LOG.
  - Trials +60 and +150 triggered (82% and 83% of ref).
  - The local search made 3 moves, found neither side >5% better, and returned "in focus".
  - The final picture was 82-83%.
- **Likely cause:**
  - The + side is close to the end stop, so the defocus moved little and left a flat flank.
  - ±2×fine (±40) probes cannot see a 5% slope there.
  - The plateau guard only falls back to B below 70% of the last landing.
- **Suggested fix**, in `afd_run_local`:
  - If no side improves and s0 < 92% of the last landing (same zoom), first probe wider
    (±4×fine).
  - If that is still flat, fall back to B.
  - Also skip the "in focus" exit when the trigger came from the drop path (`pct` well below
    100).

### 4.2 Reference erosion

- Across one Refocus test, ref_s drifted 115k → 95k → 86k → 77k (earlier session) and
  97k → 84k → 71k (latest), because each re-land lands a little lower.
- The climb-to-top (`afd_land_steps`, commit `47d8589`) helped only partly.
- Consider tracking a per-zoom, per-scene maximum, and comparing landings to it rather than
  to the last landing.

### 4.3 Speed

- The median AF time went from about 3 s to about 6 s after the motion-end waits (`fcc6f88`).
- The travel model (`AFD_MV_MSPP_*`, `AFD_MV_LAT_MS`) uses guessed values for 0x14 and 0x1F.
- Measure the real speed per speed byte with **AF sweep record** (AFSWEEP.LOG), then tune.
  The offline replay tool (`contrib/af_replay`) parses AFSWEEP.

### 4.4 Focal length

- Almost all data is at 18-20 mm. The user reports worse AF at 30-50 mm.
- **Fine step: Auto** (20 / 14 / 10 by focal length) is untested there.
- Run the Refocus test and the bench at 35 and 50 mm.

### 4.5 Recording

- Auto refocus during RAW recording (MPU drive while recording) worked in the few tries made
  (a REC test landed at 100% in 6.2 s, with steady LV cadence), but it needs more runs.
- True dropped-frame counts need an mlv_lite accessor; today only the vsync cadence is
  logged.

### 4.6 Minor

- The bench reference drifts if the light changes. It re-references per algorithm and marks
  `drift=1`.
- Algorithms D, E and F are inferior on the bench; keep them only as experiments.

## 5. Build, flash, test, analyse

```
# build (python2 is unavailable; module_strings.h placeholders must exist, see below)
for m in crop_rec mpu_lab; do printf '#define MODULE_STRINGS() \\\n    MODULE_STRINGS_START() \\\n    MODULE_STRINGS_END()\n' > modules/$m/module_strings.h; done
make -C platform/EOSM.202 zip SHELL=/bin/bash PYTHON=python3
# flash: card is /dev/disk4s1 → /Volumes/EOS_DIGITAL (often present but unmounted)
diskutil mount /dev/disk4s1
unzip -oq platform/EOSM.202/magiclantern-crop_mood.2026Oct19.EOSM202.zip -d /Volumes/EOS_DIGITAL
md5 /Volumes/EOS_DIGITAL/ML/modules/crop_rec.mo     # compare with the zip
diskutil unmount /Volumes/EOS_DIGITAL
# analyse a session
python3 contrib/af_report/af_report.py --copy <dir>
```

The test procedure is [EOSM-AF-TESTING.md](EOSM-AF-TESTING.md). The **Refocus test** is the
key regression measure for Auto refocus; target ok 5/5 with false_trig 0. The **AF bench**
with "B speeds" is the regression measure for the engine.

## 6. Rules and pitfalls (each one has bitten this project)

- **ML `snprintf`:** only `%d`, `%x` and plain `%s`. `%c` prints garbage and `%%` shifts
  arguments. No field widths on `%s`.
- **Never create a task in the module init.** crop_rec loads before mlv_lite. A task created
  there made mlv_lite's `compress_task` creation fail, so RAW recording silently did nothing
  and the console stayed open (`ASSERT … mlv_lite.c:5270`). The AF worker is created lazily
  (after the first menu AF run).
- **Never `task_create` during RAW recording** (no memory). Recording-time work uses the
  pre-created worker or the REC test's arm-time task.
- **Never open or close the MPU monitor while recording** (2 s LV blackout). **Never write
  the card while recording;** logs are held in RAM and flushed after recording.
- **Every engine path must send the stop `(9,3,1)` and the session close.** The epilogue in
  `afd_engine` does this; keep it single-exit.
- **CPU:** long AF loops must `msleep` between frames. A 30-50 ms measure in a tight loop at
  high priority coincided with a Canon assert in `LvWbAlgorWrap.c:522`. AF tasks run at
  priority `0x1f`.
- **`module_strings.h`** is generated by a Python 2 script. Keep the 3-line placeholder. A
  failing build can delete it, and an empty file produces a `MODULE_STRINGS` warning and
  drops the module metadata.
- **Do not trust FOCUS_DONE as the end of a move.** Do not land at a computed absolute
  position after a long or fast move. Land by sharpness feedback with uniform short steps in
  one direction.

## 7. Tools and logs

| Tool | Purpose |
|---|---|
| `contrib/af_report/af_report.py` | One-command session report and verdicts (reads the card) |
| `contrib/af_replay/` | Offline algorithm simulator fitted on the logged curves (REPORT.md) |
| AFDRIVE.LOG | Every AF run, plus monitor, LV and `# aref` notes |
| AFRTEST.LOG | Refocus test (`# RT_TRIAL`, `# RTEST` summary) |
| AFBENCH.LOG | Bench per algorithm and defocus |
| AFSWEEP / AFSTOP / AFREV / AFCALIB / AFPROBE / AFSPEED.LOG | Diagnostics |

Field log copies from this session are in the Claude scratchpad (`card8` … `card18`). They
are temporary, so copy anything worth keeping.

## 8. Commit history of this work

There are 47 commits from `f127fe3` to `47d8589`. The milestones:

| Commit | Milestone |
|---|---|
| `ba0824c` | mpu_lab drive test: first lens movement with `(9,1)` at speed 07 |
| `7be6799` | First AF drive (MPU) |
| `81799ee` | Monitor "Keep open" |
| `8917e0b` | Feedback-retrace landing: first reliable AF |
| `22e0613` | `afd_engine(mode, flags)` refactor |
| `3ef455f` / `2fa454a` | Algorithm selector, bench, reversal cal |
| `7287f35` / `fa4fcaf` | Motion chunking, cheap measure, B default |
| `44be715` | Persistent AF worker, MPU refocus while recording |
| `aa80074` | Fix: no task at init (mlv_lite) |
| `8a562c6` + `1e57793` | End stop by flags, low texture; Auto refocus settle gate, Refocus test |
| `fcc6f88` | Wait for the real end of motion, landing verify |
| `47d8589` | MPU trigger sensitivity 60/75/85%, climb-to-top landing |

## 9. Housekeeping

- Nothing is pushed. `git push` and any PR are for the owner to decide.
- `doc/EOSM-MPU-HANDOVER.md` has an uncommitted change from another session.
  `doc/EOSM-AF-ALGORITHMS.md` and `doc/EOSM-AF-IMPLEMENTATION.md` are untracked research notes
  from another session. They were used, but this work did not commit them.
- Agent worktrees are left under `.claude/worktrees/`. They can be removed with
  `git worktree remove`.
- Card module backups are in `/tmp/mpu_lab_backup_*`.
