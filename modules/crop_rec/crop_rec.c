#include <dryos.h>
#include <module.h>
#include <config.h>
#include <menu.h>
#include <menu-grid.h>
#include <beep.h>
#include <property.h>
#include <patch.h>
#include <bmp.h>
#include <lvinfo.h>
#include <powersave.h>
#include <raw.h>
#include <fps.h>
#include <shoot.h>
#include <lens.h>
#include <focus.h>
#include <vram.h>
#include "../mlv_lite/mlv_lite.h"
#include "../dual_iso/dual_iso.h"
#include "histogram.h"

#undef CROP_DEBUG

#ifdef CROP_DEBUG
#define dbg_printf(fmt,...) { printf(fmt, ## __VA_ARGS__); }
#else
#define dbg_printf(fmt,...) {}
#endif


int YUV_HD_S_H_height = 0;
int YUV_HD_S_H_width = 0;
int YUV_HD_S_V_height = 0;
int YUV_HD_S_V_width = 0;
int reg_skip_left = 0;
int reg_skip_right = 0;
int reg_skip_top = 0;
int reg_skip_bottom = 0;
int reg_cmos5 = 0;
int reg_cmos7 = 0;
int reg_height = 0;
int reg_width = 0;
int reg_Preview_H = 0;
int reg_Preview_V = 0;
int reg_YUV_HD_S_H = 0;
int reg_YUV_HD_S_V = 0;

static int zoom = 0;
static int submenu = 0;

static int is_DIGIC_5 = 0;
static int is_5D3 = 0;
static int is_6D = 0;
static int is_70D = 0;
static int is_700D = 0;
static int is_650D = 0;
static int is_100D = 0;
static int is_EOSM = 0;
static int is_basic = 0;

static CONFIG_INT("crop.fps_over", fps_over, 0);
static CONFIG_INT("crop.tapdisp", tapdisp, 1);
static CONFIG_INT("crop.preset_fps", crop_preset_fps_reduce, 1);
static CONFIG_INT("crop.preset", crop_preset_index, 2);
static CONFIG_INT("crop.shutter_range", shutter_range, 0);
static CONFIG_INT("crop.fix_dual_iso_flicker", fix_dual_iso_flicker, 1);

CONFIG_INT("crop.bit_depth", bit_depth_analog, 1);
#define OUTPUT_14BIT (bit_depth_analog == 0)
#define OUTPUT_12BIT (bit_depth_analog == 1)
#define OUTPUT_11BIT (bit_depth_analog == 2)
#define OUTPUT_10BIT (bit_depth_analog == 3)

// check raw.c
extern int BitDepth_Analog;

static CONFIG_INT("crop.brighten_lv", brighten_lv_method, 0);

static CONFIG_INT("crop.preset_aspect_ratio", crop_preset_ar_menu, 0);
static int crop_preset_ar = 0;
#define AR_16_9        (crop_preset_ar == 0)
#define AR_2_1         (crop_preset_ar == 1)
#define AR_2_20_1      (crop_preset_ar == 2)
#define AR_2_35_1      (crop_preset_ar == 3)
#define AR_2_39_1      (crop_preset_ar == 4)

static CONFIG_INT("crop.preset_1x1", crop_preset_1x1_res_menu, 3);
static int crop_preset_1x1_res = 0;
#define CROP_2_5K      (crop_preset_1x1_res == 0)
#define CROP_2_8K      (crop_preset_1x1_res == 1)
#define CROP_3K        (crop_preset_1x1_res == 2)
#define CROP_1440p     (crop_preset_1x1_res == 3)
#define CROP_1280p     (crop_preset_1x1_res == 4)
#define CROP_Full_Res  (crop_preset_1x1_res == 5)
#define CROP_1620p     (crop_preset_1x1_res == 6)
#define CROP_1080p     (crop_preset_1x1_res == 7)

CONFIG_INT("crop.preset_1x3", crop_preset_1x3_res_menu, 1);
static int crop_preset_1x3_res = 0;
#define Anam_Highest   (crop_preset_1x3_res == 0)
#define Anam_Higher    (crop_preset_1x3_res == 1)
#define Anam_Medium    (crop_preset_1x3_res == 2)
#define Anam_FLV    (crop_preset_1x3_res == 3)

static CONFIG_INT("crop.preset_3x3", crop_preset_3x3_res_menu, 1);
static int crop_preset_3x3_res = 0;
#define High_FPS       (crop_preset_3x3_res == 0)
#define mv1080         (crop_preset_3x3_res == 1)
#define mv1080_3_2        (crop_preset_3x3_res == 2)

static CONFIG_INT("crop.preset_fps", crop_preset_fps_menu, 0);
static int crop_preset_fps = 0;
#define Framerate_24   (crop_preset_fps == 0)
#define Framerate_25   (crop_preset_fps == 1)
#define Framerate_30   (crop_preset_fps == 2)

/* customized buttons variables
 * EOS M slim defaults:
 *   SET = Zoom x10; U/D = ISO; L/R = Aperture; INFO off.
 * Arrow modes: 0=OFF, 1=Shutter, 2=Aperture, 3=ISO
 * INFO modes:  0=OFF, 1=Dual ISO, 2=Histogram, 3=Waveform, 4=Zebras,
 *              5=False Color, 6=framing, 7=Quick Panel
 */
CONFIG_INT("crop.button_SET",       SET_button, 1);
static CONFIG_INT("crop.button_H-Shutter", Half_Shutter, 2);
CONFIG_INT("crop.button_INFO",      INFO_button, 0);
CONFIG_INT("crop.auto_refocus", Auto_refocus, 0); /* EOS M slim: 0=OFF, 1=ON, 2=ON + REC */
CONFIG_INT("crop.auto_refocus_sens", Auto_refocus_sens, 1); /* EOS M slim: 0=Low, 1=Medium, 2=High */
CONFIG_INT("crop.shutter_zoom", Shutter_zoom, 0); /* EOS M slim: 0=OFF, 1=hold x10, 2=sticky x10, 3=AF assist */
CONFIG_INT("crop.arrows_U_D",       Arrows_U_D, 3); /* ISO */
CONFIG_INT("crop.more_hacks",       more_hacks, 1);
static CONFIG_INT("crop.arrows_L_R",       Arrows_L_R, 2); /* Aperture */
static CONFIG_INT("crop.button_map_v",     button_map_v, 0); /* remap old configs once */

enum crop_preset {
    CROP_PRESET_OFF = 0,
    CROP_PRESET_3X,
    CROP_PRESET_3X_TALL,
    CROP_PRESET_3K,
    CROP_PRESET_4K_HFPS,
    CROP_PRESET_UHD,
    CROP_PRESET_FULLRES_LV,
    CROP_PRESET_3x3_1X,
    CROP_PRESET_3x3_1X_48p,
    CROP_PRESET_1x3,
    CROP_PRESET_3x1,
    CROP_PRESET_40_FPS,
    CROP_PRESET_CENTER_Z,
    
    /* these are for 650D / 700D / EOSM/M2 / 100D */
    CROP_PRESET_1X1,
    CROP_PRESET_1X3,
    CROP_PRESET_3X3,
    NUM_CROP_PRESETS
};

/* presets are not enabled right away (we need to go to play mode and back)
 * so we keep two variables: what's selected in menu and what's actually used.
 * note: the menu choices are camera-dependent */
static enum crop_preset crop_preset = 0;

/* must be assigned in crop_rec_init */
static enum crop_preset * crop_presets = 0;

/* current menu selection (*/
#define CROP_PRESET_MENU crop_presets[crop_preset_index]

/* menu choices for 5D3 */
static enum crop_preset crop_presets_5d3[] = {
    CROP_PRESET_OFF,
    CROP_PRESET_3X,
    CROP_PRESET_3X_TALL,
    CROP_PRESET_3x3_1X,
    CROP_PRESET_3x3_1X_48p,
    CROP_PRESET_3K,
    CROP_PRESET_UHD,
    CROP_PRESET_4K_HFPS,
    CROP_PRESET_CENTER_Z,
    CROP_PRESET_FULLRES_LV,
  //CROP_PRESET_1x3,
  //CROP_PRESET_3x1,
  //CROP_PRESET_40_FPS,
};

static const char * crop_choices_5d3[] = {
    "OFF",
    "1920 1:1",
    "1920 1:1 tall",
    "1920 50/60 3x3",
    "1080p45/1040p48 3x3",
    "3K 1:1",
    "UHD 1:1",
    "4K 1:1 half-fps",
    "3.5K 1:1 centered x5",
    "Full-res LiveView",
  //"1x3 binning",
  //"3x1 binning",      /* needs manual LV refresh (by getting outside LV) */
  //"40 fps",
};

static const char crop_choices_help_5d3[] =
    "Change 1080p and 720p movie modes into crop modes (select one)";

static const char crop_choices_help2_5d3[] =
    "\n"
    "1:1 sensor readout (square raw pixels, 3x crop, good preview in 1080p)\n"
    "1:1 crop, higher vertical resolution (1920x1920 @ 24p, cropped preview)\n"
    "1920x960 @ 50p, 1920x800 @ 60p (3x3 binning, cropped preview)\n"
    "1920x1080 @ 45p, 1920x1040 @ 48p, 3x3 binning (50/60 FPS in Canon menu)\n"
    "1:1 3K crop (3072x1920 @ 24p, square raw pixels, preview broken)\n"
    "1:1 4K UHD crop (3840x1600 @ 24p, square raw pixels, preview broken)\n"
    "1:1 4K crop (4096x3072 @ 12.5 fps, half frame rate, preview broken)\n"
    "1:1 readout in x5 zoom mode (centered raw, high res, cropped preview)\n"
    "Full resolution LiveView (5796x3870 @ 7.4 fps, 5784x3864, preview broken)\n"
    "1x3 binning: read all lines, bin every 3 columns (extreme anamorphic)\n"
    "3x1 binning: bin every 3 lines, read all columns (extreme anamorphic)\n"
    "FPS override test\n";

/* menu choices for 70D */
static enum crop_preset crop_presets_70d[] = {
    CROP_PRESET_OFF,
    CROP_PRESET_CENTER_Z,
    CROP_PRESET_UHD,
    CROP_PRESET_1x3,
    CROP_PRESET_3x3_1X,
};

static const char * crop_choices_70d[] = {
    "OFF",
    "1:1 3.5K centered x5",
    "1:1 UHD",
    "1x3 5.5K",
    "3x3 720p",
};

static const char crop_choices_help_70d[] =
    "Change 1080p and 720p movie modes into crop modes (one choice)";

static const char crop_choices_help2_70d[] =
    "\n"
    "1:1 readout in x5 zoom mode (centered raw, high res, cropped preview)\n"
    "1:1 4K UHD crop (3840x2160 @ 24p, square raw pixels, preview broken)\n"
    "1x3 5.5K 1832x1816 ~3:1 AR @ 23.976 FPS\n"
    "3x3 binning in 720p (square pixels in RAW, vertical crop)";

/* menu choices for entry level DIGIC 5 models, 650D / 700D / EOS M/M2 / 100D */
static enum crop_preset crop_presets_DIGIC_5[] = {
    CROP_PRESET_OFF,
    CROP_PRESET_1X1,
    CROP_PRESET_1X3,
    CROP_PRESET_3X3,
};

static const char * crop_choices_DIGIC_5[] = {
    "OFF",
    "1:1 crop",
    "1x3",
    "3x3",
};

static const char crop_choices_help_DIGIC_5[] =
    "Turn your camera into crop moods (select one)\n"
    "Center crop on sensor, no pixel binning/skipping in this mode.\n"
    "a.k.a Anamorphic, reads all vertical pixels, reduces aliasing.\n"
    "1080p mode and experimental High Framerate options.\n";
    
    
/* menu choices for cameras that only have the basic 3x3 crop_rec option */
static enum crop_preset crop_presets_basic[] = {
    CROP_PRESET_OFF,
    CROP_PRESET_3x3_1X,
};

static const char * crop_choices_basic[] = {
    "OFF",
    "3x3 720p",
};

static const char crop_choices_help_basic[] =
    "Change 1080p and 720p movie modes into crop modes (one choice)";

static const char crop_choices_help2_basic[] =
    "3x3 binning in 720p (square pixels in RAW, vertical crop)";


/* camera-specific parameters */
static uint32_t CMOS_WRITE               = 0;
static uint32_t MEM_CMOS_WRITE           = 0;
static uint32_t ADTG_WRITE               = 0;
static uint32_t MEM_ADTG_WRITE           = 0;
static uint32_t ENGIO_WRITE              = 0;
static uint32_t MEM_ENGIO_WRITE          = 0;
static uint32_t ENG_DRV_OUT              = 0;
static uint32_t ENG_DRV_OUTS             = 0;
static uint32_t PATH_SelectPathDriveMode = 0;

/* from SENSOR_TIMING_TABLE (fps-engio.c) or FPS override submenu */
static int fps_main_clock = 0;
static int default_timerA[11]; /* 1080p  1080p  1080p   720p   720p   zoom   crop   crop   crop   crop   crop */
static int default_timerB[11]; /*   24p    25p    30p    50p    60p     x5    24p    25p    30p    50p    60p */
static int default_fps_1k[11] = { 23976, 25000, 29970, 50000, 59940, 29970, 23976, 25000, 29970, 50000, 59940 };

/* video modes */

/* properties are fired AFTER the new video mode is fully up and running
 * to apply our presets, we need to know the video more DURING the switch
 * we'll peek into the PathDriveMode structure for that
 * 
 * Example: x10 -> x1 on 5D3
 * This sequence cannot be identified just by looking at C0F06804;
 * some ADTG registers that we need to overide are configured before that.
 * 
 * CtrlSrv: DlgLiveView.c PRESS_TELE_MAG_BUTTON KeyRepeat[0]
 *     Gmt: gmtModeChange
 *     Evf: evfModeChangeRequest(4)
 *     Evf: PATH_SelectPathDriveMode S:0 Z:10000 R:0 DZ:0 SM:1
 *      (lots of stuff going on)
 *     Evf: evfModeChangeComplete
 *      (some more stuff)
 *     Gmt: VisibleParam 720, 480, 0, 38, 720, 404.
 *     Gmt: gmtUpdateDispSize (10 -> 1)
 * PropMgr: *** mpu_send(06 05 09 11 01 00)     ; finally triggered PROP_LV_DISPSIZE...
 */

/* faster version than the one from ML core */
static void set_zoom(int zoom)
{
    if (!lv) return;
    if (RECORDING) return;
    if (is_movie_mode() && video_mode_crop) return;
    zoom = COERCE(zoom, 1, 10);
    if (zoom > 1 && zoom < 10) zoom = 5;
    prop_request_change_wait(PROP_LV_DISPSIZE, &zoom, 4, 1000);
}

#ifdef CONFIG_EOSM
/* AF changes rebuild Canon's Live View pipeline just like a zoom or menu
 * return.  Keep that rebuild inside the same transition controller. */
static void eosm_lv_guard_request(void);
#endif
int crop_rec_lv_transition_diag(char *buffer, int size);
int crop_rec_lv_transition_busy(void);

/* faster version than the one from ML core */
static void set_lv_af_mode(int lv_af_mode)
{
    if (!lv) return;
    if (RECORDING) return;
    if (lv_af_mode > 3 && lv_af_mode != 0) lv_af_mode = 1;
    prop_request_change(PROP_LIVE_VIEW_AF_SYSTEM, &lv_af_mode, 4);
#ifdef CONFIG_EOSM
    eosm_lv_guard_request();
#endif
}

//Photo mode
static int reciso = 0; /* coming from crop_rec.c */
extern int WEAK_FUNC(reciso) isoless_recovery_iso;

/* Main dial (EOS M: WHEEL_LEFT/RIGHT) → shutter. Clockwise = faster. */
static int slim_handle_main_dial_shutter(unsigned int key)
{
    if (key == MODULE_KEY_WHEEL_RIGHT)
    {
        shutter_toggle(0, 1);
        return 1;
    }
    if (key == MODULE_KEY_WHEEL_LEFT)
    {
        shutter_toggle(0, -1);
        return 1;
    }
    return 0;
}

static int slim_lv_base_zoom(void)
{
    return is_movie_mode() ? 5 : 1;
}

static void slim_zoom_to_x10(void)
{
    extern int kill_canon_gui_mode;

    int base = slim_lv_base_zoom();
    if (!lv || RECORDING || lv_dispsize != base) return;
    if (lv_disp_mode != 0) return;

    set_zoom(10);
    /* Danne EOS M path: Canon owns x10 UI; restore its front buffer. */
    kill_canon_gui_mode = 0;
    if (canon_gui_front_buffer_disabled())
        canon_gui_enable_front_buffer(0);
    wait_lv_frames(1);
    redraw();
}

static void slim_zoom_from_x10(void)
{
    extern int kill_canon_gui_mode;

    if (!lv || RECORDING || lv_dispsize != 10) return;

    set_zoom(1);
    if (is_movie_mode())
    {
        msleep(50);
        set_zoom(5);
    }
    kill_canon_gui_mode = is_movie_mode() ? 1 : 0;
    if (canon_gui_front_buffer_disabled())
        canon_gui_enable_front_buffer(0);
    wait_lv_frames(1);
    redraw();
}

/* EOS M slim: one-shot AF assist (Settings → Shutter zoom → AF).
 *
 * Crop presets live in the patched x5 path, where Canon's AF data is not
 * trustworthy.  x10 is left unpatched, so AF is accurate there.  On a
 * half-shutter press this sequence goes to x10, asks Canon for one AF run
 * through the remote half-shutter line (so a short tap is enough), waits for
 * it to finish and returns to x5 through the normal transition controller.
 * Holding half-shutter keeps x10 on screen to check the result.
 *
 * The key handler only raises a request; all waiting happens here, from
 * CBR_SHOOT_TASK, one non-blocking step per poll. */
#define SLIM_AF_SETTLE_MS   250   /* let Canon finish the x10 switch first */
#define SLIM_AF_BLIND_MS   1500   /* AF time when Canon reports no status */
#define SLIM_AF_MAX_MS     4000   /* hard limit for one reported AF run */
#define SLIM_AF_RELEASE_MS  150   /* pause around releasing the remote line */
#define SLIM_AF_INPLACE_SETTLE_MS 30 /* in-place AF has no zoom switch to wait for */

enum slim_af_state
{
    SLIM_AF_IDLE = 0,
    SLIM_AF_SETTLE,
    SLIM_AF_WAIT_START,
    SLIM_AF_RUNNING,
    SLIM_AF_RELEASE,
    SLIM_AF_HOLD,
};

static volatile int slim_af_request = 0;
/* Defined further down: the AF lens test (not CONFIG_INT) and Auto refocus idle state. */
static int afdiag_lens_test;
static int aref_is_idle(void);
/* PROP_LV_AF_RESULT fires once at the end of every Canon AF run on EOS M
 * (0 = focused, 1 = failed); field logs showed it ~0.3-1 s after the AF
 * request.  PROP_LV_FOCUS_STATUS never reports "focusing" on this camera. */
static volatile int slim_af_result_count = 0;
static volatile int slim_af_last_result = 0; /* payload of the last PROP_LV_AF_RESULT: 0 = OK, 1 = failed */
static int slim_af_result_seen = 0;
/* Auto refocus while recording: run AF in place (no x10 zoom, which would
 * wreck the recording); request value 2 instead of 1. */
static int slim_af_inplace = 0;
static int slim_af_state = SLIM_AF_IDLE;
static int slim_af_since = 0;
static int slim_af_sw1_held = 0;

/* Only ever release a remote half-press that this sequence made itself. */
static void slim_af_sw1(int on)
{
    if (slim_af_sw1_held == on) return;
    slim_af_sw1_held = on;
    SW1(on, 0);
}

static int slim_af_can_start(void)
{
    return is_EOSM && lv && is_movie_mode() && CROP_PRESET_MENU &&
           !RECORDING && !gui_menu_shown() && lv_disp_mode == 0 &&
           !is_manual_focus() && lv_dispsize == 5 &&
           !crop_rec_lv_transition_busy();
}

static int slim_af_can_start_inplace(void)
{
    return is_EOSM && lv && is_movie_mode() && CROP_PRESET_MENU &&
           !gui_menu_shown() && lv_disp_mode == 0 && !is_manual_focus() &&
           !crop_rec_lv_transition_busy();
}

static void slim_af_finish(void)
{
    slim_af_sw1(0);
    slim_af_state = SLIM_AF_IDLE;
    slim_af_request = 0;
    slim_af_inplace = 0;
    /* No-op unless we are still idle in x10; leaving x10 restarts the guard. */
    slim_zoom_from_x10();
}

/* Returns 1 while the sequence owns Live View. */
static int slim_af_step(void)
{
    int now = get_ms_clock();

    if (slim_af_state == SLIM_AF_IDLE)
    {
        if (!slim_af_request) return 0;
        if (afdiag_lens_test)
        {
            /* the AF lens test owns the lens and the remote half-press */
            slim_af_request = 0;
            return 0;
        }
        int inplace = (slim_af_request == 2);
        slim_af_request = 0;
        if (inplace)
        {
            if (!slim_af_can_start_inplace()) return 0;
            slim_af_inplace = 1;
            slim_af_result_seen = slim_af_result_count;
            slim_af_state = SLIM_AF_SETTLE;
            slim_af_since = now;
            return 1;
        }
        if (!slim_af_can_start()) return 0;

        slim_zoom_to_x10();
        if (lv_dispsize != 10) return 0;

        slim_af_result_seen = slim_af_result_count;
        slim_af_state = SLIM_AF_SETTLE;
        slim_af_since = now;
        return 1;
    }

    /* Recording, a menu, SET or a lens switch took x10 away: stop cleanly. */
    if (!lv || gui_menu_shown() || is_manual_focus() ||
        (!slim_af_inplace && (RECORDING || lv_dispsize != 10)))
    {
        slim_af_finish();
        return 0;
    }

    switch (slim_af_state)
    {
        case SLIM_AF_SETTLE:
            /* Canon often starts AF on its own from the physical press as
             * soon as x10 is up; if that run already finished, we are done. */
            if (slim_af_result_count != slim_af_result_seen)
            {
                slim_af_state = SLIM_AF_RELEASE;
                slim_af_since = now;
                return 1;
            }
            if (now - slim_af_since < (slim_af_inplace ? SLIM_AF_INPLACE_SETTLE_MS : SLIM_AF_SETTLE_MS))
                return 1;
            slim_af_sw1(1);
            slim_af_state = SLIM_AF_WAIT_START;
            slim_af_since = now;
            return 1;

        case SLIM_AF_WAIT_START:
            /* End on Canon's AF result.  Fixed AF time only as a fallback. */
            if (slim_af_result_count != slim_af_result_seen)
            {
                slim_af_state = SLIM_AF_RELEASE;
                slim_af_since = now;
            }
            else if (lv_focus_status == 3)
                slim_af_state = SLIM_AF_RUNNING;
            else if (now - slim_af_since >= SLIM_AF_BLIND_MS)
            {
                slim_af_state = SLIM_AF_RELEASE;
                slim_af_since = now;
            }
            return 1;

        case SLIM_AF_RUNNING:
            if (lv_focus_status != 3 || slim_af_result_count != slim_af_result_seen ||
                now - slim_af_since >= SLIM_AF_MAX_MS)
            {
                slim_af_state = SLIM_AF_RELEASE;
                slim_af_since = now;
            }
            return 1;

        case SLIM_AF_RELEASE:
            if (now - slim_af_since < SLIM_AF_RELEASE_MS)
                return 1;
            slim_af_sw1(0);
            slim_af_state = SLIM_AF_HOLD;
            slim_af_since = now;
            return 1;

        case SLIM_AF_HOLD:
            if (now - slim_af_since < SLIM_AF_RELEASE_MS)
                return 1;
            /* The remote line is released; what is left is the user's finger. */
            if (get_halfshutter_pressed())
                return 1;
            slim_af_finish();
            return 0;
    }

    slim_af_finish();
    return 0;
}

/* Settings → Shutter zoom: half-shutter x10 like SET (hold or sticky), or AF assist. */
static int slim_handle_shutter_zoom(unsigned int key)
{
    if (!Shutter_zoom || !is_EOSM) return 0;
    if (!lv || gui_menu_shown() || RECORDING) return 0;
    if (lv_disp_mode != 0) return 0;

    int base = slim_lv_base_zoom();
    int mode = Shutter_zoom;

    if (mode == 3)
    {
        if (is_manual_focus())
        {
            /* Nothing to autofocus: behave like ON (hold to zoom). */
            mode = 1;
        }
        else
        {
            /* Never swallow the key here: Canon must still see the half-press. */
            if (key == MODULE_KEY_PRESS_HALFSHUTTER && is_movie_mode() &&
                CROP_PRESET_MENU && lv_dispsize == base &&
                slim_af_state == SLIM_AF_IDLE && !afdiag_lens_test)
                slim_af_request = 1;
            return 0;
        }
    }

    if (mode == 1)
    {
        if (key == MODULE_KEY_PRESS_HALFSHUTTER && lv_dispsize == base)
        {
            slim_zoom_to_x10();
            return 1;
        }
        if (key == MODULE_KEY_UNPRESS_HALFSHUTTER && lv_dispsize == 10)
        {
            slim_zoom_from_x10();
            return 1;
        }
    }
    else if (mode == 2)
    {
        if (key == MODULE_KEY_PRESS_HALFSHUTTER)
        {
            if (lv_dispsize == base)
                slim_zoom_to_x10();
            else if (lv_dispsize == 10)
                slim_zoom_from_x10();
            return 1;
        }
        /* Swallow half-shutter release so Canon does not disturb x10 preview. */
        if (key == MODULE_KEY_UNPRESS_HALFSHUTTER)
            return 1;
    }

    return 0;
}

/* EOS M Settings -> SET Button. Keep this independent of the legacy SET
 * assignments below: value 2 used to mean ISO on other cameras. */
static int slim_handle_set_button(unsigned int key)
{
    if (!is_EOSM || SET_button != 2 || key != MODULE_KEY_PRESS_SET)
        return 0;

    /* Same policy as idle-LiveView touch: never open menus while recording. */
    if (RECORDING)
        return 1;

    if (lv && is_movie_mode() && !gui_menu_shown() && lv_disp_mode == 0)
        gui_open_last_menu_selection();

    /* Do not fall through to legacy SET=2 (ISO) handling. */
    return 1;
}

/* Instant Dual ISO on/off for INFO/SET shortcuts: config + bottom bar only.
 * CMOS refresh runs from CBR_SHOOT_TASK (do not block the key handler). */
static void slim_toggle_dual_iso(void)
{
    if (RECORDING)
        return;
    int en = get_config_var("isoless.hdr");
    set_config_var("isoless.hdr", en ? 0 : 1);
    lens_display_set_dirty();
}

/* ISO arrow shortcuts: when Dual ISO is ON, step primary+recovery as a pair. */
static void crop_rec_adjust_iso(int sign)
{
    if (lens_info.raw_iso == 0x0)
        return;

    /* Module .mo builds lack CONFIG_SLIM_MENUS; use dual_iso_is_enabled() like the bottom bar. */
    if (dual_iso_is_enabled())
    {
        dual_iso_slim_step_pair(sign > 0 ? 1 : -1);
        return;
    }

    if (sign > 0)
    {
        if (lens_info.raw_iso == ISO_6400)
            return;
    }
    else
    {
        if (lens_info.raw_iso == ISO_100)
            return;
    }
    iso_toggle(0, sign);
}

/* EOS M Settings → INFO Button:
 * 0=OFF, 1=Dual ISO, 2=Histogram, 3=Waveform, 4=Zebras, 5=False Color,
 * 6=framing, 7=Quick Panel.
 * Returns: 1 = handled (block Canon), -1 = pass to Canon, 0 = not our INFO mapping. */
static int slim_handle_info_button(unsigned int key)
{
    if (!is_EOSM || key != MODULE_KEY_INFO)
        return 0;
    if (!INFO_button)
        return 0; /* OFF — Canon INFO / LV cycle */

    /* Outside ML overlay LV, keep Canon INFO for non-framing modes only. */
    if (INFO_button != 6 && lv_disp_mode != 0)
        return -1;

    switch (INFO_button)
    {
        case 1: /* Dual ISO on/off */
            slim_toggle_dual_iso();
            return 1;

        case 2: /* Histogram Off ↔ Performance */
        {
            int h = get_config_var("hist.draw");
            set_config_var("hist.draw", h ? 0 : 1);
            if (!get_config_var("hist.draw")) redraw();
            return 1;
        }

        case 3: /* Waveform Off ↔ Performance */
        {
            int w = get_config_var("waveform.draw");
            set_config_var("waveform.draw", w ? 0 : 1);
            if (!get_config_var("waveform.draw")) redraw();
            return 1;
        }

        case 4: /* Zebras Off ↔ Performance */
        {
            int z = get_config_var("zebra.draw");
            set_config_var("zebra.draw", z ? 0 : 1);
            if (!get_config_var("zebra.draw")) redraw();
            return 1;
        }

        case 5: /* False Color toggle */
        {
            extern int falsecolor_draw;
            if (!falsecolor_draw)
                falsecolor_draw = 1;
            else
            {
                falsecolor_draw = 0;
                redraw();
            }
            return 1;
        }

        case 6: /* framing ↔ real-time (MLV Lite Preview → Framing) */
            mlv_lite_info_framing_toggle();
            return 1;

        case 7: /* Quick Panel */
            if (!RECORDING && lv && is_movie_mode() &&
                !gui_menu_shown() && lv_disp_mode == 0)
            {
                menu_quick_screen_open();
                gui_open_menu();
            }
            return 1;

        default:
            return 0;
    }
}

/* Arrow assignment: 0=OFF, 1=Shutter, 2=Aperture, 3=ISO.
 * dir: +1 = UP/RIGHT, -1 = DOWN/LEFT. Returns 1 if consumed. */
/* Exposure shortcuts (ISO / shutter / aperture) are ignored only while
 * mlv_lite's "More" small hacks has actually suspended Canon's AeWb task;
 * then a change would not apply cleanly.  This used to test crop_rec's own
 * "More Hacks" flag, which is forced on at startup on the EOS M, so ISO
 * could never be changed while recording, even with Small hacks OFF. */
static int slim_expo_frozen(void)
{
    return RECORDING && is_more_hacks_selected() && AeWbTask_Disabled();
}

static int slim_handle_arrow_adjust(int mode, int dir)
{
    if (!mode)
        return 0;
    if (slim_expo_frozen())
        return 1;

    if (mode == 1) /* Shutter */
    {
        shutter_toggle(0, dir);
        return 1;
    }
    if (mode == 2) /* Aperture */
    {
        if (!lens_info.aperture)
            return 1;
        if (dir > 0)
        {
            if (lens_info.raw_aperture == lens_info.raw_aperture_max)
                return 1;
            aperture_toggle(0, 1);
        }
        else
        {
            if (lens_info.raw_aperture == lens_info.raw_aperture_min)
                return 1;
            aperture_toggle(0, -1);
        }
        return 1;
    }
    if (mode == 3) /* ISO */
    {
        /* Dual ISO owns the ISO pair while active. Consume the shortcut so
         * an Up/Down ISO assignment cannot alter either ISO value. */
        if (dual_iso_is_enabled())
            return 1;
        if (lens_info.raw_iso == 0x0)
            return 1;
        if (dir > 0)
        {
            if (lens_info.raw_iso == ISO_6400)
                return 1;
            crop_rec_adjust_iso(2);
        }
        else
        {
            if (lens_info.raw_iso == ISO_100)
                return 1;
            crop_rec_adjust_iso(-2);
        }
        return 1;
    }
    return 0;
}

/* customize buttons and buttons shortcuts, FIXME: implement these as feature in ML core? */
static unsigned int photo_keypress_cbr(unsigned int key)
{
    
    if (lv && !gui_menu_shown() && !is_movie_mode())
    {
        if (is_EOSM && slim_handle_shutter_zoom(key))
            return 0;

        if (slim_handle_main_dial_shutter(key))
            return 0;

        {
            int info = slim_handle_info_button(key);
            if (info == 1) return 0;
            if (info == -1) return 1;
        }

        extern int kill_canon_gui_mode;
        /* Quick x10 mode */

        if (lv_dispsize == 1)
        {
            if (((key == MODULE_KEY_PRESS_SET         ) && SET_button  == 1)                 ||
                ((key == MODULE_KEY_INFO              ) && !is_EOSM && INFO_button == 1)     ||
                (!is_EOSM && (key == MODULE_KEY_PRESS_HALFSHUTTER ) && Half_Shutter) )
            {
            if (key == MODULE_KEY_PRESS_HALFSHUTTER && Half_Shutter)
            {
                msleep(400);
            }
                set_zoom(10);
                /* Enable Canon overlays in x10 mode */
                kill_canon_gui_mode = 0;
                if (canon_gui_front_buffer_disabled())
                {
                        canon_gui_enable_front_buffer(0);
                }
                return 0;
            }
        }

        /* Finished from x10 mode? Let's get back to normal preview */
        if (lv_dispsize == 10)
        {
            if (((key == MODULE_KEY_PRESS_SET           ) && SET_button  == 1)                 ||
                ((key == MODULE_KEY_INFO                ) && !is_EOSM && INFO_button == 1)     ||
                (!is_EOSM && (key == MODULE_KEY_UNPRESS_HALFSHUTTER ) && Half_Shutter != 3 && is_manual_focus()) ||
                (!is_EOSM && (key == MODULE_KEY_PRESS_HALFSHUTTER ) && Half_Shutter == 3 && is_manual_focus()) )
            {
                set_zoom(1); // Get to x1 first, sometime we get black preview when going x10 --> x5

                /* Disable Canon overlays in x5 mode */
                kill_canon_gui_mode = 1;
                return 0;
            }
        }
        
        if (lv_dispsize != 10)
        {
            /* U/D and L/R: Shutter / Aperture / ISO (Settings → Up/Down / Left/Right Button) */
            if (key == MODULE_KEY_PRESS_UP && slim_handle_arrow_adjust(Arrows_U_D, 1))
                return 0;
            if (key == MODULE_KEY_PRESS_DOWN && slim_handle_arrow_adjust(Arrows_U_D, -1))
                return 0;
            if (key == MODULE_KEY_PRESS_RIGHT && slim_handle_arrow_adjust(Arrows_L_R, 1))
                return 0;
            if (key == MODULE_KEY_PRESS_LEFT && slim_handle_arrow_adjust(Arrows_L_R, -1))
                return 0;

            /* Legacy SET / non-EOSM INFO ISO/aperture shortcuts (unchanged mappings) */
            if (((key == MODULE_KEY_INFO)       && !is_EOSM && INFO_button == 2) ||
                ((key == MODULE_KEY_PRESS_SET)  && SET_button  == 2))
            {
                crop_rec_adjust_iso(2);
                return 0;
            }
            if (key == MODULE_KEY_INFO && !is_EOSM && INFO_button == 3)
            {
                aperture_toggle(0, -1);
                return 0;
            }
            if (key == MODULE_KEY_PRESS_SET && SET_button == 3)
            {
                aperture_toggle(0, 1);
                return 0;
            }
            
            /* Dual ISO ON / OFF */
            if (((key == MODULE_KEY_INFO)       && !is_EOSM && INFO_button == 4) ||
                ((key == MODULE_KEY_PRESS_SET)  && SET_button == 4))
            {
                if (!RECORDING)
                {
                    slim_toggle_dual_iso();
                    return 0;
                }
            }
            
            /* False color ON / OFF */
            if (((key == MODULE_KEY_INFO)       && !is_EOSM && INFO_button == 5) ||
                ((key == MODULE_KEY_PRESS_SET)  && SET_button  == 5))
            {
                SetGUIRequestMode(0);
                extern int falsecolor_draw;
                if (!falsecolor_draw)
                {
                    falsecolor_draw = 1;
                    return 0;
                }
                if (falsecolor_draw)
                {
                    falsecolor_draw = 0;
                    redraw();
                    return 0;
                }
            }
            
#ifdef CONFIG_SLIM_MENUS
            if (tapdisp == 4 && key == MODULE_KEY_TOUCH_1_FINGER)
#else
            if (tapdisp == 5 && key == MODULE_KEY_TOUCH_1_FINGER)
#endif
            {
                SetGUIRequestMode(0);
                extern int falsecolor_draw;
                if (!falsecolor_draw)
                {
                    falsecolor_draw = 1;
                    return 0;
                }
                if (falsecolor_draw)
                {
                    falsecolor_draw = 0;
                    redraw();
                    return 0;
                }
            }
        }
                        
        /* Opens up last magic lantern menu tab by tapping display */
        if (!is_movie_mode() && !gui_menu_shown() && lv && lv_dispsize != 10)
        {

            if ((key == MODULE_KEY_TOUCH_1_FINGER && tapdisp == 2) || (key == MODULE_KEY_PRESS_SET && SET_button == 7) || (key == MODULE_KEY_INFO && !is_EOSM && INFO_button == 6))
            {
                msleep(100);
                if(lv_disp_mode != 0){
                    // Use INFO key to cycle LV as normal when not in the LV with ML overlays
                    return 1;
                }
                select_menu_by_name("Expo", "Shutter");
                gui_open_menu();
                submenu = 1;
            }
            if ((key == MODULE_KEY_PRESS_SET && SET_button == 6) || (key == MODULE_KEY_TOUCH_1_FINGER && tapdisp == 3) || (key == MODULE_KEY_INFO && !is_EOSM && INFO_button == 7))
            {
                msleep(100);
                if(lv_disp_mode != 0){
                    // Use INFO key to cycle LV as normal when not in the LV with ML overlays
                    return 1;
                }
                select_menu_by_name("Expo", "Aperture");
                gui_open_menu();
                submenu = 1;
            }
            if ((key == MODULE_KEY_PRESS_SET && SET_button == 8) || (key == MODULE_KEY_INFO && !is_EOSM && INFO_button == 8)
#ifndef CONFIG_SLIM_MENUS
                 || (key == MODULE_KEY_TOUCH_1_FINGER && tapdisp == 4)
#endif
            )
            {
                msleep(100);
                if(lv_disp_mode != 0){
                    // Use INFO key to cycle LV as normal when not in the LV with ML overlays
                    return 1;
                }
                select_menu_by_name("Expo", "ISO");
                gui_open_menu();
                submenu = 1;
            }
            if (tapdisp == 1 && key == MODULE_KEY_TOUCH_1_FINGER)
            {
                msleep(100);
                if(lv_disp_mode != 0){
                    // Use INFO key to cycle LV as normal when not in the LV with ML overlays
                    return 1;
                }
                select_menu_by_name("Expo", "White Balance");
                gui_open_menu();
            }
        }
    }

return 1;
}

/* PATH_SelectPathDriveMode S:%d Z:%lx R:%lx DZ:%d SM:%d */
/* offsets verified on 5D3, 6D, 70D, EOSM, 100D, 60D, 80D, 200D */
const struct PathDriveMode
{
    uint32_t SM;            /* 5D3,700D: 0 during zoom, 1 in all other modes; 6D: 2 is flicker-related?! */
    uint32_t fps_mode;      /* 5D3,700D: 0=60p, 1=50p, 2=30p/zoom, 3=25p, 4=24p */
    uint32_t S;             /* 5D3,700D: 0=1080p, 1=720p, 8=zoom, 6=1080crop (700D) */
    uint32_t resolution_idx;/* 5D3,700D: 0=1080p/zoom, 1=720p, 2=640x480 (PathDriveMode->resolution_idx) */
    uint16_t zoom_lo;       /* 5D3,700D: lower word of zoom; unused? */
    uint16_t zoom;          /* 5D3,700D: 1, 5 or 10 */
    uint32_t unk_14;        /* 5D3: unused? */
    uint32_t DZ;            /* 5D3: unused? 700D: 2=1080crop */
    uint32_t unk_1c;
    uint32_t unk_20;
    uint32_t CF;            /* 100D, 80D: ? */
    uint32_t SV;            /* 100D, EOSM, 80D: ? */
    uint32_t unk_2c;
    uint32_t unk_30;
    uint32_t OutputType;   /*  SelectPath: 0 LCD, 1 VIDEO(NTSC), 2 VIDEO(PAL), 3 HDMI(1080i FULL), 4 HDMI(1080i INFO), 5 HDMI(720p FULL), 6 HDMI(720p INFO), 7 HDMI(480), 8 HDMI(576) */
    uint32_t unk_38;
    uint32_t unk_3c;
    uint32_t unk_40;
    uint32_t DT;            /* 100D, EOSM: ? */
} * PathDriveMode = 0;

enum fps_mode {
    FPS_60 = 0,
    FPS_50 = 1,
    FPS_30 = 2,
    FPS_25 = 3,
    FPS_24 = 4,
};

static int is_1080p()
{
    /* properties triggered too late */
    if (PathDriveMode->zoom != 1)
    {
        return 0;
    }

    /* unsure whether fast enough or not; to be tested */
    if (PathDriveMode->DZ)
    {
        return 0;
    }

    /* this snippet seems OK with properties */
    /* note: on 5D2 and 5D3 (maybe also 6D, not sure),
     * sensor configuration in photo mode is identical to 1080p.
     * other cameras may be different */
    return !is_movie_mode() || PathDriveMode->resolution_idx == 0;
}

static int is_720p()
{
    /* properties triggered too late */
    if (PathDriveMode->zoom != 1)
    {
        return 0;
    }

    /* unsure whether fast enough or not; to be tested */
    if (PathDriveMode->DZ)
    {
        return 0;
    }

    if (is_EOSM && !RECORDING_H264)
    {
        /* EOS M stays in 720p30 during standby */
        return 1;
    }

    /* this snippet seems OK with properties */
    return is_movie_mode() && PathDriveMode->resolution_idx == 1;
}

static int is_x5_zoom()
{
    if (PathDriveMode->zoom != 5)
    {
        return 0;
    }

    if (PathDriveMode->S != 8)
    {
        return 0;
    }

    if (PathDriveMode->SM != 0)
    {
        return 0;
    }

    /* this snippet seems OK with properties */
    return is_movie_mode() && PathDriveMode->zoom == 5;
}

static int is_supported_mode()
{
    if (!lv) return 0;

    if (0)
    {
        printf(
            "Path: SM=%d S=%d res=%d DZ=%d zoom=%d mode=%d\n", 
            PathDriveMode->SM, PathDriveMode->S, PathDriveMode->resolution_idx,
            PathDriveMode->DZ, PathDriveMode->zoom, PathDriveMode->fps_mode
        );
    }
    
    /* 650D / 700D / EOSM/M2 / 100D prests will only work in x5 mode, don't patch x1 */
    if (PathDriveMode->zoom == 1)
    {
        if (is_650D || is_700D || is_EOSM || is_100D) 
        {
            return 0;
        }

        if (is_5D3 || is_70D)
        {
            if (crop_preset == CROP_PRESET_CENTER_Z)
            {
                return 0;
            }
        }
    }

    if (PathDriveMode->zoom == 10)
    {
        /* leave the x10 zoom unaltered, for focusing */
        return 0; 
    }

    if (PathDriveMode->zoom == 5)
    {
        if (is_5D3)
        {
            if (crop_preset != CROP_PRESET_CENTER_Z)
            {
                return 0;
            }
        }
        
        if (is_basic)
        {
            return 0;
        }
    }

    return 1;
}

/* These appear to hold the selected HDMI configuration before applying it
 * we can detect LCD, 480p or 1080i outputs early from here, cool!
 *
 * This LOG from 700D when connecting HDMI to 480p ouput:
 *DisplayMgr:ff330104:88:16: [EDID] dwVideoCode = 2
 *DisplayMgr:ff330118:88:16: [EDID] dwHsize = 720
 *DisplayMgr:ff33012c:88:16: [EDID] dwVsize = 480
 *DisplayMgr:ff330148:88:16: [EDID] ScaningMode = EDID_NON_INTERLACE(p)
 *DisplayMgr:ff330194:88:16: [EDID] VerticalFreq = EDID_FREQ_60Hz
 *DisplayMgr:ff3301b0:88:16: [EDID] AspectRatio = EDID_ASPECT_4x3
 *DisplayMgr:ff3301cc:88:16: [EDID] AudioMode = EDID_AUDIO_LINEAR_PCM
 *DisplayMgr:ff331580:88:16: [EDID] ColorMode = EDID_COLOR_RGB */
const struct EDID_HDMI_INFO
{
    uint32_t dwVideoCode;   /* 0 LCD, 2 480p, 5 1080i */
    uint32_t dwHsize;       /* LCD = 0, 480p = 720, 1080i = 1920 */
    uint32_t dwVsize;       /* LCD = 0, 480p = 480, 1080i = 1080 */
    uint32_t ScaningMode;   /* 0 = EDID_NON_INTERLACE(p), 1 = EDID_INTERLACE(i) */
    uint32_t VerticalFreq;
    uint32_t AspectRatio;   /* 0 = EDID_ASPECT_4x3, 1 = EDID_ASPECT_16x9 */
    uint32_t AudioMode;
    uint32_t ColorMode;
} * EDID_HDMI_INFO = 0;

static int32_t  target_yres = 0;
static int32_t  delta_adtg0 = 0;
static int32_t  delta_adtg1 = 0;
static int32_t  delta_head3 = 0;
static int32_t  delta_head4 = 0;
static uint32_t cmos1_lo = 0, cmos1_hi = 0;
static uint32_t cmos2 = 0;

/* helper to allow indexing various properties of Canon's video modes */
static inline int get_video_mode_index()
{
    if (lv_dispsize > 1)
    {
        if (PathDriveMode->zoom == 5)
        {
            return 5;
        }
    }

    return
        (video_mode_fps == 24) ?  0 :
        (video_mode_fps == 25) ?  1 :
        (video_mode_fps == 30) ?  2 :
        (video_mode_fps == 50) ?  3 :
     /* (video_mode_fps == 60) */ 4 ;
}

/* optical black area sizes */
/* not sure how to adjust them from registers, so... hardcode them here */
static inline void FAST calc_skip_offsets(int * p_skip_left, int * p_skip_right, int * p_skip_top, int * p_skip_bottom)
{
    /* start from LiveView values */
    int skip_left       = 146;
    int skip_right      = 2;
    int skip_top        = 28;
    int skip_bottom     = 0;
    
    skip_left       = 72;
    skip_right      = 0;
    skip_top        = 28;
    skip_bottom     = 0;
    
    switch (crop_preset)
    {
            
        case CROP_PRESET_3X3:
            if (mv1080)
            {
                if (AR_16_9)//976
                {
                    skip_left       = 72 + reg_skip_left;
                    skip_right      = 0 + reg_skip_right;
                    skip_top        = 28 + 92;
                    skip_bottom     = 0 + 92;
                }
                if (AR_2_1)//868
                {
                    skip_left       = 72 + reg_skip_left;
                    skip_right      = 0 + reg_skip_right;
                    skip_top        = 28 + 146;
                    skip_bottom     = 0 + 146;
                }
                if (AR_2_20_1)//790
                {
                    skip_left       = 72 + reg_skip_left;
                    skip_right      = 0 + reg_skip_right;
                    skip_top        = 28 + 185;
                    skip_bottom     = 0 + 185;
                }
                if (AR_2_35_1)//738
                {
                    skip_left       = 72 + reg_skip_left;
                    skip_right      = 0 + reg_skip_right;
                    skip_top        = 28 + 211;
                    skip_bottom     = 0 + 211;
                }
                if (AR_2_39_1)//726
                {
                    skip_left       = 72 + reg_skip_left;
                    skip_right      = 0 + reg_skip_right;
                    skip_top        = 28 + 217;
                    skip_bottom     = 0 + 217;
                }
            }
            if (mv1080_3_2)
            {
                skip_left       = 72;
                skip_right      = 0;
                skip_top        = 28;
                skip_bottom     = 0;
            }
            break;
                                    
        case CROP_PRESET_FULLRES_LV:
            /* photo mode values */
            skip_left       = 138;
            skip_right      = 2;
            skip_top        = 60;   /* fixme: this is different, why? */
            break;

        case CROP_PRESET_3K:
        case CROP_PRESET_UHD:
        case CROP_PRESET_4K_HFPS:
            skip_right      = 0;    /* required for 3840 - tight fit */
            /* fall-through */
        
        case CROP_PRESET_3X_TALL:
            skip_top        = 30;
            break;

        case CROP_PRESET_3X:
        case CROP_PRESET_1x3:
            skip_top        = 60;
            break;

        case CROP_PRESET_3x3_1X:
        case CROP_PRESET_3x3_1X_48p:
            if (is_720p()) skip_top = 0;
            break;
    }

    if (p_skip_left)   *p_skip_left    = skip_left;
    if (p_skip_right)  *p_skip_right   = skip_right;
    if (p_skip_top)    *p_skip_top     = skip_top;
    if (p_skip_bottom) *p_skip_bottom  = skip_bottom;
}

/* to be in sync with 0xC0F06800 */
static int get_top_bar_adjustment()
{
    switch (crop_preset)
    {
        case CROP_PRESET_FULLRES_LV:
            return 0;                   /* 0x10018: photo mode value, unchanged */
        case CROP_PRESET_3x3_1X:
        case CROP_PRESET_3x3_1X_48p:
            if (is_720p()) return 28;   /* 0x1D0017 from 0x10017 */
            /* fall through */
        default:
            return 30;                  /* 0x1F0017 from 0x10017 */
    }
}

/* Vertical resolution from current unmodified video mode */
/* (active area only, as seen by mlv_lite) */
static inline int get_default_yres()
{
    return 
        (video_mode_fps <= 30) ? 1290 : 672;
}

/* EOS M 1:1 sub-presets (2.5K/3K/…) use CROP_PRESET_1X1 hooks but need 3K yres table row. */
static inline enum crop_preset crop_preset_yres_lookup(void)
{
    if (crop_preset == CROP_PRESET_1X1 && CROP_3K)
        return CROP_PRESET_3K;
    return crop_preset;
}

/* skip_top from unmodified video mode (raw.c, LiveView skip offsets) */
static inline int get_default_skip_top()
{
    return 
        (video_mode_fps <= 30) ? 28 : 20;
}

/* max resolution for each video mode (trial and error) */
/* it's usually possible to push the numbers a few pixels further,
 * at the risk of corrupted frames */
static int max_resolutions[NUM_CROP_PRESETS][6] = {
                                /*   24p   25p   30p   50p   60p   x5 */
    [CROP_PRESET_3X_TALL]       = { 1920, 1728, 1536,  960,  800, 1320 },
    [CROP_PRESET_3x3_1X]        = { 1290, 1290, 1290,  960,  800, 1320 },
    [CROP_PRESET_3x3_1X_48p]    = { 1290, 1290, 1290, 1080, 1040, 1320 }, /* 1080p45/48 */
    [CROP_PRESET_3K]            = { 1920, 1728, 1504,  760,  680, 1320 },
    [CROP_PRESET_UHD]           = { 1536, 1472, 1120,  640,  540, 1320 },
    [CROP_PRESET_4K_HFPS]       = { 3072, 3072, 2500, 1440, 1200, 1320 },
    [CROP_PRESET_FULLRES_LV]    = { 3870, 3870, 3870, 3870, 3870, 1320 },
};

/* 5D3 vertical resolution increments over default configuration */
/* note that first scanline may be moved down by 30 px (see reg_override_top_bar) */
static inline int FAST calc_yres_delta()
{
    int desired_yres = (target_yres) ? target_yres
        : max_resolutions[crop_preset_yres_lookup()][get_video_mode_index()];

    if (desired_yres)
    {
        /* user override */
        int skip_top;
        calc_skip_offsets(0, 0, &skip_top, 0);
        int default_yres = get_default_yres();
        int default_skip_top = get_default_skip_top();
        int top_adj = get_top_bar_adjustment();
        return desired_yres - default_yres + skip_top - default_skip_top + top_adj;
    }

    ASSERT(0);
    return 0;
}

#define YRES_DELTA calc_yres_delta()

int CMOS_5_Debug = 0;
int CMOS_7_Debug = 0;

/* pack two 6-bit values into a 12-bit one */
#define PACK12(lo,hi) ((((lo) & 0x3F) | ((hi) << 6)) & 0xFFF)

/* pack two 16-bit values into a 32-bit one */
#define PACK32(lo,hi) (((uint32_t)(lo) & 0xFFFF) | ((uint32_t)(hi) << 16))

/* pack two 16-bit values into a 32-bit one */
#define PACK32(lo,hi) (((uint32_t)(lo) & 0xFFFF) | ((uint32_t)(hi) << 16))

static void FAST cmos_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    /* make sure we are in 1080p/720p mode */
    if (!is_supported_mode())
    {
        /* looks like checking properties works fine for detecting
         * changes in video mode, but not for detecting the zoom change */
        return;
    }

    uint16_t* data_buf = (uint16_t*) regs[0];
    int cmos_new[15] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    
    if (is_5D3)
    {
        switch (crop_preset)
        {
            /* 1:1 (3x) */
            case CROP_PRESET_3X:
                /* start/stop scanning line, very large increments */
                /* note: these are two values, 6 bit each, trial and error */
                cmos_new[1] = (is_720p())
                    ? PACK12(13,10)     /* 720p,  almost centered */
                    : PACK12(11,11);    /* 1080p, almost centered */
                
                cmos_new[2] = 0x10E;    /* read every column, centered crop */
                cmos_new[6] = 0x170;    /* pink highlights without this */
                break;
            
            case CROP_PRESET_3X_TALL:
                cmos_new[1] =           /* vertical centering (trial and error) */
                    (video_mode_fps == 24) ? PACK12(8,13)  :
                    (video_mode_fps == 25) ? PACK12(8,12)  :
                    (video_mode_fps == 30) ? PACK12(9,11)  :
                    (video_mode_fps == 50) ? PACK12(12,10) :
                    (video_mode_fps == 60) ? PACK12(13,10) :
                                             (uint32_t) -1 ;
                cmos_new[2] = 0x10E;    /* horizontal centering (trial and error) */
                cmos_new[6] = 0x170;    /* pink highlights without this */
                break;

            /* 3x3 binning in 720p */
            /* 1080p it's already 3x3, don't change it */
            case CROP_PRESET_3x3_1X:
            case CROP_PRESET_3x3_1X_48p:
                if (is_720p())
                {
                    /* start/stop scanning line, very large increments */
                    cmos_new[1] =
                        (crop_preset == CROP_PRESET_3x3_1X_48p) ? PACK12(3,15) :
                        (video_mode_fps == 50)                  ? PACK12(4,14) :
                        (video_mode_fps == 60)                  ? PACK12(6,14) :
                                                                 (uint32_t) -1 ;
                    cmos_new[6] = 0x370;    /* pink highlights without this */
                }
                break;

            case CROP_PRESET_3K:
                cmos_new[1] =           /* vertical centering (trial and error) */
                    (video_mode_fps == 24) ? PACK12(8,12)  :
                    (video_mode_fps == 25) ? PACK12(8,12)  :
                    (video_mode_fps == 30) ? PACK12(9,11)  :
                    (video_mode_fps == 50) ? PACK12(13,10) :
                    (video_mode_fps == 60) ? PACK12(14,10) :    /* 13,10 has better centering, but overflows */
                                             (uint32_t) -1 ;
                cmos_new[2] = 0x0BE;    /* horizontal centering (trial and error) */
                cmos_new[6] = 0x170;    /* pink highlights without this */
                break;

            case CROP_PRESET_UHD:
                cmos_new[1] =
                    (video_mode_fps == 24) ? PACK12(10,12) :
                    (video_mode_fps == 25) ? PACK12(10,12) :
                    (video_mode_fps == 30) ? PACK12(11,11) :
                    (video_mode_fps == 50) ? PACK12(14,10) :
                    (video_mode_fps == 60) ? PACK12(13,9)  :
                                            (uint32_t) -1 ;
                cmos_new[2] = 0x08E;    /* horizontal centering (trial and error) */
                cmos_new[6] = 0x170;    /* pink highlights without this */
                break;

            case CROP_PRESET_4K_HFPS:
                cmos_new[1] =
                    (video_mode_fps == 24) ? PACK12(4,15)  :
                    (video_mode_fps == 25) ? PACK12(4,15)  :
                    (video_mode_fps == 30) ? PACK12(6,14)  :
                    (video_mode_fps == 50) ? PACK12(10,11) :
                    (video_mode_fps == 60) ? PACK12(12,11) :
                                             (uint32_t) -1 ;
                cmos_new[2] = 0x07E;    /* horizontal centering (trial and error) */
                cmos_new[6] = 0x170;    /* pink highlights without this */
                break;

            case CROP_PRESET_FULLRES_LV:
                cmos_new[1] = 0x800;    /* from photo mode */
                cmos_new[2] = 0x00E;    /* 8 in photo mode; E enables shutter speed control from ADTG 805E */
                cmos_new[6] = 0x170;    /* pink highlights without this */
                break;

            /* 1x3 binning (read every line, bin every 3 columns) */
            case CROP_PRESET_1x3:
                /* start/stop scanning line, very large increments */
                cmos_new[1] = (is_720p())
                    ? PACK12(14,10)     /* 720p,  almost centered */
                    : PACK12(11,11);    /* 1080p, almost centered */
                
                cmos_new[6] = 0x170;    /* pink highlights without this */
                break;

            /* 3x1 binning (bin every 3 lines, read every column) */
            case CROP_PRESET_3x1:
                cmos_new[2] = 0x10E;    /* read every column, centered crop */
                break;

            /* raw buffer centered in zoom mode */
            case CROP_PRESET_CENTER_Z:
                cmos_new[1] = PACK12(9+2,42+1); /* vertical (first|last) */
                cmos_new[2] = 0x09E;            /* horizontal offset (mask 0xFF0) */
                break;
        }
    }

    if (is_basic)
    {
        switch (crop_preset)
        {
            case CROP_PRESET_3x3_1X:
            if (is_720p())
            {
                /* start/stop scanning line, very large increments */
                cmos_new[7] = (is_6D) ? PACK12(37,10) : PACK12(6,29);
            }
            break; 
        }
    }

    if (is_70D)
    {
        switch (crop_preset)
        {
            case CROP_PRESET_1x3:
            if (is_1080p())
            {
                cmos_new[0xB] = 0x34A; // vertical offset
            }
            break;
            case CROP_PRESET_3x3_1X:
            if (is_720p())
            {
                /* FIXME: ghosty artifacts when pointing the camera to bright objects then pointing it to darker objects 
                 * https://www.magiclantern.fm/forum/index.php?topic=14309.msg205843#msg205843 */
                cmos_new[0xA] = 0x1F1; // vertical offset/line skipping related, value taken from 1080p mode
                cmos_new[0xB] = 0x307; // vertical offset
            }
            break; 
            case CROP_PRESET_CENTER_Z:
                cmos_new[7]   = 0xC80;          /* horizontal offset */
                cmos_new[0xb] = 0x289;          /* vertical offset */ 
            break; 
            case CROP_PRESET_UHD:
            if (is_x5_zoom()) {
                cmos_new[7]   = 0xC70;          /* horizontal offset */
                cmos_new[0xb] = 0x326;          /* vertical offset */  }
            break; 
        }
    }

    // 650D / 700D / EOSM/M2 / 100D presets
    // cmos_new[5] used for vertical offset, cmos_new[7] for horizontal offset
    if (is_DIGIC_5)
    {
        switch (crop_preset)
        {
            case CROP_PRESET_1X1:
            if (!is_720p() || !is_1080p())
            {
                if (CROP_2_5K)
                {
                    cmos_new[5] = 0x2C0;
                    cmos_new[7] = 0xA6A;
                }
                
                if (CROP_1440p)
                {
                    cmos_new[5] = 0x2C0;
                    cmos_new[7] = 0xAA9;
                }
                
                if (CROP_1620p)
                {
                    cmos_new[5] = 0x344 + reg_cmos5;
                    cmos_new[7] = 0xAC8 + reg_cmos7;
                }
                
                if (CROP_2_8K)
                {
                    cmos_new[5] = 0x280;
                    cmos_new[7] = 0xAA9;
                }
                
                if (CROP_3K)
                {
                    cmos_new[5] = 0x240;
                    cmos_new[7] = 0xAA9;
                }
                
                if (CROP_1280p || CROP_1080p)
                {
                    cmos_new[5] = 0x380;
                    cmos_new[7] = 0xACA;
                }
                
                if (CROP_Full_Res)
                {
                    cmos_new[5] = 0x0;
                    cmos_new[7] = 0x3A0;
                }
            }
            break;
            
            case CROP_PRESET_1X3:
            if (AR_16_9)
            {
                if (Anam_Highest)
                {
                    cmos_new[5] = 0xE0;
                    cmos_new[7] = 0xB44;
                }
                if (Anam_Higher)
                {
                    cmos_new[5] = 0x120;
                    cmos_new[7] = 0xB25;
                }
                if (Anam_Medium)
                {
                    cmos_new[5] = 0x1A0;
                    cmos_new[7] = 0xB46;
                }
            }
            if (AR_2_1)
            {
                if (Anam_Highest)
                {
                    cmos_new[5] = 0xA0;
                    cmos_new[7] = 0xB44;
                }
                if (Anam_Higher)
                {
                    cmos_new[5] = 0x120;
                    cmos_new[7] = 0xB25;
                }
                if (Anam_Medium)
                {
                    cmos_new[5] = 0x160;
                    cmos_new[7] = 0xB46;
                }
            }
            if (AR_2_20_1)
            {
                if (Anam_Highest)
                {
                    cmos_new[5] = 0x60;
                    cmos_new[7] = 0xB25;
                }
                if (Anam_Higher)
                {
                    cmos_new[5] = 0xA0;
                    cmos_new[7] = 0xB26;
                }
                if (Anam_Medium)
                {
                    cmos_new[5] = 0x120;
                    cmos_new[7] = 0xB26;
                }
            }
            if (AR_2_35_1 || AR_2_39_1)
            {
                if (Anam_Highest)
                {
                    cmos_new[5] = 0x20;
                    cmos_new[7] = 0xB05;
                }
                if (Anam_Higher)
                {
                    cmos_new[5] = 0xA0;
                    cmos_new[7] = 0xB26;
                }
                if (Anam_Medium)
                {
                    cmos_new[5] = 0xE0;
                    cmos_new[7] = 0xB27;
                }
            }
                if (Anam_FLV)
                {
                    cmos_new[5] = 0x20 + CMOS_5_Debug;
                    cmos_new[7] = 0xC00 + CMOS_7_Debug;
                }
            break;

            case CROP_PRESET_3X3:
                if (High_FPS)
                {
                    if (AR_16_9)   cmos_new[7] = 0x802;
                    if (AR_2_1    || 
                        AR_2_20_1) cmos_new[7] = 0x804;
                    if (AR_2_35_1 ||
                        AR_2_39_1) cmos_new[7] = 0x806; // AR_2_39_1 is actually 2.50:1 preset
                }
                if (mv1080 || mv1080_3_2) cmos_new[7] = 0x800;
                cmos_new[5] = 0x20;
            break;
        }
    }

    /* menu overrides */
    if (cmos1_lo || cmos1_hi)
    {
        cmos_new[1] = PACK12(cmos1_lo,cmos1_hi);
    }

    if (cmos2)
    {
        cmos_new[2] = cmos2;
    }
    
    /* copy data into a buffer, to make the override temporary */
    /* that means: as soon as we stop executing the hooks, values are back to normal */
    static uint16_t copy[512];
    uint16_t* copy_end = &copy[COUNT(copy)];
    uint16_t* copy_ptr = copy;

    while (*data_buf != 0xFFFF)
    {
        *copy_ptr = *data_buf;

        int reg = (*data_buf) >> 12;
        if (cmos_new[reg] != -1)
        {
            *copy_ptr = (reg << 12) | cmos_new[reg];
            dbg_printf("CMOS[%x] = %x\n", reg, cmos_new[reg]);
        }

        data_buf++;
        copy_ptr++;
        if (copy_ptr > copy_end) while(1);
    }
    *copy_ptr = 0xFFFF;

    /* pass our modified register list to cmos_write */
    regs[0] = (uint32_t) copy;
}

static uint32_t nrzi_encode( uint32_t in_val )
{
    uint32_t out_val = 0;
    uint32_t old_bit = 0;
    for (int num = 0; num < 31; num++)
    {
        uint32_t bit = in_val & 1<<(30-num) ? 1 : 0;
        if (bit != old_bit)
            out_val |= (1 << (30-num));
        old_bit = bit;
    }
    return out_val;
}

static uint32_t nrzi_decode( uint32_t in_val )
{
    uint32_t val = 0;
    if (in_val & 0x8000)
        val |= 0x8000;
    for (int num = 0; num < 31; num++)
    {
        uint32_t old_bit = (val & 1<<(30-num+1)) >> 1;
        val |= old_bit ^ (in_val & 1<<(30-num));
    }
    return val;
}

static int FAST adtg_lookup(uint32_t* data_buf, int reg_needle)
{
    while(*data_buf != 0xFFFFFFFF)
    {
        int reg = (*data_buf) >> 16;
        if (reg == reg_needle)
        {
            return *(uint16_t*)data_buf;
        }
    }
    return -1;
}

/* adapted from fps_override_shutter_blanking in fps-engio.c */
static int adjust_shutter_blanking(int old)
{
    /* sensor duty cycle: range 0 ... timer B */
    int current_blanking = nrzi_decode(old);
    
    static int previous_blanking = -1;
    
    if (ABS(current_blanking - previous_blanking) == 1) 
    {
        current_blanking = previous_blanking;
    } 
    else 
    {
       previous_blanking = current_blanking;
    }

    int video_mode = get_video_mode_index();

    /* what value Canon firmware assumes for timer B? */
    int fps_timer_b_orig = default_timerB[video_mode];

    int current_exposure = fps_timer_b_orig - current_blanking;
    
    /* wrong assumptions? */
    if (current_exposure < 0)
    {
        return old;
    }

    int default_fps = default_fps_1k[video_mode];
    int current_fps = fps_get_current_x1000();

    dbg_printf("FPS %d->%d\n", default_fps, current_fps);

    float frame_duration_orig = 1000.0 / default_fps;
    float frame_duration_current = 1000.0 / current_fps;

    float orig_shutter = frame_duration_orig * current_exposure / fps_timer_b_orig;

    float new_shutter =
        (shutter_range == 0) ?
        ({
            /* original shutter speed from the altered video mode */
            orig_shutter;
        }) :
        ({
            /* map the available range of 1/4000...1/30 (24-30p) or 1/4000...1/60 (50-60p)
             * from minimum allowed (1/15000 with full-res LV) to 1/fps */
            int max_fps_shutter = (video_mode_fps <= 30) ? 33333 : 64000;
            int default_fps_adj = 1e9 / (1e9 / max_fps_shutter - 250);
            (orig_shutter - 250e-6) * default_fps_adj / current_fps;
        });

    /* what value is actually used for timer B? (possibly after our overrides) */
    int fps_timer_b = (shamem_read(0xC0F06014) & 0xFFFF) + 1;

    dbg_printf("Timer B %d->%d\n", fps_timer_b_orig, fps_timer_b);

    int new_exposure = new_shutter * fps_timer_b / frame_duration_current;
    int new_blanking = COERCE(fps_timer_b - new_exposure, 10, fps_timer_b - 2);

    dbg_printf("Exposure %d->%d (timer B units)\n", current_exposure, new_exposure);

#ifdef CROP_DEBUG
    float chk_shutter = frame_duration_current * new_exposure / fps_timer_b;
    dbg_printf("Shutter %d->%d us\n", (int)(orig_shutter*1e6), (int)(chk_shutter*1e6));
#endif

    dbg_printf("Blanking %d->%d\n", current_blanking, new_blanking);

    return nrzi_encode(new_blanking);
}

extern void fps_override_shutter_blanking();
static int shutter_blanking_idle;

static void FAST adtg_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    if (!is_supported_mode())
    {
        /* don't patch other video modes */
        return;
    }

    if (!is_720p())
    {
        if (crop_preset == CROP_PRESET_3x3_1X ||
            crop_preset == CROP_PRESET_3x3_1X_48p)
        {
            /* these presets only have effect in 720p mode */
            return;
        }
    }

    /* This hook is called from the DebugMsg's in adtg_write,
     * so if we change the register list address, it won't be able to override them.
     * Workaround: let's call it here. */
    fps_override_shutter_blanking();

    uint32_t cs = regs[0];
    uint32_t *data_buf = (uint32_t *) regs[1];
    int dst = cs & 0xF;
    
    /* copy data into a buffer, to make the override temporary */
    /* that means: as soon as we stop executing the hooks, values are back to normal */
    static uint32_t copy[512];
    uint32_t* copy_end = &copy[COUNT(copy)];
    uint32_t* copy_ptr = copy;
    
    struct adtg_new
    {
        int dst;
        int reg;
        int val;
    };
    
    /* expand this as required */
    struct adtg_new adtg_new[24] = {{0}};

    /* scan for shutter blanking and make both zoom and non-zoom value equal */
    /* (the values are different when using FPS override with ADTG shutter override) */
    /* (fixme: might be better to handle this in ML core?) */
    /* also scan for one ADTG gain register and get its value to be used for lower bit-depth */
    /* this is similair method to lowering digital gain to get "fake" lossless compression 
       in lower bit-depths, digital gain method only works with native RAW resolutions       */
    /* we will use [ADTG2/4] 8882/8884/8886/8888 registers, they change values slightly with ISO changes */
    int shutter_blanking = 0;
    int analog_gain = 0;
    
    const int blanking_reg_zoom   = (is_5D3) ? 0x805E : 0x805F;
    const int blanking_reg_nozoom = (is_5D3) ? 0x8060 : 0x8061;
    const int blanking_reg        = (lv_dispsize == 1) ? blanking_reg_nozoom : blanking_reg_zoom;
    
    int adtg_analog_gain_reg = 0x8882;
    for (uint32_t * buf = data_buf; *buf != 0xFFFFFFFF; buf++)
    {
        int reg = (*buf) >> 16;
        if (reg == blanking_reg)
        {
            int val = (*buf) & 0xFFFF;
            shutter_blanking = val;
        }
        if (reg == adtg_analog_gain_reg)
        {
            int val = (*buf) & 0xFFFF;
            analog_gain = val;
        }
    }

    /* some modes may need adjustments to maintain exposure */
    if (shutter_blanking)
    {
        /* FIXME: remove this kind of hardcoded conditions */
        if ((crop_preset == CROP_PRESET_CENTER_Z && lv_dispsize != 1) ||
            (crop_preset != CROP_PRESET_CENTER_Z && lv_dispsize == 1) || is_DIGIC_5 || is_70D)
        {
            shutter_blanking = adjust_shutter_blanking(shutter_blanking);
        }
    }

    /* a workaround to fix shutter fine-tuning when using more hacks (suspending AeWb task) in mlv_lite */
    /* we need to save last shutter_blanking value into a variable before suspending AeWb task */
    if (!AeWbTask_Disabled())
    {
        /* save shutter_blanking value when shutter_blanking value isn't 0 */
        if (shutter_blanking) shutter_blanking_idle = shutter_blanking;
    }

    /* always use our saved shutter blanking value when "More" hacks is selected (i.e when AeWb is suspended) */
    /* FIXME: exclude this method when shutter-fine isn't used */
    if (is_more_hacks_selected() && AeWbTask_Disabled())
    {
        /* change shutter_blanking value to our saved value when shutter_blanking value isn't 0 */
        if (shutter_blanking) shutter_blanking = shutter_blanking_idle;
    }

    /* all modes may want to override shutter speed */
    /* ADTG[0x8060]: shutter blanking for 3x3 mode  */
    /* ADTG[0x805E]: shutter blanking for zoom mode  */
    adtg_new[0] = (struct adtg_new) {6, blanking_reg_nozoom, shutter_blanking};
    adtg_new[1] = (struct adtg_new) {6, blanking_reg_zoom, shutter_blanking};   

    /* hopefully generic; to be tested later */
    if (1)
    {
        switch (crop_preset)
        {
            /* all 1:1 modes (3x, 3K, 4K...) */
            case CROP_PRESET_3X:
            case CROP_PRESET_3X_TALL:
            case CROP_PRESET_3K:
            case CROP_PRESET_UHD:
            case CROP_PRESET_4K_HFPS:
            case CROP_PRESET_FULLRES_LV:
                /* ADTG2/4[0x8000] = 5 (set in one call) */
                /* ADTG2[0x8806] = 0x6088 on 5D3 (artifacts without it) */
                if (!is_70D) adtg_new[2] = (struct adtg_new) {6, 0x8000, 5};
                if (is_5D3) {
                    /* this register is model-specific */
                    adtg_new[3] = (struct adtg_new) {2, 0x8806, 0x6088};
                }
                break;

            /* 3x3 binning in 720p (in 1080p it's already 3x3) */
            case CROP_PRESET_3x3_1X:
            case CROP_PRESET_3x3_1X_48p:
                /* ADTG2/4[0x800C] = 2: vertical binning factor = 3 */
                adtg_new[2] = (struct adtg_new) {6, 0x800C, 2};
                break;

            /* 1x3 binning (read every line, bin every 3 columns) */
            case CROP_PRESET_1x3:
                /* ADTG2/4[0x800C] = 0: read every line */
                if ((is_70D && is_1080p()) || is_5D3) adtg_new[2] = (struct adtg_new) {6, 0x800C, 0};
                break;

            /* 3x1 binning (bin every 3 lines, read every column) */
            case CROP_PRESET_3x1:
                /* ADTG2/4[0x800C] = 2: vertical binning factor = 3 */
                /* ADTG2[0x8806] = 0x6088 on 5D3 (artifacts worse without it) */
                /* ADTG2[0x8183]/[0x8184] used for horizontal binning, artifacts without it */
                /* FIXME: ADTG2[0x8183]/[0x8184] won't be overridden until we go outside LV 
                          then get back, maybe because Canon only update them once?        */
                adtg_new[2] = (struct adtg_new) {6, 0x800C, 2};
                adtg_new[3] = (struct adtg_new) {2, 0x8183, 0};
                adtg_new[4] = (struct adtg_new) {2, 0x8184, 0};
                if (is_5D3) {
                    /* this register is model-specific */
                    adtg_new[5] = (struct adtg_new) {2, 0x8806, 0x6088};
                }
                break;
        }
        
        // 650D / 700D / EOSM/M2 / 100D presets
        // ADTG2[0x8183] and ADTG2[0x8184] enable horizontal pixel binning instead of skipping
        // in 1080p ADTG2[0x8183] = 0x21, ADTG2[0x8183] = 0x7B, in x5 both are = 0x0 
        // ADTG2[0x800C] = 2: vertical binning/skipping factor = 3, ADTG2[0x800C] = 0 read all vertical lines
        if (is_DIGIC_5)
        {
            switch (crop_preset)
            {
                case CROP_PRESET_1X3:
                if (is_650D || is_700D || is_100D || is_EOSM)
                {
                    adtg_new[2] = (struct adtg_new) {2, 0x800C, 0};
                    adtg_new[3] = (struct adtg_new) {2, 0x8000, 0x6};
                    adtg_new[4] = (struct adtg_new) {2, 0x8183, 0x21};
                    adtg_new[5] = (struct adtg_new) {2, 0x8184, 0x7B};
                    
                    if (is_100D)
                    {
                    /*  Artifacts without these on certain 100D bodies:
                        https://www.magiclantern.fm/forum/index.php?topic=26511.msg239495#msg239495
                        https://www.magiclantern.fm/forum/index.php?topic=26511.msg239557#msg239557  */

                        adtg_new[6]  = (struct adtg_new) {2, 0xc00d, 0x5000};
                        adtg_new[7]  = (struct adtg_new) {2, 0xc00e, 0x53};
                        adtg_new[8]  = (struct adtg_new) {2, 0xc00f, 0x52};
                        adtg_new[9]  = (struct adtg_new) {2, 0xc010, 0x52};
                        adtg_new[10] = (struct adtg_new) {2, 0xc011, 0x52};
                    }
                }
                break;
                
                case CROP_PRESET_3X3:
                if (is_650D || is_700D || is_100D || is_EOSM)
                {
                    adtg_new[2] = (struct adtg_new) {2, 0x800C, 0x2};
                    adtg_new[3] = (struct adtg_new) {2, 0x8000, 0x6};
                    adtg_new[4] = (struct adtg_new) {2, 0x8183, 0x21};
                    adtg_new[5] = (struct adtg_new) {2, 0x8184, 0x7B};
                }
                break; 
            }
        }

        /* PowerSaveTiming & ReadOutTiming registers */
        /* these need changing in all modes with higher vertical resolution */
        switch (crop_preset)
        {
            case CROP_PRESET_3X_TALL:
            case CROP_PRESET_3x3_1X:
            case CROP_PRESET_3x3_1X_48p:
            case CROP_PRESET_1x3:
            case CROP_PRESET_3K:
            case CROP_PRESET_UHD:
            case CROP_PRESET_4K_HFPS:
            case CROP_PRESET_FULLRES_LV:
            case CROP_PRESET_40_FPS:
            case CROP_PRESET_1X1:
            case CROP_PRESET_1X3:
            case CROP_PRESET_3X3:
            {
                /* assuming FPS timer B was overridden before this */
                int fps_timer_b = (shamem_read(0xC0F06014) & 0xFFFF) + 1;
                int readout_end = shamem_read(0xC0F06804) >> 16;    /* fixme: D5 only */

                /* PowerSaveTiming registers */
                /* after readout is finished, we can turn off the sensor until the next frame */
                /* we could also set these to 0; it will work, but the sensor will run a bit hotter */
                /* to be tested to find out exactly how much */
                adtg_new[11] = (struct adtg_new) {6, 0x8172, nrzi_encode(readout_end + 1) }; /* PowerSaveTiming ON (6D/700D) */
                adtg_new[12] = (struct adtg_new) {6, 0x8178, nrzi_encode(readout_end + 1) }; /* PowerSaveTiming ON (5D3/6D/700D) */
                adtg_new[13] = (struct adtg_new) {6, 0x8196, nrzi_encode(readout_end + 1) }; /* PowerSaveTiming ON (5D3) */

                adtg_new[14] = (struct adtg_new) {6, 0x8173, nrzi_encode(fps_timer_b - 5) }; /* PowerSaveTiming OFF (6D/700D) */
                adtg_new[15] = (struct adtg_new) {6, 0x8179, nrzi_encode(fps_timer_b - 5) }; /* PowerSaveTiming OFF (5D3/6D/700D) */
                adtg_new[16] = (struct adtg_new) {6, 0x8197, nrzi_encode(fps_timer_b - 5) }; /* PowerSaveTiming OFF (5D3) */

                adtg_new[17] = (struct adtg_new) {6, 0x82B6, nrzi_encode(readout_end - 1) }; /* PowerSaveTiming ON? (700D); 2 units below the "ON" timing from above */

                /* ReadOutTiming registers */
                /* these shouldn't be 0, as they affect the image */
                adtg_new[18] = (struct adtg_new) {6, 0x82F8, nrzi_encode(readout_end + 1) }; /* ReadOutTiming */
                adtg_new[19] = (struct adtg_new) {6, 0x82F9, nrzi_encode(fps_timer_b - 1) }; /* ReadOutTiming end? */
                break;
            }
        }
    }
    
    /* divid signal to achieve lower bit-depths using negative analog gain */
    if (which_output_format() >= 3) // don't patch if uncompressed RAW is selected
    {
        if (OUTPUT_14BIT)
        {
            BitDepth_Analog = 14;
        }
        
        if (RECORDING || RAW_HISTOGRAM_ENABLED)//When RAW histogram is used turn off the temporary 14bit stuff
        {
            
            if (OUTPUT_12BIT)
            {
                adtg_new[20] = (struct adtg_new) {6, 0x8882, analog_gain / 4};
                adtg_new[21] = (struct adtg_new) {6, 0x8884, analog_gain / 4};
                adtg_new[22] = (struct adtg_new) {6, 0x8886, analog_gain / 4};
                adtg_new[23] = (struct adtg_new) {6, 0x8888, analog_gain / 4};
                BitDepth_Analog = 12;
            }
            
            if (OUTPUT_11BIT)
            {
                adtg_new[20] = (struct adtg_new) {6, 0x8882, analog_gain / 8};
                adtg_new[21] = (struct adtg_new) {6, 0x8884, analog_gain / 8};
                adtg_new[22] = (struct adtg_new) {6, 0x8886, analog_gain / 8};
                adtg_new[23] = (struct adtg_new) {6, 0x8888, analog_gain / 8};
                BitDepth_Analog = 11;
            }
            
            if (OUTPUT_10BIT)
            {
                adtg_new[20] = (struct adtg_new) {6, 0x8882, analog_gain / 16};
                adtg_new[21] = (struct adtg_new) {6, 0x8884, analog_gain / 16};
                adtg_new[22] = (struct adtg_new) {6, 0x8886, analog_gain / 16};
                adtg_new[23] = (struct adtg_new) {6, 0x8888, analog_gain / 16};
                BitDepth_Analog = 10;
            }
        }
    }

    while(*data_buf != 0xFFFFFFFF)
    {
        *copy_ptr = *data_buf;
        int reg = (*data_buf) >> 16;
        for (int i = 0; i < COUNT(adtg_new); i++)
        {
            if ((reg == adtg_new[i].reg) && (dst & adtg_new[i].dst))
            {
                int new_value = adtg_new[i].val;
                dbg_printf("ADTG%x[%x] = %x\n", dst, reg, new_value);
                *(uint16_t*)copy_ptr = new_value;

                if (reg == blanking_reg_zoom || reg == blanking_reg_nozoom)
                {
                    /* also override in original data structure */
                    /* to be picked up on the screen indicators */
                    *(uint16_t*)data_buf = new_value;
                }
            }
        }
        data_buf++;
        copy_ptr++;
        if (copy_ptr >= copy_end) while(1);
    }
    *copy_ptr = 0xFFFFFFFF;
    
    /* pass our modified register list to adtg_write */
    regs[1] = (uint32_t) copy;
}

int analog_gain_is_acive()
{
    if (CROP_PRESET_MENU) // analog gain is only active when preset is selected
    {
        if (bit_depth_analog == 1)
        {
            return 1;
        }
    
        if (bit_depth_analog == 2)
        {
            return 2;
        }
    
        if (bit_depth_analog == 3)
        {
            return 3;
        }
    }
   
    return 0;
}

int crop_rec_is_enabled()
{
    if (CROP_PRESET_MENU)
    {
        return 1;
    }
    
    return 0;
}

/* this is used to cover the black bar at the top of the image in 1:1 modes */
/* (used in most other presets) */
static inline uint32_t reg_override_top_bar(uint32_t reg, uint32_t old_val)
{
    switch (reg)
    {
        /* raw start line/column */
        /* move start line down by 30 pixels */
        /* not sure where this offset comes from */
        case 0xC0F06800:
            return 0x1F0017;
    }

    return 0;
}

/* these are required for increasing vertical resolution */
/* (used in most other presets) */
static inline uint32_t reg_override_HEAD34(uint32_t reg, uint32_t old_val)
{
    switch (reg)
    {
        /* HEAD3 timer */
        case 0xC0F0713C:
            return old_val + YRES_DELTA + delta_head3;

        /* HEAD4 timer */
        case 0xC0F07150:
            return old_val + YRES_DELTA + delta_head4;
    }

    return 0;
}

static inline uint32_t reg_override_common(uint32_t reg, uint32_t old_val)
{
    uint32_t a = reg_override_top_bar(reg, old_val);
    if (a) return a;

    uint32_t b = reg_override_HEAD34(reg, old_val);
    if (b) return b;

    return 0;
}

static inline uint32_t reg_override_fps(uint32_t reg, uint32_t timerA, uint32_t timerB, uint32_t old_val)
{
    /* hardware register requires timer-1 */
    timerA--;
    timerB--;

    /* only override FPS registers if the old value is what we expect
     * otherwise we may be in some different video mode for a short time
     * this race condition is enough to lock up LiveView in some cases
     * e.g. 5D3 3x3 50/60p when going from photo mode to video mode
     */

    switch (reg)
    {
        case 0xC0F06824:
        case 0xC0F06828:
        case 0xC0F0682C:
        case 0xC0F06830:
        case 0xC0F06010:
        {
            uint32_t expected = default_timerA[get_video_mode_index()] - 1;

            if (old_val == expected || old_val == expected + 1)
            {
                return timerA;
            }

            break;
        }
        
        case 0xC0F06008:
        case 0xC0F0600C:
        {
            uint32_t expected = default_timerA[get_video_mode_index()] - 1;
            expected |= (expected << 16);

            if (old_val == expected || old_val == expected + 0x00010001)
            {
                return timerA | (timerA << 16);
            }

            break;
        }

        case 0xC0F06014:
        {
            uint32_t expected = default_timerB[get_video_mode_index()] - 1;

            if (old_val == expected || old_val == expected + 1)
            {
                return timerB;
            }

            break;
        }
    }

    return 0;
}

static inline uint32_t reg_override_3X_tall(uint32_t reg, uint32_t old_val)
{
    /* change FPS timers to increase vertical resolution */
    if (video_mode_fps >= 50)
    {
        int timerA = 400;

        int timerB =
            (video_mode_fps == 50) ? 1200 :
            (video_mode_fps == 60) ? 1001 :
                                       -1 ;

        int a = reg_override_fps(reg, timerA, timerB, old_val);
        if (a) return a;
    }

    /* fine-tuning head timers appears to help
     * pushing the resolution a tiny bit further */
    int head_adj =
        (video_mode_fps == 50) ? -30 :
        (video_mode_fps == 60) ? -20 :
                                   0 ;

    switch (reg)
    {
        /* raw resolution (end line/column) */
        case 0xC0F06804:
            return old_val + (YRES_DELTA << 16);

        /* HEAD3 timer */
        case 0xC0F0713C:
            return old_val + YRES_DELTA + delta_head3 + head_adj;

        /* HEAD4 timer */
        case 0xC0F07150:
            return old_val + YRES_DELTA + delta_head4 + head_adj;
    }

    return reg_override_common(reg, old_val);
}

static inline uint32_t reg_override_3x3_tall(uint32_t reg, uint32_t old_val)
{
    if (!is_720p() || !is_5D3)
    {
        /* 1080p not patched in 3x3 */
        return 0;
    }

    /* change FPS timers to increase vertical resolution */
    if (video_mode_fps >= 50)
    {
        int timerA = 400;

        int timerB =
            (video_mode_fps == 50) ? 1200 :
            (video_mode_fps == 60) ? 1001 :
                                       -1 ;

        int a = reg_override_fps(reg, timerA, timerB, old_val);
        if (a) return a;
    }

    /* fine-tuning head timers appears to help
     * pushing the resolution a tiny bit further */
    int head_adj =
        (video_mode_fps == 50) ? -10 :
        (video_mode_fps == 60) ? -20 :
                                   0 ;

    switch (reg)
    {
        /* for some reason, top bar disappears with the common overrides */
        /* very tight fit - every pixel counts here */
        case 0xC0F06800:
            return 0x1D0017;

        /* raw resolution (end line/column) */
        case 0xC0F06804:
            return old_val + (YRES_DELTA << 16);

        /* HEAD3 timer */
        case 0xC0F0713C:
            return old_val + YRES_DELTA + delta_head3 + head_adj;

        /* HEAD4 timer */
        case 0xC0F07150:
            return old_val + YRES_DELTA + delta_head4 + head_adj;
    }

    return reg_override_common(reg, old_val);
}

static inline uint32_t reg_override_3x3_48p(uint32_t reg, uint32_t old_val)
{
    if (!is_720p())
    {
        /* 1080p not patched in 3x3 */
        return 0;
    }

    /* change FPS timers to increase vertical resolution */
    if (video_mode_fps >= 50)
    {
        int timerA =
            (video_mode_fps == 50) ? 401 :
            (video_mode_fps == 60) ? 400 :
                                      -1 ;
        int timerB =
            (video_mode_fps == 50) ? 1330 : /* 45p */
            (video_mode_fps == 60) ? 1250 : /* 48p */
                                       -1 ;

        int a = reg_override_fps(reg, timerA, timerB, old_val);
        if (a) return a;
    }

    switch (reg)
    {
        /* for some reason, top bar disappears with the common overrides */
        /* very tight fit - every pixel counts here */
        case 0xC0F06800:
            return 0x1D0017;

        /* raw resolution (end line/column) */
        case 0xC0F06804:
            return old_val + (YRES_DELTA << 16);

        /* HEAD3 timer */
        /* 2E6 in 50p, 2B4 in 60p */
        case 0xC0F0713C:
            return 0x2B4 + YRES_DELTA + delta_head3;

        /* HEAD4 timer */
        /* 2B4 in 50p, 26D in 60p */
        case 0xC0F07150:
            return 0x26D + YRES_DELTA + delta_head4;
    }

    return reg_override_common(reg, old_val);
}

static inline uint32_t reg_override_3K(uint32_t reg, uint32_t old_val)
{
    /* FPS timer A, for increasing horizontal resolution */
    /* 25p uses 480 (OK), 24p uses 440 (too small); */
    /* only override in 24p, 30p and 60p modes */
    if (video_mode_fps != 25 && video_mode_fps !=  50)
    {
        int timerA = 455;
        int timerB =
            (video_mode_fps == 24) ? 2200 :
            (video_mode_fps == 30) ? 1760 :
            (video_mode_fps == 60) ?  880 :
                                       -1 ;

        int a = reg_override_fps(reg, timerA, timerB, old_val);
        if (a) return a;
    }

    switch (reg)
    {
        /* raw resolution (end line/column) */
        /* X: (3072+140)/8 + 0x17, adjusted for 3072 in raw_rec */
        case 0xC0F06804:
            return (old_val & 0xFFFF0000) + 0x1AA + (YRES_DELTA << 16);

    }

    return reg_override_common(reg, old_val);
}

static inline uint32_t reg_override_4K_hfps(uint32_t reg, uint32_t old_val)
{
    /* FPS timer A, for increasing horizontal resolution */
    /* trial and error to allow 4096; 572 is too low, 576 looks fine */
    /* pick some values with small roundoff error */
    int timerA =
        (video_mode_fps < 30)  ?  585 : /* for 23.976/2 and 25/2 fps */
                                  579 ; /* for all others */

    /* FPS timer B, tuned to get half of the frame rate from Canon menu */
    int timerB =
        (video_mode_fps == 24) ? 3422 :
        (video_mode_fps == 25) ? 3282 :
        (video_mode_fps == 30) ? 2766 :
        (video_mode_fps == 50) ? 1658 :
        (video_mode_fps == 60) ? 1383 :
                                   -1 ;

    int a = reg_override_fps(reg, timerA, timerB, old_val);
    if (a) return a;

    switch (reg)
    {
        /* raw resolution (end line/column) */
        /* X: (4096+140)/8 + 0x18, adjusted for 4096 in raw_rec */
        case 0xC0F06804:
            return (old_val & 0xFFFF0000) + 0x22A + (YRES_DELTA << 16);
    }

    return reg_override_common(reg, old_val);
}

static inline uint32_t reg_override_UHD(uint32_t reg, uint32_t old_val)
{
    if (is_5D3)
    {
        /* FPS timer A, for increasing horizontal resolution */
        /* trial and error to allow 3840; 536 is too low */
        int timerA = 
            (video_mode_fps == 25) ? 547 :
            (video_mode_fps == 50) ? 546 :
                                 550 ;
        int timerB =
            (video_mode_fps == 24) ? 1820 :
            (video_mode_fps == 25) ? 1755 :
            (video_mode_fps == 30) ? 1456 :
            (video_mode_fps == 50) ?  879 :
            (video_mode_fps == 60) ?  728 :
                                   -1 ;

        int a = reg_override_fps(reg, timerA, timerB, old_val);
        if (a) return a;

        switch (reg)
        {
            /* raw resolution (end line/column) */
            /* X: (3840+140)/8 + 0x18, adjusted for 3840 in raw_rec */
            case 0xC0F06804:
                return (old_val & 0xFFFF0000) + 0x20A + (YRES_DELTA << 16);
        }

        return reg_override_common(reg, old_val);
    }

    if (is_70D)
    {
       if (!is_x5_zoom())
        {
            /* don't patch other modes */
            return 0;
        }

        int RAW_V = 0x896;
        int RAW_H = 0x201;
        int TimerA = 0x224;
        int TimerB = 0x97E;

        switch (reg)
        {
            case 0xC0F06804: return (RAW_V << 16) + RAW_H;

            case 0xC0F06824:
            case 0xC0F06828:
            case 0xC0F0682C:
            case 0xC0F06830:
            {
                return TimerA - 1;
            }

            case 0xC0F0713C: return RAW_V + 0x1;
            case 0xC0F07150: return RAW_V - 0x3A;
            case 0xC0F07064: return RAW_V + 0xB2;

            case 0xC0F06014: return TimerB;
            case 0xC0F06024: return TimerB;
            case 0xC0F06010: return TimerA;
            case 0xC0F06008: return TimerA + (TimerA << 16);
            case 0xC0F0600C: return TimerA + (TimerA << 16);
        }
    }

    return 0;
}

static inline uint32_t reg_override_fullres_lv(uint32_t reg, uint32_t old_val)
{
    switch (reg)
    {
        case 0xC0F06800:
            return 0x10018;         /* raw start line/column, from photo mode */
        
        case 0xC0F06804:            /* 1080p 0x528011B, photo 0xF6E02FE */
            return (old_val & 0xFFFF0000) + 0x2FE + (YRES_DELTA << 16);
        
        case 0xC0F06824:
        case 0xC0F06828:
        case 0xC0F0682C:
        case 0xC0F06830:
            return 0x312;           /* from photo mode */
        
        case 0xC0F06010:            /* FPS timer A, for increasing horizontal resolution */
            return 0x317;           /* from photo mode; lower values give black border on the right */
        
        case 0xC0F06008:
        case 0xC0F0600C:
            return 0x3170317;

        case 0xC0F06014:
            return (video_mode_fps > 30 ? 856 : 1482) + YRES_DELTA;   /* up to 7.4 fps */
    }

    /* no need to adjust the black bar */
    return reg_override_HEAD34(reg, old_val);
}

/* just for testing */
/* (might be useful for FPS override on e.g. 70D) */
static inline uint32_t reg_override_40_fps(uint32_t reg, uint32_t old_val)
{
    switch (reg)
    {
        case 0xC0F06824:
        case 0xC0F06828:
        case 0xC0F0682C:
        case 0xC0F06830:
        case 0xC0F06010:
            return 0x18F;
        
        case 0xC0F06008:
        case 0xC0F0600C:
            return 0x18F018F;

        case 0xC0F06014:
            return 0x5DB;
    }

    return 0;
}

static inline uint32_t reg_override_fps_nocheck(uint32_t reg, uint32_t timerA, uint32_t timerB, uint32_t old_val)
{
    /* hardware register requires timer-1 */
    timerA--;
    timerB--;

    switch (reg)
    {
        case 0xC0F06824:
        case 0xC0F06828:
        case 0xC0F0682C:
        case 0xC0F06830:
        case 0xC0F06010:
        {
            return timerA;
        }
        
        case 0xC0F06008:
        case 0xC0F0600C:
        {
            return timerA | (timerA << 16);
        }

        case 0xC0F06014:
        {
            return timerB;
        }
    }

    return 0;
}

static inline uint32_t reg_override_zoom_fps(uint32_t reg, uint32_t old_val)
{
    int timerA = -1;
    int timerB = -1;

    /* attempt to reconfigure the x5 zoom at the FPS selected in Canon menu */
    if (video_mode_fps == 24)
    {
        if (is_5D3) { timerA = 512; timerB = 1955; }
        if (is_70D) { timerA = 503; timerB = 2653; }
    }
    if (video_mode_fps == 25)
    {
        if (is_5D3) { timerA = 512; timerB = 1875; }
        if (is_70D) { timerA = 503; timerB = 2544; }
    }
    if (video_mode_fps == 30)
    {
        if (is_5D3) { timerA = 520; timerB = 1540; }
        if (is_70D) { timerA = 503; timerB = 2122; }
    }
    if (video_mode_fps == 50)
    {
        if (is_5D3) { timerA = 512; timerB = 1875; } /* cannot get 50, use 25 */
        if (is_70D) { timerA = 503; timerB = 1588; } /* cannot get 50, use 40 */
    }
    if (video_mode_fps == 60)
    {
        if (is_5D3) { timerA = 520; timerB = 1540; } /* cannot get 60, use 30 */
        if (is_70D) { timerA = 503; timerB = 1588; } /* cannot get 60, use 40 */
    }

    return reg_override_fps_nocheck(reg, timerA, timerB, old_val);
}

/* adjust Timer B to make scanning Dual-ISO lines static  */
/* Timer B value should be in 4 increment  */
int Adjust_TimerB_For_Dual_ISO(int TimerB)
{   TimerB = (TimerB / 4) * 4;
    return TimerB + 3;
}

/* 650D / 700D / EOSM/M2 / 100D reg_override presets */

int preview_debug_1 = 0;
int preview_debug_2 = 0;
int preview_debug_3 = 0;
int preview_debug_4 = 0;

int RAW_H_Debug  = 0;
int RAW_V_Debug  = 0;
int TimerA_Debug = 0;
int TimerB_Debug = 0;

static unsigned TimerB = 0;
static unsigned TimerA = 0;

static unsigned RAW_H = 0;            // RAW width    resolution          0xC0F06804
static unsigned RAW_V = 0;            // RAW vertical resolution          0xC0F06804

static unsigned Preview_Control = 0;        // Flag tells we want full real-time preview
static unsigned Preview_Control_Basic = 0;  // Flag tells we want basic preview (cropped preview)

/* used to center preview on RAW buffer in presets which have basic preview (cropped preview) */
static unsigned Preview_x1 = 0;
static unsigned Preview_x2 = 0;
static unsigned Preview_y1 = 0;
static unsigned Preview_y2 = 0;

/* help to shift preview on RAW buffer horizontally in case if active preview width is less than active RAW width */
/* it should be tweaked with 0xC0F383D4 (Preview_R) which also shift preview on RAW buffer (check Preview_Control_Basic) */
static int REG_C0F383DC_Tuning = 0;

/* used to increase processed RAW data in LiveView, also show new image on screen via stretch regs */
static unsigned Preview_H = 0;        // How much width to process        List of registers
static unsigned Preview_V = 0;        // How much height to process       List of registers
static unsigned Preview_R = 0;        // Preview related                  0xC0F383D4
static unsigned YUV_HD_S_H = 0;       // YUV (HD) horizontal stretch      0xC0F11B8C
static unsigned YUV_HD_S_V = 0;       // YUV (HD) vertical stretch        0xC0F11BCC
static unsigned YUV_HD_S_V_E = 0;     // YUV (HD) enable vertical stretch 0xC0F11BC8

/* used to correct aspect ratio on screen */
static unsigned YUV_LV_S_V = 0;       // YUV (LV) vertical stretch        0xC0F11ACC
static unsigned YUV_LV_Buf = 0;       // YUV (LV) buffer size             0xC0F04210

/* used to exceed preview limits */
//static unsigned EDMAC_24_s = 0;       // EDMAC#24 size                    0xC0F26810
//static unsigned EDMAC_24_address = 0; // EDMAC#24 buffer address          0xC0F26808
static unsigned EDMAC_24_Redirect = 0;  // EDMAC#24 re-driect buffer flag
static unsigned Black_Bar = 0;          // Exceed black bar width limit     0xC0F3B038, 0xC0F3B088

// addresses (part of EDMAC#9 configuration structure) which holds vertical HIV size in x5 mode
// on 700D the structure starts in 0x3e1b4 and ends in 0x3e234, it's being loaded in LVx5_StartPreproPath from ff4f2860
// overriding them are needed to exceed vertical preview limit, on 700D it's RAW V - 1 = 0x453 (in x5 mode)

// 0xC0F08184 = RAW V - 1 = 0x453 (in x5), which is also related somehow to EDMAC#9 vertical size and needs to be tweaked
// also EDMAC_24_Redirect is required before increasing 0xC0F08184 value, otherwise RAW data would be corrupted

// AFAIK EDMAC#9 is used for darkframe subtraction for LiveView, it also sets black and white level values (for LiveView)
static uint32_t EDMAC_9_Vertical_1 = 0;         // 0x453 , it's being set in 0xC0F04910 register which control EDMAC#9 Size B
static uint32_t EDMAC_9_Vertical_2 = 0;         // 0x453 , tweaking it has no effect? let's tweak just in case

static unsigned EDMAC_9_Vertical_Change = 0;    // flag to enable/disable EDMAC#9 tweaks

/* used to recover preview height eaten by C0F38024 after increasing C0F38024 horizontal value */
static int Preview_V_Recover = 0;

/* flags to center preview and clear VRAM artifacts (NewShiftVal, NewClearVal are related too) */
static unsigned Shift_Preview = 0;
static unsigned Center_Preview_ON = 0;
static unsigned Clear_Artifacts = 0;
static unsigned Clear_Artifacts_ON = 0;

/* camera-specific ROM addresses */
/* Shift_x5 holds preview shifting value for an output for x5 mode */
static uint32_t Shift_x5_LCD = 0;
static uint32_t Shift_x5_HDMI_480p = 0; // Called VIDEO NTSC 
static uint32_t Shift_x5_HDMI_1080i_Full = 0;
static uint32_t Shift_x5_HDMI_1080i_Info = 0;

/* Clear_Vram_x5 holds preview clear Vram value for an output for x5 mode */
static uint32_t Clear_Vram_x5_LCD = 0;
static uint32_t Clear_Vram_x5_HDMI_480p = 0;
static uint32_t Clear_Vram_x5_HDMI_1080i_Full = 0;
static uint32_t Clear_Vram_x5_HDMI_1080i_Info = 0;

/* address holds darkframe subtraction data for photo mode for (RAW_V) */
static uint32_t HIV_Vertical_Photo_Address = 0;
static uint32_t HIV_Vertical_Address_hook = 0; // LVx5_StartPreproPath, sets HIV address for RAW_V

static inline uint32_t reg_override_1X1(uint32_t reg, uint32_t old_val)
{
    if (CROP_2_5K)
    {
        if (is_650D || is_700D || is_EOSM)
        {
            RAW_H         = 0x298 + reg_width;
            RAW_V         = 0x455 + reg_height;
            TimerA        = 0x2CB;
            if (Framerate_24) TimerB = 0x747;
            if (Framerate_25) TimerB = 0x6FA;
            if (Framerate_30) TimerB = 0x5D3;
        }

        if (is_100D)
        {
            RAW_H         = 0x2a1;
            RAW_V         = 0x459;
            TimerA        = 0x2DB;
            if (Framerate_24) TimerB = 0x71E;
            if (Framerate_25) TimerB = 0x6D3;
            if (Framerate_30) TimerB = 0x5B3;
        }

        // Preview_H should be = active RAW width - 4? , 2520 - 4 = 2516 (active RAW width is 2520)
        // otherwise a black bar will appear in the left part of both YUV (HD) and (LV) dumps 
        // e.g. Preview_H = 2520 --> black bar on the left, also will loss some pixel on the right
        Preview_H     = 2516; 
        Preview_V     = 1080;
        Preview_R     = 0x19000D;
        YUV_HD_S_H    = 0x105027D;
        YUV_HD_S_V    = 0x1050195;
        YUV_HD_S_V_E  = 0;
        Black_Bar     = 2;

        Preview_Control = 1;
        EDMAC_24_Redirect = 0;
        Preview_V_Recover = 0;
        Preview_Control_Basic = 0;
    }
    
/*  initially I wanted to make 2880x1226 preset, RAW data works there, the issue is I could only get 1073 height (from 1226) in preview (width was 2868)
    more likely somehow when increasing width preview in C0F38024, it casues a limit in height, it'a act like give me width pixels in cost 
    of vertical pixels, EDMAC_24_Redirect and EDMAC_9_Vertical_Change doesn't help to get more height in this case, something else is causing a limit?
    BTW: C0F38024 can fix broken preview which casued of increasing RAW horizontal resolution in C0F06804, e.g. on 700D it's:
    
    in x5 mode C0F06804 = 0x4540298 while C0F38024 = 0x453x287. in C0F06804 let's say RAW_V = 454 and RAW_H = 298:
    C0F38024 = ((RAW_V - 1) << 16)  + RAW_H - 0x11*/
    
    /* at this moment I was making 2800x1192 preset instead because it would be more manageable in terms of preview, but . . */
    /* huh, never mind! figuerd it out, we can just increase EDMAC_9_Vertical and C0F08184 *more than actual RAW_V* to recover preview height  */
    
    if (CROP_2_8K)
    {
        if (is_650D || is_700D || is_EOSM)
        {
            RAW_H         = 0x2F2 + reg_width;
            RAW_V         = 0x4d3 + reg_height;
            TimerA        = 0x325;
            if (Framerate_24) TimerB = 0x676;
            if (Framerate_25) TimerB = 0x633;
            if (Framerate_30) TimerB = 0x633;  // 30 Doesn't work, make it 25
        }

        if (is_100D)
        {
            RAW_H         = 0x2FB;
            RAW_V         = 0x4EB;
            TimerB        = 0x666;
            TimerA        = 0x32D;
            if (Framerate_24) TimerB = 0x666;
            if (Framerate_25) TimerB = 0x623;
            if (Framerate_30) TimerB = 0x623;  // 30 Doesn't work, make it 25
        }

        Preview_H         = 2868;  // black bar above 2868
        Preview_V         = 1226 + YUV_HD_S_H_height;
        Preview_V_Recover = 171 + YUV_HD_S_H_width;   // trial and error
        
        Preview_R     = 0x19000F;
        
        YUV_HD_S_H    = 0x10502D6;
        YUV_HD_S_V    = 0x10501b8 + YUV_HD_S_V_width;
        YUV_HD_S_V_E  = 0;
        Black_Bar     = 2;

        Preview_Control = 1;
        EDMAC_24_Redirect = 1;
        EDMAC_9_Vertical_Change = 1;
        Preview_Control_Basic = 0;
    } 

    if (CROP_3K)
    {
        /* Active RAW 3072x1308 (2.35:1). Old RAW_V 0x521 gave ~1284 lines (2.39:1). */
        enum { CROP_3K_RAW_V_EXTRA = 0x18 }; /* +24 lines → 1308 active height */

        if (is_650D || is_700D || is_EOSM)
        {
            RAW_H    = 0x322 + reg_width;
            RAW_V    = 0x521 + reg_height + CROP_3K_RAW_V_EXTRA;
            TimerB   = 0x60F;
            TimerA   = 0x35B;
        }

        if (is_100D)
        {
            RAW_H    = 0x32B;
            RAW_V    = 0x53D + CROP_3K_RAW_V_EXTRA;
            TimerB   = 0x60B;
            TimerA   = 0x35D;
        }

        Preview_H         = 2868;  // black bar above 2868
        Preview_V         = 1308;
        Preview_V_Recover = 284 + CROP_3K_RAW_V_EXTRA;

        Preview_R     = 0x190028;
        REG_C0F383DC_Tuning = -26; 

        YUV_HD_S_H    = 0x1050308;
        YUV_HD_S_V    = 0x10501D4;
        YUV_HD_S_V_E  = 0;
        Black_Bar     = 2;

        Preview_Control = 1;
        EDMAC_24_Redirect = 1;
        Preview_Control_Basic = 0;
        EDMAC_9_Vertical_Change = 1;
    }
        
    if (CROP_1440p)
    {
        if (is_650D || is_700D || is_EOSM)
        {
            RAW_H    = 0x2A2 + reg_width;
            RAW_V    = 0x5BD + reg_height;
            TimerA   = 0x2DB;
            if (Framerate_24) TimerB = 0x71E;
            if (Framerate_25) TimerB = 0x6D3;
            if (Framerate_30) TimerB = 0x6D3;  // 30 Doesn't work, make it 25
        }

        if (is_100D)
        {
            RAW_H    = 0x2AB;
            RAW_V    = 0x5C1;
            TimerA   = 0x2DD;
            if (Framerate_24) TimerB = 0x719;
            if (Framerate_25) TimerB = 0x6CE;
            if (Framerate_30) TimerB = 0x6CE;  // 30 Doesn't work, make it 25
        }

        Preview_H     = 2552;  // 2556 causes preview artifacts
        Preview_V     = 1440;
        Preview_R     = 0x19000E;
        Preview_V_Recover = 22;
        
        YUV_HD_S_H    = 0x1050286;
        YUV_HD_S_V    = 0x105021E;

        Black_Bar     = 2;
        Preview_Control = 1;
        EDMAC_24_Redirect = 1;
        EDMAC_9_Vertical_Change = 1;
        Preview_Control_Basic = 0;
    }
    
    if (CROP_1620p)
    {
        if (is_650D || is_700D || is_EOSM)
        {
            RAW_H    = 0x23E + reg_width;
            RAW_V    = 0x671 + reg_height;
            TimerA   = 0x279;
            /* Single supported rate (same TimerB as dannephoto for all menu FPS indices). */
            TimerB   = 0x838;
        }

        Preview_H     = 2156 + reg_Preview_H;  // 2556 causes preview artifacts
        Preview_V     = 1620 + reg_Preview_V;
        Preview_R     = 0x19000D;
        Preview_V_Recover = 22;
        
        YUV_HD_S_H    = 0x1050220 + reg_YUV_HD_S_H; //+ 50
        YUV_HD_S_V    = 0x1050240 + reg_YUV_HD_S_V;
        
        //doktorkrek suggestion (same as dannephoto — leave 0xC0F11A8C alone)
        YUV_LV_Buf = 0x1B505A0;
        YUV_LV_S_V = 0x10501B2;
        //EngDrvOutLV(0xC0F11A8C, 0x1E0038);
                        
        Black_Bar     = 2;
        Preview_Control = 1;
        EDMAC_24_Redirect = 1;
        EDMAC_9_Vertical_Change = 1;
        Preview_Control_Basic = 0;
    }

    if (CROP_1280p)
    {
        if (is_650D || is_700D || is_EOSM)
        {
            RAW_H    = 0x202 + reg_width;
            RAW_V    = 0x51D + reg_height;
            TimerA   = 0x235;
            if (Framerate_24) TimerB = 0x935;
            if (Framerate_25) TimerB = 0x8D4;
            if (Framerate_30) TimerB = 0x75D;
        }

        if (is_100D)
        {
            RAW_H    = 0x20B;
            RAW_V    = 0x521;
            TimerA   = 0x23D;
            if (Framerate_24) TimerB = 0x914;
            if (Framerate_25) TimerB = 0x8B5;
            if (Framerate_30) TimerB = 0x743;
        }

        Preview_H     = 1916;
        Preview_V     = 1280;
        Preview_R     = 0x19000D;

        YUV_HD_S_H    = 0x450080;
        YUV_HD_S_V    = 0x250044;

        Black_Bar     = 0;
        Preview_Control = 1;
        EDMAC_24_Redirect = 0;
        EDMAC_9_Vertical_Change = 0;
        Preview_Control_Basic = 0;
    }
    
    if (CROP_1080p)
    {
        if (is_650D || is_700D || is_EOSM)
        {
            RAW_H    = 0x202 + reg_width;
            RAW_V    = 0x455 + reg_height;
            TimerA   = 0x235;
            if (Framerate_24) TimerB = 0x935;
            if (Framerate_25) TimerB = 0x8D4;
            if (Framerate_30) TimerB = 0x75D;
        }

        if (is_100D)
        {
            RAW_H    = 0x20B;
            RAW_V    = 0x521;
            TimerA   = 0x23D;
            if (Framerate_24) TimerB = 0x914;
            if (Framerate_25) TimerB = 0x8B5;
            if (Framerate_30) TimerB = 0x743;
        }

        Preview_H     = 1916 + reg_Preview_H;
        Preview_V     = 1080 + reg_Preview_V;
        Preview_R     = 0x19000D;

        YUV_HD_S_H    = 0x450080 + YUV_HD_S_H_width;
        YUV_HD_S_V    = 0x250039 + YUV_HD_S_H_height;

        Black_Bar     = 0;
        Preview_Control = 1;
        EDMAC_24_Redirect = 0;
        EDMAC_9_Vertical_Change = 0;
        Preview_Control_Basic = 0;
    }

    if (CROP_Full_Res) /* 5208x3478 — EOS M slim LV @ 3 FPS; other Digic5 @ 2 FPS */
    {
        if (is_650D || is_700D || is_EOSM)
        {
            RAW_H    = 0x538 + reg_width;
            RAW_V    = 0xDB3 + reg_height;
            /* 32000000 / (0x56B * TimerB) ≈ fps */
            TimerB   = is_EOSM ? 0x1E0A : (0x1E03 + 3840); /* EOSM ~3fps; others ~2fps */
            TimerA   = 0x56B;
        }

        if (is_100D)
        {
            RAW_H    = 0x541;
            RAW_V    = 0xDB7;
            TimerB   = 0x1DD6;
            TimerA   = 0x573;
        }

        Preview_x1 = 0x217;
        Preview_x2 = 0x31F;
        Preview_y1 = 0x593;
        Preview_y2 = 0x84C;

        Preview_Control = 0;
        EDMAC_24_Redirect = 1;
        Preview_V_Recover = 820;  // is this dangrous? this exceeds vertical EDMAC#9 size in photo mode which is 3529 (to 4326)  
        Preview_Control_Basic = 1;
        EDMAC_9_Vertical_Change = 1;
    }

    if (!CROP_3K)
    {
        REG_C0F383DC_Tuning = 0;
    }

    if (Preview_Control)
    {
        if (EDMAC_9_Vertical_Change)
        {
            if (MEM(EDMAC_9_Vertical_1) != (RAW_V - 1) + Preview_V_Recover ||
                MEM(EDMAC_9_Vertical_2) != (RAW_V - 1) + Preview_V_Recover) // set our new value if not set yet
            {
                MEM(EDMAC_9_Vertical_1)  = (RAW_V - 1) + Preview_V_Recover ;
                MEM(EDMAC_9_Vertical_2)  = (RAW_V - 1) + Preview_V_Recover;
            }

            switch (reg)
            {
                case 0xC0F08184: return (RAW_V - 1) + Preview_V_Recover; // used to exceed vertical preview limit
            }
        }
    }

    if (Preview_Control_Basic)
    {
        if (EDMAC_9_Vertical_Change)
        {
            if (MEM(EDMAC_9_Vertical_1) != (RAW_V - 1) + Preview_V_Recover ||
                MEM(EDMAC_9_Vertical_2) != (RAW_V - 1) + Preview_V_Recover) // set our new value if not set yet
            {
                MEM(EDMAC_9_Vertical_1)  = (RAW_V - 1) + Preview_V_Recover ;
                MEM(EDMAC_9_Vertical_2)  = (RAW_V - 1) + Preview_V_Recover;
            }

            switch (reg)
            {
               case 0xC0F08184: return (RAW_V - 1) + Preview_V_Recover; // used to exceed vertical preview limit
            }
        }
    }

    /* get rid of moving Dual ISO lines by tweaking Timer B a tiny bit */
    if (fix_dual_iso_flicker && dual_iso_is_enabled())
    {
        TimerB = Adjust_TimerB_For_Dual_ISO(TimerB);
    }

    switch (reg)
    {
        case 0xC0F06804: return (RAW_V << 16) + RAW_H;

        case 0xC0F06824:
        case 0xC0F06828:
        case 0xC0F0682C:
        case 0xC0F06830:
        {
            return RAW_H + 0x32;
        }

        case 0xC0F0713c: return RAW_V + 0x1;
        case 0xC0F07150: return RAW_V - 0x3A;

        case 0xC0F06014: return TimerB;
        case 0xC0F06010: return TimerA;
        case 0xC0F06008: return TimerA + (TimerA << 16);
        case 0xC0F0600C: return TimerA + (TimerA << 16);
    }

    return 0;
}



static inline uint32_t reg_override_1X3(uint32_t reg, uint32_t old_val)
{
    if (Anam_FLV)
    {
        RAW_H         = 0x1D4 + reg_width;  // from mv1080 mode
        RAW_V         = 0xDB3 + reg_height;
        TimerB        = OUTPUT_10BIT ? 0xf05 - fps_over: (OUTPUT_12BIT || OUTPUT_11BIT) ? 0x112b - fps_over: OUTPUT_14BIT ? 0x1407 - fps_over: 0;
        TimerA        = 0x207 + TimerA_Debug;
        
        //From AR_2_35_1
        Preview_H     = 1728;      // from mv1080 mode
        Preview_V     = 3478;
        Preview_R     = 0x1D000E;  // from mv1080 mode
        YUV_HD_S_H    = 0x10501B5 + YUV_HD_S_H_width + (YUV_HD_S_H_height << 16);
        YUV_HD_S_V    = 0x45015C + YUV_HD_S_V_width + (YUV_HD_S_V_height << 16);
        
        //Works well with focus aid for 1x3 presets
        //EngDrvOutLV(0xc0f11A88, 0x1);
        //YUV_HD_S_H    = 0x10501B5 + YUV_HD_S_H_width - (90 << 16);
        //YUV_HD_S_V    = 0x45015C + 800 + (1000 << 16);
    }
    else
    {
        if (AR_16_9)
        {
            if (Anam_Highest) /* 1504x2538 */
            {
                if (is_650D || is_700D)
                {
                    RAW_H         = 0x19A;  /*  @ 23.976 FPS */
                    RAW_V         = 0xA06;
                    TimerB        = 0xB47;
                    TimerA        = 0x1CD;
                }
                
                if (is_EOSM)
                {
                    RAW_H         = 0x19A + reg_width; /*  @ 22.250 FPS */
                    RAW_V         = 0xA07 + reg_height;
                    TimerB        = 0xAF7;
                    TimerA        = 0x1FF;  // Danne confirmed that EOS M has 0x1FF limit. it seems same as 100D
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x1A3; /*  @ 22.250 FPS */
                    RAW_V         = 0xA0B;
                    TimerB        = 0xB07;  // we might be able to lower TimerB a little more
                    TimerA        = 0x1FF;  // lowering TimerA under 0x1FF --> black image (RAW data), anyway to exceed minimal Timer A limit?
                }
                
                Preview_H     = 1500;
                Preview_V     = 2538;
                Preview_R     = 0x1D000D;
                YUV_HD_S_H    = 0x105017B;
                YUV_HD_S_V    = 0x10503BE;
            }
            
            if (Anam_Higher)
            {
                if (is_650D || is_700D)
                {
                    RAW_H         = 0x17E;  /* 1392x2350 @ 23.976 and 25 FPS */
                    RAW_V         = 0x94A;
                    TimerA        = 0x1B9;
                    if (Framerate_24) TimerB = 0xBCA;
                    if (Framerate_25) TimerB = 0xB4E;
                    if (Framerate_30) TimerB = 0xB4E; // 30 Doesn't work, make it 25
                    
                    Preview_H     = 1388;
                    Preview_V     = 2350;
                    Preview_R     = 0x1D000D;
                    YUV_HD_S_H    = 0x105015F;
                    YUV_HD_S_V    = 0x1050377;
                }
                
                if (is_EOSM)
                {
                    RAW_H         = 0x17A + reg_width;  /* 1376x2322 to achieve 23.976 FPS */
                    RAW_V         = 0x92f + reg_height;
                    TimerB        = 0xA2E;
                    TimerA        = 0x1FF;
                    
                    Preview_H     = 1372;
                    Preview_V     = 2322;
                    Preview_R     = 0x1D000D;
                    YUV_HD_S_H    = 0x105015B + YUV_HD_S_H_width + (YUV_HD_S_H_height << 16);
                    YUV_HD_S_V    = 0x105036D + YUV_HD_S_V_width + (YUV_HD_S_V_height << 16);
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x183;  /* 1376x2322 to achieve 23.976 FPS */
                    RAW_V         = 0x933;
                    TimerB        = 0xA2E;
                    TimerA        = 0x1FF;
                    
                    Preview_H     = 1372;
                    Preview_V     = 2322;
                    Preview_R     = 0x1D000D;
                    YUV_HD_S_H    = 0x105015B;
                    YUV_HD_S_V    = 0x105036E;
                }
            }
            
            if (Anam_Medium) /* 1280x2160 @ 23.976 and 25 FPS */
            {
                if (is_650D || is_700D)
                {
                    RAW_H         = 0x162;
                    RAW_V         = 0x88C;
                    TimerA        = 0x1B9;
                    if (Framerate_24) TimerB = 0xBCA;
                    if (Framerate_25) TimerB = 0xB4E;
                    if (Framerate_30) TimerB = 0xB4E; // 30 Doesn't work, make it 25
                }
                
                if (is_EOSM)
                {
                    RAW_H         = 0x162 + reg_width;
                    RAW_V         = 0x88D + reg_height;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2E;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x16B;
                    RAW_V         = 0x891;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2E;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                Preview_H     = 1276;
                Preview_V     = 2160;
                Preview_R     = 0x1D000D;
                YUV_HD_S_H    = 0x1050142;
                YUV_HD_S_V    = 0x105032F;
            }
        }
        
        if (AR_2_1)
        {
            if (Anam_Highest) /* 1600x2400 */
            {
                if (is_650D || is_700D)
                {
                    RAW_H         = 0x1B2;  /* @ 23.976 FPS */
                    RAW_V         = 0x97D;
                    TimerB        = 0xAB9;
                    TimerA        = 0x1E5;
                }
                
                if (is_EOSM)
                {
                    RAW_H         = 0x1B2 + reg_width; /* @ 23.300 FPS */
                    RAW_V         = 0x97D + reg_height;
                    TimerB        = 0xA79;
                    TimerA        = 0x1FF;
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x1BB; /* @ 23.300 FPS */
                    RAW_V         = 0x981;
                    TimerB        = 0xA79;
                    TimerA        = 0x1FF;
                }
                
                Preview_H     = 1596;
                Preview_V     = 2400;
                Preview_R     = 0x1D000D;
                YUV_HD_S_H    = 0x1050193;
                YUV_HD_S_V    = 0x1050389;
            }
            
            if (Anam_Higher) /* 1472x2208 @ 23.976 and 25 FPS */
            {
                if (is_650D || is_700D)
                {
                    RAW_H         = 0x192;
                    RAW_V         = 0x8BD;
                    TimerA        = 0x1C5;
                    if (Framerate_24) TimerB = 0xB7A;
                    if (Framerate_25) TimerB = 0xB02;
                    if (Framerate_30) TimerB = 0xB02; // 30 Doesn't work, make it 25
                }
                
                if (is_EOSM)
                {
                    RAW_H         = 0x192 + reg_width;
                    RAW_V         = 0x8BD + reg_height;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2D;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x19B;
                    RAW_V         = 0x8C1;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2D;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                Preview_H     = 1468;
                Preview_V     = 2208;
                Preview_R     = 0x1D000D;
                YUV_HD_S_H    = 0x1050173;
                YUV_HD_S_V    = 0x1050341;
            }
            
            if (Anam_Medium) /* 1360x2040 @ 23.976 and 25 FPS */
            {
                if (is_650D || is_700D)
                {
                    RAW_H         = 0x176;
                    RAW_V         = 0x813;
                    TimerA        = 0x1B9;
                    if (Framerate_24) TimerB = 0xBCA;
                    if (Framerate_25) TimerB = 0xB4E;
                    if (Framerate_30) TimerB = 0xB4E; // 30 Doesn't work, make it 25
                }
                
                if (is_EOSM)
                {
                    RAW_H         = 0x176 + reg_width;
                    RAW_V         = 0x815 + reg_height;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2D;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x17F;
                    RAW_V         = 0x819;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2D;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                Preview_H     = 1356;
                Preview_V     = 2040;
                Preview_R     = 0x1D000D;
                YUV_HD_S_H    = 0x1050157;
                YUV_HD_S_V    = 0x1050301;
            }
        }
        
        if (AR_2_20_1)
        {
            if (Anam_Highest)
            {
                if (is_650D || is_700D) /* 1680x2290 @ 23.976 FPS */
                {
                    RAW_H         = 0x1C6;
                    RAW_V         = 0x90E;
                    TimerB        = 0xA4C;
                    TimerA        = 0x1F9;
                    
                    Preview_H     = 1676;
                    Preview_V     = 2290;
                    Preview_R     = 0x1D000D;
                    YUV_HD_S_H    = 0x10501A8;
                    YUV_HD_S_V    = 0x1050362;
                }
                
                if (is_EOSM) /* 1664x2268 @ 23.976 FPS */
                {
                    RAW_H         = 0x1C2 + reg_width;
                    RAW_V         = 0x8F9 + reg_height;
                    TimerB        = 0xA2D;
                    TimerA        = 0x1FF;
                    
                    Preview_H     = 1660;
                    Preview_V     = 2268;
                    Preview_R     = 0x1D000D;
                    YUV_HD_S_H    = 0x10501A3;
                    YUV_HD_S_V    = 0x1050359;
                }
                
                if (is_100D) /* 1664x2268 @ 23.976 FPS */
                {
                    RAW_H         = 0x1CB;
                    RAW_V         = 0x8FD;
                    TimerB        = 0xA2D;
                    TimerA        = 0x1FF;
                    
                    Preview_H     = 1660;
                    Preview_V     = 2268;
                    Preview_R     = 0x1D000D;
                    YUV_HD_S_H    = 0x10501A3;
                    YUV_HD_S_V    = 0x1050359;
                }
            }
            
            if (Anam_Higher) /* 1552x2218 @ 23.976 FPS */
            {
                if (is_650D || is_700D)
                {
                    RAW_H         = 0x1A6;
                    RAW_V         = 0x860;
                    TimerA        = 0x1D9;
                    if (Framerate_24) TimerB = 0xAFE;
                    if (Framerate_25) TimerB = 0xA8B;
                    if (Framerate_30) TimerB = 0xA8B; // 30 Doesn't work, make it 25
                }
                
                if (is_EOSM)
                {
                    RAW_H         = 0x1A6 + reg_width;
                    RAW_V         = 0x863 + reg_height;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2E;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x1AF;
                    RAW_V         = 0x865;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2E;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                Preview_H     = 1548;
                Preview_V     = 2216;
                Preview_R     = 0x1D000D;
                YUV_HD_S_H    = 0x1050187;
                YUV_HD_S_V    = 0x105031C;
            }
            
            if (Anam_Medium) /* 1424x1942 @ 23.976 FPS */
            {
                if (is_650D || is_700D)
                {
                    RAW_H         = 0x186;
                    RAW_V         = 0x7B2;
                    TimerA        = 0x1B9;
                    if (Framerate_24) TimerB = 0xBCA;
                    if (Framerate_25) TimerB = 0xB4E;
                    if (Framerate_30) TimerB = 0xB4E; // 30 Doesn't work, make it 25
                }
                
                if (is_EOSM)
                {
                    RAW_H         = 0x186 + reg_width;
                    RAW_V         = 0x7B3 + reg_height;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2E;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x18F;
                    RAW_V         = 0x7B7;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2E;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                Preview_H     = 1420;
                Preview_V     = 1942;
                Preview_R     = 0x1D000D;
                YUV_HD_S_H    = 0x1050167;
                YUV_HD_S_V    = 0x10502DB;
            }
        }
        
        if (AR_2_35_1)
        {
            if (Anam_Highest) /* 1736x2216 @ 23.976 FPS */
            {
                if (is_650D || is_700D || is_EOSM)
                {
                    RAW_H         = 0x1D4 + reg_width;  // from mv1080 mode
                    RAW_V         = 0x8C3 + reg_height;
                    TimerB        = 0xA07;
                    TimerA        = 0x207;
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x1DD;
                    RAW_V         = 0x8C7;
                    TimerB        = 0x9DF;
                    TimerA        = 0x20F;
                }
                
                Preview_H     = 1728;      // from mv1080 mode
                Preview_V     = 2214;
                Preview_R     = 0x1D000E;  // from mv1080 mode
                YUV_HD_S_H    = 0x10501B5;
                YUV_HD_S_V    = 0x1050341;
            }
            
            if (Anam_Higher) /* 1600x2040 @ 23.976 FPS */
            {
                if (is_650D || is_700D)
                {
                    RAW_H         = 0x1B2;
                    RAW_V         = 0x814;
                    TimerA        = 0x1E5;
                    if (Framerate_24) TimerB = 0xAB9;
                    if (Framerate_25) TimerB = 0xA48;
                    if (Framerate_30) TimerB = 0xA48; // 30 Doesn't work, make it 25
                }
                
                if (is_EOSM)
                {
                    RAW_H         = 0x1B2 + reg_width;
                    RAW_V         = 0x815 + reg_height;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2D;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x1BB;
                    RAW_V         = 0x819;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2D;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                Preview_H     = 1596;
                Preview_V     = 2040;
                Preview_R     = 0x1D000D;
                YUV_HD_S_H    = 0x1050193;
                YUV_HD_S_V    = 0x1050301;
            }
            
            if (Anam_Medium) /* 1472x1878 @ 23.976 FPS */
            {
                if (is_650D || is_700D)
                {
                    RAW_H         = 0x192;
                    RAW_V         = 0x772;
                    TimerA        = 0x1C5;
                    if (Framerate_24) TimerB = 0xB7A;
                    if (Framerate_25) TimerB = 0xB02;
                    if (Framerate_30) TimerB = 0xB02; // 30 Doesn't work, make it 25
                }
                
                if (is_EOSM)
                {
                    RAW_H         = 0x192 + reg_width;
                    RAW_V         = 0x773 + reg_height;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2D;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x19B;
                    RAW_V         = 0x777;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2D;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                Preview_H     = 1468;
                Preview_V     = 1878;
                Preview_R     = 0x1D000D;
                YUV_HD_S_H    = 0x1050173;
                YUV_HD_S_V    = 0x10502C4;
            }
        }
        
        if (AR_2_39_1)
        {
            if (Anam_Highest) /* 1736x2178 @ 23.976 FPS */
            {
                if (is_650D || is_700D || is_EOSM)
                {
                    RAW_H         = 0x1D4 + reg_width;  // from mv1080 mode
                    RAW_V         = 0x89f + reg_height;
                    TimerB        = 0xA05;
                    TimerA        = 0x207;
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x1DD;
                    RAW_V         = 0x8A3;
                    TimerB        = 0x9CB;
                    TimerA        = 0x213;  // can be lowered even more? need to be fine tuned
                }
                
                Preview_H     = 1728;      // from mv1080 mode
                Preview_V     = 2178;
                Preview_R     = 0x1D000E;  // from mv1080 mode
                YUV_HD_S_H    = 0x10501B5;
                YUV_HD_S_V    = 0x1050336;
            }
            
            if (Anam_Higher) /* 1600x2008 @ 23.976 FPS */
            {
                if (is_650D || is_700D)
                {
                    RAW_H         = 0x1B2;
                    RAW_V         = 0x7F4;
                    TimerA        = 0x1E5;
                    if (Framerate_24) TimerB = 0xAB9;
                    if (Framerate_25) TimerB = 0xA48;
                    if (Framerate_30) TimerB = 0xA48; // 30 Doesn't work, make it 25
                }
                
                if (is_EOSM)
                {
                    RAW_H         = 0x1B2 + reg_width;
                    RAW_V         = 0x7F5 + reg_height;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2D;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x1BB;
                    RAW_V         = 0x7F9;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2D;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                Preview_H     = 1596;
                Preview_V     = 2008;
                Preview_R     = 0x1D000D;
                YUV_HD_S_H    = 0x1050193;
                YUV_HD_S_V    = 0x10502F4;
            }
            
            if (Anam_Medium) /* 1472x1846 @ 23.976 FPS */
            {
                if (is_650D || is_700D)
                {
                    RAW_H         = 0x192;
                    RAW_V         = 0x752;
                    TimerA        = 0x1C5;
                    if (Framerate_24) TimerB = 0xB7A;
                    if (Framerate_25) TimerB = 0xB02;
                    if (Framerate_30) TimerB = 0xB02; // 30 Doesn't work, make it 25
                }
                
                if (is_EOSM)
                {
                    RAW_H         = 0x192 + reg_width;
                    RAW_V         = 0x753 + reg_height;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2D;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                if (is_100D)
                {
                    RAW_H         = 0x19B;
                    RAW_V         = 0x757;
                    TimerA        = 0x1FF;
                    if (Framerate_24) TimerB = 0xA2D;
                    if (Framerate_25) TimerB = 0x9C3;
                    if (Framerate_30) TimerB = 0x9C3; // 30 Doesn't work, make it 25
                }
                
                Preview_H     = 1468;
                Preview_V     = 1846;
                Preview_R     = 0x1D000D;
                YUV_HD_S_H    = 0x1050173;
                YUV_HD_S_V    = 0x10502B6;
            }
        }
    }
    Black_Bar = 0;
    YUV_HD_S_V_E  = 0;
    Preview_Control = 1;
    EDMAC_24_Redirect = 1;
    EDMAC_9_Vertical_Change = 1;
    Preview_Control_Basic = 0;
    REG_C0F383DC_Tuning = 0;

    if (Preview_Control)
    {
        if (EDMAC_9_Vertical_Change)
        {
            if (MEM(EDMAC_9_Vertical_1) != RAW_V - 1 || MEM(EDMAC_9_Vertical_2) != RAW_V - 1) // set our new value if not set yet
            {
                MEM(EDMAC_9_Vertical_1)  = RAW_V - 1;
                MEM(EDMAC_9_Vertical_2)  = RAW_V - 1;
            }

            switch (reg)
            {
                case 0xC0F08184: return RAW_V - 1; // used to exceed vertical preview limit
            }
        }
    }

    /* get rid of moving Dual ISO lines by tweaking Timer B a tiny bit */
    if (fix_dual_iso_flicker && dual_iso_is_enabled())
    {
        TimerB = Adjust_TimerB_For_Dual_ISO(TimerB);
    }

    switch (reg)
    {
        case 0xC0F06804: return (RAW_V << 16) + RAW_H;

        case 0xC0F06824:
        case 0xC0F06828:
        case 0xC0F0682C:
        case 0xC0F06830:
        {
            return RAW_H + 0x32;
        }

        case 0xC0F0713c: return RAW_V + 0x1;
        case 0xC0F07150: return RAW_V - 0x3A;

        case 0xC0F06014: return TimerB;
        case 0xC0F06010: return TimerA;
        case 0xC0F06008: return TimerA + (TimerA << 16);
        case 0xC0F0600C: return TimerA + (TimerA << 16);
    }

    return 0;
}

/* these presets have some preview quirks mainly because we lowered RAW resolution lower than default
   RAW resolution in x5 mode, some tasks expect a minimal amount of RAW resolution, otherwise some issues
   start to appear. in our case it's a frozen preview on YUV (LV) path.
   on 700D, for 1736x976, 1736x868 and 1736x790 presets and in order to make preview work, the focus
   box should be centered on screen + lower it one step by using arrow down button. when lowering focus box,
   some functions are being called and set some new values, I don't know which exact function we are looking for.
   in 1736x738 preset preview will be always frozen, changing focus box position doesn't help, but a workaround
   which make preview work with mentioned presets is suspending aewb task, the issue seems related to aewb task 
   in 1736x694 suspending aewb task doesn't help, preview is still frozen, pretty sure it can be solved by lowering
   some preview registers related to RAW resolution (not the ones in crop_rec.c), currently I don't know how to do it,
   or where to look exactly, for 1736x694 it could from YUV (HD) path resolution too */
static inline uint32_t reg_override_3X3(uint32_t reg, uint32_t old_val)
{
    if (High_FPS)
    {
        if (AR_16_9)
        {
            if (is_650D || is_700D || is_EOSM) // 1736x976 @ 46.800 FPS
            {
                RAW_H         = 0x1D4 + reg_width;
                RAW_V         = 0x3ED + reg_height;
                TimerB        = crop_preset_fps_reduce == 0x1 ? 0x5a2: 0x50E;
                TimerA        = 0x20F;  // can go lower down to 0x207
            }
            
            if (is_100D) // 1736x976 @ 46.300 FPS
            {
                RAW_H         = 0x1DD;
                RAW_V         = 0x3F1;
                TimerB        = 0x51C;
                TimerA        = 0x20F;
            }
            
            Preview_H     = 1728;      // from mv1080 mode
            Preview_V     = 976;
            Preview_R     = 0x1D000E;  // from mv1080 mode
            YUV_HD_S_V    = 0x105016C;
        }
        
        if (AR_2_1)
        {
            if (is_650D || is_700D || is_EOSM) // 1736x868 @ 50 FPS
            {
                RAW_H         = 0x1D4 + reg_width;
                RAW_V         = 0x381 + reg_height;
                TimerB        = crop_preset_fps_reduce == 0x1 ? 0x501: 0x4CD;
                TimerA        = 0x207;
            }
            
            if (is_100D)
            {
                RAW_H         = 0x1DD;
                RAW_V         = 0x385;
                TimerB        = 0x4BB;
                TimerA        = 0x20F;
            }
            
            Preview_H     = 1728;
            Preview_V     = 868;
            Preview_R     = 0x1D000E;
            YUV_HD_S_V    = 0x1050143;
        }
        
        if (AR_2_20_1)
        {
            if (is_650D || is_700D || is_EOSM) // 1736x790 @ 54 FPS
            {
                RAW_H         = 0x1D4 + reg_width;
                RAW_V         = 0x333 + reg_height;
                TimerB        = crop_preset_fps_reduce == 0x1 ? 0x4ce: 0x472;
                TimerA        = 0x207;
            }
            
            if (is_100D)
            {
                RAW_H         = 0x1DD;
                RAW_V         = 0x337;
                TimerB        = 0x461;
                TimerA        = 0x20F;
            }
            
            Preview_H     = 1728;
            Preview_V     = 790;
            Preview_R     = 0x1D000E;
            YUV_HD_S_V    = 0x1050125;
        }
        
        if (AR_2_35_1)
        {
            if (is_650D || is_700D || is_EOSM) // 1736x738 @ 57 FPS
            {
                RAW_H         = 0x1D4 + reg_width;
                RAW_V         = 0x2FF + reg_height;
                TimerB        = crop_preset_fps_reduce == 0x1 ? 0x4ce: 0x436;
                TimerA        = 0x207;
            }
            
            if (is_100D) // 1736x738 @ 55.6 FPS
            {
                RAW_H         = 0x1DD;
                RAW_V         = 0x303;
                TimerB        = 0x441;
                TimerA        = 0x20F;
            }
            
            Preview_H     = 1728;
            Preview_V     = 738;
            Preview_R     = 0x1D000E;
            YUV_HD_S_V    = 0x1050112;
        }
        
        if (AR_2_39_1 && crop_preset_fps_reduce == 1 && is_EOSM)
        {
            // 1736x726 @ 57 FPS
            RAW_H         = 0x1D4 + reg_width;
            RAW_V         = 0x2F3 + reg_height;
            TimerB        = crop_preset_fps_reduce == 0x1 ? 0x4ce: 0x436;
            TimerA        = 0x207;
            
            Preview_H     = 1728;
            Preview_V     = 726;
            Preview_R     = 0x1D000E;
            YUV_HD_S_V    = 0x1050106;
        }
        
        if (AR_2_39_1 && crop_preset_fps_reduce == 0)  // 2.39:1 doesn't make sense, very similair to 2.35:1, let's make it 2.50:1
        {
            if (is_650D || is_700D || is_EOSM) // 1736x694 @ 60 FPS
            {
                RAW_H         = 0x1D4 + reg_width;
                RAW_V         = 0x2D2 + reg_height;
                TimerB        = 0x401;
                TimerA        = 0x207;
            }
            
            if (is_100D) // 1736x694 @ 58 FPS
            {
                RAW_H         = 0x1DD;
                RAW_V         = 0x2D7;
                TimerB        = 0x413;
                TimerA        = 0x20F;
            }
            
            Preview_H     = 1728;
            Preview_V     = 694;
            Preview_R     = 0x1D000E;
            YUV_HD_S_V    = 0;          // default x5 mode value
            
            YUV_LV_S_V    = 0x1E002B;   // default x5 mode value
            YUV_LV_Buf    = 0x1DF05A0;  // default x5 mode value
        }
    }
    
    /* mv1080 preset made to enable 1080p mode mainly for EOS M (other models don't really need it) */
    if (mv1080)
    {
        if (is_EOSM)
        {
            RAW_H         = 0x1D4 + reg_width;
            RAW_V         = 0x4A5 + reg_height;
        }
        
        if (Framerate_24) {TimerA = 0x20F; TimerB = 0x9DE;}
        if (Framerate_25) {TimerA = 0x27F; TimerB = 0x7CF;}
        if (Framerate_30) {TimerA = 0x20F; TimerB = 0x7E4;}
        
        Preview_H     = 1728;      // from mv1080 mode
        Preview_V     = 1152;      // from mv1080 mode
        Preview_R     = 0x1D000E;  // from mv1080 mode
        YUV_HD_S_V    = 0x450072;
    }
    
    if (mv1080_3_2)
    {
        if (is_EOSM)
        {
            RAW_H         = 0x1D4 + reg_width;
            RAW_V         = 0x4A5 + reg_height;
        }
        
        if (Framerate_24) {TimerA = 0x20F; TimerB = 0x9DE;}
        if (Framerate_25) {TimerA = 0x27F; TimerB = 0x7CF;}
        if (Framerate_30) {TimerA = 0x20F; TimerB = 0x7E4;}
        
        Preview_H     = 1728;      // from mv1080 mode
        Preview_V     = 1152;      // from mv1080 mode
        Preview_R     = 0x1D000E;  // from mv1080 mode
        YUV_HD_S_V    = 0x450072;
    }
    
    YUV_HD_S_H    = 0x10501B5;

    Black_Bar = 0;
    YUV_HD_S_V_E  = 0;
    Preview_Control = 1;
    Preview_Control_Basic = 0;
    REG_C0F383DC_Tuning = 0;

    /* get rid of moving Dual ISO lines by tweaking Timer B a tiny bit */
    if (fix_dual_iso_flicker && dual_iso_is_enabled())
    {
        TimerB = Adjust_TimerB_For_Dual_ISO(TimerB);
    }

    switch (reg)
    {
        case 0xC0F06804: return (RAW_V << 16) + RAW_H;

        case 0xC0F06824:
        case 0xC0F06828:
        case 0xC0F0682C:
        case 0xC0F06830:
        {
            return RAW_H + 0x32;
        }

        case 0xC0F0713c: return RAW_V + 0x1;
        case 0xC0F07150: return RAW_V - 0x3A;

        case 0xC0F06014: return TimerB;
        case 0xC0F06010: return TimerA;
        case 0xC0F06008: return TimerA + (TimerA << 16);
        case 0xC0F0600C: return TimerA + (TimerA << 16);
    }

    return 0;
}

/* 70D ENGIO overrides */
static inline uint32_t reg_override_1x3(uint32_t reg, uint32_t old_val)
{
    if (!is_70D)
    {
        /* don't patch engio overrides on other models (5D3 in this case) */
        return 0;
    }

    if (is_70D)
    {
        if (!is_1080p())
        {
            /* don't patch other modes */
            return 0;
        }

        RAW_V = 0x736; // The image freeze when going above 0x736, because of HeadTimer 3 0xC0F0713C = 0x738, why?! 
        RAW_H = 0x107;
        TimerA = 0x27E; // This is the lowest value we can use in 1080p mode, going lower than that will break the image
        TimerB = 0x827;

        switch (reg)
        {
            case 0xC0F06804: return (RAW_V << 16) + RAW_H;

            /* I need to double check these, do they always take their value based on TimerA? */
            case 0xC0F06824:
            case 0xC0F06828:
            case 0xC0F0682C:
            case 0xC0F06830:
            {
                return TimerA;
            }

            case 0xC0F0713C: return RAW_V + 0x1;
            case 0xC0F07150: return RAW_V - 0x3A;

            case 0xC0F06014: return TimerB;
            case 0xC0F06024: return TimerB;
            case 0xC0F06010: return TimerA;
            case 0xC0F06008: return TimerA + (TimerA << 16);
            case 0xC0F0600C: return TimerA + (TimerA << 16);
        }
    }

    return 0;
}

static int engio_vidmode_ok = 0;

static void * get_engio_reg_override_func()
{
    uint32_t (*reg_override_func)(uint32_t, uint32_t) = 
      //(crop_preset == CROP_PRESET_3X)         ? reg_override_top_bar     : /* fixme: corrupted image */
        (crop_preset == CROP_PRESET_3X_TALL)    ? reg_override_3X_tall    :
        (crop_preset == CROP_PRESET_3x3_1X)     ? reg_override_3x3_tall   :
        (crop_preset == CROP_PRESET_3x3_1X_48p) ? reg_override_3x3_48p    :
        (crop_preset == CROP_PRESET_1x3)        ? reg_override_1x3        :
        (crop_preset == CROP_PRESET_3K)         ? reg_override_3K         :
        (crop_preset == CROP_PRESET_4K_HFPS)    ? reg_override_4K_hfps    :
        (crop_preset == CROP_PRESET_UHD)        ? reg_override_UHD        :
        (crop_preset == CROP_PRESET_40_FPS)     ? reg_override_40_fps     :
        (crop_preset == CROP_PRESET_FULLRES_LV) ? reg_override_fullres_lv :
        (crop_preset == CROP_PRESET_CENTER_Z)   ? reg_override_zoom_fps   :
        
        /* 650D / 700D / EOSM/M2 / 100D reg_override_func presets */
        (crop_preset == CROP_PRESET_1X1)        ? reg_override_1X1        :
        (crop_preset == CROP_PRESET_1X3)        ? reg_override_1X3        :
        (crop_preset == CROP_PRESET_3X3)        ? reg_override_3X3        :
                                                  0                       ;
    return reg_override_func;
}

static void FAST engio_write_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    uint32_t (*reg_override_func)(uint32_t, uint32_t) = 
        get_engio_reg_override_func();

    if (!reg_override_func)
    {
        return;
    }

    // is engio_vidmode_ok still needed? PathDriveMode might be enough to detect video modes
    
    if (is_basic || is_5D3)
    {
        /* cmos_vidmode_ok doesn't help;
        * we can identify the current video mode from 0xC0F06804 */
        for (uint32_t * buf = (uint32_t *) regs[0]; *buf != 0xFFFFFFFF; buf += 2)
        {
            uint32_t reg = *buf;
            uint32_t old = *(buf+1);
            if (reg == 0xC0F06804)
            {
                if (is_5D3)
                {
                    engio_vidmode_ok = (crop_preset == CROP_PRESET_CENTER_Z)
                    ? (old == 0x56601EB)                        /* x5 zoom */
                    : (old == 0x528011B || old == 0x2B6011B);   /* 1080p or 720p */
                }
            
                else
                {
                    if ((PathDriveMode->zoom > 1) && is_basic) // don't brighten up LiveView in x5/x10 modes for now for is_basic
                    {
                        engio_vidmode_ok = 0;
                    }
                
                    else
                    {
                        engio_vidmode_ok = 1;
                    }
                }
            }
        }
    }

    if (!is_supported_mode())
    {
        /* don't patch other video modes */
        return;
        
        if (is_5D3 || is_basic)
        {
            if (!engio_vidmode_ok)
            {
                return;
            }
        }
    }

    for (uint32_t * buf = (uint32_t *) regs[0]; *buf != 0xFFFFFFFF; buf += 2)
    {
        uint32_t reg = *buf;
        uint32_t old = *(buf+1);
        
        int new = reg_override_func(reg, old);
        if (new)
        {
            dbg_printf("[%x] %x: %x -> %x\n", regs[0], reg, old, new);
            *(buf+1) = new;
        }

        /* brighten up LiveView when using negative analog gain in lower bit-depths */

        // this method seems better in terms of stability, it doesn't produce corrupted frames,
        // but it also affect autofocus when using negative analog gain which makes it inaccurate.
        // (read about the other method for more info.)
        if ((brighten_lv_method == 0 && RECORDING) || (brighten_lv_method == 0 && RAW_HISTOGRAM_ENABLED))//When RAW histogram is used turn off the temporary 14bit stuff
        {
            //Workaround when small_hacks is set to More in mlv_lite.c
            if ((!Arrows_U_D && Arrows_L_R != 3 && SET_button != 2 && SET_button != 3) || more_hacks)
            {
                if (OUTPUT_12BIT)
                {
                    EngDrvOutLV(0xC0F42744, 0x2020202);
                }
                if (OUTPUT_11BIT)
                {
                    EngDrvOutLV(0xC0F42744, 0x3030303);
                }
                if (OUTPUT_10BIT)
                {
                    EngDrvOutLV(0xC0F42744, 0x4040404);
                }
            }
            else
            {
                
                if (reg == 0xC0F42744)
                {
                    if (which_output_format() >= 3) // don't patch if we are using uncompressed RAW
                    {
                        if (OUTPUT_12BIT && old != 0x2020202)
                        {
                            *(buf+1) = 0x2020202;
                        }
                
                        if (OUTPUT_11BIT && old != 0x3030303)
                        {
                            *(buf+1) = 0x3030303;
                        }
                
                        if (OUTPUT_10BIT && old != 0x4040404)
                        {
                            *(buf+1) = 0x4040404;
                        }
                    }
                }
            }
        }

        /* it seems more reliable to override them directly from here */
        if (Preview_Control)
        {
            switch (reg)
            {
                case 0xC0F1A00C: *(buf+1) = (Preview_V << 16) + Preview_H - 0x1;  break;
                case 0xC0F11B9C: *(buf+1) = (Preview_V << 16) + Preview_H - 0x1;  break;

                case 0xC0F11B8C: *(buf+1) = YUV_HD_S_H;                           break;
                case 0xC0F11BCC: *(buf+1) = YUV_HD_S_V;                           break;
                case 0xC0F11BC8: *(buf+1) = YUV_HD_S_V_E;                         break;
                case 0xC0F11ACC: *(buf+1) = YUV_LV_S_V;                           break;
                case 0xC0F04210: *(buf+1) = YUV_LV_Buf;                           break;
            }
        }
    }
}

static int change_buffer_now = 0;

static void FAST EngDrvOut_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    if (!is_supported_mode())
    {
        /* don't patch other video modes */
        return;
    }
    
    uint32_t data = (uint32_t) regs[0];
    uint16_t dst = (data & 0xFFFF0000) >> 16;
    uint16_t reg = data & 0x0000FFFF;
    uint32_t val = (uint32_t) regs[1];

    /* brighten up LiveView when using negative analog gain in lower bit-depths */
    /* this method may produce corrupted frames in some settings (in lowest bit-depth, high ISO and resolution combos?) 
     * but it has accurate AF ...                                                                                   */

    /* These four registers apply positive gain for preview per RGB channel, two for green, one for red, one for blue.
     * The pervious register which was used to brighten LV is 0xC0F42744, altough it can correct preview brightness
     * in LiveView, but according to autofocus data it will stay underexposed, resulting in autofocus failure/loss when
     * using lower bit-depths in lossless. The following four regisers can correct LV image brightness beside it will
     * affect autofocus data --> the four regisers will adjust autofocus data to the correct brightness too . .
     * --> This way autofocus in 10/11/12-bit will be as accurate as in 14-bit.
     * BTW, these regisers used to achieve 12800 digital ISO from Canon.                                           */
    if ((brighten_lv_method == 1 && RECORDING) || (brighten_lv_method == 1 && RAW_HISTOGRAM_ENABLED))
    {
        if (data == 0xC0F37AE4 || data == 0xC0F37AF0 || data == 0xC0F37AFC || data == 0xC0F37B08) 
        {
            if (which_output_format() >= 3) // don't patch if we are using uncompressed RAW 
            {
                if (OUTPUT_12BIT) regs[1] = 0x30100;
                if (OUTPUT_11BIT) regs[1] = 0x40100;
                if (OUTPUT_10BIT) regs[1] = 0x50100;
            }
        }
    }

    // adjust LiveView black level when using lower bit-depths with negative analog gain
    if (data == 0xC0F0819C)
    {
        // 100D doesn't need this
        if (is_650D || is_700D || is_EOSM)
        {
            if (lens_info.iso_analog_raw == ISO_400)
            {
                if (OUTPUT_10BIT) regs[1] = 0xC39;
                if (OUTPUT_11BIT) regs[1] = 0xC39;
                if (OUTPUT_12BIT) regs[1] = 0xC39;
            }
            if (lens_info.iso_analog_raw == ISO_800)
            {
                if (OUTPUT_10BIT) regs[1] = 0xC3C;
                if (OUTPUT_11BIT) regs[1] = 0xC3A;
                if (OUTPUT_12BIT) regs[1] = 0xC3A;
            }
            if (lens_info.iso_analog_raw == ISO_1600)
            {
                if (OUTPUT_10BIT) regs[1] = 0xC41;
                if (OUTPUT_11BIT) regs[1] = 0xC40;
                if (OUTPUT_12BIT) regs[1] = 0xC40;
            }
            if (lens_info.iso_analog_raw == ISO_3200)
            {
                if (OUTPUT_10BIT) regs[1] = 0xC4A;
                if (OUTPUT_11BIT) regs[1] = 0xC48;
                if (OUTPUT_12BIT) regs[1] = 0xC48;
            }
            if (lens_info.iso_analog_raw == ISO_6400)
            {
                if (OUTPUT_10BIT) regs[1] = 0xC5C;
                if (OUTPUT_11BIT) regs[1] = 0xC5B;
                if (OUTPUT_12BIT) regs[1] = 0xC58;
            }
            if (lens_info.iso_analog_raw == ISO_12800)
            {
                if (OUTPUT_10BIT) regs[1] = 0xC5C;
                if (OUTPUT_11BIT) regs[1] = 0xC5A;
                if (OUTPUT_12BIT) regs[1] = 0xC58;
            }
        }
    }

    /* makes LiveView smoother when using 3x3 presets for is_DIGIC_5 models when aewb task is active 
     * the values taken when setting focus box to center then pressing down button one time in x5 mode.
     * it seems these also make preview more reliable in these modes, in some focus box positions preview
     * become black without these values */
    if ((CROP_PRESET_MENU == CROP_PRESET_3X3 || CROP_PRESET_MENU == CROP_PRESET_1X3) ||
       ((CROP_PRESET_MENU == CROP_PRESET_1X1))) // also for 1280p preset
    {
        if (data == 0xC0F09050) {regs[1] =   0x3002D0;}
        if (data == 0xC0F09054) {regs[1] =  0x2E006D8;}
    }

    if (dst == 0xC0F2)
    {
        // 0xC0F26808 register sets EDMAC#24 buffer address, change it to Photo mode buffer address
        if (EDMAC_24_Redirect)
        {
            // we need to know when to override 0xC0F26808, detect it from 0xC0F26804, it's always
            // being set to 0x40000000 before setting 0xC0F26808 value
            if (reg == 0x6804 && val == 0x40000000) 
            {
                change_buffer_now = 1;
            }
    
            if (reg == 0x6808 && change_buffer_now == 1) // 0xC0F26808
            {
                if (is_650D || is_700D || is_EOSM)
                {
                    regs[1] = 0x1595b00; // Size 0xC0F26810  = 0x3237e  is being set in EngDrvOuts_hook
                }
            
                if (is_100D)
                {
                    regs[1] = 0x2000000; // Size 0xC0F26810  = 0x1f32da is being set in EngDrvOuts_hook
                }
            
                change_buffer_now = 0;
            }
        }
    }

    /* set our preview registers overrides */
    if (dst == 0xC0F3)
    {
        if (Preview_Control)
        {
            switch (reg)
            {
                case 0x8070: regs[1] = ((Preview_V + 0x9) << 16) + Preview_H / 4 + 5;       break;
                case 0x8078: regs[1] = (((Preview_H / 4) + 6) << 16) + 1;                   break;
                case 0x807C: regs[1] = ((Preview_H / 4) + 5) << 16;                         break;
                case 0x8080: regs[1] = ((Preview_V + 0x7) << 16) + 2;                       break;
                case 0x8084: regs[1] = ((Preview_H / 4) + 7) << 16;                         break;
                case 0x8094: regs[1] = ( Preview_V + 0xa) << 16;                            break;
                case 0x80A0: regs[1] = ((Preview_H / 4) + 7) << 16;                         break;
                case 0x80A4: regs[1] = ((Preview_H / 4) + 7) << 16;                         break;
                case 0x8024: 
                if (is_700D || is_EOSM || is_650D)
                             regs[1] = ((RAW_V - 1) << 16)  + RAW_H - 0x11;                 
                if (is_100D) regs[1] = ((RAW_V - 5) << 16)  + RAW_H - 0x1A;                 break;
                case 0x83D4: regs[1] =   Preview_R;                                         break;
                case 0x83DC: regs[1] = ((Preview_V + 0x1c) << 16)  + Preview_H / 4 + 0x48
                                                                   + REG_C0F383DC_Tuning;   break;
                case 0x8934: regs[1] = ((Preview_V + 0x6) << 16)   + Preview_H / 4 + 5;     break;
                case 0x8960: regs[1] = ( Preview_V + 0x6) << 16;                            break;
                case 0x89A4: regs[1] = ((Preview_V + 0x6) << 16)   + Preview_H / 4 + 5;     break;
                case 0x89B4: regs[1] = ((Preview_V + 0x7) << 16)   + Preview_H / 4 + 6;     break;
                case 0x89D4: regs[1] = ((Preview_V + 0x6) << 16)   + Preview_H / 4 + 5;     break;
                case 0x89E4: regs[1] = ((Preview_V + 0x7) << 16)   + Preview_H / 4 + 7;     break;
                case 0x89EC: regs[1] = ((Preview_H / 4 + 6) << 16) + 1;                     break;
                
            //  case 0xA04C: regs[1] = ((Preview_V + 0x6) << 16)   + Preview_H / 4 + 5;     break; // It's being set in EngDrvOuts_hook
            //  case 0xA0A0: regs[1] = ((Preview_V + 0xa) << 16)   + Preview_H + 0xb;       break; // It's being set in EngDrvOuts_hook
            //  case 0xA0B0: regs[1] = ((Preview_V + 0xa) << 16)   + Preview_H + 0x8;       break; // It's being set in EngDrvOuts_hook
                case 0xB038: regs[1] =  Black_Bar;                                          break;
                case 0xB088: regs[1] =  Black_Bar;                                          break;
                case 0xB054: regs[1] = ((Preview_V + 0x6) << 16)   + Preview_H + 0x7;       break;
                case 0xB070: regs[1] = ((Preview_V + 0x6) << 16)   + Preview_H + 0x57;      break;
                case 0xB074: regs[1] = ( Preview_V        << 16)   + Preview_H + 0x57;      break;
                case 0xB0DC: regs[1] = ( Preview_V        << 16)   + Preview_H + 0x4f;      break;
            }
        }

        // basic preview: fix broken preview casued by increasing width RAW resolution, also center the preview
        if (Preview_Control_Basic)
        {
            switch (reg)
            {
                case 0x8024: 
                if (is_700D || is_EOSM || is_650D)
                             regs[1] = ((RAW_V - 1) << 16)  + RAW_H - 0x11;                 
                if (is_100D) regs[1] = ((RAW_V - 5) << 16)  + RAW_H - 0x1A;                 break;
                
                /* used here to center Canon cropped preview on RAW buffer */
                case 0x83D4: regs[1] =  (Preview_y1 << 16) + Preview_x1;                    break;
                case 0x83DC: regs[1] =  (Preview_y2 << 16) + Preview_x2;                    break;
            }
        }
    }

    if (dst == 0xC0F4)
    {
        if (Preview_Control)
        {
            switch (reg)
            {
                case 0x2014: regs[1] = ((Preview_V + 0x9) << 16) + Preview_H / 4 + 5;      break;
                case 0x204C: regs[1] = ((Preview_V + 0x9) << 16) + Preview_H / 4 + 5;      break;
                case 0x2194: regs[1] = ( Preview_H / 4) + 5;                               break;
            }
        }
    }
}

static void FAST EngDrvOuts_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    if (!is_supported_mode())
    {
        /* don't patch other video modes */
        return;
    }

    uint32_t data = (uint32_t) regs[0];
//  uint16_t dst = (data & 0xFFFF0000) >> 16;
//  uint16_t reg = data & 0x0000FFFF;
//  uint32_t * val = (uint32_t*) regs[1];
//  uint32_t num = (uint32_t) regs[2];

    if (Preview_Control)
    {
        /* set our preview registers overrides */
        if (data == 0xC0F3A048)
        {
            *(uint32_t*) (regs[1] + 4)    = ((Preview_V + 0x6) << 16) + Preview_H / 4 + 5; // 0xC0F3A04C
        }
        
        if (data == 0xC0F3A098)
        {
            *(uint32_t*) (regs[1] + 8)    = ((Preview_V + 0xa) << 16) + Preview_H + 0xb;   // 0xC0F3A0A0
            *(uint32_t*) (regs[1] + 0x18) = ((Preview_V + 0xa) << 16) + Preview_H + 0x8;   // 0xC0F3A0B0
        }
    }

    /* change EDMAC#24 buffer size 0xC0F26810 to photo mode buffer size */
    if (EDMAC_24_Redirect)
    {
        if (data == 0xC0F2680C)
        {
            // we need to know when to set buffer size because the channel does other things before
            // setting the final buffer size which we want to change, let's use 0xC0F35084 as flag because
            // it's always being set after "the other things" finish and before setting 0xC0F26810 final size
            if (shamem_read(0xC0F35084) == 0xA1F)
            {
                if (is_650D || is_700D || is_EOSM)
                {
                    *(uint32_t*) (regs[1] + 4) = 0x3237e;
                }

                if (is_100D)
                {
                    *(uint32_t*) (regs[1] + 4) = 0x1f32da;
                }
            }
        }  
    }
}

/* sometime and for some reaseon not all preview registers get overriden, especially the ones in engio_write hook and when HDMI is connected */
/* while idle let's check preview registers values, if they don't match our values, set values using EngDrvOut call */
uint32_t REG_C0F38024_Val = 0;
void CheckPreviewRegsValuesAndForce()
{
    if (!lv) return;
    if (!CROP_PRESET_MENU) return;
    if (lv_dispsize != 5) return;
    if (PathDriveMode->zoom != 5) return;
    if (Preview_Control_Basic) return;

#ifdef CONFIG_EOSM
    /* EOS M: engio hooks already patch preview; forcing registers here fights Canon
     * and stalls the whole UI (laggy audio meters, menu won't open). Recovery uses
     * normal x5 zoom path instead. */
    return;
#endif

    if (is_100D)                       REG_C0F38024_Val = ((RAW_V - 5) << 16)  + RAW_H - 0x1A;
    if (is_650D || is_700D || is_EOSM) REG_C0F38024_Val = ((RAW_V - 1) << 16)  + RAW_H - 0x11;

    if (shamem_read(0xC0F38070) != ((Preview_V + 0x9) << 16) + Preview_H / 4 + 5      ||
        shamem_read(0xC0F38078) != (((Preview_H / 4) + 6) << 16) + 1                  ||
        shamem_read(0xC0F3807C) != ((Preview_H / 4) + 5) << 16                        ||
        shamem_read(0xC0F38080) != ((Preview_V + 0x7) << 16) + 2                      ||
        shamem_read(0xC0F38084) != ((Preview_H / 4) + 7) << 16                        ||
        shamem_read(0xC0F38094) != ( Preview_V + 0xa) << 16                           ||
        shamem_read(0xC0F380A0) != ((Preview_H / 4) + 7) << 16                        ||
        shamem_read(0xC0F380A4) != ((Preview_H / 4) + 7) << 16                        ||
        shamem_read(0xC0F38024) != REG_C0F38024_Val                                   ||
        shamem_read(0xC0F383D4) != Preview_R                                          ||
        shamem_read(0xC0F383DC) != ((Preview_V + 0x1c) << 16)  + Preview_H / 4 + 0x48 
                                                               + REG_C0F383DC_Tuning  ||
        shamem_read(0xC0F38934) != ((Preview_V + 0x6) << 16)   + Preview_H / 4 + 5    ||
        shamem_read(0xC0F38960) != ( Preview_V + 0x6) << 16                           ||
        shamem_read(0xC0F389A4) != ((Preview_V + 0x6) << 16)   + Preview_H / 4 + 5    ||
        shamem_read(0xC0F389B4) != ((Preview_V + 0x7) << 16)   + Preview_H / 4 + 6    ||
        shamem_read(0xC0F389D4) != ((Preview_V + 0x6) << 16)   + Preview_H / 4 + 5    ||
        shamem_read(0xC0F389E4) != ((Preview_V + 0x7) << 16)   + Preview_H / 4 + 7    ||
        shamem_read(0xC0F389EC) != ((Preview_H / 4 + 6) << 16) + 1                    ||
        shamem_read(0xC0F42014) != ((Preview_V + 0x9) << 16) + Preview_H / 4 + 5      ||
        shamem_read(0xC0F4204C) != ((Preview_V + 0x9) << 16) + Preview_H / 4 + 5      ||
        shamem_read(0xC0F42194) != ( Preview_H / 4) + 5                               ||
        shamem_read(0xC0F3A04C) != ((Preview_V + 0x6) << 16)   + Preview_H / 4 + 5    ||
        shamem_read(0xC0F3A0A0) != ((Preview_V + 0xa) << 16)   + Preview_H + 0xb      ||
        shamem_read(0xC0F3A0B0) != ((Preview_V + 0xa) << 16)   + Preview_H + 0x8      ||
        shamem_read(0xC0F3B054) != ((Preview_V + 0x6) << 16)   + Preview_H + 0x7      ||
        shamem_read(0xC0F3B070) != ((Preview_V + 0x6) << 16)   + Preview_H + 0x57     ||
        shamem_read(0xC0F3B074) != ( Preview_V        << 16)   + Preview_H + 0x57     ||
        shamem_read(0xC0F3B0DC) != ( Preview_V        << 16)   + Preview_H + 0x4f     ||
        shamem_read(0xC0F1A00C) != (Preview_V << 16) + Preview_H - 0x1                ||
        shamem_read(0xC0F11B9C) != (Preview_V << 16) + Preview_H - 0x1                ||
        shamem_read(0xC0F11B8C) != YUV_HD_S_H                                         ||
        shamem_read(0xC0F11BCC) != YUV_HD_S_V                                         ||
        shamem_read(0xC0F11BC8) != YUV_HD_S_V_E                                       ||
        shamem_read(0xC0F11ACC) != YUV_LV_S_V                                         ||
        shamem_read(0xC0F04210) != YUV_LV_Buf                                          )
        {
            EngDrvOutLV(0xC0F38070, ((Preview_V + 0x9) << 16) + Preview_H / 4 + 5);
            EngDrvOutLV(0xC0F38078, (((Preview_H / 4) + 6) << 16) + 1);
            EngDrvOutLV(0xC0F3807C, ((Preview_H / 4) + 5) << 16);
            EngDrvOutLV(0xC0F38080, ((Preview_V + 0x7) << 16) + 2);
            EngDrvOutLV(0xC0F38084, ((Preview_H / 4) + 7) << 16);
            EngDrvOutLV(0xC0F38094, ( Preview_V + 0xa) << 16);
            EngDrvOutLV(0xC0F380A0, ((Preview_H / 4) + 7) << 16);
            EngDrvOutLV(0xC0F380A4, ((Preview_H / 4) + 7) << 16);
            EngDrvOutLV(0xC0F38024, REG_C0F38024_Val);
            EngDrvOutLV(0xC0F383D4, Preview_R);
            EngDrvOutLV(0xC0F383DC, ((Preview_V + 0x1c) << 16)  + Preview_H / 4 + 0x48 
                                                                + REG_C0F383DC_Tuning);
            EngDrvOutLV(0xC0F38934, ((Preview_V + 0x6) << 16)   + Preview_H / 4 + 5);
            EngDrvOutLV(0xC0F38960, ( Preview_V + 0x6) << 16);
            EngDrvOutLV(0xC0F389A4, ((Preview_V + 0x6) << 16)   + Preview_H / 4 + 5);
            EngDrvOutLV(0xC0F389B4, ((Preview_V + 0x7) << 16)   + Preview_H / 4 + 6);
            EngDrvOutLV(0xC0F389D4, ((Preview_V + 0x6) << 16)   + Preview_H / 4 + 5);
            EngDrvOutLV(0xC0F389E4, ((Preview_V + 0x7) << 16)   + Preview_H / 4 + 7);
            EngDrvOutLV(0xC0F389EC, ((Preview_H / 4 + 6) << 16) + 1);
            EngDrvOutLV(0xC0F42014, ((Preview_V + 0x9) << 16) + Preview_H / 4 + 5);
            EngDrvOutLV(0xC0F4204C, ((Preview_V + 0x9) << 16) + Preview_H / 4 + 5);
            EngDrvOutLV(0xC0F42194, ( Preview_H / 4) + 5);
            EngDrvOutLV(0xC0F3A04C, ((Preview_V + 0x6) << 16)   + Preview_H / 4 + 5);
            EngDrvOutLV(0xC0F3A0A0, ((Preview_V + 0xa) << 16)   + Preview_H + 0xb);
            EngDrvOutLV(0xC0F3A0B0, ((Preview_V + 0xa) << 16)   + Preview_H + 0x8);
            EngDrvOutLV(0xC0F3B054, ((Preview_V + 0x6) << 16)   + Preview_H + 0x7);
            EngDrvOutLV(0xC0F3B070, ((Preview_V + 0x6) << 16)   + Preview_H + 0x57);
            EngDrvOutLV(0xC0F3B074, ( Preview_V        << 16)   + Preview_H + 0x57);
            EngDrvOutLV(0xC0F3B0DC, ( Preview_V        << 16)   + Preview_H + 0x4f);
            EngDrvOutLV(0xC0F1A00C, (Preview_V << 16) + Preview_H - 0x1);
            EngDrvOutLV(0xC0F11B9C, (Preview_V << 16) + Preview_H - 0x1);
            EngDrvOutLV(0xC0F11B8C, YUV_HD_S_H);
            EngDrvOutLV(0xC0F11BCC, YUV_HD_S_V);
            EngDrvOutLV(0xC0F11BC8, YUV_HD_S_V_E);
            EngDrvOutLV(0xC0F11ACC, YUV_LV_S_V);
            EngDrvOutLV(0xC0F04210, YUV_LV_Buf);
        }
}

// 0xC0F04908 register sets EDMAC#9 address, I think it holds darkframe subtraction data, change it to photo mode address (use darkframe data from photo mode)
// cleaner preview this way in presets which exceed default vertical RAW resolution (above 1080 vertical pixels), photo mode data should cover height up to 3528
// 0xC0F04908 changes among two addresses in LiveView, one dedicated for RAW_H and other one for RAW_V, we want to change the address for RAW_V (our hook does that)
// note: I am not sure what I am doing
static void Change_HIV_V_Address(uint32_t* regs, uint32_t* stack, uint32_t pc)     
{
    regs[1] = HIV_Vertical_Photo_Address;
}

int is_LCD_Output()
{
    if (PathDriveMode->OutputType == 0)
    {
        return 1;
    }

    return 0;
}

int is_480p_Output()
{
    if (PathDriveMode->OutputType == 7)
    {
        return 1;
    }

    return 0;
}

int is_1080i_Full_Output()
{
    if (PathDriveMode->OutputType == 3)
    {
        return 1;
    }

    return 0;
}

int is_1080i_Info_Output()
{
    if (PathDriveMode->OutputType == 4)
    {
        return 1;
    }

    return 0;
}

static uint32_t ShiftAddress  = 0;
static uint32_t ClearAddress  = 0;
static uint32_t DefaultShift  = 0; // expected value which we want to patch
static uint32_t DefaultClear  = 0; // expected value which we want to patch
static uint32_t NewShiftVal   = 0; // new shift value, trial and error
static uint32_t NewClearVal   = 0; // new clear value, should be same as width from 0xC0F04210?

int GetShiftValue()
{
    if (CROP_PRESET_MENU == CROP_PRESET_1X1)
    {
        switch (crop_preset_1x1_res)
         {
            case 0:
            case 1:
            {
                if (is_LCD_Output())  return 0x1F4A0;
                if (is_480p_Output()) return 0x1B160;
                if (is_1080i_Full_Output()) return 0x5BF2C;
                if (is_1080i_Info_Output()) return 0x5ED58;
            }

            case 2:
            {
                if (is_LCD_Output())  return 0x1F4D0;
                if (is_480p_Output()) return 0x1B18C;
                if (is_1080i_Full_Output()) return 0x5BF9C;
                if (is_1080i_Info_Output()) return 0x5EDB0;
            }

            case 3: 
            {
                if (is_LCD_Output())  return 0xD5C0;
                if (is_480p_Output()) return 0xB9E0;
                if (is_1080i_Full_Output()) return 0x2772C;
                if (is_1080i_Info_Output()) return 0x33B58;
            }

            case 4:
            {
                if (is_LCD_Output()) return 0;
            }
        }        
    }

    if (CROP_PRESET_MENU == CROP_PRESET_1X3 || CROP_PRESET_MENU == CROP_PRESET_3X3)
    {
        switch (crop_preset_ar)
        {
            case 0:
            {
                if (is_LCD_Output())  return 0xD5C0;
                if (is_480p_Output()) return 0xB9E0;
                if (is_1080i_Full_Output()) return 0x2772C;
                if (is_1080i_Info_Output()) return 0x33B58;
            }            
            case 1:
            {
                if (is_LCD_Output())  return 0x15720;
                if (is_480p_Output()) return 0x124C0;
                if (is_1080i_Full_Output()) return 0x3FD2C;
                if (is_1080i_Info_Output()) return 0x46758;
            }            
            case 2:
            {
                if (is_LCD_Output())  return 0x1B6C0;
                if (is_480p_Output()) return 0x17380;
                if (is_1080i_Full_Output()) return 0x51A2C;
                if (is_1080i_Info_Output()) return 0x55758;
            }            
            case 3:
            case 4:
            {
                if (is_LCD_Output())  return 0x1F4A0;
                if (is_480p_Output()) return 0x1B160;
                if (is_1080i_Full_Output()) return 0x5BF2C;
                if (is_1080i_Info_Output()) return 0x5ED58;
            }
        }  
    }

    return 0;
}

void SetAspectRatioCorrectionValues()
{
    if (CROP_PRESET_MENU == CROP_PRESET_1X1)
    {
        if (is_LCD_Output())
        {
            switch (crop_preset_1x1_res)
            {
                case 0:  YUV_LV_Buf = 0x13505A0; YUV_LV_S_V = 0x1050244; break; // CROP_2_5K
                case 1:                                                         // CROP_2_8K
                case 2:  YUV_LV_Buf = 0x13305A0; YUV_LV_S_V = 0x1050248; break; // CROP_3K
                case 3:  YUV_LV_Buf = 0x19505A0; YUV_LV_S_V = 0x10501BA; break; // CROP_1440p
                case 6:  YUV_LV_Buf = 0x1B505A0; YUV_LV_S_V = 0x10501B2; break; // CROP_1620p (dannephoto)
                default: YUV_LV_Buf = 0x1DF05A0; YUV_LV_S_V = 0x1E002B;  break;
            }
        }
        if (is_480p_Output())
        {
            switch (crop_preset_1x1_res)
            {
                case 0:                                                         // CROP_2_5K
                case 1:                                                         // CROP_2_8K
                case 2:                                                         // CROP_3K
                         YUV_LV_Buf = 0x1170520; YUV_LV_S_V = 0x1050282; break;
                case 3:  YUV_LV_Buf = 0x1710520; YUV_LV_S_V = 0x10501E5; break; // CROP_1440p
                default: YUV_LV_Buf = 0x1830520; YUV_LV_S_V = 0x6100AC;  break;
            }
        }
        if (is_1080i_Full_Output())
        {
            switch (crop_preset_1x1_res)
            {
                case 0:                                                         // CROP_2_5K
                case 1:                                                         // CROP_2_8K
                case 2:                                                         // CROP_3K
                         YUV_LV_Buf = 0x1580CA8; YUV_LV_S_V = 0x1050209; break;
                case 3:  YUV_LV_Buf = 0x1C70CA8; YUV_LV_S_V = 0x105018A; break; // CROP_1440p
                default: YUV_LV_Buf = 0x21B0CA8; YUV_LV_S_V = 0x8700AC;  break;
            }
        }
        if (is_1080i_Info_Output())
        {
            switch (crop_preset_1x1_res)
            {
                case 0:                                                         // CROP_2_5K
                case 1:                                                         // CROP_2_8K
                case 2:                                                         // CROP_3K
                         YUV_LV_Buf = 0x1180A50; YUV_LV_S_V = 0x1050280; break;
                case 3:  YUV_LV_Buf = 0x1730A50; YUV_LV_S_V = 0x10501E3; break; // CROP_1440p
                default: YUV_LV_Buf = 0x1B70A50; YUV_LV_S_V = 0x370056;  break;
            }
        }
    }

    if (CROP_PRESET_MENU == CROP_PRESET_1X3 || CROP_PRESET_MENU == CROP_PRESET_3X3)
    {
        if (is_LCD_Output())
        {
            switch (crop_preset_ar)
            {
                case 0: YUV_LV_Buf = 0x19505A0; YUV_LV_S_V = 0x10501BA; break; // AR_16_9
                case 1: YUV_LV_Buf = 0x16805A0; YUV_LV_S_V = 0x10501F2; break; // AR_2_1
                case 2: YUV_LV_Buf = 0x14805A0; YUV_LV_S_V = 0x1050222; break; // AR_2_20_1
                case 3: // AR_2_35_1
                case 4: // AR_2_39_1
                {
                    YUV_LV_Buf = 0x13205A0; YUV_LV_S_V = 0x1050249;
                }
                break;
            }  
        }
        if (is_480p_Output())
        {
            switch (crop_preset_ar)
            {
                case 0: YUV_LV_Buf = 0x1710520; YUV_LV_S_V = 0x10501E5; break; // AR_16_9
                case 1: YUV_LV_Buf = 0x1480520; YUV_LV_S_V = 0x1050222; break; // AR_2_1
                case 2: YUV_LV_Buf = 0x12A0520; YUV_LV_S_V = 0x1050259; break; // AR_2_20_1
                case 3: // AR_2_35_1
                case 4: // AR_2_39_1
                {
                    YUV_LV_Buf = 0x1170520; YUV_LV_S_V = 0x1050282;
                }
                break;
            }  
        }
        if (is_1080i_Full_Output())
        {
            switch (crop_preset_ar)
            {
                case 0: YUV_LV_Buf = 0x1C70CA8; YUV_LV_S_V = 0x105018A; break; // AR_16_9
                case 1: YUV_LV_Buf = 0x1950CA8; YUV_LV_S_V = 0x10501BA; break; // AR_2_1
                case 2: YUV_LV_Buf = 0x1700CA8; YUV_LV_S_V = 0x10501E7; break; // AR_2_20_1
                case 3: // AR_2_35_1
                case 4: // AR_2_39_1
                {
                    YUV_LV_Buf = 0x1580CA8; YUV_LV_S_V = 0x1050209;
                }
                break;
            }  
        }
        if (is_1080i_Info_Output())
        {
            switch (crop_preset_ar)
            {
                case 0: YUV_LV_Buf = 0x1730A50; YUV_LV_S_V = 0x10501E3; break; // AR_16_9
                case 1: YUV_LV_Buf = 0x14A0A50; YUV_LV_S_V = 0x105021F; break; // AR_2_1
                case 2: YUV_LV_Buf = 0x12C0A50; YUV_LV_S_V = 0x1050255; break; // AR_2_20_1
                case 3: // AR_2_35_1
                case 4: // AR_2_39_1
                {
                    YUV_LV_Buf = 0x1180A50; YUV_LV_S_V = 0x1050280;
                }
                break;
            }  
        }
    }

    /* Set default x5 mode values for mv1080 preset, also for Anam_FLV */
    if ((CROP_PRESET_MENU == CROP_PRESET_3X3 && (crop_preset_3x3_res == 1 || crop_preset_3x3_res == 2)) || // mv1080
        (CROP_PRESET_MENU == CROP_PRESET_1X3 && crop_preset_1x3_res == 3))   // Anam_FLV
    {
        if (is_LCD_Output()){        YUV_LV_Buf = 0x1DF05A0; YUV_LV_S_V = 0x1E002B;}
        if (is_480p_Output()){       YUV_LV_Buf = 0x1830520; YUV_LV_S_V = 0x6100AC;}
        if (is_1080i_Full_Output()){ YUV_LV_Buf = 0x21B0CA8; YUV_LV_S_V = 0x8700AC;}
        if (is_1080i_Info_Output()){ YUV_LV_Buf = 0x1B70A50; YUV_LV_S_V = 0x370056;}
    }
}

static void FAST PATH_SelectPathDriveMode_hook(uint32_t* regs, uint32_t* stack, uint32_t pc)
{
    /* we need to enable and set preview shifting and clearing artifacts values here especially for clear artifacts value */
    /* I don't know which function load shifting preview value, but it's being loaded and applied many times in LiveView, not just once. */
    /* clear artifacts value is being loaded very early before CMOS, ADTG, ENGIO, ENG_DRV_OUT, ENG_DRV_OUTS stuff */
    /* in [VRAM] VRAM_PTH_StartTripleRamClearInALump[ff962a90] (ff962a90 + 0x8 holds value for clearing artifacts for x5 mode for LCD output on 700D) */
    /* apparently PATH_SelectPathDriveMode sets its arguments before loading/applying any video configuration, e.g: */

    /*  700D DebugLog, x5 mode:
    
        Evf:ff19ce1c:ad:03: PATH_Select S:8 Z:50000 R:0 DZ:0 SM:0 SV:0 DT:0       <-- we are patching it from here before loading it
        Evf:ff19cfb0:ad:03: PathDriveMode Change: 10->2
        Evf:ff37ad64:ad:03: GetPathDriveInfo[2]
        Evf:ff4f2ce8:ad:03: LVx5_SelectPath LCD
        Evf:ff4f3980:ad:01: LVx5_GetVramParam(W:720 H:480)
        Evf:ff37cebc:ad:03: RamClear_SetPath
        Evf:ff37d4c8:ad:03: LV_ResLockTripleRamClearPass
        Evf:ff4ee18c:a9:03: [VRAM] VRAM_PTH_StartTripleRamClearInALump[ff962a90]  <-- clear artifacts value is being loaded here, ff962a90 is array holding six 32 bits values
        Evf:ff37cf1c:ad:03: RamClear_StartPath
        Evf:ff37d084:ad:03: RamClear_LV_RAMCLEAR_COLOR_BLACK
        Evf:ff37cf1c:ad:03: RamClear_StartPath
        Evf:ff37d084:ad:03: RamClear_LV_RAMCLEAR_COLOR_BLACK
        Evf:ff37cf1c:ad:03: RamClear_StartPath
        Evf:ff37d084:ad:03: RamClear_LV_RAMCLEAR_COLOR_BLACK
        Evf:ff36ce48:a9:03: [VRAM]====>> PathRamClearCompleteCBR   */ 

    /* FIXME: we might be able to implement clearing artifacts directly in VRAM_PTH_StartTripleRamClearInALump
              this way we don't to patch ROM addresses for clearing artifacts for x5 mode and for every output on every model */

    /* always unpatch to check output and update addresses and values */
    if (Center_Preview_ON)
    {
        unpatch_memory(ShiftAddress);
        Center_Preview_ON = 0;
    }

    if (Clear_Artifacts_ON)
    {
        unpatch_memory(ClearAddress);
        Clear_Artifacts_ON = 0;
    }

    /* detect current output and update patch parameters */
    if (is_LCD_Output())
    {
        DefaultShift  = 0x0;
        DefaultClear  = 0x0;
        NewClearVal   = 0x5A0;
        ShiftAddress  = Shift_x5_LCD;
        ClearAddress  = Clear_Vram_x5_LCD;
    }

    if (is_480p_Output())
    {
        DefaultShift  = 0x8740;
        DefaultClear  = 0x40;
        NewClearVal   = 0x520;
        ShiftAddress  = Shift_x5_HDMI_480p;
        ClearAddress  = Clear_Vram_x5_HDMI_480p;
    }

    if (is_1080i_Full_Output())
    {
        DefaultShift  = 0x12C;
        DefaultClear  = 0x12C;
        NewClearVal   = 0xCA8;
        ShiftAddress  = Shift_x5_HDMI_1080i_Full;
        ClearAddress  = Clear_Vram_x5_HDMI_1080i_Full;
    }

    if (is_1080i_Info_Output())
    {
        DefaultShift  = 0x16A58;
        DefaultClear  = 0x258;
        NewClearVal   = 0xA50;
        ShiftAddress  = Shift_x5_HDMI_1080i_Info;
        ClearAddress  = Clear_Vram_x5_HDMI_1080i_Info;
    }

    if (CROP_PRESET_MENU == CROP_PRESET_1X1)
    {
        if (crop_preset_1x1_res == 0)  // CROP_2_5K
        {
            Shift_Preview = 1;
            Clear_Artifacts = 1;
            EDMAC_9_Vertical_Change = 0;
        }

        if (crop_preset_1x1_res == 1)  // CROP_2_8K
        {
            Shift_Preview = 1;
            Clear_Artifacts = 1;
            EDMAC_9_Vertical_Change = 1;
        }

        if (crop_preset_1x1_res == 2)  // CROP_3K
        {
            Shift_Preview = 1;
            Clear_Artifacts = 1;
            EDMAC_9_Vertical_Change = 1;
        }

        if (crop_preset_1x1_res == 3)  // CROP_1440p
        {
            Shift_Preview = 1;
            Clear_Artifacts = 1;
            EDMAC_9_Vertical_Change = 1;
        }

        if (crop_preset_1x1_res == 4 || crop_preset_1x1_res == 7)    // CROP_1280p
        {
            Shift_Preview = 0;
            Clear_Artifacts = 0;
            EDMAC_9_Vertical_Change = 0;
        }

        if (crop_preset_1x1_res == 5)    // CROP_Full_Res
        {
            Shift_Preview = 0;
            Clear_Artifacts = 0;
            EDMAC_9_Vertical_Change = 1;
        }
    }
    
    if (crop_preset_1x1_res == 6)    // CROP_1620p
    {
        Shift_Preview = 0;
        Clear_Artifacts = 1;
        EDMAC_9_Vertical_Change = 0;
    }

    if (CROP_PRESET_MENU == CROP_PRESET_1X3)
    {
		if (crop_preset_1x3_res == 3) // Anam_FLV
		{
			Shift_Preview = 0;
			Clear_Artifacts = 0;
		}
		else
		{
		    Shift_Preview = 1;
			Clear_Artifacts = 1;
		}

        EDMAC_9_Vertical_Change = 1;
    }

    if (CROP_PRESET_MENU == CROP_PRESET_3X3)
    {
        Shift_Preview = 1;
        Clear_Artifacts = 1;
        EDMAC_9_Vertical_Change = 0;
        
        if (crop_preset_3x3_res == 1 || crop_preset_3x3_res == 2) // mv1080 doesn't need them
        {
            Shift_Preview = 0;
            Clear_Artifacts = 0;
        }
    }

    NewShiftVal = GetShiftValue();
    SetAspectRatioCorrectionValues();

    /* restore defualt EDMAC#9 vertical size */
    if (EDMAC_9_Vertical_Change == 0 || PathDriveMode->zoom != 5)
    {
        if (MEM(EDMAC_9_Vertical_1) != 0x453 || MEM(EDMAC_9_Vertical_2) != 0x453)
        {
            MEM(EDMAC_9_Vertical_1)  = 0x453;
            MEM(EDMAC_9_Vertical_2)  = 0x453;
        }
        
        EDMAC_9_Vertical_Change = 0;
    }

    if (PathDriveMode->zoom == 5)
    {
        /* patch supported presets if patch not active */
        if (Shift_Preview && !Center_Preview_ON)
        {
            patch_memory(ShiftAddress, DefaultShift, NewShiftVal, "Center");
            Center_Preview_ON = 1;
        }

        if (Clear_Artifacts && !Clear_Artifacts_ON)
        {
            patch_memory(ClearAddress, DefaultClear, NewClearVal, "Clear");
            Clear_Artifacts_ON = 1;
        }

        /* unpatch not supported presets if patch already active */
        if (!Shift_Preview && Center_Preview_ON)
        {
            unpatch_memory(ShiftAddress);
            Center_Preview_ON = 0;
        }

        if (!Clear_Artifacts && Clear_Artifacts_ON)
        {
            unpatch_memory(ClearAddress);
            Clear_Artifacts_ON = 0;
        }
    }

    /* unpatch in all other modes if patch already active  */
    if (PathDriveMode->zoom != 5)
    {
        if (Center_Preview_ON)
        {
            unpatch_memory(ShiftAddress);
            Center_Preview_ON = 0;
        }

        if (Clear_Artifacts_ON)
        {
            unpatch_memory(ClearAddress);
            Clear_Artifacts_ON = 0;
        }
    }
}

static int patch_active = 0;

#ifdef CONFIG_EOSM
/* EOS M Live View is rebuilt asynchronously after boot, Canon-menu return,
 * record-stop and zoom changes.  Keep the crop hooks quiet until the base
 * x5 pipeline has delivered stable RAW dimensions, then apply once. */
#define EOSM_LV_GUARD_SETTLE_MS 350
#define EOSM_LV_GUARD_QUIET_MS 120
#define EOSM_LV_GUARD_STABLE_FRAMES 3
#define EOSM_LV_GUARD_MAX_RECOVERIES 2
#define EOSM_LV_GUARD_CONTENT_SAMPLES 3
static volatile int eosm_lv_guard_pending = 1;
static volatile int eosm_lv_guard_busy = 0;
static int eosm_lv_guard_started;
static int eosm_lv_guard_state;
static int eosm_lv_guard_stable_frames;
static int eosm_lv_guard_width;
static int eosm_lv_guard_height;
static int eosm_lv_guard_pitch;
static uintptr_t eosm_lv_guard_raw_buffer;
static uint32_t eosm_lv_guard_display_buffer;
static int eosm_lv_guard_quiet_since;
static int eosm_lv_guard_retries;
static int eosm_lv_guard_internal_zoom;
static int eosm_lv_guard_content_dark_frames;
static int eosm_lv_guard_content_retries;
static int eosm_lv_guard_route_retries;

enum eosm_lv_guard_state
{
    EOSM_LV_GUARD_WAIT = 0,
    EOSM_LV_GUARD_APPLY_X1,
    EOSM_LV_GUARD_APPLY_X5,
    EOSM_LV_GUARD_VALIDATE,
    EOSM_LV_GUARD_CONTENT,
    EOSM_LV_GUARD_ROUTE,
    EOSM_LV_GUARD_RECOVER_X1,
    EOSM_LV_GUARD_RECOVER_X5,
};

/* Exported to the core touch router: do not accept a new control gesture
 * while Canon is rebuilding the sensor/display path. */
__attribute__((used, noinline))
int crop_rec_lv_transition_busy(void)
{
    return eosm_lv_guard_busy;
}

static void eosm_lv_guard_request(void)
{
    eosm_lv_guard_pending = 1;
    eosm_lv_guard_busy = 1;
    eosm_lv_guard_started = 0;
    eosm_lv_guard_state = EOSM_LV_GUARD_WAIT;
    eosm_lv_guard_stable_frames = 0;
    eosm_lv_guard_width = 0;
    eosm_lv_guard_height = 0;
    eosm_lv_guard_pitch = 0;
    eosm_lv_guard_raw_buffer = 0;
    eosm_lv_guard_display_buffer = 0;
    eosm_lv_guard_quiet_since = 0;
    eosm_lv_guard_retries = 0;
    eosm_lv_guard_content_dark_frames = 0;
    eosm_lv_guard_content_retries = 0;
    eosm_lv_guard_route_retries = 0;
}

static void eosm_lv_guard_set_zoom(int zoom)
{
    eosm_lv_guard_internal_zoom = 1;
    set_zoom(zoom);
}
#else
int crop_rec_lv_transition_busy(void) { return 0; }
int crop_rec_lv_transition_diag(char *buffer, int size)
{
    if (buffer && size > 0) buffer[0] = '\0';
    return 0;
}
static void eosm_lv_guard_request(void) {}
#endif

static void install_patches()
{
    patch_hook_function(CMOS_WRITE, MEM_CMOS_WRITE, &cmos_hook, "crop_rec: CMOS[1,2,6] parameters hook");
    patch_hook_function(ADTG_WRITE, MEM_ADTG_WRITE, &adtg_hook, "crop_rec: ADTG[8000,8806] parameters hook");
    if (ENGIO_WRITE) patch_hook_function(ENGIO_WRITE, MEM_ENGIO_WRITE, engio_write_hook, "crop_rec: video timers hook");
    if (ENG_DRV_OUT) patch_hook_function(ENG_DRV_OUT, MEM(ENG_DRV_OUT), EngDrvOut_hook, "crop_rec: preview stuff 1");
    if (ENG_DRV_OUTS) patch_hook_function(ENG_DRV_OUTS, MEM(ENG_DRV_OUTS), EngDrvOuts_hook, "crop_rec: preview stuff 2");
    if (PATH_SelectPathDriveMode) patch_hook_function(PATH_SelectPathDriveMode, MEM(PATH_SelectPathDriveMode), PATH_SelectPathDriveMode_hook, "crop_rec: preview stuff 3");
    if (HIV_Vertical_Address_hook) patch_hook_function(HIV_Vertical_Address_hook, MEM(HIV_Vertical_Address_hook), Change_HIV_V_Address, "crop_rec: preview stuff 4");
}

static void uninstall_patches()
{
    unpatch_memory(CMOS_WRITE);
    unpatch_memory(ADTG_WRITE);
    if (ENGIO_WRITE) unpatch_memory(ENGIO_WRITE);
    if (ENG_DRV_OUT) unpatch_memory(ENG_DRV_OUT);
    if (ENG_DRV_OUTS) unpatch_memory(ENG_DRV_OUTS);
    if (PATH_SelectPathDriveMode) unpatch_memory(PATH_SelectPathDriveMode);
    if (HIV_Vertical_Address_hook) unpatch_memory(HIV_Vertical_Address_hook);
    if (Clear_Artifacts_ON)
        {
            unpatch_memory(ClearAddress);
            Clear_Artifacts_ON = 0;
        }
    if (Center_Preview_ON)
        {
            unpatch_memory(ShiftAddress);
            Center_Preview_ON = 0 ;
        }
}

static void update_patch()
{
    if (CROP_PRESET_MENU)
    {
        /* update preset */
        crop_preset = CROP_PRESET_MENU;
        
        crop_preset_ar      = crop_preset_ar_menu;
        crop_preset_fps     = crop_preset_fps_menu;
        crop_preset_1x1_res = crop_preset_1x1_res_menu;
        crop_preset_1x3_res = crop_preset_1x3_res_menu;
        crop_preset_3x3_res = crop_preset_3x3_res_menu;

        /* install our hooks, if we haven't already do so */
        if (!patch_active)
        {
            install_patches();
            patch_active = 1;
        }
    }
    
    /* assuming we will take a normal picture in LiveView while crop_rec is active
     * clear artifacts patch will be overwritten by Canon back to default value in this case
     * which will give us patch error (in memory patches), this might help with busy screen too
     * when taking a picture and crop mood is active, let's unpatch all patches outside lv 
     * PROP_LV_ACTION then PROP_LV_STOP get triggerd after pressing full shutter button (SW2)
     * and before taking a picture process happens (also before clear artifacts get overwritten) */
    if (CROP_PRESET_MENU && !lv)
    {
        if (patch_active)
        {
            uninstall_patches();

            /* turn off Kill Canon GUI setting */
            extern int kill_canon_gui_mode;
            if (kill_canon_gui_mode != 0)
            {
                kill_canon_gui_mode = 0;
            }

            patch_active = 0;
            crop_preset = 0;
        }
    }
    /* unpatch when no crop presets is selected, also outside movie mode */
    if (!CROP_PRESET_MENU || !is_movie_mode())
    {
        /* undo active patches, if any */
        if (patch_active)
        {
            uninstall_patches();

            /* enable Canon overlays (turn off Kill Canon GUI setting) */
            extern int kill_canon_gui_mode;
            if (kill_canon_gui_mode != 0)
            {
                kill_canon_gui_mode = 0;
                if (canon_gui_front_buffer_disabled())
                {
                    canon_gui_enable_front_buffer(0);
                }
            }

            patch_active = 0;
            crop_preset = 0;
        }
    }
}

/* enable patch when switching LiveView (not in the middle of LiveView) */
/* otherwise you will end up with a halfway configured video mode that looks weird */
PROP_HANDLER(PROP_LV_ACTION)
{
    update_patch();
    eosm_lv_guard_request();
}

/* also try when switching zoom modes */
PROP_HANDLER(PROP_LV_DISPSIZE)
{
    update_patch();
#ifdef CONFIG_EOSM
    if (eosm_lv_guard_internal_zoom)
        eosm_lv_guard_internal_zoom = 0;
    else
#endif
        eosm_lv_guard_request();
}

/* forward reference */
static struct menu_entry crop_rec_menu[];

/* give a warning if picture quality not set to RAW for entry-level models */
static int pic_quality_warning = 0;

static MENU_UPDATE_FUNC(crop_update)
{
    if (is_DIGIC_5)
    {
        /* reveal options for the current crop mode (1:1, 1x3 and 3x3) */
        crop_rec_menu[0].children[0].shidden = (crop_preset_index != 1);  // 1 CROP_PRESET_1X1
        crop_rec_menu[0].children[1].shidden = (crop_preset_index != 2);  // 2 CROP_PRESET_1X3
        crop_rec_menu[0].children[2].shidden = (crop_preset_index != 3);  // 3 CROP_PRESET_3X3
        crop_rec_menu[0].children[5].shidden = (CROP_PRESET_MENU == CROP_PRESET_1X3 || CROP_PRESET_MENU == CROP_PRESET_1X1);  // 3 CROP_PRESET_3X3
        

        if (CROP_PRESET_MENU && lv && patch_active)
        {
            if (raw_lv_is_enabled())
            {
                /* print resolution and binning mode in help section, maybe add FPS too? */
                MENU_SET_HELP("%dx%d %d%s%d",raw_info.width - 72, raw_info.height - 28, raw_capture_info.binning_y + raw_capture_info.skipping_y,
                                                                                       (raw_capture_info.binning_x + raw_capture_info.skipping_x == 1 &&
                                                                                        raw_capture_info.binning_y + raw_capture_info.skipping_y == 1) ? 
                            ":" : "x",                                                  raw_capture_info.binning_x + raw_capture_info.skipping_x);

                /* print picture warning if Image quality not set to RAW from Canon menu */
                if (pic_quality_warning)
                {
                    MENU_SET_WARNING(MENU_WARN_ADVICE, "Set Image quality to RAW, restart camera. This extends recording times.");
                }
            }

            /* print selected preset name in crop mode menu */
            if (CROP_PRESET_MENU == CROP_PRESET_1X1)
            {
                MENU_SET_VALUE("%s %s", crop_preset_1x1_res_menu == 0 ? "2.5K"  : crop_preset_1x1_res_menu == 1 ? "2.8K"     :
                                        crop_preset_1x1_res_menu == 2 ? "3K"    : crop_preset_1x1_res_menu == 3 ? "1440p"    :
                                        crop_preset_1x1_res_menu == 4 ? "1280p" : crop_preset_1x1_res_menu == 5 ? "Full-Res" : "", "1:1 crop");
            }
            if (CROP_PRESET_MENU == CROP_PRESET_1X3)
            {
                MENU_SET_VALUE("%s %s", (crop_preset_1x3_res_menu == 0 && crop_preset_ar_menu == 0) ? "4.5K"  : 
                                        (crop_preset_1x3_res_menu == 1 && crop_preset_ar_menu == 0) ? "4.2K"  :
                                        (crop_preset_1x3_res_menu == 2 && crop_preset_ar_menu == 0) ? "UHD"   :
                                        (crop_preset_1x3_res_menu == 0 && crop_preset_ar_menu == 1) ? "4.8K"  :
                                        (crop_preset_1x3_res_menu == 1 && crop_preset_ar_menu == 1) ? "4.4K"  :
                                        (crop_preset_1x3_res_menu == 2 && crop_preset_ar_menu == 1) ? "4K"    :
                                        (crop_preset_1x3_res_menu == 0 && crop_preset_ar_menu == 2) ? "5K"    :
                                        (crop_preset_1x3_res_menu == 1 && crop_preset_ar_menu == 2) ? "4.6K"  :
                                        (crop_preset_1x3_res_menu == 2 && crop_preset_ar_menu == 2) ? "4.2K"  :
                                        (crop_preset_1x3_res_menu == 0 && crop_preset_ar_menu >= 3) ? "5.2K"  :
                                        (crop_preset_1x3_res_menu == 1 && crop_preset_ar_menu >= 3) ? "4.8K"  :
                                        (crop_preset_1x3_res_menu == 2 && crop_preset_ar_menu >= 3) ? "4.4K"  : "",  "1x3");
            }
            if (CROP_PRESET_MENU == CROP_PRESET_3X3)
            {
                MENU_SET_VALUE("%s %s", (crop_preset_3x3_res_menu == 0 && crop_preset_ar_menu == 0) ? "976p"  : 
                                        (crop_preset_3x3_res_menu == 0 && crop_preset_ar_menu == 1) ? "868p"  :
                                        (crop_preset_3x3_res_menu == 0 && crop_preset_ar_menu == 2) ? "790p"  :
                                        (crop_preset_3x3_res_menu == 0 && crop_preset_ar_menu == 3) ? "738p"  :
                                        (crop_preset_3x3_res_menu == 0 && crop_preset_ar_menu == 4 && crop_preset_fps_reduce == 0 && is_EOSM) ? "694p"  :
                                        (crop_preset_3x3_res_menu == 0 && crop_preset_ar_menu == 4 && crop_preset_fps_reduce == 1 && is_EOSM) ? "726p"  :
                                        (crop_preset_3x3_res_menu == 1)                             ? "1080p"  : "",  "3x3");
                
                if (crop_preset_3x3_res_menu == 0) MENU_SET_RINFO("(HFR)");
            }
        }
    }

    /* hide Framerate and Aspect ratio menus for none supported models */
    crop_rec_menu[0].children[3].shidden = !is_DIGIC_5;  // Aspect ratio
    crop_rec_menu[0].children[4].shidden = !is_DIGIC_5;  // Framerate

    if (CROP_PRESET_MENU && lv)
    {
        if (CROP_PRESET_MENU == CROP_PRESET_CENTER_Z || (is_70D && CROP_PRESET_MENU == CROP_PRESET_UHD) || is_DIGIC_5)
        {
            if (lv_dispsize == 1)
            {
                MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "To use this mode, exit ML menu & press the zoom button (set to x5).");
            }
        }
        else /* non-zoom modes */
        {
            if (is_basic || is_70D || is_5D3)
            {
                if (!is_supported_mode())
                {
                    MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "This preset only works in 1080p and 720p video modes.");
                }
                else if (lv_dispsize != 1)
                {
                    MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "To use this mode, exit ML menu and press the zoom button (set to x1).");
                }
                else if (!is_1080p())
                {
                    if (is_70D)
                    {
                        if (CROP_PRESET_MENU == CROP_PRESET_1x3) MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "This preset only works in the 1080p mode from Canon menu.");
                    }
                }
                else if (!is_720p())
                {
                    if (CROP_PRESET_MENU == CROP_PRESET_3x3_1X ||
                        CROP_PRESET_MENU == CROP_PRESET_3x3_1X_48p)
                    {
                        /* these presets only have effect in 720p mode */
                        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "This preset only works in the 720p 50/60 fps modes from Canon menu.");
                        return;
                    }
                }
            }
        }
    }
}

static MENU_UPDATE_FUNC(crop_preset_1x1_res_update)
{
    if (crop_preset_1x1_res_menu == 0)
    {
        MENU_SET_HELP("2520x1080 @ 23.976, 25 and 30 FPS");
    }
    if (crop_preset_1x1_res_menu == 1)
    {
        MENU_SET_HELP("2880x1206(2.39:1) @ 23.976 and 25 FPS");
    }
    if (crop_preset_1x1_res_menu == 2)
    {
        MENU_SET_HELP("3072x1308 @ 23.976 FPS. Real-Time preview isn't perfect.");
    }
    if (crop_preset_1x1_res_menu == 3)
    {
        MENU_SET_HELP("2560x1440(16:9) @ 23.976 and 25 FPS");
    }
    if (crop_preset_1x1_res_menu == 4)
    {
        MENU_SET_HELP("1920x1280 @ 23.976 and 25 FPS");
    }
    if (crop_preset_1x1_res_menu == 5)
    {
        MENU_SET_HELP("5208x3478 @ 2 FPS. Has cropped centered real-time preview.");
    }
    if (crop_preset_1x1_res_menu == 6)
    {
        MENU_SET_HELP("2160x1620 @ 23.943 FPS");
    }
}

static MENU_UPDATE_FUNC(crop_preset_1x3_res_update)
{
    if (crop_preset_ar_menu == 0) // AR_16_9
    {
        if (crop_preset_1x3_res_menu == 0) // Anam_Highest
        {
            MENU_SET_VALUE("4.5K");
            if (is_650D || is_700D)MENU_SET_HELP("1504x2538 @ 23.976 FPS");
            if (is_EOSM || is_100D)MENU_SET_HELP("1504x2538 @ 22.250 FPS");
        }

        if (crop_preset_1x3_res_menu == 1) // Anam_Higher
        {
            MENU_SET_VALUE("4.2K");
            if (is_650D || is_700D)MENU_SET_HELP("1392x2350 @ 23.976 and 25 FPS");
            if (is_EOSM || is_100D)MENU_SET_HELP("1376x2322 @ 23.976 FPS");
        }

        if (crop_preset_1x3_res_menu == 2) // Anam_Medium
        {
            MENU_SET_VALUE("UHD");
            MENU_SET_HELP("1280x2160 @ 23.976 and 25 FPS");
        } 
        if (crop_preset_1x3_res_menu == 3) // Anam_Medium
        {
            MENU_SET_VALUE("Full-Res LV");
            MENU_SET_HELP("1736x3476 @ 12, 14 and 16 FPS");
        }
    }

    if (crop_preset_ar_menu == 1) // AR_2_1
    {
        if (crop_preset_1x3_res_menu == 0) // Anam_Highest
        {
            MENU_SET_VALUE("4.8K");
            if (is_650D || is_700D)MENU_SET_HELP("1600x2400 @ 23.976 FPS");
            if (is_EOSM || is_100D)MENU_SET_HELP("1600x2400 @ 23.300 FPS");
        }

        if (crop_preset_1x3_res_menu == 1) // Anam_Higher
        {
            MENU_SET_VALUE("4.4K");
            MENU_SET_HELP("1472x2208 @ 23.976 and 25 FPS");
        }

        if (crop_preset_1x3_res_menu == 2) // Anam_Medium
        {
            MENU_SET_VALUE("4K");
            MENU_SET_HELP("1360x2040 @ 23.976 and 25 FPS");
        }
    }

    if (crop_preset_ar_menu == 2) // AR_2_20_1
    {
        if (crop_preset_1x3_res_menu == 0) // Anam_Highest
        {
            MENU_SET_VALUE("5K");
            if (is_650D || is_700D)MENU_SET_HELP("1680x2290 @ 23.976 FPS");
            if (is_EOSM || is_100D)MENU_SET_HELP("1664x2268 @ 23.976 FPS");
        }

        if (crop_preset_1x3_res_menu == 1) // Anam_Higher
        {
            MENU_SET_VALUE("4.6K");
            MENU_SET_HELP("1552x2216 @ 23.976 and 25 FPS");
        }

        if (crop_preset_1x3_res_menu == 2) // Anam_Medium
        {
            MENU_SET_VALUE("4.2K");
            MENU_SET_HELP("1424x1942 @ 23.976 and 25 FPS");
        }
    }

    if (crop_preset_ar_menu == 3)  // AR_2_35_1
    {
        if (crop_preset_1x3_res_menu == 0) // Anam_Highest
        {
            MENU_SET_VALUE("5.2K");
            MENU_SET_HELP("1736x2214 @ 23.976 FPS");
        }

        if (crop_preset_1x3_res_menu == 1) // Anam_Higher
        {
            MENU_SET_VALUE("4.8K");
            MENU_SET_HELP("1600x2040 @ 23.976 and 25 FPS");
        }

        if (crop_preset_1x3_res_menu == 2) // Anam_Medium
        {
            MENU_SET_VALUE("4.4K");
            MENU_SET_HELP("1472x1878 @ 23.976 and 25 FPS");
        }
    }

    if (crop_preset_ar_menu == 4)  // AR_2_39_1
    {
        if (crop_preset_1x3_res_menu == 0) // Anam_Highest
        {
            MENU_SET_VALUE("5.2K");
            MENU_SET_HELP("1736x2178 @ 23.976 FPS");
        }

        if (crop_preset_1x3_res_menu == 1) // Anam_Higher
        {
            MENU_SET_VALUE("4.8K");
            MENU_SET_HELP("1600x2008 @ 23.976 and 25 FPS");
        }

        if (crop_preset_1x3_res_menu == 2) // Anam_Medium
        {
            MENU_SET_VALUE("4.4K");
            MENU_SET_HELP("1472x1846 @ 23.976 and 25 FPS");
        }
    }
}

static MENU_UPDATE_FUNC(crop_preset_3x3_res_update)
{
    if (crop_preset_3x3_res_menu == 0) // High FPS
    {
        if (crop_preset_ar_menu == 0) MENU_SET_VALUE("976p (HFR)"); // AR_16_9
        if (crop_preset_ar_menu == 1) MENU_SET_VALUE("868p (HFR)"); // AR_2_1
        if (crop_preset_ar_menu == 2) MENU_SET_VALUE("790p (HFR)"); // AR_2_20_1
        if (crop_preset_ar_menu == 3) MENU_SET_VALUE("738p (HFR)"); // AR_2_35_1
        if (crop_preset_ar_menu == 4 && crop_preset_fps_reduce == 0 && is_EOSM) // AR_2_39_1  // actually 2.50:1 aspect ratio */
        {
            MENU_SET_VALUE("694p (HFR)"); 
            MENU_SET_WARNING(MENU_WARN_ADVICE, "Real-Time preview doesn't work in 694p (HFR), it's always black.");
        }        
        if (crop_preset_ar_menu == 4 && crop_preset_fps_reduce == 1 && is_EOSM) // AR_2_39_1  // actually 2.50:1 aspect ratio */
        {
            MENU_SET_VALUE("726p (HFR)");
        }
    }
}

static MENU_UPDATE_FUNC(crop_preset_ar_update)
{
    if (CROP_PRESET_MENU == CROP_PRESET_3X3)
    {
        if (crop_preset_3x3_res_menu == 0)  // High_FPS
        {
            if (crop_preset_ar_menu == 4 && crop_preset_fps_reduce == 1 && is_EOSM) MENU_SET_VALUE("2.39:1");
            if (crop_preset_ar_menu == 4 && crop_preset_fps_reduce == 0) MENU_SET_VALUE("2.50:1"); // AR_2_39_1 // we are using AR_2_39_1 as 2.50:1 in this case
        }
        if (crop_preset_3x3_res_menu == 2)  // mv1080
        {
            MENU_SET_VALUE("3:2");
            MENU_SET_WARNING(MENU_WARN_ADVICE, "This option doesn't work in current preset.");
        }
    }

    if (CROP_PRESET_MENU == CROP_PRESET_1X1)
    {
        if (crop_preset_1x1_res_menu == 0) MENU_SET_VALUE("2.33:1");  // CROP_2_5K
        if (crop_preset_1x1_res_menu == 1) MENU_SET_VALUE("2.39:1");  // CROP_2_8K
        if (crop_preset_1x1_res_menu == 2) MENU_SET_VALUE("2.35:1");  // CROP_3K 3072x1308
        if (crop_preset_1x1_res_menu == 3) MENU_SET_VALUE("16:9");    // CROP_1440p
        if (crop_preset_1x1_res_menu == 4) MENU_SET_VALUE("3:2");     // CROP_1280p
        if (crop_preset_1x1_res_menu == 5) MENU_SET_VALUE("3:2");     // CROP_Full_Res
        if (crop_preset_1x1_res_menu == 6) MENU_SET_VALUE("4:3");     // CROP_1620p
        if (crop_preset_1x1_res_menu == 7) MENU_SET_VALUE("16:9");    // CROP_1080p
        MENU_SET_WARNING(MENU_WARN_ADVICE, "This option doesn't work in 1:1 crop.");
    }
}

static MENU_UPDATE_FUNC(crop_preset_fps_update)
{
    if (CROP_PRESET_MENU == CROP_PRESET_1X3)
    {
        if (crop_preset_ar_menu == 0) // AR_16_9
        {
            if (crop_preset_1x3_res_menu == 0) // Anam_Highest
            {
                if (is_650D || is_700D) MENU_SET_VALUE("23.976 FPS");
                if (is_EOSM || is_100D) MENU_SET_VALUE("22.250 FPS");
                if (crop_preset_fps_menu != 0)
                {
                    MENU_SET_WARNING(MENU_WARN_ADVICE, "25 and 30 FPS don't work in current preset.");
                }
            }

            if (crop_preset_1x3_res_menu == 1) // Anam_Higher
            {
                if (is_650D || is_700D)
                {
                    if (crop_preset_fps_menu == 2)
                    {
                        MENU_SET_VALUE("25 FPS");
                        MENU_SET_WARNING(MENU_WARN_ADVICE, "30 FPS doesn't work in current preset.");
                    }
                }

                if (is_100D || is_EOSM)
                {
                    if (crop_preset_fps_menu > 0)
                    {
                        MENU_SET_VALUE("23.976 FPS");
                        MENU_SET_WARNING(MENU_WARN_ADVICE, "25 and 30 FPS don't work in current preset.");
                    }
                }
            }

            if (crop_preset_1x3_res_menu == 2) // Anam_Medium
            {
                if (crop_preset_fps_menu == 2)
                {
                    MENU_SET_VALUE("25 FPS");
                    MENU_SET_WARNING(MENU_WARN_ADVICE, "30 FPS doesn't work in current preset.");
                }
            }
        }

        if (crop_preset_ar_menu == 1) // AR_2_1
        {
            if (crop_preset_1x3_res_menu == 0) // Anam_Highest
            {
                if (is_650D || is_700D) MENU_SET_VALUE("23.976 FPS");
                if (is_EOSM || is_100D) MENU_SET_VALUE("23.300 FPS");
                if (crop_preset_fps_menu != 0)
                {
                    MENU_SET_WARNING(MENU_WARN_ADVICE, "25 and 30 FPS don't work in current preset.");
                }
            }

            if (crop_preset_1x3_res_menu > 0) // Anam_Higher, Anam_Medium
            {
                if (crop_preset_fps_menu == 2)
                {
                    MENU_SET_VALUE("25 FPS");
                    MENU_SET_WARNING(MENU_WARN_ADVICE, "30 FPS doesn't work in current preset.");
                }
            }
        }

        if (crop_preset_ar_menu > 1) // AR_2_20_1, AR_2_35_1, AR_2_39_1
        {
            if (crop_preset_1x3_res_menu == 0) // Anam_Highest
            {
                MENU_SET_VALUE("23.976 FPS");
                if (crop_preset_fps_menu != 0)
                {
                    MENU_SET_WARNING(MENU_WARN_ADVICE, "25 and 30 FPS don't work in current preset.");
                }
            }

            if (crop_preset_1x3_res_menu > 0) // Anam_Higher, Anam_Medium
            {
                if (crop_preset_fps_menu == 2)
                {
                    MENU_SET_VALUE("25 FPS");
                    MENU_SET_WARNING(MENU_WARN_ADVICE, "30 FPS doesn't work in current preset.");
                }
            }
        }
                    
        if (crop_preset_1x3_res_menu == 3) // High FPS
        {
            int current_fps = fps_get_current_x1000();

            if (is_EOSM)
            {
                MENU_SET_VALUE("%d.%03d",current_fps/1000, current_fps%1000);
            }
        }
    }

    if (CROP_PRESET_MENU == CROP_PRESET_1X1)
    {
        if ((crop_preset_1x1_res_menu == 1 || crop_preset_1x1_res_menu == 3) && crop_preset_fps_menu == 2) // CROP_2_8K, CROP_1440p and 30 FPS
        {
            MENU_SET_VALUE("25 FPS");
            MENU_SET_WARNING(MENU_WARN_ADVICE, "30 FPS doesn't work in current preset.");
        }

        if (crop_preset_1x1_res_menu == 2 && crop_preset_fps_menu > 0) // CROP_3K and 25, 30 FPS
        {
            MENU_SET_VALUE("23.976 FPS");
            MENU_SET_WARNING(MENU_WARN_ADVICE, "25 and 30 FPS don't work in current preset.");
        }

        if (crop_preset_1x1_res_menu == 5) // CROP_Full_Res
        {
            MENU_SET_VALUE("2 FPS");
            MENU_SET_WARNING(MENU_WARN_ADVICE, "This option doesn't work with Full-Res LV.");
        }
    }

    if (CROP_PRESET_MENU == CROP_PRESET_3X3)
    {
        
        int current_fps = fps_get_current_x1000();
                    
        if (crop_preset_3x3_res_menu == 0) // High FPS
        {
            if (is_650D || is_700D || is_EOSM)
            {
                if (crop_preset_ar_menu == 0) MENU_SET_VALUE("%d.%03d",current_fps/1000, current_fps%1000); // AR_16_9
                if (crop_preset_ar_menu == 1) MENU_SET_VALUE("%d.%03d",current_fps/1000, current_fps%1000);     // AR_2_1
                if (crop_preset_ar_menu == 2) MENU_SET_VALUE("%d.%03d",current_fps/1000, current_fps%1000);     // AR_2_20_1
                if (crop_preset_ar_menu == 3) MENU_SET_VALUE("%d.%03d",current_fps/1000, current_fps%1000);     // AR_2_35_1
                if (crop_preset_ar_menu == 4) MENU_SET_VALUE("%d.%03d",current_fps/1000, current_fps%1000);     // AR_2_39_1  // actually 2.50:1 aspect ratio
            }

            if (is_100D)
            {
                if (crop_preset_ar_menu == 0) MENU_SET_VALUE("46.300 FPS"); // AR_16_9
                if (crop_preset_ar_menu == 1) MENU_SET_VALUE("50 FPS");     // AR_2_1
                if (crop_preset_ar_menu == 2) MENU_SET_VALUE("54 FPS");     // AR_2_20_1
                if (crop_preset_ar_menu == 3) MENU_SET_VALUE("55.6 FPS");   // AR_2_35_1
                if (crop_preset_ar_menu == 4) MENU_SET_VALUE("58 FPS");     // AR_2_39_1  // actually 2.50:1 aspect ratio
            }

            MENU_SET_WARNING(MENU_WARN_ADVICE, "This option doesn't work in (HFR) preset.");
        }
    }
}

static MENU_UPDATE_FUNC(target_yres_update)
{
    MENU_SET_RINFO("from %d", max_resolutions[crop_preset][get_video_mode_index()]);
}

static MENU_UPDATE_FUNC(bit_depth_analog_update)
{
    if (which_output_format() < 3)
    {
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "To use this option, change Data format to lossless in RAW video.");
    }
}

static MENU_UPDATE_FUNC(fix_dual_iso_flicker_update)
{
    if (!dual_iso_is_enabled())
    {
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "This option only works with Dual-ISO.");
    }
}

/* ---- EOS M slim Crop Mode (single screen) ----
 * Mode / Aspect / Preset drive recording config; Resolution is read-only;
 * Frame Rate cycles only valid rates; Bit Depth stays 14/12/10 always.
 * Module builds lack CONFIG_SLIM_MENUS — gate with is_EOSM. */

static struct menu_entry slim_more_hacks_menu[] = {
    {
        .name     = "More Hacks",
        .max      = 1,
        .choices  = CHOICES("OFF", "Allow"),
        .priv     = &more_hacks,
        .edit_mode = EM_INLINE_ADJUST,
        .icon_type = IT_DICE,
        .help     = "Allow More hacks even when other settings would block them.",
    },
};

/* Expo → Shutter range (EOS M slim; dial L/R like Mode / Aspect). */
static struct menu_entry expo_shutter_range_eosm[] = {
    {
        .name       = "Shutter range",
        .priv       = &shutter_range,
        .min        = 0,
        .max        = 1,
        .choices    = CHOICES("Original", "Full range"),
        .edit_mode  = EM_INLINE_ADJUST,
        .help       = "Choose the available shutter speed range:",
        .help2      = "Original: default range used by Canon in selected video mode.\n"
                      "Full range: from 1/FPS to minimum exposure time allowed by hardware."
    },
};

static MENU_UPDATE_FUNC(slim_info_button_update)
{
    static int last_info_button = -1;
    if (last_info_button == 6 && INFO_button != 6)
        mlv_lite_info_framing_reset();
    last_info_button = INFO_button;
}

/* Settings → INFO / Up-Down / Shutter zoom (EOS M slim). */
static struct menu_entry slim_info_button_menu[] = {
    {
        .name      = "INFO Button",
        .priv      = &INFO_button,
        .max       = 7,
        .choices   = CHOICES("OFF", "Dual ISO", "Histogram", "Waveform", "Zebras", "False Color", "framing", "Quick Panel"),
        .edit_mode = EM_INLINE_ADJUST,
        .update    = slim_info_button_update,
        .icon_type = IT_DICE,
        .help      = "Assign INFO to an overlay, framing, or the Quick Panel.",
        .help2     = "OFF uses Canon INFO. Idle LV: long-press INFO (or double-press) opens last setting.",
    },
    {
        .name      = "SET Button",
        .priv      = &SET_button,
        .min       = 1,
        .max       = 2,
        .choices   = CHOICES("x10 zoom", "Last settings"),
        .edit_mode = EM_INLINE_ADJUST,
        .icon_type = IT_DICE,
        .help      = "Choose what SET does on the movie LiveView screen.",
        .help2     = "Last settings opens the last changed ML setting, like a LiveView screen tap.",
    },
    {
        .name      = "Up/Down Button",
        .priv      = &Arrows_U_D,
        .max       = 3,
        .choices   = CHOICES("OFF", "Shutter", "Aperture", "ISO"),
        .edit_mode = EM_INLINE_ADJUST,
        .icon_type = IT_DICE,
        .help      = "What UP/DOWN adjust on the movie LiveView screen (not while recording).",
        .help2     = "Shutter: faster/slower. Aperture: open/close. ISO: up/down.",
    },
    {
        .name      = "Shutter zoom",
        .priv      = &Shutter_zoom,
        .max       = 3,
        .choices   = CHOICES("OFF", "ON", "Sticky", "AF"),
        .edit_mode = EM_INLINE_ADJUST,
        .icon_type = IT_DICE,
        .help      = "Half-shutter x10 zoom (same as SET). ON: hold to zoom; Sticky: tap to toggle.",
        .help2     = "AF: zoom to x10, autofocus once, return (AF lens; acts like ON with manual lens).\n"
                     "Only in movie LV with ML overlays. Not active while recording.",
    },
    {
        .name      = "Auto refocus",
        .priv      = &Auto_refocus,
        .max       = 2,
        .choices   = CHOICES("OFF", "ON", "ON + REC"),
        .edit_mode = EM_INLINE_ADJUST,
        .icon_type = IT_DICE,
        .help      = "Refocus with Canon AF when the picture gets blurry and the scene has settled.",
        .help2     = "AF lens only; off with Movie Servo AF. ON + REC: also while recording - during RAW\n"
                     "recording Canon AF needs Small Hacks OFF and may hunt.",
    },
    {
        .name      = "Refocus sensitivity",
        .priv      = &Auto_refocus_sens,
        .max       = 2,
        .choices   = CHOICES("Low", "Medium", "High"),
        .edit_mode = EM_INLINE_ADJUST,
        .icon_type = IT_DICE,
        .help      = "How readily Auto refocus reacts to blur.",
        .help2     = "Low: only clear blur, slower. Medium: default. High: smaller sharpness drops and shorter\n"
                     "waits - may also refocus on pans or scene changes.",
    },
};

/* Mode UI: 0=1x1, 1=1x3, 2=3x3, 3=LV (Full-Res LiveView). */
static int slim_mode_ui = 0;
static int slim_unified_preset = 1; /* Highest=0 Higher=1 Medium=2 */
static int slim_bit_depth_ui = 1;   /* 0=10 1=12 2=14 → bit_depth_analog 3/1/0 */
/* Crop register changes are applied asynchronously at frame boundaries.
 * Do not let direct-touch input start another transition while the previous
 * preview geometry is still settling. */
static int slim_touch_crop_ready_at = 0;
#define SLIM_TOUCH_CROP_SETTLE_MS 900

/* EOS M slim: TEMPORARY AF diagnostics (Settings → AF diag log / AF lens test).
 *
 * Goal of one test session: learn which AF-related Canon properties the EOS M
 * emits (and when), and whether ML can drive the lens focus motor while RAW
 * video is recording.  Both switches are plain statics, so they are OFF after
 * every restart, and with both OFF nothing here does any work.
 *
 * Events go into a RAM ring buffer (property handlers must stay tiny and must
 * not touch the card).  The ring is appended to ML/LOGS/AFDIAG.LOG from
 * CBR_SHOOT_TASK only, never while recording or while mlv_lite is busy.
 * The ring has no lock: a rare torn entry from two tasks writing at once is
 * acceptable for a diagnostic. */
#define AFDIAG_LOG_FILE      "ML/LOGS/AFDIAG.LOG"
#define AFDIAG_LOG_DIR       "ML/LOGS"
#define AFDIAG_RING          1024
#define AFDIAG_MAX_FILE      (1024 * 1024)
#define AFDIAG_FLUSH_MS      2000
#define AFDIAG_DATA_MIN_MS   100   /* PROP_LV_FOCUS_DATA: normal spacing */
#define AFDIAG_DATA_FAST_MS  20    /* ... when a value jumped by a lot */
#define AFDIAG_DATA_JUMP     256
#define AFDIAG_LENS_PERIOD   2000
#define AFDIAG_SHARP_MS      100   /* @SHARP spacing */
#define AFDIAG_STEP_SETTLE   300   /* ms between lens_focus and the 'after' reading */

static int afdiag_log = 0;        /* not CONFIG_INT: OFF after restart */
static int afdiag_lens_test = 0;  /* not CONFIG_INT: OFF after restart */

/* internal event codes (property ids are all >= 0x80000000) */
enum
{
    AFD_EV_AFSTATE = 1,  /* w0 = old slim_af_state, w1 = new */
    AFD_EV_FOCUS,        /* w0/w1 = focus_pos old/new, w2/w3 = focus_dist old/new */
    AFD_EV_REC,          /* w0 = RECORDING, w1 = RECORDING_RAW */
    AFD_EV_LENS_STEP,    /* w0 = signed burst count (size-3 commands), w1 = lens_focus ret, w2 = pos before, w3 = elapsed ms */
    AFD_EV_LENS_SKIP,    /* w0 = reason: 1 manual focus, 2 continuous AF, 3 slim AF busy, 4 not LV movie */
    AFD_EV_SHARP,        /* w0 = edge sum, w1 = mean luma, w2 = samples, w3 = max edge
                          * (centre 1/4 x 1/4 of the displayed LV buffer; Laplacian on luma) */
    AFD_EV_STEP_SHARP,   /* w0 = signed burst count, w1 = edge sum before the step, w2 = edge sum 300 ms after,
                          * w3 = mean luma after */
    AFD_EV_SWEEP,        /* w0 = phase*100 + size, w1 = commands, w2 = elapsed ms, w3 = end reason */
    AFD_EV_LVLENS,       /* PROP_LV_LENS bytes 0x20..0x2F as four big-endian words (logged on change).
                          * The 2.0.2 firmware itself reads 0x24, 0x27 ("Po") and 0x2C (focal length). */
    AFD_EV_FPC,          /* w0 = focus pulse count (signed BE16 from PROP 0x8005001E), w1 = next BE16,
                          * w2 = bytes 4/5, w3 = byte 6 */
    AFD_EV_MPU,          /* ICU -> MPU message (hook on Canon's packet builder): w0 = grp << 16 | op << 8 | len,
                          * w1..w3 = payload bytes 0-11 (byte 0 in the low byte), dropped repeats in len's top */
    AFD_EV_LVOBJ,        /* Canon LV AF state (RAM *(0x4069C+12)), logged on change: w0 = obj,
                          * w1 = +0x184 LV mode, w2 = +0x710 AF method/zoom tag, w3 = +0xC7C servo << 8 | +0xC88 */
    AFD_EV_MPUTEST,      /* AF lens test "MPU", two records per step.
                          * start: w0 = step << 8 | MF << 16 | movie << 17, w1 = property id (0 = none),
                          *        w2/w3 = payload bytes 0-3 / 4-7 (byte 0 in the low byte)
                          * end:   w0 = fpc delta (int8) << 24 | flags << 16 | step << 8 | 1,
                          *        w1 = sharpness before, w2 = sharpness after, w3 = dist before << 16 | after
                          * flags: 1 sent, 2 FOCUS_DONE seen, 4 FOCUS_DONE with error bit, 8 focus_dist moved
                          *        during the step, 0x10 pulse count changed, 0x40 aborted
                          * end of run: w0 = 0xFF01, w1 = stop reason (0 none, 1 LV lost, 2 recording,
                          * 3 menu, 4 switched off), w2 = steps done, w3 = steps planned */
    AFD_EV_MPUSRCH,      /* MPU AF search with sharpness stop: w0 = stopped, w1 = peak, w2 = elapsed ms,
                          * w3 = lens_info.focus_dist at stop */
    AFD_EV_REFOCUS,      /* auto refocus: w0 = what (1 trigger, 2 AF ok, 3 AF failed, 4 request not started,
                          * 5 paused, 6 resumed, 7 reference taken, 8 AF frame, 9 AF ok but still blurry,
                          * 10 periodic decision record, 11 ineligible gates, 12 new scene - peak forgotten),
                          * w1 = current sharpness,
                          * w2 = S_ref, w3 = lv_dispsize | RECORDING << 8 | consecutive fails << 16 */
};

/* Found in the EOS M 2.0.2 ROM (handler 0xFF27AC18): the MPU reports the
 * real focus motor position here, big-endian int16 in bytes 0..1.  ML's
 * lens_info.focus_pos is not a position on this camera. */
#define PROP_LV_FOCUS_PULSE_COUNT 0x8005001E
static volatile int afdiag_fpc = INT_MIN;          /* last pulse count, INT_MIN = never seen */
static volatile int afdiag_focus_done_count = 0;   /* PROP_LV_FOCUS_DONE events */

struct afdiag_entry
{
    int      ms;
    uint32_t code;
    int      len;
    uint32_t w[4];
    int16_t  zoom;
    int8_t   rraw;
    int8_t   hs;
    int      af;
    int      pos;
    int      dist;
};

static struct afdiag_entry afdiag_ring[AFDIAG_RING];
static volatile unsigned afdiag_head = 0;   /* total entries ever recorded */
static unsigned afdiag_tail = 0;            /* total entries already flushed */
static char afdiag_header[320];
static volatile int afdiag_header_pending = 0;

/* Centre-of-frame sharpness from the displayed UYVY LV buffer: Laplacian on
 * luma over the centre 1/4 x 1/4, every 2nd row and 2nd pixel.  Read-only and
 * cheap (a few thousand samples).  Returns the edge sum; 0 and samples=0 if
 * the buffer is unavailable. */
static uint32_t afdiag_sharpness_ex(int *luma_mean, int *samples, int *emax, uint32_t *gsum_out)
{
    uint32_t esum = 0, lsum = 0, emx = 0, gsum = 0;
    int n = 0;
    *luma_mean = 0; *samples = 0; *emax = 0;
    if (gsum_out) *gsum_out = 0;

    /* YUV422_LV_BUFFER_DISPLAY_ADDR is not visible to modules: use the core
     * accessor, which returns the same displayed LV buffer. */
    struct vram_info *vi = get_yuv422_vram();
    if (!vi || !vi->vram) return 0;
    int width = vi->width;
    int height = vi->height;
    int pitch = vi->pitch;
    if (width < 64 || height < 64 || pitch < width * 2)
        return 0;

    const uint8_t *vram = (const uint8_t *)UNCACHEABLE(vi->vram);
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
            if ((uint32_t)e > emx) emx = e;
            /* first-derivative magnitude: scene-contrast normaliser for the
             * Laplacian (blur lowers e much more than g) */
            int gx = c[2] - c[-2]; if (gx < 0) gx = -gx;
            int gy = c[pitch] - c[-pitch]; if (gy < 0) gy = -gy;
            gsum += gx + gy;
            n++;
        }
    }
    if (n)
    {
        *luma_mean = lsum / n;
        *samples = n;
        *emax = emx;
        if (gsum_out) *gsum_out = gsum;
    }
    return esum;
}

static uint32_t afdiag_sharpness(int *luma_mean, int *samples, int *emax)
{
    return afdiag_sharpness_ex(luma_mean, samples, emax, 0);
}

static void afdiag_record(uint32_t code, int len, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    if (!afdiag_log) return;
    struct afdiag_entry *e = &afdiag_ring[afdiag_head % AFDIAG_RING];
    e->ms = get_ms_clock();
    e->code = code;
    e->len = len;
    e->w[0] = a; e->w[1] = b; e->w[2] = c; e->w[3] = d;
    e->zoom = lv_dispsize;
    e->rraw = RECORDING_RAW ? 1 : 0;
    e->hs = get_halfshutter_pressed() ? 1 : 0;
    e->af = slim_af_state;
    e->pos = lens_info.focus_pos;
    e->dist = lens_info.focus_dist;
    afdiag_head++;
}

static void afdiag_prop(unsigned property, uint32_t *buf, unsigned len)
{
    if (!afdiag_log) return;
    /* The lens test sends hundreds of drive commands per cycle and logs a
     * summary per burst; one line per command and acknowledgement flooded
     * the ring and the file.  Keep them only while the test is off. */
    if (afdiag_lens_test && afdiag_lens_test < 3 &&
        (property == PROP_LV_LENS_DRIVE_REMOTE || property == PROP_LV_FOCUS_DONE))
        return;
    uint32_t w[4] = {0, 0, 0, 0};
    unsigned n = buf ? MIN(len / 4, 4) : 0;
    for (unsigned i = 0; i < n; i++) w[i] = buf[i];

    if (property == PROP_LV_FOCUS_DATA)
    {
        static int last_ms = 0;
        static uint32_t last_w[4];
        int now = get_ms_clock();
        int jump = 0;
        for (unsigned i = 0; i < 4; i++)
            if (ABS((int)w[i] - (int)last_w[i]) > AFDIAG_DATA_JUMP) jump = 1;
        if (now - last_ms < (jump ? AFDIAG_DATA_FAST_MS : AFDIAG_DATA_MIN_MS))
            return;
        last_ms = now;
        for (unsigned i = 0; i < 4; i++) last_w[i] = w[i];
    }
    afdiag_record(property, len, w[0], w[1], w[2], w[3]);
}

static volatile int afdiag_focus_done_err = 0;
static volatile uint32_t afdiag_focus_done_val = 0;   /* first word of the last FOCUS_DONE, for AF drive */
PROP_HANDLER(PROP_LV_FOCUS_DONE)
{
    afdiag_focus_done_err = (buf && len >= 4 && (buf[0] & 0x1000)) ? 1 : 0;
    afdiag_focus_done_val = (buf && len >= 4) ? (uint32_t) buf[0] : 0;
    afdiag_focus_done_count++;
    afdiag_prop(property, buf, len);
}
/* Raw lens status, to find a finer position field than focus_dist (which
 * only changes in coarse distance-scale steps and never reported the
 * small moves made by the lens test). */
static uint8_t afdiag_lvlens_raw[128];
static volatile int afdiag_lvlens_len = 0;
PROP_HANDLER(PROP_LV_LENS)
{
    static uint32_t last[4];
    const uint8_t *b = (const uint8_t *)buf;
    if (len > sizeof(afdiag_lvlens_raw)) return;
    memcpy(afdiag_lvlens_raw, b, len);
    afdiag_lvlens_len = len;
    if (len < 0x30) return;
    uint32_t w[4];
    for (int i = 0; i < 4; i++)
        w[i] = (b[0x20 + 4*i] << 24) | (b[0x21 + 4*i] << 16) | (b[0x22 + 4*i] << 8) | b[0x23 + 4*i];
    if (w[0] == last[0] && w[1] == last[1] && w[2] == last[2] && w[3] == last[3]) return;
    for (int i = 0; i < 4; i++) last[i] = w[i];
    afdiag_record(AFD_EV_LVLENS, len, w[0], w[1], w[2], w[3]);
}

/* Ask the MPU for a fresh PROP_LV_LENS report, the way ML core's
 * _prop_lv_lens_request_update() does (not exported to modules): resend the
 * last received payload with its own length; the MPU replies with the
 * current lens status. */
static void __attribute__((unused)) afdiag_request_lens_update(void)
{
    if (afdiag_lvlens_len > 0)
        prop_request_change(PROP_LV_LENS, afdiag_lvlens_raw, afdiag_lvlens_len);
}

PROP_HANDLER(PROP_LV_FOCUS_PULSE_COUNT)
{
    const uint8_t *b = (const uint8_t *)buf;
    if (len < 2) return;
    afdiag_fpc = (int16_t)((b[0] << 8) | b[1]);
    afdiag_record(AFD_EV_FPC, len, afdiag_fpc,
        len >= 4 ? (uint32_t)((b[2] << 8) | b[3]) : 0,
        len >= 6 ? (uint32_t)((b[4] << 8) | b[5]) : 0,
        len >= 7 ? b[6] : 0);
}
PROP_HANDLER(PROP_LV_AF_RESULT)
{
    slim_af_last_result = (buf && len >= 1 && (((uint8_t *)buf)[0] & 0xFF)) ? 1 : 0;
    slim_af_result_count++;
    afdiag_prop(property, buf, len);
}
PROP_HANDLER(PROP_LV_FOCUS_STATE)      { afdiag_prop(property, buf, len); }
PROP_HANDLER(PROP_LV_FOCUS_STATUS)     { afdiag_prop(property, buf, len); }
PROP_HANDLER(PROP_LV_FOCUS_DATA)       { afdiag_prop(property, buf, len); }
/* Last value Canon set for FOCUS_CMD, raw: the MPU test's step B changes it
 * and restores exactly this afterwards. */
static uint8_t afdiag_focus_cmd_raw[16];
static volatile int afdiag_focus_cmd_len = 0;   /* 0 = never seen */
PROP_HANDLER(PROP_LV_FOCUS_CMD)
{
    if (buf && len >= 1 && len <= sizeof(afdiag_focus_cmd_raw))
    {
        memcpy(afdiag_focus_cmd_raw, buf, len);
        afdiag_focus_cmd_len = len;
    }
    afdiag_prop(property, buf, len);
}
PROP_HANDLER(PROP_LVCAF_STATE)         { afdiag_prop(property, buf, len); }
PROP_HANDLER(PROP_LVAF_001D)           { afdiag_prop(property, buf, len); }
PROP_HANDLER(PROP_LV_LENS_DRIVE_REMOTE){ afdiag_prop(property, buf, len); }
PROP_HANDLER(PROP_HALF_SHUTTER)        { afdiag_prop(property, buf, len); }
PROP_HANDLER(PROP_MOVIE_SERVO_AF)      { afdiag_prop(property, buf, len); }
PROP_HANDLER(PROP_LIVE_VIEW_AF_SYSTEM) { afdiag_prop(property, buf, len); }

static const char *afdiag_prop_name(uint32_t code)
{
    switch (code)
    {
        case PROP_LV_FOCUS_DONE:        return "LV_FOCUS_DONE";
        case PROP_LV_AF_RESULT:         return "LV_AF_RESULT";
        case PROP_LV_FOCUS_STATE:       return "LV_FOCUS_STATE";
        case PROP_LV_FOCUS_STATUS:      return "LV_FOCUS_STATUS";
        case PROP_LV_FOCUS_DATA:        return "LV_FOCUS_DATA";
        case PROP_LV_FOCUS_CMD:         return "LV_FOCUS_CMD";
        case PROP_LVCAF_STATE:          return "LVCAF_STATE";
        case PROP_LVAF_001D:            return "LVAF_001D";
        case PROP_LV_LENS_DRIVE_REMOTE: return "LV_LENS_DRIVE_REMOTE";
        case PROP_HALF_SHUTTER:         return "HALF_SHUTTER";
        case PROP_MOVIE_SERVO_AF:       return "MOVIE_SERVO_AF";
        case PROP_LIVE_VIEW_AF_SYSTEM:  return "LIVE_VIEW_AF_SYSTEM";
        case AFD_EV_AFSTATE:            return "@AF_STATE";
        case AFD_EV_FOCUS:              return "@FOCUS_CHG";
        case AFD_EV_REC:                return "@REC";
        case AFD_EV_LENS_STEP:          return "@LENS_STEP";
        case AFD_EV_LENS_SKIP:          return "@LENS_SKIP";
        case AFD_EV_SHARP:              return "@SHARP";
        case AFD_EV_STEP_SHARP:         return "@STEP_SHARP";
        case AFD_EV_FPC:                return "@FPC";
        case AFD_EV_LVLENS:             return "@LVLENS";
        case AFD_EV_SWEEP:              return "@SWEEP";
        case AFD_EV_REFOCUS:            return "@REFOCUS";
        case AFD_EV_LVOBJ:              return "@LVOBJ";
        case AFD_EV_MPU:                return "@MPU";
        case AFD_EV_MPUTEST:            return "@MPUTEST";
    }
    return "?";
}

static void afdiag_make_header(void)
{
    snprintf(afdiag_header, sizeof(afdiag_header),
        "# AFDIAG ON t=%d lens=\"%s\" lv_af_mode=%d cont_af_movie=%d manual_focus=%d "
        "crop_preset=%d slim_mode=%d small_hacks=%d zoom=x%d\n",
        get_ms_clock(), lens_info.name, lv_af_mode, continuous_af_movie,
        is_manual_focus() ? 1 : 0, crop_preset_index, slim_mode_ui,
        get_config_var("raw.small.hacks"), lv_dispsize);
    afdiag_header_pending = 1;
}

/* Poll samples (called from the top of crop_rec_polling_cbr). */
/* AF diag "ON + MPU": log every message the ICU sends to the MPU, which runs
 * the lens and AF on this camera.  EOS M 2.0.2 ROM 0xFFA6CE54, copied to RAM
 * at 0x38C4 (stubs RAM_OFFSET): (grp, op, len, data) -> [len+3, grp, op,
 * payload].  PROP_LV_FOCUS ends up here as (9, 1, 5).  Read-only: the hook
 * only copies the arguments.  Each (grp, op) pair is logged at most every
 * AFDIAG_MPU_MIN_MS, so the per-frame AF feed (9, 0x2C) can't flood the ring. */
#define AFDIAG_MPU_ADDR     0x38C4
#define AFDIAG_MPU_INSTR    0xE92D4030  /* push {r4, r5, lr} */
#define AFDIAG_MPU_MIN_MS   250
static int afdiag_mpu_hooked = 0;
static int afdiag_mpu_last[32 * 64];
static uint8_t afdiag_mpu_drop[32 * 64];

static void afdiag_mpu_cbr(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    (void)stack; (void)pc;
    if (afdiag_log != 2) return;
    uint32_t grp = regs[0], op = regs[1], len = regs[2];
    const uint8_t *data = (const uint8_t *) regs[3];
    int slot = (grp & 31) * 64 + (op & 63);
    uint32_t old = cli();
    int now = get_ms_clock();
    if (afdiag_mpu_last[slot] && now - afdiag_mpu_last[slot] < AFDIAG_MPU_MIN_MS)
    {
        if (afdiag_mpu_drop[slot] < 255) afdiag_mpu_drop[slot]++;
        sei(old);
        return;
    }
    afdiag_mpu_last[slot] = now ? now : 1;
    uint32_t w[3] = { 0, 0, 0 };
    if (data && len <= 100 && (uint32_t) data >= 0x1000)
        for (uint32_t i = 0; i < len && i < 12; i++)
            w[i / 4] |= (uint32_t) data[i] << (8 * (i % 4));
    afdiag_record(AFD_EV_MPU, 16,
                  ((grp & 0xFF) << 16) | ((op & 0xFF) << 8) | (len & 0xFF) | ((uint32_t) afdiag_mpu_drop[slot] << 24),
                  w[0], w[1], w[2]);
    afdiag_mpu_drop[slot] = 0;
    sei(old);
}

/* Install the hook from the shoot task (once), never from the hook itself. */
static void afdiag_mpu_update(void)
{
    int want = (afdiag_log == 2) && is_camera("EOSM", "2.0.2");
    if (want && !afdiag_mpu_hooked)
    {
        if (*(volatile uint32_t *) AFDIAG_MPU_ADDR != AFDIAG_MPU_INSTR)
        {
            NotifyBox(3000, "MPU log: unexpected code at %x", AFDIAG_MPU_ADDR);
            afdiag_log = 1;
            return;
        }
        memset(afdiag_mpu_last, 0, sizeof(afdiag_mpu_last));
        memset(afdiag_mpu_drop, 0, sizeof(afdiag_mpu_drop));
        if (patch_hook_function(AFDIAG_MPU_ADDR, AFDIAG_MPU_INSTR, afdiag_mpu_cbr, "AF diag: MPU log") == 0)
            afdiag_mpu_hooked = 1;
        else
        {
            NotifyBox(3000, "MPU log: hook failed");
            afdiag_log = 1;
        }
    }
    /* Never unpatch while the camera runs: unpatch_memory() wipes the hook
     * stub at once, and a task preempted inside it would then crash.  Once
     * installed the hook stays; afdiag_log != 2 makes it return at once. */
}

static void afdiag_sample(void)
{
    static int last_log = 0;
    afdiag_mpu_update();
    static int last_af = SLIM_AF_IDLE;
    static int last_pos = INT_MIN, last_dist = INT_MIN;
    static int last_rec = 0, last_rraw = 0;

    int rec = RECORDING ? 1 : 0;
    int rraw = RECORDING_RAW ? 1 : 0;

    if (afdiag_log && !last_log)
    {
        afdiag_make_header();
        /* start from the present state, so only changes get logged */
        last_af = slim_af_state;
        last_pos = lens_info.focus_pos;
        last_dist = lens_info.focus_dist;
        last_rec = rec;
        last_rraw = rraw;
    }
    last_log = afdiag_log;
    if (!afdiag_log) return;

    /* Read-only peek at Canon's LV AF state object (EOS M 2.0.2 ROM: pointer at
     * 0x4069C+12, from the literal at 0xFF1835F4).  Movie Servo may only start
     * when +0xC7C == 1, +0x184 == 2 and +0x710 == 1 (0xFF18597C); zooming
     * writes 5 / 0x81 / 10 into +0x710 (0xFF18EDB8).  Log on change. */
    if (is_camera("EOSM", "2.0.2"))
    {
        static uint32_t last_o[4] = { 1, 1, 1, 1 };
        uint32_t obj = *(volatile uint32_t *)(0x4069C + 12);
        uint32_t v[4] = { obj, 0, 0, 0 };
        if (obj >= 0x1000 && obj < 0x20000000 && !(obj & 3))
        {
            v[1] = *(volatile uint32_t *)(obj + 0x184);
            v[2] = *(volatile uint32_t *)(obj + 0x710);
            v[3] = ((*(volatile uint32_t *)(obj + 0xC7C) & 0xFF) << 8) |
                    (*(volatile uint32_t *)(obj + 0xC88) & 0xFF);
        }
        if (memcmp(v, last_o, sizeof(v)))
        {
            afdiag_record(AFD_EV_LVOBJ, 16, v[0], v[1], v[2], v[3]);
            memcpy(last_o, v, sizeof(v));
        }
    }

    if (slim_af_state != last_af)
    {
        afdiag_record(AFD_EV_AFSTATE, 8, last_af, slim_af_state, 0, 0);
        last_af = slim_af_state;
    }
    if (lens_info.focus_pos != last_pos || (int)lens_info.focus_dist != last_dist)
    {
        afdiag_record(AFD_EV_FOCUS, 16, last_pos, lens_info.focus_pos, last_dist, lens_info.focus_dist);
        last_pos = lens_info.focus_pos;
        last_dist = lens_info.focus_dist;
    }
    if (lv)
    {
        static int last_sharp = 0;
        int now = get_ms_clock();
        if (now - last_sharp >= AFDIAG_SHARP_MS)
        {
            int mean, n, emax;
            uint32_t es = afdiag_sharpness(&mean, &n, &emax);
            last_sharp = now;
            afdiag_record(AFD_EV_SHARP, 16, es, mean, n, emax);
        }
    }
    if (rec != last_rec || rraw != last_rraw)
    {
        afdiag_record(AFD_EV_REC, 8, rec, rraw, 0, 0);
        last_rec = rec;
        last_rraw = rraw;
    }
}

/* Append pending entries to the log.  Only when the card is free. */
static void afdiag_flush(void)
{
    static int last_flush = 0;
    static char buf[2048];
    int now = get_ms_clock();

    unsigned head = afdiag_head;
    if (head == afdiag_tail && !afdiag_header_pending) return;
    if (RECORDING || RECORDING_RAW || mlv_raw_rec_busy()) return;
    if (now - last_flush < AFDIAG_FLUSH_MS) return;
    last_flush = now;

    if (!is_dir(AFDIAG_LOG_DIR))
        FIO_CreateDirectory(AFDIAG_LOG_DIR);

    /* Field test: a lens test session passed the cap within ~2 minutes and
     * recreating the file wiped the whole session.  Keep the full file as
     * AFDIAG.OLD (replacing an older one) and start a fresh log instead. */
    uint32_t size = 0;
    FIO_GetFileSize(AFDIAG_LOG_FILE, &size);
    if (size > AFDIAG_MAX_FILE)
    {
        FIO_RemoveFile(AFDIAG_LOG_DIR "/AFDIAG.OLD");
        FIO_RenameFile(AFDIAG_LOG_FILE, AFDIAG_LOG_DIR "/AFDIAG.OLD");
    }
    FILE *f = FIO_CreateFileOrAppend(AFDIAG_LOG_FILE);
    if (!f) return;

    int n = 0;
    if (afdiag_header_pending)
    {
        afdiag_header_pending = 0;
        n += snprintf(buf, sizeof(buf), "%s", afdiag_header);
    }
    if (head - afdiag_tail > AFDIAG_RING)
    {
        unsigned lost = head - afdiag_tail - AFDIAG_RING;
        afdiag_tail += lost;
        n += snprintf(buf + n, sizeof(buf) - n, "# dropped %d entries (ring full)\n", (int)lost); /* ML snprintf has no %u */
    }

    while (afdiag_tail != head)
    {
        struct afdiag_entry e = afdiag_ring[afdiag_tail % AFDIAG_RING];
        afdiag_tail++;
        n += snprintf(buf + n, sizeof(buf) - n,
            /* ML's snprintf does not support field widths on %s ("%-20s"
             * came out literally on the camera), so keep this plain. */
            "%08d %s n=%d w=%08x,%08x,%08x,%08x z=%d rr=%d hs=%d af=%d pos=%d dist=%d\n",
            e.ms, afdiag_prop_name(e.code), e.len, e.w[0], e.w[1], e.w[2], e.w[3],
            e.zoom, e.rraw, e.hs, e.af, e.pos, e.dist);
        if (n > (int)sizeof(buf) - 200)
        {
            FIO_WriteFile(f, buf, n);
            n = 0;
        }
    }
    if (n > 0)
        FIO_WriteFile(f, buf, n);
    FIO_CloseFile(f);
}

/* Direct focus drive, as Canon's own LV code sends it (EOS M 2.0.2 ROM,
 * 0xFF184494): PROP_LV_FOCUS, 5 bytes { 1, steps_hi, steps_lo, 0xFF, 0xFF },
 * steps = signed 16-bit.  PROP_LV_LENS_DRIVE_REMOTE (what lens_focus sends)
 * is decoded by the firmware into this same message with |steps| <= 15.
 * Nothing on the main CPU consumes it, so prop_request_change_wait would
 * never return: wait for PROP_LV_FOCUS_DONE instead.  Returns 1 when done. */
static int __attribute__((unused)) afdiag_direct_focus(int steps) /* kept for reference: never moved the lens */
{
    uint8_t f[5];
    int done0 = afdiag_focus_done_count;
    steps = COERCE(steps, -1000, 1000);
    f[0] = 1;
    f[1] = (steps >> 8) & 0xFF;
    f[2] = steps & 0xFF;
    f[3] = 0xFF;
    f[4] = 0xFF;
    /* ML's prop_request_change() refuses this: Canon's property table
     * reports length 0 for PROP_LV_FOCUS (it is a request to the MPU that
     * the main CPU never stores), so the call ended in an ML ASSERT and was
     * never sent.  Call Canon's _prop_request_change exactly the way the
     * 2.0.2 firmware itself does at 0xFF1844C4 (same 5 bytes, same length).
     * The stub is not exported to modules, so call it by address and only
     * on this exact firmware. */
    if (!is_camera("EOSM", "2.0.2")) return 0;
    void (*canon_prop_request_change)(unsigned, const void *, size_t) =
        (void (*)(unsigned, const void *, size_t)) 0xFF11FB04;
    canon_prop_request_change(PROP_LV_FOCUS, f, 5);
    for (int i = 0; i < 300; i++)   /* up to 3 s */
    {
        if (afdiag_focus_done_count != done0) return 1;
        if (!lv || is_manual_focus()) return 0;
        msleep(10);
    }
    return 0;
}

/* One PROP_LV_LENS_DRIVE_REMOTE command, as lens_focus() sends it but
 * without its clamp to size 3: forward = size, backward = 0x8000 | size.
 * The firmware turns the low nibble (1..15) into a signed step count.
 * Waits for PROP_LV_FOCUS_DONE (up to 1 s).  Returns 1 when done. */
static int afdiag_remote_cmd(int signed_size)
{
    int size = COERCE(ABS(signed_size), 1, 15);
    int cmd = signed_size < 0 ? 0x8000 | size : size;
    int done0 = afdiag_focus_done_count;
    prop_request_change(PROP_LV_LENS_DRIVE_REMOTE, &cmd, 4);
    for (int i = 0; i < 100; i++)
    {
        if (afdiag_focus_done_count != done0) return 1;
        if (!lv || is_manual_focus()) return 0;
        msleep(10);
    }
    return 0;
}

/* lens_focus()-style remote command: wait for Canon to apply the property,
 * then for PROP_LV_FOCUS_DONE.  Returns 1 done, 2 done with the error bit
 * (0x1000, what lens.c treats as a soft limit), 0 timeout / not possible. */
static int afdiag_remote_cmd_wait(int signed_size)
{
    int size = COERCE(ABS(signed_size), 1, 15);
    int cmd = signed_size < 0 ? 0x8000 | size : size;
    int done0 = afdiag_focus_done_count;
    prop_request_change_wait(PROP_LV_LENS_DRIVE_REMOTE, &cmd, 4, 1000);
    for (int i = 0; i < 100; i++)
    {
        if (afdiag_focus_done_count != done0) return afdiag_focus_done_err ? 2 : 1;
        if (!lv || is_manual_focus()) return 0;
        msleep(10);
    }
    return 0;
}

/* Drive until an end: soft-limit error, the given focus_dist reached, or the
 * command cap.  Logs @SWEEP: w0 = phase * 100 + size, w1 = commands sent,
 * w2 = elapsed ms, w3 = end reason (0 timeout, 1 cap, 2 soft limit,
 * 3 end distance reached, 4 test switched off). */
static void afdiag_sweep_phase(int phase, int signed_size, int end_dist, int cap)
{
    int t0 = get_ms_clock();
    int n, reason = 1;
    for (n = 0; n < cap; n++)
    {
        if (afdiag_lens_test != 2) { reason = 4; break; }
        int r = afdiag_remote_cmd_wait(signed_size);
        if (r == 0) { reason = 0; break; }
        if (r == 2) { reason = 2; n++; break; }
        if (end_dist == 65535 ? (lens_info.focus_dist == 65535)
                              : (lens_info.focus_dist && (int)lens_info.focus_dist <= end_dist))
        { reason = 3; n++; break; }
    }
    afdiag_record(AFD_EV_SWEEP, 16, phase * 100 + ABS(signed_size), n,
                  get_ms_clock() - t0, reason);
}

/* A/B against the one path that has moved this lens in the field:
 * lens_focus(N, 3, wait) with nothing else going on.  The last sweep run
 * (which also requested a lens-status refresh every 10 commands) moved
 * nothing at all in either direction, so change one thing at a time:
 * control = lens_focus(+-60, 3), variant = 12 x size-15 commands sent the
 * same waiting way (same nominal travel).  @SWEEP: w0 = signed nominal
 * steps, w1 = 3 (control) or 15 (variant), w2 = elapsed ms,
 * w3 = focus_dist before << 16 | after. */
/* Field run 14: in movie mode every command was acknowledged (~30 ms each)
 * yet focus never moved, and the earlier "movement" turned out to be
 * unrelated.  ML's own focus stacking refuses movie mode on all cameras but
 * the 5D2 (src/focus.c), so the drive is probably gated by movie mode.
 * Now: lens_focus 100 x size 3, each way, in whatever mode the camera is in.
 * w1 = movie << 12 | lv_dispsize << 4 | size, so one session can compare
 * photo/movie and x1/x5.  A @STEP_SHARP follows each move (w1 = before,
 * w2 = 300 ms after). */
/* Field run 16: ML's built-in Focus End Point (lens_focus size 2, wait,
 * 10 ms extra delay) visibly moved the lens with Left, while every
 * test of ours used size 3 or 15 and never moved it.  Now compare sizes
 * 1 / 2 / 3, each +40 then -40 commands, with the built-in's timing. */
static void afdiag_ab_move(int dir, int size)
{
    int m, n, x;
    uint32_t s0 = afdiag_sharpness(&m, &n, &x);
    int t0 = get_ms_clock();
    int d0 = lens_info.focus_dist;
    lens_focus(dir * 40, size, 1, 10);
    int elapsed = get_ms_clock() - t0;
    int d1 = lens_info.focus_dist;
    msleep(300);
    uint32_t s1 = afdiag_sharpness(&m, &n, &x);
    afdiag_record(AFD_EV_SWEEP, 16, dir * 40 * size,
                  (is_movie_mode() << 12) | ((lv_dispsize & 0xFF) << 4) | size,
                  elapsed, ((d0 & 0xFFFF) << 16) | (d1 & 0xFFFF));
    afdiag_record(AFD_EV_STEP_SHARP, 16, dir * 40, s0, s1, m);
    msleep(1500);
}

static void afdiag_sweep(int unused)
{
    (void)unused;
    for (int size = 1; size <= 3 && afdiag_lens_test == 2; size++)
    {
        /* tell the viewer what is moving now */
        NotifyBox(1500, "AF lens test: size %d", size);
        afdiag_ab_move(+1, size);
        if (afdiag_lens_test != 2) break;
        afdiag_ab_move(-1, size);
    }
}

/* ---- AF lens test choices 3 "MPU" and 4 "MPU+SW1" ----
 *
 * A fixed, logged series of experiments that try to make the lens move by
 * sending Canon's own ICU -> MPU messages, each through the Canon property
 * that the 2.0.2 firmware turns into that message.  ROM findings (see the
 * property request handlers, callback 0xFF324950 and the pair table at
 * 0xFF32456C / 0xFF324100):
 *   PROP_LV_FOCUS      0x80050001 {1, hi, lo, FF, FF}  -> (9,0x01,5) [hi lo FF FF 00]; data[0] == 0 sends nothing
 *   PROP_LV_FOCUS_STOP 0x80050003 non-zero 32-bit word -> (9,0x03,1) [00]
 *   PROP_LVAF_001D     0x8005001D {1, b1, b2, b3}      -> (9,0x13,4) [b1 b2 b3 00]; data[0] == 0 sends nothing
 *   (session)          0x8005003C 15 bytes             -> (9,0x25,16) [15 bytes, 00]; Canon sends [02 00 ...]
 *   PROP_LV_FOCUS_CMD  0x80050027                      -> NOT in the callback (see below)
 *   PROP_REMOTE_SW1    0x80020015 data[0], data[1]     -> (4,0x16,3) [d0 d1 00]
 * Lengths, checked by running the ROM callback 0xFF324950 in an emulator:
 *   0x8005003C: the handler memcpy's exactly 15 bytes (mov r2,#15 at 0xFF3256B8)
 *   and appends a 00, hence the 16-byte MPU message; so the request is 15 bytes.
 *   The callback never looks at the length argument.
 *   0x80050027: the callback emits nothing for it (it falls out at 0xFF324D88),
 *   so the "(9,0x0B,2)" attributed to it earlier is really PROP_ 0x80050009
 *   (data[0] != 0, sends data[1] 00).  What step B does to 0x80050027 is therefore
 *   unconfirmed; the callback cannot tell a minimum length, so the length Canon
 *   itself last reported for the property is used (3 if never seen) and the
 *   value is put back to what Canon last had.
 * Every request goes through Canon's _prop_request_change (0xFF11FB04), like
 * afdiag_direct_focus, because ML's wrapper refuses properties whose cached
 * length is 0.  Nothing writes to the MPU ring and the packet builder is never
 * called directly.  Each step is logged as two @MPUTEST records; turn AF diag
 * log "ON + MPU" on to see the resulting @MPU messages next to them.
 * The run is one-shot: it switches the test back to OFF when it ends. */
#define AFDIAG_PROP_AF_SESSION 0x8005003C

enum
{
    AFM_STOP = 1,   /* FOCUS_STOP alone */
    AFM_FOCUS,      /* PROP_LV_FOCUS, a = signed relative pulses, then FOCUS_STOP */
    AFM_SEARCH,     /* PROP_LVAF_001D {1, a hi, a lo, b}, then FOCUS_STOP */
    AFM_OPEN25,     /* 0x8005003C {02, 0 ...} */
    AFM_STATE,      /* PROP_LV_FOCUS_CMD {a} */
    AFM_SW1,        /* remote half-press a = 1 / 0 */
    AFM_REMOTE,     /* b x PROP_LV_LENS_DRIVE_REMOTE of signed size a (MF allowed), then FOCUS_STOP */
    AFM_WAITAF,     /* wait for PROP_LV_AF_RESULT (settle ms at most) */
};

struct afdiag_mpu_step
{
    uint8_t kind;
    int16_t a;
    uint8_t b;
    uint16_t settle;  /* ms to watch after the request */
    uint8_t sw1_only; /* only in choice 4 */
    const char *name;
};

static const struct afdiag_mpu_step afdiag_mpu_table[] = {
    /* P: does a harmless request reach the MPU at all?  (expect @MPU 9,3,1) */
    { AFM_STOP,    0, 0,  600, 0, "P stop probe" },
    /* A: "AF session" message Canon sends at every half-press, then big relative drives */
    { AFM_OPEN25,  0, 0,  800, 0, "A open (9,25)" },
    { AFM_FOCUS,  50, 0, 1200, 0, "A +50" },
    { AFM_FOCUS, -50, 0, 1200, 0, "A -50" },
    { AFM_FOCUS, 200, 0, 1200, 0, "A +200" },
    { AFM_FOCUS,-200, 0, 1200, 0, "A -200" },
    { AFM_FOCUS, 500, 0, 1500, 0, "A +500" },
    { AFM_FOCUS,-500, 0, 1500, 0, "A -500" },
    /* B: Canon's "focus state" message (9,0x0B) = 0 (what it sends when an AF run ends), then drives */
    { AFM_STATE,   0, 0,  800, 0, "B state 0" },
    { AFM_FOCUS, 200, 0, 1200, 0, "B +200" },
    { AFM_FOCUS,-200, 0, 1200, 0, "B -200" },
    { AFM_FOCUS, 500, 0, 1500, 0, "B +500" },
    { AFM_FOCUS,-500, 0, 1500, 0, "B -500" },
    { AFM_STATE,   2, 0,  800, 0, "B state 2" },
    /* C: the remote drive sent directly (no lens_focus MF check); run it in AF and in MF */
    { AFM_REMOTE, 15, 10, 600, 0, "C remote +15 x10" },
    { AFM_REMOTE,-15, 10, 600, 0, "C remote -15 x10" },
    /* D: LensSearchDriveStart, payload layout inferred from the ROM only (3 bytes after the flag) */
    { AFM_SEARCH,  16, 0,    1000, 0, "D search +16,0" },
    { AFM_SEARCH, -16, 0,    1000, 0, "D search -16,0" },
    { AFM_SEARCH,  16, 1,    1000, 0, "D search +16,1" },
    { AFM_SEARCH, -16, 1,    1000, 0, "D search -16,1" },
    { AFM_SEARCH,   0, 1,    1000, 0, "D search 0,1" },
    { AFM_SEARCH,  64, 0xFF, 1000, 0, "D search +64,FF" },
    /* E: remote half-press (Canon runs an AF) and inject drives while it is running */
    { AFM_SW1,     1, 0,  400, 1, "E SW1 on" },
    { AFM_FOCUS, 100, 0, 1200, 1, "E +100 in AF" },
    { AFM_FOCUS,-100, 0, 1200, 1, "E -100 in AF" },
    { AFM_WAITAF,  0, 0, 4000, 1, "E wait AF result" },
    { AFM_SW1,     0, 0,  800, 1, "E SW1 off" },
};

static uint8_t afdiag_mpu_cmd_raw[16];  /* FOCUS_CMD as it was when the run started */
static int afdiag_mpu_cmd_len = 0;      /* 0 = never seen: restore 2 */
static int afdiag_mpu_sw1_on = 0;      /* this run holds the remote half-press */
static int afdiag_mpu_state_dirty = 0; /* this run sent (9,0x0B) = 0 and has not restored it */
static int afdiag_mpu_abort_reason = 0;

/* 0 = go on, otherwise the reason to stop (see AFD_EV_MPUTEST). */
static int afdiag_mpu_check(void)
{
    if (!lv) return 1;
    if (RECORDING) return 2;
    if (gui_menu_shown()) return 3;
    if (afdiag_lens_test < 3) return 4;
    return 0;
}

/* Sleep up to ms; returns 1 (and remembers why) if the run has to stop. */
static int afdiag_mpu_sleep(int ms)
{
    for (int t = 0; t < ms; t += 20)
    {
        int r = afdiag_mpu_check();
        if (r) { afdiag_mpu_abort_reason = r; return 1; }
        msleep(20);
    }
    return 0;
}

/* Fill the property id, payload and length of one request.  prop == 0 for
 * kinds that do not go through Canon's property call. */
static void afdiag_mpu_build(const struct afdiag_mpu_step *s, unsigned *prop, uint8_t *d, int *len)
{
    memset(d, 0, 16);
    *prop = 0;
    *len = 0;
    switch (s->kind)
    {
        case AFM_STOP:
            *prop = PROP_LV_FOCUS_STOP; d[0] = 1; *len = 4;
            break;
        case AFM_FOCUS:
            *prop = PROP_LV_FOCUS;
            d[0] = 1; d[1] = (s->a >> 8) & 0xFF; d[2] = s->a & 0xFF; d[3] = 0xFF; d[4] = 0xFF;
            *len = 5;
            break;
        case AFM_SEARCH:
            *prop = PROP_LVAF_001D;
            d[0] = 1; d[1] = (s->a >> 8) & 0xFF; d[2] = s->a & 0xFF; d[3] = s->b;
            *len = 4;
            break;
        case AFM_OPEN25:
            *prop = AFDIAG_PROP_AF_SESSION; d[0] = 2; *len = 15;
            break;
        case AFM_STATE:
            /* a == 2 means "Canon's idle value": what the run saw at its start */
            *prop = PROP_LV_FOCUS_CMD; d[0] = s->a;
            *len = afdiag_mpu_cmd_len ? afdiag_mpu_cmd_len : 3;
            if (s->a == 2 && afdiag_mpu_cmd_len) memcpy(d, afdiag_mpu_cmd_raw, afdiag_mpu_cmd_len);
            break;
        case AFM_REMOTE:
            d[0] = s->a < 0 ? 0x80 : 0;
            d[1] = ABS(s->a) & 0xFF;
            break;
        case AFM_SW1:
            d[0] = s->a;
            break;
    }
}

static int afdiag_mpu_canon_request(unsigned prop, const uint8_t *d, int len)
{
    if (!is_camera("EOSM", "2.0.2")) return 0;
    void (*canon_prop_request_change)(unsigned, const void *, size_t) =
        (void (*)(unsigned, const void *, size_t)) 0xFF11FB04;
    canon_prop_request_change(prop, d, len);
    return 1;
}

static void afdiag_mpu_stop(void)
{
    const struct afdiag_mpu_step stop = { AFM_STOP, 0, 0, 0, 0, "stop" };
    unsigned prop; uint8_t d[16]; int len;
    afdiag_mpu_build(&stop, &prop, d, &len);
    afdiag_mpu_canon_request(prop, d, len);
}

/* Start gate: nothing else may be using the lens, AF or the remote half-press. */
static int afdiag_mpu_busy(void)
{
    return RECORDING || is_continuous_af() || slim_af_state != SLIM_AF_IDLE ||
           slim_af_request || !aref_is_idle();
}

/* One step: request, watch focus_dist / pulse count / sharpness for the
 * settle time, FOCUS_STOP after anything that drives.  Returns 1 to stop. */
static int afdiag_mpu_step(int idx, int total, const struct afdiag_mpu_step *s)
{
    unsigned prop; uint8_t d[16]; int len;
    int m, n, x;

    int r0 = afdiag_mpu_check();
    if (r0) { afdiag_mpu_abort_reason = r0; return 1; }

    afdiag_mpu_build(s, &prop, d, &len);
    NotifyBox(1500, "MPU %d/%d %s", idx + 1, total, s->name);

    uint32_t sh0 = afdiag_sharpness(&m, &n, &x);
    int d0 = lens_info.focus_dist;
    int dmin = d0, dmax = d0;
    int fpc0 = afdiag_fpc;
    int done0 = afdiag_focus_done_count;
    int flags = 0;

    afdiag_record(AFD_EV_MPUTEST, 16,
                  (idx << 8) | ((is_manual_focus() ? 1 : 0) << 16) | ((is_movie_mode() ? 1 : 0) << 17),
                  prop,
                  d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t) d[3] << 24),
                  d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t) d[7] << 24));

    int sent = 0, drives = 0;
    switch (s->kind)
    {
        case AFM_STOP: case AFM_OPEN25: case AFM_STATE:
            sent = afdiag_mpu_canon_request(prop, d, len);
            if (s->kind == AFM_STATE) afdiag_mpu_state_dirty = (s->a == 0);
            break;
        case AFM_FOCUS: case AFM_SEARCH:
            sent = afdiag_mpu_canon_request(prop, d, len);
            drives = 1;
            break;
        case AFM_SW1:
            afdiag_mpu_sw1_on = s->a ? 1 : 0;
            SW1(s->a ? 1 : 0, 0);
            sent = 1;
            break;
        case AFM_REMOTE:
        {
            /* the same command lens_focus() sends, minus its MF refusal and size clamp */
            int cmd = s->a < 0 ? 0x8000 | ABS(s->a) : s->a;
            drives = 1;
            sent = 1;
            int timeouts = 0;
            for (int i = 0; i < s->b; i++)
            {
                int dn = afdiag_focus_done_count;
                prop_request_change(PROP_LV_LENS_DRIVE_REMOTE, &cmd, 4);
                for (int t = 0; t < 300 && afdiag_focus_done_count == dn; t += 10)
                {
                    int r = afdiag_mpu_check();
                    if (r) { afdiag_mpu_abort_reason = r; flags |= 0x40; break; }
                    msleep(10);
                }
                if (flags & 0x40) break;
                /* lens.c warns that queued unconfirmed focus requests crash the camera */
                timeouts = (afdiag_focus_done_count == dn) ? timeouts + 1 : 0;
                if (timeouts >= 2) { afdiag_mpu_abort_reason = 5; flags |= 0x20 | 0x40; break; }
            }
            break;
        }
        case AFM_WAITAF:
        {
            int c0 = slim_af_result_count;
            for (int t = 0; t < s->settle && slim_af_result_count == c0 && !(flags & 0x40); t += 20)
            {
                int r = afdiag_mpu_check();
                if (r) { afdiag_mpu_abort_reason = r; flags |= 0x40; break; }
                msleep(20);
            }
            break;
        }
    }
    if (sent) flags |= 1;

    if (s->kind != AFM_WAITAF)
    {
        int t0 = get_ms_clock();
        while (!(flags & 0x40) && get_ms_clock() - t0 < s->settle)
        {
            int r = afdiag_mpu_check();
            if (r) { afdiag_mpu_abort_reason = r; flags |= 0x40; break; }
            int dd = lens_info.focus_dist;
            if (dd < dmin) dmin = dd;
            if (dd > dmax) dmax = dd;
            msleep(20);
        }
    }
    if (drives && lv)
    {
        afdiag_mpu_stop();      /* always end a drive with FOCUS_STOP */
        if (!(flags & 0x40) && afdiag_mpu_sleep(400)) flags |= 0x40;
    }

    if (afdiag_focus_done_count != done0) flags |= 2;
    if ((flags & 2) && afdiag_focus_done_err) flags |= 4;
    int d1 = lens_info.focus_dist;
    if (dmin != dmax || d1 != d0) flags |= 8;
    int fpc1 = afdiag_fpc;
    int fdelta = 0;
    if (fpc0 != INT_MIN && fpc1 != INT_MIN) fdelta = COERCE(fpc1 - fpc0, -128, 127);
    if (fpc1 != fpc0) flags |= 0x10;
    uint32_t sh1 = afdiag_sharpness(&m, &n, &x);

    afdiag_record(AFD_EV_MPUTEST, 16,
                  ((uint32_t)(fdelta & 0xFF) << 24) | (flags << 16) | (idx << 8) | 1,
                  sh0, sh1, ((d0 & 0xFFFF) << 16) | (d1 & 0xFFFF));
    return (flags & 0x40) ? 1 : 0;
}

static void afdiag_mpu_run(void)
{
    if (!is_camera("EOSM", "2.0.2"))
    {
        NotifyBox(4000, "AF lens test MPU: EOS M 2.0.2 only");
        return;
    }
    if (!afdiag_log)
    {
        NotifyBox(5000, "AF lens test MPU: set AF diag log to ON + MPU first");
        return;
    }
    if (afdiag_mpu_busy())
    {
        NotifyBox(5000, "AF lens test MPU: not while recording, Servo AF, AF or Auto refocus busy");
        return;
    }
    if (afdiag_log != 2)
        NotifyBox(3000, "AF diag log is not ON + MPU: sent messages will not be logged");

    /* give the user time to close the menu and hands off the camera */
    afdiag_mpu_abort_reason = 0;
    NotifyBox(3000, "AF lens test MPU: close menu, starts in 3 s");
    for (int t = 0; t < 3000; t += 100)
    {
        msleep(100);
        if (!afdiag_lens_test) return;
    }
    while (gui_menu_shown())
    {
        msleep(200);
        if (!afdiag_lens_test) return;
    }

    /* the countdown above is long: look again before the first message */
    if (afdiag_mpu_busy() || !lv)
    {
        NotifyBox(4000, "AF lens test MPU: aborted, camera busy");
        afdiag_lens_test = 0;
        return;
    }

    int total = 0;
    for (int i = 0; i < (int) COUNT(afdiag_mpu_table); i++)
        if (!afdiag_mpu_table[i].sw1_only || afdiag_lens_test >= 4) total++;

    afdiag_mpu_sw1_on = 0;
    afdiag_mpu_state_dirty = 0;
    afdiag_mpu_cmd_len = afdiag_focus_cmd_len;
    if (afdiag_mpu_cmd_len) memcpy(afdiag_mpu_cmd_raw, afdiag_focus_cmd_raw, afdiag_mpu_cmd_len);
    int done = 0;
    for (int i = 0; i < (int) COUNT(afdiag_mpu_table); i++)
    {
        const struct afdiag_mpu_step *s = &afdiag_mpu_table[i];
        if (s->sw1_only && afdiag_lens_test < 4) continue;
        if (afdiag_mpu_step(done, total, s)) break;
        done++;
    }

    /* Whatever happened: release the half-press only if this run pressed it
     * (afdiag_mpu_sw1_on is set only by the run's own SW1 step), end the
     * drive, put FOCUS_CMD back to the value Canon last had. */
    if (afdiag_mpu_sw1_on) { SW1(0, 0); afdiag_mpu_sw1_on = 0; }
    if (lv) afdiag_mpu_stop();
    if (afdiag_mpu_state_dirty)     /* not gated on lv: leaving the value changed is worse */
    {
        const struct afdiag_mpu_step idle = { AFM_STATE, 2, 0, 0, 0, "idle" };
        unsigned prop; uint8_t d[16]; int len;
        afdiag_mpu_build(&idle, &prop, d, &len);
        afdiag_mpu_canon_request(prop, d, len);
    }
    afdiag_mpu_state_dirty = 0;
    afdiag_record(AFD_EV_MPUTEST, 8, 0xFF00 | 1, afdiag_mpu_abort_reason, done, total);
    static const char * const why[] = { "done", "LV lost", "recording started", "menu opened", "switched off", "focus not confirmed" };
    NotifyBox(5000, "AF lens test MPU: %s (%d/%d steps)", why[afdiag_mpu_abort_reason], done, total);
    afdiag_lens_test = 0;
}

/* Lens test (choice 1 "Remote": bursts of remote commands, size 3 vs 15;
 * choice 2 "Direct": a size-1 remote command, then one PROP_LV_FOCUS move
 * of 50/200 steps).  Never touches the card. */
static void afdiag_lens_task(void *unused)
{
    int dir = 1;
    int last_step = 0;
    int last_skip = 0;

    int armed_val = 0, armed_since = 0;

    while (1)
    {
        msleep(100);
        /* a choice only starts after it has been selected for 1.5 s, so
         * scrolling past it in the menu does not run it */
        if (afdiag_lens_test != armed_val)
        {
            armed_val = afdiag_lens_test;
            armed_since = get_ms_clock();
        }
        if (!afdiag_lens_test) { last_skip = 0; continue; }
        if (get_ms_clock() - armed_since < 1500) continue;
        if (!lv) continue;
        if (afdiag_lens_test >= 3)
        {
            afdiag_mpu_run();   /* one-shot; switches the test back to OFF */
            afdiag_lens_test = 0;
            continue;
        }
        /* Remote bursts stay movie-only; A/B also runs in photo LV. */
        if (afdiag_lens_test == 1 && !is_movie_mode()) continue;

        int now = get_ms_clock();
        if (now - last_step < AFDIAG_LENS_PERIOD) continue;

        int skip = 0;
        if (is_manual_focus()) skip = 1;
        else if (is_continuous_af()) skip = 2;
        else if (slim_af_state != SLIM_AF_IDLE) skip = 3;

        if (skip)
        {
            /* log a refusal once per reason, not every 2 s */
            if (skip != last_skip)
            {
                afdiag_record(AFD_EV_LENS_SKIP, 4, skip, 0, 0, 0);
                /* say it on screen too: the first field test silently did nothing */
                if (skip == 1) NotifyBox(3000, "AF lens test: lens is in MF");
                if (skip == 2) NotifyBox(3000, is_movie_mode() ? "AF lens test: turn off Movie Servo AF"
                                                               : "AF lens test: turn off Continuous AF");
            }
            last_skip = skip;
            last_step = now;
            continue;
        }
        last_skip = 0;

        int m0, n0, x0;
        uint32_t sharp_before = afdiag_sharpness(&m0, &n0, &x0);

        if (afdiag_lens_test == 2)
        {
            /* Sweep test.  Field runs: direct PROP_LV_FOCUS moves never moved
             * the lens (~40 tries, primed or not), and back-to-back remote
             * commands barely did; only lens_focus() (prop_request_change_wait
             * + PROP_LV_FOCUS_DONE, ~42 ms per command) moved it reliably.
             * Sharpness is a poor ruler once focus is lost, so time full-range
             * sweeps instead: go to the near end with size 3, then sweep to
             * infinity with size 3, 8 or 15 (one variant per cycle), all with
             * lens_focus()-style waiting.  Field log: negative = toward
             * infinity on this lens. */
            static const int sweep_size[] = { 3, 8, 15 };
            static int sweep_idx = 0;
            afdiag_sweep(sweep_size[sweep_idx]);
            sweep_idx = (sweep_idx + 1) % COUNT(sweep_size);
            last_step = get_ms_clock();
            continue;
        }
        /* Remote bursts.  Field tests: 200 size-3 commands swept the 18-55
         * from 28 cm to infinity.  The firmware decoder accepts sizes up to
         * 15 (low nibble; ML's lens_focus clamps to 3), so compare equal
         * nominal travel: 50 x size 3 vs 10 x size 15, then 100 x 3 vs
         * 20 x 15.  +N then -N, so focus returns.  Logged: w0 = signed
         * command count, w1 = size * 16 + (1 if every command was done). */
        /* Second run: when commands were sent back to back (~16 ms each),
         * 100 x size 3 moved focus less than one distance-scale step, while
         * the older lens_focus() path (~42 ms per command) had swept 200 x 3
         * from 28 cm to infinity; and 20 x size 15 moved no more than 20 x 3.
         * Hypothesis: a new command cuts the previous move short, so
         * movement depends on the gap, not the size.  Test 30 commands with
         * gaps of 0 / 40 / 100 ms, at size 3 and 15.
         * Logged: w0 = signed count, w1 = size * 16 + all-done, w2 = gap ms. */
        static const int burst_size[] = {  3,  3,   3, 15, 15,  15 };
        static const int burst_gap[]  = {  0, 40, 100,  0, 40, 100 };
        static int burst_idx = 0;
        int count = 30;
        int size  = burst_size[burst_idx];
        int gap   = burst_gap[burst_idx];
        int all_done = 1;
        for (int i = 0; i < count && afdiag_lens_test == 1; i++)
        {
            if (!afdiag_remote_cmd(dir * size)) { all_done = 0; break; }
            if (gap) msleep(gap);
        }
        int elapsed = get_ms_clock() - now;
        afdiag_record(AFD_EV_LENS_STEP, 16, dir * count, size * 16 + all_done, gap, elapsed);
        msleep(AFDIAG_STEP_SETTLE + 200);
        int m1, n1, x1;
        uint32_t sharp_after = afdiag_sharpness(&m1, &n1, &x1);
        afdiag_record(AFD_EV_STEP_SHARP, 16, dir * count, sharp_before, sharp_after, m1);
        if (dir < 0) burst_idx = (burst_idx + 1) % COUNT(burst_size);
        dir = -dir;
        last_step = get_ms_clock();
    }
}

/* One-shot MPU AF search with a sharpness-peak stop.  This is the bridge
 * between the confirmed (8,2,32) actuator and a real autofocus: the MPU only
 * scans; the ICU samples sharpness and sends FOCUS_STOP when the peak is
 * passed.  Experimental, temporary diagnostics menu only.
 *
 * The MPU sweep is not monotonic: from 2.9 m it went near (~50 cm) first and
 * only then to infinity.  Sharpness falling on that near leg is not "past the
 * peak", so the peak tracker is only armed once focus_dist starts rising
 * (lens heading to infinity); the peak is reset at that moment.  Lenses that
 * never report a distance change are armed after AFS_ARM_FALLBACK_MS.
 *
 * Every run is traced to ML/LOGS/AFSEARCH.LOG regardless of "AF diag log"
 * (the first field run was lost because afdiag_record() is a no-op while the
 * diag log is OFF, its default after restart).  The trace continues for
 * AFS_POST_MS after the stop so the overshoot can be measured. */
#define AFS_LOG_FILE         "ML/LOGS/AFSEARCH.LOG"
#define AFS_MAX_MS           3000
#define AFS_SAMPLE_MS        50
#define AFS_DROP_PCT         80     /* stop when sharpness < 80% of peak ...    */
#define AFS_DWELL_MS         120    /* ... for longer than this after the peak  */
#define AFS_ARM_FALLBACK_MS  1500
#define AFS_POST_MS          500
#define AFS_MAX_SAMPLES      128

struct afs_sample
{
    int      t;       /* ms since the search command */
    uint32_t s;       /* centre Laplacian sum (stop metric) */
    uint32_t g;       /* centre gradient sum (contrast normaliser) */
    int      luma;
    int      dist;    /* lens_info.focus_dist, cm */
    char     phase;   /* 'n' near leg / not armed, 'a' armed, 'p' post-stop */
};

static volatile int afdiag_afsearch_busy = 0;
static volatile int afdiag_afdrive_busy;   /* defined with the AF drive code below */
static struct afs_sample afs_samples[AFS_MAX_SAMPLES];
static char afs_buf[1024];

static int afs_take(int n, int t0, char phase)
{
    if (n >= AFS_MAX_SAMPLES) return n;
    int m = 0, cnt = 0, x = 0;
    uint32_t g = 0;
    struct afs_sample *e = &afs_samples[n];
    e->s = afdiag_sharpness_ex(&m, &cnt, &x, &g);
    e->g = g;
    e->luma = m;
    e->dist = lens_info.focus_dist;
    e->t = get_ms_clock() - t0;
    e->phase = phase;
    return n + 1;
}

static void afs_write_log(int n, int dist0, int ret, int stopped, int armed_ms,
                          uint32_t peak, int peak_ms, int stop_ms)
{
    if (RECORDING || RECORDING_RAW || mlv_raw_rec_busy()) return;
    if (!is_dir(AFDIAG_LOG_DIR))
        FIO_CreateDirectory(AFDIAG_LOG_DIR);
    FILE *f = FIO_CreateFileOrAppend(AFS_LOG_FILE);
    if (!f) return;

    int k = snprintf(afs_buf, sizeof(afs_buf),
        "# AFSEARCH t=%d lens=\"%s\" zoom=x%d crop_preset=%d dist0=%d ret=%d "
        "max=%d sample=%d drop=%d dwell=%d arm_fallback=%d\n"
        "# result=%s armed_ms=%d peak=%d peak_ms=%d stop_ms=%d n=%d\n"
        "# ms s g luma dist phase\n",
        get_ms_clock(), lens_info.name, lv_dispsize, crop_preset_index, dist0, ret,
        AFS_MAX_MS, AFS_SAMPLE_MS, AFS_DROP_PCT, AFS_DWELL_MS, AFS_ARM_FALLBACK_MS,
        stopped ? "stop" : "timeout", armed_ms, (int) peak, peak_ms, stop_ms, n);
    FIO_WriteFile(f, afs_buf, k);

    k = 0;
    for (int i = 0; i < n; i++)
    {
        struct afs_sample *e = &afs_samples[i];
        k += snprintf(afs_buf + k, sizeof(afs_buf) - k, "%d %d %d %d %d %s\n",
                      e->t, (int) e->s, (int) e->g, e->luma, e->dist,
                      e->phase == 'a' ? "a" : e->phase == 'p' ? "p" : "n");
        if (k > (int) sizeof(afs_buf) - 80)
        {
            FIO_WriteFile(f, afs_buf, k);
            k = 0;
        }
    }
    k += snprintf(afs_buf + k, sizeof(afs_buf) - k, "\n");
    FIO_WriteFile(f, afs_buf, k);
    FIO_CloseFile(f);
}

static void afdiag_afsearch_task(void *unused)
{
    (void) unused;
    if (afdiag_afsearch_busy || afdiag_afdrive_busy) return;
    afdiag_afsearch_busy = 1;

    if (!is_EOSM || !lv || RECORDING ||
        is_manual_focus() || slim_af_state != SLIM_AF_IDLE)
    {
        NotifyBox(4000, "AF search MPU: needs AF mode, LV, idle, not recording");
        goto out;
    }
    if (lens_info.focus_dist >= 60000)
    {
        NotifyBox(5000, "AF search MPU: lens at infinity - focus nearer first");
        goto out;
    }

    NotifyBox(3000, "AF search MPU: close menu, starts in 2 s");
    msleep(1500);
    for (int i = 0; i < 50 && gui_menu_shown(); i++) msleep(100);
    if (!lv || RECORDING || is_manual_focus() || slim_af_state != SLIM_AF_IDLE)
        goto out;
    /* re-check: the lens may have been moved while the menu was open */
    if (lens_info.focus_dist >= 60000)
    {
        NotifyBox(5000, "AF search MPU: lens at infinity - focus nearer first");
        goto out;
    }

    int e = ((int (*)(void)) 0xFF1DD098)();   /* MpuMonOpen */
    if (e)
    {
        NotifyBox(4000, "AF search MPU: MonOpen failed %d", e);
        goto out;
    }

    int dist0 = lens_info.focus_dist;
    uint8_t payload[32] = {0};
    payload[7] = 0x10;   /* (8,2,32), index 16 = 0x33794 AF_LENS_SEARCH */
    int ret = ((int (*)(int,int,int,const void *)) 0x38C4)(8, 2, 32, payload);

    int t0 = get_ms_clock();
    int n = 0;
    int armed = 0, armed_ms = -1;
    int dist_min = dist0;
    int peak_t = 0;
    uint32_t peak = 0;
    int stopped = 0;

    while (get_ms_clock() - t0 < AFS_MAX_MS)
    {
        n = afs_take(n, t0, armed ? 'a' : 'n');
        struct afs_sample *c = &afs_samples[n - 1];
        int now = c->t;

        if (!armed)
        {
            /* ignore 0 (no reading) and 1 cm jitter: need a real rise */
            if (c->dist > 0 && c->dist < dist_min) dist_min = c->dist;
            int rise = dist_min / 20 > 3 ? dist_min / 20 : 3;
            if (c->dist >= dist_min + rise ||
                (now >= AFS_ARM_FALLBACK_MS && dist_min == dist0 && c->dist == dist0))
            {
                armed = 1;
                armed_ms = now;
                peak = 0;           /* only peaks on the far leg count */
                c->phase = 'a';
            }
        }

        if (armed)
        {
            if (c->s > peak) { peak = c->s; peak_t = now; }
            if (peak > 0 && now - peak_t > AFS_DWELL_MS &&
                c->s * 100 < peak * AFS_DROP_PCT)
            {
                stopped = 1;
                break;
            }
        }
        msleep(AFS_SAMPLE_MS);
    }

    uint8_t stop = 0;
    ((int (*)(int,int,int,const void *)) 0x38C4)(9, 3, 1, &stop);
    int stop_ms = get_ms_clock() - t0;

    /* keep sampling while the lens settles: overshoot = peak vs settled s */
    int post_end = stop_ms + AFS_POST_MS;
    while (get_ms_clock() - t0 < post_end && n < AFS_MAX_SAMPLES)
    {
        msleep(AFS_SAMPLE_MS);
        n = afs_take(n, t0, 'p');
    }

    int dist = lens_info.focus_dist;
    ((int (*)(void)) 0xFF1DD118)();            /* MpuMonClose */

    afdiag_record(AFD_EV_MPUSRCH, 16, (uint32_t) stopped, peak,
                  (uint32_t) stop_ms, (uint32_t) dist);
    afs_write_log(n, dist0, ret, stopped, armed_ms, peak, peak_t, stop_ms);

    uint32_t settled = n ? afs_samples[n - 1].s : 0;
    NotifyBox(6000, "AF MPU %s: peak=%d end_pct=%d arm=%d stop=%dms d=%d->%d",
              stopped ? "stop" : "timeout", (int) peak,
              peak ? (int)(settled * 100 / peak) : 0, armed_ms, stop_ms, dist0, dist);

out:
    afdiag_afsearch_busy = 0;
}

static MENU_SELECT_FUNC(afdiag_afsearch_select)
{
    (void) priv;
    (void) delta;
    task_create("afdiag_afsearch", 0x1e, 0x2000, afdiag_afsearch_task, (void*)0);
}

/* ---- AF drive (MPU): first contrast-detect autofocus, one-shot ------------
 * Moves the lens with the confirmed (9,1,5) relative move [hi lo 07 00 00]
 * (signed 16-bit pulses), same session framing as mpu_lab "Drive test".
 * lens_info.focus_dist does not follow these moves, so the position is the
 * sum of commanded pulses (relative to the start; + is probably toward near).
 * Phases: probe direction and coarse climb on g (broad gradient sum), one-way
 * fine sweep on s (narrow Laplacian peak) with parabolic interpolation, final
 * approach to the peak (+ one finer recovery sweep if the final s missed it).
 * FOCUS_DONE (PROP_LV_FOCUS_DONE) is counted by the handler above; a move
 * waits for the count to increase (error bits are only logged) and in any
 * case for AFD_MIN_BASE_MS + |n|*AFD_MIN_PER_PULSE, then 2 LV frames. */
#define AFD_LOG_FILE         "ML/LOGS/AFDRIVE.LOG"
#define AFD_SPEED            0x07
#define AFD_COARSE           60     /* coarse step, measured on g */
#define AFD_FINE             20     /* fine sweep step, measured on s */
#define AFD_FINE_SPAN        80     /* fine sweep covers best_g_pos -80 .. +80 */
#define AFD_FINE_PTS         9
#define AFD_REC_STEP         10     /* recovery sweep step */
#define AFD_REC_PTS          9
#define AFD_PROBE_PCT        95     /* probe: < this % of g0 = wrong way, within +-5% = ambiguous */
#define AFD_COARSE_STOP_PCT  75     /* coarse stop: one move below this % of best g ... */
#define AFD_COARSE_DROP_PCT  90     /* ... or this many moves in a row below this % */
#define AFD_COARSE_DROP_N    2
#define AFD_FLAT_PCT         3      /* stop: |change| below this % ... */
#define AFD_FLAT_N           3      /* ... this many moves in a row */
#define AFD_MAX_TRAVEL       1500   /* coarse phase, total |pulses| */
#define AFD_MAX_COARSE       25     /* coarse phase moves (probes included) */
#define AFD_DONE_TIMEOUT_MS  800
#define AFD_MIN_BASE_MS      40
#define AFD_MIN_PER_PULSE    0
#define AFD_MIN_CAP_MS       700
#define AFD_MIN_S            2000   /* below this s0 the scene has no contrast: skip */
#define AFD_FRAME_MS         45     /* one LV frame, between the two samples */
#define AFD_SETTLE_MS        50     /* after FOCUS_DONE */
#define AFD_LOG_SIZE         12288
#define AFD_LOG_RESERVE     256    /* kept free for the "# RESULT" line */
#define AFD_APPROACH         120    /* approach leg, longer than the backlash (real value 30-60) */
#define AFD_RECOVER_PCT      85     /* final s < this % of the fine max: one recovery sweep */
#define AFD_MAX_TOTAL        5000   /* whole run, total |pulses| */
#define AFD_CAL_STEP         40     /* AF drive calib: pulses per move */
#define AFD_CAL_MOVES        8      /* ... moves per pass: -160..+160 */
#define AFD_CAL_START        (-160) /* sweep start, centred on the current position */
#define AFD_LV_PCT           70     /* LV valid again: luma >= this % of luma_pre ... */
#define AFD_LV_MIN           3      /* ... and at least this ... */
#define AFD_LV_N             3      /* ... for this many samples in a row */
#define AFD_LV_POLL_MS       50
#define AFD_LV_TIMEOUT_MS    5000
#define AFD_LOW_PCT          50     /* before a sample: luma < this % of luma_pre = LV not valid */
#define AFD_LOW_WAIT_MS      1000
#define AFD_FAST_MS          40     /* FOCUS_DONE sooner than this = the immediate reject signature */
#define AFC_LOG_FILE         "ML/LOGS/AFCALIB.LOG"

static volatile int afdiag_afdrive_busy = 0;
static int afdiag_afdrive_mon = 2;   /* 0 OFF, 1 ON (open/close per run), 2 keep open after the first run */
/* MPU monitor left open by "Keep open": MpuMonOpen = (8,3) to the MPU, which
 * re-initialises its lens engine (LV dark ~2 s) and is what lets (9,1) moves
 * execute.  Keeping it open pays that blackout once instead of every run. */
static volatile int afd_mon_is_open = 0;
static volatile int afd_mon_closing = 0;
static void afd_mon_close_now(void)
{
    if (!afd_mon_is_open) return;
    ((int (*)(void)) 0xFF1DD118)();   /* MpuMonClose */
    afd_mon_is_open = 0;
}   /* 1: MpuMonOpen/Close around the run (default; OFF does not drive the lens), 0: no monitor */
static char afd_buf[AFD_LOG_SIZE];
static int afd_log_reserve = AFD_LOG_RESERVE;
static int afd_len, afd_pos, afd_moves, afd_t0, afd_aborted, afd_lowcontrast;
static int afd_travel, afd_recovered;
static int afd_peak_g_pos, afd_fine_peak_pos;
static uint32_t afd_last_g;   /* g of the last measurement (afd_move_ex) */
static int afd_luma_pre, afd_nolv, afd_n_nodone, afd_n_err, afd_n_fast;
static int afd_last_dir;     /* direction of the last non-zero move */
static uint32_t afd_best_s;
static int afd_best_pos;
static int afd_best_dir;     /* direction of travel when the best was measured */

/* NOTE: ML snprintf lacks the char conversion and mishandles the percent escape: the AFD/AFS/calib log text
 * uses only %d, %x and plain %s, percentages are written as name_pct=N. */
static void afd_log(const char *fmt, ...)
{
    int room = (int) sizeof(afd_buf) - afd_log_reserve;   /* reserve 0 only for the RESULT line */
    if (afd_len >= room - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(afd_buf + afd_len, room - afd_len, fmt, ap);
    va_end(ap);
    if (k > 0) afd_len += MIN(k, room - afd_len - 1);
}

static int afd_must_abort(void)
{
    return RECORDING || RECORDING_RAW || !lv || gui_menu_shown() || is_manual_focus();
}

/* mean of 2 samples one LV frame apart */
static uint32_t afd_sample(uint32_t *g_out, int *luma_out)
{
    int m1 = 0, m2 = 0, c = 0, x = 0;
    uint32_t g1 = 0, g2 = 0;
    uint32_t s1 = afdiag_sharpness_ex(&m1, &c, &x, &g1);
    msleep(AFD_FRAME_MS);
    uint32_t s2 = afdiag_sharpness_ex(&m2, &c, &x, &g2);
    *g_out = (g1 + g2) / 2;
    *luma_out = (m1 + m2) / 2;
    return (s1 + s2) / 2;
}

static int afd_luma_now(void)
{
    int m = 0, c = 0, x = 0;
    uint32_t g = 0;
    afdiag_sharpness_ex(&m, &c, &x, &g);
    return m;
}

static int afd_luma_ok(int luma, int pct)
{
    return luma * 100 >= afd_luma_pre * pct;
}

/* After MonOpen + session open: wait until LV has its normal brightness again
 * (AFD_LV_N samples in a row >= AFD_LV_PCT of luma_pre and >= AFD_LV_MIN).
 * Returns 1 when recovered, 0 on timeout or abort (afd_aborted set on abort). */
static int afd_wait_lv(int expect_dark)
{
    int t = get_ms_clock();
    int ok = 0, n = 0, luma = 0, timeout = 0, dark_ms = -1;
    /* The blackout starts with a delay (luma still normal ~20 ms after the
     * session open, black by ~300 ms): first watch up to 800 ms for the dip,
     * otherwise the recovery check below could pass before it begins. */
    while (expect_dark && get_ms_clock() - t < 800)
    {
        luma = afd_luma_now();
        if (!afd_luma_ok(luma, 50)) { dark_ms = get_ms_clock() - t; break; }
        if (afd_must_abort()) { afd_aborted = 1; return 0; }
        msleep(AFD_LV_POLL_MS);
    }
    afd_log("# lv_dark ms=%d luma=%d\n", dark_ms, luma);
    while (1)
    {
        luma = afd_luma_now();
        n = (luma >= AFD_LV_MIN && afd_luma_ok(luma, AFD_LV_PCT)) ? n + 1 : 0;
        if (n >= AFD_LV_N) { ok = 1; break; }
        if (afd_must_abort()) { afd_aborted = 1; break; }
        if (get_ms_clock() - t >= AFD_LV_TIMEOUT_MS) { timeout = 1; break; }
        msleep(AFD_LV_POLL_MS);
    }
    afd_log("# lv_recover ms=%d luma_pre=%d luma=%d timeout=%d\n", get_ms_clock() - t, afd_luma_pre, luma, timeout);
    return ok;
}

static void afd_luma_probe(const char *tag)
{
    int m = 0, c = 0, x = 0;
    uint32_t g = 0;
    uint32_t sv = afdiag_sharpness_ex(&m, &c, &x, &g);
    afd_log("# luma %s t=%d luma=%d s=%d g=%d\n", tag, get_ms_clock() - afd_t0, m, (int) sv, (int) g);
}

/* Move by n pulses (0: just sample), settle, sample (sample=0: no
 * measurement, the line shows zeros); logs one line. track=1: the measurement
 * may become the best (position, direction of travel). Returns the sharpness,
 * or 0 after setting afd_aborted. */
static uint32_t afd_move_ex(const char *phase, int n, int sample, int track)
{
    if (afd_must_abort() || afd_travel + ABS(n) > AFD_MAX_TOTAL)
    {
        afd_aborted = 1;
        return 0;
    }
    uint32_t g = 0;
    int luma = 0;
    int done_val = 0, done_ms = -1, ret = 0;
    if (n)
    {
        afd_last_dir = n > 0 ? 1 : -1;
        uint8_t d[5] = { (uint8_t)((n >> 8) & 0xFF), (uint8_t)(n & 0xFF), AFD_SPEED, 0, 0 };
        int cnt0 = afdiag_focus_done_count;
        int t = get_ms_clock();
        ret = ((int (*)(int,int,int,const void *)) 0x38C4)(9, 1, 5, d);
        afd_pos += n;
        afd_travel += ABS(n);
        int min_ms = AFD_MIN_BASE_MS + ABS(n) * AFD_MIN_PER_PULSE;
        if (min_ms > AFD_MIN_CAP_MS) min_ms = AFD_MIN_CAP_MS;
        while (get_ms_clock() - t < AFD_DONE_TIMEOUT_MS)
        {
            if (done_ms < 0 && afdiag_focus_done_count != cnt0)
            {
                done_ms = get_ms_clock() - t;
                done_val = (int) afdiag_focus_done_val;
            }
            if (done_ms >= 0 && get_ms_clock() - t >= min_ms) break;
            msleep(10);
        }
        msleep(AFD_SETTLE_MS);
    }
    uint32_t s = 0;
    int lowluma = 0;
    if (sample && afd_luma_pre > 0 && !afd_luma_ok(afd_luma_now(), AFD_LOW_PCT))
    {
        int t = get_ms_clock();
        lowluma = 1;
        while (get_ms_clock() - t < AFD_LOW_WAIT_MS)
        {
            msleep(AFD_LV_POLL_MS);
            if (afd_luma_ok(afd_luma_now(), AFD_LOW_PCT)) { lowluma = 0; break; }
        }
    }
    if (sample) s = afd_sample(&g, &luma);
    afd_last_g = g;
    afd_moves += n ? 1 : 0;
    int f_nodone = n && done_ms < 0;
    int f_err = n && done_ms >= 0 && (done_val & 0x1000);
    int f_fast = n && done_ms >= 0 && done_ms < AFD_FAST_MS;
    afd_n_nodone += f_nodone;
    afd_n_err += f_err;
    afd_n_fast += f_fast;
    afd_log("%d %s %d %d %d %d %d %x %s%s%s%s%s %d ret%d dist=%d\n", get_ms_clock() - afd_t0, phase, n, afd_pos,
            (int) s, (int) g, luma, done_val,
            (f_nodone || f_err || f_fast || lowluma) ? "" : "-",
            f_nodone ? "nodone" : "", f_err ? (f_nodone ? "+err" : "err") : "",
            f_fast ? ((f_nodone || f_err) ? "+fast" : "fast") : "",
            lowluma ? ((f_nodone || f_err || f_fast) ? "+lowluma" : "lowluma") : "",
            done_ms, ret, (int) lens_info.focus_dist);
    if (sample && track && s > afd_best_s)
    {
        afd_best_s = s;
        afd_best_pos = afd_pos;
        afd_best_dir = afd_last_dir;
    }
    return s;
}

static uint32_t afd_move(const char *phase, int n)
{
    return afd_move_ex(phase, n, 1, 1);
}

/* Go to target so that the last movement is in approach_dir (+1/-1). If the
 * needed move is not already in approach_dir, or is shorter than AFD_APPROACH,
 * first go (unmeasured, phase "o") to target - approach_dir*AFD_APPROACH, then
 * move approach_dir*AFD_APPROACH (measured).  Two reversals absorb the backlash
 * equally, so the landing position does not depend on the backlash as long as
 * the leg is longer than it.  A long move already in approach_dir is a single
 * measured move.  Measures once at the target. */
static uint32_t afd_goto(const char *phase, int target, int approach_dir, int track)
{
    int need = target - afd_pos;
    if (need == 0)   /* no move at all, so no direction change: the backlash state is the one we arrived with */
        return afd_move_ex(phase, 0, 1, track);
    if (need * approach_dir >= AFD_APPROACH)
        return afd_move_ex(phase, need, 1, track);
    afd_move_ex("o", need - approach_dir * AFD_APPROACH, 0, 0);
    if (afd_aborted) return 0;
    return afd_move_ex(phase, approach_dir * AFD_APPROACH, 1, track);
}

/* a < b * pct / 100 */
static int afd_below(uint32_t a, uint32_t b, int pct)
{
    return (uint64_t) a * 100 < (uint64_t) b * pct;
}

/* keep the best g (and where) over the measurements of the coarse phase */
static void afd_gbest(uint32_t *bg, int *bpos)
{
    if (afd_last_g > *bg) { *bg = afd_last_g; *bpos = afd_pos; }
}

/* goto start (approach d, measured), then npts-1 measured moves of d*step.
 * Fills pos/sv; returns the number of points, 0 after an abort. */
static int afd_sweep(const char *phase, int start, int d, int step, int npts, int *pos, uint32_t *sv)
{
    sv[0] = afd_goto(phase, start, d, 0);
    pos[0] = afd_pos;
    if (afd_aborted) return 0;
    for (int i = 1; i < npts; i++)
    {
        sv[i] = afd_move_ex(phase, d * step, 1, 0);
        pos[i] = afd_pos;
        if (afd_aborted) return 0;
    }
    return npts;
}

static int afd_argmax(const uint32_t *sv, int n)
{
    int im = 0;
    for (int i = 1; i < n; i++)
        if (sv[i] > sv[im]) im = i;
    return im;
}

/* returns the final sharpness; fills result fields */
static uint32_t afd_run(uint32_t *best_out)
{
    uint32_t g;
    int luma;
    afd_best_s = 0;
    afd_best_pos = 0;
    afd_best_dir = 0;
    afd_peak_g_pos = 0;
    afd_fine_peak_pos = 0;

    /* 1. baseline (logged as a zero move) */
    uint32_t s0 = afd_move("i", 0);
    uint32_t g0 = afd_last_g;
    uint32_t s1 = afd_sample(&g, &luma);
    s0 = (s0 + s1) / 2;
    g0 = (g0 + g) / 2;
    afd_best_s = s0;
    afd_log("# s0=%d g=%d luma=%d\n", (int) s0, (int) g0, luma);
    if (afd_aborted) return s0;
    if (s0 < AFD_MIN_S)
    {
        afd_lowcontrast = 1;
        return s0;
    }

    /* 2. direction probe on g: +COARSE; wrong way = back past the start,
     * landing at -COARSE measured in the -1 direction (afd_goto) */
    int d = 1;
    int cmoves = 0;
    uint32_t bg = g0;
    int bgpos = 0;
    afd_move("p", AFD_COARSE);
    cmoves++;
    if (afd_aborted) return 0;
    uint32_t gc = afd_last_g;
    afd_gbest(&bg, &bgpos);
    if (afd_below(gc, g0, AFD_PROBE_PCT))
    {
        d = -1;
        afd_goto("p", -AFD_COARSE, -1, 0);
        cmoves++;
        if (afd_aborted) return 0;
        gc = afd_last_g;
        afd_gbest(&bg, &bgpos);
    }
    else if (!((uint64_t) gc * 100 > (uint64_t) g0 * (200 - AFD_PROBE_PCT)))
    {
        /* within +-5%: one more step the same way; rising = keep going,
         * otherwise go to the other side */
        afd_move("p", AFD_COARSE);
        cmoves++;
        if (afd_aborted) return 0;
        uint32_t g2 = afd_last_g;
        afd_gbest(&bg, &bgpos);
        if ((uint64_t) g2 * 100 > (uint64_t) gc * 105)
            gc = g2;
        else
        {
            d = -1;
            afd_goto("p", -AFD_COARSE, -1, 0);
            cmoves++;
            if (afd_aborted) return 0;
            gc = afd_last_g;
            afd_gbest(&bg, &bgpos);
        }
    }

    /* 3. coarse climb in direction d on g */
    int drops = afd_below(gc, bg, AFD_COARSE_DROP_PCT) ? 1 : 0;
    int flats = 0;
    uint32_t prev = gc;
    int stop = afd_below(gc, bg, AFD_COARSE_STOP_PCT);
    while (!stop && !afd_aborted && cmoves < AFD_MAX_COARSE && afd_travel + AFD_COARSE <= AFD_MAX_TRAVEL)
    {
        afd_move("c", d * AFD_COARSE);
        cmoves++;
        if (afd_aborted) return 0;
        gc = afd_last_g;
        afd_gbest(&bg, &bgpos);
        drops = afd_below(gc, bg, AFD_COARSE_DROP_PCT) ? drops + 1 : 0;
        uint32_t df = gc > prev ? gc - prev : prev - gc;
        flats = (df * 100 < (uint64_t) prev * AFD_FLAT_PCT) ? flats + 1 : 0;
        prev = gc;
        stop = afd_below(gc, bg, AFD_COARSE_STOP_PCT) || drops >= AFD_COARSE_DROP_N || flats >= AFD_FLAT_N;
    }
    afd_peak_g_pos = bgpos;
    afd_log("# coarse peak_g_pos=%d peak_g=%d g0=%d dir=%d moves=%d travel=%d\n",
            bgpos, (int) bg, (int) g0, d, cmoves, afd_travel);

    /* 4. fine sweep on s, one direction only: 9 points, best_g_pos-80 .. +80 */
    int fpos[AFD_FINE_PTS];
    uint32_t fs[AFD_FINE_PTS];
    int n = afd_sweep("f", bgpos - d * AFD_FINE_SPAN, d, AFD_FINE, AFD_FINE_PTS, fpos, fs);
    if (afd_aborted || !n) return 0;
    int im = afd_argmax(fs, n);
    int peak = fpos[im];
    uint32_t fmax = fs[im];
    if (im > 0 && im < n - 1)
    {
        int64_t num = (int64_t) fs[im - 1] - (int64_t) fs[im + 1];
        int64_t den = 2 * ((int64_t) fs[im - 1] - 2 * (int64_t) fs[im] + (int64_t) fs[im + 1]);
        if (den < 0)
        {
            int64_t off = (int64_t) AFD_FINE * num / den;   /* in sweep direction */
            if (off > AFD_FINE) off = AFD_FINE;
            if (off < -AFD_FINE) off = -AFD_FINE;
            peak += d * (int) off;
        }
    }
    afd_fine_peak_pos = peak;
    afd_best_s = fmax;
    afd_best_pos = peak;
    afd_log("# fine max_idx=%d max_pos=%d max_s=%d fine_peak_pos=%d\n", im, fpos[im], (int) fmax, peak);

    /* 5. final approach to the peak, approaching in d */
    uint32_t s = afd_goto("r", peak, d, 0);
    if (afd_aborted) return 0;
    *best_out = fmax;

    /* 6. the final measurement missed the fine max: one finer sweep around the
     * peak (9 points, step 10); its own measurements decide */
    if (afd_below(s, fmax, AFD_RECOVER_PCT))
    {
        afd_recovered = 1;
        afd_log("# recovery final_s=%d fine_max_s=%d center=%d dir=%d\n", (int) s, (int) fmax, peak, d);
        int rpos[AFD_REC_PTS];
        uint32_t rs[AFD_REC_PTS];
        n = afd_sweep("v", peak - d * (AFD_REC_PTS / 2) * AFD_REC_STEP, d, AFD_REC_STEP, AFD_REC_PTS, rpos, rs);
        if (afd_aborted || !n) return 0;
        im = afd_argmax(rs, n);
        afd_log("# recovery new_best_pos=%d new_best_s=%d old_best_s=%d\n", rpos[im], (int) rs[im], (int) fmax);
        afd_best_s = rs[im];
        afd_best_pos = rpos[im];
        s = afd_goto("r", rpos[im], d, 0);
        if (afd_aborted) return 0;
        *best_out = rs[im];
    }
    return s;
}

/* AF drive calib: hysteresis measurement. Go to -160 (approach +), then
 * 8 x +40 (up1: -160..+160), 8 x -40 (down: +160..-160), 8 x +40 (up2),
 * s after each move (a pass also starts with the sample the previous pass ended on).
 * Model: with backlash B, after a reversal the lens first loses B pulses, so
 * going down the true peak is reached at a LOWER commanded pos than going up
 * (actual = cmd + B): backlash_est = peak_up1 - peak_down, positive.
 * Peaks (argmax of s) are kept per pass: 0 up1, 1 down, 2 up2; edge = argmax is
 * the first or last sample of the pass (the true peak may lie outside). */
static int afd_cal_peak_pos[3], afd_cal_peak_idx[3], afd_cal_n[3];
static uint32_t afd_cal_peak_s[3];

static void afd_cal_point(int pass, uint32_t s)
{
    if (s > afd_cal_peak_s[pass])
    {
        afd_cal_peak_s[pass] = s;
        afd_cal_peak_pos[pass] = afd_pos;
        afd_cal_peak_idx[pass] = afd_cal_n[pass];
    }
    afd_cal_n[pass]++;
}

static int afd_cal_edge(int pass)
{
    return afd_cal_peak_idx[pass] == 0 || afd_cal_peak_idx[pass] == afd_cal_n[pass] - 1;
}

static void afd_calib_run(void)
{
    uint32_t g;
    int luma;
    for (int i = 0; i < 3; i++) { afd_cal_peak_pos[i] = 0; afd_cal_peak_s[i] = 0; afd_cal_peak_idx[i] = 0; afd_cal_n[i] = 0; }
    uint32_t s0 = afd_move("i", 0);
    uint32_t s1 = afd_sample(&g, &luma);
    s0 = (s0 + s1) / 2;
    afd_log("# s0=%d g=%d luma=%d\n", (int) s0, (int) g, luma);
    if (afd_aborted) return;
    if (s0 < AFD_MIN_S)
    {
        afd_lowcontrast = 1;
        return;
    }
    static const char * const names[3] = { "u", "d", "u" };
    static const char * const titles[3] = { "up1", "down", "up2" };
    uint32_t last = afd_goto("u", AFD_CAL_START, 1, 0);   /* overshoot leg "o" (unmeasured), then up to the start */
    if (afd_aborted) return;
    for (int pass = 0; pass < 3; pass++)
    {
        afd_log("# pass %s\n", titles[pass]);
        afd_cal_point(pass, last);   /* up1: the sample at the start; later passes: where the last one ended */
        for (int i = 0; i < AFD_CAL_MOVES; i++)
        {
            last = afd_move_ex(names[pass], pass == 1 ? -AFD_CAL_STEP : AFD_CAL_STEP, 1, 0);
            if (afd_aborted) return;
            afd_cal_point(pass, last);
        }
    }
    /* leave the lens in focus: back to the up2 peak (up1 if up2 has none), approaching +1 */
    int rp = afd_cal_peak_s[2] ? afd_cal_peak_pos[2] : afd_cal_peak_pos[0];
    last = afd_goto("r", rp, 1, 0);
    if (afd_aborted) return;
    afd_log("# calib_return pos=%d s=%d\n", afd_pos, (int) last);
}

/* One session for both actions: calib=0 AF drive, calib=1 AF drive calib. */
static void afd_task_common(int calib)
{
    const char *name = calib ? "AF calib MPU" : "AF drive MPU";
    if (afdiag_afdrive_busy || afdiag_afsearch_busy) return;
    afdiag_afdrive_busy = 1;

    if (!is_EOSM || !lv || RECORDING || is_manual_focus() || slim_af_state != SLIM_AF_IDLE)
    {
        NotifyBox(4000, "%s: needs AF mode, LV, idle, not recording", name);
        goto out;
    }

    NotifyBox(3000, "%s: close menu, starts in 2 s", name);
    msleep(1500);
    for (int i = 0; i < 50 && gui_menu_shown(); i++) msleep(100);
    if (!lv || RECORDING || is_manual_focus() || slim_af_state != SLIM_AF_IDLE || gui_menu_shown())
    {
        NotifyBox(3000, "%s: aborted", name);
        goto out;
    }

    afd_len = 0;
    afd_pos = 0;
    afd_moves = 0;
    afd_aborted = 0;
    afd_lowcontrast = 0;
    afd_travel = 0;
    afd_recovered = 0;
    afd_peak_g_pos = afd_fine_peak_pos = 0;
    afd_last_dir = 0;
    afd_best_dir = 0;
    afd_nolv = 0;
    afd_n_nodone = afd_n_err = afd_n_fast = 0;
    afd_t0 = get_ms_clock();
    afd_log_reserve = AFD_LOG_RESERVE;
    afd_luma_pre = afd_luma_now();   /* before MonOpen */
    int mon = afdiag_afdrive_mon;
    int e = 0;
    int opened_now = 0;
    if (afd_mon_is_open && mon != 2)
        afd_mon_close_now();               /* setting changed away from Keep open */
    if (mon && !afd_mon_is_open)
    {
        e = ((int (*)(void)) 0xFF1DD098)();   /* MpuMonOpen */
        if (e)
        {
            NotifyBox(4000, "%s: MonOpen failed %d", name, e);
            goto out;
        }
        opened_now = 1;
        if (mon == 2) afd_mon_is_open = 1;
    }
    static const uint8_t stop = 0;
    static const uint8_t zeros[16] = { 0 };
    static const uint8_t open_s[16] = { 0x02 };
    int (*send)(int,int,int,const void *) = (int (*)(int,int,int,const void *)) 0x38C4;
    afd_log("# %s t=%d lens=\"%s\" zoom=x%d crop_preset=%d dist0=%d monitor=%s opened_now=%d\n",
            calib ? "AFCALIB" : "AFDRIVE", get_ms_clock(), lens_info.name, lv_dispsize, crop_preset_index,
            (int) lens_info.focus_dist, mon == 2 ? "KEEP" : mon ? "ON" : "OFF", opened_now);
    if (calib)
        afd_log("# speed=%d step=%d moves_per_pass=%d max_total=%d done_timeout=%d "
                "min_ms=%d+%d*|n|(cap %d) frame=%d settle=%d\n",
                AFD_SPEED, AFD_CAL_STEP, AFD_CAL_MOVES, AFD_MAX_TOTAL, AFD_DONE_TIMEOUT_MS,
                AFD_MIN_BASE_MS, AFD_MIN_PER_PULSE, AFD_MIN_CAP_MS, AFD_FRAME_MS, AFD_SETTLE_MS);
    else
        afd_log("# speed=%d coarse=%d fine=%d approach=%d probe_pct=%d coarse_stop_pct=%d coarse_drop_pct=%d coarse_drop_n=%d "
                "flat_pct=%d flat_n=%d max_travel=%d max_total=%d max_coarse=%d fine_span=%d fine_pts=%d rec_step=%d "
                "recover_pct=%d done_timeout=%d min_ms=%d+%d*|n|(cap %d) frame=%d settle=%d\n",
                AFD_SPEED, AFD_COARSE, AFD_FINE, AFD_APPROACH, AFD_PROBE_PCT, AFD_COARSE_STOP_PCT,
                AFD_COARSE_DROP_PCT, AFD_COARSE_DROP_N, AFD_FLAT_PCT, AFD_FLAT_N, AFD_MAX_TRAVEL, AFD_MAX_TOTAL, AFD_MAX_COARSE,
                AFD_FINE_SPAN, AFD_FINE_PTS, AFD_REC_STEP, AFD_RECOVER_PCT, AFD_DONE_TIMEOUT_MS, AFD_MIN_BASE_MS,
                AFD_MIN_PER_PULSE, AFD_MIN_CAP_MS, AFD_FRAME_MS, AFD_SETTLE_MS);
    afd_log("# ms phase cmd pos s g luma done_val flags done_ms ret dist\n");
    afd_luma_probe("pre_monopen_or_session");

    int r_open = send(9, 0x25, 16, open_s);
    afd_luma_probe("post_session_open");
    msleep(300);
    afd_luma_probe("post_session_open_300ms");
    afd_log("# session_open ret=%d\n", r_open);

    uint32_t best = 0, final_s = 0;
    int pct = 0;
    if (!afd_wait_lv(opened_now))
    {
        if (!afd_aborted) afd_nolv = 1;
    }
    else if (calib) afd_calib_run();
    else
    {
        final_s = afd_run(&best);
        if (!best) best = afd_best_s;
        pct = best ? (int) ((uint64_t) final_s * 100 / best) : 0;
    }
    int total_ms = get_ms_clock() - afd_t0;

    /* always: stop, close the session, close the monitor */
    send(9, 3, 1, &stop);
    msleep(500);
    send(9, 0x25, 16, zeros);
    msleep(1000);
    if (mon == 1) ((int (*)(void)) 0xFF1DD118)();   /* MpuMonClose */
    /* mon == 2: left open for the next run (closed on LV exit / menu / setting) */

    afd_log_reserve = 0;
    const char *ab = afd_nolv ? "nolv" : afd_lowcontrast ? "lowcontrast" : afd_aborted ? "1" : "0";
    if (calib)
        afd_log("# RESULT aborted=%s peak_up1=%d peak_up1_s=%d peak_down=%d peak_down_s=%d peak_up2=%d peak_up2_s=%d "
                "edge_up1=%d edge_down=%d edge_up2=%d backlash_est=%d repeat_up=%d moves=%d n_nodone=%d n_err=%d n_fast=%d total_ms=%d end_pos=%d\n\n",
                ab, afd_cal_peak_pos[0], (int) afd_cal_peak_s[0], afd_cal_peak_pos[1], (int) afd_cal_peak_s[1],
                afd_cal_peak_pos[2], (int) afd_cal_peak_s[2], afd_cal_edge(0), afd_cal_edge(1), afd_cal_edge(2),
                afd_cal_peak_pos[0] - afd_cal_peak_pos[1],
                afd_cal_peak_pos[2] - afd_cal_peak_pos[0], afd_moves, afd_n_nodone, afd_n_err, afd_n_fast, total_ms, afd_pos);
    else
        afd_log("# RESULT aborted=%s best_pos=%d best_s=%d peak_g_pos=%d fine_peak_pos=%d final_s=%d final_best_pct=%d recovery=%d moves=%d n_nodone=%d n_err=%d n_fast=%d total_ms=%d end_pos=%d\n\n",
                ab, afd_best_pos, (int) best, afd_peak_g_pos, afd_fine_peak_pos, (int) final_s, pct, afd_recovered, afd_moves,
                afd_n_nodone, afd_n_err, afd_n_fast, total_ms, afd_pos);

    if (!(RECORDING || RECORDING_RAW || mlv_raw_rec_busy()))
    {
        if (!is_dir(AFDIAG_LOG_DIR))
            FIO_CreateDirectory(AFDIAG_LOG_DIR);
        FILE *f = FIO_CreateFileOrAppend(calib ? AFC_LOG_FILE : AFD_LOG_FILE);
        if (f)
        {
            FIO_WriteFile(f, afd_buf, afd_len);
            FIO_CloseFile(f);
        }
    }
    else
    {
        NotifyBox(3000, "%s: log NOT saved (recording)", name);
        msleep(3000);
    }

    if (calib)
        NotifyBox(6000, "AF calib %s: peak up1=%d down=%d up2=%d backlash_est=%d",
                  afd_aborted ? "ABORTED" : "done", afd_cal_peak_pos[0], afd_cal_peak_pos[1],
                  afd_cal_peak_pos[2], afd_cal_peak_pos[0] - afd_cal_peak_pos[1]);
    else
        NotifyBox(6000, "AF drive %s: final/best pct=%d moves=%d %dms",
                  afd_aborted ? "ABORTED" : "done", pct, afd_moves, total_ms);

out:
    afdiag_afdrive_busy = 0;
}

static void afdiag_afdrive_task(void *unused)
{
    (void) unused;
    afd_task_common(0);
}

static void afdiag_afcalib_task(void *unused)
{
    (void) unused;
    afd_task_common(1);
}

static MENU_SELECT_FUNC(afdiag_afdrive_select)
{
    (void) priv;
    (void) delta;
    task_create("afdiag_afdrive", 0x1e, 0x3000, afdiag_afdrive_task, (void*)0);
}

static MENU_SELECT_FUNC(afdiag_afcalib_select)
{
    (void) priv;
    (void) delta;
    task_create("afdiag_afcalib", 0x1e, 0x3000, afdiag_afcalib_task, (void*)0);
}

static void afd_mon_close_task(void *unused)
{
    (void) unused;
    if (!afdiag_afdrive_busy && !afdiag_afsearch_busy)
        afd_mon_close_now();
    afd_mon_closing = 0;
}

static MENU_SELECT_FUNC(afdiag_afdrive_monclose_select)
{
    (void) priv;
    (void) delta;
    if (!afd_mon_is_open)
    {
        NotifyBox(2000, "MPU monitor is not open");
        return;
    }
    if (afd_mon_closing) return;
    afd_mon_closing = 1;
    task_create("afd_monclose", 0x1e, 0x1000, afd_mon_close_task, (void*)0);
    NotifyBox(2000, "MPU monitor closed");
}

static struct menu_entry afdiag_menu[] = {
    {
        .name      = "AF diag log",
        .priv      = &afdiag_log,
        .max       = 2,
        .choices   = CHOICES("OFF", "ON", "ON + MPU"),
        .edit_mode = EM_INLINE_ADJUST,
        .icon_type = IT_DICE,
        .help      = "TEMPORARY diagnostics: log AF properties to ML/LOGS/AFDIAG.LOG.",
        .help2     = "ON + MPU also logs messages Canon sends to the MPU (lens/AF controller).\n"
                     "Always OFF after restart. Written to the card only when not recording.",
    },
    {
        .name      = "AF lens test",
        .priv      = &afdiag_lens_test,
        .max       = 4,
        .choices   = CHOICES("OFF", "Remote", "A/B", "MPU", "MPU+SW1"),
        .edit_mode = EM_INLINE_ADJUST,
        .icon_type = IT_DICE,
        .help      = "TEMPORARY: move the focus motor back and forth in LV.",
        .help2     = "Remote: 30 commands, size 3/15, gaps 0/40/100 ms (movie only). A/B: lens_focus 40 x size 1, 2, 3, each way, photo or movie.\n"
                     "MPU: one-shot series of raw MPU messages (needs AF diag log ON + MPU, LV, not recording, "
                     "close the menu); MPU+SW1 adds a remote half-press (Canon runs AF). OFF after restart.",
    },
    {
        .name      = "AF search (MPU)",
        .select    = afdiag_afsearch_select,
        .icon_type = IT_DICE,
        .help      = "EXPERIMENTAL: run the MPU AF search once, stop at sharpness peak.",
        .help2     = "Sends the confirmed (8,2,32) index-16 message and samples sharpness in the centre;\n"
                     "stops with (9,3,1) when sharpness passes its peak on the far leg (max 3 s). Needs AF mode,\n"
                     "LV, not recording; refuses at infinity. Always traced to ML/LOGS/AFSEARCH.LOG.",
    },
    {
        .name      = "AF drive (MPU)",
        .select    = afdiag_afdrive_select,
        .icon_type = IT_DICE,
        .help      = "EXPERIMENTAL: one-shot contrast-detect AF with MPU relative lens moves.",
        .help2     = "Probes the direction, climbs in coarse steps while the centre sharpness rises, returns to the\n"
                     "best spot, then refines in fine steps. Needs AF mode, LV, not recording; close the menu.\n"
                     "Aborts if recording starts or the menu opens. Traced to ML/LOGS/AFDRIVE.LOG.",
    },
    {
        .name      = "AF drive monitor",
        .priv      = &afdiag_afdrive_mon,
        .max       = 2,
        .choices   = CHOICES("OFF", "ON", "Keep open"),
        .icon_type = IT_DICE,
        .help      = "AF drive (MPU): the MPU monitor must be open for the lens to move.",
        .help2     = "Keep open (default): opened on the first run (LV dark ~2 s once), then left open so later runs\n"
                     "start at once; closed when LV ends or via 'AF drive: close monitor'. ON: open/close every run\n"
                     "(dark ~2 s each time). OFF does NOT drive the lens (MPU rejects moves with 0x4A).",
    },
    {
        .name      = "AF drive: close monitor",
        .select    = afdiag_afdrive_monclose_select,
        .icon_type = IT_ACTION,
        .help      = "Close the MPU monitor left open by 'AF drive monitor: Keep open'.",
    },
    {
        .name      = "AF drive calib",
        .select    = afdiag_afcalib_select,
        .icon_type = IT_DICE,
        .help      = "EXPERIMENTAL: measure the focus motor backlash (-160..+160 pulses up, down, up).",
        .help2     = "Run it AFTER AF drive (lens near focus). Same conditions as AF drive (MPU). Sharpness after every move goes to ML/LOGS/AFCALIB.LOG;\n"
                     "the backlash is the peak offset (up minus down).",
    },
};

/* 1x1 Aspect Ratio UI: 0=2.33:1, 1=2.35:1, 2=16:9, 3=3:2, 4=4:3 */
static int slim_1x1_ar = 2; /* default 16:9 */
static const char * const slim_1x1_ar_labels[5] = {
    "2.33:1", "2.35:1", "16:9", "3:2", "4:3"
};

static void slim_crop_apply_mode(void);
static void slim_crop_apply_unified_preset(void);
static void slim_crop_clamp_fps(void);

/* How many Preset choices are selectable right now (1 → row should be greyed). */
static int slim_preset_choice_count(void)
{
    if (slim_mode_ui == 3)
        return 1; /* LV: Highest only */
    if (slim_mode_ui == 2)
        return 1; /* 3x3: Highest only — one res per Aspect Ratio */
    if (slim_mode_ui == 0)
    {
        /* 1x1: only 2.35:1 has Higher + Highest */
        return (slim_1x1_ar == 1) ? 2 : 1;
    }
    /* 1x3: Highest / Higher / Medium */
    return 3;
}

/* Map 3x3 Aspect Ratio → backend High FPS (0) or mv1080 3:2 (2). */
static void slim_crop_apply_3x3_from_ar(void)
{
    int ar = COERCE(crop_preset_ar_menu, 0, 4);
    slim_unified_preset = 0;
    if (ar == 4)
    {
        /* 3:2 → mv1080_3_2 @ 23.976/25/30 */
        crop_preset_3x3_res_menu = 2;
    }
    else
    {
        /* 16:9 / 2:1 / 2.20 / 2.35 → High FPS presets */
        crop_preset_3x3_res_menu = 0;
        crop_preset_fps_reduce = 0; /* full High FPS timers */
    }
}

/* Map 1x1 AR (+ Preset for 2.35:1) → res index, WxH, FPS mask. */
static void slim_1x1_resolve(int *res_idx, int *w, int *h, int *fps_mask)
{
    slim_1x1_ar = COERCE(slim_1x1_ar, 0, 4);

    if (slim_1x1_ar == 0)
    {
        /* 2.33:1 → 2520x1080 @ 24/25/30 — Highest only */
        *res_idx = 0;
        *w = 2520; *h = 1080;
        *fps_mask = 0x7;
        slim_unified_preset = 0;
    }
    else if (slim_1x1_ar == 1)
    {
        /* 2.35:1 — Higher=2880x1226 @24/25; Highest=3072x1308 @24 */
        if (slim_unified_preset > 1)
            slim_unified_preset = 0;
        if (slim_unified_preset == 1)
        {
            *res_idx = 1; /* 2.8K Higher */
            *w = 2880; *h = 1226;
            *fps_mask = 0x3;
        }
        else
        {
            slim_unified_preset = 0;
            *res_idx = 2; /* 3K Highest */
            *w = 3072; *h = 1308;
            *fps_mask = 0x1;
        }
    }
    else if (slim_1x1_ar == 2)
    {
        /* 16:9 → 2560x1440 @ 24/25 — Highest only */
        *res_idx = 3;
        *w = 2560; *h = 1440;
        *fps_mask = 0x3;
        slim_unified_preset = 0;
    }
    else if (slim_1x1_ar == 3)
    {
        /* 3:2 → 1920x1280 @ 24/25 — Highest only */
        *res_idx = 4;
        *w = 1920; *h = 1280;
        *fps_mask = 0x3;
        slim_unified_preset = 0;
    }
    else
    {
        /* 4:3 → 2160x1620 @ 23.943 FPS (dannephoto CROP_1620p; single TimerB) — Highest only */
        *res_idx = 6;
        *w = 2160; *h = 1620;
        *fps_mask = 0x1;
        slim_unified_preset = 0;
    }
}

static void slim_crop_sync_from_backend(void)
{
    if (CROP_PRESET_MENU == CROP_PRESET_1X1 && crop_preset_1x1_res_menu == 5)
    {
        slim_mode_ui = 3; /* LV */
        slim_unified_preset = 0;
    }
    else if (CROP_PRESET_MENU == CROP_PRESET_1X1)
    {
        slim_mode_ui = 0;
        switch (crop_preset_1x1_res_menu)
        {
            case 0: /* 2.5K */
                slim_1x1_ar = 0;
                slim_unified_preset = 0;
                break;
            case 1: /* 2.8K Higher */
                slim_1x1_ar = 1;
                slim_unified_preset = 1;
                break;
            case 2: /* 3K Highest */
                slim_1x1_ar = 1;
                slim_unified_preset = 0;
                break;
            case 3: /* 1440p */
                slim_1x1_ar = 2;
                slim_unified_preset = 0;
                break;
            case 4: /* 1280p */
                slim_1x1_ar = 3;
                slim_unified_preset = 0;
                break;
            case 6: /* 1620p 4:3 */
                slim_1x1_ar = 4;
                slim_unified_preset = 0;
                break;
            default:
                slim_1x1_ar = 2;
                slim_unified_preset = 0;
                break;
        }
    }
    else if (CROP_PRESET_MENU == CROP_PRESET_1X3)
    {
        slim_mode_ui = 1;
        slim_unified_preset = COERCE(crop_preset_1x3_res_menu, 0, 2);
    }
    else if (CROP_PRESET_MENU == CROP_PRESET_3X3)
    {
        slim_mode_ui = 2;
        slim_unified_preset = 0;
        /* Keep AR; backend res comes from slim_crop_apply_3x3_from_ar. */
    }

    if (OUTPUT_10BIT || OUTPUT_11BIT) slim_bit_depth_ui = 0;
    else if (OUTPUT_12BIT) slim_bit_depth_ui = 1;
    else slim_bit_depth_ui = 2; /* 14-bit */
}

static void slim_crop_apply_unified_preset(void)
{
    if (slim_mode_ui == 0)
    {
        /* Apply via slim_1x1_resolve in slim_crop_apply_mode. */
        return;
    }
    if (slim_mode_ui == 2)
    {
        slim_crop_apply_3x3_from_ar();
        return;
    }
    slim_unified_preset = COERCE(slim_unified_preset, 0, 2);
    if (slim_mode_ui == 1 || CROP_PRESET_MENU == CROP_PRESET_1X3)
        crop_preset_1x3_res_menu = slim_unified_preset;
}

static void slim_crop_apply_mode(void)
{
    slim_mode_ui = COERCE(slim_mode_ui, 0, 3);

    if (slim_mode_ui == 3)
    {
        /* LV → 1x1 Full-Res backend @ 5208x3478 */
        crop_preset_index = 1;
        crop_preset_1x1_res_menu = 5;
        slim_unified_preset = 0;
    }
    else
    {
        crop_preset_index = slim_mode_ui + 1; /* 1x1 / 1x3 / 3x3 */
        if (slim_mode_ui == 0)
        {
            int res_idx, w, h, fps_mask;
            slim_1x1_resolve(&res_idx, &w, &h, &fps_mask);
            crop_preset_1x1_res_menu = res_idx;
            (void)w; (void)h; (void)fps_mask;
        }
        else
            slim_crop_apply_unified_preset();
    }
    slim_crop_clamp_fps();
}

static void slim_crop_apply_bit_depth(void)
{
    static const int map[] = { 3, 1, 0 }; /* 10, 12, 14 */
    int prev = bit_depth_analog;
    slim_bit_depth_ui = COERCE(slim_bit_depth_ui, 0, 2);
    bit_depth_analog = map[slim_bit_depth_ui];
    if (bit_depth_analog != prev)
        raw_invalidate_lv_calibration();
}

/* Expected RAW WxH for EOS M (from crop_rec help / reg_override). */
static void slim_crop_expected_res(int *w, int *h)
{
    *w = 1376;
    *h = 2322; /* default Higher 1x3 16:9 */

    if (slim_mode_ui == 3 || (CROP_PRESET_MENU == CROP_PRESET_1X1 && crop_preset_1x1_res_menu == 5))
    {
        *w = 5208;
        *h = 3478;
        return;
    }

    if (CROP_PRESET_MENU == CROP_PRESET_OFF)
        return;

    if (CROP_PRESET_MENU == CROP_PRESET_1X3)
    {
        int p = COERCE(crop_preset_1x3_res_menu, 0, 2);
        int ar = crop_preset_ar_menu;
        if (ar == 0) { /* 16:9 */
            if (p == 0) { *w = 1504; *h = 2538; }
            else if (p == 1) { *w = 1376; *h = 2322; }
            else { *w = 1280; *h = 2160; }
        } else if (ar == 1) { /* 2:1 */
            if (p == 0) { *w = 1600; *h = 2400; }
            else if (p == 1) { *w = 1472; *h = 2208; }
            else { *w = 1360; *h = 2040; }
        } else if (ar == 2) { /* 2.20:1 */
            if (p == 0) { *w = 1664; *h = 2268; }
            else if (p == 1) { *w = 1552; *h = 2216; }
            else { *w = 1424; *h = 1942; }
        } else if (ar == 3) { /* 2.35:1 */
            if (p == 0) { *w = 1736; *h = 2214; }
            else if (p == 1) { *w = 1600; *h = 2040; }
            else { *w = 1472; *h = 1878; }
        } else { /* 2.39:1 */
            if (p == 0) { *w = 1736; *h = 2178; }
            else if (p == 1) { *w = 1600; *h = 2008; }
            else { *w = 1472; *h = 1846; }
        }
        return;
    }

    if (CROP_PRESET_MENU == CROP_PRESET_1X1)
    {
        int res_idx, fps_mask;
        slim_1x1_resolve(&res_idx, w, h, &fps_mask);
        (void)res_idx; (void)fps_mask;
        return;
    }

    if (CROP_PRESET_MENU == CROP_PRESET_3X3)
    {
        /* One resolution per Aspect Ratio (Preset always Highest / High FPS or 3:2). */
        int ar = COERCE(crop_preset_ar_menu, 0, 4);
        if (ar == 0) { *w = 1736; *h = 976; }
        else if (ar == 1) { *w = 1736; *h = 868; }
        else if (ar == 2) { *w = 1736; *h = 790; }
        else if (ar == 3) { *w = 1736; *h = 738; }
        else { *w = 1736; *h = 1160; } /* 3:2 */
        return;
    }
}

/* Bit0=23.976 Bit1=25 Bit2=30 — rates allowed for current Mode/AR/Preset on EOS M.
 * LV: return 0 (handled specially as 3 fps). */
static int slim_crop_fps_mask(void)
{
    if (slim_mode_ui == 3 || (CROP_PRESET_MENU == CROP_PRESET_1X1 && crop_preset_1x1_res_menu == 5))
        return 0;

    if (CROP_PRESET_MENU == CROP_PRESET_OFF)
        return 0x1;

    if (CROP_PRESET_MENU == CROP_PRESET_1X3)
    {
        int p = COERCE(crop_preset_1x3_res_menu, 0, 2);
        int ar = crop_preset_ar_menu;
        if (p == 0) return 0x1;                 /* Highest: fixed ~24 only */
        if (p == 1 && ar == 0) return 0x1;       /* Higher 16:9: 23.976 only */
        return 0x1 | 0x2;                       /* Higher/Medium elsewhere: 24+25 */
    }

    if (CROP_PRESET_MENU == CROP_PRESET_1X1)
    {
        int res_idx, w, h, fps_mask;
        slim_1x1_resolve(&res_idx, &w, &h, &fps_mask);
        (void)res_idx; (void)w; (void)h;
        return fps_mask;
    }

    if (CROP_PRESET_MENU == CROP_PRESET_3X3)
    {
        int ar = COERCE(crop_preset_ar_menu, 0, 4);
        if (ar == 4)
            return 0x1 | 0x2 | 0x4; /* 3:2 → 23.976 / 25 / 30 */
        return 0; /* High FPS AR: single fixed rate (shown specially) */
    }

    return 0x1;
}

static void slim_crop_clamp_fps(void)
{
    int mask = slim_crop_fps_mask();
    if (!mask)
        return; /* LV @ 3 fps — no 24/25/30 index */
    if (mask & (1 << crop_preset_fps_menu))
        return;
    for (int i = 0; i < 3; i++)
    {
        if (mask & (1 << i))
        {
            crop_preset_fps_menu = i;
            return;
        }
    }
    crop_preset_fps_menu = 0;
}

static MENU_SELECT_FUNC(slim_crop_mode_select)
{
    slim_mode_ui = MOD(COERCE(slim_mode_ui, 0, 3) + delta, 4);
    slim_crop_apply_mode();
}

static MENU_UPDATE_FUNC(slim_crop_mode_update)
{
    slim_mode_ui = COERCE(slim_mode_ui, 0, 3);
}

static MENU_SELECT_FUNC(slim_crop_preset_select)
{
    int n = slim_preset_choice_count();
    if (n <= 1)
        return;

    /* Both L and R: Medium → Higher → Highest → Medium… (never reverse).
     * For 1x1 2.35:1 (2 choices): Higher → Highest → Higher… */
    (void)delta;
    if (n == 2)
    {
        slim_unified_preset = (slim_unified_preset == 1) ? 0 : 1;
        slim_crop_apply_mode();
        return;
    }

    if (slim_unified_preset == 2)
        slim_unified_preset = 1;      /* Medium → Higher */
    else if (slim_unified_preset == 1)
        slim_unified_preset = 0;      /* Higher → Highest */
    else
        slim_unified_preset = 2;      /* Highest → Medium */

    slim_crop_apply_unified_preset();
    slim_crop_clamp_fps();
}

static MENU_UPDATE_FUNC(slim_crop_preset_update)
{
    slim_crop_sync_from_backend();

    if (slim_mode_ui == 0 && slim_1x1_ar == 1)
    {
        /* Keep only Highest / Higher for 2.35:1 */
        if (slim_unified_preset > 1)
            slim_unified_preset = 0;
        MENU_SET_VALUE("%s", slim_unified_preset == 1 ? "Higher" : "Highest");
        MENU_SET_ENABLED(1);
        return;
    }

    if (slim_preset_choice_count() <= 1)
    {
        slim_unified_preset = 0;
        MENU_SET_VALUE("Highest");
        MENU_SET_ENABLED(0); /* locked — greyed */
        return;
    }

    MENU_SET_VALUE("%s",
        slim_unified_preset == 0 ? "Highest" :
        slim_unified_preset == 1 ? "Higher" : "Medium");
    MENU_SET_ENABLED(1);
}

static MENU_UPDATE_FUNC(slim_crop_ar_update)
{
    if (slim_mode_ui == 3)
    {
        MENU_SET_VALUE("3:2");
        MENU_SET_ENABLED(0); /* LV: Aspect locked */
        return;
    }
    if (slim_mode_ui == 0)
    {
        slim_1x1_ar = COERCE(slim_1x1_ar, 0, 4);
        MENU_SET_VALUE("%s", slim_1x1_ar_labels[slim_1x1_ar]);
        MENU_SET_ENABLED(1);
        return;
    }
    if (slim_mode_ui == 2)
    {
        /* 3x3: last option is 3:2 (not 2.39:1). */
        static const char * labels_3x3[] = {
            "16:9", "2:1", "2.20:1", "2.35:1", "3:2"
        };
        MENU_SET_VALUE("%s", labels_3x3[COERCE(crop_preset_ar_menu, 0, 4)]);
        MENU_SET_ENABLED(1);
        return;
    }
    MENU_SET_ENABLED(1);
}

static MENU_SELECT_FUNC(slim_crop_ar_select)
{
    if (slim_mode_ui == 3)
        return; /* LV: Aspect fixed 3:2 */

    if (slim_mode_ui == 0)
    {
        slim_1x1_ar = MOD(slim_1x1_ar + delta, 5);
        /* Entering 2.35:1 defaults to Higher unless already Highest/Higher. */
        if (slim_1x1_ar == 1 && slim_unified_preset > 1)
            slim_unified_preset = 1;
        if (slim_1x1_ar != 1)
            slim_unified_preset = 0;
        slim_crop_apply_mode();
        return;
    }

    menu_numeric_toggle(&crop_preset_ar_menu, delta, 0, 4);
    if (slim_mode_ui == 2)
        slim_crop_apply_3x3_from_ar();
    slim_crop_clamp_fps();
}

static MENU_UPDATE_FUNC(slim_crop_res_update)
{
    int w, h;
    slim_crop_sync_from_backend();
    slim_crop_expected_res(&w, &h);
    MENU_SET_VALUE("%dx%d", w, h);
    /* Read-only: greyed via enabled=0 */
    MENU_SET_ENABLED(0);
}

/* Quick Screen resolution stays within the current Aspect Ratio. Aspect
 * Ratio is changed only by its own control, so resolution arrows never
 * expose a temporary cross-aspect combination. */
static MENU_SELECT_FUNC(slim_crop_quick_res_select)
{
    int choices;

    slim_crop_sync_from_backend();
    if (slim_mode_ui == 3)
        return; /* Full-Res LV has one fixed resolution. */

    choices = slim_preset_choice_count();
    slim_unified_preset = COERCE(slim_unified_preset, 0, choices - 1);

    /* Up moves toward higher resolution; down toward lower resolution.
     * Wrap inside this Aspect Ratio, never into an adjacent one. */
    slim_unified_preset = MOD(
        slim_unified_preset + (delta > 0 ? -1 : 1), choices);

    if (slim_mode_ui == 0)
        slim_crop_apply_mode();
    else if (slim_mode_ui == 1)
        slim_crop_apply_unified_preset();
    else
        slim_crop_apply_3x3_from_ar();
    slim_crop_clamp_fps();
}

static MENU_UPDATE_FUNC(slim_crop_quick_res_update)
{
    int w, h;
    slim_crop_sync_from_backend();
    slim_crop_expected_res(&w, &h);
    MENU_SET_VALUE("%dx%d", w, h);
    MENU_SET_ENABLED(slim_mode_ui != 3 && slim_preset_choice_count() > 1);
}

/* The direct Live View editor has no Aspect Ratio item of its own. Its
 * Resolution arrows therefore cycle complete, known-good geometry pairs for
 * the selected mode. Keep Quick Screen's separate selector constrained to
 * its Aspect Ratio as designed. */
static void slim_crop_touch_res_select(int delta)
{
    slim_crop_sync_from_backend();
    if (slim_mode_ui == 3)
        return;

    if (slim_mode_ui == 0)
    {
        static const int res_list[] = { 0, 1, 2, 3, 4, 6 };
        int pos = 0;
        for (int i = 0; i < COUNT(res_list); i++)
            if (crop_preset_1x1_res_menu == res_list[i])
                pos = i;
        pos = MOD(pos + (delta > 0 ? -1 : 1), COUNT(res_list));
        crop_preset_1x1_res_menu = res_list[pos];
        slim_crop_sync_from_backend();
    }
    else if (slim_mode_ui == 1)
    {
        /* Five Aspect Ratios, each with Highest / Higher / Medium. */
        int pos = COERCE(crop_preset_ar_menu, 0, 4) * 3 +
                  COERCE(crop_preset_1x3_res_menu, 0, 2);
        pos = MOD(pos + (delta > 0 ? -1 : 1), 15);
        crop_preset_ar_menu = pos / 3;
        crop_preset_1x3_res_menu = pos % 3;
        slim_unified_preset = crop_preset_1x3_res_menu;
    }
    else /* 3x3: one supported resolution per Aspect Ratio */
    {
        crop_preset_ar_menu = MOD(crop_preset_ar_menu +
                                  (delta > 0 ? -1 : 1), 5);
        slim_crop_apply_3x3_from_ar();
    }

    slim_crop_clamp_fps();
}

static MENU_SELECT_FUNC(slim_crop_fps_select)
{
    if (slim_mode_ui == 3)
        return; /* LV: 3 fps only */

    int mask = slim_crop_fps_mask();
    int bits = (mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1);
    if (bits <= 1)
        return;

    int pos = crop_preset_fps_menu;
    for (int step = 0; step < 3; step++)
    {
        pos = MOD(pos + delta, 3);
        if (mask & (1 << pos))
        {
            crop_preset_fps_menu = pos;
            return;
        }
    }
}

static MENU_UPDATE_FUNC(slim_crop_fps_update)
{
    slim_crop_clamp_fps();

    if (slim_mode_ui == 3 || (CROP_PRESET_MENU == CROP_PRESET_1X1 && crop_preset_1x1_res_menu == 5))
    {
        MENU_SET_VALUE("3");
        MENU_SET_ENABLED(0);
        return;
    }

    /* 3x3 High FPS: one fixed rate per Aspect Ratio. */
    if (CROP_PRESET_MENU == CROP_PRESET_3X3)
    {
        int ar = COERCE(crop_preset_ar_menu, 0, 4);
        if (ar < 4)
        {
            static const char * hfr[] = { "46.800", "50", "54", "55.6" };
            MENU_SET_VALUE("%s", hfr[ar]);
            MENU_SET_ENABLED(0);
            return;
        }
        /* ar == 4 (3:2): fall through to 23.976 / 25 / 30 */
    }

    /* 1x1 4:3 2160x1620 — single fixed rate (TimerB 0x838); label 23.943. */
    if (CROP_PRESET_MENU == CROP_PRESET_1X1 && crop_preset_1x1_res_menu == 6)
    {
        MENU_SET_VALUE("23.943");
        MENU_SET_ENABLED(0);
        return;
    }

    /* EOS M 1x3 Highest 16:9 runs at 22.250, not 23.976. */
    if (CROP_PRESET_MENU == CROP_PRESET_1X3
        && COERCE(crop_preset_1x3_res_menu, 0, 2) == 0
        && crop_preset_ar_menu == 0)
    {
        MENU_SET_VALUE("22.250");
        MENU_SET_ENABLED(0);
        return;
    }

    /* EOS M 1x3 Highest 2:1 (1600x2400) runs at 23.300, not 23.976. */
    if (CROP_PRESET_MENU == CROP_PRESET_1X3
        && COERCE(crop_preset_1x3_res_menu, 0, 2) == 0
        && crop_preset_ar_menu == 1
        && (is_EOSM || is_100D))
    {
        MENU_SET_VALUE("23.300");
        MENU_SET_ENABLED(0);
        return;
    }

    static const char * labels[] = { "23.976", "25", "30" };
    MENU_SET_VALUE("%s", labels[COERCE(crop_preset_fps_menu, 0, 2)]);

    /* Only one valid rate → show it greyed (read-only). */
    int mask = slim_crop_fps_mask();
    int bits = (mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1);
    if (bits <= 1)
        MENU_SET_ENABLED(0);
}

static MENU_SELECT_FUNC(slim_crop_bit_select)
{
    /* Direct-touch arrows and menu L/R move in opposite directions:
     * 10 <-> 12 <-> 14, wrapping at the ends. */
    slim_bit_depth_ui = MOD(slim_bit_depth_ui + (delta < 0 ? -1 : 1), 3);
    slim_crop_apply_bit_depth();
}

static MENU_UPDATE_FUNC(slim_crop_bit_update)
{
    slim_crop_sync_from_backend();
    MENU_SET_VALUE("%s",
        slim_bit_depth_ui == 0 ? "10 Bit" :
        slim_bit_depth_ui == 1 ? "12 Bit" : "14 Bit");
    /* Never gate Bit Depth on lossless / other settings. */
}

/* Called by the core Live View touch editor.  This intentionally bypasses
 * menu_entry lookup and menu semaphores: the menu task is not active while
 * the camera is displaying Live View, and touching these fields must not
 * enter Canon's menu lock path (Err70 on EOS M). */
/* These entry points are called from core through MODULE_FUNCTION().  Keep
 * them in the module image even though no in-module caller references them;
 * otherwise section garbage collection can discard the exports and the core
 * pointer silently falls back to the weak stub (rendering "--" in the editor).
 */
__attribute__((used, noinline))
int crop_rec_touch_adjust(int control, int delta)
{
    int old_irq = 0;
    int now;

    if (!is_movie_mode() || RECORDING)
        return 0;

    if (control == 0 || control == 1)
    {
        now = get_ms_clock();
        if ((int)(now - slim_touch_crop_ready_at) < 0)
            return 0;

        /* The crop backend reads these configuration words from frame-time
         * callbacks.  Publish mode/AR/resolution as one atomic state so it
         * can never observe a new AR paired with the previous resolution. */
        old_irq = cli();
    }

    switch (control)
    {
        case 0:
            /* Direct Live View editor intentionally offers only 1x1/1x3/3x3.
             * Full-Res LV remains available in the regular Movie menu. */
            slim_crop_sync_from_backend();
            slim_mode_ui = MOD(COERCE(slim_mode_ui, 0, 2) + delta, 3);
            slim_crop_apply_mode();
            break;
        case 1: slim_crop_touch_res_select(delta); break;
        case 2: slim_crop_fps_select(0, delta); break;
        case 3: slim_crop_bit_select(0, delta); break;
        default:
            if (control == 0 || control == 1)
                sei(old_irq);
            return 0;
    }

    if (control == 0 || control == 1)
    {
        sei(old_irq);
        slim_touch_crop_ready_at = get_ms_clock() + SLIM_TOUCH_CROP_SETTLE_MS;
        raw_set_dirty();
    }
    return 1;
}

__attribute__((used, noinline))
int crop_rec_touch_get_value(int control, int slot, char *value, int size,
                             int *enabled_out)
{
    int enabled = 1;
    int w, h;

    if (!value || size <= 0 || !enabled_out)
        return 0;

    value[0] = '\0';
    slim_crop_sync_from_backend();

    if (control == 0)
    {
        if (slot == 0)
        {
            snprintf(value, size, "%s",
                slim_mode_ui == 0 ? "1x1" :
                slim_mode_ui == 1 ? "1x3" :
                slim_mode_ui == 2 ? "3x3" : "LV");
        }
        else
        {
            slim_crop_expected_res(&w, &h);
            snprintf(value, size, "%dx%d", w, h);
            enabled = slim_mode_ui != 3;
        }
    }
    else if (control == 1)
    {
        if (slim_mode_ui == 3)
        {
            snprintf(value, size, "3");
            enabled = 0;
        }
        else
        {
            int mask = slim_crop_fps_mask();
            int bits = (mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1);
            if (CROP_PRESET_MENU == CROP_PRESET_3X3 && crop_preset_ar_menu < 4)
            {
                static const char *hfr[] = { "46.800", "50", "54", "55.6" };
                snprintf(value, size, "%s", hfr[COERCE(crop_preset_ar_menu, 0, 3)]);
            }
            else
            {
                static const char *labels[] = { "23.976", "25", "30" };
                snprintf(value, size, "%s", labels[COERCE(crop_preset_fps_menu, 0, 2)]);
            }
            enabled = bits > 1;
        }
    }
    else if (control == 2)
    {
        snprintf(value, size, "%s",
            slim_bit_depth_ui == 0 ? "10 Bit" :
            slim_bit_depth_ui == 1 ? "12 Bit" : "14 Bit");
    }
    else
    {
        return 0;
    }

    *enabled_out = enabled;
    return 1;
}

/* Keep the closest supported aspect ratio when Custom changes Mode.  Most
 * ratios map exactly; nearest-match is only used when the destination mode
 * does not offer the source ratio (for example 4:3 when leaving 1x1). */
static int slim_crop_current_ratio_x1000(void)
{
    static const int ratios_1x1[] = { 2330, 2350, 1778, 1500, 1333 };
    static const int ratios_1x3[] = { 1778, 2000, 2200, 2350, 2390 };
    static const int ratios_3x3[] = { 1778, 2000, 2200, 2350, 1500 };

    if (slim_mode_ui == 0)
        return ratios_1x1[COERCE(slim_1x1_ar, 0, 4)];
    if (slim_mode_ui == 2)
        return ratios_3x3[COERCE(crop_preset_ar_menu, 0, 4)];
    return ratios_1x3[COERCE(crop_preset_ar_menu, 0, 4)];
}

static void slim_crop_set_nearest_ratio(int mode, int ratio_x1000)
{
    static const int ratios_1x1[] = { 2330, 2350, 1778, 1500, 1333 };
    static const int ratios_1x3[] = { 1778, 2000, 2200, 2350, 2390 };
    static const int ratios_3x3[] = { 1778, 2000, 2200, 2350, 1500 };
    const int *ratios = mode == 0 ? ratios_1x1 :
                        mode == 2 ? ratios_3x3 : ratios_1x3;
    int best = 0;
    int best_error = ABS(ratios[0] - ratio_x1000);

    for (int i = 1; i < 5; i++)
    {
        int error = ABS(ratios[i] - ratio_x1000);
        if (error < best_error)
        {
            best = i;
            best_error = error;
        }
    }

    if (mode == 0)
        slim_1x1_ar = best;
    else
        crop_preset_ar_menu = best;
}

/* Movie entries copied to Custom use these stricter rules rather than
 * altering the original Movie page behavior. */
int crop_rec_custom_adjust(int control, int delta)
{
    slim_crop_sync_from_backend();

    if (control == 0) /* Mode only: preserve Movie AR and preset tier. */
    {
        int ratio = slim_crop_current_ratio_x1000();
        int preset = slim_unified_preset;
        int mode = COERCE(slim_mode_ui, 0, 2);
        slim_mode_ui = MOD(mode + delta, 3);
        slim_crop_set_nearest_ratio(slim_mode_ui, ratio);
        slim_unified_preset = COERCE(
            preset, 0, slim_preset_choice_count() - 1);
        slim_crop_apply_mode();
        return 1;
    }

    if (control == 1) /* Aspect Ratio only: preserve Movie preset tier. */
    {
        if (slim_mode_ui == 3)
            return 1;
        int preset = slim_unified_preset;
        if (slim_mode_ui == 0)
            slim_1x1_ar = MOD(slim_1x1_ar + delta, 5);
        else
            menu_numeric_toggle(&crop_preset_ar_menu, delta, 0, 4);
        slim_unified_preset = COERCE(
            preset, 0, slim_preset_choice_count() - 1);
        slim_crop_apply_mode();
        return 1;
    }

    if (control == 2) /* Preset: existing rules preserve Mode and AR. */
    {
        slim_crop_preset_select(0, delta);
        return 1;
    }

    return 0;
}

/* Force a relocation to both callbacks for linkers that perform section GC. */
static void *crop_rec_touch_exports[] __attribute__((used)) = {
    (void *)&crop_rec_touch_adjust,
    (void *)&crop_rec_touch_get_value,
    (void *)&crop_rec_custom_adjust,
    (void *)&crop_rec_lv_transition_busy,
    (void *)&crop_rec_lv_transition_diag,
};

static struct menu_entry crop_rec_menu_eosm[] =
{
    {
        .name       = "Mode",
        .priv       = &slim_mode_ui,
        .select     = slim_crop_mode_select,
        .update     = slim_crop_mode_update,
        .min        = 0,
        .max        = 3,
        .choices    = CHOICES("1x1", "1x3", "3x3", "LV"),
        .edit_mode  = EM_INLINE_ADJUST,
        .depends_on = DEP_LIVEVIEW | DEP_MOVIE_MODE,
        .help       = "Crop / binning mode.",
    },
    {
        .name       = "Aspect Ratio",
        .priv       = &crop_preset_ar_menu,
        .select     = slim_crop_ar_select,
        .update     = slim_crop_ar_update,
        .max        = 4,
        .choices    = CHOICES("16:9", "2:1", "2.20:1", "2.35:1", "2.39:1"),
        .edit_mode  = EM_INLINE_ADJUST,
        .depends_on = DEP_LIVEVIEW | DEP_MOVIE_MODE,
        .help       = "Aspect ratio for the selected Mode and Preset.",
    },
    {
        .name       = "Preset",
        .priv       = &slim_unified_preset,
        .select     = slim_crop_preset_select,
        .update     = slim_crop_preset_update,
        .max        = 2,
        .choices    = CHOICES("Highest", "Higher", "Medium"),
        .edit_mode  = EM_INLINE_ADJUST,
        .depends_on = DEP_LIVEVIEW | DEP_MOVIE_MODE,
        .help       = "Resolution tier. Highest / Higher / Medium.",
    },
    {
        .name       = "Resolution",
        .update     = slim_crop_res_update,
        .depends_on = DEP_LIVEVIEW | DEP_MOVIE_MODE,
        .help       = "RAW resolution from Mode, Aspect Ratio and Preset (read-only).",
    },
    {
        .name       = "Quick Resolution",
        .select     = slim_crop_quick_res_select,
        .update     = slim_crop_quick_res_update,
        .depends_on = DEP_LIVEVIEW | DEP_MOVIE_MODE,
        .shidden    = 1,
        .help       = "Quick Screen selector for resolutions in this Aspect Ratio.",
    },
    {
        .name       = "Frame Rate",
        .priv       = &crop_preset_fps_menu,
        .select     = slim_crop_fps_select,
        .update     = slim_crop_fps_update,
        .max        = 2,
        .choices    = CHOICES("23.976", "25", "30"),
        .edit_mode  = EM_INLINE_ADJUST,
        .depends_on = DEP_LIVEVIEW | DEP_MOVIE_MODE,
        .help       = "Frame rates supported by the current configuration.",
    },
    {
        .name       = "Bit Depth",
        .priv       = &slim_bit_depth_ui,
        .select     = slim_crop_bit_select,
        .update     = slim_crop_bit_update,
        .max        = 2,
        .choices    = CHOICES("10 Bit", "12 Bit", "14 Bit"),
        .edit_mode  = EM_INLINE_ADJUST,
        .depends_on = DEP_LIVEVIEW | DEP_MOVIE_MODE,
        .help       = "Lossless RAW bit depth. Always available.",
    },
};

static struct menu_entry crop_rec_menu[] =
{
    // FIXME: how to handle menu in cleaner way for is_DIGIC_5 models?
    {
        .name       = "Crop mood",
        .priv       = &crop_preset_index,
        .update     = crop_update,
        .depends_on = DEP_LIVEVIEW | DEP_MOVIE_MODE,
        .children =  (struct menu_entry[]) {
            {
                .name       = "Preset:",   // CROP_PRESET_1X1
                .priv       = &crop_preset_1x1_res_menu,
                .update     = crop_preset_1x1_res_update,
                .max        = 7,
                .choices    = CHOICES("2.5K", "2.8K", "3K", "1440p", "1280p", "Full-Res LV", "1620p 4:3", "1920x1080p"),
                .help       = "Choose 1:1 preset.",
                .shidden    = 1,
            },
            {
                .name       = "Preset: ",  // CROP_PRESET_1X3
                .priv       = &crop_preset_1x3_res_menu,
                .update     = crop_preset_1x3_res_update,
                .max        = 3,
                .choices    = CHOICES("Highest", "Higher", "Medium", "Full-Res LV"),  // dummy choices, strings are being changed depending on aspect ratio and res
                .help       = "Choose 1x3 preset.",
                .shidden    = 1,
            },
            {
                .name       = "Preset:  ",  // CROP_PRESET_3X3
                .priv       = &crop_preset_3x3_res_menu,
                .update     = crop_preset_3x3_res_update,
                .max        = 2,
                .choices    = CHOICES("High FPS", "1080p", "1080p 3:2"),
                .help       = "HFR: High framerate and regular HD1080p modes.\n"
                              "Enable 1080p video mode from Canon.",
                .shidden    = 1,
            },
            /*
            {
                .name       = "Preset shortcuts",
                .priv       = &presets,
                .max        = 7,
                .choices    = CHOICES("None selected", "4.2K (16:9)", "1440p (16:9)", "976p (16:9)", "1080p (16:9)", "4.8K (2.35:1)", "2.8k (2.35:1)", "738p (2.35:1)"),
                .help       = "16:9 presets",
            },
             */
            {
                .name       = "Aspect ratio:",
                .priv       = &crop_preset_ar_menu,
                .update     = &crop_preset_ar_update,
                .max        = 4,
                .choices    = CHOICES("16:9", "2:1", "2.20:1", "2.35:1", "2.39:1"),
                .help       = "Select aspect ratio for current preset.",
                .shidden    = 1,
            },
            {
                .name       = "Framerate:",
                .priv       = &crop_preset_fps_menu,
                .update     = &crop_preset_fps_update,
                .max        = 2,
                .choices    = CHOICES("23.976 FPS", "25 FPS", "30 FPS"),
                .help       = "Select framerate for current preset.",
                .shidden    = 1,
            },
            {
                .name       = "Reduced framerate HFR",
                .priv       = &crop_preset_fps_reduce,
                .max        = 1,
                .choices    = CHOICES("OFF", "Activated"),
                .help       = "Reduces framerates slightly for HFR presets",
                .shidden    = 1,
            },
            {
                .name       = "Bit-depth",
                .priv       = &bit_depth_analog,
                .update     = bit_depth_analog_update,
                .max        = 3,
                .choices    = CHOICES("14-bit", "12-bit","11-bit", "10-bit"),
                .help       = "Choose bit-depth for lossless RAW video compression.",
            },
            {            
                .name       = "Fix Dual-ISO flicker",
                .priv       = &fix_dual_iso_flicker,
                .update     = fix_dual_iso_flicker_update,
                .max        = 1,
                .choices    = CHOICES("OFF", "ON"),
                .icon_type  = IT_DICE,
                .help       = "Removes Dual-ISO lines waterfall effect by reducing FPS a tiny bit.",
                .help2      = "This fixes a flicker and crawling cuased by moving Dual-ISO lines."
            },
            {
                .name   = "Preview Debug 1",
                .priv   = &preview_debug_1,
                .max    = 0xFFFFFFF,
                .unit   = UNIT_HEX,
                .help   = "Preview Debug.",
                .advanced = 1,
            },
            {
                .name   = "Preview Debug 2",
                .priv   = &preview_debug_2,
                .max    = 0xFFFFFFF,
                .unit   = UNIT_HEX,
                .help   = "Preview Debug.",
                .advanced = 1,
            },
            {
                .name   = "Preview Debug 4",
                .priv   = &preview_debug_4,
                .max    = 5208,
                .unit   = UNIT_DEC,
                .help   = "Preview Debug.",
                .advanced = 1,
            },
            {
                .name   = "Preview Debug 3",
                .priv   = &preview_debug_3,
                .max    = 3478,
                .unit   = UNIT_DEC,
                .help   = "Preview Debug.",
                .advanced = 1,
            },
            {
                .name   = "CMOS 5 Debug",
                .priv   = &CMOS_5_Debug,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "CMOS 5 Debug.",
                .advanced = 1,
            },
            {
                .name   = "CMOS 7 Debug",
                .priv   = &CMOS_7_Debug,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "CMOS 7 Debug.",
                .advanced = 1,
            },
            {
                .name   = "TimerA Debug",
                .priv   = &TimerA_Debug,
                .min    = -10000,
                .max    = 10000,
                .unit   = UNIT_DEC,
                .help   = "TimerA Debug.",
                .advanced = 1,
            },
            {
                .name   = "TimerB Debug",
                .priv   = &TimerB_Debug,
                .min    = -10000,
                .max    = 10000,
                .unit   = UNIT_DEC,
                .help   = "TimerB Debug.",
                .advanced = 1,
            },
            {
                .name   = "RAW H Debug",
                .priv   = &RAW_H_Debug,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "RAW H Debug.",
                .advanced = 1,
            },
            {
                .name   = "RAW V Debug",
                .priv   = &RAW_V_Debug,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "RAW V Debug.",
                .advanced = 1,
            },
            {
                .name   = "YUV_HD_S_H_height",
                .priv   = &YUV_HD_S_H_height,
                .min    = -20000,
                .max    = 20000,
                .unit   = UNIT_DEC,
                .help  = "Alter height.",
                .advanced = 1,
            },
            {
                .name   = "YUV_HD_S_H_width",
                .priv   = &YUV_HD_S_H_width,
                .min    = -20000,
                .max    = 20000,
                .unit   = UNIT_DEC,
                .help  = "Alter width. Scrambles preview",
                .advanced = 1,
            },
            {
                .name   = "YUV_HD_S_V_height",
                .priv   = &YUV_HD_S_V_height,
                .min    = -20000,
                .max    = 20000,
                .unit   = UNIT_DEC,
                .help  = "height offset",
                .advanced = 1,
            },
            {
                .name   = "YUV_HD_S_V_width",
                .priv   = &YUV_HD_S_V_width,
                .min    = -20000,
                .max    = 20000,
                .unit   = UNIT_DEC,
                .help  = "width offset",
                .advanced = 1,
            },
            {
                .name   = "Target YRES",
                .priv   = &target_yres,
                .update = target_yres_update,
                .max    = 3870,
                .unit   = UNIT_DEC,
                .help   = "Desired vertical resolution (only for presets with higher resolution).",
                .help2  = "Decrease if you get corrupted frames (dial the desired resolution here).",
                .advanced = 1,
            },
            {
                .name   = "Delta ADTG 0",
                .priv   = &delta_adtg0,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help   = "ADTG 0x8178, 0x8196, 0x82F8",
                .help2  = "May help pushing the resolution a little. Start with small increments.",
                .advanced = 1,
            },
            {
                .name   = "Delta ADTG 1",
                .priv   = &delta_adtg1,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help   = "ADTG 0x8179, 0x8197, 0x82F9",
                .help2  = "May help pushing the resolution a little. Start with small increments.",
                .advanced = 1,
            },
            {
                .name   = "Delta HEAD3",
                .priv   = &delta_head3,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help2  = "May help pushing the resolution a little. Start with small increments.",
                .advanced = 1,
            },
            {
                .name   = "Delta HEAD4",
                .priv   = &delta_head4,
                .min    = -500,
                .max    = 500,
                .unit   = UNIT_DEC,
                .help2  = "May help pushing the resolution a little. Start with small increments.",
                .advanced = 1,
            },
            {
                .name   = "CMOS[1] lo",
                .priv   = &cmos1_lo,
                .max    = 63,
                .unit   = UNIT_DEC,
                .help   = "Start scanline (very rough). Use for vertical positioning.",
                .advanced = 1,
            },
            {
                .name   = "CMOS[1] hi",
                .priv   = &cmos1_hi,
                .max    = 63,
                .unit   = UNIT_DEC,
                .help   = "End scanline (very rough). Increase if white bar at bottom.",
                .help2  = "Decrease if you get strange colors as you move the camera.",
                .advanced = 1,
            },
            {
                .name   = "CMOS[2]",
                .priv   = &cmos2,
                .max    = 0xFFF,
                .unit   = UNIT_HEX,
                .help   = "Horizontal position / binning.",
                .help2  = "Use for horizontal centering.",
                .advanced = 1,
            },
            {
                .name   = "reg_skip_left",
                .priv   = &reg_skip_left,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "skip left",
                .advanced = 1,
            },
            {
                .name   = "reg_skip_right",
                .priv   = &reg_skip_right,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "skip right",
                .advanced = 1,
            },
            {
                .name   = "reg_skip_top",
                .priv   = &reg_skip_top,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "skip top",
                .advanced = 1,
            },
            {
                .name   = "reg_skip_bottom",
                .priv   = &reg_skip_bottom,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "skip bottom",
                .advanced = 1,
            },
            {
                .name   = "reg_cmos5",
                .priv   = &reg_cmos5,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "cmos5",
                .advanced = 1,
            },
            {
                .name   = "reg_cmos7",
                .priv   = &reg_cmos7,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "cmos7",
                .advanced = 1,
            },
            {
                .name   = "reg_height",
                .priv   = &reg_height,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "height",
                .advanced = 1,
            },
            {
                .name   = "reg_width",
                .priv   = &reg_width,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "width",
                .advanced = 1,
            },
            {
                .name   = "reg_Preview_H",
                .priv   = &reg_Preview_H,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "Preview_H",
                .advanced = 1,
            },
            {
                .name   = "reg_Preview_V",
                .priv   = &reg_Preview_V,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "Preview_V",
                .advanced = 1,
            },
            {
                .name   = "reg_YUV_HD_S_H",
                .priv   = &reg_YUV_HD_S_H,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "YUV_HD_S_H",
                .advanced = 1,
            },
            {
                .name   = "reg_YUV_HD_S_V",
                .priv   = &reg_YUV_HD_S_V,
                .min    = -1000,
                .max    = 1000,
                .unit   = UNIT_DEC,
                .help  = "YUV_HD_S_V",
                .advanced = 1,
            },
            {
                .name    = "Brighten LV method",
                .priv    = &brighten_lv_method,
                .max     = 1,
                .choices = CHOICES("AeWb", "EVF"),
                .help    = "Select a method to brighten LV when using negative analog gain.",
                .help2   = "Brighten LV using a regiser related to AeWb task. Stable, inaccurate AF.\n"
                           "Brighten LV using regisers related to EVF task. Less stable, accurate AF.",
                .advanced = 1,
            },
            MENU_ADVANCED_TOGGLE,
            MENU_EOL,
        },
    },
};

            static struct menu_entry movie_menu_bitdepth[] =
            {
                {
                    .name       = "Bit-depth",
                    .priv       = &bit_depth_analog,
                    .update     = bit_depth_analog_update,
                    .max        = 3,
                    .choices    = CHOICES("14-bit", "12-bit","11-bit", "10-bit"),
                    .help       = "Choose bit-depth for lossless RAW video compression.",
                },
            };

            static struct menu_entry movie_menu_shutter_range[] =
            {
                {
                    .name       = "Shutter range",
                    .priv       = &shutter_range,
                    .max        = 1,
                    .choices    = CHOICES("Original", "Full range"),
                    .help       = "Choose the available shutter speed range:",
                    .help2      = "Original: default range used by Canon in selected video mode.\n"
                                  "Full range: from 1/FPS to minimum exposure time allowed by hardware."
                },
            };

            static struct menu_entry movie_menu_framerate[] =
            {
                {
                    .name       = "Framerate:",
                    .priv       = &crop_preset_fps_menu,
                    .update     = &crop_preset_fps_update,
                    .max        = 2,
                    .choices    = CHOICES("23.976 FPS", "25 FPS", "30 FPS"),
                    .help       = "Select framerate for current preset.",
                    .shidden    = 1,
                },
            };

            static struct menu_entry movie_menu_ratio[] =
            {
                {
                    .name       = "Aspect ratio:",
                    .priv       = &crop_preset_ar_menu,
                    .update     = &crop_preset_ar_update,
                    .max        = 4,
                    .choices    = CHOICES("16:9", "2:1", "2.20:1", "2.35:1", "2.39:1"),
                    .help       = "Select aspect ratio for current preset.",
                    .shidden    = 1,
                },
            };

            static struct menu_entry movie_menu_fps[] =
            {
                {
                    .name   = "FPS modifier",
                    .priv   = &fps_over,
                    .min    = -100000,
                    .max    = 100000,
                    .unit   = UNIT_DEC,
                    .help   = "Increase, decrease fps. Will apply to Full-Res LV 1x3",
                },
            };


static MENU_UPDATE_FUNC(customize_buttons_update)
{
    if (!CROP_PRESET_MENU && !patch_active)
    {
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "This setting only work with Crop mood.");
    }
}

static MENU_UPDATE_FUNC(Half_Shutter_update)
{
    if (Half_Shutter && !is_manual_focus())
    {
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Half-Shutter assignment only works with manual focus.");
    }
}

static struct menu_entry customize_buttons_menu[] =
{
    {
        .name       = "Customize buttons",
        .select     = menu_open_submenu,
        .update     = customize_buttons_update,
        .depends_on = DEP_LIVEVIEW,
        .help       = "Assign some phyiscal camera buttons to do some tasks.",
        .help2      = "This setting might block some ML features from using assigned buttons.",
        .children   =  (struct menu_entry[]) {
            {
               .name     = "Half-Shutter",
               .max      = 3,
               .choices  = CHOICES("OFF", "Zoom x10", "Focus aid", "Sticky Zoom"),
               .update   = Half_Shutter_update,
               .priv     = &Half_Shutter,
               .help     = "Assign Half-Shutter button to a task.",
               .help2    = "Get close up focus while filming\n"
            },
            {
               .name     = "SET Button",
#ifdef CONFIG_SLIM_MENUS
               .max      = 7,
               .choices  = CHOICES("OFF", "Zoom x10", "ISO", "Aperture +", "Dual ISO", "False color", "Aperture Expo", "Shutter Expo"),
#else
               .max      = 8,
               .choices  = CHOICES("OFF", "Zoom x10", "ISO", "Aperture +", "Dual ISO", "False color", "Aperture Expo", "Shutter Expo", "ISO Expo"),
#endif
               .priv     = &SET_button,
               .help     = "Assign SET button to a task.",
            },
            {
                .name     = "INFO Button",
#ifdef CONFIG_SLIM_MENUS
                .max      = 7,
                .choices  = CHOICES("OFF", "Zoom x10", "ISO", "Aperture -", "Dual ISO", "False color", "Shutter Expo", "Aperture Expo"),
#else
                .max      = 8,
                .choices  = CHOICES("OFF", "Zoom x10", "ISO", "Aperture -", "Dual ISO", "False color", "Shutter Expo", "Aperture Expo", "ISO Expo"),
#endif
                .priv     = &INFO_button,
                .help     = "Assign INFO button to a task.",
            },
            {
                .name   = "Tap display",
                .priv   = &tapdisp,
#ifdef CONFIG_SLIM_MENUS
                .max    = 4,
                .choices = CHOICES("OFF", "Preset list", "Shutter Expo", "Aperture Expo", "False color"),
#else
                .max    = 5,
                .choices = CHOICES("OFF", "Preset list", "Shutter Expo", "Aperture Expo", "ISO Expo", "False color"),
#endif
                .help     = "Assign Tap display to a task.",
            },
            {
                .name     = "U/D Arrows",
                .max      = 2,
                .choices  = CHOICES("OFF", "ISO", "Aperture"),
                .priv     = &Arrows_U_D,
                .help     = "Assign Up and Down arrows to a task.",
            },
            {
                .name     = "More_hacks",
                .max      = 1,
                .choices  = CHOICES("OFF", "Allow"),
                .priv     = &more_hacks,
                .help     = "More hacks always allowed",
            },
            /* not working with eosm
            {
                .name     = "L/R Arrows",
                .max      = 2,
                .choices  = CHOICES("OFF", "ISO", "Aperture"),
                .priv     = &Arrows_L_R,
                .help     = "Assign Left and Right arrows to a task.",
            },
             */

            MENU_EOL,
        },
    }
};

static int settings_changed = 0;
static int crop_rec_needs_lv_refresh()
{
    if (!lv)
    {
        return 0;
    }

    if (CROP_PRESET_MENU)
    {
        if (is_supported_mode())
        {
            if (!patch_active || CROP_PRESET_MENU != crop_preset)
            {
                return 1;
            }

            /* Currently settings_changed is only supprted for 650D / 700D / EOS M/M2 / 100D */
            if (is_DIGIC_5)
            {
                if (settings_changed) return 1;
            }
        }
    }
    else /* crop disabled */
    {
        if (patch_active)
        {
            return 1;
        }
    }

    return 0;
}

static void center_canon_preview()
{
    /* center the preview window on the raw buffer */
    /* overriding these registers once will do the trick...
     * ... until the focus box is moved by the user */
    int old = cli();

    uint32_t pos1 = shamem_read(0xc0f383d4);
    uint32_t pos2 = shamem_read(0xc0f383dc);

    if ((pos1 & 0x80008000) == 0x80008000 &&
        (pos2 & 0x80008000) == 0x80008000)
    {
        /* already centered */
        sei(old);
        return;
    }

    int x1 = pos1 & 0xFFFF;
    int x2 = pos2 & 0xFFFF;
    int y1 = pos1 >> 16;
    int y2 = pos2 >> 16;

    if (x2 - x1 != 299 && y2 - y1 != 792)
    {
        /* not x5/x10 (values hardcoded for 5D3) */
        sei(old);
        return;
    }

    int raw_xc = (146 + 3744) / 2 / 4;  /* hardcoded for 5D3 */
    int raw_yc = ( 60 + 1380) / 2;      /* values from old raw.c */

    if (1)
    {
        /* use the focus box position for moving the preview window around */
        /* don't do that while recording! */
        dbg_printf("[crop_rec] %d,%d ", raw_xc, raw_yc);
        raw_xc -= 146 / 2 / 4;  raw_yc -= 60 / 2;
        /* this won't change the position if the focus box is centered */
        get_afframe_pos(raw_xc * 2, raw_yc * 2, &raw_xc, &raw_yc);
        raw_xc += 146 / 2 / 4;  raw_yc += 60 / 2;
        raw_xc &= ~1;   /* just for consistency */
        raw_yc &= ~1;   /* this must be even, otherwise the image turns pink */
        raw_xc = COERCE(raw_xc, 176, 770);  /* trial and error; image pitch changes if we push to the right */
        raw_yc = COERCE(raw_yc, 444, 950);  /* trial and error; broken image at the edges, outside these limits */
        dbg_printf("-> %d,%d using focus box position\n", raw_xc, raw_yc);
    }
    int current_xc = (x1 + x2) / 2;
    int current_yc = (y1 + y2) / 2;
    int dx = raw_xc - current_xc;
    int dy = raw_yc - current_yc;
    
    if (dx || dy)
    {
        /* note: bits 0x80008000 appear to have no effect,
         * so we'll use them to flag the centered zoom mode,
         * e.g. for focus_box_get_raw_crop_offset */
        dbg_printf("[crop_rec] centering zoom preview: dx=%d, dy=%d\n", dx, dy);
        EngDrvOutLV(0xc0f383d4, PACK32(x1 + dx, y1 + dy) | 0x80008000);
        EngDrvOutLV(0xc0f383dc, PACK32(x2 + dx, y2 + dy) | 0x80008000);
    }

    sei(old);
}


/* variables for 650D / 700D / EOSM/M2 / 100D help to detect if settings changed */
static int old_ar_preset;
static int old_crop_preset_index;
static int old_fps_preset;
static int old_1x1_preset;
static int old_1x3_preset;
static int old_3x3_preset;
static int old_dual_iso;
static int old_diso_fix;
static int old_bit_depth;
static int old_crop_preset_fps_reduce;
static int old_fps_over;
static int old_shutter_range;

int check_if_settings_changed()
{
    if (old_crop_preset_index != crop_preset_index  ||
        old_ar_preset  != crop_preset_ar_menu       ||
        old_fps_preset != crop_preset_fps_menu      ||
        old_1x1_preset != crop_preset_1x1_res_menu  ||
        old_1x3_preset != crop_preset_1x3_res_menu  ||
        old_3x3_preset != crop_preset_3x3_res_menu  ||
        old_dual_iso   != dual_iso_is_enabled()     ||
        (old_bit_depth  != bit_depth_analog && Anam_FLV)         ||
        old_diso_fix   != fix_dual_iso_flicker      ||
        old_crop_preset_fps_reduce != crop_preset_fps_reduce ||
        old_fps_over != fps_over ||
        old_shutter_range != shutter_range)
    {
        return 1;
    }

    return 0;
}

#ifdef CONFIG_EOSM
static int crop_rec_lv_dirty = 1;

static int eosm_lv_guard_pipeline_ready(void)
{
    uint32_t display_buffer;

    if (!liveview_display_idle() || !CROP_PRESET_MENU || !patch_active)
        return 0;

    /* All EOS M movie crop presets use x5 for their stable preview path. */
    if (!is_movie_mode() || lv_dispsize != 5 || PathDriveMode->zoom != 5)
        return 0;

    /* Dimensions alone are not enough: early boot may still expose the
     * previous RAW geometry while Canon is replacing the actual buffers. */
    display_buffer = YUV422_LV_BUFFER_DISPLAY_ADDR;
    if (raw_info.width <= 0 || raw_info.height <= 0 || raw_info.pitch <= 0 ||
        !raw_info.buffer || !display_buffer)
        return 0;

    if (raw_info.width == eosm_lv_guard_width &&
        raw_info.height == eosm_lv_guard_height &&
        raw_info.pitch == eosm_lv_guard_pitch &&
        (uintptr_t)raw_info.buffer == eosm_lv_guard_raw_buffer &&
        display_buffer == eosm_lv_guard_display_buffer)
        eosm_lv_guard_stable_frames++;
    else
    {
        eosm_lv_guard_width = raw_info.width;
        eosm_lv_guard_height = raw_info.height;
        eosm_lv_guard_pitch = raw_info.pitch;
        eosm_lv_guard_raw_buffer = (uintptr_t)raw_info.buffer;
        eosm_lv_guard_display_buffer = display_buffer;
        eosm_lv_guard_stable_frames = 1;
        eosm_lv_guard_quiet_since = 0;
    }

    if (eosm_lv_guard_stable_frames < EOSM_LV_GUARD_STABLE_FRAMES)
        return 0;

    /* Canon often performs one final asynchronous write after raw geometry
     * becomes valid. Require a quiet buffer window before Crop Rec owns x5. */
    if (!eosm_lv_guard_quiet_since)
        eosm_lv_guard_quiet_since = get_ms_clock();

    return get_ms_clock() - eosm_lv_guard_quiet_since >= EOSM_LV_GUARD_QUIET_MS;
}

/* The buffer can be stable while Canon is still exposing the previous movie
 * layout (for example 2520x1080 at 29.97 fps before a 1x3 preset arrives).
 * Do not hand that transitional layout to the UI or recorder as a valid crop
 * frame.  The EOS M RAW buffer includes a small sensor margin around the
 * selected output, hence the deliberately narrow positive allowance. */
static int eosm_lv_guard_selected_geometry_ready(void)
{
    int expected_w, expected_h;

    /* Full-resolution LV is a separate 3 fps path, not a movie crop layout. */
    if (slim_mode_ui == 3 ||
        (CROP_PRESET_MENU == CROP_PRESET_1X1 && crop_preset_1x1_res_menu == 5))
        return 1;

    slim_crop_expected_res(&expected_w, &expected_h);
    if (expected_w <= 0 || expected_h <= 0)
        return 0;

    return raw_info.width  >= expected_w && raw_info.width  <= expected_w + 128 &&
           raw_info.height >= expected_h && raw_info.height <= expected_h + 64;
}

/* A valid RAW geometry does not guarantee that Canon restored the visible
 * YUV path. Sample a sparse center grid from the displayed UYVY buffer: this
 * is intentionally tiny and read-only, so it cannot disturb EDMAC or RAW
 * recording. Return false only for a uniformly video-black screen. */
static int eosm_lv_guard_display_luma_max(void)
{
    const uint8_t *vram;
    int x, y;
    int luma_max = 0;
    int width = vram_lv.width;
    int height = vram_lv.height;
    int pitch = vram_lv.pitch;
    static const uint8_t x_pos[] = { 2, 4, 6, 8 };
    static const uint8_t y_pos[] = { 3, 5, 7 };

    if (!YUV422_LV_BUFFER_DISPLAY_ADDR || width < 64 || height < 64 ||
        pitch < width * 2)
        return 255; /* unavailable data is handled by the geometry guard */

    vram = (const uint8_t *)UNCACHEABLE(YUV422_LV_BUFFER_DISPLAY_ADDR);
    for (y = 0; y < COUNT(y_pos); y++)
    {
        int py = height * y_pos[y] / 10;
        for (x = 0; x < COUNT(x_pos); x++)
        {
            int px = width * x_pos[x] / 10;
            /* UYVY: luma is the second byte of each two-byte pixel. */
            luma_max = MAX(luma_max, vram[py * pitch + px * 2 + 1]);
        }
    }

    return luma_max;
}

static int eosm_lv_guard_display_has_content(void)
{
    return eosm_lv_guard_display_luma_max() > 20;
}

/* These are the display-route values already supplied by the Crop Rec ENGIO
 * hook. Check only the final scaler/buffer-format registers; sensor timing,
 * RAW geometry and EDMAC routing are deliberately outside this recovery. */
static int eosm_lv_guard_display_route_ready(void)
{
    if (!Preview_Control || !YUV_LV_Buf)
        return 1;

    return shamem_read(0xC0F11B8C) == YUV_HD_S_H &&
           shamem_read(0xC0F11BCC) == YUV_HD_S_V &&
           shamem_read(0xC0F11BC8) == YUV_HD_S_V_E &&
           shamem_read(0xC0F11ACC) == YUV_LV_S_V &&
           shamem_read(0xC0F04210) == YUV_LV_Buf;
}

static void eosm_lv_guard_reapply_display_route(void)
{
    EngDrvOutLV(0xC0F11B8C, YUV_HD_S_H);
    EngDrvOutLV(0xC0F11BCC, YUV_HD_S_V);
    EngDrvOutLV(0xC0F11BC8, YUV_HD_S_V_E);
    EngDrvOutLV(0xC0F11ACC, YUV_LV_S_V);
    EngDrvOutLV(0xC0F04210, YUV_LV_Buf);
}

/* Exported for LVRECOV.LOG. The signature changes only when the transition
 * controller changes state, so the recorder log stays event-only. */
__attribute__((used, noinline))
int crop_rec_lv_transition_diag(char *buffer, int size)
{
    int luma = eosm_lv_guard_display_luma_max();
    int route_ok = eosm_lv_guard_display_route_ready();
    int signature = (eosm_lv_guard_pending ? 1 : 0) |
        (eosm_lv_guard_busy ? 2 : 0) |
        (eosm_lv_guard_state << 2) |
        (eosm_lv_guard_content_dark_frames << 6) |
        (eosm_lv_guard_content_retries << 10) |
        (eosm_lv_guard_route_retries << 12) |
        (route_ok ? 1 << 14 : 0);

    if (buffer && size > 0)
        snprintf(buffer, size,
            "guard=%d/%d/%d luma=%d dark=%d contentfix=%d route=%d routefix=%d",
            eosm_lv_guard_pending, eosm_lv_guard_busy, eosm_lv_guard_state,
            luma, eosm_lv_guard_content_dark_frames,
            eosm_lv_guard_content_retries, route_ok, eosm_lv_guard_route_retries);

    return signature;
}

static void eosm_lv_guard_clear(void)
{
    eosm_lv_guard_pending = 0;
    eosm_lv_guard_busy = 0;
    crop_rec_lv_dirty = 0;
    settings_changed = 0;
}

static int eosm_lv_guard_step(int menu_shown, int mlv_busy)
{
    int now;

    if (!eosm_lv_guard_pending)
        return 0;

    if (!CROP_PRESET_MENU || !is_movie_mode())
    {
        eosm_lv_guard_pending = 0;
        eosm_lv_guard_busy = 0;
        return 0;
    }

    if (!lv || menu_shown || RECORDING_RAW || mlv_busy)
        return 1;

    /* x10 is Canon's focusing view, not the custom x5 preview.  Do not hold
     * the guard open there; exiting x10 requests a fresh normal transition. */
    if (lv_dispsize == 10)
    {
        eosm_lv_guard_pending = 0;
        eosm_lv_guard_busy = 0;
        return 0;
    }

    now = get_ms_clock();
    if (!eosm_lv_guard_started)
        eosm_lv_guard_started = now;

    eosm_lv_guard_busy = 1;

    switch (eosm_lv_guard_state)
    {
        case EOSM_LV_GUARD_WAIT:
            /* Give Canon most of the 0.5-second window, then require three
             * matching RAW frames before touching the custom preview regs. */
            if (now - eosm_lv_guard_started < EOSM_LV_GUARD_SETTLE_MS)
                return 1;
            /* Canon often returns to x1 after boot or a Canon menu.  Restore
             * the crop module's normal x5 preview before validating frames. */
            if (lv_dispsize == 1)
            {
                eosm_lv_guard_set_zoom(5);
                eosm_lv_guard_stable_frames = 0;
                eosm_lv_guard_quiet_since = 0;
                return 1;
            }
            if (!eosm_lv_guard_pipeline_ready())
                return 1;

            /* Always rebuild x5 in a controlled way. This makes menu exits,
             * record-stop and boot use exactly the same known-good path,
             * instead of trusting whatever preview state Canon left behind. */
            eosm_lv_guard_set_zoom(1);
            eosm_lv_guard_state = EOSM_LV_GUARD_APPLY_X1;
            eosm_lv_guard_started = now;
            eosm_lv_guard_stable_frames = 0;
            eosm_lv_guard_quiet_since = 0;
            return 1;

        case EOSM_LV_GUARD_APPLY_X1:
            if (lv_dispsize != 1)
                return 1;
            if (now - eosm_lv_guard_started < 80)
                return 1;
            eosm_lv_guard_set_zoom(5);
            eosm_lv_guard_state = EOSM_LV_GUARD_APPLY_X5;
            eosm_lv_guard_started = now;
            eosm_lv_guard_stable_frames = 0;
            eosm_lv_guard_quiet_since = 0;
            return 1;

        case EOSM_LV_GUARD_APPLY_X5:
            if (now - eosm_lv_guard_started < EOSM_LV_GUARD_SETTLE_MS ||
                !eosm_lv_guard_pipeline_ready())
                return 1;
            eosm_lv_guard_state = EOSM_LV_GUARD_VALIDATE;
            eosm_lv_guard_started = now;
            return 1;

        case EOSM_LV_GUARD_VALIDATE:
            /* Verify x5 remains quiet after Canon has had time to consume the
             * final zoom request. A quiet buffer alone is not enough: Canon
             * may still be serving the old preset's geometry here. */
            if (!eosm_lv_guard_pipeline_ready() ||
                now - eosm_lv_guard_started < EOSM_LV_GUARD_QUIET_MS)
                return 1;
            if (eosm_lv_guard_selected_geometry_ready() &&
                !crop_rec_needs_lv_refresh())
            {
                /* Geometry is correct. Confirm the LCD path has delivered a
                 * real frame before releasing the transition controller. */
                eosm_lv_guard_state = EOSM_LV_GUARD_CONTENT;
                eosm_lv_guard_started = now;
                eosm_lv_guard_content_dark_frames = 0;
                return 1;
            }

            /* Rebuild the selected x5 path again when Canon retained a stable
             * but wrong geometry. This is the automatic equivalent of the
             * manual zoom-out/zoom-in recovery, without ever exposing the
             * temporary resolution as a yellow exclamation mark. */
            if (eosm_lv_guard_retries >= EOSM_LV_GUARD_MAX_RECOVERIES)
            {
                /* Keep waiting for Canon instead of declaring the wrong preset
                 * ready. A later LV property update restarts this guard. */
                eosm_lv_guard_state = EOSM_LV_GUARD_WAIT;
                eosm_lv_guard_started = now;
                eosm_lv_guard_stable_frames = 0;
                eosm_lv_guard_quiet_since = 0;
                eosm_lv_guard_retries = 0;
                return 1;
            }

            /* A failed validation gets a controlled x1 -> x5 rebuild.
             * Do not use x10: it is a focus-only Canon path. */
            eosm_lv_guard_set_zoom(1);
            eosm_lv_guard_state = EOSM_LV_GUARD_RECOVER_X1;
            eosm_lv_guard_started = now;
            eosm_lv_guard_stable_frames = 0;
            eosm_lv_guard_quiet_since = 0;
            eosm_lv_guard_retries++;
            return 1;

        case EOSM_LV_GUARD_CONTENT:
            if (now - eosm_lv_guard_started < EOSM_LV_GUARD_QUIET_MS)
                return 1;

            if (eosm_lv_guard_display_has_content())
            {
                eosm_lv_guard_state = EOSM_LV_GUARD_ROUTE;
                eosm_lv_guard_started = now;
                return 1;
            }

            /* A genuine dark scene is possible. Require consecutive samples,
             * then make only one extra recovery attempt and accept a persistently
             * black scene afterward rather than trapping the user in a loop. */
            if (++eosm_lv_guard_content_dark_frames < EOSM_LV_GUARD_CONTENT_SAMPLES)
                return 1;

            if (eosm_lv_guard_content_retries++ == 0)
            {
                eosm_lv_guard_set_zoom(1);
                eosm_lv_guard_state = EOSM_LV_GUARD_RECOVER_X1;
                eosm_lv_guard_started = now;
                eosm_lv_guard_stable_frames = 0;
                eosm_lv_guard_quiet_since = 0;
                eosm_lv_guard_retries++;
                return 1;
            }

            eosm_lv_guard_clear();
            return 0;

        case EOSM_LV_GUARD_ROUTE:
            if (now - eosm_lv_guard_started < EOSM_LV_GUARD_QUIET_MS)
                return 1;

            if (eosm_lv_guard_display_route_ready())
            {
                eosm_lv_guard_clear();
                return 0;
            }

            /* Canon may overwrite its final display-route values after the
             * frame itself is ready. Restore only the five existing Crop Rec
             * route values once, then leave Canon in control if it disagrees. */
            if (eosm_lv_guard_route_retries++ == 0)
            {
                eosm_lv_guard_reapply_display_route();
                eosm_lv_guard_started = now;
                return 1;
            }

            eosm_lv_guard_clear();
            return 0;

        case EOSM_LV_GUARD_RECOVER_X1:
            if (lv_dispsize != 1)
            {
                /* Canon has not acknowledged x1 yet; keep waiting, but do
                 * not issue more property writes into the same transition. */
                return 1;
            }
            if (now - eosm_lv_guard_started < 80)
                return 1;
            eosm_lv_guard_set_zoom(5);
            eosm_lv_guard_state = EOSM_LV_GUARD_RECOVER_X5;
            eosm_lv_guard_started = now;
            eosm_lv_guard_stable_frames = 0;
            eosm_lv_guard_quiet_since = 0;
            return 1;

        case EOSM_LV_GUARD_RECOVER_X5:
            if (now - eosm_lv_guard_started < EOSM_LV_GUARD_SETTLE_MS ||
                !eosm_lv_guard_pipeline_ready())
                return 1;
            eosm_lv_guard_state = EOSM_LV_GUARD_VALIDATE;
            eosm_lv_guard_started = now;
            return 1;
    }

    eosm_lv_guard_request();
    return 1;
}
#endif


/* EOS M slim: Auto refocus (Settings → Auto refocus).
 *
 * ML cannot drive the lens itself on this camera, but Canon's half-press AF
 * can, and the one-shot AF assist above already runs it.  This monitor
 * watches the live sharpness (afdiag_sharpness) and, when focus has clearly
 * been lost and the picture has settled, raises that same AF request.
 *
 * It runs inside crop_rec_polling_cbr, right after slim_af_step(), on the
 * same task: no locks, it can never overlap the AF state machine (it only
 * raises a request while slim_af_state is IDLE and no request is pending),
 * and one sample every AREF_PERIOD_MS costs a few thousand pixel reads.
 * Sharpness depends on scene and zoom, so the remembered best sharpness (a
 * 12..24 s decaying peak, not a post-AF snapshot) is only compared at the zoom
 * it was seen at.  While recording, AF runs in place (request 2). */
#define AREF_PERIOD_MS      200   /* sharpness sampling interval */
#define AREF_SETTLE_MS      500   /* wait after AF / regained eligibility before judging */
#define AREF_STEADY_PCT     15    /* last 3 samples within this % of each other ... */
#define AREF_STEADY_ABS_PCT 8     /* ... or within this % of the peak (noise floor while blurry) */
#define AREF_PEAK_HOLD_MS   12000 /* best-recent-sharpness memory: two buckets, so 12..24 s */
#define AREF_PEAK_EMAX_MIN  70    /* the remembered peak must have had a real edge: flat or noise-only
                                   * scenes peak at emax 20..30, textured in-focus at 130..370 (field logs) */
#define AREF_ABS_G_MIN      2     /* absolute path: mean |gradient| per sample above the noise floor */
#define AREF_COOLDOWN_MS    2000  /* after any AF */
#define AREF_FAIL_BACKOFF_MS 4000 /* after a failed AF */
#define AREF_MAX_FAILS      3     /* consecutive failures before pausing */
#define AREF_PAUSE_MS       20000 /* a pause ends by itself after this: one more attempt */
#define AREF_REC_START_MS   2000  /* ON + REC: wait this long after RAW recording starts */
#define AREF_REC_RETRY_MS   1500  /* ON + REC: retry delay after the first failed AF (lens parks at an end stop) */

/* Refocus sensitivity (Low / Medium / High), derived from the field logs
 * (afdiag19/20, guardrun2):
 *  - in-focus x5 sharpness is 60..200k, blurry plateaus 5..45k, i.e. real blur is
 *    10..35% of the recent best, so Low (45%) still catches it;
 *  - normal in-focus sag (exposure, subject moving, tilt) reaches ~65..75% of the
 *    peak, so Medium (60%) stays below it and only High (70%) starts to fire on it;
 *  - peak floor: noise-only plateaus sit at 10..14k, so 24..45k means "has texture";
 *  - dwell: how long the drop must persist before we spend an AF run. */
static const uint8_t  aref_drop_pct[3]  = { 45, 60, 70 };
static const uint16_t aref_dwell_ms[3]  = { 1000, 700, 500 };
static const uint16_t aref_dwell_rec[3] = { 600, 350, 250 };
static const uint32_t aref_peak_min[3]  = { 30000, 20000, 16000 }; /* field: blur 16-25k, noise 10-14k */
/* Absolute "blurry" path (scene-normalised): Laplacian energy / gradient energy, in %.
 * Used only when there is no usable remembered peak, once per blur episode.  Not
 * calibrated against real frames yet (the logs hold no gradient): the thresholds are
 * conservative and the value is logged (REFOCUS kind 10) so they can be tuned. 0 = off. */
/* Absolute blur test, calibrated on field run 27 (x5 presets): blurry
 * pictures had q = 100-132 with max edge 16-45; sharp ones q = 44-81 with
 * max edge 240-380 (low-detail sharp scenes: q ~52, edge 46-73).  Noise
 * dominates a blurry Laplacian, so q goes UP with blur. */
static const uint8_t  aref_abs_q[3]     = { 0, 105, 95 };   /* q at/above this ... */
static const uint8_t  aref_abs_em[3]    = { 0, 45, 55 };    /* ... and max edge at/below this = blurry */
#define AREF_ABS_Q_GOOD     85    /* q below this with a strong edge (see below) re-arms the absolute path */
#define AREF_ABS_EM_GOOD    100

enum { AREF_MON = 0, AREF_REQUESTED, AREF_OURS, AREF_FOREIGN };

static int aref_phase = AREF_MON;
static int aref_is_idle(void) { return aref_phase == AREF_MON; }
static int aref_prev_mode = 0;
static int aref_eligible = 0;
static uint32_t aref_ref = 0;        /* effective best recent sharpness (peak memory), 0 = none */
static int aref_ref_em = 0;          /* emax at that peak */
static int aref_ref_zoom = 0;        /* lv_dispsize the memory belongs to */
static uint32_t aref_pk_v[2];        /* peak buckets: [0] current, [1] previous */
static int aref_pk_em[2];
static int aref_pk_t = 0;            /* ms: start of the current bucket, 0 = empty */
static int aref_forget = 0;          /* clear the memory at the next (re)acquire */
static int aref_post_check = 0;      /* judge the picture after our AF finished OK */
static uint32_t aref_post_prev = 0;
static int aref_post_t = 0;          /* ms when our AF finished OK (for aref_post_check) */  /* sharpness after the previous OK-but-blurry AF, 0 = none */
static int aref_take_at = 0;         /* ms: settle end, no judging before */
static int aref_next_at = 0;         /* ms: cooldown end */
static int aref_last_t = 0;          /* ms: last sample */
static uint32_t aref_hist[3];
static int aref_hist_n = 0;
static int aref_low_since = 0;       /* ms: sharpness has been low since, 0 = not low */
static int aref_low_path = 0;        /* 1 = relative to peak, 2 = absolute */
static int aref_abs_used = 0;        /* absolute path already spent in this blur episode */
static int aref_fails = 0;
static int aref_paused = 0;
static int aref_pause_t = 0;
static uint32_t aref_pause_ref = 0;  /* peak at the failure that paused us */
static uint32_t aref_trig_ref = 0;
static int aref_count0 = 0;          /* slim_af_result_count at the request */
static int aref_dbg_t = 0;           /* ms: last periodic decision record */

static void aref_log(int what, uint32_t cur)
{
    afdiag_record(AFD_EV_REFOCUS, 16, what, cur, aref_ref,
                  lv_dispsize | (RECORDING ? 0x100 : 0) | (aref_fails << 16));
}

/* Read-only: remember and log Canon's AF frame (PROP_LV_AFFRAME: W, H, X, Y, w, h in
 * sensor units) so field logs show where AF looks at x5.  Nothing is ever written. */
static uint32_t aref_affr[6];
static void aref_log_affr(void)
{
    afdiag_record(AFD_EV_REFOCUS, 16, 8, (aref_affr[2] << 16) | (aref_affr[3] & 0xFFFF),
                  (aref_affr[4] << 16) | (aref_affr[5] & 0xFFFF),
                  (aref_affr[0] << 16) | (aref_affr[1] & 0xFFFF));
}
PROP_HANDLER(PROP_LV_AFFRAME)
{
    if (!buf || len < sizeof(aref_affr)) return;
    memcpy(aref_affr, buf, sizeof(aref_affr));
    aref_log_affr();
}

/* Wait wait_ms before judging again.  The peak memory survives unless forget:
 * field logs showed that re-taking the reference from an already blurry picture
 * (after a failed AF, menu close, recording start) disarmed the trigger for
 * tens of seconds. */
static void aref_drop_ref(int now, int wait_ms, int forget)
{
    if (forget) aref_forget = 1;
    aref_take_at = now + wait_ms;
    aref_low_since = 0;
    aref_hist_n = 0;
}

static void aref_peak_clear(void)
{
    aref_pk_v[0] = aref_pk_v[1] = 0;
    aref_pk_em[0] = aref_pk_em[1] = 0;
    aref_pk_t = 0;
    aref_ref = 0;
    aref_ref_em = 0;
    aref_post_prev = 0;
}

/* Two-bucket decaying peak: the best sharpness seen in the last 12..24 s. */
static void aref_peak_update(int now, uint32_t v, int em)
{
    if (aref_pk_t && now - aref_pk_t >= 2 * AREF_PEAK_HOLD_MS)
        aref_peak_clear();
    if (!aref_pk_t)
        aref_pk_t = now ? now : 1;
    else if (now - aref_pk_t >= AREF_PEAK_HOLD_MS)
    {
        aref_pk_v[1] = aref_pk_v[0];
        aref_pk_em[1] = aref_pk_em[0];
        aref_pk_v[0] = 0;
        aref_pk_em[0] = 0;
        aref_pk_t = now;
    }
    /* Field run: the peak value is a 3-sample median but em came from the
     * current sample, so a real 59k peak (em 324) was stored with a blurry
     * frame's em and rejected as "no texture".  Keep the strongest edge seen. */
    if (v >= aref_pk_v[0]) aref_pk_v[0] = v;
    if (em > aref_pk_em[0]) aref_pk_em[0] = em;
    int b = aref_pk_v[1] > aref_pk_v[0];
    aref_ref = aref_pk_v[b];
    aref_ref_em = aref_pk_em[b];
}

static void aref_step(void)
{
    int now = get_ms_clock();
    int busy = slim_af_state != SLIM_AF_IDLE;
    int sens = Auto_refocus_sens;
    if (sens < 0) sens = 0;
    if (sens > 2) sens = 2;

    /* Setting toggled: forget everything, including a pause. */
    if (Auto_refocus != aref_prev_mode)
    {
        aref_prev_mode = Auto_refocus;
        aref_fails = 0;
        aref_paused = 0;
        aref_eligible = 0;
        aref_forget = 1;
        aref_abs_used = 0;
    }

    /* Follow an AF run, ours or the user's (Shutter zoom AF). */
    if (aref_phase == AREF_MON && busy)
        aref_phase = AREF_FOREIGN;
    else if (aref_phase == AREF_REQUESTED)
    {
        if (busy)
            aref_phase = AREF_OURS;
        else if (!slim_af_request)
        {
            /* request dropped without running (Canon state not ready) */
            aref_phase = AREF_MON;
            aref_next_at = now + AREF_COOLDOWN_MS;
            aref_log(4, 0);
        }
        return;
    }
    else if ((aref_phase == AREF_OURS || aref_phase == AREF_FOREIGN) && !busy)
    {
        int failed = slim_af_result_count == aref_count0 || slim_af_last_result;
        if (aref_phase == AREF_FOREIGN)
        {
            /* the user focused (or zoomed): a new picture, start over */
            aref_next_at = now + AREF_COOLDOWN_MS;
            aref_drop_ref(now, AREF_SETTLE_MS, 1);
            aref_abs_used = 0;
        }
        else if (!failed)
        {
            /* The failure counter is cleared only if the picture really
             * recovered (judged after the settle): field logs had "AF ok"
             * at the lens end stop with the picture still blurry. */
            aref_next_at = now + AREF_COOLDOWN_MS;
            aref_drop_ref(now, AREF_SETTLE_MS, 0);
            aref_post_check = 1;
            aref_post_t = now;
            aref_log(2, 0);
        }
        else
        {
            aref_fails++;
            if (aref_fails >= AREF_MAX_FAILS && !aref_paused)
            {
                aref_paused = 1;
                aref_pause_t = now ? now : 1;
                aref_pause_ref = aref_trig_ref;
                aref_log(5, 0);
            }
            if (RECORDING && aref_fails == 1 && !aref_paused)
            {
                /* Field logs: a failed AF parks the lens at an end stop.
                 * Retry once, quickly. */
                aref_next_at = now + AREF_REC_RETRY_MS;
                aref_low_since = 0;
                aref_hist_n = 0;
            }
            else
            {
                aref_next_at = now + AREF_FAIL_BACKOFF_MS;
                aref_drop_ref(now, AREF_SETTLE_MS, 0);
            }
            aref_log(3, 0);
        }
        aref_phase = AREF_MON;
        return;
    }
    if (aref_phase != AREF_MON) return;

    /* mlv_raw_rec_busy() is true for the whole RAW recording, not only while
     * it starts and stops (field run: ON + REC never fired).  Block only the
     * transitions: busy without RECORDING_RAW, and the first seconds after
     * recording starts. */
    static int aref_rec_since = 0;
    if (RECORDING_RAW) { if (!aref_rec_since) aref_rec_since = now ? now : 1; }
    else aref_rec_since = 0;
    int raw_transition = (mlv_raw_rec_busy() && !RECORDING_RAW) ||
                         (RECORDING_RAW && now - aref_rec_since < AREF_REC_START_MS);

    /* Eligibility as a bit mask of what blocks us, so the log can say which gate. */
    int blk = 0;
    if (!(Auto_refocus && is_EOSM && lv && is_movie_mode() && CROP_PRESET_MENU)) blk |= 1;
    if (busy || slim_af_request || afdiag_lens_test) blk |= 2;
    if (is_manual_focus() || is_continuous_af()) blk |= 4;
    if (gui_menu_shown() || lv_disp_mode != 0) blk |= 8;
    if (crop_rec_lv_transition_busy() || raw_transition) blk |= 16;
    if (get_halfshutter_pressed()) blk |= 32;
    if (!(RECORDING ? Auto_refocus == 2 : lv_dispsize == 5)) blk |= 64;
    if (blk)
    {
        aref_eligible = 0;
        if (afdiag_log && Auto_refocus && now - aref_dbg_t >= 2000)
        {
            aref_dbg_t = now;
            afdiag_record(AFD_EV_REFOCUS, 16, 11, blk, lv_dispsize, 0);
        }
        return;
    }
    if (!aref_eligible || lv_dispsize != aref_ref_zoom)
    {
        /* newly eligible or zoom changed: keep the peak memory over a short
         * ineligible spell (menu, half-press, recording start), but not
         * across a zoom change (sharpness is zoom dependent). */
        if (lv_dispsize != aref_ref_zoom) aref_forget = 1;
        aref_eligible = 1;
        aref_ref_zoom = lv_dispsize;
        aref_drop_ref(now, AREF_SETTLE_MS, 0);
        aref_last_t = 0;
        return;
    }

    if (now - aref_last_t < AREF_PERIOD_MS) return;
    aref_last_t = now;

    int mean, n, emax;
    uint32_t gsum;
    uint32_t cur = afdiag_sharpness_ex(&mean, &n, &emax, &gsum);
    if (!n) return;

    if (aref_hist_n < 3) aref_hist_n++;
    aref_hist[2] = aref_hist[1];
    aref_hist[1] = aref_hist[0];
    aref_hist[0] = cur;

    /* scene-normalised focus measure: Laplacian / gradient energy, in %
     * (cur <= ~5.2M, so cur * 100 fits in 32 bits) */
    int q = gsum ? (int)((cur * 100) / gsum) : 0;
    int gn = gsum / n;
    if (gn > 255) gn = 255;
    if (q > 255) q = 255;

    if (now < aref_take_at) return;      /* settling: neither learn nor judge */
    if (aref_forget)
    {
        aref_peak_clear();
        aref_forget = 0;
    }

    /* Feed the peak with the median of the last 3 samples, so a one-sample
     * spike (the x10 -> x5 return gave 445k once, auto-exposure steps gave
     * 126k) cannot poison the memory. */
    if (aref_hist_n >= 3)
    {
        uint32_t a = aref_hist[0], b = aref_hist[1], c = aref_hist[2];
        uint32_t lo = a < b ? a : b;
        uint32_t hi = a < b ? b : a;
        uint32_t mid = c < hi ? c : hi;
        uint32_t med = lo > mid ? lo : mid;
        int first = !aref_ref;
        aref_peak_update(now, med, emax);
        if (first && aref_ref) aref_log(7, cur);
    }
    uint32_t peak = aref_ref;
    uint32_t pmin = aref_peak_min[sens];
    int pct = aref_drop_pct[sens];
    int dwell = RECORDING ? aref_dwell_rec[sens] : aref_dwell_ms[sens];
    int gate = 0;

    /* Re-arm the absolute path once a sharp picture has been seen again. */
    if (q < AREF_ABS_Q_GOOD && emax >= AREF_ABS_EM_GOOD) aref_abs_used = 0;

    if (aref_post_check && now - aref_post_t > 2000)
        aref_post_check = 0;  /* a menu or half-press intervened: different picture, don't judge it */
    if (aref_post_check)
    {
        aref_post_check = 0;
        if (peak && peak >= pmin && cur * 100 < peak * pct &&
            aref_post_prev && cur * 100 >= aref_post_prev * 75 && cur * 100 <= aref_post_prev * 133)
        {
            /* Two OK AFs in a row landed at the same sharpness, still far below
             * the remembered peak: that is a new, less detailed scene, not
             * blur.  Forget the old peak instead of hunting. */
            aref_post_prev = 0;
            aref_fails = 0;
            aref_peak_clear();
            aref_log(12, cur);
            return; /* decide again on the next sample, from the cleared state */
        }
        else if (peak && peak >= pmin && cur * 100 < peak * pct)
        {
            /* AF said OK but the picture is still blurry: counts as a failure */
            aref_post_prev = cur;
            aref_fails++;
            if (aref_fails >= AREF_MAX_FAILS && !aref_paused)
            {
                aref_paused = 1;
                aref_pause_t = now ? now : 1;
                aref_pause_ref = peak;
                aref_log(5, cur);
            }
            aref_log(9, cur);
        }
        else
        {
            aref_fails = 0;
            aref_post_prev = 0;
        }
    }

    if (aref_paused)
    {
        /* resume once sharpness is back (manual focus, scene change ...), or
         * after a while for one more attempt */
        if (cur * 100 >= aref_pause_ref * pct)
        {
            aref_paused = 0;
            aref_fails = 0;
            aref_drop_ref(now, 0, 0);
            aref_log(6, cur);
        }
        else if (now - aref_pause_t >= AREF_PAUSE_MS)
        {
            aref_paused = 0;
            aref_fails = AREF_MAX_FAILS - 1;
            aref_abs_used = 0;
            aref_drop_ref(now, 0, 0);
            aref_log(6, cur);
        }
        else
            gate = 6;
    }

    int path = 0;
    if (!gate)
    {
        if (peak >= pmin && aref_ref_em >= AREF_PEAK_EMAX_MIN)
        {
            if (cur * 100 < peak * pct) path = 1;
            else gate = 1;
        }
        else
            gate = 4;
        /* no usable memory (lost it, or only ever saw blur): absolute check */
        if (!path && gate == 4 && aref_abs_q[sens] && !aref_abs_used &&
            q >= aref_abs_q[sens] && emax <= aref_abs_em[sens] && gn >= AREF_ABS_G_MIN)
        {
            path = 2;
            gate = 0;
        }
    }

    if (!path)
        aref_low_since = 0;
    else
    {
        if (!aref_low_since || aref_low_path != path) aref_low_since = now;
        aref_low_path = path;
    }

    if (path)
    {
        int need = path == 2 ? 2 * dwell : dwell;   /* absolute path is the less certain one */
        if (now - aref_low_since < need) gate = 2;
        else if (now < aref_next_at) gate = 3;
        else if (aref_hist_n < 3) gate = 8;
        else
        {
            uint32_t hi = aref_hist[0], lo = aref_hist[0];
            for (int i = 1; i < 3; i++)
            {
                if (aref_hist[i] > hi) hi = aref_hist[i];
                if (aref_hist[i] < lo) lo = aref_hist[i];
            }
            /* scene still moving?  Blurry samples are small, so their relative
             * noise is large: also accept a spread below a fraction of the peak. */
            uint32_t tol = lo * AREF_STEADY_PCT;
            uint32_t ref_for_tol = peak ? peak : cur;
            if (ref_for_tol * AREF_STEADY_ABS_PCT > tol) tol = ref_for_tol * AREF_STEADY_ABS_PCT;
            if ((hi - lo) * 100 > tol) gate = 5;
        }
    }

    /* compact decision record, once a second, only with the diag log on:
     * w0 = 10 | sens<<8 | gate<<16 | path<<24, w1 = current, w2 = peak,
     * w3 = q% | gradient/sample<<8 | emax<<16 (gate: 0 fire, 1 not low, 2 dwell,
     * 3 cooldown, 4 no usable peak, 5 unsteady, 6 paused, 8 history) */
    if (afdiag_log && ((path && !gate) || now - aref_dbg_t >= 1000))
    {
        aref_dbg_t = now;
        afdiag_record(AFD_EV_REFOCUS, 16, 10 | (sens << 8) | (gate << 16) | (path << 24), cur, peak,
                      q | (gn << 8) | ((emax & 0xFFFF) << 16));
    }

    if (!path || gate) return;

    aref_trig_ref = peak;
    aref_count0 = slim_af_result_count;
    aref_low_since = 0;
    if (path == 2) aref_abs_used = 1;
    aref_phase = AREF_REQUESTED;
    slim_af_request = RECORDING ? 2 : 1;
    aref_log(1, cur);
    aref_log_affr();
}

/* when closing ML menu, check whether we need to refresh the LiveView */
static unsigned int crop_rec_polling_cbr(unsigned int unused)
{
    if (afd_mon_is_open && !lv && !afd_mon_closing && !afdiag_afdrive_busy)
    {
        afd_mon_closing = 1;
        task_create("afd_monclose", 0x1e, 0x1000, afd_mon_close_task, (void*)0);
    }
    
    /* touch/Movie-tab shortcuts inject Q+SET into an open menu — not used on slim. */
#ifndef CONFIG_SLIM_MENUS
    if (gui_menu_shown() && submenu && !RECORDING)
    {
        if (is_movie_mode())
        {
            module_send_keypress(MODULE_KEY_Q);
        }
        module_send_keypress(MODULE_KEY_PRESS_SET);
        submenu = 0;
    }
#endif
#ifdef CONFIG_EOSM
    /* crop_rec_lv_dirty is module-level; also checked at startup */
#else
    /* also check at startup */
    static int lv_dirty = 1;
#endif

#ifdef CONFIG_EOSM
    int mlv_busy = mlv_raw_rec_busy();
    static int eosm_lv_was_active = 0;
    static int eosm_recording_was_active = 0;
    static int eosm_display_mode = -1;
#else
    int mlv_busy = 0;
#endif

    int menu_shown = gui_menu_shown();
#ifdef CONFIG_EOSM
    /* Cover every path back into Movie Live View, including Canon menus
     * (which may not set gui_menu_shown), recording stop and boot. */
    if ((lv && !eosm_lv_was_active) ||
        (eosm_recording_was_active && !RECORDING) ||
        (eosm_display_mode != -1 && eosm_display_mode != lv_disp_mode))
        eosm_lv_guard_request();
    eosm_lv_was_active = lv;
    eosm_recording_was_active = RECORDING;
    eosm_display_mode = lv_disp_mode;

    static int crop_rec_menu_was_shown = 0;
    if (lv && menu_shown)
        crop_rec_menu_was_shown = 1;
    else if (crop_rec_menu_was_shown && lv && !menu_shown)
    {
        crop_rec_lv_dirty = 1;
        crop_rec_menu_was_shown = 0;
    }
#else
    if (lv && menu_shown)
    {
        lv_dirty = 1;
    }
#endif

    if (is_EOSM)
    {
        afdiag_sample();
        afdiag_flush();
    }

    /* One-shot AF assist owns x10 until it is done.  Run it before the early
     * exits below, so recording or a menu can never strand its remote
     * half-shutter press. */
    int slim_af_busy = slim_af_step();
    if (is_EOSM)
    {
        afdiag_sample(); /* catch a transition made by this step */
        aref_step();     /* also while recording: ON + REC */
    }
    if (slim_af_busy)
        return CBR_RET_CONTINUE;

    if (!lv || menu_shown || RECORDING_RAW || mlv_busy)
    {
        /* outside LV: no need to do anything */
        /* don't change while browsing the menu, but shortly after closing it */
        /* don't change while recording raw, or while mlv_lite is starting/stopping */
        return CBR_RET_CONTINUE;
    }

#ifdef CONFIG_EOSM
    if (eosm_lv_guard_step(menu_shown, mlv_busy))
        return CBR_RET_CONTINUE;
#endif
    
    /* check if any of our settings are changed */
    /* for 650D / 700D / EOSM/M2 / 100D */
    if (check_if_settings_changed())
    {
#ifdef CONFIG_EOSM
        crop_rec_lv_dirty = 1;
#else
        lv_dirty = 1;
#endif
        settings_changed = 1;
    }

#ifdef CONFIG_EOSM
    if (crop_rec_lv_dirty)
#else
    if (lv_dirty)
#endif
    {
        int needs_refresh = crop_rec_needs_lv_refresh();
        /* do we need to refresh LiveView? */
            if (needs_refresh)
            {
                /* let's check this once again, just in case */
                /* (possible race condition that would result in unnecessary refresh) */
#ifdef CONFIG_EOSM
                /* CheckPreviewRegsValuesAndForce is intentionally a no-op
                 * on EOS M, so avoid an unnecessary delayed polling wait. */
#else
                wait_lv_frames(2);
                if (crop_rec_needs_lv_refresh())
                {
                    info_led_on();
                    gui_uilock(UILOCK_EVERYTHING);
                    int old_zoom = lv_dispsize;
                    set_zoom(lv_dispsize == 1 ? 5 : 1);
                    set_zoom(old_zoom);
                    gui_uilock(UILOCK_NONE);
                    info_led_off();
                }
#endif
            }
#ifdef CONFIG_EOSM
        crop_rec_lv_dirty = 0;
#else
        lv_dirty = 0;
#endif
        settings_changed = 0;
    }

    if (is_5D3)
    {
        if (crop_preset == CROP_PRESET_CENTER_Z &&
        (lv_dispsize == 5 || lv_dispsize == 10))
        {
            center_canon_preview();
        }
    }

    /* center canon preview on raw buffer for CROP_PRESET_CENTER_Z preset.
     * FIXME: use Preview_Control_Basic or port center_canon_preview() to 70D */
    if (is_70D)
    {
        if (crop_preset == CROP_PRESET_CENTER_Z && lv_dispsize == 5 && !RECORDING)
        {
            if ((shamem_read(0xC0F383D4) != 0x1560141) || shamem_read(0xC0F383DC) != 0x44e0260)
            {
                EngDrvOutLV(0xC0F383D4,0x1560141);
                EngDrvOutLV(0xC0F383DC,0x44e0260);
            }
        }
    }

    /* 650D / 700D / EOSM/M2 / 100D preferences */
    if (is_DIGIC_5 && lv)
    {
        // all of our presets work in x5 mode because of preview, even none-cropped ones
        if (CROP_PRESET_MENU && !RECORDING && is_movie_mode()) 
        {
            // WB value will change in ML, but won't be applied until we refresh LV manually, that's because
            // of setting LV zoom to x5 zoom directly after we enter LV, this delay helps to avoid this issue
            /* Let´s keep things snappy. Removing this for now.
            if (lv_dispsize == 1)
            {
                gui_uilock(UILOCK_EVERYTHING);
                msleep(1100); 
                gui_uilock(UILOCK_NONE);
            }
             */
             
            {
                static int eosm_af_multi_set = 0;
                static int eosm_af_single_set = 0;
                if (CROP_PRESET_MENU != CROP_PRESET_3X3)
                {
                    eosm_af_multi_set = 0;
                    eosm_af_single_set = 0;
                }

            if (is_manual_focus())
            {
                /* while we are using manual focus and 3x3 presets change AF method to FlexiZone - Multi 
                 * this way preview will always work in 3x3 presets, also 738p HFR preset will have working preview while idle */
                if (lv_af_mode == 1 && CROP_PRESET_MENU == CROP_PRESET_3X3 && !eosm_af_multi_set)
                {
                    gui_uilock(UILOCK_EVERYTHING);
                    set_lv_af_mode(3); // Set it to FlexiZone - Multi
                    gui_uilock(UILOCK_NONE);
                    eosm_af_multi_set = 1;
                    NotifyBox(2500,"AF mode was set to FlexiZone Multi");
                }
                else
                {
                    if (lv_dispsize == 1) set_zoom(5);
                }
            }

            // our presets works only in x5 mode (when lv_af_mode set to "Tracking", we can't enter x5 mode anymore)
            // "Multi" uses x1 mode while focusing and it does work, but won't work with 100D due to crash mentioned in 
            // is_supported_mode(), "Single" appears to be the best in our case beside we can use it in x10 for focusing.
            // well, "Single" can also do focusing in x5 mode, but since we are modifying preview, autofocus in x5 mode
            // won't give accurate results, it seems modifying preiew break AF data
            // FlexiZone - Single = 1, Tracking = 2, FlexiZone - Multi = 3
            if (!is_manual_focus() && lv_af_mode != 1 && !eosm_af_single_set) // AF mode not set to FlexiZone - Single
            {
                gui_uilock(UILOCK_EVERYTHING);
                set_lv_af_mode(1); // Set it to FlexiZone - Single
                gui_uilock(UILOCK_NONE);
                eosm_af_single_set = 1;
                NotifyBox(2500,"AF mode was set to FlexiZone Single");
            }

            if (!is_manual_focus() && lv_af_mode == 1)
            {
                if (lv_dispsize == 1) set_zoom(5);
            }
            }
        }

        /* while idle, check our preview resgisters, force the new values if not set yet */
#ifndef CONFIG_EOSM
        if (!lv_dirty && !crop_rec_needs_lv_refresh() && CROP_PRESET_MENU && !RECORDING && lv_dispsize == 5 && PathDriveMode->zoom == 5)
        {
            if (Preview_Control && !Preview_Control_Basic) // presets with basic preview don't need it
            {
                CheckPreviewRegsValuesAndForce();
            }
        }
#endif

        // FIXME: for now, "More" hacks must be on in order to get wokring preview in 3x3 presets while recording
        // see notes in reg_override_3X3
        {
            static int slim_3x3_more_hacks_done = 0;
            if (CROP_PRESET_MENU != CROP_PRESET_3X3)
                slim_3x3_more_hacks_done = 0;
            else if (raw_lv_is_enabled() && !is_more_hacks_selected() && crop_preset_3x3_res_menu != 1
                     && !slim_3x3_more_hacks_done)
            {
                if (is_EOSM)
                    mlv_lite_set_small_hacks_more();
                else
                    menu_set_str_value_from_script("RAW video", "Small hacks", "More", 2);
                slim_3x3_more_hacks_done = 1;
                NotifyBox(2000, "Small Hacks set to More");
            }
        }

        /* disable Canon overlays in x5 mode for cleaner preview */
        extern int kill_canon_gui_mode;
        if (lv && patch_active && CROP_PRESET_MENU)
        {
            if (PathDriveMode->zoom == 5 && kill_canon_gui_mode != 1)
            {
                kill_canon_gui_mode = 1;
            }

            /* enable Canon overlays in x10 mode */
            if (PathDriveMode->zoom == 10 && kill_canon_gui_mode != 0)
            {
                kill_canon_gui_mode = 0;
                if (canon_gui_front_buffer_disabled())
                {
                    canon_gui_enable_front_buffer(0);
                    redraw();
                }
            }
        }

        /* on entry-level models, setting picture quality to RAW from Canon menu gains extra SRM chunk 
         * https://www.magiclantern.fm/forum/index.php?topic=26521.msg239231#msg239231 (+31 MB of RAM)  
         * let's check picture quality on startup, also when the user change it to other than RAW          
         * let's inform the user to change pic quality back to RAW, and a camera restart would required 
         * this extends recording times at high resolutions, also allows to record Full-Res LV @ 2 FPS */
        if (patch_active && CROP_PRESET_MENU && is_movie_mode())
        {
            if (pic_quality != 0x4060000)
            {
                pic_quality_warning = 1;
            }
        }

        if (!menu_shown)
        {
            // check crop_rec configurations while outside ML menu
            old_crop_preset_index = crop_preset_index;
            old_ar_preset  = crop_preset_ar_menu;
            old_fps_preset = crop_preset_fps_menu;
            old_1x1_preset = crop_preset_1x1_res_menu;
            old_1x3_preset = crop_preset_1x3_res_menu;
            old_3x3_preset = crop_preset_3x3_res_menu;
            old_dual_iso   = dual_iso_is_enabled();
            old_diso_fix   = fix_dual_iso_flicker;
            old_crop_preset_fps_reduce = crop_preset_fps_reduce;
            old_fps_over = fps_over;
            old_shutter_range = shutter_range;
            if (Anam_FLV)
            {
                old_bit_depth  = bit_depth_analog;
            }
        }
    }

    return CBR_RET_CONTINUE;
}

    /* customize buttons and buttons shortcuts, FIXME: implement these as feature in ML core? */
static unsigned int crop_rec_keypress_cbr(unsigned int key)
{
    extern int kill_canon_gui_mode;

#ifdef CONFIG_EOSM
    /* The transition controller owns Live View until its post-x5 validation
     * passes. Swallow only controls that can alter preview state; REC and
     * MENU remain available for normal camera safety and escape behavior. */
    if (eosm_lv_guard_busy && lv && !RECORDING &&
        (key == MODULE_KEY_TOUCH_1_FINGER ||
         key == MODULE_KEY_PRESS_SET ||
         key == MODULE_KEY_PRESS_UP || key == MODULE_KEY_PRESS_DOWN ||
         key == MODULE_KEY_PRESS_LEFT || key == MODULE_KEY_PRESS_RIGHT ||
         key == MODULE_KEY_WHEEL_LEFT || key == MODULE_KEY_WHEEL_RIGHT ||
         key == MODULE_KEY_INFO))
        return 0;
#endif

    /* Close the touch editor at the REC press itself, before the asynchronous
     * RAW/H.264 recording state flag changes.  gui-common blocks every touch
     * once RECORDING is set, regardless of Global Draw. */
    if (is_EOSM && key == MODULE_KEY_REC && lvinfo_touch_editor_is_open())
        lvinfo_touch_editor_close();

    /* EOS M Live View taps are routed by gui-common.c (Quick Panel, grid,
     * and Last Settings). Do not let the legacy crop.tapdisp shortcuts race
     * that router during boot or Canon INFO transitions. */
    if (is_EOSM && key == MODULE_KEY_TOUCH_1_FINGER)
        return 1;

    /* Recording: touch is blocked in gui-common (idle LV touch is allowed). */

    //Reset zoom when stopping recording
    
    //Prevent black screen?
    if (key == MODULE_KEY_REC && RECORDING)
    {
        set_zoom(1);
    }
    
    //Focus aid function
    if (Half_Shutter == 2 && RECORDING && (crop_preset == CROP_PRESET_3X3 || crop_preset == CROP_PRESET_1X1))
    {
        if (key == MODULE_KEY_REC && RECORDING)
        {
            zoom = 0;
        }
        
        //Resets the preview zoom when releasing halfshutter. Better for when using autofocus
        if (!get_halfshutter_pressed() && zoom)
        {
            zoom = 0;
            if (crop_preset == CROP_PRESET_3X3)
            {
                EngDrvOutLV(0xc0f11A88, 0x0);
            }
            CheckPreviewRegsValuesAndForce();
        }
        
        if (get_halfshutter_pressed() && !gui_menu_shown() && lv && is_movie_mode() && !zoom)
        {
            zoom = 1;
            EngDrvOutLV(0xc0f11B8C, 0x0);
            EngDrvOutLV(0xc0f11BCC, 0x0);
            EngDrvOutLV(0xc0f11BC8, 0x0);
            if (crop_preset == CROP_PRESET_1X3)
            {
                EngDrvOutLV(0xc0f11A88, 0x1);
            }
        }
    }
    
    //Focus aid function
    if (Half_Shutter == 2 && RECORDING && crop_preset == CROP_PRESET_1X3)
    {
        if (key == MODULE_KEY_REC && RECORDING)
        {
            zoom = 0;
        }
        //Resets the preview zoom when releasing halfshutter. Better for when using autofocus
        if (!get_halfshutter_pressed() && zoom)
        {
            zoom = 0;
            if (crop_preset == CROP_PRESET_1X3)
            {
                EngDrvOutLV(0xc0f11A88, 0x0);
                CheckPreviewRegsValuesAndForce();
            }
        }
        
        if (get_halfshutter_pressed() && !gui_menu_shown() && lv && is_movie_mode() && !zoom)
        {
            zoom = 1;

            if (Anam_FLV)
            {
                EngDrvOutLV(0xc0f11A88, 0x1);
                YUV_HD_S_H    = 0x10501B5 + YUV_HD_S_H_width - (90 << 16);
                YUV_HD_S_V    = 0x45015C + 800 + (1000 << 16);
                CheckPreviewRegsValuesAndForce();
            }
            else if (Anam_Higher || Anam_Highest)
            {
                EngDrvOutLV(0xc0f11A88, 0x1);
                YUV_HD_S_H    = 0x105015B + 4000 + (6000 << 16);
                YUV_HD_S_V    = 0x105036D + YUV_HD_S_V_width + (20000 << 16);
                CheckPreviewRegsValuesAndForce();
            }
            else
            {
                EngDrvOutLV(0xc0f11B8C, 0x0);
                EngDrvOutLV(0xc0f11BCC, 0x0);
                EngDrvOutLV(0xc0f11BC8, 0x0);
                if (crop_preset == CROP_PRESET_1X3)
                {
                    EngDrvOutLV(0xc0f11A88, 0x1);
                }
            }
            
        }
    }
    
    /* we need to use customize buttons in LiveView while ML isn't showing and when using Crop mood */
    if (lv && !gui_menu_shown())
    {
        /* EOS M slim: INFO Button mapping must work even before patch_active settles. */
        if ((is_EOSM && is_movie_mode()) || (CROP_PRESET_MENU && patch_active))
        {
            if (slim_handle_main_dial_shutter(key))
                return 0;

            if (slim_handle_shutter_zoom(key))
                return 0;

            if (slim_handle_set_button(key))
                return 0;

            {
                int info = slim_handle_info_button(key);
                if (info == 1) return 0;
                if (info == -1) return 1;
            }

            /* Quick x10 mode */
            if (lv_dispsize == 5 && !RECORDING)
            {
                if (((key == MODULE_KEY_PRESS_SET         ) && SET_button  == 1)                 ||
                    ((key == MODULE_KEY_INFO              ) && !is_EOSM && INFO_button == 1)     ||
                    (!is_EOSM && (key == MODULE_KEY_PRESS_HALFSHUTTER ) && Half_Shutter && is_manual_focus()) )
                {
                    if(lv_disp_mode != 0){
                        // Use INFO key to cycle LV as normal when not in the LV with ML overlays
                        return 1;
                    }
                    set_zoom(10);

                    /* Enable Canon overlays in x10 mode */
                    kill_canon_gui_mode = 0;
                    if (canon_gui_front_buffer_disabled())
                    {
                            canon_gui_enable_front_buffer(0);
                    }
                    return 0;
                }
            }

            /* Finished from x10 mode? Let's get back to normal preview */
            if (lv_dispsize == 10 && !RECORDING)
            {
                
                if (((key == MODULE_KEY_PRESS_SET           ) && SET_button  == 1)                 ||
                    ((key == MODULE_KEY_INFO                ) && !is_EOSM && INFO_button == 1)     ||
                    (!is_EOSM && (key == MODULE_KEY_UNPRESS_HALFSHUTTER ) && Half_Shutter != 3 && is_manual_focus()) ||
                    (!is_EOSM && (key == MODULE_KEY_PRESS_HALFSHUTTER ) && Half_Shutter == 3 && is_manual_focus()) )
                {
                    set_zoom(1); // Get to x1 first, sometime we get black preview when going x10 --> x5
                    msleep(50);
                    set_zoom(5);

                    /* Disable Canon overlays in x5 mode */
                    kill_canon_gui_mode = 1;
                    return 0;
                }
            }

            /* EOS M idle movie LV + ML overlays: Up/Down Settings remaps only. */
            if (is_EOSM && !RECORDING && lv_dispsize != 10 && lv_disp_mode == 0)
            {
                if (key == MODULE_KEY_PRESS_UP && slim_handle_arrow_adjust(Arrows_U_D, 1))
                    return 0;
                if (key == MODULE_KEY_PRESS_DOWN && slim_handle_arrow_adjust(Arrows_U_D, -1))
                    return 0;
            }

            if (lv_dispsize == 5)
            {
                /* Non-EOSM (or during REC): legacy U/D L/R shortcuts */
                if (!is_EOSM || RECORDING)
                {
                    if (key == MODULE_KEY_PRESS_UP && slim_handle_arrow_adjust(Arrows_U_D, 1))
                        return 0;
                    if (key == MODULE_KEY_PRESS_DOWN && slim_handle_arrow_adjust(Arrows_U_D, -1))
                        return 0;
                    if (key == MODULE_KEY_PRESS_RIGHT && slim_handle_arrow_adjust(Arrows_L_R, 1))
                        return 0;
                    if (key == MODULE_KEY_PRESS_LEFT && slim_handle_arrow_adjust(Arrows_L_R, -1))
                        return 0;
                }

                if (((key == MODULE_KEY_INFO)       && !is_EOSM && INFO_button == 2) ||
                    ((key == MODULE_KEY_PRESS_SET)  && SET_button  == 2))
                {
                    if(lv_disp_mode != 0){
                        // Use INFO key to cycle LV as normal when not in the LV with ML overlays
                        return 1;
                    }
                    
                    if (slim_expo_frozen()) return 0;
                    crop_rec_adjust_iso(2);
                    return 0;
                }

                if (key == MODULE_KEY_INFO && !is_EOSM && INFO_button == 3)
                {
                    if(lv_disp_mode != 0){
                        // Use INFO key to cycle LV as normal when not in the LV with ML overlays
                        return 1;
                    }
                    if (slim_expo_frozen()) return 0;
                    aperture_toggle(0, -1);
                    return 0;
                }
                if (key == MODULE_KEY_PRESS_SET && SET_button == 3)
                {
                    if (slim_expo_frozen()) return 0;
                    aperture_toggle(0, 1);
                    return 0;
                }

                /* Dual ISO ON / OFF */
                if (((key == MODULE_KEY_INFO)       && !is_EOSM && INFO_button == 4) ||
                    ((key == MODULE_KEY_PRESS_SET)  && SET_button == 4))
                {
                    if(lv_disp_mode != 0){
                        // Use INFO key to cycle LV as normal when not in the LV with ML overlays
                        return 1;
                    }
                    
                    if (!RECORDING)
                    {
                        slim_toggle_dual_iso();
                        return 0;
                    }
                }

                /* False color ON / OFF */
                if (((key == MODULE_KEY_INFO)       && !is_EOSM && INFO_button == 5) ||
                    ((key == MODULE_KEY_PRESS_SET)  && SET_button  == 5))
                {
                    if(lv_disp_mode != 0){
                        // Use INFO key to cycle LV as normal when not in the LV with ML overlays
                        return 1;
                    }
                    
                    extern int falsecolor_draw;
                    if (!falsecolor_draw)
                    {
                        falsecolor_draw = 1;
                        return 0;
                    }
                    if (falsecolor_draw)
                    {
                        falsecolor_draw = 0;
                        redraw();
                        return 0;
                    }
                }
                
#ifdef CONFIG_SLIM_MENUS
                if (tapdisp == 4 && key == MODULE_KEY_TOUCH_1_FINGER)
#else
                if (tapdisp == 5 && key == MODULE_KEY_TOUCH_1_FINGER)
#endif
                {
                    SetGUIRequestMode(0);
                    msleep(100);
                    extern int falsecolor_draw;
                    if (!falsecolor_draw)
                    {
                        falsecolor_draw = 1;
                        return 0;
                    }
                    if (falsecolor_draw)
                    {
                        falsecolor_draw = 0;
                        redraw();
                        return 0;
                    }
                }
            }
            
            
            if (lv_dispsize != 10)
            {
                if ((tapdisp == 2 && key == MODULE_KEY_TOUCH_1_FINGER) || (SET_button == 7 && key == MODULE_KEY_PRESS_SET) || (!is_EOSM && INFO_button == 6 && key == MODULE_KEY_INFO))
                {
                    msleep(100);
                    if(lv_disp_mode != 0){
                        // Use INFO key to cycle LV as normal when not in the LV with ML overlays
                        return 1;
                    }
                    select_menu_by_name("Movie", "Shutter Expo");
                    gui_open_menu();
                    msleep(10);
                    submenu = 1;
                }
                if ((tapdisp == 3 && key == MODULE_KEY_TOUCH_1_FINGER) || (SET_button == 6 && key == MODULE_KEY_PRESS_SET) || (!is_EOSM && INFO_button == 7 && key == MODULE_KEY_INFO))
                {
                    msleep(100);
                    if(lv_disp_mode != 0){
                        // Use INFO key to cycle LV as normal when not in the LV with ML overlays
                        return 1;
                    }
                    select_menu_by_name("Movie", "Aperture Expo");
                    gui_open_menu();
                    msleep(10);
                    submenu = 1;
                }
#ifdef CONFIG_SLIM_MENUS
                if ((SET_button == 8 && key == MODULE_KEY_PRESS_SET) || (!is_EOSM && INFO_button == 8 && key == MODULE_KEY_INFO))
                {
                    msleep(100);
                    if(lv_disp_mode != 0){
                        return 1;
                    }
                    select_menu_by_name("Expo", "ISO");
                    gui_open_menu();
                    msleep(10);
                    submenu = 1;
                }
#else
                if ((tapdisp == 4 && key == MODULE_KEY_TOUCH_1_FINGER) || (SET_button == 8 && key == MODULE_KEY_PRESS_SET) || (INFO_button == 8 && key == MODULE_KEY_INFO))
                {
                    msleep(100);
                    if(lv_disp_mode != 0){
                        // Use INFO key to cycle LV as normal when not in the LV with ML overlays
                        return 1;
                    }
                    select_menu_by_name("Movie", "ISO Expo");
                    gui_open_menu();
                    msleep(10);
                    submenu = 1;
                }
#endif
                if (tapdisp == 1 && key == MODULE_KEY_TOUCH_1_FINGER)
                {
                    msleep(100);
                    if(lv_disp_mode != 0){
                        // Use INFO key to cycle LV as normal when not in the LV with ML overlays
                        return 1;
                    }
                    select_menu_by_name("Movie", "Crop mood");
                    gui_open_menu();
                    msleep(10);
                    submenu = 1;
                }
            }
            
            /* Block SET/Arrows while recording to prevent changing focus box position.
             * When changing focus box position, a part of preview configuration changes, we don't 
             * want that to happen, to avoid corrupted frames, black preview or instability 
             * is there another way to block focus box from shifting, and make its position static? */
            if (RECORDING)
            {
                if (((key == MODULE_KEY_PRESS_SET)   && !SET_button) ||
                    ((key == MODULE_KEY_PRESS_UP)    && !Arrows_U_D) ||
                    ((key == MODULE_KEY_PRESS_DOWN)  && !Arrows_U_D) ||
                    ((key == MODULE_KEY_PRESS_LEFT)  && !Arrows_L_R) ||
                    ((key == MODULE_KEY_PRESS_RIGHT) && !Arrows_L_R))
                {
                    return 0;
                }
            }

            /* Block INFO button while recording to prevent switching among Full and Info preview in 1080i output */
            /* Preview configuration must not change during RAW recording, even Canon block preview switch in H.264 */
            if ((is_1080i_Full_Output() || is_1080i_Info_Output()) && RECORDING)
            {
                if (!INFO_button) // Not assigned?
                {
                    if (key == MODULE_KEY_INFO)
                    {
                        return 0; // Block INFO button
                    }
                }
            }                
        }
    }

    return 1;
}

/* Display recording status in top info bar */
static LVINFO_UPDATE_FUNC(crop_info)
{
    LVINFO_BUFFER(16);
    
    if (patch_active)
    {
        if (lv_dispsize > 1)
        {
            switch (crop_preset)
            {
                case CROP_PRESET_CENTER_Z:
                    snprintf(buffer, sizeof(buffer), "3.5K");
                    break;
                case CROP_PRESET_1X1:
                    if (CROP_2_5K)     snprintf(buffer, sizeof(buffer), "2.5K");
                    if (CROP_2_8K)     snprintf(buffer, sizeof(buffer), "2.8K");
                    if (CROP_3K)       snprintf(buffer, sizeof(buffer), "3K");
                    if (CROP_1440p)    snprintf(buffer, sizeof(buffer), "1440p");
                    if (CROP_1280p)    snprintf(buffer, sizeof(buffer), "1280p");
                    if (CROP_1080p)    snprintf(buffer, sizeof(buffer), "1080p");
                    if (CROP_1620p)    snprintf(buffer, sizeof(buffer), "1620p");
                    if (CROP_Full_Res) snprintf(buffer, sizeof(buffer), "FLV");
                    break;
                case CROP_PRESET_1X3:
                    if (AR_16_9)
                    {
                        if (Anam_Highest) snprintf(buffer, sizeof(buffer), "4.5K");
                        if (Anam_Higher)  snprintf(buffer, sizeof(buffer), "4.2K");
                        if (Anam_Medium)  snprintf(buffer, sizeof(buffer), "UHD");
                    }
                    if (AR_2_1)
                    {
                        if (Anam_Highest) snprintf(buffer, sizeof(buffer), "4.8K");
                        if (Anam_Higher)  snprintf(buffer, sizeof(buffer), "4.4K");
                        if (Anam_Medium)  snprintf(buffer, sizeof(buffer), "4K");
                    }
                    if (AR_2_20_1)
                    {
                        if (Anam_Highest) snprintf(buffer, sizeof(buffer), "5K");
                        if (Anam_Higher)  snprintf(buffer, sizeof(buffer), "4.6K");
                        if (Anam_Medium)  snprintf(buffer, sizeof(buffer), "4.2K");
                    }
                    if (AR_2_35_1 || AR_2_39_1)
                    {
                        if (Anam_Highest) snprintf(buffer, sizeof(buffer), "5.2K");
                        if (Anam_Higher)  snprintf(buffer, sizeof(buffer), "4.8K");
                        if (Anam_Medium)  snprintf(buffer, sizeof(buffer), "4.4K");
                    }
                    break;
                case CROP_PRESET_3X3:
                    if (High_FPS)
                    {
                        if (AR_16_9)    snprintf(buffer, sizeof(buffer), "976p");
                        if (AR_2_1)     snprintf(buffer, sizeof(buffer), "868p");
                        if (AR_2_20_1)  snprintf(buffer, sizeof(buffer), "790p");
                        if (AR_2_35_1)  snprintf(buffer, sizeof(buffer), "738p");
                        if (AR_2_39_1 && crop_preset_fps_reduce == 0)  snprintf(buffer, sizeof(buffer), "694p"); // Actually 2.50:1 AR
                        if (AR_2_39_1 && crop_preset_fps_reduce == 1 && is_EOSM)  snprintf(buffer, sizeof(buffer), "726p");
                    }
                    if (mv1080) snprintf(buffer, sizeof(buffer), "1080p");
                    break;
            }
        }
        else
        {
            switch (crop_preset)
            {
                case CROP_PRESET_3X:
                    /* In movie mode, we are interested in recording sensor pixels
                     * without any binning (that is, with 1:1 mapping);
                     * the actual crop factor varies with raw video resolution.
                     * So, printing 3x is not very accurate, but 1:1 is.
                     * 
                     * In photo mode (mild zoom), what changes is the magnification
                     * of the preview screen; the raw image is not affected.
                     * We aren't actually previewing at 1:1 at pixel level,
                     * so printing 1:1 is a little incorrect.
                     */
                    if (!is_movie_mode())
                    {
                        snprintf(buffer, sizeof(buffer), "3x");
                        goto warn;
                    }
                    break;

                case CROP_PRESET_3X_TALL:
                    snprintf(buffer, sizeof(buffer), "T");
                    break;

                case CROP_PRESET_3K:
                    snprintf(buffer, sizeof(buffer), "3K");
                    break;

                case CROP_PRESET_4K_HFPS:
                    snprintf(buffer, sizeof(buffer), "4K");
                    break;

                case CROP_PRESET_UHD:
                    snprintf(buffer, sizeof(buffer), "UHD");
                    break;

                case CROP_PRESET_FULLRES_LV:
                    snprintf(buffer, sizeof(buffer), "FLV");
                    break;
            }
        }
    }

    /* append info about current binning mode */

    if (raw_lv_is_enabled())
    {
        /* fixme: raw_capture_info is only updated when LV RAW is active */

        /* When not in the zoom-branch naming path above, still name 1620p. */
        if (!buffer[0] && patch_active && crop_preset == CROP_PRESET_1X1 && CROP_1620p)
            snprintf(buffer, sizeof(buffer), "1620p");

        if (raw_capture_info.binning_x + raw_capture_info.skipping_x == 1 &&
            raw_capture_info.binning_y + raw_capture_info.skipping_y == 1)
        {
            STR_APPEND(buffer, "%s1:1", buffer[0] ? " " : "");
        }
        else
        {
            STR_APPEND(buffer, "%s%dx%d",
                buffer[0] ? " " : "",
                raw_capture_info.binning_y + raw_capture_info.skipping_y,
                raw_capture_info.binning_x + raw_capture_info.skipping_x
            );
        }
    }

warn:
    if (crop_rec_needs_lv_refresh())
    {
        if (!streq(buffer, SYM_WARNING))
        {
            STR_APPEND(buffer, " " SYM_WARNING);
        }
        item->color_fg = COLOR_YELLOW;
    }
}

/* Display Bitdepth in ML bottom bar */
static LVINFO_UPDATE_FUNC(bitdepth_info)
{
    LVINFO_BUFFER(8);

    int lossless_format = which_output_format() >= 3;
    /* which_output_format() == 0 means 14-bit uncompressed
     * which_output_format() == 1 means 12-bit uncompressed
     * which_output_format() == 2 means 10-bit uncompressed  */
    if (patch_active && is_movie_mode())
    {
        if ((OUTPUT_14BIT && lossless_format) || which_output_format() == 0) snprintf(buffer, sizeof(buffer), "14 Bit");
        if ((OUTPUT_12BIT && lossless_format) || which_output_format() == 1) snprintf(buffer, sizeof(buffer), "12 Bit");
        if ( OUTPUT_11BIT && lossless_format)                                snprintf(buffer, sizeof(buffer), "11 Bit");
        if ((OUTPUT_10BIT && lossless_format) || which_output_format() == 2) snprintf(buffer, sizeof(buffer), "10 Bit");
        item->color_fg = COLOR_GREEN1;
    }
}

static struct lvinfo_item info_items[] = {
    {
        .name = "Crop info",
        .which_bar = LV_BOTTOM_BAR_ONLY,
        .update = crop_info,
        .preferred_position = -50,  /* near the focal length display */
        .priority = 1,
    },
    {
        .name = "Bitdepth info",
        .which_bar = LV_BOTTOM_BAR_ONLY,
        .update = bitdepth_info,
        .preferred_position = -128,
        .priority = 1,
    }
};

/* better put here too from raw.c since eosm is more or less 100% crop_rec based */
int raw_lv_settings_still_valid()
{
    /* Analog-gain bit depths: fixed whites matching 10/12-bit clip points.
     * 14-bit: keep raw_info.white_level from LV calibration/autodetect —
     * forcing 16200 hid real clipping on zebras/histogram. */
    if (OUTPUT_10BIT) raw_info.white_level = 2870;
    if (OUTPUT_11BIT) raw_info.white_level = 3692;
    if (OUTPUT_12BIT) raw_info.white_level = 5336;
    return 1;
}

static unsigned int raw_info_update_cbr(unsigned int unused)
{
    if (patch_active)
    {
        /* not implemented yet */
        raw_capture_info.offset_x = raw_capture_info.offset_y   = SHRT_MIN;

        if (!is_DIGIC_5) // needed for 700D and similair models
        {
            if (lv_dispsize > 1)
            {
                /* raw backend gets it right */
                return 0;
            }
        }

        /* update horizontal pixel binning parameters */
        switch (crop_preset)
        {
            case CROP_PRESET_3X:
            case CROP_PRESET_3X_TALL:
            case CROP_PRESET_3K:
            case CROP_PRESET_4K_HFPS:
            case CROP_PRESET_UHD:
            case CROP_PRESET_FULLRES_LV:
            case CROP_PRESET_3x1:
            case CROP_PRESET_1X1:
                raw_capture_info.binning_x    = raw_capture_info.binning_y  = 1;
                raw_capture_info.skipping_x   = raw_capture_info.skipping_y = 0;
                break;

            case CROP_PRESET_3x3_1X:
            case CROP_PRESET_3x3_1X_48p:
            case CROP_PRESET_1x3:
            case CROP_PRESET_1X3:
            case CROP_PRESET_3X3:
                raw_capture_info.binning_x = 3; raw_capture_info.skipping_x = 0;
                break;
        }

        /* update vertical pixel binning / line skipping parameters */
        switch (crop_preset)
        {
            case CROP_PRESET_3X:
            case CROP_PRESET_3X_TALL:
            case CROP_PRESET_3K:
            case CROP_PRESET_4K_HFPS:
            case CROP_PRESET_UHD:
            case CROP_PRESET_FULLRES_LV:
            case CROP_PRESET_1x3:
            case CROP_PRESET_1X3:
            case CROP_PRESET_1X1:
                raw_capture_info.binning_y = 1; raw_capture_info.skipping_y = 0;
                break;

            case CROP_PRESET_3x3_1X:
            case CROP_PRESET_3x3_1X_48p:
            case CROP_PRESET_3x1:
            case CROP_PRESET_3X3:
            {
                int b = (is_5D3) ? 3 : 1;
                int s = (is_5D3) ? 0 : 2;
                raw_capture_info.binning_y = b; raw_capture_info.skipping_y = s;
                break;
            }
        }

        if (is_5D3 || is_EOSM)
        {
            /* update skip offsets */
            int skip_left, skip_right, skip_top, skip_bottom;
            calc_skip_offsets(&skip_left, &skip_right, &skip_top, &skip_bottom);
            raw_set_geometry(raw_info.width, raw_info.height, skip_left, skip_right, skip_top, skip_bottom);

            /* crop modes use non-square pixel aspect (e.g. 1x3) */
            raw_set_preview_rect(skip_left + 14, skip_top + 8,
                raw_info.width - skip_left - skip_right - 28,
                raw_info.height - skip_top - skip_bottom - 16,
                1);
            raw_force_aspect_ratio(
                raw_capture_info.binning_x + raw_capture_info.skipping_x,
                raw_capture_info.binning_y + raw_capture_info.skipping_y
            );
        }
    }
    return 0;
}

static unsigned int crop_rec_init()
{
    //Will place afframe so that 2_1 HFR presets will work at least after restart
    if (!is_manual_focus())
    {
        center_lv_afframe();
        msleep(200);
        move_lv_afframe(0, 100);
        msleep(200);
        clear_lv_afframe();
    }
    
    if (is_camera("5D3",  "1.1.3") || is_camera("5D3", "1.2.3"))
    {
        /* same addresses on both 1.1.3 and 1.2.3 */
        CMOS_WRITE = 0x119CC;
        MEM_CMOS_WRITE = 0xE92D47F0;
        
        ADTG_WRITE = 0x11640;
        MEM_ADTG_WRITE = 0xE92D47F0;
        
        ENGIO_WRITE = is_camera("5D3", "1.2.3") ? 0xFF290F98 : 0xFF28CC3C;
        MEM_ENGIO_WRITE = 0xE51FC15C;

        ENG_DRV_OUT = is_camera("5D3", "1.2.3") ? 0xFF290C80 : 0xFF28C92C;

        PathDriveMode = (void *) (is_camera("5D3", "1.2.3") ? 0x56414 : 0x563BC);   /* argument of PATH_SelectPathDriveMode */
        
        is_5D3 = 1;
        crop_presets                = crop_presets_5d3;
        crop_rec_menu[0].choices    = crop_choices_5d3;
        crop_rec_menu[0].max        = COUNT(crop_choices_5d3) - 1;
        crop_rec_menu[0].help       = crop_choices_help_5d3;
        crop_rec_menu[0].help2      = crop_choices_help2_5d3;
        
        fps_main_clock = 24000000;
                                       /* 24p,  25p,  30p,  50p,  60p,   x5 */
        memcpy(default_timerA, (int[]) {  440,  480,  440,  480,  440,  518 }, 24);
        memcpy(default_timerB, (int[]) { 2275, 2000, 1820, 1000,  910, 1556 }, 24);
    }
    else if (is_camera("EOSM", "2.0.2"))
    {
        CMOS_WRITE = 0x2998C;
        MEM_CMOS_WRITE = 0xE92D41F0;
        
        ADTG_WRITE = 0x2986C;
        MEM_ADTG_WRITE = 0xE92D43F8;
        
        ENGIO_WRITE = 0xFF2C19AC;
        MEM_ENGIO_WRITE = 0xE51FC15C;
        
        ENG_DRV_OUT = 0xFF2C1694;
        ENG_DRV_OUTS = 0xFF2C17B8;
        
        PathDriveMode = (void *) 0x892E8;   /* argument of PATH_SelectPathDriveMode */
        PATH_SelectPathDriveMode = 0x14AC4; // it's being called from RAM
        
        EDMAC_9_Vertical_1 = 0x5976C;
        EDMAC_9_Vertical_2 = 0x5979C;
        HIV_Vertical_Photo_Address = 0x4FE5F070;
        HIV_Vertical_Address_hook = 0xFF500D04;
        
        EDID_HDMI_INFO = (void *) 0x821CC;
        
        Shift_x5_LCD = 0xFF96EA3C;
        Shift_x5_HDMI_480p = 0xFF96F3FC;
        Shift_x5_HDMI_1080i_Full = 0xFF96FF54;
        Shift_x5_HDMI_1080i_Info = 0xFF970558;

        Clear_Vram_x5_LCD = 0xFF96EA60;
        Clear_Vram_x5_HDMI_480p = 0xFF96F420;
        Clear_Vram_x5_HDMI_1080i_Full = 0xFF96FF90;
        Clear_Vram_x5_HDMI_1080i_Info = 0xFF970594;
        
        is_EOSM = 1;
        is_DIGIC_5 = 1;
        crop_presets                = crop_presets_DIGIC_5;
        crop_rec_menu[0].choices    = crop_choices_DIGIC_5;
        crop_rec_menu[0].max        = COUNT(crop_choices_DIGIC_5) - 1;
        crop_rec_menu[0].help       = crop_choices_help_DIGIC_5;
    }
    else if (is_camera("700D", "1.1.5") || is_camera("650D", "1.0.4"))
    {
        CMOS_WRITE = 0x17A1C;
        MEM_CMOS_WRITE = 0xE92D41F0;
        
        ADTG_WRITE = 0x178FC;
        MEM_ADTG_WRITE = 0xE92D43F8;
        
        ENGIO_WRITE = is_camera("700D", "1.1.5") ? 0xFF2C2D00 : 0xFF2C0778;
        MEM_ENGIO_WRITE = 0xE51FC15C;
        
        ENG_DRV_OUT = is_camera("700D", "1.1.5") ? 0xFF2C29E8 : 0xFF2C0460;
        ENG_DRV_OUTS = is_camera("700D", "1.1.5") ? 0xFF2C2B0C : 0xFF2C0584;
        
        PathDriveMode = (void *) (is_camera("700D", "1.1.5") ? 0x6B7F4 : 0x6AEC0);   /* argument of PATH_SelectPathDriveMode */
        PATH_SelectPathDriveMode = is_camera("700D", "1.1.5") ? 0xFF19CDD4 : 0xFF19B230;
        
        EDMAC_9_Vertical_1 = is_camera("700D", "1.1.5") ? 0x3E200 : 0x3E120;
        EDMAC_9_Vertical_2 = is_camera("700D", "1.1.5") ? 0x3E230 : 0x3E150;
        HIV_Vertical_Photo_Address = 0x4205E098;
        HIV_Vertical_Address_hook = is_camera("700D", "1.1.5") ? 0xFF4F2A04 : 0xFF4EF224;
        
        EDID_HDMI_INFO = (void *) (is_camera("700D", "1.1.5") ? 0x648B0 : 0x63F7C);
        
        /* I know these look ugly, but we want something works for now, right? , it's not that bad */
        Shift_x5_LCD = is_camera("700D", "1.1.5") ? 0xFF962A74 : 0xFF955894;
        Shift_x5_HDMI_480p = is_camera("700D", "1.1.5") ? 0xFF963434 : 0xFF956254;
        Shift_x5_HDMI_1080i_Full = is_camera("700D", "1.1.5") ? 0xFF963F8C : 0xFF956DAC;
        Shift_x5_HDMI_1080i_Info = is_camera("700D", "1.1.5") ? 0xFF964590 : 0xFF9573B0;

        Clear_Vram_x5_LCD = is_camera("700D", "1.1.5") ? 0xFF962A98 : 0xFF9558B8;
        Clear_Vram_x5_HDMI_480p = is_camera("700D", "1.1.5") ? 0xFF963458 : 0xFF956278;
        Clear_Vram_x5_HDMI_1080i_Full = is_camera("700D", "1.1.5") ? 0xFF963FC8 : 0xFF956DE8;
        Clear_Vram_x5_HDMI_1080i_Info = is_camera("700D", "1.1.5") ? 0xFF9645CC : 0xFF9573EC;
        
        is_650D = 1;
        is_700D = 1;
        is_DIGIC_5 = 1;
        crop_presets                = crop_presets_DIGIC_5;
        crop_rec_menu[0].choices    = crop_choices_DIGIC_5;
        crop_rec_menu[0].max        = COUNT(crop_choices_DIGIC_5) - 1;
        crop_rec_menu[0].help       = crop_choices_help_DIGIC_5;
    }
    else if (is_camera("100D", "1.0.1"))
    {
        CMOS_WRITE = 0x475B8;
        MEM_CMOS_WRITE = 0xE92D41F0;
        
        ADTG_WRITE = 0x47144;
        MEM_ADTG_WRITE = 0xE92D43F8;
        
        ENGIO_WRITE = 0xFF2B2460;
        MEM_ENGIO_WRITE = 0xE51FC15C;
        
        ENG_DRV_OUT = 0xFF2B2148;
        ENG_DRV_OUTS = 0xFF2B226C;
        
        PathDriveMode = (void *) 0xAAEA4;   /* argument of PATH_SelectPathDriveMode */
        PATH_SelectPathDriveMode = 0x19E30; // it's being called from RAM
        
        EDMAC_9_Vertical_1 = 0x77170;
        EDMAC_9_Vertical_2 = 0x771A0;
        HIV_Vertical_Photo_Address = 0x4144F9E4;
        HIV_Vertical_Address_hook = 0xFF50CCD4;
        
        EDID_HDMI_INFO = (void *) 0xA3C0C;
        
        Shift_x5_LCD = 0xFF98F5EC;
        Shift_x5_HDMI_480p = 0xFF98FFAC;
        Shift_x5_HDMI_1080i_Full = 0xFF990B04;
        Shift_x5_HDMI_1080i_Info = 0xFF991108;

        Clear_Vram_x5_LCD = 0xFF98F610;
        Clear_Vram_x5_HDMI_480p = 0xFF98FFD0;
        Clear_Vram_x5_HDMI_1080i_Full = 0xFF990B40;
        Clear_Vram_x5_HDMI_1080i_Info = 0xFF991144;
        
        is_100D = 1;
        is_DIGIC_5 = 1;
        crop_presets                = crop_presets_DIGIC_5;
        crop_rec_menu[0].choices    = crop_choices_DIGIC_5;
        crop_rec_menu[0].max        = COUNT(crop_choices_DIGIC_5) - 1;
        crop_rec_menu[0].help       = crop_choices_help_DIGIC_5;
        
        fps_main_clock = 32000000;
        
                                       /* 24p,  25p,  30p,  50p,  60p,   x5, c24p, c25p, c30p */
        memcpy(default_timerA, (int[]) {  520,  640,  520,  640,  528,  732,  600,  576,  600 }, 36);
        memcpy(default_timerB, (int[]) { 2566, 2000, 2053, 1000, 1011, 1460, 2224, 2222, 1779 }, 36);
                                   /* or 2567        2054        1012        2225  2223  1780 */
    }       
    else if (is_camera("6D", "1.1.6"))
    {
        CMOS_WRITE = 0x2420C;
        MEM_CMOS_WRITE = 0xE92D41F0;        
        
        ADTG_WRITE = 0x24108;
        MEM_ADTG_WRITE = 0xE92D41F0;
        
        ENG_DRV_OUT = 0xFF2ADE1C;
        
        PathDriveMode = (void *) 0xB5D1C;   /* argument of PATH_SelectPathDriveMode */
        
        is_6D = 1;
        is_basic = 1;
        crop_presets                = crop_presets_basic;
        crop_rec_menu[0].choices    = crop_choices_basic;
        crop_rec_menu[0].max        = COUNT(crop_choices_basic) - 1;
        crop_rec_menu[0].help       = crop_choices_help_basic;
        crop_rec_menu[0].help2      = crop_choices_help2_basic;
        
        fps_main_clock = 25600000;
                                       /* 24p,  25p,  30p,  50p,  60p,   x5 */
        memcpy(default_timerA, (int[]) {  546,  640,  546,  640,  520,  730 }, 24);
        memcpy(default_timerB, (int[]) { 1955, 1600, 1564,  800,  821, 1172 }, 24);
                                   /* or 1956        1565         822        2445        1956 */
    }
    else if (is_camera("70D", "1.1.2"))
    {
        CMOS_WRITE = 0x26B54;
        MEM_CMOS_WRITE = 0xE92D41F0;        

        ADTG_WRITE = 0x2684C;
        MEM_ADTG_WRITE = 0xE92D47F0;
        
        ENGIO_WRITE = 0xFF2BC6C4;
        MEM_ENGIO_WRITE = 0xE51FC15C;
        
        ENG_DRV_OUT = 0xFF2BC3AC;

        PathDriveMode = (void *) 0xD945C;   /* argument of PATH_SelectPathDriveMode */

        is_70D = 1;
        crop_presets                = crop_presets_70d;
        crop_rec_menu[0].choices    = crop_choices_70d;
        crop_rec_menu[0].max        = COUNT(crop_choices_70d) - 1;
        crop_rec_menu[0].help       = crop_choices_help_70d;
        crop_rec_menu[0].help2      = crop_choices_help2_70d;
        
        fps_main_clock = 32000000;
                                       /* 24p,  25p,  30p,  50p,  60p,   x5   c24p, c25p, c30p */
        memcpy(default_timerA, (int[]) {  700,  800,  700,  800,  672,  672,  462,  500,  462 }, 36);
        memcpy(default_timerB, (int[]) { 1906, 1600, 1525,  800,  794, 1588, 2888, 2560, 2311 }, 36);
                                   /* or 1907        1526         795        2889        2312  */
    }
    
    /* default FPS timers are the same on all these models */
    if (is_EOSM || is_700D || is_650D)
    {
        fps_main_clock = 32000000;
                                       /* 24p,  25p,  30p,  50p,  60p,   x5, c24p, c25p, c30p */
        memcpy(default_timerA, (int[]) {  528,  640,  528,  640,  528,  716,  546,  640,  546 }, 36);
        memcpy(default_timerB, (int[]) { 2527, 2000, 2022, 1000, 1011, 1491, 2444, 2000, 1955 }, 36);
                                   /* or 2528        2023        1012        2445        1956 */
    }

    /* FPS in x5 zoom may be model-dependent; assume exact */
    default_fps_1k[5] = (uint64_t) fps_main_clock * 1000ULL / default_timerA[5] / default_timerB[5];

    printf("[crop_rec] checking FPS timer values...\n");
    for (int i = 0; i < COUNT(default_fps_1k); i++)
    {
        if (default_timerA[i])
        {
            int fps_i = (uint64_t) fps_main_clock * 1000ULL / default_timerA[i] / default_timerB[i];
            if (fps_i == default_fps_1k[i])
            {
                printf("%d) %s%d.%03d: A=%d B=%d (exact)\n", i, FMT_FIXEDPOINT3(default_fps_1k[i]), default_timerA[i], default_timerB[i]);

                if (i == 5 && default_fps_1k[i] != 29970)
                {
                    printf("-> unusual FPS in x5 zoom\n", i);
                }
            }
            else
            {
                int fps_p = (uint64_t) fps_main_clock * 1000ULL / default_timerA[i] / (default_timerB[i] + 1);
                if (fps_i > default_fps_1k[i] && fps_p < default_fps_1k[i])
                {
                    printf("%d) %s%d.%03d: A=%d B=%d/%d (averaged)\n", i, FMT_FIXEDPOINT3(default_fps_1k[i]), default_timerA[i], default_timerB[i], default_timerB[i] + 1);
                }
                else
                {
                    printf("%d) %s%d.%03d: A=%d B=%d (%s%d.%03d ?!?)\n", i, FMT_FIXEDPOINT3(default_fps_1k[i]), default_timerA[i], default_timerB[i], FMT_FIXEDPOINT3(fps_i));
                    return CBR_RET_ERROR;
                }

                /* assume 25p is exact on all models */
                if (i == 1)
                {
                    printf("-> 25p check error\n");
                    return CBR_RET_ERROR;
                }
            }
        }
    }

    if (is_DIGIC_5)
    {
        /* hide 1080p preset for 650D / 700D / 100D (they don't need it) */
        if (!is_EOSM)
        {
            crop_rec_menu[0].children[2].max = 0;
            crop_preset_3x3_res_menu = 0;
        }
    }

    if (is_EOSM)
    {
        slim_crop_sync_from_backend();
        /* Slim Movie: never leave Mode on OFF. */
        if (crop_preset_index < 1 || crop_preset_index > 3)
            crop_preset_index = 1;
        /* Derive Mode UI (incl. LV) then push 1x1 combo / Full-Res. */
        slim_crop_sync_from_backend();
        slim_crop_apply_mode();
        slim_crop_apply_bit_depth();
        more_hacks = 1;

        /* Preserve SET Button choice; migrate old assignments to x10 zoom. */
        if (SET_button != 1 && SET_button != 2)
            SET_button = 1;
        Half_Shutter = 0; /* EOS M slim: half-shutter x10 only via Shutter zoom setting */

        /* One-time remap: old arrow/INFO meanings → new Settings options. */
        if (button_map_v < 1)
        {
            /* Old arrows: 0=OFF, 1=ISO, 2=Aperture → New: 0=OFF, 1=Shutter, 2=Aperture, 3=ISO */
            if (Arrows_U_D == 1) Arrows_U_D = 3;
            if (Arrows_L_R == 1) Arrows_L_R = 3;
            /* Old INFO: 0=OFF,1=Aperture,2=FC,3=DualISO,4=framing
             * New INFO: 0=OFF,1=DualISO,2=Hist,3=Wave,4=FC,5=framing */
            if (INFO_button == 1) INFO_button = 0;
            else if (INFO_button == 2) INFO_button = 4;
            else if (INFO_button == 3) INFO_button = 1;
            else if (INFO_button == 4) INFO_button = 5;
            button_map_v = 1;
        }
        /* v1 INFO: 4=False Color, 5=framing → v2: 4=Zebras, 5=False Color, 6=framing */
        if (button_map_v < 2)
        {
            if (INFO_button == 5) INFO_button = 6;
            else if (INFO_button == 4) INFO_button = 5;
            button_map_v = 2;
        }
        if (Arrows_U_D < 0 || Arrows_U_D > 3) Arrows_U_D = 3;
        /* Slim: no Left/Right Button remap — leave L/R to Canon. */
        Arrows_L_R = 0;
        if (INFO_button < 0 || INFO_button > 7) INFO_button = 0;

        /* Flat Movie-page crop settings (no Crop Mode submenu / Customize Buttons). */
        menu_add("Movie", crop_rec_menu_eosm, COUNT(crop_rec_menu_eosm));
        menu_add("Expo", expo_shutter_range_eosm, COUNT(expo_shutter_range_eosm));
        menu_add("Settings", slim_info_button_menu, COUNT(slim_info_button_menu));
        anamorphic_preview_add_slim_menu();
        menu_add("Settings", afdiag_menu, COUNT(afdiag_menu));
        menu_move_entry_after("Settings", "AF diag log", "Shutter zoom");
        menu_move_entry_after("Settings", "AF lens test", "AF diag log");
        task_create("afdiag_lens", 0x1e, 0x2000, afdiag_lens_task, (void*)0);
        menu_add("Settings", slim_more_hacks_menu, COUNT(slim_more_hacks_menu));
        lvinfo_add_items(info_items, COUNT(info_items));
        return 0;
    }

    if (!is_EOSM)
        menu_add("Movie", movie_menu_fps, COUNT(movie_menu_fps));
    menu_add("Movie", movie_menu_bitdepth, COUNT(movie_menu_bitdepth));
    menu_add("Movie", crop_rec_menu, COUNT(crop_rec_menu));
    menu_add("Movie", customize_buttons_menu, COUNT(customize_buttons_menu));
    menu_add("Movie", movie_menu_shutter_range, COUNT(movie_menu_shutter_range));
    menu_add("Movie", movie_menu_framerate, COUNT(movie_menu_framerate));
    menu_add("Movie", movie_menu_ratio, COUNT(movie_menu_ratio));
    lvinfo_add_items (info_items, COUNT(info_items));

    return 0;
}

static unsigned int crop_rec_deinit()
{
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(crop_rec_init)
    MODULE_DEINIT(crop_rec_deinit)
MODULE_INFO_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(crop_preset_index)
    MODULE_CONFIG(fps_over)
    MODULE_CONFIG(shutter_range)
    MODULE_CONFIG(bit_depth_analog)
    MODULE_CONFIG(crop_preset_1x1_res_menu)
    MODULE_CONFIG(crop_preset_1x3_res_menu)
    MODULE_CONFIG(crop_preset_3x3_res_menu)
    MODULE_CONFIG(crop_preset_ar_menu)
    MODULE_CONFIG(crop_preset_fps_menu)
    MODULE_CONFIG(crop_preset_fps_reduce)
    MODULE_CONFIG(fix_dual_iso_flicker)
    MODULE_CONFIG(brighten_lv_method)
    MODULE_CONFIG(Half_Shutter)
    MODULE_CONFIG(SET_button)
    MODULE_CONFIG(INFO_button)
    MODULE_CONFIG(Shutter_zoom)
    MODULE_CONFIG(Auto_refocus)
    MODULE_CONFIG(Auto_refocus_sens)
    MODULE_CONFIG(tapdisp)
    MODULE_CONFIG(Arrows_L_R)
    MODULE_CONFIG(Arrows_U_D)
    MODULE_CONFIG(more_hacks)
    MODULE_CONFIG(button_map_v)
MODULE_CONFIGS_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_SHOOT_TASK, crop_rec_polling_cbr, 0)
    MODULE_CBR(CBR_RAW_INFO_UPDATE, raw_info_update_cbr, 0)
    MODULE_CBR(CBR_KEYPRESS, crop_rec_keypress_cbr, 0)
    MODULE_CBR(CBR_KEYPRESS, photo_keypress_cbr, 0)
MODULE_CBRS_END()

MODULE_PROPHANDLERS_START()
    MODULE_PROPHANDLER(PROP_LV_ACTION)
    MODULE_PROPHANDLER(PROP_LV_DISPSIZE)
    MODULE_PROPHANDLER(PROP_LV_FOCUS_DONE)
    MODULE_PROPHANDLER(PROP_LV_FOCUS_PULSE_COUNT)
    MODULE_PROPHANDLER(PROP_LV_LENS)
    MODULE_PROPHANDLER(PROP_LV_AF_RESULT)
    MODULE_PROPHANDLER(PROP_LV_AFFRAME)
    MODULE_PROPHANDLER(PROP_LV_FOCUS_STATE)
    MODULE_PROPHANDLER(PROP_LV_FOCUS_STATUS)
    MODULE_PROPHANDLER(PROP_LV_FOCUS_DATA)
    MODULE_PROPHANDLER(PROP_LV_FOCUS_CMD)
    MODULE_PROPHANDLER(PROP_LVCAF_STATE)
    MODULE_PROPHANDLER(PROP_LVAF_001D)
    MODULE_PROPHANDLER(PROP_LV_LENS_DRIVE_REMOTE)
    MODULE_PROPHANDLER(PROP_HALF_SHUTTER)
    MODULE_PROPHANDLER(PROP_MOVIE_SERVO_AF)
    MODULE_PROPHANDLER(PROP_LIVE_VIEW_AF_SYSTEM)
MODULE_PROPHANDLERS_END()
