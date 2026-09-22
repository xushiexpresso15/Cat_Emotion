/*
 * imx519_af.c
 *
 * Sony IMX519 Continuous Auto-Focus (AF-C) Controller with AK7375 VCM Driver
 * for Himax WiseEye2 (WE2 HX6538) on Seeed Grove Vision AI Module V2
 */

#include "imx519_af.h"
#include "cisdp_sensor.h"
#include "cisdp_cfg.h"
#include "hx_drv_CIS_common.h"
#include "hx_drv_iic.h"
#include "hx_drv_timer.h"
#include "WE2_core.h"
#include "WE2_debug.h"

#define AF_DBG(fmt, ...) dbg_printf(DBG_LESS_INFO, "[AF] " fmt, ##__VA_ARGS__)

/*
 * AK7375 VCM I2C protocol constants, verified against the Linux kernel
 * driver (drivers/media/i2c/ak7375.c, ak7375_cdef):
 *   - reg_position = 0x00 (2-byte register): the 12-bit focus code must be
 *     LEFT-SHIFTED BY 4 before being written (occupies bits [15:4]).
 *   - reg_cont     = 0x02 (1-byte register): 0x00 = active, 0x40 = standby.
 *     The VCM powers up in/returns to standby and will NOT respond to
 *     position writes until this is written once.
 *   - power_delay_us = 10000: wait ~10ms after the VCM rail is up before
 *     sending any I2C to it.
 * Register addresses are 1 BYTE wide here �X unlike the Sony CIS registers
 * (2-byte addresses) used elsewhere in this project via hx_drv_cis_set_reg().
 * Do NOT reuse that CIS helper for the VCM; the address width mismatch is
 * the most likely cause of the earlier EDM WDT2 TIMEOUT crash.
 *
 * SLAVE ADDRESS FORMAT - read this before "fixing" the calls below.
 * hx_drv_iic.h documents the parameter as "slave_addr_sft ... the slave
 * address shifted by 1 bit to the left", and names it _sft. That doc is
 * WRONG: the driver takes the plain 7-bit address and masks bit 7 itself.
 * Proven on hardware by imx519_i2c_bus_scan() - probing with (addr << 1)
 * reported ACKs at 0x0D/0x46/0x28, whose shifted values 0x1A/0x8C/0xD0 mask
 * down to 0x1A (the sensor), 0x0C (this VCM) and 0x50 (the module EEPROM).
 * The rest of the SDK agrees: hx_drv_cis_set_slaveID() is handed 0x1A for
 * IMX519 and 0x24 for HM0360, both unshifted 7-bit.
 * Passing (0x0C << 1) = 0x18 addressed an empty slot, which is why every
 * VCM write returned -60 (HX_CIS_I2C_ADDR_NO_ACK) and the lens never moved.
 */
#define AK7375_REG_POSITION      (0x00)
#define AK7375_REG_CONTROL       (0x02)
#define AK7375_MODE_ACTIVE       (0x00)
#define AK7375_MODE_STANDBY      (0x40)
#define AK7375_POWER_DELAY_MS    (10)

/*
 * Which I2C master bus the VCM is on. The AK7375 sits on the SAME 2-wire
 * bus as the IMX519 sensor on the camera module's flex cable, so this
 * should be whichever USE_DW_IIC_x instance the CIS driver already uses
 * for the sensor (per hx_drv_iic.h, USE_DW_IIC_1 is documented as "for
 * image sensor use"). ASSUMPTION �X if the VCM never ACKs, this is the
 * first thing to double check (try USE_DW_IIC_0 instead).
 */
#define AK7375_VCM_I2C_MASTER    USE_DW_IIC_1

/*
 * Search range covers the AK7375's full 12-bit travel (0 ~ 4095).
 *
 * These were previously 300..980 step 20, which is only 17% of the travel and
 * all of it bunched at the near end - a leftover from when the DAC was assumed
 * to be 10-bit (0..1023), where 300..980 really would have been end to end.
 * The header was later corrected to 12-bit and IMX519_AF_DAC_DEFAULT set to
 * 2400, but these were never rescaled, leaving the default position sitting
 * outside the range the sweep could even reach. The lens never explored its
 * mid or far travel, which is why no subject distance ever came into focus.
 *
 * Step counts are kept close to the original so a sweep takes about as long:
 * coarse 32 steps, fine 17.
 */
#define COARSE_START_DAC        IMX519_AF_DAC_MIN
#define COARSE_END_DAC          IMX519_AF_DAC_MAX
/*
 * 256 samples the measured peak (full width at half maximum about 540 DAC)
 * about twice within its half-width, which is enough to find it, and halves
 * the coarse pass from 32 steps to 16. The fine pass range below must stay
 * equal to this step so it covers the interval the coarse pass skipped.
 */
#define COARSE_STEP_DAC         256

/*
 * Fine search parameters: refine around the coarse peak.
 *
 * Measured focus curve (textured target, 2026-09-21): peak 102k-108k at
 * DAC ~1300 against a 4k floor, full width at half maximum about 540 DAC. The
 * old step of 16 sampled that width 34 times, far finer than the optics can
 * show - the score barely changes within +/-32 DAC of the peak. The range is
 * wider than the coarse step (128), so the fine pass also covers the
 * neighbouring coarse intervals.
 */
