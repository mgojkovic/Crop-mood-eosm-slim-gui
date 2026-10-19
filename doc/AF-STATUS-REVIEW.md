# EOS M ML autofocus — current-state review

**Review date:** 2026-10-19
**Branch:** `eosm-slim-gui` · **Audited commit:** `043d9148c24bd710df1caee7063cf9a630a19eda` (`043d914`, "doc: AF algorithm research notes…")
**Code audited:** `modules/crop_rec/crop_rec.c` at HEAD (15 740 lines), plus `modules/mpu_lab/mpu_lab.c`, `src/lens.c`
**Why HEAD and not the working tree:** another agent is editing `crop_rec.c` while this review was written
(`git status` shows it modified; `git diff` adds ~95 net lines implementing a local-search / reference fix).
**Every line reference below is for HEAD**, reproducible with
`git show HEAD:modules/crop_rec/crop_rec.c > /tmp/crop_rec_HEAD.c`. Working-tree line numbers will drift and are
not used except where explicitly labelled *WIP*.

**Evidence limits — read before trusting any number:**
- No camera is attached; the field-log copies (`card8`…`card18`, `/private/tmp/...` scratchpad) are gone.
  `contrib/af_replay/REPORT.md:3` still points at that missing directory. Every "6/6", "3/5", "3-6 s",
  "115k→77k" figure below is therefore a **doc claim**, not a re-measured number.
- What *is* verifiable: code paths, constants, state machines, and the offline simulator
  (`contrib/af_replay/REPORT.md`). The simulator is a ranking tool, not a measurement of this build —
  its assumed sweep speed/lag (0.40 pulses/ms, 42 ms lag, 30 ms start delay) differ from the code's own model
  (2.9 ms/pulse = 0.345 pulses/ms, 120 ms latency; `crop_rec.c:8519-8531`).
- The handover's claimed HEAD is `47d8589` (`doc/EOSM-AF-HANDOVER.md:3`); the two commits since then
  (`2f880c9`, `043d914`) are documentation only — `git diff 47d8589..HEAD -- modules/crop_rec/crop_rec.c` is empty.
  So the handover's code statements describe current code, only its HEAD hash is stale.

---

## 1. Status matrix

Verdicts: **WORKS** = code path complete and internally consistent, with some corroborating doc evidence;
**PARTLY** = works in the happy path but has a known structural defect or an unverified dependency;
**BROKEN** = the code, as written, cannot deliver the claimed behaviour; **UNVERIFIED** = implemented but no
usable evidence either way.

| # | Capability | Verdict | Evidence (doc claim + code ref) | Confidence |
|---|---|---|---|---|
| 1 | Engine A (stepwise: g-probe, coarse g climb, fine s pass, feedback retrace) | **PARTLY** | Code complete at `crop_rec.c:8306-8498` (`afd_run_a`), retrace `8256-8291`; handover calls B the default and A the fallback (`HANDOVER:71-73`). Wrong on the docs' own account of "working algorithm" (see D8). | Medium |
| 2 | Engine B (tracked sweep + stepped landing) — **default** | **PARTLY** | `afdiag_afalgo=1` (`7508`), dispatch `9317-9325`, sweep `8674-9083`; handover claims 6/6 bench and median 3-6 s (`HANDOVER:16-18`) but logs are gone. Self-referential scoring (root causes 4.1 and 4.2). | Medium: it is the least-bad structure; claims unverified |
| 3 | Engine D (Gaussian model jump) | **UNVERIFIED / PARTLY** | Implemented `9088-9136`; fixed `AFD_SIGMA 21` (`8537`) and floor assumptions (`8539`) calibrated only at 18-20 mm. Handover says inferior (`HANDOVER:133-134`). | Low outside 18-20 mm |
| 4 | Engine E (warm start) | **PARTLY** | `9143-9205`; depends on `afd_last_land_s` (`8562-8563`, read `9187`, `9195`) which is re-taken after **every** landing, including soft local landings (`9349-9352`) — same erosion family as 4.2. | Medium |
| 5 | Engine F (predictive fine pass) | **UNVERIFIED** | `afd_run_a` with `afd_algo_cur==5` (`8432-8440`), parabola fit `8598-8610`; simulator ranks it 76 % / 9.5 s cold vs B 85 % / 5.0 s (`af_replay/REPORT.md:97,104`). Thresholds `AFD_F_*` (`8559-8560`) untuned. | Low |
| 6 | Auto refocus **local search** | **BROKEN** | In-focus exit `9244-9257`; fallback to B only when `smax <50 %` (`9230`, `AFD_L_FAR_PCT`, `8557`) or `<70 %` (`9236`) of the last landing — matches handover 3/5 evidence (`HANDOVER:20-22,85-100`). See 4.1. | High (code logic) / field count unverified |
| 7 | Auto-refocus trigger gates 0-12 | **WORKS / PARTLY** | `aref_eval` `13460-13693`; gates enumerated `13455-13457`; MPU thresholds 60/75/85 % (`13063`), 500 ms extra dwell (`13125`), scene-settled `13144-13145`. Works but calibrated on a handful of 18 mm scenes; doc tables stale (D6). | Medium-high |
| 8 | Reference handling (peak memory / post-landing reference) | **BROKEN** | Peak memory `13296-13322`, clear `13283-13293`; but `afd_mpu_finish` sets `aref_mpu_newref=1` on any pct≥0 (`13744-13748`) and `aref_mpu_settle` adopts the first settled sample as the new peak (`13343-13355`). See 4.2. | High |
| 9 | Recording path (Auto refocus MPU while RAW recording) | **UNVERIFIED / PARTLY** | Requires RAW + Keep open + monitor already open (`10062-10067`); worker pre-created (`13240-13246`, `13925-13960`); REC test path `10489-10581`; handover claims one run at 100 %/6.2 s (`HANDOVER:123-128`), log gone. True dropped frames not measurable (`10550-10557`). Menu-task safety gap in risk R3. | Medium |
| 10 | Monitor / session safety | **WORKS / PARTLY** | Monitor helpers `7528-7543`; required for drive `10115-10132`; stale self-heal `10216-10256`; epilogue stop + session close + epilogue samples `10262-10311`; no monitor ops while recording (`10062-10065`, `10235-10309`). Invariant is not encoded (risk R2); servo not aborted (R4). | Medium-high |
| 11 | Diagnostics / tests | **WORKS, with caveats** | AFDIAG 1 MB rotate `6026-6028`, `6495-6510`; AFDRIVE/AFBENCH/AFRTEST/AFSWEEP/AFSTOP/AFREV; bench `9727-9935`, Refocus test `14112-14227`. Bench/Refocus scoring is relative and drift-prone (4.2, 4.3). | High that tools exist; Low that their verdicts are trustworthy |
| 12 | Speed | **PARTLY / fails its own target** | Target: median ≤5 s (`doc/EOSM-AF-TESTING.md:43`); handover reports ~6 s (`HANDOVER:110-116`). Cost structure: 100-pulse 07 chunks (`7417`, `8512`), FOCUS_DONE timer ~510 ms (`7414-7416`), landing verify +300 ms + re-land (`9326-9346`), epilogue ~1.5-2 s (`10262-10279`). See 4.3. | Medium (arithmetic from code); field numbers gone |
| 13 | Focal-length coverage (35-50 mm) | **UNVERIFIED / likely BROKEN** | Only `afd_fine_eff` adapts: 20/14/10 pulses by `lens_info.focal_len` (`7497-7507`). Probe sizes, landing margins, acceptance percentages and all absolute sharpness gates are focal-invariant (`8537`, `8522-8536`, `8553-8560`, `13066`, `13149`). Handover admits all data is 18-20 mm (`HANDOVER:117-121`). See 4.4. | Low / no data |
| 14 | Canon one-shot paths (Shutter-zoom AF, Canon Refocus method) | **WORKS (per docs, not re-verifiable)** | `slim_af_*` `447-...`, `slim_af_step` `500`; Auto-refocus Canon fallback `13962-13969`; `doc/EOSM-AF.md:38-63`, `106-107`. Not central to the MPU work. | Medium |
| 15 | Legacy "AF search (MPU)" action (`(8,2,32)` index 16, AFSEARCH.LOG) | **WORKS but obsolete** | `afdiag_afsearch_task` `7274-7384`, limits `7199`; still in the Settings menu (`10824-10830`) but omitted from the handover code map and from the current test protocol. Its `AFS_MAX_MS 3000` is the only surviving instance of the "current action limit" mentioned in `doc/EOSM-AF-IMPLEMENTATION.md:64-67`. | Medium |

