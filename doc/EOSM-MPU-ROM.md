# EOS M MPU firmware (F74964A) — monitor, parser and message routes

Read-only reverse engineering of the dumped EOS M MPU ROM, branch `eosm-slim-gui`.

This document complements [`EOSM-MPU.md`](EOSM-MPU.md) (ICU-side protocol and the
`mpu_lab` probe). It corrects the first-pass MPU-ROM section in that document
(the command-table pairing there is shifted by one record) and records how the
MPU debug monitor is reached, plus the normal-message route that reaches the
`AF_LENS_SEARCH` handler.

**Scope.** Analysis only. Nothing here was run on the camera. The raw dumps and
tools live outside the repo in the session scratchpad:

```
.../scratchpad/card30/MPU-ROM.BIN   786432 bytes   canonical 0xC0000 image
.../scratchpad/card29/MPU-ROM.BIN   808960 bytes   extended image (same first 0xC0000)
.../scratchpad/re6/sh-objdump, full2.dis, lib.py, xref.py
```

---

## 1. Dump validation and memory map

### 1.1 Checksums

```
md5(card30/MPU-ROM.BIN) = 6a2dba300b4f6536a8643b1beb2fba77   (786432 bytes)
md5(card29/MPU-ROM.BIN) = 7dcbd11aef0610b571c6020b8426f05f   (808960 bytes)
card29[0:0xC0000] == card30[0:0xC0000]  (byte-for-byte; 0 differences)
```

`card30` is the canonical 0xC0000 image; `card29` only adds an unreadable/hanging
22,528-byte tail. Zero bytes: 55,408; `0xFF` bytes: 34,396; entropy ≈ 7.0.

### 1.2 Reset vectors and addressing

Big-endian SH2A, code linked at address 0 (file offset == CPU address for ROM).

| Offset | Value | Meaning |
|---|---|---|
| `0x00` | `00000060` | power-on reset PC = 0x60 |
| `0x04` | `FFF88000` | reset SP |
| `0x08` | `000063F4` | manual-reset PC = 0x63F4 |
| `0x0C` | `FFF88000` | manual-reset SP |

Reset path (reproducible from `full2.dis`):

```
0x0060  reset entry: clear 0xFFFD804C..0xFFFD8054, call 0x0AB0, 0x100C
0x01AA  call 0x02F4, then 0x6400, then spin at 0x01C0
0x6400  early init: optional 0x69FE, call 0x6448 (OS + tasks)
0x6448  set VBR=0x6000-0x10, set GBR, TBR, create RTOS tasks from
        descriptors at 0x127FC.. (see §4.1), then start scheduler
```

The 252-entry exception/vector table loaded at `VBR = 0x5FF0` has its first real
entry at `0x6000` (reset/exception stubs `0x3E680..`, default `0x0B10`).

### 1.3 RAM map used by the monitor / message path

| Address | Use | Evidence |
|---|---|---|
| `0xFFF80000` | command/status scratch block passed to normal handlers | `0x55DB6`, `0x6F550` |
| `0xFFF8256C` | VLENS command block (VLENS_SET / VLENS_DRIVE) | `0x6FCD6`, `0x6FD58` |
| `0xFFF825DA` | AF command mailbox written by `AF_LENS_SEARCH` | `0x337A0` |
| `0xFFF84CAA` | intercom (group,op) FIFO data, 20 entries × 2 bytes | `0x5571A` |
| `0xFFF84CD4` | FIFO head index | `0x55730` |
| `0xFFF84CD8` | FIFO tail index | `0x55722` |
| `0xFFF84CE4` | decoded {group,op} for the consumer | `0x504D6` |
| `0xFFF84CEB` | 32-byte copy of a group-8 payload | `0x552C2` |
| `0xFFFD8748` | monitor line buffer (60 bytes) | `0x71906`, `0x7193C` |
| `0xFFFD86D1` | console RX ring buffer (100 bytes) | `0x70F28` |
| `0xFFFD8740` / `0xFFFD873C` | RX ring head / tail | `0x70E06`/`0x70E3C` |
| `0xFFFD8738` | RX count | `0x70E3C` |
| `0xFFFD8784` | monitor token pointer array (60 entries) | `0x71904`, `0x719A4` |
| `0xFFFD8874` | monitor token count (`uint16`) | `0x7195E` |
| `0xFFFDA4C8` | last console char / getchar wake byte | `0x70E02` |

---

## 2. Monitor command table — corrected layout and full list

### 2.1 Layout

The monitor command table is **name-first**:

```
record (0x28 bytes, stride 0x28):
  +0x00 : char name[36]   NUL-padded command name
  +0x24 : uint32 handler  absolute handler pointer
terminator: all-zero record (first byte of name == 0)
```

The table starts at `0x147B0` (not `0x14824`). The dispatcher walks a list of
sub-table pointers at `0x1545C`:

```
0x1545C: 0x000147B0   main command table (49 entries, terminator 0x14F58)
0x15460: 0x00014650   TITLE / ABOUT / VER table
0x15464: 0x00014474   MDUMP / MDUMPB / MDUMPW / MDUMPL table
0x15468: 0x00000000
```