/*
 * The fine range MUST equal the coarse step: the true peak can sit anywhere
 * within one coarse step of the best coarse sample. Measured 2026-09-21 at a
 * 40 cm target with coarse 256 / fine +/-128: the coarse pass returned 1280
 * (110k) with 1536 almost as high (105k), and the fine pass then rose all the
 * way to its last point, 1408 (128k), and locked there - on the slope, with
 * the real peak outside the window. Tied to COARSE_STEP_DAC so the two cannot
 * drift apart again.
 */
#define FINE_HALF_RANGE_DAC     COARSE_STEP_DAC
#define FINE_STEP_DAC           64

/*
 * Golden-section search parameters.
 *
 * The measured focus curve is a single clean peak - 15 cm, 40 cm and 1 m each
 * gave one maximum, 5x to 12x above the floor - so the bracket containing the
 * peak can be narrowed instead of sampling the whole travel: every measurement
 * discards the part that cannot hold the maximum, leaving 0.618 of the
 * interval. From the full 4096 down to GS_TOL_DAC that is about 10 iterations
 * plus the 2 initial probes, against 25 measurements for coarse + fine.
 *
 * The assumption is the risk: two subjects at different distances can give two
 * peaks, and narrowing may converge on the weaker one where a full sweep would
 * find the higher. Key '0' switches between the three searches.
 */
#define GS_TOL_DAC              64      /* stop when the bracket is this wide */
#define GS_MAX_EVALS            24      /* safety stop */
#define GS_FRAC_NEAR            382     /* 1 - 0.618, in thousandths */
#define GS_FRAC_FAR             618

/*
 * Hybrid (the default): a coarse pass over the whole travel picks the region,
 * then the bracket is narrowed inside it.
 *
 * Replaying both searches on the host over synthetic curves (af_sim.py) showed
 * where plain narrowing breaks: two peaks more than about 800 DAC apart - one
 * subject nearer than ~20 cm plus another around 1 m - with the near one at
 * least as strong. The early comparisons then point away from the higher peak
 * and it locks on the lower one, at 52% to 84% of it. Peaks closer together
 * than that merge into one maximum and narrowing stays correct, which is why
 * the 20/40/100 cm bench scene could not reproduce it.
 *
 * The coarse pass is what survives several peaks: with a step no larger than
 * the curve's width at half height (measured 448-768 DAC), every peak gets at
 * least one sample above its own half height, so no peak can be stepped over.
 * 512 costs 8 samples and leaves the narrowing 6-7, about 15 in total - still
 * well under the 25 of coarse + fine, and correct on every case in af_sim.py.
 */
#define HYBRID_COARSE_STEP_DAC  512

/*
 * Re-focus after the subject moves: narrow within this much either side of the
 * lens position that was locked, instead of searching the whole travel again.
 *
 * Measured 2026-09-21 with a subject moved between 1 m and 40 cm: the lens had
 * to travel 276 and 155 DAC. 512 covers a 1 m subject moving anywhere from
 * about 35 cm to infinity, and costs 7 narrowing measurements (~1.6 s) instead
 * of the full search's 16 (~3.5 s). A move that lands outside the window shows
 * up as a weak peak and escalates to the full search, so nothing is lost
 * except the time already spent.
 */
#define REFOCUS_HALF_RANGE_DAC  512

/* Settle frame count after DAC change (wait 1 frame for lens movement & exposure) */
#define AF_SETTLE_FRAMES        1

/* Continuous AF (AF-C) Defocus Detection Parameters */
#define DEFOCUS_DROP_THRESHOLD_PCT  65   /* Defocus triggered if score < 65% of locked score */
#define DEFOCUS_CONFIRM_FRAMES      6    /* Must persist for 6 consecutive frames */
#define LOCK_COOLDOWN_FRAMES        15   /* Cooldown frames after locking before monitoring */

/*
 * A large RISE also means the scene changed, and has to re-focus as well.
 *
 * Watching only for a drop is not symmetric, and the gap it leaves was reached
 * in testing 2026-09-21: the colour-bar pattern has no detail in the G plane,
 * every position scored 0, the search locked at DAC 0 with locked_score 0 -
 * and 65% of 0 is 0, so no frame could ever be "below" it again. The lens
 * stayed at 0 after the scene came back. A blank wall or a dark room reaches
 * the same state with a small score rather than zero: whatever appears later
 * only makes the score RISE, which the old test could not see.
 */
#define SCENE_RISE_THRESHOLD_PCT    250  /* Re-focus if score > 2.5x the locked score */

/*
 * A search only means something if the curve has a peak. On a scene with no
 * detail every position scores about the same, and locking on the highest of
 * those samples puts the lens somewhere arbitrary. Measured peaks stand 4.8x
 * to 12.6x above the flattest sample of the same search, so 1.5x is far below
 * anything real and still well clear of frame noise.
 */
#define FLAT_CURVE_RATIO_PCT        150

/* AF Controller State Structure */
typedef struct {
    imx519_af_state_t state;
    bool     continuous_enabled;
    uint16_t current_dac;
    uint8_t  settle_counter;

    /* Coarse sweep state */
    uint16_t coarse_dac;
    uint16_t best_coarse_dac;
    uint32_t max_coarse_score;

    /* Fine sweep state */
    uint16_t fine_start_dac;
    uint16_t fine_end_dac;
    uint16_t fine_dac;
    uint16_t best_fine_dac;
    uint32_t max_fine_score;

    /* Lowest score seen during the search in progress, and the lens position
     * it started from: a search that turns out to be flat leaves the lens
     * where it was instead of moving it somewhere arbitrary. */
    uint32_t min_score;
    uint16_t entry_dac;

    /* Continuous monitoring state */
    uint32_t locked_score;
    uint16_t lock_cooldown;
    uint8_t  defocus_counter;
    uint8_t  rise_counter;
    uint32_t scan_count;
} imx519_af_ctrl_t;

