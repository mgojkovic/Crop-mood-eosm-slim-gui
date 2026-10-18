/** Temporary MPU investigation module for EOS M 2.0.2.
 *
 * The EOS M MPU is a separate Renesas F74964A (same protocol family as the
 * 5D3/6D F74965A).  This module does two things and nothing else:
 *
 *  1. read-only memory dumps through Canon's own MPU monitor:
 *       MpuMonOpen  0xFF1DD098
 *       MpuMonRead  0xFF1DD2B8   (kind, addr, size, buffer)
 *       MpuMonClose 0xFF1DD118
 *     The same functions are what mpu_dump uses through call("MonRead").
 *
 *  2. sends raw ICU -> MPU messages through the packet builder at RAM 0x38C4
 *     (grp, op, len, data).  The builder is the one already documented in
 *     crop_rec.c; this bypasses the property layer so arbitrary payloads can
 *     be tested, including the (9,0x13) lens search drive and (9,0x01) focus
 *     drive with Canon's speed byte.
 *
 *  3. EXPERIMENTAL: group 8 is the MPU monitor / factory-debug channel.  The
 *     ICU ROM functions prove the exact format (EOS M 2.0.2, ICU ROM):
 *
 *       MpuMonRead  (0xFF1DD2B8) -> (8, 1, 13) payload
 *           [kind:4 BE][addr:4 BE][size:4 BE][00]   (size is chunked to 16)
 *       MpuMonWrite (0xFF1DD474) -> (8, 0, 17+n) payload
 *           [kind:4 BE][addr:4 BE][size:4 BE][data:n][~sum(data):4 BE][00]
 *
 *     The MPU-side group 8 receive/consumer path (MPU ROM): 0x552B0 copies a
 *     32-byte payload to RAM 0xFFF84CEB and enqueues (group, subcommand);
 *     0x559E0 handles it (op 1 = read, op 0 = write, op 2 = EXEC/type 2).
 *     For op 2 it reads bytes 4..7 of the payload as a big-endian index into
 *     the 128-entry handler table at MPU ROM 0x1417C and calls
 *     handler(index) with r4 = 0xFFF80000.
 *
 *     0x1417C[16] = 0x33794.  The corrected monitor command table (name first
 *     at MPU ROM 0x147B0/+0, handler at +0x24, stride 0x28) gives
 *     AF_LENS_SEARCH = 0x33794, TVAFLOG = 0x6F81A (argc==4, three hex args,
 *     usage strings 0x150E8/0x15118/0x15144/0x1516C), EXEC = 0x6F526,
 *     VLENS_SET = 0x6FCD2, VLENS_DRIVE = 0x6FDAC, EEPR = 0x6EAF4,
 *     EEPW = 0x6EB7C, UPBNY = 0x70574, ABOUT_LENS = 0x206FE.
 *
 *     So (8, 2, 32) with payload[4..7] = 00 00 00 10 is the candidate
 *     "run AF_LENS_SEARCH" message (confirmed in MPU disassembly, unverified
 *     on camera).  The 0x38C4 builder writes the size byte itself: the wire
 *     bytes are 24 08 02 00 00 00 00 00 00 00 10 then 24 zero bytes (35 bytes
 *     total).  Expected reply record: 08 08 02 <BE32 ret> 00.  That reply is a
 *     raw group 8 message, not an 0x8005xxxx property, so the REPLY lines of
 *     MPULAB.LOG will not show it; the module logs an EXPECT line instead.
 *     The no-op control is index 0x14 = 0x71892 (an rts stub); index 0 =
 *     0x30E4C is a lens-command-family worker and is a probe as well.  Indices
 *     0x4C/0x4D = 0x1C610/0x1C612 are rts stubs in the 0x1C1xx AF family and
 *     0x50 = 0x1C554 is an AF-family worker.
 *
 *     The monitor text parser itself (0x71CAC/0x71D2A) is serial-console-only;
 *     TVAFLOG needs argc/argv and is therefore NOT reachable from any ICU
 *     message found so far.  AF_LENS_SEARCH (0x33794) is reachable only via
 *     the unverified (8,2) EXEC route above; (9,0x25)+(9,0x13) remains the
 *     search proxy.
 *
 * SAFETY: the dumps are read-only.  Raw messages are one-shot, delayed by
 * 1.5 s, and refused while recording.  Do not add EEPROM/MEMW experiments
 * here until the MPU ROM has been dumped and disassembled.
 *
 * Menu: Focus -> MPU lab (the slim GUI has no short way to Settings).
 * Every command/sequence step captures the MPU->ICU replies (properties
 * 0x800500xx) and appends a text record to ML/LOGS/MPULAB.LOG.  AF diag log
 * "ON + MPU" additionally records the outgoing messages in AFDIAG.LOG.
 * All module switches are OFF after every restart (no persistent CONFIG_INT).
 */
#include <module.h>
#include <dryos.h>
#include <stdint.h>
#include <string.h>
#include <property.h>
#include <menu.h>
#include <propvalues.h>
#include <lens.h>
#include <shoot.h>
#include <fio-ml.h>
#include <notify_box.h>
#include <timer.h>
#include <vram.h>
#include <focus.h>

#define MPU_MON_OPEN()  ((int (*)(void)) 0xFF1DD098)()
#define MPU_MON_CLOSE() ((int (*)(void)) 0xFF1DD118)()
#define MPU_MON_READ    ((int (*)(int, uint32_t, int, void *)) 0xFF1DD2B8)
#define MPU_SEND        ((int (*)(int, int, int, const void *)) 0x38C4)

/* ------------------------------------------------------------ log / capture */

#define LOG_PATH "ML/LOGS/MPULAB.LOG"

/* MPU -> ICU properties watched around each command.  Names that property.h
 * lacks (0x8005001E, 0x80050028) are raw numbers. */
#define MPL_LENS        0x80050000
#define MPL_FOCUS_DONE  0x80050002
#define MPL_CAF_STATE   0x8005001B
#define MPL_FPC         0x8005001E
#define MPL_FOCUS_CMD   0x80050027
#define MPL_P28         0x80050028
#define MPL_AF_RESULT   0x80050029
#define MPL_FOCAL_DIST  0x80050045

struct cap
{
    uint32_t prop;
    const char *tag;
    volatile int count;
    volatile int vlen;       /* length of the last value */
    uint8_t val[8];          /* first 8 bytes of the last value */
};

enum { CAP_LENS, CAP_DONE, CAP_CAF, CAP_FPC, CAP_CMD, CAP_P28, CAP_RESULT, CAP_DIST, CAP_N };

static struct cap caps[CAP_N] =
{
    { MPL_LENS,       "LENS" },
    { MPL_FOCUS_DONE, "FOCUS_DONE" },
    { MPL_CAF_STATE,  "CAF_STATE" },
    { MPL_FPC,        "FOCUS_PULSE" },
    { MPL_FOCUS_CMD,  "FOCUS_CMD" },
    { MPL_P28,        "P28" },
    { MPL_AF_RESULT,  "AF_RESULT" },
    { MPL_FOCAL_DIST, "FOCAL_DIST" },
};

static volatile int cap_active = 0;
static void drv_lvlens_note(const void *buf, unsigned int len);