Why the old pairing was wrong: reading the same bytes as `[handler][name]`
starts one record too early. Two behavioural confirmations of the name-first
layout:

- record `RELEASE` (`0x148F0`) -> handler `0x6F448`, and `0x6F448` prints
  `"usage: release [<Tv> <Av> <Xout>]"` from `0x14F80` and parses 3 arguments.
- record `TVAFLOG` (`0x14C88`) -> handler `0x6F81A`, and `0x6F81A` requires
  `argc == 4`, calls `sscanf(...,"%lx")` three times and prints the usage strings
  at `0x150E8/0x15118/0x15144/0x1516C`.
- record `EXEC` (`0x148A0`) -> handler `0x6F526`: `sscanf(argv[1],"%lx")` then
  `handler_table_0x1417C[index](0xFFF80000)`.

The previous document's "TEST = 0x6F526" is the `EXEC` command. Real `TEST` is
`0x6F8FC`. Likewise "AF_LENS_SEARCH = 0x74D3E" is `CHECKEXTFLASH`; real
`AF_LENS_SEARCH` is `0x33794`.

### 2.2 Full main table (`0x147B0`, 49 records)

The first three records are S-record-shaped keys used by the factory update
loader (`S0` headers for `k331_mpu.mot`, `k269_eep.mot`, `LENS_EF...`); they can
only be produced by a host tool, not typed by a human.

| # | Name address | Name | Handler |
|---|---|---|---|
| 0 | `0x147B0` | `S00F00006B3333315F6D70752E6D6F74BF` (k331_mpu.mot) | `0x6F2E4` |
| 1 | `0x147D8` | `S00F00006B3236395F6565702E6D6F74CD` (k269_eep.mot) | `0x6EB7C` |
| 2 | `0x14800` | `S00F00004C454E535F4546...` (LENS_EF) | `0x713A6` |
| 3 | `0x14828` | `UPBNY` | `0x70574` |
| 4 | `0x14850` | `EEPW` | `0x6EB7C` |
| 5 | `0x14878` | `EEPR` | `0x6EAF4` |
| 6 | `0x148A0` | `EXEC` | `0x6F526` |
| 7 | `0x148C8` | `TEST` | `0x6F8FC` |
| 8 | `0x148F0` | `RELEASE` | `0x6F448` |
| 9 | `0x14918` | `SW` | `0x6FA02` |
| 10 | `0x14940` | `DISPALLONOLC` | `0x6FA12` |
| 11 | `0x14968` | `DISPALLOFF` | `0x6FA28` |
| 12 | `0x14990` | `DISPBLINKOLC` | `0x6FA88` |
| 13 | `0x149B8` | `DISPALLONILC` | `0x6FA42` |
| 14 | `0x149E0` | `DISPBLINKILC` | `0x6FA9E` |
| 15 | `0x14A08` | `DISPALLONSI` | `0x6FA58` |
| 16 | `0x14A30` | `CHECKBUZZER` | `0x6FAB4` |
| 17 | `0x14A58` | `CHECKSELFLED` | `0x6FAC6` |
| 18 | `0x14A80` | `SHRUN` | `0x6F55E` |
| 19 | `0x14AA8` | `SHREADY` | `0x6F586` |
| 20 | `0x14AD0` | `SHCHARGE` | `0x6F6B6` |
| 21 | `0x14AF8` | `SH1ST` | `0x6F71E` |
| 22 | `0x14B20` | `SH2ND` | `0x6F744` |
| 23 | `0x14B48` | `SHUTTEROPEN` | `0x6F790` |
| 24 | `0x14B70` | `MRUP` | `0x6F690` |
| 25 | `0x14B98` | `MRDOWN` | `0x6F76A` |
| 26 | `0x14BC0` | `MRCHARGE` | `0x6F6F8` |
| 27 | `0x14BE8` | `CHECKEFST2` | `0x1C152` |
| 28 | `0x14C10` | `CHECKCCC` | `0x1C4E2` |
| 29 | `0x14C38` | `CHECKEXTFLASH` | `0x74D3E` |
| 30 | `0x14C60` | `AF_LENS_SEARCH` | `0x33794` |
| 31 | `0x14C88` | `TVAFLOG` | `0x6F81A` |
| 32 | `0x14CB0` | `TVAFPRTSAF` | `0x6F8C6` |
| 33 | `0x14CD8` | `TEMPINFO` | `0x6F7B6` |
| 34 | `0x14D00` | `INDIINFO` | `0x6F7DE` |
| 35 | `0x14D28` | `MAINLOG` | `0x6F7E8` |
| 36 | `0x14D50` | `ABOUT_LENS` | `0x206FE` |
| 37 | `0x14D78` | `ON` | `0x6F8D6` |
| 38 | `0x14DA0` | `OFF` | `0x6F8E0` |
| 39 | `0x14DC8` | `SETDCDC` | `0x6FB38` |
| 40 | `0x14DF0` | `GETDCDC` | `0x6FBC0` |
| 41 | `0x14E18` | `GETCDCRESULT` | `0x6FB08` |
| 42 | `0x14E40` | `STATE` | `0x6FCCE` |
| 43 | `0x14E68` | `POWERLEDON` | `0x6FC9A` |
| 44 | `0x14E90` | `POWERLEDOFF` | `0x6FCB4` |
| 45 | `0x14EB8` | `VLENS_SET` | `0x6FCD2` |
| 46 | `0x14EE0` | `VLENS_DRIVE` | `0x6FDAC` |
| 47 | `0x14F08` | `SETENABLEGPS` | `0x6FB18` |
| 48 | `0x14F30` | `SETDISABLEGPS` | `0x6FB28` |
| — | `0x14F58` | terminator (all zero) | — |