static imx519_af_ctrl_t g_af_ctrl = {
    .state = IMX519_AF_STATE_IDLE,
    .continuous_enabled = true,
    .current_dac = IMX519_AF_DAC_DEFAULT,
    .settle_counter = 0,
    .coarse_dac = COARSE_START_DAC,
    .best_coarse_dac = IMX519_AF_DAC_DEFAULT,
    .max_coarse_score = 0,
    .fine_start_dac = 0,
    .fine_end_dac = 0,
    .fine_dac = 0,
    .best_fine_dac = IMX519_AF_DAC_DEFAULT,
    .max_fine_score = 0,
    .min_score = 0xFFFFFFFFU,
    .entry_dac = IMX519_AF_DAC_DEFAULT,
    .locked_score = 0,
    .lock_cooldown = 0,
    .defocus_counter = 0,
    .rise_counter = 0,
    .scan_count = 0,
};

/* Golden-section search state (see GS_* above). */
static struct {
    uint16_t lo, hi;        /* current bracket */
    uint16_t x1, x2;        /* interior probes, x1 < x2 */
    uint32_t f1, f2;        /* their scores */
    uint16_t best_dac;      /* best position actually measured, and its score */
    uint32_t best_score;
    uint8_t  pending;       /* which probe the next measurement belongs to */
    uint8_t  evals;
} g_gs;

static imx519_af_algo_t g_af_algo = IMX519_AF_ALGO_HYBRID;

/* True while the narrowing in progress is an AF-C re-focus in a window around
 * the locked position, rather than a full search. Such a run has to be allowed
 * to fail: if the subject moved further than the window, the best score inside
 * it is poor and the full search has to take over. */
static bool g_gs_refocus = false;

/* Coarse step of the run in progress: the sweep and the hybrid pass over the
 * travel with different steps, and the value is needed again when the pass
 * ends to size the window that follows it. */
static uint16_t g_coarse_step = HYBRID_COARSE_STEP_DAC;

static const char *imx519_af_algo_name(imx519_af_algo_t a)
{
    return (a == IMX519_AF_ALGO_SWEEP)  ? "coarse+fine sweep" :
           (a == IMX519_AF_ALGO_GOLDEN) ? "golden section (whole travel)" :
                                          "hybrid: coarse pass + narrowing";
}

void imx519_af_set_algorithm(imx519_af_algo_t algo)
{
    g_af_algo = (algo > IMX519_AF_ALGO_HYBRID) ? IMX519_AF_ALGO_HYBRID : algo;
    AF_DBG("search algorithm: %s\n", imx519_af_algo_name(g_af_algo));
}

imx519_af_algo_t imx519_af_get_algorithm(void)
{
    return g_af_algo;
}

void imx519_af_cycle_algorithm(void)
{
    imx519_af_set_algorithm((imx519_af_algo_t)((g_af_algo + 1) % 3));
}

/*
 * Start narrowing within [lo, hi]. Used both for the whole travel and for the
 * window the hybrid's coarse pass hands over.
 */
static void imx519_af_gs_begin(uint16_t lo, uint16_t hi)
{
    uint32_t span = (uint32_t)hi - lo;

    g_gs.lo = lo;
    g_gs.hi = hi;
    g_gs.x1 = (uint16_t)(lo + span * GS_FRAC_NEAR / 1000U);
    g_gs.x2 = (uint16_t)(lo + span * GS_FRAC_FAR / 1000U);
    g_gs.f1 = 0;
    g_gs.f2 = 0;
    g_gs.best_dac = g_gs.x1;
    g_gs.best_score = 0;
    g_gs.pending = 1;
    g_gs.evals = 0;
    g_af_ctrl.settle_counter = AF_SETTLE_FRAMES;
    imx519_vcm_set_focus(g_gs.x1);
    g_af_ctrl.state = IMX519_AF_STATE_GOLDEN_SEARCH;
    AF_DBG("Narrowing bracket [%u ~ %u], probes %u / %u\n",
           (unsigned)lo, (unsigned)hi, (unsigned)g_gs.x1, (unsigned)g_gs.x2);
}

/* Tracks whether we've successfully sent the wake/active command to the
 * VCM yet this power cycle. Starts false; set true on first successful
 * wake, reset to false on any write failure so the next call retries. */
static bool g_vcm_active = false;

/*
 * Wake the AK7375 from standby by writing its 1-byte mode-control register.
 * Must succeed before any position write will have an effect - the chip
 * ignores position writes while in standby.
 */
static bool imx519_vcm_wake(void)
{
    uint8_t addr[1] = { AK7375_REG_CONTROL };
    uint8_t data[1] = { AK7375_MODE_ACTIVE };
    IIC_ERR_CODE_E ret;

    ret = hx_drv_i2cm_write_data(AK7375_VCM_I2C_MASTER,
                                  AK7375_VCM_I2C_ADDR,
                                  addr, 1, data, 1);
    if (ret != IIC_ERR_OK) {
        AF_DBG("VCM wake FAILED (I2C err=%d) - check AK7375_VCM_I2C_MASTER / wiring\n", (int)ret);
        g_vcm_active = false;
        return false;
    }

    g_vcm_active = true;
    AF_DBG("VCM woken from standby (reg 0x02 = 0x00)\n");
    return true;
}