---

## 2. Verified code map (HEAD `043d914`)

### 2.1 Actuator, motion model, engine

| Symbol | HEAD lines | What it actually does |
|---|---|---|
| `afdiag_sharpness_ex` | `6110-6159` | Laplacian sum + gradient sum + luma + max edge over the centre ¼×¼, stride 2 ("ex" scale) |
| `afd_measure` | `7785-7839` | Same ROI minus 4 px border, stride 4 by default (stride 2 + s2/s4/band in `afd_meas_full`); `q=s1*1000/g` |
| `afd_measure_cost_us` | `7843-7862` | Times 8 calls; logged, never used for decisions |
| `afd_move_track` | `7895-7972` | Raw `(9,1,5)`, one sample per new LV frame, optional stop callback, FOCUS_DONE wait |
| `afd_move_ex` | `8067-8210` | Chunked move; per-chunk FOCUS_DONE wait, end-stop check, modelled travel wait, settle + sample, end-stop-by-flat check, stale-monitor detection |
| `afd_goto` | `8224-8234` | Approach-leg helper (unmeasured overshoot then measured `AFD_APPROACH=120`) |
| `afd_retrace` | `8256-8291` | One landing pass: unmeasured back move, fine steps, threshold hit, fall reaction, flat-floor abort |
| `afd_land_steps(_i)` | `8839-8904` | Two retraces + best-point fallback, then the "climb to top" half-step loop |
| `afd_run_a` | `8306-8498` | A and F |
| `afd_run_sweep` / `afd_sweep` / `afd_sweep_find` | `8989-9083` / `8674-8763` / `8914-8977` | B and (unreachable) C |
| `afd_run_d` / `afd_run_e` / `afd_run_local` | `9088-9136` / `9143-9205` / `9214-9306` | Model jump / warm start / auto-refocus local |
| `afd_run` | `9311-9354` | Dispatch, deadline, 300 ms landing verify + one re-land, publishes `afd_land_s`/`afd_last_land_s` |
| `afd_engine` | `10046-10375` | Session open, monitor, LV recovery, dispatch, stale self-heal, epilogue, logs, NotifyBox |
| `afd_task_common` | `10378-10419` | Menu-run busy claim, preconditions, delayed engine call, lazy worker creation |

### 2.2 Auto refocus

| Symbol | HEAD lines | Notes |
|---|---|---|
| `aref_drop_ref` / `aref_peak_clear` / `aref_peak_update` | `13274-13322` | Peak memory: two 12 s buckets, median-of-3 feed, max edge/g remembered |
| `aref_mpu_arm` | `13696-13706` | Trigger bookkeeping; stores `aref_mpu_pre` (median of last 3) |
| `aref_mpu_exec` / `afd_worker_task` / `afd_worker_ensure` | `13194-13246` | Persistent worker (one task), publishes pct/ms/land/path |
| `aref_mpu_settle` | `13343-13375` | **Adopts the first settled sample as the new reference** (or the landing if settled <70 % of it); "hold scene in focus" hysteresis |
| `aref_mpu_finish` | `13709-13758` | Cooldown, failure count, failed-scene memory, `aref_mpu_newref=1` if pct ≥ 0 |
| `aref_eval` | `13460-13693` | One decision: sample, peak update, peak/absolute path, dwell, cooldown, steady, texture, settled, failure-pause, logging; returns path 1/2 |
| `aref_step` | `13760-13970` | Phase machine, eligibility, Canon vs MPU dispatch, worker post |
| `aref_test_run` / `afrt_prep` / `afrt_reference` | `14112-14227` / `9991-10010` / `14098-14110` | Refocus test: reference landing, holds, 5 defocus trials, re-reference when a trial fails |
| `afd_rect_task` / `afdiag_afrect_select` | `10489-10607` | REC test: arm-time task, 1 s wait, 3 s delay, one engine run under recording, 5 s watch |