/* Called from the property task: store only, no file I/O, no waiting. */
static void cap_note(int i, const void *buf, unsigned int len)
{
    if (!cap_active) return;
    struct cap *c = &caps[i];
    unsigned int n = len < sizeof(c->val) ? len : sizeof(c->val);
    memset(c->val, 0, sizeof(c->val));
    if (buf) memcpy(c->val, buf, n);
    c->vlen = len;
    c->count++;
}

PROP_HANDLER(MPL_LENS)       { cap_note(CAP_LENS, buf, len); drv_lvlens_note(buf, len); }
PROP_HANDLER(MPL_FOCUS_DONE) { cap_note(CAP_DONE, buf, len); }
PROP_HANDLER(MPL_CAF_STATE)  { cap_note(CAP_CAF, buf, len); }
PROP_HANDLER(MPL_FPC)        { cap_note(CAP_FPC, buf, len); }
PROP_HANDLER(MPL_FOCUS_CMD)  { cap_note(CAP_CMD, buf, len); }
PROP_HANDLER(MPL_P28)        { cap_note(CAP_P28, buf, len); }
PROP_HANDLER(MPL_AF_RESULT)  { cap_note(CAP_RESULT, buf, len); }
PROP_HANDLER(MPL_FOCAL_DIST) { cap_note(CAP_DIST, buf, len); }

static char log_line[1600];     /* only used from the (single) busy task */
static int log_len = 0;

static const char *log_path = LOG_PATH;   /* the drive test redirects this */

static void log_flush(void)
{
    if (!log_len) return;
    FILE *f = FIO_CreateFileOrAppend(log_path);
    if (f)
    {
        FIO_WriteFile(f, log_line, log_len);
        FIO_CloseFile(f);
    }
    log_len = 0;
}

static void log_add(const char *fmt, ...);

/* Read a 32-bit word from the MPU through Canon's monitor.  Tries kind 0..2
 * because the EOS M RAM kind is not documented; logs each attempt. */
static volatile uint32_t exp_shutter, exp_aperture, exp_iso, exp_bv;
static volatile uint32_t exp_lvstate, exp_dispsize;

PROP_HANDLER(PROP_SHUTTER)   { if (buf && len >= 4) exp_shutter   = *(uint32_t *)buf; }
PROP_HANDLER(PROP_APERTURE)  { if (buf && len >= 4) exp_aperture  = *(uint32_t *)buf; }
PROP_HANDLER(PROP_ISO)       { if (buf && len >= 4) exp_iso       = *(uint32_t *)buf; }
PROP_HANDLER(PROP_LV_BV)     { if (buf && len >= 4) exp_bv        = *(uint32_t *)buf; }
PROP_HANDLER(PROP_LV_STATE)  { if (buf && len >= 4) exp_lvstate   = *(uint32_t *)buf; }
PROP_HANDLER(PROP_LV_DISPSIZE) { if (buf && len >= 4) exp_dispsize = *(uint32_t *)buf; }

static void mpu_log_exposure(const char *tag)
{
    log_add("%d EXP %s shutter=%x aperture=%x iso=%x bv=%x lvstate=%x dispsize=%x\n",
            get_ms_clock(), tag, exp_shutter, exp_aperture, exp_iso,
            exp_bv, exp_lvstate, exp_dispsize);
}

static void mpu_log_read(const char *tag, uint32_t addr)
{
    uint8_t b[8];
    for (int kind = 0; kind <= 2; kind++)
    {
        memset(b, 0, sizeof(b));
        int e = MPU_MON_READ(kind, addr, 4, b);
        log_add("%d MON %s kind=%d addr=%x err=%d val=%02x%02x%02x%02x\n",
                get_ms_clock(), tag, kind, addr, e, b[0], b[1], b[2], b[3]);
        if (!e) break;
    }
}

static void log_add(const char *fmt, ...)
{
    va_list ap;
    if (log_len > (int) sizeof(log_line) - 200) log_flush();
    va_start(ap, fmt);
    int n = vsnprintf(log_line + log_len, sizeof(log_line) - log_len, fmt, ap);
    va_end(ap);
    if (n > 0 && n < (int) sizeof(log_line) - log_len) log_len += n;
}

static void hex_str(char *out, int out_size, const uint8_t *b, int n)
{
    int o = 0;
    out[0] = 0;
    for (int i = 0; i < n && o + 4 < out_size; i++)
        o += snprintf(out + o, out_size - o, i ? " %02X" : "%02X", b[i]);
}

/* One capture window: cap_begin, one or more cap_send, wait, cap_end. */
static char win_sent[160];
static char win_ret[40];
static int win_dist0;
static int win_t0;

static void cap_begin(void)
{
    for (int i = 0; i < CAP_N; i++)
    {
        caps[i].count = 0;
        caps[i].vlen = 0;
        memset(caps[i].val, 0, sizeof(caps[i].val));
    }
    win_sent[0] = 0;
    win_ret[0] = 0;
    win_dist0 = lens_info.focus_dist;
    win_t0 = get_ms_clock();
    cap_active = 1;
}

static int cap_send(int grp, int op, int len, const void *data)
{
    char h[64];
    hex_str(h, sizeof(h), data, len > 16 ? 16 : len);
    int ret = MPU_SEND(grp, op, len, data);
    int o = strlen(win_sent);
    snprintf(win_sent + o, sizeof(win_sent) - o, "%s%02X.%02X.%02X:%s",
             o ? " | " : "", grp, op, len, h);
    o = strlen(win_ret);
    snprintf(win_ret + o, sizeof(win_ret) - o, "%s%d", o ? "," : "", ret);
    return ret;
}

static void cap_end(const char *name, const char *seq, int step, int notify)
{
    cap_active = 0;
    int dist1 = lens_info.focus_dist;

    log_add("%d CMD name=%s seq=%s step=%d sent=[%s] ret=%s dist=%d->%d win=%dms\n",
            win_t0, name, seq ? seq : "-", step, win_sent, win_ret,
            win_dist0, dist1, get_ms_clock() - win_t0);
    for (int i = 0; i < CAP_N; i++)
    {
        char h[40];
        int n = caps[i].vlen < (int) sizeof(caps[i].val) ? caps[i].vlen : (int) sizeof(caps[i].val);
        hex_str(h, sizeof(h), caps[i].val, n);
        log_add("%d REPLY name=%s prop=%x tag=%s n=%d len=%d last=[%s]\n",
                win_t0, name, caps[i].prop, caps[i].tag, caps[i].count, caps[i].vlen, h);
    }
    log_flush();

    if (notify)
        NotifyBox(5000, "%s: done:%d fpc:%d cmd:%d dist %d->%d", name,
                  caps[CAP_DONE].count, caps[CAP_FPC].count, caps[CAP_CMD].count,
                  win_dist0, dist1);
}

static int mpu_lab_refuse(void)
{
    if (!is_camera("EOSM", "2.0.2"))
    {
        NotifyBox(4000, "MPU lab: EOS M 2.0.2 only");
        return 1;
    }
    if (RECORDING)
    {
        NotifyBox(4000, "MPU lab: not while recording");
        return 1;
    }
    return 0;
}

