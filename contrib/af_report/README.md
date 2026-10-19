# af_report

One-command summary of the EOS M ML AF logs. Python 3, standard library only.

    python3 contrib/af_report/af_report.py            # card at /Volumes/EOS_DIGITAL
    python3 contrib/af_report/af_report.py --logs DIR # a copied folder (flat, or with ML/LOGS)
    python3 contrib/af_report/af_report.py --all      # whole logs, not just the newest session
    python3 contrib/af_report/af_report.py --since 2h # or '30m', '2d', '2026-10-19 13:00'
    python3 contrib/af_report/af_report.py --copy ~/afcopies   # copy logs to ~/afcopies/<timestamp> first

- If the card is not mounted it tries `diskutil mount /dev/disk4s1` and says so.
- Default window: log files within 2 h of the newest log mtime.
- Crashes (CRASH*/ASSERT*.LOG) are "new" if newer than the previous run for the same source
  (stored in `~/.cache/af_report/state.json`; `--no-state` to ignore and not update).
- Sections print only when data exists: card, crashes, AF drive runs, Auto refocus
  (hunting index = triggers per LV minute), bench table, AFRTEST, recording, verdict.
- Verdict rules: AF drive OK if success >= 90% and median time <= 5 s; Auto refocus hunting
  if RTEST false_trig > 1 or hunting index > 2/min; CRASH if a new crash exists.
