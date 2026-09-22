/*
 * cisdp_sensor.c
 *
 *  Created on: 20240122
 *      Author: 901912
 */

#include "cisdp_sensor.h"
#include "cisdp_cfg.h"
#include "hx_drv_CIS_common.h"
#include "hx_drv_timer.h"
#include "hx_drv_hxautoi2c_mst.h"
#include "hx_drv_CIS_common.h"

#include "WE2_core.h"
#include "WE2_debug.h"
#include "hx_drv_swreg_aon.h"
#include "hx_drv_scu_export.h"
#include "driver_interface.h"
#include "hx_drv_scu.h"
#include "math.h"
#include "memory_manage.h"
#include "xprintf.h"
#include "hx_drv_xdma.h"
#include "hx_drv_uart.h"

#define GROVE_VISION_AI

#ifdef TRUSTZONE_SEC
#ifdef IP_INST_NS_csirx
#define	CSIRX_REGS_BASE 				BASE_ADDR_MIPI_RX_CTRL
#define CSIRX_DPHY_REG					(BASE_ADDR_APB_MIPI_RX_PHY+0x50)
#define CSIRX_DPHY_TUNCATE_REG			(BASE_ADDR_APB_MIPI_RX_PHY+0x48)
#else
#define CSIRX_REGS_BASE             	BASE_ADDR_MIPI_RX_CTRL_ALIAS
#define CSIRX_DPHY_REG					(BASE_ADDR_APB_MIPI_RX_PHY_ALIAS+0x50)
#define CSIRX_DPHY_TUNCATE_REG			(BASE_ADDR_APB_MIPI_RX_PHY_ALIAS+0x48)
#endif
#else
#ifndef TRUSTZONE
#define CSIRX_REGS_BASE             	BASE_ADDR_MIPI_RX_CTRL_ALIAS
#define CSIRX_DPHY_REG					(BASE_ADDR_APB_MIPI_RX_PHY_ALIAS+0x50)
#define CSIRX_DPHY_TUNCATE_REG			(BASE_ADDR_APB_MIPI_RX_PHY_ALIAS+0x48)
#else
#define CSIRX_REGS_BASE             	BASE_ADDR_MIPI_RX_CTRL
#define CSIRX_DPHY_REG					(BASE_ADDR_APB_MIPI_RX_PHY+0x50)
#define CSIRX_DPHY_TUNCATE_REG			(BASE_ADDR_APB_MIPI_RX_PHY+0x48)
#endif
#endif

#ifndef DYNAMIC_ADDRESS
#define JPEG_BUFSIZE  (((623+ (IMX519_HW5x5_CROP_WIDTH/16)*(IMX519_HW5x5_CROP_HEIGHT/16)* 38 + 35) >>2 ) <<2)	//YUV420 x10 Compress = ((623+ (W/16)*(H/16)* 38 + 35) >>2 ) <<2  byte
__attribute__(( section(".bss.NoInit"))) uint8_t jpegbuf[JPEG_BUFSIZE] __ALIGNED(32);

#define RAW_BUFSIZE  (IMX519_HW5x5_CROP_WIDTH*IMX519_HW5x5_CROP_HEIGHT*3/2)   //YUV420: Y= W*H byte, U = ((W*H)>>2) byte, V = ((W*H)>>2) byte
__attribute__(( section(".bss.NoInit"))) uint8_t demosbuf[RAW_BUFSIZE] __ALIGNED(32);

#define JPEG_HEADER_BUFSIZE 100
__attribute__(( section(".bss.NoInit"))) uint8_t jpegfilesizebuf[JPEG_HEADER_BUFSIZE] __ALIGNED(32);

static volatile uint32_t g_wdma1_baseaddr = (uint32_t)jpegbuf; // = (uint32_t)cdmbuf; // - no use
static volatile uint32_t g_wdma2_baseaddr = (uint32_t)jpegbuf;
static volatile uint32_t g_wdma3_baseaddr = (uint32_t)demosbuf;
static volatile uint32_t g_jpegautofill_addr = (uint32_t)jpegfilesizebuf;
#else
static volatile uint32_t g_wdma1_baseaddr = 0;
static volatile uint32_t g_wdma2_baseaddr = 0;
static volatile uint32_t g_wdma3_baseaddr = 0;
static volatile uint32_t g_jpegautofill_addr = 0;
#endif

static HX_CIS_SensorSetting_t IMX519_common_setting[] = {
#include "IMX519_common_setting.i"
};

static HX_CIS_SensorSetting_t IMX519_2328x1748_setting[] = {
#include "IMX519_mipi_2lane_2328x1748.i"
};

void imx519_set_test_pattern(uint16_t pattern, uint16_t color_r, uint16_t color_gr, uint16_t color_b, uint16_t color_gb) {
    hx_drv_cis_set_reg(0x0600, pattern >> 8, 0); 
    hx_drv_cis_set_reg(0x0601, pattern & 0xFF, 0); 
    /* MIPI CCS: 1 = solid colour, using TEST_DATA_RED / GREENR / BLUE / GREENB
     * (0x0602..0x0609, 10-bit). The colours used to be written only for
     * pattern 2 (colour bars, which ignores them), so solid colour never got
     * its values. */
    if (pattern == 1 || pattern == 2) {
        hx_drv_cis_set_reg(0x0602, color_r >> 8, 0);
        hx_drv_cis_set_reg(0x0603, color_r & 0xFF, 0);
        hx_drv_cis_set_reg(0x0604, color_gr >> 8, 0);
        hx_drv_cis_set_reg(0x0605, color_gr & 0xFF, 0);
        hx_drv_cis_set_reg(0x0606, color_b >> 8, 0);
        hx_drv_cis_set_reg(0x0607, color_b & 0xFF, 0);
        hx_drv_cis_set_reg(0x0608, color_gb >> 8, 0);
        hx_drv_cis_set_reg(0x0609, color_gb & 0xFF, 0);
    }
}

static HX_CIS_SensorSetting_t IMX519_stream_on[] = {
		{HX_CIS_I2C_Action_W, 0x0100, 0x01},
};

static HX_CIS_SensorSetting_t IMX519_stream_off[] = {
	    {HX_CIS_I2C_Action_W, 0x0100, 0x00},
};

static HX_CIS_SensorSetting_t  IMX519_exposure_setting[] = {
		{HX_CIS_I2C_Action_W, 0x0202, ((IMX519_EXPOSURE_SETTING>>8)&0xFF)},
		{HX_CIS_I2C_Action_W, 0x0203, (IMX519_EXPOSURE_SETTING&0xFF)},
};

static HX_CIS_SensorSetting_t  IMX519_again_setting[] = {
		{HX_CIS_I2C_Action_W, 0x0204, ((IMX519_AGAIN_SETTING>>8)&0xFF)},
		{HX_CIS_I2C_Action_W, 0x0205, (IMX519_AGAIN_SETTING&0xFF)},
};

static HX_CIS_SensorSetting_t  IMX519_dgain_setting[] = {
		{HX_CIS_I2C_Action_W, 0x020e, ((IMX519_DGAIN_SETTING>>8)&0xFF)},
		{HX_CIS_I2C_Action_W, 0x020f, (IMX519_DGAIN_SETTING&0xFF)},
};

static HX_CIS_SensorSetting_t  IMX519_mirror_setting[] = {
		{HX_CIS_I2C_Action_W, 0x0101, (CIS_MIRROR_SETTING&0xFF)},
};