### 2.3 Constants that matter (HEAD)

| Constant | Value | Line |
|---|---|---|
| `AFD_SPEED` / `MID` / `FAST` | `0x07` / `0x14` / `0x1F` | `7406-7408` |
| `AFD_MV_MSPP_*_X10`, `AFD_MV_LAT_MS` | 29 / 14 / 10, 120 ms | `7410-7413` |
| `AFD_CHUNK_07/MID/FAST` | 100 / 200 / 300 | `7417-7419` |
| `AFD_DONE_TIMEOUT_MS` | 800 | `7446` |
| `AFD_MAX_TRAVEL` / `AFD_MAX_TOTAL` | 1500 (coarse) / 9000 (run) | `7444`, `7460` |
| `AFD_RUN_CAP_MS` / `_B_MS` | 15 000 / 18 000 | `7662-7663` |
| `AFD_SW_MAX_CHUNKS` / `_PRE` / `_MARGIN` / `_ERR` | 12 / 60 / 80 / 100 | `8512-8531` |
| `AFD_SW_MSPP_X10` / `LAT_MS` / `COAST_MS` | 29 / 120 / 40 | `8522-8529` |
| `AFD_SIGMA` (D) | 21 pulses | `8537` |
| Local: `IMPROVE` / `FAR` / `DROP` / `LAND` / `INFOCUS` | 105 / 50 / 90 / 92 / 85 % | `8553-8558` |
| End stop: `MIN` / `PCT` / `ES_MS` / `ES_N` | 40 / 2 % / 150 ms / 2 | `7576-7580` |
| `AFD_MIN_S` / `AFD_MIN_G` | 2000 / 5000 | `7452`, `7582` |
| AREF period / settle / peak hold | 200 / 500 / 12 000 ms | `13037-13041` |
| AREF MPU drop / peak min / abs q,em | 60/75/85 %, 30k/20k/16k, q≥105/95 & em≤45/55 | `13063-13076` |
| AREF MPU gap / dwell extra / same / improve | 4000 / 500 / 8 % / 5 % | `13124-13128` |
| AREF settled / min g | 800 ms / 12 000 | `13144-13149` |

### 2.4 What the handover's code map gets wrong or omits

- It omits `afd_sweep`, `afd_land_steps_i`, `afrt_prep`, `afw_run`, `afd_rect_*`, the whole `afd_*` constant block,
  `afd_algo_menu_tbl` (`7561`), `afd_rt_thr1` (`8254`), `afd_ep_close2` (`10035`), and the legacy AF-search action
  (`7274`, still in the menu at `10824`).
- It says the engine modes are 0-8 with mode 8 "Refocus-test prep"; mode 8 is a real prep action (`afrt_prep`) but
  the handover never says that a run of mode 8 is what the Refocus test calls before every trial.
- It lists chunking as "100 at 07, 300 at 1F"; 0x14 is also a supported speed with a 200-pulse chunk (`7418`,
  `7567`) and is exposed as bench B14 (`7561`, `9663`).
- It presents the AF worker as created "lazily (after the first menu AF run)"; code also creates it after the
  Refocus test (`14263-14264`) and from `aref_step` before recording (`13928`). A comment inside the aref block
  still says "created once at module init" (`13178-13179`) — wrong; see D12.
- It does not mention that `aref_mpu_finish` treats **any non-negative engine pct** as success (`13744`), or that
  engine pct is relative to the run's own best (4.1).
- It does not mention the `@REFOCUS` event kinds 13 and 14 used by the MPU path (`13756`, `13357`).

---

## 3. Doc/code discrepancies

