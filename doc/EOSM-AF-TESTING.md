# EOS M MPU autofocus: field test protocol

This is a short, repeatable session. The camera runs the tests itself, so the tester only aims
and presses. One command on the Mac turns the card into a report.

## Before you start (once per session)

1. Camera: LiveView, movie mode, **x5** with your crop preset, **AF mode** (not MF), not recording.
2. Settings → AF diag menu:
   - **AF drive monitor: Keep open**
   - **AF algorithm: B**, **Sweep speed: 07**
   - **Session close: ON**
   - **Refocus method: MPU drive** (for steps 3 and 4)
3. Aim at a **textured, static subject** (a bookshelf or a printed page) 0.5–2 m away, centred.
   Use a tripod, or rest the camera on something.
4. Run **AF drive (MPU)** once. The first run opens the MPU monitor, and the picture goes dark
   for about 2 s.

## The tests (about 10 minutes)

| # | Menu item | What you do | Time |
|---|---|---|---|
| 1 | **AF bench**, Bench algos = B speeds (or A+B+D+F) | Nothing. Keep the camera still. | 4–6 min |
| 2 | **Refocus test** | Nothing. Keep the camera still. It counts false triggers, then defocuses the lens itself and times the recovery. | 2–3 min |
| 3 | Auto refocus ON (MPU drive) | Point at 3 subjects at clearly different distances. Hold each one for 5 s. | 1 min |
| 4 | Auto refocus **ON + REC**, **MPU drive while recording: ON** | Record RAW for about 30 s. Change the subject distance once. Then stop. | 1 min |
| 5 | Close the monitor (**AF drive: close monitor**) | Then take the card out. | |

Optional, at a different focal length: repeat test 1 or 2 at 35 mm and at 50 mm.

**If something goes wrong:** if the camera freezes, pull the battery and note the step. If
recording does not start or stops on its own, set **MPU drive while recording: OFF** and note it.

## On the Mac

```
python3 contrib/af_report/af_report.py            # reads /Volumes/EOS_DIGITAL
python3 contrib/af_report/af_report.py --copy ~/af_logs   # keep a timestamped copy
```

The report ends with verdict lines:

- **AF drive OK / NOT OK:** at least 90% of runs succeed, with a median of 5 s or less.
- **Auto refocus hunting:** more than 1 false trigger in the Refocus test, or more than 2
  triggers per minute.
- **Crash:** a new CRASH or ASSERT log on the card.

## What each log is

| Log | Written by |
|---|---|
| AFDRIVE.LOG | Every AF run (menu, Auto refocus, REC test), plus monitor, LV and Auto refocus notes |
| AFBENCH.LOG | AF bench |
| AFRTEST.LOG | Refocus test |
| AFSWEEP / AFSTOP / AFREV / AFCALIB / AFPROBE.LOG | Diagnostic tests (only when you run them) |