static void cisdp_wdma_addr_init(APP_DP_INP_SUBSAMPLE_E subs)
{
#if IMX519_RAW_DUMP_MODE
    (void)subs;
    imx519_raw_dump_alloc();
#elif defined(DYNAMIC_ADDRESS)
	if(subs == APP_DP_RES_RGB640x480_INP_SUBSAMPLE_1X) {
		g_jpegautofill_addr = mm_reserve_align(100,0x20);
		g_wdma1_baseaddr = mm_reserve_align(76800,0x20); //640*480/4
		if(g_wdma1_baseaddr!=0)
			g_wdma2_baseaddr = g_wdma1_baseaddr;
		else
			return ;

		g_wdma3_baseaddr= mm_reserve_align(921600,0x20); //640*480*3
	}
	else if(subs == APP_DP_RES_RGB640x480_INP_SUBSAMPLE_2X) {
		g_jpegautofill_addr = mm_reserve_align(100,0x20);
		g_wdma1_baseaddr = mm_reserve_align(19200,0x20); //320*240/4
		if(g_wdma1_baseaddr!=0)
			g_wdma2_baseaddr = g_wdma1_baseaddr;
		else
			return ;

		g_wdma3_baseaddr= mm_reserve_align(230400,0x20); //320*240*3
	}
	else if(subs == APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) {
		g_jpegautofill_addr = mm_reserve_align(100,0x20);
		g_wdma1_baseaddr = mm_reserve_align(76800,0x20); //640*480/4
		if(g_wdma1_baseaddr!=0)
			g_wdma2_baseaddr = g_wdma1_baseaddr;
		else
			return ;

		g_wdma3_baseaddr= mm_reserve_align(460800,0x20); //640*480*1.5
	}
	else if(subs == APP_DP_RES_YUV640x480_INP_SUBSAMPLE_2X) {
		g_jpegautofill_addr = mm_reserve_align(100,0x20);
		g_wdma1_baseaddr = mm_reserve_align(19200,0x20); //320*240/4
		if(g_wdma1_baseaddr!=0)
			g_wdma2_baseaddr = g_wdma1_baseaddr;
		else
			return ;

		g_wdma3_baseaddr= mm_reserve_align(115200,0x20); //320*240*1.5
	}
#else
    g_wdma1_baseaddr = (uint32_t)jpegbuf;
    g_wdma2_baseaddr = (uint32_t)jpegbuf;
    g_wdma3_baseaddr = (uint32_t)demosbuf;
    g_jpegautofill_addr = (uint32_t)jpegfilesizebuf;
#endif

    sensordplib_set_xDMA_baseaddrbyapp(g_wdma1_baseaddr, g_wdma2_baseaddr, g_wdma3_baseaddr);
    sensordplib_set_jpegfilesize_addrbyapp(g_jpegautofill_addr);

	xprintf("WD1[%x], WD2_J[%x], WD3_RAW[%x], JPAuto[%x]\n",g_wdma1_baseaddr, g_wdma2_baseaddr,
			g_wdma3_baseaddr, g_jpegautofill_addr);
}


void IMX519_set_pll200()
{
	SCU_PDHSC_DPCLK_CFG_T cfg;

	hx_drv_scu_get_pdhsc_dpclk_cfg(&cfg);

	uint32_t pllfreq;
	hx_drv_swreg_aon_get_pllfreq(&pllfreq);

	if(pllfreq == 400000000)
	{
		cfg.mipiclk.hscmipiclksrc = SCU_HSCMIPICLKSRC_PLL;
		cfg.mipiclk.hscmipiclkdiv = 1;
	}
	else
	{
		cfg.mipiclk.hscmipiclksrc = SCU_HSCMIPICLKSRC_PLL;
		cfg.mipiclk.hscmipiclkdiv = 0;
	}

	hx_drv_scu_set_pdhsc_dpclk_cfg(cfg, 0, 1);

	uint32_t mipi_pixel_clk = 96;
	hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_HSC_MIPI_RXCLK, &mipi_pixel_clk);
	mipi_pixel_clk = mipi_pixel_clk / 1000000;

    dbg_printf(DBG_LESS_INFO, "MIPI CLK change to PLL freq:(%d / %d)\n", pllfreq, (cfg.mipiclk.hscmipiclkdiv+1));
	dbg_printf(DBG_LESS_INFO, "MIPI TX CLK: %dM\n", mipi_pixel_clk);
}


