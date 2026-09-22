		/**
		 * IMX519 mode_2328x1748_regs (2x2 binned, 30fps)
		 * From Linux kernel raspberrypi/linux drivers/media/i2c/imx519.c
		 *
		 * PLL settings (0x0301~0x0310): Platform dependent, may need modification
		 * MIPI link speed: 0x0820-0x0823 = 0x0a20 = 2592 (408MHz * 2 * 2lanes / 10bit * 8)
		 * Output size: 2328 x 1748 (binned from 4656 x 3496)
		 */

		/* MIPI configuration - Non-Continuous clock mode to match Himax WE2 D-PHY standard */
		{ HX_CIS_I2C_Action_W, 0x0111, 0x02},
		{ HX_CIS_I2C_Action_W, 0x0112, 0x0a},  /* CSI_DT RAW10 */
		{ HX_CIS_I2C_Action_W, 0x0113, 0x0a},  /* CSI_DT RAW10 */
		{ HX_CIS_I2C_Action_W, 0x0114, 0x01},  /* CSI 2 lanes (value = lanes - 1) */

		/* Line length */
		{ HX_CIS_I2C_Action_W, 0x0342, 0x19},  /* LINE_LENGTH_PCK[15:8] */
		{ HX_CIS_I2C_Action_W, 0x0343, 0x70},  /* LINE_LENGTH_PCK[7:0] = 0x1970 = 6512 */

		/* Frame length */
		{ HX_CIS_I2C_Action_W, 0x0340, 0x08},  /* FRM_LENGTH_LINES[15:8] */
		{ HX_CIS_I2C_Action_W, 0x0341, 0x88},  /* FRM_LENGTH_LINES[7:0] = 0x0888 = 2184 */

		/* Analog crop window (full sensor area 2328x1748) */
		{ HX_CIS_I2C_Action_W, 0x0344, 0x00},  /* X_ADD_STA[15:8] */
		{ HX_CIS_I2C_Action_W, 0x0345, 0x00},  /* X_ADD_STA[7:0] = 0 */
		{ HX_CIS_I2C_Action_W, 0x0346, 0x00},  /* Y_ADD_STA[15:8] */
		{ HX_CIS_I2C_Action_W, 0x0347, 0x00},  /* Y_ADD_STA[7:0] = 0 */
		{ HX_CIS_I2C_Action_W, 0x0348, 0x12},  /* X_ADD_END[15:8] */
		{ HX_CIS_I2C_Action_W, 0x0349, 0x2f},  /* X_ADD_END[7:0] = 0x122F = 4655 */
		{ HX_CIS_I2C_Action_W, 0x034a, 0x0d},  /* Y_ADD_END[15:8] */
		{ HX_CIS_I2C_Action_W, 0x034b, 0xa7},  /* Y_ADD_END[7:0] = 0x0DA7 = 3495 */

		/* HDR setting */
		{ HX_CIS_I2C_Action_W, 0x0220, 0x00},
		{ HX_CIS_I2C_Action_W, 0x0221, 0x11},
		{ HX_CIS_I2C_Action_W, 0x0222, 0x01},

		/* Binning mode: 2x2 binning enabled */
		{ HX_CIS_I2C_Action_W, 0x0900, 0x01},  /* BINNING_MODE = ON */
		{ HX_CIS_I2C_Action_W, 0x0901, 0x22},  /* BINNING_TYPE = 2x2 */
		{ HX_CIS_I2C_Action_W, 0x0902, 0x0a},  /* BINNING_WEIGHTING */

		/* Sensor-specific mode registers */
		{ HX_CIS_I2C_Action_W, 0x3f4c, 0x05},
		{ HX_CIS_I2C_Action_W, 0x3f4d, 0x03},
		{ HX_CIS_I2C_Action_W, 0x4254, 0x7f},

		/* Digital scaling (disabled) */
		{ HX_CIS_I2C_Action_W, 0x0401, 0x00},
		{ HX_CIS_I2C_Action_W, 0x0404, 0x00},
		{ HX_CIS_I2C_Action_W, 0x0405, 0x10},

		/* Digital crop: 2304x1744 from 2328x1748 binned array */
		{ HX_CIS_I2C_Action_W, 0x0408, 0x00},  /* DIG_CROP_X_OFFSET[15:8] */
		{ HX_CIS_I2C_Action_W, 0x0409, 0x0c},  /* DIG_CROP_X_OFFSET[7:0] = 12 */
		{ HX_CIS_I2C_Action_W, 0x040a, 0x00},  /* DIG_CROP_Y_OFFSET[15:8] */
		{ HX_CIS_I2C_Action_W, 0x040b, 0x02},  /* DIG_CROP_Y_OFFSET[7:0] = 2 */
		{ HX_CIS_I2C_Action_W, 0x040c, 0x09},  /* DIG_CROP_IMAGE_WIDTH[15:8] */
		{ HX_CIS_I2C_Action_W, 0x040d, 0x00},  /* DIG_CROP_IMAGE_WIDTH[7:0] = 0x0900 = 2304 */
		{ HX_CIS_I2C_Action_W, 0x040e, 0x06},  /* DIG_CROP_IMAGE_HEIGHT[15:8] */
		{ HX_CIS_I2C_Action_W, 0x040f, 0xd0},  /* DIG_CROP_IMAGE_HEIGHT[7:0] = 0x06D0 = 1744 */

		/* Output size: 2304x1744 */
		{ HX_CIS_I2C_Action_W, 0x034c, 0x09},  /* X_OUTPUT_SIZE[15:8] */
		{ HX_CIS_I2C_Action_W, 0x034d, 0x00},  /* X_OUTPUT_SIZE[7:0] = 0x0900 = 2304 */
		{ HX_CIS_I2C_Action_W, 0x034e, 0x06},  /* Y_OUTPUT_SIZE[15:8] */
		{ HX_CIS_I2C_Action_W, 0x034f, 0xd0},  /* Y_OUTPUT_SIZE[7:0] = 0x06D0 = 1744 */

		/* PLL settings - Proven working Himax WE2 D-PHY RX lock configuration */
		{ HX_CIS_I2C_Action_W, 0x0301, 0x06},  /* VT_PIX_CLK_DIV */
		{ HX_CIS_I2C_Action_W, 0x0303, 0x04},  /* VT_SYS_CLK_DIV */
		{ HX_CIS_I2C_Action_W, 0x0305, 0x06},  /* PRE_PLL_CLK_DIV = 6 (24MHz/6=4MHz) */
		{ HX_CIS_I2C_Action_W, 0x0306, 0x01},  /* PLL_MULTIPLIER[15:8] */
		{ HX_CIS_I2C_Action_W, 0x0307, 0x40},  /* PLL_MULTIPLIER[7:0] = 320 */
		{ HX_CIS_I2C_Action_W, 0x0309, 0x0a},  /* OP_PIX_CLK_DIV */
		{ HX_CIS_I2C_Action_W, 0x030b, 0x02},  /* OP_SYS_CLK_DIV */
		{ HX_CIS_I2C_Action_W, 0x030d, 0x04},  /* PRE_PLL_CLK_DIV2 = 4 */
		{ HX_CIS_I2C_Action_W, 0x030e, 0x01},  /* PLL_MULTIPLIER2[15:8] */
		{ HX_CIS_I2C_Action_W, 0x030f, 0x10},  /* PLL_MULTIPLIER2[7:0] = 272 */
		{ HX_CIS_I2C_Action_W, 0x0310, 0x01},  /* PLL_MODE = Dual PLL */

		/* MIPI data rate - 816Mbps per lane x 2 lanes = 1632Mbps total */
		{ HX_CIS_I2C_Action_W, 0x0820, 0x06},  /* REQ_LINK_BIT_RATE[31:24] */
		{ HX_CIS_I2C_Action_W, 0x0821, 0x60},  /* REQ_LINK_BIT_RATE[23:16] = 0x0660 = 1632 Mbps */
		{ HX_CIS_I2C_Action_W, 0x0822, 0x00},  /* REQ_LINK_BIT_RATE[15:8] */
		{ HX_CIS_I2C_Action_W, 0x0823, 0x00},  /* REQ_LINK_BIT_RATE[7:0] */

		/* Global timing */
		{ HX_CIS_I2C_Action_W, 0x3e20, 0x01},
		{ HX_CIS_I2C_Action_W, 0x3e37, 0x01},
		{ HX_CIS_I2C_Action_W, 0x3e3b, 0x00},

		/* QBC adjustment */
		{ HX_CIS_I2C_Action_W, 0x38a4, 0x00},
		{ HX_CIS_I2C_Action_W, 0x38a5, 0x00},
		{ HX_CIS_I2C_Action_W, 0x38a6, 0x00},
		{ HX_CIS_I2C_Action_W, 0x38a7, 0x00},
		{ HX_CIS_I2C_Action_W, 0x38a8, 0x00},
		{ HX_CIS_I2C_Action_W, 0x38a9, 0x91},
		{ HX_CIS_I2C_Action_W, 0x38aa, 0x00},
		{ HX_CIS_I2C_Action_W, 0x38ab, 0x91},

		/* Misc - DPC & Black Level Compensation ENABLED */
		{ HX_CIS_I2C_Action_W, 0x0106, 0x00},
		{ HX_CIS_I2C_Action_W, 0x0b00, 0x01},
		{ HX_CIS_I2C_Action_W, 0x3230, 0x00},
		{ HX_CIS_I2C_Action_W, 0x3f14, 0x01},
		{ HX_CIS_I2C_Action_W, 0x3f3c, 0x01},
		{ HX_CIS_I2C_Action_W, 0x3f0d, 0x0a},
		{ HX_CIS_I2C_Action_W, 0x3fbc, 0x00},
		{ HX_CIS_I2C_Action_W, 0x3c06, 0x00},
		{ HX_CIS_I2C_Action_W, 0x3c07, 0x48},
		{ HX_CIS_I2C_Action_W, 0x3c0a, 0x00},
		{ HX_CIS_I2C_Action_W, 0x3c0b, 0x00},
		{ HX_CIS_I2C_Action_W, 0x3f78, 0x00},
		{ HX_CIS_I2C_Action_W, 0x3f79, 0x40},
		{ HX_CIS_I2C_Action_W, 0x3f7c, 0x00},
		{ HX_CIS_I2C_Action_W, 0x3f7d, 0x00},