The two other tables in the dispatch list:

| Table | Entries |
|---|---|
| `0x14650` | `TITLE` -> `0x6F334` (prints banner + `0x146F0` dashes); `ABOUT` -> `0x6F35E`; `VER` -> `0x6F35E` |
| `0x14474` | `MDUMP`, `MDUMPB`, `MDUMPW`, `MDUMPL` -> all `0x6EE74` (monitor memory dump; `0x6EE74` takes the suffix in the command name) |

Banner strings (plain ASCII, no code pointer to them):

```
0x14724  "\n             Debug Monitor (K269)              "
0x14750  "     MPU Ver...0x"
0x1476C  "  MPU check sum...0x"
0x147A0  "  EEP Ver...0x"
0x15470  "\nMON>>>"
```

### 2.3 Reproducing the table parse

```python
import struct
D = open('card30/MPU-ROM.BIN','rb').read()
a = 0x147B0
while True:
    name = D[a:a+36].split(b'\0')[0]
    h = struct.unpack('>I', D[a+0x24:a+0x28])[0]
    if not name: break
    print("0x%06X  %-24s  0x%06X" % (a, name.decode('ascii','replace'), h))
    a += 0x28
```

Disassembly in this document uses:

```
re6/sh-objdump -b binary -m sh2a -EB -D --start-address=0x... --stop-address=0x... card30/MPU-ROM.BIN
```

---

## 3. How the monitor is invoked, and its input source

### 3.1 Result: the text parser is serial-console-only

The command parser/dispatcher is reached **only from the monitor task's console
loop**. There is no group 7/8/9 message path, no `EXEC` path and no ICU intercom
path into the line parser. Evidence:

1. The line editor is `0x70A60` (60-byte buffer at `0xFFFD8748`). Its only call
   site in the image is `0x7193C`.
2. The tokenizer is `0x71CAC` (delimiter string at `0x1546C` = `" ,\t"`). Call
   sites: `0x71968` and `0x7198C` (both inside the monitor loop).
3. The command dispatcher is `0x71D2A` (and its record scanner `0x71D78`).
   Its only call site in the whole ROM is `0x719AC` (monitor loop).
4. The monitor loop (entry `0x718C4`, prompt at `0x15470`) is an RTOS task, not
   a message handler: it is RTOS task id 6, descriptor at `0x12888`, created
   unconditionally at boot (see §4.1). No message dispatcher enqueues work to
   it and no code jumps into it.
5. `EXEC` (`0x6F526`), the most powerful monitor command, does not call the
   parser: it calls `0x1417C[index](0xFFF80000)` directly (§5). It is itself
   only typed on the console.

Console input path:

```
UART RX ISR/console driver
        -> ring buffer 0xFFFD86D1 (100 bytes), head 0xFFFD8740, tail 0xFFFD873C
getchar   0x70DF0   (blocks until char; 0xFFFDA4C8 is the wake byte)
line editor 0x70A60 (echo via putchar 0x70DC4, CR/LF end, BS/DEL edit)
uppercase 0x71C6A
tokenizer 0x71CAC(delims " ,\t") -> token array 0xFFFD8784, count 0xFFFD8874
dispatcher 0x71D2A(argc, argv)
   for each table ptr in {0x147B0, 0x14650, 0x14474}:
       scan records (stride 0x28): compare token[0] with record name; on match
       return *(record+0x24) and jmp @handler with r4=argc, r5=argv
putchar 0x70DC4 -> 0x3C21E(channel=1, char)
```

`0x71C26` is the name comparison (byte compare; it accepts the table name as a
prefix of the typed token), so the first token selects the command and all
remaining whitespace-separated tokens are `argv[1..]`.

### 3.2 UART / console hardware (MPU side)

- The console is **channel 1** of a 16-channel serial block:
  `base = 0xFFFE8000 + (channel << 11)`, so channel 1 = **`0xFFFE8800`**
  (`0x3C21E`: `movi20 #-98304` = `0xFFFE8000`, then `channel << 11`).
- Register offsets used by the driver (`0x3C1BC..0x3C3xx` and boot init `0x0D10`):