void set_mipi_csirx_enable()
{
	uint32_t bitrate_1lane = IMX519_MIPI_CLOCK_FEQ*2;
	uint32_t mipi_lnno = IMX519_MIPI_LANE_CNT;
	uint32_t pixel_dpp = IMX519_MIPI_DPP;
	uint32_t line_length = IMX519_SENSOR_WIDTH;
	uint32_t total_line_length = 6512; /* IMX519 LINE_LENGTH_PCK for 2328x1748 mode */
	uint32_t frame_length = IMX519_SENSOR_HEIGHT;
	uint32_t byte_clk = bitrate_1lane/8;
	uint32_t continuousout = IMX519_MIPITX_CNTCLK_EN;
	uint32_t deskew_en = 0;
	uint32_t mipi_pixel_clk = 96;

	IMX519_set_pll200();

	hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_HSC_MIPI_RXCLK, &mipi_pixel_clk);
	mipi_pixel_clk = mipi_pixel_clk / 1000000;

	uint32_t n_preload = 15;
	uint32_t l_header = 4;
	uint32_t l_footer = 2;

	dbg_printf(DBG_LESS_INFO, "MIPI CSI Init Enable\n");
	dbg_printf(DBG_LESS_INFO, "MIPI TX CLK: %dM\n", mipi_pixel_clk);
	dbg_printf(DBG_LESS_INFO, "MIPI BITRATE 1LANE: %dM\n", bitrate_1lane);
	dbg_printf(DBG_LESS_INFO, "MIPI DATA LANE: %d\n", mipi_lnno);
	dbg_printf(DBG_LESS_INFO, "MIPI PIXEL DEPTH: %d\n", pixel_dpp);
	dbg_printf(DBG_LESS_INFO, "MIPI LINE LENGTH: %d (Total: %d)\n", line_length, total_line_length);
	dbg_printf(DBG_LESS_INFO, "MIPI FRAME LENGTH: %d\n", frame_length);
	dbg_printf(DBG_LESS_INFO, "MIPI BYTE CLK: %d\n", byte_clk);

	double t_input = (double)(l_header+line_length*pixel_dpp/8+l_footer)/(mipi_lnno*byte_clk)+0.06;
	double t_output = (double)line_length/mipi_pixel_clk;
	double t_preload = (double)(7+(n_preload*4)/mipi_lnno)/mipi_pixel_clk;

	double delta_t = t_input - t_output - t_preload;

	dbg_printf(DBG_LESS_INFO, "t_input: %dns\n", (uint32_t)(t_input*1000));
	dbg_printf(DBG_LESS_INFO, "t_output: %dns\n", (uint32_t)(t_output*1000));
	dbg_printf(DBG_LESS_INFO, "t_preload: %dns\n", (uint32_t)(t_preload*1000));

	uint16_t rx_fifo_fill = 0;
	uint16_t tx_fifo_fill = 0;

	if(delta_t <= 0)
	{
		delta_t = 0 - delta_t;
		tx_fifo_fill = ceil(delta_t*byte_clk*mipi_lnno/4/(pixel_dpp/2))*(pixel_dpp/2);
		rx_fifo_fill = 0;
	}
	else
	{
		rx_fifo_fill = ceil(delta_t*byte_clk*mipi_lnno/4/(pixel_dpp/2))*(pixel_dpp/2);
		tx_fifo_fill = 0;
	}
	dbg_printf(DBG_LESS_INFO, "MIPI RX FIFO FILL: %d\n", rx_fifo_fill);
	dbg_printf(DBG_LESS_INFO, "MIPI TX FIFO FILL: %d\n", tx_fifo_fill);

	/*
	 * Reset CSI RX/TX
	 */
	dbg_printf(DBG_LESS_INFO, "RESET MIPI CSI RX/TX\n");
	SCU_DP_SWRESET_T dp_swrst;
	drv_interface_get_dp_swreset(&dp_swrst);
	dp_swrst.HSC_MIPIRX = 0;
	dp_swrst.HSC_MIPITX = 0;

	hx_drv_scu_set_DP_SWReset(dp_swrst);
	hx_drv_timer_cm55x_delay_us(50, TIMER_STATE_DC);

	dp_swrst.HSC_MIPIRX = 1;
	dp_swrst.HSC_MIPITX = 1;
	hx_drv_scu_set_DP_SWReset(dp_swrst);

    MIPIRX_DPHYHSCNT_CFG_T hscnt_cfg;
    hscnt_cfg.mipirx_dphy_hscnt_clk_en = 0;
    hscnt_cfg.mipirx_dphy_hscnt_ln0_en = 1;
    hscnt_cfg.mipirx_dphy_hscnt_ln1_en = 1;

    if(mipi_pixel_clk == 200) //pll200
    {
		hscnt_cfg.mipirx_dphy_hscnt_clk_val = 0x03;
		hscnt_cfg.mipirx_dphy_hscnt_ln0_val = 0x10;
		hscnt_cfg.mipirx_dphy_hscnt_ln1_val = 0x10;
    }
    else if(mipi_pixel_clk == 300) //pll300
    {
		hscnt_cfg.mipirx_dphy_hscnt_clk_val = 0x03;
		hscnt_cfg.mipirx_dphy_hscnt_ln0_val = 0x18;
		hscnt_cfg.mipirx_dphy_hscnt_ln1_val = 0x18;
    }
    else //rc96
    {
		hscnt_cfg.mipirx_dphy_hscnt_clk_val = 0x03;
		hscnt_cfg.mipirx_dphy_hscnt_ln0_val = 0x06;
		hscnt_cfg.mipirx_dphy_hscnt_ln1_val = 0x06;
    }

    sensordplib_csirx_set_hscnt(hscnt_cfg);

    if(pixel_dpp == 10 || pixel_dpp == 8)
    {
    	sensordplib_csirx_set_pixel_depth(pixel_dpp);
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "PIXEL DEPTH fail %d\n", pixel_dpp);
        return;
    }

	sensordplib_csirx_set_deskew(deskew_en);
	sensordplib_csirx_set_fifo_fill(rx_fifo_fill);
    sensordplib_csirx_enable(mipi_lnno);

    CSITX_DPHYCLKMODE_E clkmode;
    if(continuousout)
    {
    	clkmode = CSITX_DPHYCLOCK_CONT;
    }
    else
    {
    	clkmode = CSITX_DPHYCLOCK_NON_CONT;
    }
    sensordplib_csitx_set_dphy_clkmode(clkmode);

    if(pixel_dpp == 10 || pixel_dpp == 8)
    {
    	sensordplib_csitx_set_pixel_depth(pixel_dpp);
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "PIXEL DEPTH fail %d\n", pixel_dpp);
        return;
    }

	sensordplib_csitx_set_deskew(deskew_en);
    sensordplib_csitx_set_fifo_fill(tx_fifo_fill);
    sensordplib_csitx_enable(mipi_lnno, bitrate_1lane, line_length, frame_length);

    /*
     * //VMUTE setting: Enable VMUTE
     * W:0x52001408:0x0000000D:4:4
     */
    SCU_VMUTE_CFG_T ctrl;
    ctrl.timingsrc = SCU_VMUTE_CTRL_TIMING_SRC_VMUTE;
    ctrl.txphypwr = SCU_VMUTE_CTRL_TXPHY_PWR_DISABLE;
    ctrl.ctrlsrc = SCU_VMUTE_CTRL_SRC_SW;
    ctrl.swctrl = SCU_VMUTE_CTRL_SW_ENABLE;
    hx_drv_scu_set_vmute(&ctrl);
}


void set_mipi_csirx_disable()
{
    sensordplib_csirx_disable();
}