static volatile int mpu_lab_busy = 0;

/* ------------------------------------------------------------------ dumps */

struct mpu_dump
{
    int kind;
    uint32_t addr;
    int size;
    const char *file;
};

static const struct mpu_dump mpu_dumps[] =
{
    { 0, 0x00000000,  768 * 1024, "MPU-ROM.BIN" },  /* F74964A ROM: code ends at 0xC0000 (FF after); reading past ~0xC5800 hangs the monitor */
    { 2, 0x00000000,    64 * 1024, "MPU-EEP.BIN" }, /* MPU EEPROM */
    { 0, 0xFFFF8000,    24 * 1024, "MPU-RAM.BIN" }, /* TX19A RAM address; harmless probe */
    { 0, 0x00000000,     4 * 1024, "MPU-TEST.BIN" } /* quick smoke test */
};

static const char * mpu_dump_choices[] =
{
    "ROM 768 KB", "EEPROM 64 KB", "RAM 24 KB", "test 4 KB"
};

static int mpu_dump_choice = 0;

static void mpu_lab_dump_task(int idx)
{
    if (mpu_lab_busy) return;
    mpu_lab_busy = 1;

    if (mpu_lab_refuse()) goto out;
    if (idx < 0 || idx >= COUNT(mpu_dumps)) goto out;

    const struct mpu_dump *d = &mpu_dumps[idx];
    char path[64];
    snprintf(path, sizeof(path), "ML/LOGS/%s", d->file);

    NotifyBox(3000, "MPU dump %s starts in 2 s", d->file);
    msleep(2000);

    FILE *f = FIO_CreateFile(path);
    if (!f)
    {
        NotifyBox(5000, "MPU dump: cannot create %s", path);
        goto out;
    }

    int t0 = get_ms_clock();
    int e = MPU_MON_OPEN();
    if (e)
    {
        NotifyBox(5000, "MPU dump: MonOpen failed (%d)", e);
        log_add("%d DUMP file=%s kind=%d addr=%x req=%d result=MonOpen_err(%d)\n",
                t0, d->file, d->kind, d->addr, d->size, e);
        log_flush();
        FIO_CloseFile(f);
        goto out;
    }

    static uint8_t buf[1024];
    int got = 0, n_zero = 0, n_ff = 0, err = 0, err_off = 0;
    uint32_t sum = 0;
    for (int off = 0; off < d->size; off += (int) sizeof(buf))
    {
        int n = d->size - off;
        if (n > (int) sizeof(buf)) n = (int) sizeof(buf);
        memset(buf, 0, sizeof(buf));
        int r = MPU_MON_READ(d->kind, d->addr + off, n, buf);
        if (r)
        {
            err = r;
            err_off = off;
            NotifyBox(6000, "MPU dump: MonRead err %d at 0x%x", r, off);
            break;
        }
        if (FIO_WriteFile(f, buf, n) != n)
        {
            err = -1;
            err_off = off;
            NotifyBox(6000, "MPU dump: write error at 0x%x", off);
            break;
        }
        for (int i = 0; i < n; i++)
        {
            if (buf[i] == 0x00) n_zero++;
            else if (buf[i] == 0xFF) n_ff++;
            sum += buf[i];
        }
        got += n;
        if ((off & 0xFFFF) == 0)
            NotifyBox(250, "MPU dump %d%%", off * 100 / d->size);
        msleep(10);
    }

    MPU_MON_CLOSE();
    FIO_CloseFile(f);

    int empty = (got == 0) || (n_zero == got) || (n_ff == got);
    log_add("%d DUMP file=%s kind=%d addr=%x req=%d got=%d zero=%d ff=%d sum=%x empty=%s result=%s err=%d@%x ms=%d\n",
            t0, d->file, d->kind, d->addr, d->size, got, n_zero, n_ff, sum,
            empty ? "YES" : "no", err ? "ERROR" : "ok", err, err_off, get_ms_clock() - t0);
    log_flush();

    if (!err)
        NotifyBox(7000, "MPU dump done: %s %d B%s", d->file, got,
                  empty ? " - EMPTY (all 00/FF)" : "");
    else if (got)
        NotifyBox(7000, "MPU dump partial: %d B%s", got, empty ? " (empty)" : "");

out:
    mpu_lab_busy = 0;
}

static void mpu_lab_dump_select(void *priv, int delta)
{
    (void) priv;
    (void) delta;
    run_in_separate_task(mpu_lab_dump_task, mpu_dump_choice);
}

/* ------------------------------------------------------------- raw commands */

#define MPU_CMD_DATA   32

struct mpu_cmd
{
    uint8_t grp;
    uint8_t op;
    uint8_t len;
    uint8_t data[MPU_CMD_DATA];
    uint8_t stop_after;    /* send (9,3,1) after the settle time */
    const char *name;
    uint16_t win_ms;       /* capture/settle window; 0 = default */
};

/* Payload notes:
 *  - (9,0x13,4) LensSearchDriveStart: forum RE says [?, end, speed, timeout].
 *    Both 0 and 1 in the first byte are tried; no firmware sender exists.
 *  - (9,0x01,5) focus drive: a1ex's working 5D3 message is [hi lo 07 00 00],
 *    0x07 = driving speed.  ML's property path always sends [hi lo FF FF 00].
 *  - (9,0x25,16) AF session: Canon EOS M sends [02 00 ...] on half-press.
 *  - (9,0x0B,2) is PROP_LV_FOCUS_STATE, not AF_RESULT when sent to the MPU.
 *  - (8,1,13) is Canon's own MpuMonRead request: [kind][addr][size][00], all
 *    32-bit big-endian (see the header comment).  Read-only.
 *  - (8,2,32) is the candidate factory-debug EXEC message that reaches the
 *    MPU group 8 consumer: the 32-byte payload is copied to RAM 0xFFF84CEB,
 *    and the big-endian dword at payload[4..7] is the 0x1417C handler index.
 *    0x1417C[16] = 0x33794 = the corrected AF_LENS_SEARCH handler.  The
 *    (8,2,13) variants use the same consumer with the ICU's 13-byte layout.
 *    Exposed indices: 0x00 (0x30E4C lens-family worker), 0x0F (0x33730),
 *    0x10 (0x33794 AF_LENS_SEARCH), 0x14 (0x71892 no-op stub control),
 *    0x4C/0x4D (0x1C610/0x1C612 AF-region rts stubs), 0x50 (0x1C554
 *    AF-family worker).  UNVERIFIED ON CAMERA - all experimental.
 *  - (7,8,1) is the assistmpu/COM_CPU "finish" byte, a low-risk group 7 probe.
 */
#define X8_EXEC(IDX) \
    { 0, 0, 0, 0, 0, 0, 0, (IDX), 0, 0, 0, 0, 0 }
#define X8_EXEC32(IDX) \
    { 0, 0, 0, 0, 0, 0, 0, (IDX), 0, 0, 0, 0, 0, 0, 0, 0, \
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }

