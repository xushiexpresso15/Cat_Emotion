/*
 * cisdp_cfg.h
 *
 *  Created on: 20240122
 *      Author: 901912
 *      Modified for IMX519
 */

#ifndef APP_SCENARIO_CISDP_CFG_H_
#define APP_SCENARIO_CISDP_CFG_H_

#include "hx_drv_gpio.h"
#include "hx_drv_inp.h"

typedef enum
{
	APP_DP_RES_RGB640x480_INP_SUBSAMPLE_1X,
	APP_DP_RES_RGB640x480_INP_SUBSAMPLE_2X,
	APP_DP_RES_RGB640x480_INP_SUBSAMPLE_4X,
	APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X,
	APP_DP_RES_YUV640x480_INP_SUBSAMPLE_2X,
	APP_DP_RES_YUV640x480_INP_SUBSAMPLE_4X,
}APP_DP_INP_SUBSAMPLE_E;

#define IMX519_SENSOR_I2CID				(0x1A)
#define IMX519_MIPI_CLOCK_FEQ			(408)	//MHz, assumed link freq
#define IMX519_MIPI_LANE_CNT			(2)
#define IMX519_MIPI_DPP					(10)	//depth per pixel RAW10
#define IMX519_MIPITX_CNTCLK_EN			(0)		// non-continuous clock output (matching IMX219/IMX477/IMX708)
#define IMX519_LANE_NB					(2)
#define SENSORDPLIB_SENSOR_IMX519		(SENSORDPLIB_SENSOR_HM2130) // Assuming compatible with HM2130 ISP params
#define DYNAMIC_ADDRESS

/*
 * RAW Bayer capture test firmware (colour-cast investigation, 2026-09-17).
 *   IMX519_RAW_DUMP_MODE 1: datapath is Sensor -> INP -> WDMA2 only - no
 *     demosaic, no JPEG, no model (its 1 MB tensor arena is not allocated).
 *     UART key 'u' sends the next frame to the PC (!!RAWH header, base64
 *     !!RAWD chunks, !!RAWE trailer with CRC32; see raw_capture.py). Keys
 *     't' / 'y' turn the sensor colour-bar test pattern on / off. Per-channel
 *     sensor gains are forced to 1.0 and software white balance is not run.
 *   IMX519_RAW_DUMP_MODE 0: normal firmware.
 *   IMX519_RAW_DUMP_SUBS (raw mode only): 0 = INP subsampling off, 640x480
 *     out; 1 = INP_SUBSAMPLE_4TO2, 320x240; 2 = INP_SUBSAMPLE_4TO2_B, 320x240.
 */
#define IMX519_RAW_DUMP_MODE			(0)
#define IMX519_RAW_DUMP_SUBS			(1)

/*
 * Sensor output dimensions after .i file DIG_CROP trimming.
 * Must be 64-aligned (width) and 16-aligned (height) for WE2 INP/WDMA.
 */
#define IMX519_SENSOR_WIDTH				2304
#define IMX519_SENSOR_HEIGHT			1744
/*
 * RAW capture test, subsampling off: IMX519_RAW_DUMP_CROP_ORIGIN 1 crops from
 * (0,0) instead of the centre. With the centre origin (832,632) and
 * subsampling off, INP produced no valid raster (the WDMA2 stream repeats
 * every ~366.3 samples, not 640, and the library reports
 * FE_COUNT_NOT_REACH); the SDK's own raw example crops from (0,0).
 */
