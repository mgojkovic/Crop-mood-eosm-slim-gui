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

PROP_HANDLER(MPL_LENS)       { cap_note(CAP_LENS, buf, len); }
PROP_HANDLER(MPL_FOCUS_DONE) { cap_note(CAP_DONE, buf, len); }
PROP_HANDLER(MPL_CAF_STATE)  { cap_note(CAP_CAF, buf, len); }
PROP_HANDLER(MPL_FPC)        { cap_note(CAP_FPC, buf, len); }
PROP_HANDLER(MPL_FOCUS_CMD)  { cap_note(CAP_CMD, buf, len); }
PROP_HANDLER(MPL_P28)        { cap_note(CAP_P28, buf, len); }
PROP_HANDLER(MPL_AF_RESULT)  { cap_note(CAP_RESULT, buf, len); }
PROP_HANDLER(MPL_FOCAL_DIST) { cap_note(CAP_DIST, buf, len); }

static char log_line[1600];     /* only used from the (single) busy task */
static int log_len = 0;

static void log_flush(void)
{
    if (!log_len) return;
    FILE *f = FIO_CreateFileOrAppend(LOG_PATH);
    if (f)
    {
        FIO_WriteFile(f, log_line, log_len);
        FIO_CloseFile(f);
    }
    log_len = 0;
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

struct mpu_cmd
{
    uint8_t grp;
    uint8_t op;
    uint8_t len;
    uint8_t data[16];
    uint8_t stop_after;    /* send (9,3,1) after the settle time */
    const char *name;
};

/* Payload notes:
 *  - (9,0x13,4) LensSearchDriveStart: forum RE says [?, end, speed, timeout].
 *    Both 0 and 1 in the first byte are tried; no firmware sender exists.
 *  - (9,0x01,5) focus drive: a1ex's working 5D3 message is [hi lo 07 00 00],
 *    0x07 = driving speed.  ML's property path always sends [hi lo FF FF 00].
 *  - (9,0x25,16) AF session: Canon EOS M sends [02 00 ...] on half-press.
 *  - (9,0x0B,2) is PROP_LV_FOCUS_STATE, not AF_RESULT when sent to the MPU.
 */
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
    { 9, 0x0B, 2, { 0x02, 0x00 }, 0, "focus state 2" }
};

static const char * mpu_cmd_choices[] =
{
    "search end1 spd7", "search flag1 end1", "search end0 spd7",
    "search flag1 end0", "search end2 spd7", "search end1 spd0",
    "focus +1 spd7", "focus +15 spd7", "focus +50 spd7",
    "focus -15 spd7", "focus -50 spd7",
    "session 02", "session 00", "session 01",
    "focus stop", "focus state 0", "focus state 2"
};

static int mpu_cmd_choice = 0;

static void mpu_lab_cmd_task(int idx)
{
    if (mpu_lab_busy) return;
    mpu_lab_busy = 1;

    if (mpu_lab_refuse()) goto out;
    if (idx < 0 || idx >= COUNT(mpu_cmds)) goto out;

    const struct mpu_cmd *c = &mpu_cmds[idx];

    NotifyBox(2500, "MPU lab: %s", c->name);
    msleep(1500);
    if (RECORDING)
    {
        NotifyBox(4000, "MPU lab: recording started, aborted");
        goto out;
    }

    cap_begin();
    cap_send(c->grp, c->op, c->len, c->data);
    msleep(800);
    if (c->stop_after)
    {
        uint8_t stop = 0;
        cap_send(9, 3, 1, &stop);
    }
    msleep(1200);   /* capture window: ~2 s after the command */
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
    uint8_t data[16];
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

static const struct mpu_seq mpu_seqs[] =
{
    { "session + drive spd7", seq_drive_spd7,    COUNT(seq_drive_spd7) },
    { "session + drive ML form", seq_drive_ml,   COUNT(seq_drive_ml) },
    { "search near/far",     seq_search,         COUNT(seq_search) },
    { "session + search",    seq_session_search, COUNT(seq_session_search) },
};

static const char * mpu_seq_choices[] =
{
    "session + drive spd7", "session + drive ML form",
    "search near/far", "session + search"
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
                 "Raw commands are one-shot and refused while recording. OFF after restart.",
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
                         "The first focus/search tests are the ones Canon EOS M firmware never sends."
            },
            {
                .name = "Run command",
                .select = mpu_lab_cmd_select,
                .help = "Send the selected raw MPU message and log the replies.",
                .help2 = "Close the menu and keep the camera still. Summary: done/fpc/cmd counts, focus dist."
            },
            {
                .name = "MPU sequence",
                .priv = &mpu_seq_choice,
                .max = COUNT(mpu_seqs) - 1,
                .choices = mpu_seq_choices,
                .edit_mode = EM_INLINE_ADJUST,
                .help = "Multi-step test (session, drive, search). Needs LiveView.",
                .help2 = "Always ends with focus stop and, if a session was opened, a session close."
            },
            {
                .name = "Run sequence",
                .select = mpu_lab_seq_select,
                .help = "Run the sequence. Close the menu; it aborts if the menu reopens.",
                .help2 = "Also aborts on recording or leaving LiveView; stop/close are still sent."
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