static const struct mpu_cmd mpu_cmds[] =
{
    { 9, 0x13, 4, { 0x00, 0x01, 0x07, 0x00 }, 1, "search end1 spd7" },
    { 9, 0x13, 4, { 0x01, 0x01, 0x07, 0x00 }, 1, "search flag1 end1" },
    { 9, 0x13, 4, { 0x00, 0x00, 0x07, 0x00 }, 1, "search end0 spd7" },
    { 9, 0x13, 4, { 0x01, 0x00, 0x07, 0x00 }, 1, "search flag1 end0" },
    { 9, 0x13, 4, { 0x00, 0x02, 0x07, 0x00 }, 1, "search end2 spd7" },
    { 9, 0x13, 4, { 0x00, 0x01, 0x00, 0x00 }, 1, "search end1 spd0" },
    { 9, 0x01, 5, { 0x00, 0x01, 0x07, 0x00, 0x00 }, 1, "focus +1 spd7" },
    { 9, 0x01, 5, { 0x00, 0x0F, 0x07, 0x00, 0x00 }, 1, "focus +15 spd7" },
    { 9, 0x01, 5, { 0x00, 0x32, 0x07, 0x00, 0x00 }, 1, "focus +50 spd7" },
    { 9, 0x01, 5, { 0xFF, 0xF1, 0x07, 0x00, 0x00 }, 1, "focus -15 spd7" },
    { 9, 0x01, 5, { 0xFF, 0xCE, 0x07, 0x00, 0x00 }, 1, "focus -50 spd7" },
    { 9, 0x25, 16, { 0x02 }, 0, "session 02" },
    { 9, 0x25, 16, { 0x00 }, 0, "session 00" },
    { 9, 0x25, 16, { 0x01 }, 0, "session 01" },
    { 9, 0x03, 1, { 0x00 }, 0, "focus stop" },
    { 9, 0x0B, 2, { 0x00, 0x00 }, 0, "focus state 0" },
    { 9, 0x0B, 2, { 0x02, 0x00 }, 0, "focus state 2" },

    /* Experimental group 8 / group 7 probes.  Everything below is UNVERIFIED
     * on camera; the (8,2) entries reach the MPU group 8 EXEC consumer and
     * call an MPU handler by index.  Keep all of them one-shot. */
    { 8, 0x02, 32, X8_EXEC32(0x10), 0, "E 8.2.32 AF_SEARCH idx16", 6000 },
    { 8, 0x02, 32, X8_EXEC32(0x10), 0, "AF search (MPU) 8s", 8000 },
    { 8, 0x02, 32, X8_EXEC32(0x10), 0, "AF search (MPU) 2s + stop", 2000 },
    { 8, 0x02, 32, X8_EXEC32(0x14), 0, "E 8.2.32 no-op ctl idx20" },
    { 8, 0x02, 32, X8_EXEC32(0x00), 0, "E 8.2.32 lens? ctl idx0" },
    { 8, 0x02, 13, X8_EXEC(0x10), 0, "E 8.2.13 AF_SEARCH idx16" },
    { 8, 0x02, 13, X8_EXEC(0x14), 0, "E 8.2.13 no-op ctl idx20" },
    { 8, 0x02, 13, X8_EXEC(0x0F), 0, "E 8.2.13 idx15 0x33730" },
    { 8, 0x02, 13, X8_EXEC(0x4C), 0, "E 8.2.13 AFstub idx4C" },
    { 8, 0x02, 13, X8_EXEC(0x4D), 0, "E 8.2.13 AFstub idx4D" },
    { 8, 0x02, 13, X8_EXEC(0x50), 0, "E 8.2.13 AFfam idx50" },
    { 8, 0x01, 13, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0 }, 0,
      "E 8.1 MonRead raw @0" },
    { 7, 0x08, 1, { 0x00 }, 0, "E 7.8 COM_CPU finish" }
};

static const char * mpu_cmd_choices[] =
{
    "search end1 spd7", "search flag1 end1", "search end0 spd7",
    "search flag1 end0", "search end2 spd7", "search end1 spd0",
    "focus +1 spd7", "focus +15 spd7", "focus +50 spd7",
    "focus -15 spd7", "focus -50 spd7",
    "session 02", "session 00", "session 01",
    "focus stop", "focus state 0", "focus state 2",
    "E 8.2.32 AF_SEARCH idx16", "AF search (MPU) 8s",
    "AF search (MPU) 2s + stop",
    "E 8.2.32 no-op ctl idx20",
    "E 8.2.32 lens? ctl idx0",
    "E 8.2.13 AF_SEARCH idx16", "E 8.2.13 no-op ctl idx20",
    "E 8.2.13 idx15 0x33730",
    "E 8.2.13 AFstub idx4C", "E 8.2.13 AFstub idx4D",
    "E 8.2.13 AFfam idx50",
    "E 8.1 MonRead raw @0", "E 7.8 COM_CPU finish"
};

static int mpu_cmd_choice = 0;

static void mpu_lab_cmd_task(int idx)
{
    if (mpu_lab_busy) return;
    mpu_lab_busy = 1;

    if (mpu_lab_refuse()) goto out;
    if (idx < 0 || idx >= COUNT(mpu_cmds)) goto out;

    const struct mpu_cmd *c = &mpu_cmds[idx];

    /* Strict bounds for the group 8 probes: the EXEC index is the second
     * 32-bit big-endian payload word and must be one of the handlers we have
     * reviewed in the MPU ROM.  Nothing outside this list can be sent, even if
     * the table is edited carelessly. */
    uint32_t exec_index = 0;
    int is_exec = 0;
    if (c->grp == 8)
    {
        if (c->op != 0x01 && c->op != 0x02)
        {
            NotifyBox(5000, "MPU lab: group 8 op %d refused", c->op);
            goto out;
        }
        if (c->op == 0x01 && c->len != 13)
        {
            NotifyBox(5000, "MPU lab: MonRead len %d refused", c->len);
            goto out;
        }
        if (c->op == 0x02)
        {
            if (c->len != 13 && c->len != 32)
            {
                NotifyBox(5000, "MPU lab: exec len %d refused", c->len);
                goto out;
            }
            is_exec = 1;
            exec_index = ((uint32_t) c->data[4] << 24) |
                         ((uint32_t) c->data[5] << 16) |
                         ((uint32_t) c->data[6] << 8) |
                         (uint32_t) c->data[7];
            if (exec_index != 0x00 && exec_index != 0x0F &&
                exec_index != 0x10 && exec_index != 0x14 &&
                exec_index != 0x4C && exec_index != 0x4D &&
                exec_index != 0x50)
            {
                NotifyBox(5000, "MPU lab: exec index %x refused", exec_index);
                goto out;
            }
        }
    }

    /* The 00:19 test hung when AF search was started with the lens already at
     * infinity.  Refuse that known-bad state; move focus in first. */
    if (is_exec && exec_index == 0x10 && lens_info.focus_dist >= 60000)
    {
        log_add("%d REFUSE AF search: lens at infinity (%d)\n",
                get_ms_clock(), lens_info.focus_dist);
        NotifyBox(5000, "MPU lab: lens at infinity - focus nearer first");
        goto out;
    }

    NotifyBox(2500, "MPU lab: %s", c->name);
    msleep(1500);
    if (RECORDING)
    {
        NotifyBox(4000, "MPU lab: recording started, aborted");
        goto out;
    }

    /* Extra confirmation: the group 8 EXEC probes run an MPU-internal
     * function by index (0x1417C[]), which is not verified on camera.
     * Everything is still one-shot and logged. */
    if (is_exec)
    {
        NotifyBox(2500, "EXPERIMENTAL exec idx%02X in 2 s", exec_index);
        msleep(2000);
        if (RECORDING || gui_menu_shown())
        {
            NotifyBox(4000, "MPU lab: aborted before exec");
            goto out;
        }
    }

    if (is_exec)
        log_add("%d EXPECT group8 exec idx=%x reply=08 08 02 <BE32 ret> 00\n",
                get_ms_clock(), exec_index);

    if (is_exec)
    {
        int e = MPU_MON_OPEN();
        log_add("%d MONOPEN err=%d\n", get_ms_clock(), e);
        mpu_log_read("PRE_80000", 0xFFF80000);
        mpu_log_read("PRE_825DA", 0xFFF825DA);
        mpu_log_exposure("PRE");
    }

    cap_begin();
    cap_send(c->grp, c->op, c->len, c->data);
    msleep(800);

    if (is_exec) mpu_log_exposure("MID");
    if (c->stop_after)
    {
        uint8_t stop = 0;
        cap_send(9, 3, 1, &stop);
    }
    {
        /* The successful AF search finishes after the old 2 s window. */
        int win = c->win_ms ? c->win_ms : (is_exec ? 6000 : 1200);
        msleep(win);
    }
    if (is_exec)
    {
        /* Always end an AF search with FOCUS_STOP, even if the handler is
         * still waiting: the 00:19 infinity-start test hung. */
        uint8_t stop = 0;
        cap_send(9, 3, 1, &stop);
        log_add("%d STOP after group 8 exec\n", get_ms_clock());
        msleep(300);
        mpu_log_read("POST_80000", 0xFFF80000);
        mpu_log_read("POST_825DA", 0xFFF825DA);
        mpu_log_read("POST_825E0", 0xFFF825E0);
        mpu_log_exposure("POST");
        MPU_MON_CLOSE();
    }
    cap_end(c->name, NULL, 0, 1);

out:
    mpu_lab_busy = 0;
}