#define IMX519_RAW_DUMP_CROP_ORIGIN		(1)
#if (IMX519_RAW_DUMP_MODE && (IMX519_RAW_DUMP_SUBS == 0) && IMX519_RAW_DUMP_CROP_ORIGIN)
#define IMX519_INP_CROP_START_X			0
#define IMX519_INP_CROP_START_Y			0
#else
#define IMX519_INP_CROP_START_X			832		// Centered: (2304-640)/2 = 832 (Even -> Bayer phase aligned)
#define IMX519_INP_CROP_START_Y			632		// Centered: (1744-480)/2 = 632 (Even -> Bayer phase aligned)
#endif
#define IMX519_INP_CROP_WIDTH			640		// 640 / 2 = 320 via INP_SUBSAMPLE_4TO2
#define IMX519_INP_CROP_HEIGHT			480		// 480 / 2 = 240 via INP_SUBSAMPLE_4TO2
#if (IMX519_RAW_DUMP_MODE && (IMX519_RAW_DUMP_SUBS == 0))
#define IMX519_INP_OUT_WIDTH			640		// RAW capture test, subsampling off
#define IMX519_INP_OUT_HEIGHT			480
#else
#define IMX519_INP_OUT_WIDTH			320
#define IMX519_INP_OUT_HEIGHT			240
#endif
#define IMX519_HW2x2_CROP_WIDTH			320
#define IMX519_HW2x2_CROP_HEIGHT		240
#define IMX519_HW5x5_CROP_WIDTH			320
#define IMX519_HW5x5_CROP_HEIGHT		240

#define IMX519_EXPOSURE_DEFAULT			(0x0400)
/*
 * Exposure and gain are set ONLY here. cisdp_sensor.c used to overwrite all
 * three at init with hardcoded values (1280 lines, analog code 960, digital
 * 1.5x), so none of these defines had any effect until that block was removed.
 *
 * Analog gain code -> gain: gain = 1024 / (1024 - code) (libcamera's IMX519
 * helper; the Linux driver caps the code at 960). 0 = 1x, 512 = 2x, 768 = 4x,
 * 896 = 8x, 960 = 16x. Verified on hardware: 512x16x, 1024x8x and 2048x4x give
 * the same brightness (80.7-81.3).
 *
 * Exposure time: line time = LINE_LENGTH_PCK / pixel rate = 6512 / 426666667
 * = 15.26 us, sensor at 30 fps. The module runs on its own oscillator (J2
 * pin 12 is no-connect, WE2's MCLK never reaches it). 512 lines = 7.8 ms,
 * 1024 = 15.6 ms, 2048 = 31.3 ms; max 2152 lines = 32.8 ms at this frame length.
 *
 * Current values: 512 lines / 16x analog / 1.0x digital. Digital gain is 1.0x
 * because it is applied after the ADC and adds no information. At equal
 * brightness, 2048x4x measured 16-20% lower temporal noise than 512x16x in
 * the JPEG stream (the true difference is likely larger - JPEG quantization
 * compresses it); 1024x8x was not distinguishable from 512x16x at that
 * resolution. The exposure/analog split is deliberately left at 512x16x until
 * a replacement module that can focus is available: motion blur cannot be
 * judged while this lens is badly defocused. Re-run exposure_gain_compare.py
 * then. Runtime presets: keys 1-6 = 64..2048 lines, Shift+1..5 = 1x..16x.
 * Fixed exposure, no AE: a much brighter scene will clip.
 */
#define IMX519_EXPOSURE_SETTING			(0x0200)	// 512 lines
#define IMX519_AGAIN_SETTING			(0x03C0)	// code 960 = 16x analog (max)
#define IMX519_DGAIN_SETTING			(0x0100)	// 1.0x digital gain

#define BASE_ADDR_SYS_SRAM			0x34000000

#define BASE_ADDR_SRAM0				0x34000000
#define BASE_ADDR_SRAM1				0x34100000

#ifdef DYNAMIC_ADDRESS
#define EXT_RAM_START   BASE_ADDR_SRAM0
#define MAX_SRAM_ADDR	(BASE_ADDR_SRAM1+1024*1024-1)
#endif

