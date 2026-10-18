mpu_lab
=======

Temporary EOS M (2.0.2) MPU investigation tool.  Menu: **Focus -> MPU lab**.

- Dump memory: read-only dumps through Canon's MPU monitor to
  ``ML/LOGS/MPU-*.BIN``; stops on a MonRead error, logs byte counts, 00/FF counts
  and a checksum, and says if the dump looks empty.
- MPU command: one raw ICU->MPU message, with reply capture.  Besides the
  group 9 lens/AF tests there are experimental group 8/7 probes.  **All of the
  group 8 EXEC entries are unverified on camera**.

  - ``8.2.32`` sends ``(8, 2, 32)``.  The MPU group 8 receive handler
    (``0x552B0``) copies the 32-byte payload to RAM ``0xFFF84CEB``; the
    consumer ``0x559E0`` then reads bytes 4..7 as a big-endian index into the
    128-entry handler table at ROM ``0x1417C`` and calls
    ``handler(index)`` with ``r4 = 0xFFF80000``.  ``0x1417C[16] = 0x33794``,
    which the corrected monitor command table (name first at ``0x147B0``,
    handler at ``+0x24``, stride ``0x28``) names ``AF_LENS_SEARCH``.
    Candidate payload: ``00 00 00 00 00 00 00 10`` + 24 zero bytes.
    The packet builder at ``0x38C4`` writes the size byte itself, so the wire
    bytes are ``24 08 02 00 00 00 00 00 00 00 10`` + 24 zero bytes (35 bytes).
    Expected reply record: ``08 08 02 <BE32 ret> 00``.  This is a raw group 8
    message rather than an ``0x8005xxxx`` property, so the REPLY lines in
    ``MPULAB.LOG`` do not cover it; the module logs an ``EXPECT`` line and AF
    diag "ON + MPU" records the outgoing bytes.  See
    ``doc/EOSM-MPU-ROM.md`` section 4.3 for the disassembly proof.
  - ``8.2.13`` is the same EXEC consumer using the ICU's 13-byte monitor
    payload layout ``[00 00 00 00][00 00 00 IDX][00 00 00 00][00]``.
  - Exposed indices: ``0x10`` AF_LENS_SEARCH, ``0x14`` a no-op rts stub
    (control), ``0x00`` a lens-command-family worker (control probe),
    ``0x0F`` a sibling of the AF handler, ``0x4C``/``0x4D`` rts stubs in the
    ``0x1C1xx`` AF code family, ``0x50`` an AF-family worker.  These are the
    only indices the module will send; it refuses anything else.
  - ``8.1`` sends Canon's own MpuMonRead request ``(8,1,13)``
    ``[kind][addr][size][00]`` and is read-only.
  - ``7.8`` sends ``(7,8,1)`` ``{00}``, a group 7 / COM_CPU probe.
  - ``(8,0)`` MpuMonWrite is intentionally not implemented (it writes MPU
    memory); the module has no EEPROM or MEMW commands.

  The EXEC entries have an extra 2 s confirmation delay and abort if recording
  starts or the menu is reopened.
- MPU sequence: session / drive / search sequences, always ending with a focus
  stop and a session close.  ``AF_LENS_SEARCH proxy`` is Canon's confirmed
  ``(9,0x25)`` + ``(9,0x13)`` search path.  ``8.2 idx16 + search`` first sends
  the unverified ``(8,2,32)`` AF_LENS_SEARCH probe, then the same proxy.
- Info entries ``AF_SEARCH path`` and ``TVAFLOG (serial only)`` send nothing;
  they record where the handlers live.  TVAFLOG is ``0x6F81A`` and needs
  ``argc==4`` + three hex args on the serial monitor, so no ICU message found
  so far reaches it.
- Every command logs every sent byte and the MPU->ICU replies to
  ``ML/LOGS/MPULAB.LOG``.

All raw sends are one-shot, delayed, and refused while recording.

Needs an empty ``ML/SETTINGS/mpu_lab.en`` to load.