static void mpu_lab_cmd_select(void *priv, int delta)
{
    (void) priv;
    (void) delta;
    run_in_separate_task(mpu_lab_cmd_task, mpu_cmd_choice);
}

/* --------------------------------------------------------------- sequences */

struct seq_step
{
    uint8_t grp;
    uint8_t op;
    uint8_t len;
    uint8_t data[MPU_CMD_DATA];
    uint16_t wait_ms;      /* capture/settle time after sending */
    const char *name;
};

struct mpu_seq
{
    const char *name;
    const struct seq_step *steps;
    int count;
};

/* Step bodies only.  The runner always appends a focus stop (9,3,1) and, if a
 * session (9,0x25 with a non-zero first byte) was sent, closes it with an
 * all-zero (9,0x25,16) - also when the sequence is aborted. */
#define S_OPEN   { 9, 0x25, 16, { 0x02 }, 200, "session open" }
#define S_STOP   { 9, 0x03, 1, { 0x00 }, 300, "focus stop" }
#define S_NEAR   { 9, 0x13, 4, { 0x00, 0x01, 0x07, 0x00 }, 1500, "search near" }
#define S_FAR    { 9, 0x13, 4, { 0x00, 0x00, 0x07, 0x00 }, 1500, "search far" }

static const struct seq_step seq_drive_spd7[] =
{
    S_OPEN,
    { 9, 0x01, 5, { 0x00, 0x32, 0x07, 0x00, 0x00 }, 800, "drive +50 spd7" },
    { 9, 0x01, 5, { 0xFF, 0xCE, 0x07, 0x00, 0x00 }, 800, "drive -50 spd7" },
};

static const struct seq_step seq_drive_ml[] =
{
    S_OPEN,
    { 9, 0x01, 5, { 0x00, 0x32, 0xFF, 0xFF, 0x00 }, 800, "drive +50 FFFF" },
    { 9, 0x01, 5, { 0xFF, 0xCE, 0xFF, 0xFF, 0x00 }, 800, "drive -50 FFFF" },
};

static const struct seq_step seq_search[] =
{
    S_NEAR, S_STOP, S_FAR, S_STOP,
};

static const struct seq_step seq_session_search[] =
{
    S_OPEN, S_NEAR, S_STOP, S_FAR, S_STOP,
};

/* The AF_LENS_SEARCH monitor handler is 0x33794 == 0x1417C[16] in the
 * corrected map.  The candidate group 8 message that reaches it is
 * (8,2,32) with payload[4..7] = 00 00 00 10; this is UNVERIFIED on camera.
 * The sequence below sends that probe first and then drives Canon's own
 * (9,0x25)+(9,0x13) search proxy so the run still exercises the lens even if
 * the group 8 message is ignored.  TVAFLOG (0x6F81A) needs argc/argv and is
 * serial-monitor only, so there is no sequence for it. */
static const struct seq_step seq_afexec_search[] =
{
    { 8, 0x02, 32, X8_EXEC32(0x10), 1500, "8.2.32 AF_SEARCH idx16" },
    S_OPEN, S_NEAR, S_STOP, S_FAR, S_STOP,
};

static const struct mpu_seq mpu_seqs[] =
{
    { "session + drive spd7", seq_drive_spd7,    COUNT(seq_drive_spd7) },
    { "session + drive ML form", seq_drive_ml,   COUNT(seq_drive_ml) },
    { "AF search near/far",  seq_search,         COUNT(seq_search) },
    { "AF_LENS_SEARCH proxy", seq_session_search, COUNT(seq_session_search) },
    { "8.2 idx16 + search", seq_afexec_search, COUNT(seq_afexec_search) },
};

static const char * mpu_seq_choices[] =
{
    "session + drive spd7", "session + drive ML form",
    "AF search near/far", "AF_LENS_SEARCH proxy",
    "8.2 idx16 + search"
};

static int mpu_seq_choice = 0;

static int seq_must_abort(void)
{
    return RECORDING || !lv || gui_menu_shown();
}

static void mpu_lab_seq_task(int idx)
{
    if (mpu_lab_busy) return;
    mpu_lab_busy = 1;

    if (mpu_lab_refuse()) goto out;
    if (idx < 0 || idx >= COUNT(mpu_seqs)) goto out;
    if (!lv)
    {
        NotifyBox(4000, "MPU lab: sequences need LiveView");
        goto out;
    }

    const struct mpu_seq *s = &mpu_seqs[idx];
    NotifyBox(4000, "MPU seq: %s - close menu now", s->name);
    msleep(1500);

    /* the menu is still open after the select: give the user time to close it */
    for (int i = 0; i < 70 && gui_menu_shown(); i++) msleep(100);
    if (seq_must_abort())
    {
        NotifyBox(4000, "MPU seq: aborted (menu open / no LV / recording)");
        goto out;
    }

    int session = 0, aborted = 0;
    static const uint8_t stop = 0;
    static const uint8_t zeros[16] = { 0 };

    for (int i = 0; i < s->count; i++)
    {
        const struct seq_step *st = &s->steps[i];
        if (seq_must_abort())
        {
            aborted = 1;
            break;
        }
        cap_begin();
        cap_send(st->grp, st->op, st->len, st->data);
        if (st->grp == 9 && st->op == 0x25 && st->data[0]) session = 1;
        msleep(st->wait_ms);
        cap_end(st->name, s->name, i + 1, 1);
    }

    /* always: stop, then close the session if one was opened */
    cap_begin();
    cap_send(9, 3, 1, &stop);
    msleep(500);
    cap_end("focus stop (final)", s->name, s->count + 1, 0);
    if (session)
    {
        cap_begin();
        cap_send(9, 0x25, 16, zeros);
        msleep(2000);
        cap_end("session close", s->name, s->count + 2, 0);
    }

    NotifyBox(6000, "MPU seq %s: %s", s->name, aborted ? "ABORTED, stopped" : "done");

out:
    mpu_lab_busy = 0;
}