| # | Type | Doc says | Code says | Impact |
|---|---|---|---|---|
| D1 | stale header | HEAD `47d8589` (`HANDOVER:3`) | HEAD `043d914`; code unchanged since `47d8589` | Cosmetic; pin docs to a commit |
| D2 | incomplete root cause | "plateau guard only falls back to B below 70 % of the last landing" (`HANDOVER:94`) | Also a 50 % far guard (`9230`) and an in-focus exit at 85 % **of the current, already-soft `s0`** (`8558`, `9248`); the 70 % check is against `afd_last_land_s`, not the trigger reference | The handover's fix list misses the actual accepting branch; see 4.1 |
| D3 | wrong root-cause model | Erosion "because each re-land lands a little lower" (`HANDOVER:104-108`) | Two bookkeeping ratchets: `aref_mpu_settle` clears the peak and adopts a new one (`13343-13355`), and the Refocus test re-takes `ref_s` from a fresh B07 landing after a failed trial (`14203-14208`, `14098-14109`). Both are reference *definition* bugs as much as landing accuracy | A local-search-only fix cannot stop the drift (Lead finding, confirmed) |
| D4 | stale doc | "After each AF it stores the sharpness as the reference" (`EOSM-AF.md:42-43`) | MPU path waits for `settled >= AREF_SETTLED_MS` (800 ms) and can keep the landing instead (`13523-13526`, `13347-13355`) | Triggers and reference behaviour differ from the Canon method |
| D5 | stale summary | "ML-driven lens movement (native contrast AF) **Not achieved yet**" (`EOSM-AF.md:21`) | Fully implemented by `(9,1,5)` `afd_*` engine (`doc/EOSM-AF.md:230-324` supersedes) | Old doc still read first by newcomers |
| D6 | stale table | Peak minimum 45000/30000/24000; "Absolute q ≤ off/15/25" (`EOSM-AF.md:76-84`) | `aref_peak_min = 30000/20000/16000` (`13066`); absolute blur needs q ≥ 105/95 **and** em ≤ 45/55 (`13075-13076`) | A maintainer tuning from the table would set wrong thresholds |
| D7 | stale event table | `@REFOCUS` kinds 1-12 (`EOSM-AF.md:88-103`) | kinds 13 (MPU result, `13756`) and 14 (settled sample, `13357`) also exist; AFDIAG also emits `@LENS_SKIP`, `@FPC`, `@MPUSRCH` (`6043-6072`, `6311-6320`) | Log-analysis tools may drop records |
| D8 | stale algorithm description | "Working algorithm (commit 8917e0b)" = 60-pulse g climb, 20-pulse fine, retrace (`EOSM-AF.md:264-270`) | That is algorithm A; the default is B sweep (`7508`, `8989-9083`), and A's coarse step is 100 (`7422`) | Benchmark/field reports citing "the algorithm" are ambiguous |
| D9 | stale research notes | "(9,1,5) raw form not camera-tested", "AF drive not yet field-validated", "no command abstraction", "`AFS_MAX_MS` 3000 is the current limit" (`ALGORITHMS:20-25,67,234-239`; `IMPLEMENTATION:16-23,57-60,64-67`) | Engine is implemented and field-used; `AFS_MAX_MS 3000` now only limits the legacy `afdiag_afsearch_task` (`7199`) | Old notes contradict the handover; mark them historical |
| D10 | unreachable code | "Algorithm C (stop-on-threshold) is dead" (`HANDOVER:51-52`) | True: C is `case 2` (`9320`), the menu table excludes it (`7561`), bench masks exclude bit 2 (`9663`). The C code remains (`9044-9061`) | Dead but harmless; delete or document |
| D11 | dead state | Handover implies end-stop range memory exists (`HANDOVER:52-53`) | `afd_es_off/afd_es_have` are only logged (`7632-7641`); `afd_best_dir` (`7574`) is written, never read; `afd_last_done_val` is written, only `_ms` is read (`9513-9516`) | Misleading map; no range logic to rely on |
| D12 | wrong comment | AF worker "created once at module init" (`13178-13179`) | Explicitly forbidden and not done (`15653-15656`); only `afdiag_lens` is created at init (`15652`) | Could tempt a future engineer to move worker creation to init, re-breaking mlv_lite |
| D13 | internal contradiction | "(9,3,1) does not halt a (9,1) move" (`HANDOVER:51`; menu help `10871`) | B sweep uses `(9,3,1)` as its stop (`7942-7946`, `8642-8650`, `9049`) and then computes the lens position from `stop_ms + AFD_SW_COAST_MS` (`8705-8707`) | If the doc is right, B's peak-position estimate is systematically wrong; the retrace tolerates it only because it re-finds the peak by feedback. Must be measured (action 3) |
| D14 | claim vs code comment | "Hunting from … texture-less scenes is fixed" (`HANDOVER:19`) | `AFD_MIN_G 5000` comment admits the opposite: "g cannot separate them" — successful defocused runs start at g 7.9-14.6k, a texture-less scene at 13.5-14.5k (`7582-7583`). Only the aref gate 10 (`13149`, `13664`) adds a second check on the peak's gradient | The engine can still hill-climb a blank wall; "0 false triggers" is a property of the aref gates, not the engine |
| D15 | doc omission | "Every engine path must send stop and session close… keep it single-exit" (`HANDOVER:167-168`) | True and implemented as a long palindromic block (`10262-10311`), but there is no cleanup label; the invariant relies on "no `return` after line 10183" (`10183` opens the session) | Maintenance risk R2 |
| D16 | doc omission | "Never create a task during RAW recording" (`HANDOVER:163-164`) | Menu SELECT handlers `task_create(..., 0x3000, ...)` **before** `afd_task_common` checks recording (`10729-10733`, etc.; check at `10390`). Only the REC test arms safely (`10591`) | Risk R3 |
| D17 | doc overstates log completeness | Handover calls AFDRIVE.LOG "every AF run, plus monitor, LV and `# aref` notes" (`HANDOVER:185`) | `afd_note` uses a 2048-byte buffer that silently drops overflow (`9570-9587`) and `afd_note_flush` refuses to run while a log is pending (`9589-9591`) | Notes can be missing or delayed; the log is not a complete record |
| D18 | scoring semantics | Handover "6/6 in focus" / "3/5 ok" (`HANDOVER:16-18,22`) | Bench score = `land_s >= 90 %` of a reference that is re-landed per algorithm and silently re-based on drift (`9649`, `9770-9777`, `9806-9818`); Refocus-test score = final vs a reference that is re-taken after a failed trial (`14196`, `14203-14208`) | Both metrics can drift with the reference; a clean score does not prove absolute focus. TRT/bench logs need the `ref_s`/`drift` columns read together |
| D19 | naming | Handover calls mode 8 "Refocus-test prep" (`HANDOVER:71`) | Engine name is "Refocus test" (`10049`), while the actual test driver is `aref_test_run` (`14112`) | Cosmetic |
| D20 | absolute gates | `EOSM-AF.md:73` says blur = q ≥ 105 (Medium) / 95 (High) and max edge ≤ 45/55 | Matches code (`13075-13076`) but was calibrated on one session; `aref_abs_q` threshold for Low is 0 = off | Absolute fallback is uncalibrated for other scenes |
| D21 | stale "current action" | `IMPLEMENTATION:57-60` "AF drive uses fixed steps 80/20 and a 1500-pulse cap" | Engine uses coarse 100 / fine auto 20/14/10 (`7422`, `7497-7507`), `AFD_MAX_TOTAL 9000` (`7460`) and a 1500 coarse-only cap (`7444`) | The handover is right; implementation doc is obsolete |

---

## 4. Root-cause analysis for the open problems

Marking: **[verified]** = directly readable in HEAD code; **[mechanism]** = code-supported but needs field
confirmation; **[speculation]** = hypothesis, no usable evidence.

### 4.1 Local search accepts a soft picture (the 3/5)

**[verified]** The accepting path is not the 70 % guard alone. With no better probe side (`dir==0`,
`9214-9227`), `afd_run_local`:

1. falls back to B only if the far guard trips: `smax < 50 %` of `afd_last_land_s` (`9230`) or
   `smax < 70 %` of `afd_last_land_s` (`9236`);
