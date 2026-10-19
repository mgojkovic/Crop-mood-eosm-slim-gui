# contrib/af_novel

Offline prototypes for the "out-of-the-box" AF workstream (doc/EOSM-AF-NOVEL.md).
Standard library only, deterministic, no camera.

| file | what it does |
|---|---|
| `defocus_estimators.py` | Evaluates one-step / few-frame defocus estimators on the **real** sweep data in `samples/AFSWEEP.LOG`: single-frame multi-band absolute defocus, longitudinal 3-point dither, two-position differential band slope (leave-one-scene-out), and a synthetic edge-width/PSF check. |
| `focusfeed_probe.py` | Extract and summarise the `(9,0x17,32)` per-frame Canon AF data feed from an `AFDIAG.LOG` (or from `samples/focusfeed_AFDIAG_card.csv`), and print a first decoding protocol. |
| `analyze_focusfeed.py` | Deep mining of **all** recovered `AFDIAG*.LOG` files: dedup, payload-format classification, static/focus/crossing windows, per-field Spearman/permutation/partial-correlation tests with confound control. The verdict is in [doc/EOSM-AF-FOCUSFEED.md](../../doc/EOSM-AF-FOCUSFEED.md): no field is a usable defocus proxy from the existing logs. |
| `samples/AFSWEEP.LOG` | Real x5 sweep record (s1/s2/s4/g/q/band/luma per frame) recovered from the 2026-10-19 field-session copies. Preserved because the original scratchpad copies are temporary. |
| `samples/focusfeed_AFDIAG_card.csv` | First 12 bytes of every `(9,0x17,32)` message from the 2026-10-19 Refocus-test `AFDIAG.LOG`, plus the nearest sharpness sample. |

```
python3 contrib/af_novel/defocus_estimators.py
python3 contrib/af_novel/focusfeed_probe.py
python3 contrib/af_novel/focusfeed_probe.py --log /path/to/AFDIAG.LOG
```

Key numbers from the current run (see the scripts' own output):

- a single-frame `band = s1/(s2+s4)` cannot be an absolute defocus ruler across scenes:
  at a fixed |defocus| the cross-scene spread is comparable to its dynamic range;
- a **two-position** `d log(band)` estimator, with a globally calibrated slope and
  leave-one-scene-out validation, predicts the focus plane to ~10-20 pulses
  (do-nothing ~25 pulses) at d = 20-40 pulses - the best offline signal found;
- the 3-point parabola / log-Gaussian vertex on `s` is *worse* than doing nothing at
  the measured ~1 % reading noise unless points are averaged;
- an edge-width/PSF estimator saturates below ~0.5 px of blur, so it is coarse-only;
- `(9,0x17,32)` is a real per-frame feed with two payload formats (active and a
  constant default identical to the QEMU EOSM spell).  Mining all 26 unique logs
  found 3 204 records but **zero controlled raw speed-07 moves**, so no
  ground-truth defocus test is possible: field `A` (bytes 4..7) is brightness-linked
  (rho(A,luma)=+0.80), field `B` (bytes 8..11) is scene-contrast/sharpness-linked
  (rho(B,scene s)=+0.90), and `w/h` is a fluctuating 2:1 rectangle.
  No field qualifies as a defocus proxy.  See
  [doc/EOSM-AF-FOCUSFEED.md](../../doc/EOSM-AF-FOCUSFEED.md) for the full test and
  the fixed-exposure, known-move protocol that can still settle it.