| Offset | Use |
|---|---|
| `+0x00` | mode/format word; init writes `0x0080` |
| `+0x04` | control byte; init writes `0x06` |
| `+0x08` | control word; init clears bit 2, later `(x & ~0x30) | 0x30` |
| `+0x0C` | transmit data byte (`0x3C282`, `0x0DE6`) |
| `+0x10` | status/control word (masks `0x9F`, `0xCF`, tests bit 6) |
| `+0x14` | receive data byte (`0x0E0A`) |
| `+0x18` | interrupt/control word (`|= 6` at init) |
| `+0x1C` | status word (TX-ready bit `0x1000`; RX bits 0..4) |
| `+0x20` | divisor/config word; init writes **`0x005F`** |
| `+0x24` | init clears |
| `+0x102` | init writes **`0xFA`** |
| `+0x104` | init writes **`0x8282`** |

- Console init `0x0D10` also calls `0x028A(500)` (a 500-iteration delay after
  configuration).
- A second, separate status/flag block is used by the monitor and the factory
  update paths: `0xFFF85108` (mode byte: `1`/`2`/`7`...), `0xFFF85109` (flag
  byte, bits 0..3), `0xFFF8510A` (putchar status, bits 6/7), `0xFFF8510C`
  (monitor task event flag; bit 3 = console data), `0xFFF85110`
  (tokenizer cursor), `0xFFF85114/15/18` (console scratch).
- Console parameter block in ROM: **`0x15434`, bytes `05 07 01 00 00`**,
  copied by `0x70F38` into the SCI setup call at `0x0EF8`/`0x0E20` family.
  Patching this block changes the console format; it is one of the mechanisms
  used to enter the `fload`/`mload` S-record loader modes (`0xFFF85108` mode
  byte selects the message printed by `0x70C84`/`0x70CE2`).
- **Baud:** the service-connector UART path is analysed in the parallel
  [`EOSM-MPU-UART.md`](EOSM-MPU-UART.md) document (reported there as
  115200 8N1). On the MPU side the init constants above (`+0x20 = 0x5F`,
  `+0x104 = 0x8282`, mode `0x0080`) are the reproducible values; the exact
  divisor formula was **not** independently confirmed in this pass, so use the
  UART document for the baud claim and these constants for the MPU register
  state.
- Monitor prompt: `"\nMON>>>"` at `0x15470`. Other console strings:
  `"\nfload"` `0x15414`, `"\nmload"` `0x1541C`, `"Complete.\n"`, `"format
  error!!"`, `"E1OFF"` (`0x15474`..`0x15480`).

Typical console session (format inferred from the strings and handlers):

```
MON>>>TITLE
MON>>>ABOUT
MON>>>EXEC 10            ; hex index! calls 0x1417C[0x10]
MON>>>TVAFLOG 1 2 0     ; all arguments parsed with %lx (hex)
MON>>>EEPR 100 20       ; addr/len parsed with %lx
```

---

## 4. Normal-message route: group 8 / op 2 -> indexed handler

This is the important positive result: although normal intercom messages cannot
drive the text parser, the MPU's group-8 command queue exposes exactly the same
handler table that `EXEC` uses, so `AF_LENS_SEARCH` is reachable with a normal
ICU message.

### 4.1 Receive handler and queue

The intercom receive dispatcher is `0x54E82`. Its argument is the received
message buffer; the layout is `[transfer_size][group][op][payload...]`:

```
0x54E86  r2 = byte[msg+1]   ; group
0x54E8A  r6 = byte[msg+2]   ; op
0x54E98  group==3 && op==0x5B -> group=9, op=10 (alias)
...      switch on group: 0,1,2,3,4,5,8,9 are handled; group 7 is NOT handled
0x552B0  case group==8:
           if op==0 || op==1 || op==2:
               copy 32 bytes from msg+3 to RAM 0xFFF84CEB   (0x552C2..0x552DA)
           call 0x556D0(group, op)                          (enqueue, 0x552DC)
0x552E6  case group==9: ... (property channel, see EOSM-MPU.md)
```

Note: group **7** (COM_CPU) does not appear in this dispatcher at all, so it
cannot reach the group-8 queue or the indexed handler table.

FIFO: `0xFFF84CAA` 20 entries of 2 bytes `[group][op]`, head `0xFFF84CD4`,
tail `0xFFF84CD8`; enqueue `0x556D0` -> `0x55768`, pop `0x5572E`.

Consumer `0x503D2` pops one entry, stores group at `0xFFF84CE9` and op at
`0xFFF84CEA`. For **group 8** it calls `0x559E0(r4 = op)`:

```
0x50828  r0 = group
0x5082A  if group==5: return
0x50832  if group==8:
0x50836      r2 = 0x559E0
0x5083C      jsr @r2          ; r4 = op
```

`0x559E0` switches on op (`r4 & 0xFF`):

| op | meaning (as used by the monitor / MonRead-Write protocol) |
|---|---|
| 0 | memory/monitor operation type 0 (uses the 32-byte payload, builds a 52-byte request, `0x55A76`) |
| 1 | memory/monitor operation type 1 (payload word 0 selects 0/1/2 read/write sub-op, `0x559EA`/`0x55B8E`) |
| 2 | **execute indexed handler**: `index = be32(payload[4..7])`, call `0x1417C[index](0xFFF80000)`, reply |