/*
 * Image orientation, written to IMAGE_ORIENTATION (0x0101) at init. 0x03 =
 * horizontal + vertical flip (180 degrees), chosen 2026-09-17 to match how the
 * module is mounted. DP_HW5X5_DEMOS_PATTERN below follows this value: a flip
 * does shift the IMX519's Bayer order - verified for 0x02 by comparing frames
 * of a still scene before/after (channels matched their own channel, R-B
 * structure correlation 0.983); re-check colours if this value changes.
 */
#define CIS_MIRROR_SETTING			(0x03) //0x00: off/0x01:H-Mirror/0x02:V-Mirror/0x03:HV-Mirror
#define CIS_I2C_ID					IMX519_SENSOR_I2CID
#define CIS_ENABLE_MIPI_INF			(0x01) //0x00: off/0x01: on
#define CIS_MIPI_LANE_NUMBER		(0x02)
#define CIS_ENABLE_HX_AUTOI2C		(0x00) //0x00: off/0x01: on/0x2: on and XSLEEP KEEP HIGH
#define DEAULT_XHSUTDOWN_PIN    	AON_GPIO2

/*
 * DP SENCTRL CFG
 */
#define SENCTRL_SENSOR_TYPE			SENSORDPLIB_SENSOR_IMX519
#define SENCTRL_STREAM_TYPE			SENSORDPLIB_STREAM_NONEAOS
#define SENCTRL_SENSOR_WIDTH 		IMX519_SENSOR_WIDTH
#define SENCTRL_SENSOR_HEIGHT 		IMX519_SENSOR_HEIGHT
#define SENCTRL_SENSOR_CH	 		3

/*
 * DP INP CFG: 4-to-2 subsampling (matching OV5647 pipeline)
 */
/*
 * INP_SUBSAMPLE_4TO2 vs the Bayer-aware INP_SUBSAMPLE_4TO2_B is UNTESTED.
 * An earlier A/B comparison reported byte-identical output, but it was
 * invalid: the build did not track this header (options/rules.mk only
 * included APPL_DEPS), so cisdp_sensor.c was never recompiled and both runs
 * used 4TO2. What was validly measured is that the 4TO2 datapath preserves a
 * single-pixel colour-bar edge, so the soft image does not come from here.
 * Whether _B matters for Bayer phase is still open; re-test now that the
 * build tracks header changes.
 */
#if IMX519_RAW_DUMP_MODE
#if (IMX519_RAW_DUMP_SUBS == 0)
#define DP_INP_SUBSAMPLE			INP_SUBSAMPLE_DISABLE
#elif (IMX519_RAW_DUMP_SUBS == 1)
#define DP_INP_SUBSAMPLE			INP_SUBSAMPLE_4TO2
#else
#define DP_INP_SUBSAMPLE			INP_SUBSAMPLE_4TO2_B
#endif
#else
#define DP_INP_SUBSAMPLE			INP_SUBSAMPLE_4TO2
#endif
#define DP_INP_BINNING				INP_BINNING_DISABLE
#define DP_INP_CROP_START_X			IMX519_INP_CROP_START_X
#define DP_INP_CROP_START_Y			IMX519_INP_CROP_START_Y
#define DP_INP_CROP_WIDTH          	IMX519_INP_CROP_WIDTH
#define DP_INP_CROP_HEIGHT         	IMX519_INP_CROP_HEIGHT
#define DP_INP_OUT_WIDTH 		    IMX519_INP_OUT_WIDTH
#define DP_INP_OUT_HEIGHT 		    IMX519_INP_OUT_HEIGHT

/*
 * DP HW2X2 CFG
 */
#define DP_HW2X2_PATH				HW2x2_PATH_THROUGH
#define DP_HW2X2_PROCESS_MODE		HW2x2_MODE_UNITY
#define DP_HW2X2_CROP_START_X		0
#define DP_HW2X2_CROP_START_Y		0
#define DP_HW2X2_CROP_WIDTH			DP_INP_OUT_WIDTH
#define DP_HW2X2_CROP_HEIGHT		DP_INP_OUT_HEIGHT
#define DP_HW2X2_ROUND_MODE			HW2x2_ROUNDMODE_FLOOR
#define DP_HW2X2_OUT_WIDTH          (DP_INP_OUT_WIDTH)
#define DP_HW2X2_OUT_HEIGHT         (DP_INP_OUT_HEIGHT)