/* VCM Focus: writes the 12-bit position to the AK7375 over I2C.
 * Protocol verified against the Linux ak7375.c driver - see the constants
 * block near the top of this file for details. */
void imx519_vcm_set_focus(uint16_t focus_dac)
{
    uint8_t addr[1] = { AK7375_REG_POSITION };
    uint8_t data[2];
    uint16_t reg_val;
    IIC_ERR_CODE_E ret;

    if (focus_dac > IMX519_AF_DAC_MAX) {
        focus_dac = IMX519_AF_DAC_MAX;
    }

    if (!g_vcm_active) {
        if (!imx519_vcm_wake()) {
            /* Hardware isn't responding - don't update current_dac, since
             * that would claim the lens moved when it didn't. */
            return;
        }
    }

    /* 12-bit code occupies bits [15:4] of the 16-bit position register. */
    reg_val = (uint16_t)(focus_dac << 4);
    data[0] = (uint8_t)(reg_val >> 8);    /* MSB first, per hx_drv_i2cm_write_data byte order */
    data[1] = (uint8_t)(reg_val & 0xFF);  /* LSB */

    ret = hx_drv_i2cm_write_data(AK7375_VCM_I2C_MASTER,
                                  AK7375_VCM_I2C_ADDR,
                                  addr, 1, data, 2);
    if (ret != IIC_ERR_OK) {
        AF_DBG("VCM focus write FAILED (DAC=%d, I2C err=%d)\n", focus_dac, (int)ret);
        /* Mark inactive so the next call re-sends the wake command first -
         * covers the case where the VCM dropped back to standby/lost power. */
        g_vcm_active = false;
        return;
    }

    g_af_ctrl.current_dac = focus_dac;
    dbg_printf(DBG_LESS_INFO, "[VCM] DAC=%d -> reg=0x%04X\n", focus_dac, reg_val);
}


uint16_t imx519_af_get_current_dac(void)
{
    return g_af_ctrl.current_dac;
}

void imx519_af_set_continuous(bool enable)
{
    g_af_ctrl.continuous_enabled = enable;
    AF_DBG("Continuous AF (AF-C) %s\n", enable ? "ENABLED" : "DISABLED");
}

bool imx519_af_is_continuous(void)
{
    return g_af_ctrl.continuous_enabled;
}

void imx519_af_init(void)
{
    g_af_ctrl.state = IMX519_AF_STATE_IDLE;
    g_af_ctrl.continuous_enabled = true;
    g_af_ctrl.current_dac = IMX519_AF_DAC_DEFAULT;
    g_af_ctrl.best_fine_dac = IMX519_AF_DAC_DEFAULT;
    g_af_ctrl.defocus_counter = 0;
    g_af_ctrl.lock_cooldown = 0;
    g_vcm_active = false;

    /* Per the AK7375 datasheet / Linux driver power_delay_us, wait for the
     * VCM rail to stabilize before sending it any I2C. If the sensor/VCM
     * power was already up before this point, this is a harmless extra
     * wait, not a correctness requirement. */
    hx_drv_timer_cm55x_delay_ms(AK7375_POWER_DELAY_MS, TIMER_STATE_DC);

    if (imx519_vcm_wake()) {
        /* Move to the default position so the lens starts somewhere sane
         * rather than wherever it happened to settle mechanically. */
        imx519_vcm_set_focus(IMX519_AF_DAC_DEFAULT);
        AF_DBG("VCM online, default DAC = %d\n", IMX519_AF_DAC_DEFAULT);
    } else {
        AF_DBG("VCM did NOT respond at init - AF will run in software-only "
               "mode (scores computed, lens won't move) until a write succeeds\n");
    }
}

/* Called at the start of every AF run, full or local. */
static void imx519_af_run_begin(void)
{
    g_af_ctrl.min_score = 0xFFFFFFFFU;
    /* Only when a run starts from rest. A re-focus that escalates to a full
     * search must keep the position it had before the episode began, or the
     * "nothing to focus on" fallback would park the lens wherever the failed
     * search happened to leave it instead of at the last good focus. */
    if (g_af_ctrl.state == IMX519_AF_STATE_LOCKED ||
        g_af_ctrl.state == IMX519_AF_STATE_IDLE) {
        g_af_ctrl.entry_dac = g_af_ctrl.current_dac;
    }
    g_af_ctrl.defocus_counter = 0;
    g_af_ctrl.rise_counter = 0;
}

/*
 * A search that found no peak says nothing about where to put the lens: on a
 * scene with no detail every sample scores about the same and the "best" of
 * them is arbitrary. Locking there was how the lens got stranded at DAC 0 in
 * the colour-bar test (see SCENE_RISE_THRESHOLD_PCT).
 */
static bool imx519_af_curve_is_flat(uint32_t best_score)
{
    if (g_af_ctrl.min_score == 0xFFFFFFFFU) {
        return false;                      /* nothing measured; don't judge */
    }
    if (best_score == 0) {
        /* The ratio test cannot see this one: 0 is not less than 1.5 x 0, so a
         * curve that is zero everywhere would read as a peak. That is exactly
         * what the colour-bar scene produces. */
        return true;
    }
    return best_score < ((uint64_t)g_af_ctrl.min_score * FLAT_CURVE_RATIO_PCT) / 100U;
}