### 4.2 The op-2 path in detail

```
0x55B4C  if op != 2: return
0x55B50  r1 = 0xFFF84CEB                      ; 32-byte payload copy
0x55B56  r4 = be32([0xFFF84CEB+4 .. +7])      ; big-endian index
0x55B72  bsr 0x55DA8
0x55DA8  r6 = 0x1417C
         r4 = r4 << 2
         r2 = *(r6 + r4)                      ; handler = 0x1417C[index]
         r4 = 0xFFF80000                      ; command/status block
         jsr @r2
         return handler return value in r0
0x55B7C  r4 = 2, r5 = handler return value
0x55B84  bsr 0x55E9C                          ; build + send 8-byte reply
```

`0x1417C` is a 128-entry table of 32-bit function pointers (normal firmware
handlers, all even addresses inside the ROM). Selected entries:

| index | handler | notes |
|---|---|---|
| 0 | `0x30E4C` | |
| 1 | `0x30F96` | |
| 2..15 | `0x32AEA`, `0x32B70`, `0x32B84`, `0x32B96`, `0x32BE4`, `0x32C60`, `0x32CAE`, `0x32CFC`, `0x33296`, `0x33572`, `0x335C6`, `0x335FC`, `0x3368E`, `0x33730` | |
| **16** | **`0x33794`** | **`AF_LENS_SEARCH`** (same pointer as monitor record 30) |
| 17 | `0x337DE` | group/op message handler (16-byte records, byte0=17) |
| 18 | `0x33896` | message handler (byte0=18) |
| 19 | `0x33A10` | |
| 20+ | `0x71892` (many) | default stub: writes `0` to `*r4`, returns 0 |
| 32..47 | `0x686BC`, `0x71892`, `0x6856C`, `0x685A0`, `0x68562`, `0x685EC`, `0x6861C`, `0x68562`, `0x68524`, `0x6890C`, `0x56964`, `0x569C6`, ... | |
| 64..79 | `0x1BFFE`, `0x1C000`, `0x1C002`, ... | AF/lens related low-level handlers |
| 80..127 | `0x56A04`, `0x57...`, `0x68D48`, `0x429F2`, ... | |

There is **no bounds check** on the index (`0x55DA8` just shifts and loads), so
only indices 0..127 are safe; anything larger is an arbitrary call.

Reply built by `0x55E9C` (8 bytes, sent through `0x66002`):

```
[0x08][0x08][0x02][BE32(handler return value)][0x00]
```

i.e. transfer size 8, group 8, op 2, 5 payload bytes.

### 4.3 Exact message to reach `0x33794` (AF_LENS_SEARCH)

`send(group=8, op=2, len=32, payload)` with:

```
transfer_size = (3 + 32 + 2) & 0xFE = 0x24
payload[0..3]  : not used by the op-2 path (recommend 00 00 00 00)
payload[4..7]  : big-endian index = 0x00000010 (16)
payload[8..31] : not used by the op-2 path (recommend 00)
```

Wire bytes (35 bytes, `24` = transfer size, then `group=08`, `op=02`,
then the 32-byte payload; payload bytes 4..7 are shown in bold):

```
24 08 02  00 00 00 00 00 00 00 10  00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00
             ^ payload[4..7] = 00 00 00 10 (index 16)
```

`modules/mpu_lab`'s raw sender can emit exactly this (or `send(8,2,32,data)`
from the ICUs packet builder at `0x38C4`, which computes the `0x24` size byte).
The group-8 receive handler has no visible "session opened" gate, but opening
the monitor once with Canon's `MpuMonOpen` before sending is prudent; the
receive path itself is `0x54E82` -> `0x556D0` -> consumer `0x503D2`.

Expected reply record: `08 08 02 <ret32> 00`; `0x33794` also writes the byte
`0x10` to `0xFFF80000` (its "result" out-parameter) and returns the value of its
last OS wait call in `r0`.

### 4.4 What `0x33794` actually does

```
0x33794  r13 = r4 (in the op-2 path r4 = 0xFFF80000)
0x3379C  r2 = 0xFFF825DA ; *r2 = 17
0x337A2  dly_tsk(5, 0x7FFFFFFF) family call via 0x9730
0x337B8  set-flag(5, 0x04000000) via 0x945C
0x337D0  wait-flag(5, 0x08000000, 1) via 0x71F4A
0x337D8  *(uint8*)r13 = 0x10          ; writes 0x10 to 0xFFF80000
```

So it takes **no input arguments** from the payload: `r4` is only an output
pointer. The meaningful side effect is the mailbox write `0xFFF825DA = 17`
(consumed by the AF/lens logic around `0x1E2A0`) followed by event-flag waits.
This is the `AF_LENS_SEARCH` action; payload bytes 0..3 and 8..31 are unused.

**Important convention trap.** Normal handlers in `0x1417C` are called with
`r4 = 0xFFF80000` (a status/command block pointer). Text-monitor handlers are
called with `r4 = argc`, `r5 = argv`. Typing `AF_LENS_SEARCH` in the monitor
therefore calls `0x33794` with `r4 = argc` (a small integer like 1), and the
final `*(uint8*)r4 = 0x10` becomes a write to ROM address 0x1..0x4. The
supported ways to run it with the correct argument are:

- ICU message: `(8, 2, 32)` with `payload[4..7] = 00 00 00 10` (this document);
- serial monitor: `EXEC 10` (hex index; `0x6F526` always passes
  `r4 = 0xFFF80000`).

Both use the same `0x1417C[16]` entry.

### 4.5 Group 8 op 0 / op 1 (monitor memory ops)

For completeness, the same queue carries the read/write operations:

- op 0 (`0x55A76`): payload is interpreted as three big-endian words
  `[w0][w1][w2]` plus data; calls `0x55CA4` and then sends a reply.
- op 1 (`0x559EA` -> `0x55B8E`): payload words `[type][addr][len]`:
  - type 0: copy `len` bytes from `addr` (normal address decoding via `0x65xxx`)
  - type 1: write bytes to `0xFFF80000 + addr` range
  - type 2: block operation via `0x3D11A`
  Results are returned in a reply record.
These correspond to the ICU-side `MpuMonRead`/`MpuMonWrite` (group 8) use.

---

## 5. `EXEC 10` is equivalent to the group-8 op-2 index 16 call

`EXEC` handler `0x6F526`:

```
0x6F526  sscanf(argv[1], "%lx", &index)        ; 0x14FE0 = "%lx"
0x6F53C  r0 = index
0x6F542  r1 = 0x1417C
0x6F54C  r5 = *(r1 + index*4)
0x6F550  r4 = 0xFFF80000
0x6F552  jsr @r5
```

Thus on the serial console `EXEC 10` (0x10 = 16, arguments are **hex**) runs
`0x33794` exactly like message `(8,2,32/{4..7}=0x10)`. `EXEC` can invoke any
entry of `0x1417C`, which is useful for probing indices 64..127 (AF/lens low
level) from the console without new firmware.

The monitor `TEST` command (`0x6F8FC`) is unrelated (it prints/controls
LVSTART/LVSTOP strings at `0x151A0`/`0x151A8`).

---

## 6. Exact argument formats of the AF/lens commands

All text-monitor handlers follow `handler(argc, argv)`. Numeric arguments are
parsed with `sscanf("%lx")` unless noted, i.e. **hexadecimal**.

### 6.1 `TVAFLOG` — handler `0x6F81A` (confirmed)

```
Usage: TVAFLOG <AllScanMode> <DriveLevel> <Direction>
```

- requires `argc == 4` (command + 3 args); otherwise prints the usage block at
  `0x150E8`, `0x15118`, `0x15144`, `0x1516C`;
- parses `argv[1]`, `argv[2]`, `argv[3]` with `%lx`;
- calls `0x84A18(arg1, arg2, arg3)`.

Usage text from the ROM:

```
0x150E9  Usage: TVAFLOG [0|1] [DriveLevel] [Direction]
0x15119    0|1 - AllScanMode OFF/ON (0:OFF, 1:ON)
0x15145    DriveLevel - 1:low, 2:middle, 3:high
0x1516D    Direction - Start direction (0:near, 1:far)
```

Ranges are documented by the ROM strings: AllScanMode 0/1, DriveLevel 1/2/3,
Direction 0(near)/1(far). `0x84A18` is the per-AF log control routine (writes
the internal AF log/scan configuration).

Monitor examples:

```
TVAFLOG 1 2 0     ; AllScanMode ON, drive level middle, direction near
TVAFLOG 0 1 1     ; AllScanMode OFF, level low, direction far
```

### 6.2 `AF_LENS_SEARCH` — handler `0x33794` (confirmed, no arguments)

See §4.4. There are no text arguments. The handler writes mailbox
`0xFFF825DA = 17`, performs the flag/delay sequence, and stores `0x10` at
`0xFFF80000`. It does not read `argv`.

- Correct invocation: §4.3 message, or `EXEC 10`.
- Typing `AF_LENS_SEARCH` in the monitor passes `argc` as the output pointer
  (do not use; see the convention trap in §4.4).

### 6.3 `VLENS_SET` — handler `0x6FCD2` (structure recovered; semantics inferred)

```
VLENS_SET <A> <B> <C> <d0> [<d1> ...]
```

- requires `argc >= 4` (command + 3), else prints usage/error string
  `"size err"` at `0x1523C`;
- parses `argv[1]`, `argv[2]`, `argv[3]` with `%lx`;
- rejects `argv[2] > 4`;
- parses up to `argv[2]` additional byte arguments (also hex) from `argv[3..]`;
- builds the block at **`0xFFF8256C`**:

| Offset | Value |
|---|---|
| `+0` | gate: bit 7 set by the handler (bit 6 used by VLENS_DRIVE) |
| `+1` | `argv[1]` (low byte) — command/parameter id |
| `+2` | `argv[2]` — byte count (0..4) |
| `+3` | `argv[3]` — destination offset/address byte |
| `+4..` | the parsed byte values `d0..` |