int cisdp_sensor_init()
{
    dbg_printf(DBG_LESS_INFO, "cis_IMX519_init \r\n");

    /*
     * common CIS init
     */
    hx_drv_cis_init((CIS_XHSHUTDOWN_INDEX_E)DEAULT_XHSUTDOWN_PIN, SENSORCTRL_MCLK_DIV3);
    dbg_printf(DBG_LESS_INFO, "mclk DIV3, xshutdown_pin=%d\n",DEAULT_XHSUTDOWN_PIN);

    /* Enable Camera Power & Reset via AON_GPIO1 (matches OV5647 hardware power tree) */
    hx_drv_gpio_set_output(AON_GPIO1, GPIO_OUT_HIGH);
    hx_drv_scu_set_PA1_pinmux(SCU_PA1_PINMUX_AON_GPIO1, 1);
    hx_drv_gpio_set_out_value(AON_GPIO1, GPIO_OUT_HIGH);
    dbg_printf(DBG_LESS_INFO, "Set PA1(AON_GPIO1) to High (Full Camera & VCM Power Active)\n");
    hx_drv_timer_cm55x_delay_ms(50, TIMER_STATE_DC);

    hx_drv_cis_set_slaveID(CIS_I2C_ID);
    dbg_printf(DBG_LESS_INFO, "hx_drv_cis_set_slaveID(0x%02X)\n", CIS_I2C_ID);
    /*
     * off stream before init sensor
     */
    if(hx_drv_cis_setRegTable(IMX519_stream_off, HX_CIS_SIZE_N(IMX519_stream_off, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
    	dbg_printf(DBG_LESS_INFO, "IMX519 off by app fail\r\n");
        return -1;
    }

    if(hx_drv_cis_setRegTable(IMX519_common_setting, HX_CIS_SIZE_N(IMX519_common_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
        dbg_printf(DBG_LESS_INFO, "IMX519 Init setting by app fail (IMX519_common_setting)\r\n");
        return -1;
    }
    else
    {
        dbg_printf(DBG_LESS_INFO, "IMX519 Init setting by app (IMX519_common_setting)\n");
    }

    if(hx_drv_cis_setRegTable(IMX519_2328x1748_setting, HX_CIS_SIZE_N(IMX519_2328x1748_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
        dbg_printf(DBG_LESS_INFO, "IMX519 Init setting by app fail (IMX519_2328x1748_setting)\r\n");
        return -1;
    }
    else
    {
        dbg_printf(DBG_LESS_INFO, "IMX519 Init setting by app (IMX519_2328x1748_setting)\n");
    }

    //IMX519_set_exposure
    if(hx_drv_cis_setRegTable(IMX519_exposure_setting, HX_CIS_SIZE_N(IMX519_exposure_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
        dbg_printf(DBG_LESS_INFO, "IMX519 Init by app fail (IMX519_exposure_setting)\n");
		return -1;
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX519 Init by app (IMX519_exposure_setting)\n");
    }

    //IMX519_set_again
    if(hx_drv_cis_setRegTable(IMX519_again_setting, HX_CIS_SIZE_N(IMX519_again_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
        dbg_printf(DBG_LESS_INFO, "IMX519 Init by app fail (IMX519_again_setting)\n");
		return -1;
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX519 Init by app (IMX519_again_setting)\n");
    }

    //IMX519_set_dgain
    if(hx_drv_cis_setRegTable(IMX519_dgain_setting, HX_CIS_SIZE_N(IMX519_dgain_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
        dbg_printf(DBG_LESS_INFO, "IMX519 Init by app fail (IMX519_dgain_setting)\n");
		return -1;
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX519 Init by app (IMX519_dgain_setting)\n");
    }

    //IMX519_set_mirror
    if(hx_drv_cis_setRegTable(IMX519_mirror_setting, HX_CIS_SIZE_N(IMX519_mirror_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
        dbg_printf(DBG_LESS_INFO, "IMX519 Init by app fail (IMX519_mirror_setting)\n");
		return -1;
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX519 Init by app (IMX519_mirror_setting)\n");
    }

    /* Wait for PLL lock after mode setting */
    hx_drv_timer_cm55x_delay_ms(10, TIMER_STATE_DC);

    /* Phase 2A: I2C Readback Validation - IMX519 Chip ID at 0x0016/0x0017 */
    uint8_t id_h = 0, id_l = 0;
    hx_drv_cis_get_reg(0x0016, &id_h);
    hx_drv_cis_get_reg(0x0017, &id_l);
    dbg_printf(DBG_LESS_INFO, "IMX519 Sensor ID: 0x%02X%02X\n", id_h, id_l);
    if(id_h != 0x05 || id_l != 0x19) {
        dbg_printf(DBG_LESS_INFO, "Warning: Sensor ID mismatch! Expected 0x0519, got 0x%02X%02X\n", id_h, id_l);
    }
    
    /* Phase 2A-1: I2C Readback Validation - Standby/Streaming Reg 0x0100 */
    uint8_t mode = 0;
    hx_drv_cis_get_reg(0x0100, &mode);
    dbg_printf(DBG_LESS_INFO, "IMX519 Standby/Streaming Reg(0x0100): 0x%02X\n", mode);

    /* Phase 28: Real Live Camera Mode - Disable Test Pattern for real physical image streaming */
    imx519_set_test_pattern(0, 0, 0, 0, 0);
    dbg_printf(DBG_LESS_INFO, "IMX519 Test Pattern: DISABLED (Real Live Camera Streaming)\n");

    /*
     * Exposure and gain are set only by the init tables above, from
     * cisdp_cfg.h (IMX519_EXPOSURE_SETTING / IMX519_AGAIN_SETTING /
     * IMX519_DGAIN_SETTING). A hardcoded block here (Phase 106/120) used to
     * overwrite all three - 1280 lines, analog gain code 960, digital gain
     * 1.5x - silently shadowing the config. Code 960 is 16x, the sensor's
     * maximum, not the 8x its comment claimed.
     */

    /* Phase 30: I2C Readback Diagnostics - verify what the sensor actually has latched */
    uint8_t rb_exp_h = 0, rb_exp_l = 0;
    uint8_t rb_ag_h = 0, rb_ag_l = 0;
    uint8_t rb_dg_h = 0, rb_dg_l = 0;
    uint8_t rb_tp_h = 0, rb_tp_l = 0;
    uint8_t rb_blc = 0;
    uint8_t rb_bin_mode = 0, rb_bin_type = 0;
    uint8_t rb_frl_h = 0, rb_frl_l = 0;

    hx_drv_cis_get_reg(0x0202, &rb_exp_h);
    hx_drv_cis_get_reg(0x0203, &rb_exp_l);
    hx_drv_cis_get_reg(0x0204, &rb_ag_h);
    hx_drv_cis_get_reg(0x0205, &rb_ag_l);
    hx_drv_cis_get_reg(0x020e, &rb_dg_h);
    hx_drv_cis_get_reg(0x020f, &rb_dg_l);
    hx_drv_cis_get_reg(0x0600, &rb_tp_h);
    hx_drv_cis_get_reg(0x0601, &rb_tp_l);
    hx_drv_cis_get_reg(0x0b00, &rb_blc);
    hx_drv_cis_get_reg(0x0900, &rb_bin_mode);
    hx_drv_cis_get_reg(0x0901, &rb_bin_type);
    hx_drv_cis_get_reg(0x0340, &rb_frl_h);
    hx_drv_cis_get_reg(0x0341, &rb_frl_l);

    dbg_printf(DBG_LESS_INFO, "IMX519 READBACK: Exposure=0x%02X%02X, AGain=0x%02X%02X, DGain=0x%02X%02X\n",
               rb_exp_h, rb_exp_l, rb_ag_h, rb_ag_l, rb_dg_h, rb_dg_l);
    dbg_printf(DBG_LESS_INFO, "IMX519 READBACK: TestPattern=0x%02X%02X, BLC=0x%02X, Bin=%02X/%02X, FRL=0x%02X%02X\n",
               rb_tp_h, rb_tp_l, rb_blc, rb_bin_mode, rb_bin_type, rb_frl_h, rb_frl_l);

    return 0;
}


static APP_DP_INP_SUBSAMPLE_E g_subs = APP_DP_RES_YUV640x480_INP_SUBSAMPLE_2X;

int cisdp_dp_init(bool inp_init, SENSORDPLIB_PATH_E dp_type, evthandlerdp_CBEvent_t cb_event, uint32_t jpg_ratio, APP_DP_INP_SUBSAMPLE_E subs)
{
    HW2x2_CFG_T hw2x2_cfg;
    CDM_CFG_T cdm_cfg;
    HW5x5_CFG_T hw5x5_cfg;
    JPEG_CFG_T jpeg_cfg;

    g_subs = subs;

    //HW2x2 Cfg
    hw2x2_cfg.hw2x2_path = DP_HW2X2_PATH;
    hw2x2_cfg.hw_22_process_mode = DP_HW2X2_PROCESS_MODE;
    hw2x2_cfg.hw_22_crop_stx = DP_HW2X2_CROP_START_X;
    hw2x2_cfg.hw_22_crop_sty = DP_HW2X2_CROP_START_Y;
    hw2x2_cfg.hw_22_in_width = DP_HW2X2_CROP_WIDTH;
    hw2x2_cfg.hw_22_in_height = DP_HW2X2_CROP_HEIGHT;
    hw2x2_cfg.hw_22_mono_round_mode = DP_HW2X2_ROUND_MODE;

    //CDM Cfg
    cdm_cfg.cdm_enable = DP_CDM_ENABLE;
    cdm_cfg.cdm_crop_stx = DP_CDM_IN_START_X;
    cdm_cfg.cdm_crop_sty = DP_CDM_IN_START_Y;
    cdm_cfg.cdm_in_width = DP_CDM_IN_WIDTH;
    cdm_cfg.cdm_in_height = DP_CDM_IN_HEIGHT;
    cdm_cfg.meta_dump = DP_CDM_META_DUMP;
    cdm_cfg.ht_packing = DP_CDM_HT_PACKING;
    cdm_cfg.cdm_min_allow_dis = DP_CDM_MIN_ALLOW_DIS;
    cdm_cfg.cdm_tolerance = DP_CDM_TOLERANCE;
    cdm_cfg.cdm_reactance = DP_CDM_REACTANCE;
    cdm_cfg.cdm_relaxation = DP_CDM_RELAXATION;
    cdm_cfg.cdm_eros_th = DP_CDM_EROS_TH;
    cdm_cfg.cdm_num_ht_th = DP_CDM_NUM_HT_TH;
    cdm_cfg.cdm_num_ht_vect_th_x = DP_CDM_NUM_HT_VECT_TH_X;
    cdm_cfg.cdm_num_ht_vect_th_y = DP_CDM_NUM_HT_VECT_TH_X;
    cdm_cfg.cdm_num_cons_ht_bin_th_x = DP_CDM_NUM_CONS_HT_BIN_TH_X;
    cdm_cfg.cdm_num_cons_ht_bin_th_y = DP_CDM_NUM_CONS_HT_BIN_TH_Y;
    cdm_cfg.cpu_activeflag = DP_CDM_CPU_ACTIVEFLAG;
    cdm_cfg.init_map_flag = DP_CDM_INIT_MAP_FLAG;

    //HW5x5 Cfg
    hw5x5_cfg.hw5x5_path = DP_HW5X5_PATH;
    hw5x5_cfg.demos_bndmode = DP_HW5X5_DEMOS_BNDMODE;
    hw5x5_cfg.demos_color_mode = DP_HW5X5_DEMOS_COLORMODE;
    hw5x5_cfg.demos_pattern_mode = DP_HW5X5_DEMOS_PATTERN;
    hw5x5_cfg.demoslpf_roundmode = DP_HW5X5_DEMOSLPF_ROUNDMODE;
    hw5x5_cfg.hw55_crop_stx = DP_HW5X5_CROP_START_X;
    hw5x5_cfg.hw55_crop_sty = DP_HW5X5_CROP_START_Y;
    hw5x5_cfg.hw55_in_width = (subs==APP_DP_RES_RGB640x480_INP_SUBSAMPLE_1X||subs==APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X)?
    		640:(subs==APP_DP_RES_RGB640x480_INP_SUBSAMPLE_2X||subs==APP_DP_RES_YUV640x480_INP_SUBSAMPLE_2X)?
    		320:640;
    hw5x5_cfg.hw55_in_height = (subs==APP_DP_RES_RGB640x480_INP_SUBSAMPLE_1X||subs==APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X)?
    		480:(subs==APP_DP_RES_RGB640x480_INP_SUBSAMPLE_2X||subs==APP_DP_RES_YUV640x480_INP_SUBSAMPLE_2X)?
    		240:480;

    //JPEG Cfg
    jpeg_cfg.jpeg_path = DP_JPEG_PATH;
    jpeg_cfg.enc_width = hw5x5_cfg.hw55_in_width;
    jpeg_cfg.enc_height = hw5x5_cfg.hw55_in_height;
    jpeg_cfg.jpeg_enctype = DP_JPEG_ENCTYPE;
    jpeg_cfg.jpeg_encqtable = DP_JPEG_ENCQTABLE;

    cisdp_wdma_addr_init(subs);

    //setup MIPI RX & CSITX with Raw Sensor resolution
	set_mipi_csirx_enable();

    INP_CROP_T crop;
    crop.start_x = DP_INP_CROP_START_X;
    crop.start_y = DP_INP_CROP_START_Y;

    if(DP_INP_CROP_WIDTH >= 1)
    	crop.last_x = crop.start_x + DP_INP_CROP_WIDTH - 1;
    else
    	crop.last_x = crop.start_x;

    if(DP_INP_CROP_HEIGHT >= 1)
    	crop.last_y = crop.start_y + DP_INP_CROP_HEIGHT - 1;
    else
    	crop.last_y = crop.start_y;

    sensordplib_set_sensorctrl_inp_wi_crop_bin(SENCTRL_SENSOR_TYPE, SENCTRL_STREAM_TYPE, SENCTRL_SENSOR_WIDTH, SENCTRL_SENSOR_HEIGHT, DP_INP_SUBSAMPLE, crop, DP_INP_BINNING);

	uint8_t cyclic_buffer_cnt = 1;

	int32_t non_support = 0;
	switch (dp_type)
	{
	case SENSORDPLIB_PATH_INP_WDMA2:
	    sensordplib_set_raw_wdma2(DP_INP_OUT_WIDTH, DP_INP_OUT_HEIGHT,
#if IMX519_RAW_DUMP_MODE
	    		imx519_raw_dp_cb);
#if (IMX519_RAW_DUMP_SUBS == 0)
	    hx_drv_xdma_WDMA2_register_cb(imx519_raw_wdma2_isr);	/* see imx519_raw_wdma2_isr() */
#endif
#else
	    		NULL);
#endif
	    break;
	case SENSORDPLIB_PATH_INP_HW2x2_CDM:
	    sensordplib_set_HW2x2_CDM(hw2x2_cfg, cdm_cfg,
	    		NULL);
	    break;
	case SENSORDPLIB_PATH_INP_HW5x5:
	    sensordplib_set_hw5x5_wdma3(hw5x5_cfg,
	    		NULL);
	    break;
	case SENSORDPLIB_PATH_INP_HW5x5_JPEG:
	    sensordplib_set_hw5x5_jpeg_wdma2(hw5x5_cfg
	            , jpeg_cfg,
				cyclic_buffer_cnt,
				NULL);
	    break;
	case SENSORDPLIB_PATH_INP_HW2x2:
		sensordplib_set_HW2x2_wdma1(hw2x2_cfg, NULL);
		break;
	case SENSORDPLIB_PATH_INP_CDM:
		sensordplib_set_CDM(cdm_cfg, NULL);
		break;
	case SENSORDPLIB_PATH_INT1:
	    sensordplib_set_INT1_HWACC(hw2x2_cfg,
	            cdm_cfg, hw5x5_cfg,jpeg_cfg,
				cyclic_buffer_cnt,
	            NULL);
	    break;
	case SENSORDPLIB_PATH_INTNOJPEG:
		sensordplib_set_INTNoJPEG_HWACC(hw2x2_cfg,
	            cdm_cfg, hw5x5_cfg,
	            NULL);
		break;
	case SENSORDPLIB_PATH_INT3:
		sensordplib_set_int_raw_hw5x5_wdma23(DP_INP_OUT_WIDTH,
				DP_INP_OUT_HEIGHT,
				hw5x5_cfg,
				NULL);
		break;
	case SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG:
		if(hw5x5_cfg.demos_color_mode == DEMOS_COLORMODE_RGB)
		{
			sensordplib_set_int_hw5x5rgb_jpeg_wdma23(hw5x5_cfg,jpeg_cfg,
					cyclic_buffer_cnt,
		            NULL);
		}
		else
		{
			sensordplib_set_int_hw5x5_jpeg_wdma23(hw5x5_cfg,jpeg_cfg,
					cyclic_buffer_cnt,
					NULL);
		}
		break;
	case SENSORDPLIB_PATH_INT_INP_HW2x2_HW5x5_JPEG:
		sensordplib_set_int_hw2x2_hw5x5_jpeg_wdma12(hw2x2_cfg,
	            hw5x5_cfg,jpeg_cfg,
				cyclic_buffer_cnt,
	            NULL);
		break;
	case SENSORDPLIB_PATH_JPEGDEC:
	case SENSORDPLIB_PATH_TPG_JPEGENC:
	case SENSORDPLIB_PATH_TPG_HW2x2:
	case SENSORDPLIB_PATH_INP_HXCSC_CDM:
	case SENSORDPLIB_PATH_INP_HXCSC:
	case SENSORDPLIB_PATH_INP_HXCSC_JPEG:
	case SENSORDPLIB_PATH_INT1_CSC:
	case SENSORDPLIB_PATH_INTNOJPEG_CSC:
	case SENSORDPLIB_PATH_INT3_CSC:
	case SENSORDPLIB_PATH_INT_INP_HXCSC_JPEG:
	case SENSORDPLIB_PATH_NO:
	default:
		dbg_printf(DBG_LESS_INFO, "Not support case \r\n");
		non_support = 1;
		break;
	}

	if(non_support == 1)
		return -1;

	if(cb_event != NULL)
		hx_dplib_evthandler_register_cb(cb_event, SENSORDPLIB_CB_FUNTYPE_DP);

	return 0;
}


void cisdp_stream_on()
{
    /*
     * Stream On
     */
    if(hx_drv_cis_setRegTable(IMX519_stream_on, HX_CIS_SIZE_N(IMX519_stream_on, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
    	dbg_printf(DBG_LESS_INFO, "IMX519 on by app fail\r\n");
        return;
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX519 on by app done\r\n");
    }
}


void cisdp_stream_off()
{
    /*
     * Stream Off
     */
    if(hx_drv_cis_setRegTable(IMX519_stream_off, HX_CIS_SIZE_N(IMX519_stream_off, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
    	dbg_printf(DBG_LESS_INFO, "IMX519 off by app fail\r\n");
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX519 off by app \n");
    }
}

void imx519_set_hardware_white_balance(float r_gain, float gr_gain, float gb_gain, float b_gain)
{
    uint16_t r_val  = (uint16_t)(r_gain  * 256.0f);
    uint16_t gr_val = (uint16_t)(gr_gain * 256.0f);
    uint16_t gb_val = (uint16_t)(gb_gain * 256.0f);
    uint16_t b_val  = (uint16_t)(b_gain  * 256.0f);

    hx_drv_cis_set_reg(0x0104, 0x01, 0);  /* GROUP_HOLD = 1 */

    /* Standard Sony Digital Gain Registers (0x0210 ~ 0x0217) */
    hx_drv_cis_set_reg(0x0210, (uint8_t)(gr_val >> 8), 0);   /* Gr MSB */
    hx_drv_cis_set_reg(0x0211, (uint8_t)(gr_val & 0xFF), 0); /* Gr LSB */

    hx_drv_cis_set_reg(0x0212, (uint8_t)(r_val >> 8), 0);    /* R MSB */
    hx_drv_cis_set_reg(0x0213, (uint8_t)(r_val & 0xFF), 0);  /* R LSB */

    hx_drv_cis_set_reg(0x0214, (uint8_t)(b_val >> 8), 0);    /* B MSB */
    hx_drv_cis_set_reg(0x0215, (uint8_t)(b_val & 0xFF), 0);  /* B LSB */

    hx_drv_cis_set_reg(0x0216, (uint8_t)(gb_val >> 8), 0);   /* Gb MSB */
    hx_drv_cis_set_reg(0x0217, (uint8_t)(gb_val & 0xFF), 0); /* Gb LSB */

    /* Sony DSP Block Color Gain Registers (0x341A ~ 0x3421) */
    hx_drv_cis_set_reg(0x341a, (uint8_t)(gr_val >> 8), 0);
    hx_drv_cis_set_reg(0x341b, (uint8_t)(gr_val & 0xFF), 0);
    hx_drv_cis_set_reg(0x341c, (uint8_t)(gb_val >> 8), 0);
    hx_drv_cis_set_reg(0x341d, (uint8_t)(gb_val & 0xFF), 0);
    hx_drv_cis_set_reg(0x341e, (uint8_t)(r_val >> 8), 0);
    hx_drv_cis_set_reg(0x341f, (uint8_t)(r_val & 0xFF), 0);
    hx_drv_cis_set_reg(0x3420, (uint8_t)(b_val >> 8), 0);
    hx_drv_cis_set_reg(0x3421, (uint8_t)(b_val & 0xFF), 0);

    hx_drv_cis_set_reg(0x0104, 0x00, 0);  /* GROUP_HOLD = 0 (Apply immediately) */

    /*
     * Readback verification.
     *
     * Printed as integers only. This firmware links newlib-nano, whose printf
     * has no floating point support: a "%f" prints the literal text and, worse,
     * does not consume its argument, so every conversion after it reads the
     * wrong bytes. The previous version of this line used "%.2f" and its Gr and
     * B readbacks were garbage for exactly that reason - they were showing
     * pieces of the doubles that were never consumed.
     *
     * Values are 8.8 fixed point, so 0x0100 is 1.00x. Compare the readback
     * against the requested value: the per-channel digital gain may well clamp
     * at 1.00x, in which case any request below 0x0100 silently does nothing
     * and green can only be corrected by raising red and blue instead.
     */
    uint8_t rb[8] = {0};
    for (int i = 0; i < 8; i++) {
        hx_drv_cis_get_reg((uint16_t)(0x0210 + i), &rb[i]);
    }

    dbg_printf(DBG_LESS_INFO,
               "IMX519 WB requested (x256): Gr=%u R=%u B=%u Gb=%u\n",
               (unsigned)gr_val, (unsigned)r_val, (unsigned)b_val, (unsigned)gb_val);
    dbg_printf(DBG_LESS_INFO,
               "IMX519 WB readback  (x256): Gr=%u R=%u B=%u Gb=%u  [0x0100 = 1.00x]\n",
               (unsigned)((rb[0] << 8) | rb[1]), (unsigned)((rb[2] << 8) | rb[3]),
               (unsigned)((rb[4] << 8) | rb[5]), (unsigned)((rb[6] << 8) | rb[7]));
}

/*
 * Set coarse integration time (exposure) at runtime, in lines.
 *
 * IMX519_EXPOSURE_SETTING is otherwise written once at init and never again,
 * and the value in cisdp_cfg.h (1024 lines) was tuned for a much darker scene
 * than the one it now runs in - the green channel was measured sitting at a
 * mean of 254/255, i.e. clipped. Clipped data cannot be recovered by any later
 * white balance, so exposure has to be right first.
 *
 * Exposure must stay below the frame length (0x0340/0x0341 = 2184 lines in the
 * 2328x1748 mode) minus the sensor's margin; the Linux imx519 driver keeps a
 * 32-line offset, so anything above 2152 is clamped.
 *
 * Integers only in the log: this build's printf has no %f support.
 */
#define IMX519_FRAME_LENGTH_LINES   (2184)
#define IMX519_EXPOSURE_MARGIN      (32)

void imx519_set_exposure_lines(uint16_t lines)
{
    uint16_t max_lines = IMX519_FRAME_LENGTH_LINES - IMX519_EXPOSURE_MARGIN;
    uint8_t hi = 0, lo = 0;

    if (lines < 1) {
        lines = 1;
    }
    if (lines > max_lines) {
        lines = max_lines;
    }

    hx_drv_cis_set_reg(0x0104, 0x01, 0);            /* GROUP_HOLD on */
    hx_drv_cis_set_reg(0x0202, (uint8_t)(lines >> 8), 0);
    hx_drv_cis_set_reg(0x0203, (uint8_t)(lines & 0xFF), 0);
    hx_drv_cis_set_reg(0x0104, 0x00, 0);            /* GROUP_HOLD off, apply */

    hx_drv_cis_get_reg(0x0202, &hi);
    hx_drv_cis_get_reg(0x0203, &lo);
    dbg_printf(DBG_LESS_INFO, "IMX519 EXPOSURE req=%u readback=%u lines\n",
               (unsigned)lines, (unsigned)((hi << 8) | lo));
}

/*
 * Set analog gain at runtime by raw register code, with readback.
 *
 * IMX519 analog gain is gain = 1024 / (1024 - code) (libcamera's IMX519 helper),
 * so code 0 is 1x and the maximum code 960 is 16x (IMX519_ANA_GAIN_MAX in the
 * Linux driver). Codes above 960 are clamped. Gain is logged x100 because this
 * build's printf has no floating point support.
 */
#define IMX519_ANA_GAIN_CODE_MAX    (960)

void imx519_set_analog_gain_code(uint16_t code)
{
    uint8_t hi = 0, lo = 0;
    uint16_t rb;

    if (code > IMX519_ANA_GAIN_CODE_MAX) {
        code = IMX519_ANA_GAIN_CODE_MAX;
    }

    hx_drv_cis_set_reg(0x0104, 0x01, 0);            /* GROUP_HOLD on */
    hx_drv_cis_set_reg(0x0204, (uint8_t)(code >> 8), 0);
    hx_drv_cis_set_reg(0x0205, (uint8_t)(code & 0xFF), 0);
    hx_drv_cis_set_reg(0x0104, 0x00, 0);            /* GROUP_HOLD off, apply */

    hx_drv_cis_get_reg(0x0204, &hi);
    hx_drv_cis_get_reg(0x0205, &lo);
    rb = (uint16_t)((hi << 8) | lo);
    dbg_printf(DBG_LESS_INFO, "IMX519 AGAIN req=%u readback=%u gain_x100=%u\n",
               (unsigned)code, (unsigned)rb,
               (unsigned)(102400U / (1024U - (rb > IMX519_ANA_GAIN_CODE_MAX ? IMX519_ANA_GAIN_CODE_MAX : rb))));
}


void cisdp_sensor_start()
{
    /*
     * Phase 66: BLC Bypass (0x0b00=0x00) for active live photon stream
     */
    hx_drv_cis_set_reg(0x0b00, 0x00, 0);
    dbg_printf(DBG_LESS_INFO, "IMX519 BLC: BYPASS (0x0b00 = 0x00)\n");

    /*
     * Stream On
     */
    if(hx_drv_cis_setRegTable(IMX519_stream_on, HX_CIS_SIZE_N(IMX519_stream_on, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
    	dbg_printf(DBG_LESS_INFO, "IMX519 on by app fail\r\n");
        return;
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX519 on by app done\r\n");
    }

    sensordplib_set_mclkctrl_xsleepctrl_bySCMode();

    sensordplib_set_sensorctrl_start();

    /* Phase 32A: Readback 0x0100 (Streaming Mode) after HW start to confirm sensor stays in streaming */
    uint8_t st_mode = 0;
    hx_drv_cis_get_reg(0x0100, &st_mode);
    dbg_printf(DBG_LESS_INFO, "IMX519 LIVE STREAMING REG 0x0100: 0x%02X (%s)\n", 
               st_mode, (st_mode == 0x01) ? "STREAMING ACTIVE" : "STANDBY/STOPPED");

    /* Phase 118: Balanced Color Gains (Gr=0.85x, Gb=0.85x, R=1.60x, B=1.80x) */
#if IMX519_RAW_DUMP_MODE
    /* RAW capture test: per-channel sensor gains at unity. They were measured
     * to have no effect on pixels; forced to 1.0 so the test does not depend
     * on that. */
    imx519_set_hardware_white_balance(1.0f, 1.0f, 1.0f, 1.0f);
#else
    imx519_set_hardware_white_balance(1.60f, 0.85f, 0.85f, 1.80f);
#endif

    /* Initialize IMX519 Auto-Focus Subsystem with AK7375 VCM */
    imx519_af_init();
}


void cisdp_sensor_stop()
{
    sensordplib_stop_capture();
    sensordplib_start_swreset();
    sensordplib_stop_swreset_WoSensorCtrl();

    /*
     * Stream Off
     */
    if(hx_drv_cis_setRegTable(IMX519_stream_off, HX_CIS_SIZE_N(IMX519_stream_off, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
    	dbg_printf(DBG_LESS_INFO, "IMX519 off by app fail\r\n");
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX519 off by app \n");
    }

    set_mipi_csirx_disable();
}


void cisdp_mipi_reset()
{
    cisdp_stream_off();
    set_mipi_csirx_disable();
    set_mipi_csirx_enable();
    cisdp_stream_on();
}


void cisdp_get_jpginfo(uint32_t *jpeg_enc_filesize, uint32_t *jpeg_enc_addr)
{
    uint8_t frame_no;
    uint8_t buffer_no = 0;
    uint32_t jpeg_enc_filesize_real;

    hx_drv_xdma_get_WDMA2_bufferNo(&buffer_no);
    hx_drv_xdma_get_WDMA2NextFrameIdx(&frame_no);
    if(frame_no == 0)
    {
        frame_no = buffer_no - 1;
    }else{
        frame_no = frame_no - 1;
    }
    hx_drv_jpeg_get_EncOutRealMEMSize(&jpeg_enc_filesize_real);

    //dbg_printf(DBG_LESS_INFO, "current jpeg_size=0x%x\n", jpeg_enc_filesize_real);

    hx_drv_jpeg_get_FillFileSizeToMem(frame_no, g_jpegautofill_addr, jpeg_enc_filesize);
    hx_drv_jpeg_get_MemAddrByFrameNo(frame_no, g_wdma2_baseaddr, jpeg_enc_addr);

    if( jpeg_enc_filesize_real != *jpeg_enc_filesize)
    {
        dbg_printf(DBG_LESS_INFO, "*jpeg_enc_filesize_real(0x%08X) != *jpeg_enc_filesize(0x%08X)\n"
        		, jpeg_enc_filesize_real, *jpeg_enc_filesize);

        //change value
        *jpeg_enc_filesize = jpeg_enc_filesize_real;
    }

    //dbg_printf(DBG_LESS_INFO, "g_jpegautofill_addr: 0x%08X\n" "g_wdma2_baseaddr: 0x%08X\n", g_jpegautofill_addr, g_wdma2_baseaddr);
    //dbg_printf(DBG_LESS_INFO, "current frame_no=%d, jpeg_size=0x%x,addr=0x%x\n",frame_no,*jpeg_enc_filesize,*jpeg_enc_addr);
}

uint32_t app_get_jpeg_addr()
{
	return g_wdma2_baseaddr;
}

uint32_t app_get_jpeg_sz()
{
    hx_InvalidateDCache_by_Addr((volatile void *)g_jpegautofill_addr, 32);
	return *((uint32_t*)g_jpegautofill_addr);
}

uint32_t app_get_raw_addr()
{
	return g_wdma3_baseaddr;
}

uint32_t app_get_raw_sz()
{
	if (g_subs == APP_DP_RES_RGB640x480_INP_SUBSAMPLE_1X ||
	    g_subs == APP_DP_RES_RGB640x480_INP_SUBSAMPLE_2X ||
	    g_subs == APP_DP_RES_RGB640x480_INP_SUBSAMPLE_4X) {
		return (IMX519_HW5x5_CROP_WIDTH*IMX519_HW5x5_CROP_HEIGHT*3);   //RGB888
	} else {
		return (IMX519_HW5x5_CROP_WIDTH*IMX519_HW5x5_CROP_HEIGHT*3/2); //YUV420
	}
}

uint32_t app_get_raw_width()
{
	return IMX519_HW5x5_CROP_WIDTH;
}

uint32_t app_get_raw_height()
{
	return IMX519_HW5x5_CROP_HEIGHT;
}

uint32_t app_get_raw_channels() {
	if (g_subs == APP_DP_RES_RGB640x480_INP_SUBSAMPLE_1X ||
	    g_subs == APP_DP_RES_RGB640x480_INP_SUBSAMPLE_2X ||
	    g_subs == APP_DP_RES_RGB640x480_INP_SUBSAMPLE_4X) {
		return 3;
	}
	return 1; // YUV420 has 1 channel in Y-plane
}

/*
 * Software black level + white balance on the WDMA3 buffer (the model input).
 *
 * The buffer is planar B, G, R (see imx519_log_raw_stats()). Each plane goes
 * through its own 256-entry table, so the per-frame cost is one lookup per
 * byte whatever the table holds. The table holds
 *     x   = (in - black) * gain * 255 / (255 - black), clamped to 0..255
 *     out = 255 * (x / 255)^(IMX519_SWWB_TONE_EXP_X1000 / 1000)
 * i.e. remove the pedestal, stretch back to full range, apply the channel
 * gain, then re-encode the near-linear result with a gamma-like tone curve so
 * the model input looks like the photos it was trained on. Because the WE2
 * output is not exactly linear, fixed gains are exact only near the brightness
 * they were calibrated at. Linearising first (inverse curve -> gain -> curve)
 * only needs imx519_swwb_build_lut() to fill the tables differently - and the
 * gains re-calibrated in that domain.
 *
 * The JPEG preview comes from WDMA2 and is not affected.
 */
static uint8_t  s_swwb_lut[3][256];
static uint16_t s_swwb_gain_x1000[3] = {
    IMX519_SWWB_GAIN_B_X1000, IMX519_SWWB_GAIN_G_X1000, IMX519_SWWB_GAIN_R_X1000
};
static uint8_t  s_swwb_enabled = IMX519_SWWB_ENABLE_DEFAULT;
static uint8_t  s_swwb_lut_valid = 0;

static void imx519_swwb_build_lut(void)
{
    const uint32_t black = IMX519_SWWB_BLACK_LEVEL;
    const uint32_t span = 255U - black;
    uint8_t tone[256];
    uint32_t c, v;

    /* Tone curve. Runs only when the tables are rebuilt (boot, calibration,
     * gain change), never per frame, so floating point is fine here. */
    for (v = 0; v < 256; v++) {
        if (IMX519_SWWB_TONE_EXP_X1000 == 1000) {
            tone[v] = (uint8_t)v;
        } else {
            double y = 255.0 * pow((double)v / 255.0, (double)IMX519_SWWB_TONE_EXP_X1000 / 1000.0);
            tone[v] = (uint8_t)(y + 0.5);
        }
    }

    for (c = 0; c < 3; c++) {
        for (v = 0; v < 256; v++) {
            uint32_t out = 0;

            if (v > black) {
                out = ((v - black) * s_swwb_gain_x1000[c] * 255U + span * 500U) / (span * 1000U);
                if (out > 255U) {
                    out = 255U;
                }
            }
            s_swwb_lut[c][v] = tone[out];
        }
    }
    s_swwb_lut_valid = 1;
}

void imx519_swwb_set_gains_x1000(uint16_t gain_b, uint16_t gain_g, uint16_t gain_r)
{
    s_swwb_gain_x1000[0] = gain_b;
    s_swwb_gain_x1000[1] = gain_g;
    s_swwb_gain_x1000[2] = gain_r;
    imx519_swwb_build_lut();
    dbg_printf(DBG_LESS_INFO, "SWWB gains_x1000 B=%u G=%u R=%u black=%u tone_x1000=%u\n",
               (unsigned)gain_b, (unsigned)gain_g, (unsigned)gain_r,
               (unsigned)IMX519_SWWB_BLACK_LEVEL, (unsigned)IMX519_SWWB_TONE_EXP_X1000);
}

void imx519_swwb_set_enable(uint8_t enable)
{
    s_swwb_enabled = enable ? 1 : 0;
    dbg_printf(DBG_LESS_INFO, "SWWB %s\n", s_swwb_enabled ? "ON" : "OFF");
}

void imx519_swwb_apply(void)
{
    uint8_t *buf = (uint8_t *)app_get_raw_addr();
    uint32_t npx, c, i;

    if (!s_swwb_enabled || buf == NULL || app_get_raw_channels() != 3) {
        return;
    }
    if (!s_swwb_lut_valid) {
        imx519_swwb_build_lut();
    }

    npx = app_get_raw_width() * app_get_raw_height();
    hx_InvalidateDCache_by_Addr((volatile void *)buf, app_get_raw_sz());
    for (c = 0; c < 3; c++) {
        uint8_t *plane = buf + c * npx;
        const uint8_t *lut = s_swwb_lut[c];

        for (i = 0; i < npx; i++) {
            plane[i] = lut[plane[i]];
        }
    }
    hx_CleanDCache_by_Addr((volatile void *)buf, app_get_raw_sz());
}

/*
 * Derive gains from the centre 50% of the current frame, which must be a
 * white or grey target: gain_c = (G - black) / (C - black), G fixed at 1.0.
 * Must run on the buffer as captured, i.e. before imx519_swwb_apply() in the
 * frame loop. Rejects frames that are clipped or too dark to give a stable
 * ratio.
 */
void imx519_swwb_calibrate(void)
{
    const uint32_t black = IMX519_SWWB_BLACK_LEVEL;
    uint8_t *buf = (uint8_t *)app_get_raw_addr();
    uint32_t w = app_get_raw_width();
    uint32_t h = app_get_raw_height();
    uint64_t sum[3] = { 0, 0, 0 };
    uint32_t mean_x100[3];
    uint32_t gain[3];
    uint32_t npx, n = 0, clipped = 0, x, y, c;

    if (buf == NULL || app_get_raw_channels() != 3 || w < 16 || h < 16) {
        dbg_printf(DBG_LESS_INFO, "SWWB CAL unsupported buffer\n");
        return;
    }

    npx = w * h;
    hx_InvalidateDCache_by_Addr((volatile void *)buf, app_get_raw_sz());
    for (y = h / 4; y < (h * 3) / 4; y++) {
        for (x = w / 4; x < (w * 3) / 4; x++) {
            uint32_t idx = y * w + x;
            uint8_t any_clip = 0;

            for (c = 0; c < 3; c++) {
                uint8_t v = buf[c * npx + idx];

                sum[c] += v;
                if (v >= 250) {
                    any_clip = 1;
                }
            }
            clipped += any_clip;
            n++;
        }
    }
    for (c = 0; c < 3; c++) {
        mean_x100[c] = (uint32_t)((sum[c] * 100U) / n);
    }
    dbg_printf(DBG_LESS_INFO, "SWWB CAL roi mean_x100 B=%u G=%u R=%u clipped=%u%%\n",
               (unsigned)mean_x100[0], (unsigned)mean_x100[1], (unsigned)mean_x100[2],
               (unsigned)((uint64_t)clipped * 100U / n));

    if (clipped * 100U > n) {
        dbg_printf(DBG_LESS_INFO, "SWWB CAL rejected: over 1%% of ROI clipped - lower exposure or light\n");
        return;
    }
    for (c = 0; c < 3; c++) {
        if (mean_x100[c] < (black + 20U) * 100U) {
            dbg_printf(DBG_LESS_INFO, "SWWB CAL rejected: ROI too dark - every plane must average >= %u\n",
                       (unsigned)(black + 20U));
            return;
        }
    }

    for (c = 0; c < 3; c++) {
        gain[c] = ((mean_x100[1] - black * 100U) * 1000U) / (mean_x100[c] - black * 100U);
        if (gain[c] < 250U) {
            gain[c] = 250U;
        } else if (gain[c] > 8000U) {
            gain[c] = 8000U;
        }
    }
    imx519_swwb_set_gains_x1000((uint16_t)gain[0], (uint16_t)gain[1], (uint16_t)gain[2]);
}