static void mpu_lab_seq_select(void *priv, int delta)
{
    (void) priv;
    (void) delta;
    run_in_separate_task(mpu_lab_seq_task, mpu_seq_choice);
}

/* ------------------------------------------------------------- drive test */
/* One-shot test of the relative focus drive (9,1,5) [hi lo SPD x 00].
 * Static RE (unverified): SPD == 0xFF makes the signed word a selector (only
 * +-1..3 move the lens); SPD != 0xFF makes it a raw relative pulse count.
 * Each step: read gate bytes, send, sample sharpness/luma/focus_dist/LV_LENS
 * every ~50 ms for ~600 ms, read gate bytes again.  Nothing else is sent
 * except a session open/close (9,0x25) around the test and a final (9,3,1)
 * focus stop.  No (8,x) messages, no EEPROM.  Log: ML/LOGS/MPUDRIVE.LOG. */

#define DRIVE_LOG_PATH   "ML/LOGS/MPUDRIVE.LOG"
#define DRV_SAMPLE_MS    50
#define DRV_WINDOW_MS    600
#define DRV_MAX_SAMPLES  16

static uint8_t drv_lvlens[128];
static volatile int drv_lvlens_len = 0;
/* PROP_LV_LENS is the same property as MPL_LENS (0x80050000); mirrored from
 * its handler below because a second handler would be a duplicate. */
static void drv_lvlens_note(const void *buf, unsigned int len)
{
    if (!buf || len > sizeof(drv_lvlens)) return;
    memcpy(drv_lvlens, buf, len);
    drv_lvlens_len = len;
}

static const uint32_t drv_gates[] =
{
    0xFFF84D4C, 0xFFF84D44, 0xFFFD8270, 0xFFFD8280, 0xFFF85D4C,
    0xFFF81A79, 0xFFF81C89, 0xFFF81CD9, 0xFFF81C12, 0xFFF81C14,
};

/* Single-word kind 0 reads; a failure (0xFFFD8xxx may) is logged and skipped. */
static void drv_read_gates(const char *tag, int step)
{
    char line[160];
    int o = snprintf(line, sizeof(line), "%d GATES %s step=%d", get_ms_clock(), tag, step);
    for (unsigned i = 0; i < COUNT(drv_gates); i++)
    {
        uint8_t b[8] = { 0 };
        int e = MPU_MON_READ(0, drv_gates[i], 4, b);
        if (e)
            o += snprintf(line + o, sizeof(line) - o, " %x=ERR%d", drv_gates[i], e);
        else
            o += snprintf(line + o, sizeof(line) - o, " %x=%02x%02x%02x%02x",
                          drv_gates[i], b[0], b[1], b[2], b[3]);
        if (o > (int) sizeof(line) - 40)
        {
            log_add("%s\n", line);
            o = snprintf(line, sizeof(line), "%d GATES %s step=%d(cont)", get_ms_clock(), tag, step);
        }
    }
    log_add("%s\n", line);
}

/* Same centre-of-frame Laplacian as crop_rec afdiag_sharpness_ex. */
static uint32_t drv_sharpness(int *luma_mean, uint32_t *gsum_out)
{
    uint32_t esum = 0, lsum = 0, gsum = 0;
    int n = 0;
    *luma_mean = 0;
    *gsum_out = 0;

    struct vram_info *vi = get_yuv422_vram();
    if (!vi || !vi->vram) return 0;
    int width = vi->width, height = vi->height, pitch = vi->pitch;
    if (width < 64 || height < 64 || pitch < width * 2) return 0;

    const uint8_t *vram = (const uint8_t *) UNCACHEABLE(vi->vram);
    int x0 = width * 3 / 8,  x1 = width * 5 / 8;
    int y0 = height * 3 / 8, y1 = height * 5 / 8;

    for (int y = y0 + 2; y < y1 - 2; y += 2)
    {
        const uint8_t *row = vram + y * pitch;
        for (int x = x0 + 2; x < x1 - 2; x += 2)
        {
            const uint8_t *c = row + x * 2 + 1;
            int cv = c[0];
            int e = 4 * cv - c[-2] - c[2] - c[-pitch] - c[pitch];
            if (e < 0) e = -e;
            esum += e;
            lsum += cv;
            int gx = c[2] - c[-2]; if (gx < 0) gx = -gx;
            int gy = c[pitch] - c[-pitch]; if (gy < 0) gy = -gy;
            gsum += gx + gy;
            n++;
        }
    }
    if (n)
    {
        *luma_mean = lsum / n;
        *gsum_out = gsum;
    }
    return esum;
}

struct drv_step
{
    const char *name;
    uint8_t data[5];
};

#define DS(NAME, HI, LO, SPD, X) { NAME, { HI, LO, SPD, X, 0x00 } }
static const struct drv_step drv_steps[] =
{
    DS("spd07 +20",  0x00, 0x14, 0x07, 0x00),
    DS("spd07 -20",  0xFF, 0xEC, 0x07, 0x00),
    DS("spd07 +100", 0x00, 0x64, 0x07, 0x00),
    DS("spd07 -100", 0xFF, 0x9C, 0x07, 0x00),
    DS("spd07 +300", 0x01, 0x2C, 0x07, 0x00),
    DS("spd07 -300", 0xFE, 0xD4, 0x07, 0x00),
    DS("spd01 +100", 0x00, 0x64, 0x01, 0x00),
    DS("spd01 -100", 0xFF, 0x9C, 0x01, 0x00),
    DS("spd00 +100", 0x00, 0x64, 0x00, 0x00),
    DS("spd00 -100", 0xFF, 0x9C, 0x00, 0x00),
    DS("FF form +3", 0x00, 0x03, 0xFF, 0xFF),
    DS("FF form -3", 0xFF, 0xFD, 0xFF, 0xFF),
};

struct drv_result
{
    int d0, d1;
    int s0, s1;      /* edge sum first / last sample */
    int ret;
    int done;        /* FOCUS_DONE event count */
};

static struct drv_result drv_res[COUNT(drv_steps)];