- The block is consumed by the lens command task around `0x302E0`/`0x30300`
  (`bld.b #7,@(0,0xFFF8256C)` at `0x30302`, `0x2EF80`, `0x2FBE8`): the gate bit
  is cleared, then for `argv[1]==2` the bytes are copied to
  `[argv[3] + i]` of the destination selected by the surrounding lens context.
- `VLENS_SET` itself does not call the lens command path: it only fills the
  block and raises the gate bit. This matches `VLENS_SET` = "stage command",
  `VLENS_DRIVE` = "stage + trigger".

### 6.4 `VLENS_DRIVE` — handler `0x6FDAC` (structure recovered; semantics inferred)

```
VLENS_DRIVE <b1> <b2> <b3> <b4> <b5> <b6> [<b7>]
```

- requires `argc >= 7` (command + 6), else prints `"size err"` at `0x1523C`
  and returns;
- clears/initialises the same block at `0xFFF8256C`:
  `+0 = 0`, `+1 = 2`, `+8..+11 = 0`, `+12..+14 = 0`;
- parses `argv[1..4]` with `%lx`, shifts them `<<24`, `<<16`, `<<8`, `<<0` and
  stores the resulting big-endian 32-bit value at **`+8`** (the lens position /
  drive value);
- parses `argv[5]`, `argv[6]`, `argv[7]` with `%lx` into bytes `+12`, `+13`,
  `+14` (note: the `argc >= 7` check permits reading `argv[7]` one past the
  terminator for exactly 6 arguments — supply 7 arguments, or treat `+14` as
  optional in practice);
- sets gate bit 6 (`bset.b #6,@(0,0xFFF8256C)`);
- if the hardware word at `0xFFF81B78` equals `0x1031`, additionally converts
  the `+8` value through the float path (`0x23318`, `0x83370` family) before
  returning;
- the lens command task then sees gate bit 6, clears it, and for `+1 == 2`
  calls `0x30BC4`, `0x30B56`, `0x30B30` with addresses inside `0xFFF825xx`
  and copies `+12..+14` to `0xFFF8253D..0x3F`.

The resulting block for `VLENS_DRIVE 0 0 0 0 0 0 0`, for example, is:

```
FF F8 25 6C: 40 02 00 00 00 00 00 00 00 00 00 00 00 00 00 00
             ^gate bit6, +1=2, +8 = 32-bit position, +12,+13,+14 = b5,b6,b7
```

The exact lens-side meaning of `+12..+14` (speed / direction / timeout) is
still inferred; see `EOSM-MPU.md` §4.1 for the ICU property `(9,0x13)` analogue.

### 6.5 Related commands found while tracing (for reference)

| Command | Handler | Behaviour |
|---|---|---|
| `CHECKEXTFLASH` | `0x74D3E` | `r15 -= 64`, call `0x1C1F8(buffer64)`, return `buffer[0]`; `0x1C1F8` tests hardware at `0xFFF83984` and runs external-flash checks |
| `ABOUT_LENS` | `0x206FE` | lens info print |
| `MAINLOG` | `0x6F7E8` | main-log helper |
| `EEPW` | `0x6EB7C` | EEPROM write entry (writes mode byte to `0xFFF85108`, goes through `0x6FF74`) |
| `EEPR` | `0x6EAF4` | EEPROM read entry (numeric address/len via `%lx`) |
| `UPBNY` | `0x70574` | update/binary upload entry (touches `0xFFF85108`, `0x7079C`) |
| `TITLE` | `0x6F334` | prints `0x146F0` dashes + `0x14724` banner + dashes |
| `ABOUT`/`VER` | `0x6F35E` | prints MPU/EEP version strings |
| `MDUMP*` | `0x6EE74` | monitor hex dump |
| `EXEC` | `0x6F526` | `0x1417C[hexindex](0xFFF80000)` |
| `TEST` | `0x6F8FC` | LVSTART/LVSTOP test helper (`0x151A0`, `0x151A8`) |
| `RELEASE` | `0x6F448` | `RELEASE [<Tv> <Av> <Xout>]` (3 `%lx` args) writes `0xFFF80000..2` |

---

## 6.6 Direct answers to the review questions

1. **Does the group-8 receive handler copy 32 bytes for op 2 and does the
   consumer pass op as `r4`?** Confirmed: `0x54E82` reads group at `msg+1` and op
   at `msg+2`; for group 8 and op 0/1/2 it copies `msg+3..msg+0x22` (32 bytes)
   to `0xFFF84CEB` (`0x552C2..0x552DA`) and enqueues `[group,op]` with
   `0x556D0`. The consumer `0x503D2` pops the pair and for group 8 calls
   `0x559E0(r4 = op)` (`0x50832..0x5083E`). For op 2 it reads
   `be32(0xFFF84CEB+4)` and calls `0x1417C[index](0xFFF80000)` (`0x55B4C`,
   `0x55DA8`). `0x1417C[16] = 0x33794`.
2. **Payload bytes 0..3 and 8..31:** unused by the op-2 path; only bytes 4..7
   are read as the big-endian index. Use zeroes.