/*
 * DP CDM CFG
 */
#define DP_CDM_ENABLE				CDM_ENABLE_ON
#define DP_CDM_IN_START_X			0
#define DP_CDM_IN_START_Y			0
#define DP_CDM_IN_WIDTH 			DP_HW2X2_OUT_WIDTH
#define DP_CDM_IN_HEIGHT			DP_HW2X2_OUT_HEIGHT
#define DP_CDM_META_DUMP 			CDM_ENABLE_MATA_DUMP_ON
#define DP_CDM_HT_PACKING 			CDM_ENABLE_HT_PACKING_ON
#define DP_CDM_MIN_ALLOW_DIS 		3
#define DP_CDM_TOLERANCE 			3
#define DP_CDM_REACTANCE 			2
#define DP_CDM_RELAXATION 			1
#define DP_CDM_EROS_TH 				3
#define DP_CDM_NUM_HT_TH 			10
#define DP_CDM_NUM_HT_VECT_TH_X 	8
#define DP_CDM_NUM_HT_VECT_TH_Y 	4
#define DP_CDM_NUM_CONS_HT_BIN_TH_X 1
#define DP_CDM_NUM_CONS_HT_BIN_TH_Y 1
#define DP_CDM_CPU_ACTIVEFLAG 		CDM_CPU_ACTFLAG_SLEEP
#define DP_CDM_INIT_MAP_FLAG 		CDM_INIMAP_FLAG_ON


/*
 * DP HW5X5 CFG
 */
#define DP_HW5X5_PATH				HW5x5_PATH_THROUGH_DEMOSAIC
#define DP_HW5X5_DEMOS_BNDMODE		DEMOS_BNDODE_REFLECT
#define DP_HW5X5_DEMOS_COLORMODE	DEMOS_COLORMODE_RGB

#if (CIS_MIRROR_SETTING == 0x01)
#define DP_HW5X5_DEMOS_PATTERN		DEMOS_PATTENMODE_GRBG
#elif (CIS_MIRROR_SETTING == 0x02)
#define DP_HW5X5_DEMOS_PATTERN		DEMOS_PATTENMODE_GBRG
#elif (CIS_MIRROR_SETTING == 0x03)
#define DP_HW5X5_DEMOS_PATTERN		DEMOS_PATTENMODE_BGGR
#else
#define DP_HW5X5_DEMOS_PATTERN		DEMOS_PATTENMODE_RGGB
#endif

#define DP_HW5X5_DEMOSLPF_ROUNDMODE DEMOSLPF_ROUNDMODE_FLOOR
#define DP_HW5X5_CROP_START_X 		0
#define DP_HW5X5_CROP_START_Y 		0
#define DP_HW5X5_CROP_WIDTH 		IMX519_HW5x5_CROP_WIDTH
#define DP_HW5X5_CROP_HEIGHT 		IMX519_HW5x5_CROP_HEIGHT
#define DP_HW5X5_OUT_WIDTH 			IMX519_HW5x5_CROP_WIDTH
#define DP_HW5X5_OUT_HEIGHT 		IMX519_HW5x5_CROP_HEIGHT

/*
 * DP JPEG CFG
 */
#define DP_JPEG_PATH				JPEG_PATH_ENCODER_EN
#define DP_JPEG_ENC_WIDTH 			DP_HW5X5_OUT_WIDTH
#define DP_JPEG_ENC_HEIGHT 			DP_HW5X5_OUT_HEIGHT
#define DP_JPEG_ENCTYPE 			JPEG_ENC_TYPE_YUV420
#define DP_JPEG_ENCQTABLE 			JPEG_ENC_QTABLE_4X

