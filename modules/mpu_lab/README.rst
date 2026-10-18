mpu_lab
=======

Temporary EOS M (2.0.2) MPU investigation tool.  Menu: **Focus -> MPU lab**.

- Dump memory: read-only dumps through Canon's MPU monitor to
  ``ML/LOGS/MPU-*.BIN``; stops on a MonRead error, logs byte counts, 00/FF counts
  and a checksum, and says if the dump looks empty.
- MPU command: one raw ICU->MPU message, with reply capture.
- MPU sequence: session / drive / search sequences, always ending with a focus
  stop and a session close.
- Every command logs the MPU->ICU replies to ``ML/LOGS/MPULAB.LOG``.

Needs an empty ``ML/SETTINGS/mpu_lab.en`` to load.