static void mpu_lab_drive_task(int unused)
{
    (void) unused;
    if (mpu_lab_busy) return;
    mpu_lab_busy = 1;

    int opened = 0, session = 0, aborted = 0, nsteps = 0;
    static const uint8_t stop = 0;
    static const uint8_t zeros[16] = { 0 };
    static const uint8_t open_s[16] = { 0x02 };

    if (mpu_lab_refuse()) goto out;
    if (!lv || is_manual_focus())
    {
        NotifyBox(4000, "Drive test: needs LiveView and AF mode");
        goto out;
    }

    NotifyBox(4000, "Drive test (9,1): close menu now");
    msleep(1500);
    for (int i = 0; i < 70 && gui_menu_shown(); i++) msleep(100);
    if (seq_must_abort())
    {
        NotifyBox(4000, "Drive test: aborted (menu open / no LV / recording)");
        goto out;
    }

    log_path = DRIVE_LOG_PATH;
    log_len = 0;
    log_add("# MPUDRIVE t=%d lens=\"%s\" zoom=x%d crop_preset=n/a (see crop_rec) dist0=%d\n"
            "# step lines: STEP id name payload; then GATES pre/post, SEND ret, FOCUS_DONE count,\n"
            "# samples 'ms s g luma dist' (ms from send)\n",
            get_ms_clock(), lens_info.name, lv_dispsize, (int) lens_info.focus_dist);

    int e = MPU_MON_OPEN();
    opened = 1;
    log_add("%d MONOPEN err=%d\n", get_ms_clock(), e);

    cap_begin();
    cap_send(9, 0x25, 16, open_s);      /* same session open as the drive sequences */
    session = 1;
    msleep(300);
    cap_active = 0;
    log_add("%d SESSION open ret=%s\n", get_ms_clock(), win_ret);

    for (unsigned i = 0; i < COUNT(drv_steps); i++)
    {
        const struct drv_step *st = &drv_steps[i];
        if (seq_must_abort())
        {
            aborted = 1;
            break;
        }
        struct drv_result *r = &drv_res[i];
        char h[24];
        hex_str(h, sizeof(h), st->data, 5);
        log_add("%d STEP %d \"%s\" payload=[%s]\n", get_ms_clock(), (int) i + 1, st->name, h);

        drv_read_gates("pre", i + 1);

        int dist0 = lens_info.focus_dist;
        cap_begin();
        int t0 = get_ms_clock();
        int ret = cap_send(9, 0x01, 5, st->data);
        log_add("%d SEND ret=%d dist0=%d\n", t0, ret, dist0);

        static struct { int t, s, g, luma, dist; } smp[DRV_MAX_SAMPLES];
        int n = 0;
        int lvl[4] = { 0 };
        while (n < DRV_MAX_SAMPLES && get_ms_clock() - t0 <= DRV_WINDOW_MS)
        {
            int m;
            uint32_t g;
            smp[n].s = drv_sharpness(&m, &g);
            smp[n].g = g;
            smp[n].luma = m;
            smp[n].dist = lens_info.focus_dist;
            smp[n].t = get_ms_clock() - t0;
            n++;
            msleep(DRV_SAMPLE_MS);
        }
        cap_active = 0;

        /* LV_LENS bytes 0x20..0x2F as four BE words, if the property arrived */
        if (drv_lvlens_len >= 0x30)
            for (int k = 0; k < 4; k++)
                lvl[k] = (drv_lvlens[0x20 + 4*k] << 24) | (drv_lvlens[0x21 + 4*k] << 16) |
                         (drv_lvlens[0x22 + 4*k] << 8) | drv_lvlens[0x23 + 4*k];
        char dh[24];
        hex_str(dh, sizeof(dh), caps[CAP_DONE].val, caps[CAP_DONE].vlen < 4 ? caps[CAP_DONE].vlen : 4);
        log_add("%d RESULT step=%d FOCUS_DONE n=%d val=[%s] fpc n=%d cmd n=%d LV_LENS len=%d w=%08x %08x %08x %08x\n",
                get_ms_clock(), (int) i + 1, caps[CAP_DONE].count, dh,
                caps[CAP_FPC].count, caps[CAP_CMD].count, drv_lvlens_len,
                lvl[0], lvl[1], lvl[2], lvl[3]);
        log_add("# ms s g luma dist\n");
        for (int k = 0; k < n; k++)
            log_add("%d %d %d %d %d\n", smp[k].t, smp[k].s, smp[k].g, smp[k].luma, smp[k].dist);

        drv_read_gates("post", i + 1);

        r->d0 = dist0;
        r->d1 = lens_info.focus_dist;
        r->s0 = n ? smp[0].s : 0;
        r->s1 = n ? smp[n - 1].s : 0;
        r->ret = ret;
        r->done = caps[CAP_DONE].count;
        nsteps++;
        log_flush();
        msleep(200);
    }

    /* always: stop, close the session, close the monitor */
    cap_begin();
    cap_send(9, 3, 1, &stop);
    msleep(500);
    cap_active = 0;
    log_add("%d STOP ret=%s dist=%d\n", get_ms_clock(), win_ret, (int) lens_info.focus_dist);
    if (session)
    {
        cap_begin();
        cap_send(9, 0x25, 16, zeros);
        msleep(1000);
        cap_active = 0;
        log_add("%d SESSION close ret=%s\n", get_ms_clock(), win_ret);
    }
    if (opened) MPU_MON_CLOSE();
    log_add("%d END steps=%d aborted=%d\n\n", get_ms_clock(), nsteps, aborted);
    if (!RECORDING) log_flush();
    log_len = 0;
    log_path = LOG_PATH;

    /* one NotifyBox per step: dist before->after, sharpness change in % */
    for (int i = 0; i < nsteps; i++)
    {
        struct drv_result *r = &drv_res[i];
        int pct = r->s0 ? (int) (((int64_t) r->s1 - r->s0) * 100 / r->s0) : 0;
        NotifyBox(3000, "%d %s: dist %d>%d s %+d%% done%d", i + 1, drv_steps[i].name,
                  r->d0, r->d1, pct, r->done);
        msleep(3000);
    }
    NotifyBox(5000, "Drive test %s: MPUDRIVE.LOG", aborted ? "ABORTED" : "done");

out:
    mpu_lab_busy = 0;
}

static void mpu_lab_drive_select(void *priv, int delta)
{
    (void) priv;
    (void) delta;
    run_in_separate_task(mpu_lab_drive_task, 0);
}

/* --------------------------------------------------------------- notices */
/* Neither TVAFLOG nor AF_LENS_SEARCH can be reached through the confirmed
 * ICU dispatch: the monitor text parser is serial-console-only and TVAFLOG
 * needs argc/argv.  AF_LENS_SEARCH is only an unverified group 8 EXEC probe.
 * These entries send nothing; they explain the state of the investigation. */

static void mpu_lab_notice_task(int which)
{
    if (mpu_lab_busy) return;
    mpu_lab_busy = 1;

    if (which == 0)
        NotifyBox(9000,
            "AF_LENS_SEARCH = MPU 0x33794 = 0x1417C[16]. Candidate msg: "
            "(8,2,32) payload 00 00 00 00 00 00 00 10 00 ... - UNVERIFIED. "
            "Use '8.2.32 AF_SEARCH idx16' (one-shot, logged) or the search proxy.");
    else
        NotifyBox(9000,
            "TVAFLOG = MPU 0x6F81A: serial-monitor command, needs argc==4 + 3 hex args. "
            "No ICU group 7/8/9 message reaches it; nothing is sent here. "
            "Use the (9,0x25)+(9,0x13) AF search proxy.");

    mpu_lab_busy = 0;
}

