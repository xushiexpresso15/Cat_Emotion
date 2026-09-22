/*
 * cisdp_sensor.h
 *
 *  Created on: 20240206
 *      Author: 901912
 */

#ifndef APP_SCENARIO_CISDP_SENSOR_H_
#define APP_SCENARIO_CISDP_SENSOR_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include "WE2_device.h"
#include "hxevent_debug.h"
#include "sensor_dp_lib.h"
#include "evt_datapath.h"
#include "cisdp_cfg.h"
#include "imx519_af.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \enum CISDP_INIT_TYPE_E
 * \brief
 */
typedef enum CISDP_INIT_TYPE_S
{
	CISDP_INIT_TYPE_NONE			= 0x00,
	CISDP_INIT_TYPE_VIDEO_STREAM	= 0x01,			/*For CPU usage*/
	CISDP_INIT_TYPE_AOS				= 0x02,			/*For PMU USAGE*/
} CISDP_INIT_TYPE_E;

int  cisdp_sensor_init();
int  cisdp_dp_init(bool inp_init, SENSORDPLIB_PATH_E dp_type, evthandlerdp_CBEvent_t cb_event, uint32_t jpg_ratio, APP_DP_INP_SUBSAMPLE_E subs);
void cisdp_sensor_start();
void cisdp_sensor_stop();
void cisdp_stream_on();
void cisdp_stream_off();
void set_mipi_csirx_disable();
void set_mipi_csirx_enable();
void cisdp_get_jpginfo(uint32_t *jpeg_enc_filesize, uint32_t *jpeg_enc_addr);

uint32_t app_get_jpeg_addr();
uint32_t app_get_jpeg_sz();
uint32_t app_get_raw_addr();
uint32_t app_get_raw_sz();
uint32_t app_get_raw_width();
uint32_t app_get_raw_height();
uint32_t app_get_raw_channels();
void imx519_set_hardware_white_balance(float r_gain, float gr_gain, float gb_gain, float b_gain);

/* Sensor built-in test pattern (reg 0x0600): 0 = off, 1 = solid colour,
 * 2 = colour bars, 3 = fade-to-grey bars, 4 = PN9.
 * Colour bars are a synthetic, perfectly sharp source, so they separate
 * "the lens is out of focus" from "the datapath is destroying detail". */
void imx519_set_test_pattern(uint16_t pattern, uint16_t color_r, uint16_t color_gr, uint16_t color_b, uint16_t color_gb);

/* Set exposure (coarse integration time) in lines at runtime, with readback.
 * Clamped to frame length minus the sensor margin. */
void imx519_set_exposure_lines(uint16_t lines);

/* Set analog gain at runtime by raw register code, with readback.
 * gain = 1024 / (1024 - code): 0 = 1x ... 960 = 16x (max, clamped). */
void imx519_set_analog_gain_code(uint16_t code);


/* Software black level + white balance on the WDMA3 buffer (model input only;
 * the JPEG preview is unaffected). Per-plane 256-entry LUTs:
 * x = (in - IMX519_SWWB_BLACK_LEVEL) * gain * 255 / (255 - black), then
 * out = 255 * (x / 255)^(IMX519_SWWB_TONE_EXP_X1000 / 1000).
 * apply(): in place, handles the D-cache itself; call once per frame.
 * calibrate(): centre 50% of the frame must be white/grey; call on the frame
 * as captured, before apply(). */
void imx519_swwb_apply(void);

#if IMX519_RAW_DUMP_MODE
/* RAW capture test firmware: send the current WDMA2 raw frame over UART. */
void imx519_raw_dump_frame(void);
#endif
void imx519_swwb_calibrate(void);
void imx519_swwb_set_enable(uint8_t enable);
void imx519_swwb_set_gains_x1000(uint16_t gain_b, uint16_t gain_g, uint16_t gain_r);

#ifdef __cplusplus
}
#endif

#endif /* APP_SCENARIO_CISDP_SENSOR_H_ */