2. otherwise returns to the start and measures `se` (`9246`);
3. accepts `se` if `se >= 85 %` of **`s0`** (`AFD_L_INFOCUS_PCT`, `8558`, `9248`) — i.e. if the picture is
   *still as soft as when the trigger fired*;
4. sets `afd_land_s = se; *best_out = MAX(s0, se)` (`9255-9256`).

So an 82 % picture (relative to the true reference) passes whenever `smax` is 70-100 % of the last landing and
`se` is ≥85 % of the already-soft `s0`. That is exactly the handover's +60/+150 trials (`HANDOVER:87-93`).
The +side near an end stop gives a flat flank, the ±40-pulse probes (`2*fine`, `9219`) cannot see the 5 % slope
needed by `AFD_L_IMPROVE_PCT = 105` (`8553`, `9224-9225`), and nothing in this path consults the trigger
reference (`aref_ref`) or the trigger drop percentage.

**[verified] The engine's success metric hides it.** `afd_engine` reports
`pct = final_s * 100 / best` (`10213`), and for this path `best = MAX(s0, se)` (`9256`), so the run can log
`land_pct = 100` while the absolute picture is 82 % of the scene reference. `aref_mpu_finish` only requires
`pct >= 0` to treat the run as a success and trigger post-landing reference adoption (`13744-13748`). The
Refocus test does **not** use that pct: its per-trial criterion is `fin_pct >= 90 %` of `ref_s`
(`14194-14196`), so within a trial a soft landing is correctly failed — but see 4.2 for how `ref_s` drifts
between trials. Answer to the Lead's question: the trial criterion is absolute *against the current ref_s* and
cannot hide a soft landing in the same trial; it can hide it across trials because a failed trial re-takes
`ref_s` from a new B07 landing (`14203-14208` → `14098-14109`) and that re-land is not required to be close to
the old `ref_s`.

**[mechanism] Why B is the fallback but was not always reached:** `afd_last_land_s` is itself updated by soft
local landings (`9350-9351`), so after one soft acceptance the 70 % guard is measured against an eroded
baseline. That is the link between 4.1 and 4.2.

**[WIP, not in HEAD]** The working tree (another agent) adds `afd_ref_max_s` (per-zoom running max of verified
landings), a wide ±4×fine probe, and a trigger hint `afd_aref_trig_pct/path` passed from `aref_mpu_arm`
(uncommitted diff touching the local-search block around HEAD `8554-8560`/`9214-9306`, the trigger arm at `13696-13706`, the scene reset at `13383-13390`, and the landing update at `9327-9352`). This addresses 4.1
and the local-search comparison, but **not** the aref reference ratchet in 4.2. Do not review/land it as a
complete fix.

### 4.2 Reference erosion (115k→95k→86k→77k)

**[verified] Ratchet 1 — post-landing reference adoption.** After any run with `pct >= 0`,
`aref_mpu_finish` sets `aref_mpu_newref = 1` (`13744-13748`). In `aref_eval`, once the scene has been settled
for 800 ms (`13523-13526`), `aref_mpu_settle` does:

- `aref_peak_clear()` — the old peak is destroyed (`13346`);
- if `med < 70 % of aref_mpu_land`, keep `aref_mpu_land`; **otherwise adopt `med`** (`13347-13355`).

A landing at 82 % of the true peak therefore becomes the new 100 % whenever the settled sample is within 70 %
of that same landing — which is always true when the landing is stable. The pre-trigger reference
(`aref_mpu_pre`, `13703`) and the trigger reference (`aref_trig_ref`, `13699`) are **not** used as a floor.

**[verified] Why the existing hysteresis does not stop it.** `aref_mpu_settle` only penalises two consecutive
landings that are within `AREF_MPU_SAME_PCT = 8 %` of each other (`13127`, `13359-13368`). The documented drift
steps are 115→95 (−17 %), 95→86 (−9.5 %), 86→77 (−10.5 %): each step is larger than 8 %, so `same` is false
every time, and `impr` is true because the new settled value is above the pre-trigger blur value
(`13358`). The hold/penalty path is never entered. This is a concrete, testable bug: the guard was designed for
a repeated *equal* landing, not a monotone staircase.

**[verified] Ratchet 2 — the Refocus test re-references itself.** If a trial ends below 90 % of `ref_s`
(`14203`), the next iteration calls `afrt_reference` (`14143`, `14208`), which lands with B07 and re-samples
`ref_s = median of 5` (`14098-14109`, `14008-14025`). There is no comparison with the previous `ref_s`; the
only reset is `afrt_state_reset` (`14036-14057`). A single bad re-land permanently lowers the bar for all later
trials, and *reporting* stays "ok x/5" — the number cannot distinguish "kept up with a 115k scene" from
"eventually passed against 77k".

**[verified] Smaller contributory erosions:**

- `afd_run`'s landing verify can lower the published landing by up to 10 %:
  `r = MIN(r, MAX(v, r * 9 / 10))` (`9345`), and that `r` is stored in `afd_land_s`/`afd_last_land_s`
  (`9349-9351`) which the aref adoption and E/local compare against.
- `afd_land_steps` "climb to top" accepts a half-step that is slightly below the best: if `s2` is lower than
  `best` but within 3 %, it sets `r = s2` and breaks (`8885-8898`) without stepping back. The comment says it
  steps back when it went past the top; the code only does so on a >3 % drop.
- `afd_run_local` stores the soft in-focus `se` as `afd_land_s` (`9255`), feeding the next comparison.

**[mechanism] Why the handover's "climb-to-top helped only partly" is expected:** the climb is a ~0-3 %
improvement mechanism applied after the reference has already been redefined; it cannot undo a 10-20 %
bookkeeping ratchet.

### 4.3 The "3 s → 6 s" speed regression

**[verified] Where the time goes in the current default (B, 07):**