/*
 * Stay where we were, and keep monitoring with whatever the scene scores now.
 * Recovery is the rise test: as soon as something with detail appears, the
 * score climbs past SCENE_RISE_THRESHOLD_PCT and a re-focus starts.
 */
static void imx519_af_hold_without_lock(uint32_t best_score)
{
    imx519_vcm_set_focus(g_af_ctrl.entry_dac);
    /* Reference the flattest sample, not the best one. On a flat curve the two
     * are within FLAT_CURVE_RATIO_PCT of each other, so taking the best would
     * leave the current frame sitting near the drop threshold and the search
     * would keep re-triggering on nothing. */
    g_af_ctrl.locked_score = g_af_ctrl.min_score;
    g_af_ctrl.lock_cooldown = LOCK_COOLDOWN_FRAMES;
    g_af_ctrl.defocus_counter = 0;
    g_af_ctrl.rise_counter = 0;
    g_af_ctrl.state = IMX519_AF_STATE_LOCKED;
    AF_DBG("NO PEAK: best %lu vs flattest %lu - nothing to focus on. Holding DAC %u "
           "and waiting for the scene to change.\n",
           best_score, g_af_ctrl.min_score, (unsigned)g_af_ctrl.entry_dac);
}

void imx519_af_trigger(void)
{
    g_gs_refocus = false;
    imx519_af_run_begin();
    g_af_ctrl.state = IMX519_AF_STATE_INIT_SWEEP;
    g_af_ctrl.scan_count++;
    AF_DBG("Starting full Auto-Focus sweep #%lu...\n", g_af_ctrl.scan_count);
}

void imx519_af_trigger_refocus(void)
{
    uint16_t cur = g_af_ctrl.current_dac;
    uint16_t lo = (cur > REFOCUS_HALF_RANGE_DAC)
                  ? (uint16_t)(cur - REFOCUS_HALF_RANGE_DAC)
                  : (uint16_t)IMX519_AF_DAC_MIN;
    uint32_t hi = (uint32_t)cur + REFOCUS_HALF_RANGE_DAC;

    if (hi > IMX519_AF_DAC_MAX) {
        hi = IMX519_AF_DAC_MAX;
    }

    g_af_ctrl.scan_count++;
    imx519_af_run_begin();
    AF_DBG("[AF-C] Scene change detected! Re-focusing around DAC %u (+/-%d)...\n",
           (unsigned)cur, REFOCUS_HALF_RANGE_DAC);
    imx519_af_gs_begin(lo, (uint16_t)hi);
    g_gs_refocus = true;   /* after gs_begin: it is also used for full searches */
}

bool imx519_af_is_busy(void)
{
    return (g_af_ctrl.state != IMX519_AF_STATE_IDLE && g_af_ctrl.state != IMX519_AF_STATE_LOCKED);
}

imx519_af_state_t imx519_af_get_state(void)
{
    return g_af_ctrl.state;
}

/*
 * Laplacian contrast / sharpness score on the central 50% ROI, normalised by
 * the mean signal above the black level, squared.
 *
 * The normalisation matters: Laplacian energy scales with the square of the
 * signal amplitude, so an unnormalised score tracks scene brightness. Since
 * the lock uses a relative drop to decide the picture went out of focus, a
 * lighting change alone could trigger a re-focus sweep. This is the same
 * measure focus_assist.py uses on the host, so device and host numbers are
 * comparable. The black level is removed first because contrast scales with
 * (signal - black), not with the raw level.
 */
uint32_t imx519_calc_focus_score(const uint8_t *img_plane, uint32_t width, uint32_t height)
{
    if (!img_plane || width < 16 || height < 16) {
        return 0;
    }

    uint64_t sum_lap2 = 0;
    uint64_t sum_val = 0;
    uint32_t n_px = 0;
    uint32_t x_start = width >> 2;       /* 25% from left edge (80 for 320w) */
    uint32_t x_end   = (width * 3) >> 2;  /* 75% from left edge (240 for 320w) */
    uint32_t y_start = height >> 2;      /* 25% from top edge (60 for 240h) */
    uint32_t y_end   = (height * 3) >> 2; /* 75% from top edge (180 for 240h) */

    uint32_t ch = app_get_raw_channels();

    if (ch == 3) {
        /* RGB mode: the WDMA3 buffer is PLANAR - three full w*h planes in the
         * order B, G, R (SDK doc of hx_lib_image_resize_BGR8U3C_to_RGB24_helium;
         * confirmed on the board with imx519_log_raw_stats(): only the planar
         * reading matches the JPEG's per-channel means, interleaved triplets
         * give identical stats on all three "channels").
         * The old code indexed it as interleaved BGR, so its "central ROI" was
         * really the bottom of the B plane, the whole G plane and the top of the
         * R plane, sampled at a 3-pixel spacing. Score the G plane instead:
         * it has twice the Bayer samples of R or B and carries most of the
         * luminance detail. */
        img_plane += width * height;
    }

    {
        /* Single plane (Y in YUV420 mode, G in RGB mode), 1-byte pixel stride */
        for (uint32_t y = y_start; y < y_end; y++) {
            const uint8_t *row_prev = img_plane + (y - 1) * width;
            const uint8_t *row_curr = img_plane + y * width;
            const uint8_t *row_next = img_plane + (y + 1) * width;

            for (uint32_t x = x_start; x < x_end; x++) {
                int32_t center = (int32_t)row_curr[x];
                int32_t lap = (center << 2) - (int32_t)row_curr[x - 1] - (int32_t)row_curr[x + 1]
                                            - (int32_t)row_prev[x]     - (int32_t)row_next[x];
                if (lap < 0) lap = -lap;
                if (lap > 8) sum_lap2 += (uint64_t)((uint32_t)lap * (uint32_t)lap);
                sum_val += (uint64_t)center;
                n_px++;
            }
        }
    }

    if (n_px == 0) {
        return 0;
    }
    {
        uint32_t mean = (uint32_t)(sum_val / n_px);
        uint32_t signal;

        if (mean <= IMX519_AF_BLACK_LEVEL + 1U) {
            return 0;                       /* too dark to score */
        }
        signal = mean - IMX519_AF_BLACK_LEVEL;
        /* Scale: 1e6 keeps a badly defocused scene in the thousands, so small
         * changes are still visible in integer output (1e3 quantised the whole
         * blurred range to "1"). */
        return (uint32_t)((sum_lap2 * 1000000U) / ((uint64_t)n_px * signal * signal));
    }
}


