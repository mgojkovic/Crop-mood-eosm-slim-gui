# AF field test — 2026-10-19 session (patches 1–5)

**Card state:** flashed from `platform/EOSM.202/magiclantern-crop_mood.2026Oct19.EOSM202.zip`;
verified on the card: `ML/modules/crop_rec.mo` md5 `ebb45f8b53ba4cd7b53036ca5a8a4e8f`, matching the
reviewed build. Old logs/settings are backed up on the Mac in
`~/af_logs/preflash-20261019-1655/`; the card's old logs were moved to
`ML/LOGS/pre-flash-20261019/` so this session starts clean.

**This is the first field test of the reference/landing patch set.** The point is not a high score:
it is to see whether the patch does what the reviews say (no soft local landings, no reference
staircase, honest Refocus test) and to collect the motion-model data (stop, sweep, reversal).

---

## 0. Camera setup (2 minutes)

1. Card in, camera on. Confirm **ML loaded** (the ML menu opens; version line shows
   `crop_mood.2026Oct19`).
2. **Movie mode**, LiveView on, **x5** with the crop preset, **AF mode** (not MF), **not recording**.
   Lens at **18 mm**, textured static subject 0.5–2 m away, centred; tripod or a steady rest.
3. Settings → AF diag (values are boot defaults, but confirm):
   - **AF drive monitor: Keep open**
   - **AF algorithm: B**
   - **Sweep speed: 07**
   - **Session close: ON**
   - **Fine step: Auto**
4. Settings → Auto refocus: **Refocus method: MPU drive**, **Refocus AF: Local**,
   **sensitivity: High**, Auto refocus **ON** (ON+REC is fine, you are not recording).
5. **Movie Servo: OFF** (menu 2 / AF menu). Patch 2 deliberately aborts Auto-refocus MPU runs when
   the servo setting is on; with it off the runs proceed.
6. AF diag log: **ON** (AF diag menu) so AFDIAG/AFDRIVE are written.
7. Run **AF drive (MPU)** once. The first run opens the MPU monitor and the picture goes black for
   ~2 s. Wait for it to recover. Everything after this uses the open monitor.

---

## 1. Refocus test (the main regression measure, ~3 min)

Settings → AF diag → **Refocus test**. Put the camera down / do not touch it. It runs the reference
landing, a 20 s hold, five defocused trials (+60, −60, +150, −150, +400 pulses), a 10 s hold, and
writes `AFRTEST.LOG` + appends `AFDRIVE.LOG`.

**Pass criteria (new, patch-aware):**

| Field | Pass |
|---|---|
| `notrig` | **0** (all 5 trials actually triggered) |
| `valid` | **5/5** |
| `false_pass` | **0** |
| `ok` | **≥ 4/5** (target 5/5) |
| `false_trig` | **0** |
| local landings | no `# local in focus` / landing below 92 % of its `ref=` while `drop=1` |
| reference | no staircase in `# aref done ... anchor=` / `# RT_REF ref_s=`; `# aref ref anchor: raise` must not immediately follow `confirmed soft scene -> reset` |
| critical | no `# local: wide still flat -> B` on **every** trial, and no crash/assert |

If a trial fails, note the defocus value and whether the run was `path=local` or `path=full`; the
logs will tell the rest.

---

## 2. AF stop test (~2 min) — expected `halted=0`

Settings → AF diag → **AF stop test**. Keep the camera still. This is the measurement that decides
whether `(9,3,1)` can ever halt a running move.

**Expected (from the recovered 2026-10-19 traces):** every `# stop_check` line has **`halted=0`**
with |`slope_pct_s`| well above 2 %/s (8–400 %/s range). If you see `halted=1`, that is new evidence —
stop and report it before anything else; it reopens the fast-sweep algorithms.

---

## 3. AF sweep record (~3 min)

Settings → AF diag → **AF sweep record**. Keep the camera still. It runs one chunk at 07 and one at
1F in both directions and writes `AFSWEEP.LOG`. This fixes the time→position model and is the input
the simulator needs.

**Expected:** four `# sweep` blocks, `# done` values and sample counts; no crash. We are not tuning
thresholds yet — this is data.

---

## 4. Auto-refocus walk at 18 mm (~2 min)

With Auto refocus ON (MPU drive, Local), slowly point at **3 subjects at clearly different
distances** (e.g. 0.5 m, 1 m, 2 m), holding each for ~5 s. Then sweep back and forth between two
subjects a few times.

**Watch for / report:** how many refocuses per subject change, whether it lands sharp each time,
obvious hunting, and any visible latency spikes. The logs will show `# local ... wide probe`,
`# local: climb still soft -> B`, `# refmax`, and `# aref ref anchor`.

---

## 5. Optional: 35 mm and 50 mm (if you have time)

Set the lens to 35 mm, re-frame the same subject, and run **Refocus test** once; repeat at 50 mm.
This is the first focal-length data for the patch set. Note the header line's `focal_mm=` value in
`AFRTEST.LOG` — we still need to confirm `lens_info.focal_len` is valid under crop presets.

---

## 6. Finish and hand the logs back

1. Settings → AF diag → **AF drive: close monitor** (or just power off).
2. Power off, take the card out, put it in the Mac.
3. On the Mac, run the one-command checker (copies the logs and prints the verdicts):

```
cd /Users/mgojkovic/repos/Crop-mood-eosm-slim-gui
python3 contrib/af_test/field_session.py --copy
```

It copies the session to `~/af_logs/af-<timestamp>/`, prints the Refocus-test verdict, the
stop-check verdict, the drive timings, and the new log lines to look at. Send me that output (and the
directory path); I will add the session to the regression corpus and analyse anything unexpected.

**If something goes wrong:** camera freeze → pull the battery, note the last step; recording starts
by itself or stops → set Auto refocus to ON (not ON+REC) and note it; picture stays black after the
monitor open → wait 5 s, then check `AF drive monitor` is still Keep open.

---

## What we are specifically testing (do not "fix" these by hand)

- Soft-picture acceptance: can a local refocus end below 92 % of `ref=` while `drop=1`? (Should be
  impossible now; it falls back to B or wide-probes.)
- Reference integrity: does `# aref done ... anchor=` stay flat across soft landings, dropping only
  via `confirmed soft scene -> reset` / scene / zoom / focal-length / RAW / toggle resets?
- Refocus-test honesty: `notrig=0`, `valid=5`, `false_pass=0`; no more "ok" on a trial that never ran.
- Stop: `halted=0` (historical) — and if not, that changes the roadmap.
- Timing: `search_ms` vs `epilogue_ms` in `# RESULT`.
