/*
 * imx519_af.h
 *
 * Sony IMX519 Continuous Auto-Focus (AF-C) Controller with AK7375 VCM Driver
 * for Himax WiseEye2 (WE2 HX6538) on Seeed Grove Vision AI Module V2
 */

#ifndef IMX519_AF_H_
#define IMX519_AF_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* VCM I2C Address (7-bit) */
#define AK7375_VCM_I2C_ADDR         0x0C

/* DAC Range Limits (Official 12-bit AK7375: 0 to 4095) */
#define IMX519_AF_DAC_MIN           0
#define IMX519_AF_DAC_MAX           4095
#define IMX519_AF_DAC_DEFAULT       2400

/* AF States */
typedef enum {
    IMX519_AF_STATE_IDLE = 0,       /* AF disabled; lens fixed */
    IMX519_AF_STATE_INIT_SWEEP,     /* Starting a full sweep */
    IMX519_AF_STATE_COARSE_SEARCH,  /* Coarse step scan (100 -> 900) */
    IMX519_AF_STATE_FINE_SEARCH,    /* Fine step scan around best coarse peak */
    IMX519_AF_STATE_LOCKED,         /* Locked at best peak focus (monitoring for defocus) */
    IMX519_AF_STATE_GOLDEN_SEARCH,  /* Interval-narrowing (golden section) search, over the
                                     * whole travel, the region the coarse pass picked, or
                                     * a window around the lock for an AF-C re-focus */
} imx519_af_state_t;

/* Initialize Auto-Focus subsystem */
void imx519_af_init(void);

/* Enable or disable Continuous Auto-Focus (AF-C) */
void imx519_af_set_continuous(bool enable);

/* Query if Continuous Auto-Focus is active */
bool imx519_af_is_continuous(void);

/* Directly set the VCM focus DAC (0 ~ 1023) */
void imx519_vcm_set_focus(uint16_t focus_dac);

/* Get current VCM focus DAC */
uint16_t imx519_af_get_current_dac(void);

/* Trigger a new full Auto-Focus sweep */
void imx519_af_trigger(void);

/* Re-focus after the subject moved: narrow within a window around the locked
 * DAC (7 measurements, ~1.6 s). Escalates to a full search by itself if the
 * subject moved further than the window covers. */
void imx519_af_trigger_refocus(void);

/* Feed a new frame Y plane into AF state machine. Returns true if AF is busy searching */
bool imx519_af_process_frame(const uint8_t *y_plane, uint32_t width, uint32_t height);

/* Query whether AF is actively scanning */
bool imx519_af_is_busy(void);

/* Query current AF state */
imx519_af_state_t imx519_af_get_state(void);


/* Black level of the captured buffer, measured with the lens covered: RAW
 * Bayer, WDMA3 and JPEG all read 40 (sensor DATA_PEDESTAL is 64 in 10-bit;
 * WE2 does not map that to 64>>2). Kept here as well as in cisdp_cfg.h
 * (IMX519_SWWB_BLACK_LEVEL) so this header does not depend on the datapath
 * configuration. */
#define IMX519_AF_BLACK_LEVEL (40)

/* Contrast / sharpness score on the centre 50% ROI, normalised by the mean
 * signal above black squared - comparable with focus_assist.py on the host
 * and independent of scene brightness. */
uint32_t imx519_calc_focus_score(const uint8_t *y_plane, uint32_t width, uint32_t height);

/*
 * Search algorithm used for a full AF run. Key '0' cycles through them at
 * runtime, so they can be compared on one scene.
 */
typedef enum {
    /* Coarse sweep (step 256) + fine sweep: 25 measurements, ~5.4 s. Samples
     * the whole travel, so it always finds the highest of several peaks. */
    IMX519_AF_ALGO_SWEEP = 0,
    /* Narrowing alone, over the whole travel: 10 measurements, ~2.2 s, but it
     * assumes a single maximum - see HYBRID_COARSE_STEP_DAC in the .c for the
     * measured case where that costs it half the peak. */
    IMX519_AF_ALGO_GOLDEN,
    /* Default: coarse pass (step 512) to pick the region, then narrowing
     * inside it. About 15 measurements, ~3.3 s, and correct on every two-peak
     * case the host-side replay covers. */
    IMX519_AF_ALGO_HYBRID,
} imx519_af_algo_t;

void imx519_af_set_algorithm(imx519_af_algo_t algo);
imx519_af_algo_t imx519_af_get_algorithm(void);
void imx519_af_cycle_algorithm(void);


#ifdef __cplusplus
}
#endif

#endif /* IMX519_AF_H_ */