- A tracked sweep command is at most 100 pulses at 07 (`AFD_CHUNK_07`, `7417`; capped again at `8679`).
- For long moves FOCUS_DONE is a ~510 ms timer, not the end of motion (`7414-7416`; handover:43-47).
  `afd_move_ex` exits its chunk wait as soon as `done >= 0 && elapsed >= min_ms`, and `afd_fast` makes
  `min_ms = 0` (`8097`, `8106`), so each 100-pulse chunk costs ~510 ms even though the modelled travel is
  only `120 + 100*2.9 = 410 ms` (`8117-8122`).
- A cold full-range B sweep allows up to 12 chunks (`AFD_SW_MAX_CHUNKS`, `8512`; `afd_sw_maxch`, `8980-8983`):
  12 × ~510 ms ≈ 6.1 s before the landing even starts. Then `afd_land_steps` adds 20-pulse steps (each a
  FOCUS_DONE wait + frame wait + sample) and can add a second retrace and the climb.
- `afd_run`'s 300 ms landing verify + possible re-land (`9326-9346`) adds ~0.4-0.8 s for every non-A/F
  algorithm, including the local search.
- `afd_engine`'s epilogue always adds ~1.5-2 s: stable sample, stop, 500 ms, sample, session close, 300 ms,
  sample, wait to 1000 ms, sample (`10264-10279`). Bench timing excludes it (`9792-9796` times `afd_run`
  only), while `AFDRIVE.LOG` `total_ms` and `aref_mpu_ms` include it (`10260`, `13213`). Field reports rarely
  say which clock they used.

**[speculation] Attribution of the 3→6 s shift to `fcc6f88`'s motion-end waits is not proven.** A plausible
alternative is that the median moved from short-move algorithms (A/local) to the B sweep, and/or that
"median AF time" includes the epilogue and landing verify. Without the old/new AFDRIVE logs this cannot be
settled; that is exactly why action 3 measures search time and total time separately.

**[verified] Model constants vs replay assumptions:** code uses `AFD_SW_MSPP_X10=29` (0.345 pulses/ms),
`AFD_SW_LAT_MS=120`, frame period measured; `af_replay/REPORT.md:26-36` simulates 0.40 pulses/ms, 42 ms lag,
30 ms start delay. The replay's speed ranking is therefore not a prediction of this build's timing.

### 4.4 Worse AF at 35-50 mm

**[verified] Only one parameter adapts to focal length.** `afd_fine_eff` returns 20/14/10 by
`lens_info.focal_len` (`7497-7507`). Everything else is in motor pulses and focal-length blind:

- local probes are `2*fine` = ±40/28/20 pulses (`9219-9222`), and acceptance is a fixed +5 % improvement
  (`AFD_L_IMPROVE_PCT`, `8553`);
- landing/retrace margins `AFD_SW_MARGIN 80`, `AFD_SW_ERR 100`, `AFD_RT_BACK 60`, `AFD_RT_SLACK 4`
  (`8530-8531`, `7431-7436`);
- D's Gaussian width is a single `AFD_SIGMA 21` (`8537`) fitted at 18-20 mm;
- absolute gates `AFD_MIN_S 2000`, `AFD_MIN_G 5000`, `aref_peak_min 30k/20k/16k`, `AREF_MIN_G 12000`
  (`7452`, `7582`, `13066`, `13149`) — all scene/zoom dependent and calibrated at one focal length.

**[mechanism]** A narrower depth of field and a narrower sharpness peak at 50 mm (fewer pulses between the
90 % flanks) make a ±20-pulse local probe capable of stepping over the peak: both sides can read no better
than `s0`, `dir` stays 0, and the in-focus exit accepts. At the same time the x5 preset on a 50 mm lens shows
a much narrower field, so the centre ROI can have less texture → lower `s` and `g`, interacting with the
absolute gates.

**[unverified]** Whether `lens_info.focal_len` is valid/updated under x5 crop presets — this determines
whether "Fine step: Auto" selects 20/14/10 at all. The engines log `focal_mm` in the header
(`10141`, `9736`), so a single session at 35/50 mm answers it from the log.

### 4.5 Recording path

**[verified]** Requirements and guards are explicit:

- `AFD_F_REC` refuses unless `RECORDING_RAW`, `AF drive monitor == Keep open`, and the monitor is already open
  (`10062-10067`);
- the persistent worker must already exist (`13240-13246`); Auto refocus posts only if `afd_wk_ok`
  (`13929-13959`);
- recording state changes abort the run (`7668-7671`, `7687-7702`);
- REC test task is created when armed, before recording (`10489-10607`), and refuses to arm while recording
  (`10591`);
- logs are held in RAM (`afd_log_pending`, `afd_log_hold`) and flushed when recording ends (`9559-9564`,
  `9589-9604`, `10560-10571`); monitor open/close is refused while recording (`10064-10065`, `10225-10238`,
  `10304-10310`, `14282-14315`).

**[verified gaps]** (a) true dropped-frame counts are unavailable by design — mlv_lite's counters are static,
so only vsync cadence is logged (`10550-10557`, comment `10421-10426`); (b) the epilogue still sends stop +
session close and blocks ~1.5-2 s while recording (`10262-10311`); the docs permit "no monitor operations",
not "no MPU traffic", but the LV-cadence effect of that block during recording is unmeasured; (c) the plain
AF-drive menu actions can `task_create` during recording before their precondition check (D16/R3).

### 4.6 FOCUS_DONE and motion modelling

**[verified] What is modelled:**

- `afd_move_ex`: chunk size by speed, wait for FOCUS_DONE (`afd_done_timeout=800 ms`), then a *minimum* travel
  time `AFD_MV_LAT_MS + |n| * mspp/10` (`8099-8122`). Because it is `min_ms=0` and the wait exits at done,
  the model only ever delays a move whose FOCUS_DONE arrived early; it never corrects an early done.
- at 1F the model predicts motion end at `120 + 300*1.0 = 420 ms`; the handover says the lens moves until
  ~590 ms (`HANDOVER:44-47`). That is an under-model of ~170 ms, but 1F is used only for **unmeasured**
  repositioning (`8080`), which is followed by the two-still-frame wait (`8127-8138`) — so measured sampling
  is protected by the stillness heuristic rather than the model.