3. **Does 0x33794 expect arguments in the 0xFFF80000 block?** No input
   arguments: it only writes the byte `0x10` to `*(uint8*)0xFFF80000` as a
   result/status and triggers the AF mailbox/flag sequence. `r4` is an output
   pointer, which is why the normal-route call (and `EXEC <hex index>`) is the
   correct way to invoke it.

---

## 7. Consequences for the ML probe (next steps)

1. **Do not try to reach the text monitor through the intercom.** No group 7/8/9
   message, and no `EXEC` path, feeds the line parser. The parser is a
   serial-console task only (§3).
2. **`AF_LENS_SEARCH` is reachable through a normal ICU message**:
   `(8, 2, 32)` with `payload[4..7] = 00 00 00 10` (§4.3). This is the same
   call as the monitor's `EXEC 10` and passes the correct `0xFFF80000` block.
3. **Index 16 is AF_LENS_SEARCH, not the only useful index.** `0x1417C[64..79]`
   are AF/lens low-level handlers (`0x1BFFE`, `0x1C000`, ...) and could be
   probed with the same message by changing `payload[4..7]` (all hex/big-endian)
   — but note the missing bounds check: only 0..127 are valid, and unknown
   entries execute the `0x71892` stub.
4. **TVAFLOG is the exact search-control command** and it is text-monitor only
   (`0x6F81A`), but it is a normal-format handler too; try to find an `0x1417C`
   index that points to `0x6F81A`/`0x84A18` before assuming it needs the console
   (index 21+ are message handlers; `0x6F81A` is not in the table in this ROM).
   The ICU property route in `EOSM-MPU.md` remains the alternative.
5. **Serial path remains available** over the service connector UART
   (channel 1, `0xFFFE8800`, 115200 8N1 per `EOSM-MPU-UART.md`); commands:
   `EXEC 10`, `TVAFLOG 1 2 0`, `VLENS_SET ...`, `VLENS_DRIVE ...`.
6. **Safety.** The normal-message `(8,2,32)` path with an in-range index only
   invokes read/status-style handlers; index 16 (`0x33794`) writes
   `0xFFF825DA=17` and flag registers, then a status byte. It does not write
   EEPROM or flash. Keep indices <= 127 and prefer index 16, 20..31.

---

## 8. Reproducibility index

| Item | Address / value |
|---|---|
| Reset PC / SP | `0x60` / `0xFFF88000` |
| VBR source table | `0x6000` (VBR=0x5FF0) |
| Bot init / task creation | `0x6400`, `0x6448`, task-create `0x860C` |
| Monitor task descriptor | `0x12888` (entry `+8 = 0x718C4`, id 6, stack 0x280, top `0xFFF87B78`) |
| Monitor loop / prompt | `0x718C4`, `0x15470` (`"\nMON>>>"`) |
| Line editor / getchar / putchar | `0x70A60` / `0x70DF0` / `0x70DC4` |
| Tokenizer / uppercase / dispatcher | `0x71CAC` / `0x71C6A` / `0x71D2A` |
| Record scanner / name compare | `0x71D78` / `0x71C26` |
| Dispatch sub-table list | `0x1545C` = `{0x147B0, 0x14650, 0x14474, 0}` |
| Command table delimiter string | `0x1546C` = `" ,\t"` |
| Command table / terminator | `0x147B0` / `0x14F58` |
| Intercom receive dispatcher | `0x54E82` (group at `msg+1`, op at `msg+2`, payload `msg+3`) |
| Group-8 payload copy | `0x552C2` -> `0xFFF84CEB` (32 bytes) |
| FIFO enqueue / pop | `0x556D0`/`0x55768` / `0x5572E` |
| Queue consumer / group-8 call | `0x503D2`, `0x50832` -> `0x559E0(op)` |
| op-2 branch / table call | `0x55B4C`, `0x55DA8` |
| Handler table | `0x1417C` (128 entries), `[16] = 0x33794` |
| Index source | `be32(0xFFF84CEB+4)` |
| Status block / result | `0xFFF80000` |
| AF mailbox | `0xFFF825DA` |
| VLENS block | `0xFFF8256C` |
| Console channel / base | 1 / `0xFFFE8800` (`0xFFFE8000 + ch<<11`) |
| Console init | `0x0D10`, param block `0x15434` |
| Console RX ring | `0xFFFD86D1`, head `0xFFFD8740`, tail `0xFFFD873C` |
| Serial line buffer | `0xFFFD8748` (60 bytes) |

### Disassembly helper

```bash
# full linear disassembly used for all snippets above
sh-objdump -b binary -m sh2a -EB -D card30/MPU-ROM.BIN > full2.dis
# one range
sh-objdump -b binary -m sh2a -EB -D --start-address=0x552b0 --stop-address=0x55300 \
           card30/MPU-ROM.BIN | tail -n +7
```

Literal-pool loads appear in `objdump` as `mov.l 0xADDR,rN ! VALUE`, so
`grep '! fff825da'` finds all users of a RAM variable and `grep '! 1417c'` the
handler-table users.