static void mpu_lab_af_note_select(void *priv, int delta)
{
    (void) priv;
    (void) delta;
    run_in_separate_task(mpu_lab_notice_task, 0);
}

static void mpu_lab_tvaf_note_select(void *priv, int delta)
{
    (void) priv;
    (void) delta;
    run_in_separate_task(mpu_lab_notice_task, 1);
}

/* ------------------------------------------------------------------- menu */

static struct menu_entry mpu_lab_menu[] =
{
    {
        .name = "MPU lab",
        .select = menu_open_submenu,
        /* Explicit action icon: an IT_SUBMENU entry counts as "enabled" only if
         * a child value is non-zero (guess_submenu_enabled), and the slim GUI
         * greys out and skips disabled non-bool rows.  All our choices start at
         * index 0, so the entry was greyed out and unreachable. */
        .icon_type = IT_ACTION,
        .help = "TEMPORARY: dump MPU memory and send raw ICU->MPU messages.",
        .help2 = "Dumps go to ML/LOGS/MPU-*.BIN, every command logs replies to ML/LOGS/MPULAB.LOG.\n"
                 "Raw commands are one-shot and refused while recording. OFF after restart.\n"
                 "The 8.2 EXEC probes are UNVERIFIED: they call MPU 0x1417C[index] via group 8;\n"
                 "index 16 is the corrected AF_LENS_SEARCH handler. The search entries drive the lens.",
        .children = (struct menu_entry[])
        {
            {
                .name = "Dump memory",
                .priv = &mpu_dump_choice,
                .max = COUNT(mpu_dumps) - 1,
                .choices = mpu_dump_choices,
                .edit_mode = EM_INLINE_ADJUST,
                .help = "Which MPU memory range to dump.",
                .help2 = "ROM 768 KB is the main goal; EEPROM 64 KB and RAM 24 KB are the 5D3/TX19A attempts.\n"
                         "Test 4 KB is a quick smoke test before a long dump."
            },
            {
                .name = "Run dump",
                .select = mpu_lab_dump_select,
                .help = "Run the selected read-only MPU dump (starts after 2 s).",
                .help2 = "Stops on a MonRead error; the result (bytes, 00/FF counts) is in MPULAB.LOG."
            },
            {
                .name = "MPU command",
                .priv = &mpu_cmd_choice,
                .max = COUNT(mpu_cmds) - 1,
                .choices = mpu_cmd_choices,
                .edit_mode = EM_INLINE_ADJUST,
                .help = "Raw message to send to the MPU (one shot, after 1.5 s).",
                .help2 = "(9,0x13) is LensSearchDriveStart; (9,0x01) is the focus drive with speed byte 0x07.\n"
                         "E 8.2.32 / 8.2.13 are UNVERIFIED group 8 EXEC probes: [..][00 00 00 IDX][..];\n"
                         "IDX 10 = AF_LENS_SEARCH (MPU 0x33794), 14 = no-op control, 0F/4C/4D/50 = AF family.\n"
                         "AF search (MPU) 8s is the confirmed long-window action; it watches for 8 s.\n"
                         "8.1 is a raw MpuMonRead request; 7.8 is a group 7 probe.\n"
                         "(8,0) MpuMonWrite / MEMW is intentionally not implemented: it writes MPU memory."
            },
            {
                .name = "Run command",
                .select = mpu_lab_cmd_select,
                .help = "Send the selected raw MPU message and log the replies.",
                .help2 = "Close the menu and keep the camera still. Summary: done/fpc/cmd counts, focus dist."
            },
            {
                .name = "AF_SEARCH path (info)",
                .select = mpu_lab_af_note_select,
                .help = "Where AF_LENS_SEARCH lives and the candidate bytes (sends nothing).",
                .help2 = "AF_LENS_SEARCH = MPU 0x33794 = 0x1417C[16]; candidate (8,2,32) payload 00..10.."
            },
            {
                .name = "TVAFLOG (serial only)",
                .select = mpu_lab_tvaf_note_select,
                .help = "TVAFLOG is a serial-monitor command; no ICU message sends it.",
                .help2 = "Handler 0x6F81A needs argc==4 and three hex args. This entry sends nothing."
            },
            {
                .name = "MPU sequence",
                .priv = &mpu_seq_choice,
                .max = COUNT(mpu_seqs) - 1,
                .choices = mpu_seq_choices,
                .edit_mode = EM_INLINE_ADJUST,
                .help = "Multi-step test (session, drive, search, 8.2 probe). Needs LiveView.",
                .help2 = "'AF_LENS_SEARCH proxy' uses Canon's (9,0x25)+(9,0x13) search path (confirmed).\n"
                         "'8.2 idx16 + search' first sends the UNVERIFIED (8,2,32) AF_LENS_SEARCH probe.\n"
                         "TVAFLOG (0x6F81A) is serial-monitor only and has no sequence.\n"
                         "Always ends with focus stop and, if a session was opened, a session close."
            },
            {
                .name = "Run sequence",
                .select = mpu_lab_seq_select,
                .help = "Run the sequence. Close the menu; it aborts if the menu reopens.",
                .help2 = "Also aborts on recording or leaving LiveView; stop/close are still sent."
            },
            {
                .name = "Drive test (9,1)",
                .select = mpu_lab_drive_select,
                .help = "One-shot relative focus drive test (9,1,5); logs ML/LOGS/MPUDRIVE.LOG.",
                .help2 = "Needs LiveView + AF mode, menu closed. Sends SPD 07/01/00 raw counts and the FF form,\n"
                         "alternating +/-, with gate bytes read before/after and a sharpness/dist trace.\n"
                         "Lens ends near where it started; always ends with focus stop + session close."
            },
            MENU_EOL,
        }
    }
};

static unsigned int mpu_lab_init(void)
{
    /* Unconditional: is_camera() reads firmware_version, which may not be
     * populated yet when module init runs at boot.  This module is only built
     * for the EOS M anyway; the runtime checks in the tasks still guard the
     * actual operations.  "Focus" is a slim-grid tile; "Settings" is not
     * easily reachable there. */
    menu_add("Focus", mpu_lab_menu, COUNT(mpu_lab_menu));
    return 0;
}

static unsigned int mpu_lab_deinit(void)
{
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(mpu_lab_init)
    MODULE_DEINIT(mpu_lab_deinit)
MODULE_INFO_END()

MODULE_PROPHANDLERS_START()
    MODULE_PROPHANDLER(MPL_LENS)
    MODULE_PROPHANDLER(MPL_FOCUS_DONE)
    MODULE_PROPHANDLER(MPL_CAF_STATE)
    MODULE_PROPHANDLER(MPL_FPC)
    MODULE_PROPHANDLER(MPL_FOCUS_CMD)
    MODULE_PROPHANDLER(MPL_P28)
    MODULE_PROPHANDLER(MPL_AF_RESULT)
    MODULE_PROPHANDLER(MPL_FOCAL_DIST)
MODULE_PROPHANDLERS_END()