- B's tracked sweep uses its own model: `t_exp = LAT + len*mspp/10`, `pos(t)` linear; end of motion = stop+40 ms
  or the first 3 stable frames after `t_exp` (`8682-8707`). Peak position and `afd_pos` are then adjusted by
  the estimated `moved` (`8705-8707`, `8751-8754`).
- Landings are always by sharpness feedback, never by an absolute computed position (`8839-8904`), which is the
  correct mitigation for a bad motion model.

**[verified] Internal contradiction:** if `(9,3,1)` really does not halt a `(9,1)` move (handover and menu
text), B's `end_t = stop_ms + 40` and the resulting `moved`/`pos_pk` are wrong by the un-stopped remainder;
the retrace starts `AFD_SW_MARGIN` (80) before the estimated peak and may not find it. The landing's flat-floor
detector and one re-sweep (`9065-9077`) are the only recovery. This is exactly what the AFSTOP test exists to
measure (`9972-9997`, `9922-9928`).

**[verified] End stop:** two consecutive chunks in one direction with FOCUS_DONE <150 ms and bit `0x1000`
(`7579-7580`, `7643-7654`), plus a flat-response fallback (`8194-8198`, `8720-8731`). On a declaration the
code gives the refused pulses back to `afd_pos` (`7651`, `8729`). If the declaration is a false positive, the
position bookkeeping is silently shifted; the range memory that would detect this is log-only (D11).

---

## 5. Risk register

Severity: **H** = can invalidate results or break recording, **M** = likely to waste a field session or lose
evidence, **L** = maintainability.

| ID | Risk | Where | Why / evidence | Sev |
|---|---|---|---|---|
| R1 | Reference ratchet: every soft but "successful" landing redefines the reference downward; local-search fixes cannot stop it | `aref_mpu_finish` `13744-13748`; `aref_mpu_settle` `13343-13375`; Refocus test re-ref `14203-14208` | 4.2; hysteresis missed because drops >8 % | H |
| R2 | Engine epilogue invariant is by convention, not construction: no cleanup label; any future early `return` after the session open (`10183`) leaks an open session/monitor and leaves the lens state to the next run | `afd_engine` `10046-10375` | Handover rule "keep it single-exit" (`HANDOVER:167-168`); ~300 lines between open and epilogue | H (maintenance) |
| R3 | Menu AF actions call `task_create` before checking recording — violates "never task_create during RAW recording" | `10729-10733` and the other select handlers `10686-10747`; check only at `10390` | Handover warns this broke mlv_lite/compress_task (`HANDOVER:159-164`) | H |
| R4 | Engine does not abort on Movie Servo / continuous AF; only Auto refocus gates on it | `afd_must_abort` `7687-7702`; `afd_task_common` `10390`; aref gate 4 at `13445` | Menu AF drive can run while Canon servo is active; undocumented interaction | M |
| R5 | The AF success pct is self-referential; operators and the cooldown logic treat ≥0 as success | `10213`, `10374`, `13744` | 4.1; a 5 % landing can be "success" if it is the run's best | H |
| R6 | FOCUS_DONE/motion model is wrong at 1F and unverified for 07's timer; B relies on a stop that the docs say does not halt | `7410-7419`, `8099-8122`, `8682-8707` | 4.6, D13; fix by measurement (AFSTOP/AFSWEEP), not by more constants | H |
| R7 | NODONE move path: when FOCUS_DONE times out, the remaining chunks are silently dropped and no stop is sent inside `afd_move_ex`; the motor may still be running while the caller samples/commands | `8112`, `8084-8123` | The comment says "do not send the rest on top of a running motor", but there is no stop/recovery until the epilogue | M |
| R8 | Logs can be silently truncated/dropped: `afd_note` 2048 B drops on overflow (`9570-9587`), `afd_buf` 24 KB with reserve 256 (`7457-7458`), bench 160 KB (`9642`) | `9544-9604` | Losing the only evidence is worse than a failed run; no drop counter in the note path | M |
| R9 | CPU starvation: `afd_measure` cost is only measured/logged, never bounded; a measurement taking 24-49 ms previously coincided with a Canon assert (`EOSM-AF.md:314-317`); task priority 0x1f and `msleep(8)` polling in `afd_move_ex` (`8108`) | `7843-7862`, `7420`, `8067-8210` | The invariant "measure is fast" is not enforced; full mode (sweep record) is much slower (`7785-7839`) | M |
| R10 | Focal-length blindness: all search/landing thresholds except fine step are pulse constants calibrated at 18-20 mm | 2.3 table, `7497-7507`, `8553-8560` | 4.4; user already reports worse 30-50 mm | H |
| R11 | Absolute sharpness gates are scene/zoom dependent and only measured once; `AFD_MIN_G 5000` cannot reject texture-less scenes (code says so) | `7582-7583`, `7452`, `13066`, `13149` | D14; "0 false triggers" is aref-gate-specific, not engine-wide | M |
| R12 | Bench and Refocus-test verdicts are relative and drift-aware but the drift flag/ref column is easy to ignore; "6/6" and "3/5" are not absolute-focus measurements | `9806-9818`, `9836`, `14196`, `14203-14208` | D18; the two regression metrics the handover relies on can both be gamed by reference drift | M |
| R13 | `lens_info.focal_len` used for Auto fine step may be invalid under crop presets | `7497-7507` | 4.4; one log line (`10141`) settles it | M |
| R14 | Prompt says no position feedback; all `afd_pos` bookkeeping is commanded-pulse accounting, corrected only on declared end stops and sweep-motion estimates. Any silent no-op or slip corrupts every later "goto" and landing start | `8067-8210`, `7651`, `8705-8707` | Trust in `afd_pos` is load-bearing; landings are feedback-based, which is the mitigation | M |
| R15 | `aref_abs_q/em` absolute fallback is uncalibrated for other scenes; it can fire on a low-detail sharp scene and burn an AF run | `13075-13076`, `13622-13627` | Doc says "guesses until calibrated" (`EOSM-AF.md:69,76-84`) but the table is stale | L/M |
| R16 | Restore epilogue clears `afd_aborted`/`afd_timeout` after a failed restore, so a timeout can be reported as a normal run | `10301-10302` | Only when `Session close = Restore`; masks a failed extra landing | L |
| R17 | Menu help still claims stop-based landing is not offered while B uses stop; future engineer may "fix" B by removing the stop and break its position model (or trust the model) | `10871`, `8442-8649` | D13 | M |