/*
 * Compile-time guards: catch WDMA2_ABNORMAL-causing mistakes at build
 * time instead of discovering them on the device. If either of these
 * fires, IMX519_SENSOR_WIDTH/HEIGHT no longer matches what
 * IMX519_mipi_2lane_2328x1748.i actually outputs, or the value isn't
 * aligned to the WE2 WDMA 64/16 pixel boundary.
 */
_Static_assert((IMX519_SENSOR_WIDTH % 64) == 0,
		"IMX519_SENSOR_WIDTH must be a multiple of 64 (WE2 WDMA line alignment)");
_Static_assert((IMX519_SENSOR_HEIGHT % 16) == 0,
		"IMX519_SENSOR_HEIGHT must be a multiple of 16 (WE2 WDMA line alignment)");
_Static_assert((IMX519_INP_CROP_START_X + IMX519_INP_CROP_WIDTH) <= IMX519_SENSOR_WIDTH,
		"IMX519_INP_CROP_START_X + IMX519_INP_CROP_WIDTH exceeds IMX519_SENSOR_WIDTH");
_Static_assert((IMX519_INP_CROP_START_Y + IMX519_INP_CROP_HEIGHT) <= IMX519_SENSOR_HEIGHT,
		"IMX519_INP_CROP_START_Y + IMX519_INP_CROP_HEIGHT exceeds IMX519_SENSOR_HEIGHT");

/*
 * Software black level + white balance on the WDMA3 buffer (the model input;
 * the JPEG preview is not affected).
 *
 * Black level 40: WDMA3 with the lens covered read 39.7 / 40.0 / 39.7 (B/G/R
 * planes, mode 40) at 512 lines x16, and the JPEG dark frame read 40 at both
 * 512 lines x16 and 64 lines x1 - so it does not depend on gain. The sensor's
 * DATA_PEDESTAL is 64 (10-bit); WE2 does not map that to 64>>2 = 16.
 *
 * Gains (x1000) calibrated 2026-09-14 on white paper filling the centre 50%,
 * under the room lighting in use, at 512 lines x16 analog: ROI means B 104.6,
 * G 146.2, R 94.4 (no clipping); 4 calibrations gave B 1642..1644 and
 * R 1952..1958. They are only valid for that light source - recalibrate with
 * UART key 'f' under different lighting (calibration is RAM only; copy the
 * printed gains here to keep them).
 * The WE2 output is not linear in exposure (the (mean - 40) increment per
 * exposure doubling measured 17.3, 24.2, 43.6 for 128..1024 lines), so fixed
 * gains are exact only near the brightness they were calibrated at: about
 * -3%..+8% off over 128..1024 lines.
 */
#define IMX519_SWWB_BLACK_LEVEL     (40)
#define IMX519_SWWB_GAIN_B_X1000    (1643)
#define IMX519_SWWB_GAIN_G_X1000    (1000)
#define IMX519_SWWB_GAIN_R_X1000    (1954)
#define IMX519_SWWB_ENABLE_DEFAULT  (1)

/*
 * Tone curve applied after black level + gain, folded into the same LUT:
 *     out = 255 * (x / 255)^(IMX519_SWWB_TONE_EXP_X1000 / 1000)
 * Once the black level is removed the WE2 data is close to linear in light
 * ((mean - 40) grew as exposure^~0.8 over 128..1024 lines), while the model is
 * trained on ordinary gamma-encoded photos (~light^0.45). Without the curve
 * the corrected image measured much darker and more saturated than the raw
 * JPEG (luma 36 vs 65, mean HSV saturation 86 vs 61 on the same frames).
 * 560 ~= 0.45 / 0.8 is a rough fit, confirmed by eye on the host preview
 * (live_camera_monitor.py, which reads this value). 1000 = no curve.
 * Independent of the light source, unlike the gains above.
 */
#define IMX519_SWWB_TONE_EXP_X1000  (560)

#endif /* APP_SCENARIO_CISDP_CFG_H_ */
