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
 * Turn Settings -> AF diag log to "ON + MPU" to see the outgoing messages in
 * ML/LOGS/AFDIAG.LOG.  All module switches are OFF after every restart
 * (there is no persistent CONFIG_INT).
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

#define MPU_MON_OPEN()  ((int (*)(void)) 0xFF1DD098)()
#define MPU_MON_CLOSE() ((int (*)(void)) 0xFF1DD118)()
#define MPU_MON_READ    ((int (*)(int, uint32_t, int, void *)) 0xFF1DD2B8)
#define MPU_SEND        ((int (*)(int, int, int, const void *)) 0x38C4)

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
    { 0, 0x00000000, 1024 * 1024, "MPU-ROM.BIN" },  /* F749 ROM, same try as 5D3 */
    { 2, 0x00000000,    64 * 1024, "MPU-EEP.BIN" }, /* MPU EEPROM */
    { 0, 0xFFFF8000,    24 * 1024, "MPU-RAM.BIN" }, /* TX19A RAM address; harmless probe */
    { 0, 0x00000000,     4 * 1024, "MPU-TEST.BIN" } /* quick smoke test */
};

static const char * mpu_dump_choices[] =
{
    "ROM 1 MB", "EEPROM 64 KB", "RAM 24 KB", "test 4 KB"
};

static int mpu_dump_choice = 0;
static volatile int mpu_lab_busy = 0;

static void mpu_lab_dump_task(int idx)
{
    if (mpu_lab_busy) return;
    mpu_lab_busy = 1;

    if (!is_camera("EOSM", "2.0.2"))
    {
        NotifyBox(4000, "MPU lab: EOS M 2.0.2 only");
        goto out;
    }
    if (RECORDING)
    {
        NotifyBox(4000, "MPU lab: not while recording");
        goto out;
    }
    if (idx < 0 || idx >= COUNT(mpu_dumps))
    {
        goto out;
    }

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

    int e = MPU_MON_OPEN();
    if (e)
    {
        NotifyBox(5000, "MPU dump: MonOpen failed (%d)", e);
        FIO_CloseFile(f);
        goto out;
    }

    uint8_t buf[1024];
    for (int off = 0; off < d->size; off += (int) sizeof(buf))
    {
        int n = d->size - off;
        if (n > (int) sizeof(buf)) n = (int) sizeof(buf);
        memset(buf, 0, sizeof(buf));
        MPU_MON_READ(d->kind, d->addr + off, n, buf);
        FIO_WriteFile(f, buf, n);
        if ((off & 0xFFFF) == 0)
            NotifyBox(250, "MPU dump %d%%", off * 100 / d->size);
    }

    MPU_MON_CLOSE();
    FIO_CloseFile(f);
    NotifyBox(6000, "MPU dump done: %s", d->file);

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

    if (!is_camera("EOSM", "2.0.2"))
    {
        NotifyBox(4000, "MPU lab: EOS M 2.0.2 only");
        goto out;
    }
    if (RECORDING)
    {
        NotifyBox(4000, "MPU lab: not while recording");
        goto out;
    }
    if (idx < 0 || idx >= COUNT(mpu_cmds))
    {
        goto out;
    }

    const struct mpu_cmd *c = &mpu_cmds[idx];
    int dist0 = lens_info.focus_dist;

    NotifyBox(2500, "MPU lab: %s", c->name);
    msleep(1500);
    if (RECORDING)
    {
        NotifyBox(4000, "MPU lab: recording started, aborted");
        goto out;
    }

    int ret = MPU_SEND(c->grp, c->op, c->len, c->data);
    msleep(800);

    if (c->stop_after)
    {
        uint8_t stop = 0;
        MPU_SEND(9, 3, 1, &stop);
        msleep(400);
    }

    int dist1 = lens_info.focus_dist;
    NotifyBox(6000, "%s: ret %d dist %d->%d", c->name, ret, dist0, dist1);

out:
    mpu_lab_busy = 0;
}

static void mpu_lab_cmd_select(void *priv, int delta)
{
    (void) priv;
    (void) delta;
    run_in_separate_task(mpu_lab_cmd_task, mpu_cmd_choice);
}

/* ------------------------------------------------------------------- menu */

static struct menu_entry mpu_lab_menu[] =
{
    {
        .name = "MPU lab",
        .select = menu_open_submenu,
        .help = "TEMPORARY: dump MPU memory and send raw ICU->MPU messages.",
        .help2 = "Read-only dumps go to ML/LOGS/MPU-*.BIN. Raw commands are one-shot and refused while\n"
                 "recording. Set AF diag log to ON + MPU to see the messages. OFF after restart.",
        .children = (struct menu_entry[])
        {
            {
                .name = "Dump memory",
                .priv = &mpu_dump_choice,
                .max = COUNT(mpu_dumps) - 1,
                .choices = mpu_dump_choices,
                .edit_mode = EM_INLINE_ADJUST,
                .help = "Which MPU memory range to dump.",
                .help2 = "ROM 1 MB is the main goal; EEPROM 64 KB and RAM 24 KB are the 5D3/TX19A attempts.\n"
                         "Test 4 KB is a quick smoke test before a long dump."
            },
            {
                .name = "Run dump",
                .select = mpu_lab_dump_select,
                .help = "Run the selected read-only MPU dump (starts after 2 s).",
                .help2 = "The menu closes first. Do not touch the camera during the dump."
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
                .help = "Send the selected raw MPU message.",
                .help2 = "Close the menu and keep the camera still. Watch the lens and the @MPU log."
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
     * actual operations. */
    menu_add("Settings", mpu_lab_menu, COUNT(mpu_lab_menu));
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