/* Set by cvapp_yolov8n_ob.cpp; halved preview rate while a search is running. */
extern volatile uint8_t g_preview_divider;

/* Process incoming frame in Continuous AF state machine */
bool imx519_af_process_frame(const uint8_t *y_plane, uint32_t width, uint32_t height)
{
    /* A search advances one DAC step per couple of frames, so its duration is
     * set by the frame rate. The preview JPEG costs 79 ms of UART per frame,
     * the second largest item in a 324 ms frame, so send it every other frame
     * while searching and at full rate once locked. */
    g_preview_divider = (g_af_ctrl.state == IMX519_AF_STATE_INIT_SWEEP ||
                         g_af_ctrl.state == IMX519_AF_STATE_COARSE_SEARCH ||
                         g_af_ctrl.state == IMX519_AF_STATE_FINE_SEARCH ||
                         g_af_ctrl.state == IMX519_AF_STATE_GOLDEN_SEARCH) ? 2 : 1;


    switch (g_af_ctrl.state) {
    case IMX519_AF_STATE_IDLE:
        /* Nothing to do; lens locked */
        return false;

    case IMX519_AF_STATE_LOCKED:
        /* Continuous AF Monitoring Mode */
        if (!g_af_ctrl.continuous_enabled) {
            return false;
        }

        if (g_af_ctrl.lock_cooldown > 0) {
            g_af_ctrl.lock_cooldown--;
            return false;
        }

        {
            uint32_t cur_score = imx519_calc_focus_score(y_plane, width, height);
            uint32_t drop_threshold = (g_af_ctrl.locked_score * DEFOCUS_DROP_THRESHOLD_PCT) / 100;


            uint32_t rise_threshold =
                (uint32_t)(((uint64_t)g_af_ctrl.locked_score * SCENE_RISE_THRESHOLD_PCT) / 100U);

            if (cur_score < drop_threshold) {
                g_af_ctrl.rise_counter = 0;
                g_af_ctrl.defocus_counter++;
                if (g_af_ctrl.defocus_counter >= DEFOCUS_CONFIRM_FRAMES) {
                    imx519_af_trigger_refocus();
                    return true;
                }
            } else if (cur_score > rise_threshold) {
                /* Far more detail than what was locked on: the scene changed.
                 * With locked_score 0 - a search that found nothing - this is
                 * the only test that can fire, and it is what gets the lens
                 * moving again once something worth focusing on appears. */
                g_af_ctrl.defocus_counter = 0;
                g_af_ctrl.rise_counter++;
                if (g_af_ctrl.rise_counter >= DEFOCUS_CONFIRM_FRAMES) {
                    imx519_af_trigger_refocus();
                    return true;
                }
            } else {
                g_af_ctrl.defocus_counter = 0;
                g_af_ctrl.rise_counter = 0;
                /* Slowly adapt locked score with exponential moving average */
                if (cur_score > g_af_ctrl.locked_score) {
                    g_af_ctrl.locked_score = (g_af_ctrl.locked_score + cur_score) >> 1;
                }
            }
        }
        return false;

    case IMX519_AF_STATE_INIT_SWEEP:
        if (g_af_algo == IMX519_AF_ALGO_GOLDEN) {
            AF_DBG("Golden search starting (whole travel)\n");
            imx519_af_gs_begin(COARSE_START_DAC, COARSE_END_DAC);
            return true;
        }

        g_coarse_step = (g_af_algo == IMX519_AF_ALGO_HYBRID) ? HYBRID_COARSE_STEP_DAC
                                                            : COARSE_STEP_DAC;
        g_af_ctrl.coarse_dac = COARSE_START_DAC;
        g_af_ctrl.best_coarse_dac = COARSE_START_DAC;
        g_af_ctrl.max_coarse_score = 0;
        g_af_ctrl.settle_counter = AF_SETTLE_FRAMES;

        imx519_vcm_set_focus(g_af_ctrl.coarse_dac);
        g_af_ctrl.state = IMX519_AF_STATE_COARSE_SEARCH;
        AF_DBG("Coarse sweep starting at DAC = %d...\n", g_af_ctrl.coarse_dac);
        return true;

    case IMX519_AF_STATE_COARSE_SEARCH:
        if (g_af_ctrl.settle_counter > 0) {
            g_af_ctrl.settle_counter--;
            return true;
        }

        {
            uint32_t score = imx519_calc_focus_score(y_plane, width, height);
            AF_DBG("Coarse DAC %3d -> Score: %lu\n", g_af_ctrl.coarse_dac, score);

            if (score < g_af_ctrl.min_score) {
                g_af_ctrl.min_score = score;
            }
            if (score > g_af_ctrl.max_coarse_score) {
                g_af_ctrl.max_coarse_score = score;
                g_af_ctrl.best_coarse_dac = g_af_ctrl.coarse_dac;
            }

            /* Always finish on the last DAC. Stepping 512 from 0 stops at
             * 3584 and leaves the final 511 of the travel unsampled, which in
             * the host-side replay was exactly where a peak could be missed
             * (a subject at ~5 cm); one extra sample closes it. */
            if ((uint32_t)g_af_ctrl.coarse_dac + g_coarse_step > COARSE_END_DAC &&
                g_af_ctrl.coarse_dac < COARSE_END_DAC) {
                g_af_ctrl.coarse_dac = COARSE_END_DAC;
            } else {
                g_af_ctrl.coarse_dac += g_coarse_step;
            }
            if (g_af_ctrl.coarse_dac <= COARSE_END_DAC) {
                imx519_vcm_set_focus(g_af_ctrl.coarse_dac);
                g_af_ctrl.settle_counter = AF_SETTLE_FRAMES;
            } else {
                AF_DBG("Coarse search done. Best coarse DAC = %d (Peak Score: %lu)\n",
                       g_af_ctrl.best_coarse_dac, g_af_ctrl.max_coarse_score);

                if (imx519_af_curve_is_flat(g_af_ctrl.max_coarse_score)) {
                    /* The coarse pass covered the whole travel and found no
                     * peak, so refining inside it cannot produce one either. */
                    imx519_af_hold_without_lock(g_af_ctrl.max_coarse_score);
                    return false;
                }

                if (g_af_algo == IMX519_AF_ALGO_HYBRID) {
                    /* The coarse pass has picked the region; narrow inside it.
                     * The window is one coarse step either side, because the
                     * true peak can sit anywhere between the best sample and
                     * its neighbours. */
                    uint16_t lo = (g_af_ctrl.best_coarse_dac > g_coarse_step)
                                  ? (uint16_t)(g_af_ctrl.best_coarse_dac - g_coarse_step)
                                  : (uint16_t)IMX519_AF_DAC_MIN;
                    uint32_t hi = (uint32_t)g_af_ctrl.best_coarse_dac + g_coarse_step;

                    if (hi > IMX519_AF_DAC_MAX) {
                        hi = IMX519_AF_DAC_MAX;
                    }
                    imx519_af_gs_begin(lo, (uint16_t)hi);
                    /* The coarse best is a measured point inside that window,
                     * so let it stand as the best so far: the lock can then
                     * never come out worse than the coarse pass alone. */
                    g_gs.best_dac = g_af_ctrl.best_coarse_dac;
                    g_gs.best_score = g_af_ctrl.max_coarse_score;
                    return true;
                }

                g_af_ctrl.fine_start_dac = (int16_t)g_af_ctrl.best_coarse_dac - FINE_HALF_RANGE_DAC;
                if (g_af_ctrl.fine_start_dac < IMX519_AF_DAC_MIN) {
                    g_af_ctrl.fine_start_dac = IMX519_AF_DAC_MIN;
                }

                g_af_ctrl.fine_end_dac = (int16_t)g_af_ctrl.best_coarse_dac + FINE_HALF_RANGE_DAC;
                if (g_af_ctrl.fine_end_dac > IMX519_AF_DAC_MAX) {
                    g_af_ctrl.fine_end_dac = IMX519_AF_DAC_MAX;
                }

                g_af_ctrl.fine_dac = g_af_ctrl.fine_start_dac;
                g_af_ctrl.best_fine_dac = g_af_ctrl.best_coarse_dac;
                g_af_ctrl.max_fine_score = 0;
                g_af_ctrl.settle_counter = AF_SETTLE_FRAMES;

                imx519_vcm_set_focus((uint16_t)g_af_ctrl.fine_dac);
                g_af_ctrl.state = IMX519_AF_STATE_FINE_SEARCH;
                AF_DBG("Fine sweep starting: [%d ~ %d], step = %d\n",
                       g_af_ctrl.fine_start_dac, g_af_ctrl.fine_end_dac, FINE_STEP_DAC);
            }
        }
        return true;

    case IMX519_AF_STATE_FINE_SEARCH:
        if (g_af_ctrl.settle_counter > 0) {
            g_af_ctrl.settle_counter--;
            return true;
        }

        {
            uint32_t score = imx519_calc_focus_score(y_plane, width, height);
            AF_DBG("  Fine DAC %3d -> Score: %lu\n", g_af_ctrl.fine_dac, score);

            if (score < g_af_ctrl.min_score) {
                g_af_ctrl.min_score = score;
            }
            if (score > g_af_ctrl.max_fine_score) {
                g_af_ctrl.max_fine_score = score;
                g_af_ctrl.best_fine_dac = (uint16_t)g_af_ctrl.fine_dac;
            }

            g_af_ctrl.fine_dac += FINE_STEP_DAC;
            if (g_af_ctrl.fine_dac <= g_af_ctrl.fine_end_dac) {
                imx519_vcm_set_focus((uint16_t)g_af_ctrl.fine_dac);
                g_af_ctrl.settle_counter = AF_SETTLE_FRAMES;
            } else {
                if (imx519_af_curve_is_flat(g_af_ctrl.max_fine_score)) {
                    imx519_af_hold_without_lock(g_af_ctrl.max_fine_score);
                    return false;
                }
                imx519_vcm_set_focus(g_af_ctrl.best_fine_dac);
                g_af_ctrl.locked_score = g_af_ctrl.max_fine_score;
                g_af_ctrl.lock_cooldown = LOCK_COOLDOWN_FRAMES;
                g_af_ctrl.defocus_counter = 0;
                g_af_ctrl.state = IMX519_AF_STATE_LOCKED;
                AF_DBG("====================================================\n");
                AF_DBG(">>> AUTOFOCUS LOCKED! Optimal DAC = %d (Peak Score: %lu) <<<\n",
                       g_af_ctrl.best_fine_dac, g_af_ctrl.locked_score);
                AF_DBG("====================================================\n");
                return false;
            }
        }
        return true;

    case IMX519_AF_STATE_GOLDEN_SEARCH:
        if (g_af_ctrl.settle_counter > 0) {
            g_af_ctrl.settle_counter--;
            return true;
        }

        {
            uint32_t score = imx519_calc_focus_score(y_plane, width, height);
            uint16_t meas_dac = (g_gs.pending == 1) ? g_gs.x1 : g_gs.x2;
            uint32_t span;

            if (g_gs.pending == 1) {
                g_gs.f1 = score;
            } else {
                g_gs.f2 = score;
            }
            AF_DBG("  Golden DAC %4u -> Score: %lu  bracket [%u ~ %u]\n",
                   (unsigned)meas_dac, score, (unsigned)g_gs.lo, (unsigned)g_gs.hi);

            if (score < g_af_ctrl.min_score) {
                g_af_ctrl.min_score = score;
            }

            /* Lock to the best position that was actually measured. The x1/x2
             * pair cannot serve for this: after the bracket is narrowed one of
             * them is a brand-new position whose f is still the previous
             * probe's score, so picking the larger of f1/f2 can name a DAC that
             * was never measured and pair it with another point's score. */
            if (score > g_gs.best_score) {
                g_gs.best_score = score;
                g_gs.best_dac = meas_dac;
            }
            g_gs.evals++;

            if (g_gs.evals == 1) {
                /* Only the first probe is known; measure the second one. */
                g_gs.pending = 2;
                imx519_vcm_set_focus(g_gs.x2);
                g_af_ctrl.settle_counter = AF_SETTLE_FRAMES;
                return true;
            }

            /* Drop the side of the bracket that cannot contain the peak and
             * reuse the surviving probe as one of the next pair. */
            if (g_gs.f1 > g_gs.f2) {
                g_gs.hi = g_gs.x2;
                g_gs.x2 = g_gs.x1;
                g_gs.f2 = g_gs.f1;
                span = (uint32_t)g_gs.hi - g_gs.lo;
                g_gs.x1 = (uint16_t)(g_gs.lo + span * GS_FRAC_NEAR / 1000U);
                g_gs.pending = 1;
            } else {
                g_gs.lo = g_gs.x1;
                g_gs.x1 = g_gs.x2;
                g_gs.f1 = g_gs.f2;
                span = (uint32_t)g_gs.hi - g_gs.lo;
                g_gs.x2 = (uint16_t)(g_gs.lo + span * GS_FRAC_FAR / 1000U);
                g_gs.pending = 2;
            }

            if (((uint32_t)g_gs.hi - g_gs.lo) <= GS_TOL_DAC || g_gs.evals >= GS_MAX_EVALS) {
                if (g_gs_refocus &&
                    (g_gs.best_score < (g_af_ctrl.locked_score / 2) ||
                     imx519_af_curve_is_flat(g_gs.best_score))) {
                    /* Nothing worth locking inside the window: the subject
                     * moved further than it covers, or the scene changed
                     * altogether. The window is local, so a flat result here
                     * says nothing about the rest of the travel - search it
                     * all before concluding there is no peak anywhere. */
                    AF_DBG("[AF-C] Best in window only %lu (flattest %lu, locked %lu) - escalating to a full search\n",
                           g_gs.best_score, g_af_ctrl.min_score, g_af_ctrl.locked_score);
                    imx519_af_trigger();
                    return true;
                }
                g_gs_refocus = false;
                if (imx519_af_curve_is_flat(g_gs.best_score)) {
                    imx519_af_hold_without_lock(g_gs.best_score);
                    return false;
                }
                imx519_vcm_set_focus(g_gs.best_dac);
                g_af_ctrl.current_dac = g_gs.best_dac;
                g_af_ctrl.locked_score = g_gs.best_score;
                g_af_ctrl.lock_cooldown = LOCK_COOLDOWN_FRAMES;
                g_af_ctrl.defocus_counter = 0;
                g_af_ctrl.state = IMX519_AF_STATE_LOCKED;
                AF_DBG("====================================================\n");
                AF_DBG(">>> AUTOFOCUS LOCKED! Optimal DAC = %d (Peak Score: %lu) <<< [%s, %u narrowing measurements]\n",
                       g_gs.best_dac, g_gs.best_score, imx519_af_algo_name(g_af_algo),
                       (unsigned)g_gs.evals);
                AF_DBG("====================================================\n");
                return false;
            }

            imx519_vcm_set_focus((g_gs.pending == 1) ? g_gs.x1 : g_gs.x2);
            g_af_ctrl.settle_counter = AF_SETTLE_FRAMES;
        }
        return true;

    default:
        g_af_ctrl.state = IMX519_AF_STATE_IDLE;
        return false;
    }
}