---

## 6. Top-5 prioritised actions

**A1 — Fix the reference ratchet at its source (one code change, no camera needed to reason about it).**
In `aref_mpu_settle` (`13343-13375`), before `aref_peak_clear()`, require the new settled sample to be
"verified": `med >= aref_trig_ref * 92 %` (or >= the pre-trigger reference), else keep the old peak and mark
the run as *not improved* rather than adopting `med`. Do the same for `afd_mpu_finish`'s decision to set
`aref_mpu_newref` (`13744-13748`): pass the engine's absolute landing-vs-reference result, not the
self-referential pct. Tighten `AREF_MPU_SAME_PCT`/add a "new ref must not be lower than the old ref by more
than X %" rule so a staircase cannot pass the 8 % gate. Add unit-style checks on the log fields
(`new_ref`, `land_s`, `pre`, `trig_ref`) so the next session proves the ratchet is gone. **This is the
prerequisite for A2**: a local-only fix leaves the next trigger reference soft.

**A2 — Land the local-search fix properly (one code change + one field session).**
The uncommitted WIP (wide ±4×fine probe, `AFD_L_SOFT_PCT`, `afd_ref_max_s`, trigger hint) is the right shape;
verify against HEAD behaviour rather than assuming it fixes 4.1: (a) the in-focus path must never accept when
the trigger path is "drop" or `smax < 92 %` of the per-zoom max; (b) the wide probe must be followed by B when
both wide sides are flat; (c) the engine must return an absolute confidence/landing value separate from
`final/best`, and `aref_mpu_finish` must use it. Acceptance: Refocus test 5/5 with `final_pct >= 90` for the
*original* reference and `false_trig = 0`, and a log line showing `ref_s` is not re-based after a failed trial.

**A3 — One field session to settle the motion model before changing any timing constant.**
Run, in one sitting at 18 mm, monitor Keep open, algorithm B, sweep speed 07:
(1) **AF sweep record** (`AFSWEEP.LOG`) at 07/14/1F — this gives the real speed and the real FOCUS_DONE
behaviour per speed; (2) **AF stop test** (`AFSTOP.LOG`) — answers whether `(9,3,1)` halts a tracked `(9,1)`
move, which D13 and B's position estimate depend on; (3) **AF drive** once with the epilogue instrumentation
(`# epilogue`/`# RESULT total_ms`) so "search time" and "total time" are separated. Do not tune
`AFD_MV_MSPP_*` before these logs exist; if the stop test shows `(9,3,1)` does not halt, fix B's
`moved/pos_pk` logic and remove the stop-based C path rather than patching constants.

**A4 — One field session at 35 mm and 50 mm to turn 4.4 from speculation into data.**
Same protocol as A3 plus **Refocus test** at each focal length, recording the header `focal_mm=` and
`fine=` lines, and **AF bench B speeds** at each. Decision rule: if `focal_mm` is valid and `final_pct` at
50 mm is materially lower than at 18 mm, replace the pulse constants with a generic "pulses per measured
sharpness-peak width": derive `sigma_s` per focal length from **AF drive calib** / sweep traces and scale
`fine`, `AFD_SW_MARGIN`, `AFD_SW_ERR`, local probe distances and `AFD_SIGMA` from it. If `focal_mm` is
invalid, fix the input first — no threshold tuning will help.

**A5 — Close the safety/observability gaps before the next recording test (one code change).**
(a) Move `task_create` out of every menu SELECT handler or guard it with the same "not recording"
precondition used by `afd_task_common` (R3, D16); (b) add `is_continuous_af()` to `afd_must_abort`/engine
preconditions (R4); (c) restructure `afd_engine` so the epilogue is reached via one cleanup path (no `return`
between session open and epilogue), and assert/log the monitor/session state at entry/exit (R2);
(d) send an explicit `(9,3,1)` and log the event when a move times out with no FOCUS_DONE (R7);
(e) add a dropped-note counter and print it in `# RESULT` (R8); (f) log `search_ms` and `total_ms` separately
in AFDRIVE.LOG (A3 depends on it).

---

## 7. Questions the next field session must answer (and what would falsify this review)

1. **Refocus test re-run with the reference columns:** does `ref_s` stay within a few percent across trials
   when no trial fails? If yes, the test's re-referencing is not the cause in that session; if no, 4.2's
   Ratchet 2 is confirmed. Track `RT_REF why=reland` lines and the `final_pct` per trial.
2. **Does `(9,3,1)` halt a tracked `(9,1)` move?** If it does halt, D13 is a doc bug and B's position model
   is fine; if it does not, B is leaning on the retrace and the re-sweep and the stop-based C path is
   unfixable as designed.
3. **Is `lens_info.focal_len` valid at 35/50 mm under x5?** The engine header already logs it; no code change
   needed to answer.
4. **Is the epilogue (~1.5-2 s) included in the "6 s" field numbers?** Compare `# RESULT total_ms` with the
   bench's search-only `median_ms`.
5. **Can the engine's own texture gate reject a blank wall?** Aim at a texture-less scene with Auto refocus
   OFF and run AF drive: the handover says the engine's low-texture abort is fixed (`HANDOVER:19`), but the
   code comment (`7582-7583`) says `g` cannot separate the two cases. If it cannot, that claim should be
   attributed to the aref gates only.

---

## Appendix — reproduction

```bash
# line references used in this review
git show HEAD:modules/crop_rec/crop_rec.c > /tmp/crop_rec_HEAD.c   # 15 740 lines
# working tree currently differs (another agent): do not use it for line refs
git diff --stat            # expect modules/crop_rec/crop_rec.c modified while the WIP lands
# offline simulator (ranking only; its assumptions differ from the code's model)
python3 contrib/af_replay/af_replay.py --logs <dir> --trials 300
```
