// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/crc32.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/moduleparam.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/unaligned.h>
#include <linux/units.h>
#include <media/v4l2-subdev.h>

#include "isp4.h"
#include "isp4_debug.h"
#include "isp4_fw_cmd_resp.h"
#include "isp4_interface.h"

#define ISP4SD_MIN_BUF_CNT_BEF_START_STREAM 4

/*
 * EXPERIMENT (GZ302 bring-up, not for upstream): make the buffer count that
 * gates ISP4FW_CMD_ID_START_STREAM tunable.  The default keeps the stock value
 * so loading the module without the parameter behaves exactly as before.
 */
static unsigned int min_start_bufs = ISP4SD_MIN_BUF_CNT_BEF_START_STREAM;
module_param(min_start_bufs, uint, 0644);
MODULE_PARM_DESC(min_start_bufs,
		 "EXPERIMENT: buffers sent to the ISP before START_STREAM (default 4, use >= 1)");

/*
 * EXPERIMENT (GZ302 bring-up, not for upstream): nothing in this driver starts the OV13B sensor,
 * so the firmware arms its MIPI receiver and times out with zero frames (findings section 32).
 * With start_sensor != 0 the driver finds the ov13b10 I2C client on the ISP's own I2C bus and calls
 * its V4L2 subdev s_stream(1) (power-on, chip-ID check, PLL and mode registers, stream-on) and
 * s_stream(0) when the stream is torn down.
 *   0 = off (default)   1 = start after START_STREAM has been queued   2 = start before it
 */
#define EXP_SENSOR_I2C_NAME "i2c-ov13b10"

static unsigned int start_sensor = 1;	/* patch 0035: default 1 */
module_param(start_sensor, uint, 0444);
MODULE_PARM_DESC(start_sensor,
		 "EXPERIMENT: start the OV13B via ov13b10 s_stream: 0 off, 1 after START_STREAM is queued, 2 before");

/*
 * EXPERIMENT (GZ302): the sensor's enable pin is a GPIO of the ISP block (amdisp-pinctrl GPIO 0, mapped as the
 * ov13b10 "reset" line by an earlier patch) and that block is power-gated when idle.  The sensor was alive at
 * probe, but the NACKs of findings section 36 suggest the pin state is lost while the ov13b10 driver still
 * thinks it is powered (runtime status "active").  With sensor_pm_cycle=1 the sensor is forced through its own
 * runtime suspend and resume once the ISP is up (power_off then power_on of the driver: the reset GPIO is
 * asserted and released again), before the raw I2C probe and s_stream.  Needs start_sensor != 0.
 */
static bool sensor_pm_cycle = true;	/* patch 0035: default on */
module_param(sensor_pm_cycle, bool, 0444);
MODULE_PARM_DESC(sensor_pm_cycle,
		 "EXPERIMENT: force an ov13b10 runtime suspend+resume (re-drives its enable GPIO) before starting it; needs start_sensor");

/*
 * EXPERIMENT (GZ302, patch 0023): the "enable_isp" GPIO (AMDI0030:00 pin 85, assigned to this board by an earlier
 * patch only by analogy with the OV05C10 reference platform) goes high when the capture node is opened and low when
 * it is closed.  The front-camera LED lights during exactly that window (findings sections 50 and 51).  With
 * skip_enable_gpio=1 the driver never changes the line, which stays low from probe, to see whether the LED still
 * comes on and whether the ISP still boots without it.
 */
static bool skip_enable_gpio;
module_param(skip_enable_gpio, bool, 0444);
MODULE_PARM_DESC(skip_enable_gpio,
		 "EXPERIMENT: never drive the enable_isp GPIO (AMDI0030:00 pin 85); it stays low");

/*
 * EXPERIMENT (GZ302, patch 0025): the Windows init table programs the OV13B PLL as 0x0303=0x01, 0x0305=0x46,
 * 0x0325=0x55, which gives 1120 Mbps/lane only from a 24 MHz input clock; the Linux driver programs 0x04/0xaf/0xa4,
 * which gives 1120 Mbps only from 19.2 MHz (findings section 58).  The ISP firmware and the D-PHY are set up for
 * 1120 Mbps, and the receiver sees corrupt packet headers.  After a successful s_stream(1):
 *   sensor_pll=1  read the live PLL/MIPI registers back over raw I2C and log them (no writes)
 *   sensor_pll=2  read back, then in software standby (0x0100=0) write the Windows PLL values, stream on again
 *                 (0x0100=1), and read back again
 *   sensor_pll=3  (patch 0033) as 2, plus the other mode registers where the Windows table differs from the
 *                 Linux ov13b10 one (findings section 59): above all 0x3813=0x08 (vertical window offset; Linux
 *                 0x09 shifts the Bayer pattern by one row, suspected cause of the magenta cast, section 88)
 */
static unsigned int sensor_pll;	/* patch 0036: default 0, ov13b10 applies the GZ302 values itself (gz302_fixups) */
module_param(sensor_pll, uint, 0444);
MODULE_PARM_DESC(sensor_pll,
		 "EXPERIMENT: after ov13b10 s_stream: 1 log the PLL registers, 2 also apply the Windows (24 MHz) PLL values, 3 also the other Windows mode registers (0x3813=08 ...)");

/*
 * EXPERIMENT (GZ302, patch 0028): firmware commands that the Windows driver sends for a working preview and the
 * Linux driver never sends (Windows ETW trace, findings section 71; payloads read from amdcamera.sys, findings 72).
 * win_cmds is a bit mask, 0 (default) = behave as before:
 *   0x01 ENABLE_RTA                      {1}                      right after the memory pool
 *   0x02 APPLY_TUNING_SCENARIO           {0 = SCENARIO_MODE_PREVIEW}  after SET_STREAM_CONFIG
 *   0x04 SET_OUT_CHAN_FRAME_RATE_RATIO   {channel 0 = preview, ratio 1}  after SET_OUT_CHAN_PROP
 *   0x08 SET_SCENE_MODE                  {0 = SCENE_MODE_DEFAULT}     after ENABLE_OUT_CHAN
 *   0x10 SET_FPS_RANGE                   {15000, 30000}           after the META buffers, before the preview buffers
 *   0x20 SET_ITIME_RANGE                 {40, 66666} (us)         right after SET_FPS_RANGE
 *   0x40 ENABLE_PREFETCH (GLOBAL, patch 0029) {1, 1, 1}  (INFERRED payload)  after the META buffers, before SET_FPS_RANGE
 *   0x80 SET_3A_CONFIG (patch 0038, 92 bytes, indirect: isp4if_send_3a_config(); AF mode from af_mode)
 *        right after the META buffers, before ENABLE_PREFETCH
 *  0x100 SET_ADAPTIVE_RTA (patch 0041) {0, 3, 3, 3, bytes 01 01 01 00}  right after START_STREAM succeeds
 *        (Windows' registry defaults MemProfiling / adaptiveRtaType 3 / adaptiveRtaStrength 1,1,1; findings 106)
 *  0x200 SET_LOG_MODULE_LEVEL (patch 0044, GLOBAL, indirect: isp4if_send_log_levels(); fw_log_mod, fw_log_rta)
 *        right after the memory pool, before ENABLE_RTA
 * Not included (payload unknown or does not fit the 48-byte command slot): power/clock gating
 * (global commands), SET_M2M_CAL_DATA, SET_ADAPTIVE_RTA.
 */
#define EXP_CMD_ID_ENABLE_RTA			0x02090008
#define EXP_CMD_ID_APPLY_TUNING_SCENARIO	0x02050009
#define EXP_CMD_ID_SET_OUT_CHAN_FRAME_RATE_RATIO 0x02010004
#define EXP_CMD_ID_SET_SCENE_MODE		0x02020007
#define EXP_CMD_ID_SET_FPS_RANGE		0x02020008
#define EXP_CMD_ID_SET_ITIME_RANGE		0x02020009
#define EXP_CMD_ID_ENABLE_PREFETCH		0x01030003	/* type 0x01 = global: goes to the global ring */

#define EXP_WIN_RTA		0x01
#define EXP_WIN_TUNING_SCENARIO	0x02
#define EXP_WIN_RATE_RATIO	0x04
#define EXP_WIN_SCENE_MODE	0x08
#define EXP_WIN_FPS_RANGE	0x10
#define EXP_WIN_ITIME_RANGE	0x20
#define EXP_WIN_PREFETCH	0x40
#define EXP_WIN_3A_CONFIG	0x80	/* patch 0038 */
#define EXP_WIN_ADAPTIVE_RTA	0x100	/* patch 0041 */
#define EXP_WIN_LOG_LEVEL	0x200	/* patch 0044 */
#define EXP_CMD_ID_SET_ADAPTIVE_RTA		0x0201000c

static unsigned int win_cmds = 0x77;	/* patch 0035: all but scene mode */
module_param(win_cmds, uint, 0444);
MODULE_PARM_DESC(win_cmds,
		 "EXPERIMENT: bit mask of extra Windows firmware commands: 1 RTA, 2 tuning scenario, 4 out-chan rate ratio, 8 scene mode, 0x10 fps range, 0x20 itime range, 0x40 global ENABLE_PREFETCH, 0x80 SET_3A_CONFIG, 0x100 SET_ADAPTIVE_RTA, 0x200 SET_LOG_MODULE_LEVEL (0 = none)");

/*
 * EXPERIMENT (GZ302, patch 0030): with {1,1,1} the firmware hangs in MMhubPrefetchSingleStream during
 * START_STREAM (findings section 80).  prefetch_flags picks the three ENABLE_PREFETCH payload words one bit
 * each (bit 0 -> word 0, bit 1 -> word 1, bit 2 -> word 2) so the switch that causes the hang can be found.
 * The firmware logs its reading as "RTPipe MMHub Prefetch", "SoftRTPipe MMHub Prefetch", "Add YUV GAP".
 * Default 7 = {1,1,1}, the patch 0029 behaviour.  Only used when win_cmds has bit 0x40.
 */
static unsigned int prefetch_flags = 1;	/* patch 0035: RT-pipe prefetch only */
module_param(prefetch_flags, uint, 0444);
MODULE_PARM_DESC(prefetch_flags,
		 "EXPERIMENT: ENABLE_PREFETCH payload, bit N -> word N (0..2), default 7 = {1,1,1}; needs win_cmds bit 0x40");

/* Send one of the Windows-only commands if its bit is set; never fails the bring-up, only logs the result. */
static void isp4sd_exp_win_cmd(struct isp4_subdev *isp_subdev, unsigned int bit, u32 cmd_id,
			       const char *name, const u32 *words, u32 count)
{
	int ret;

	if (!(win_cmds & bit))
		return;

	ret = isp4if_send_command(&isp_subdev->ispif, cmd_id, words, count * sizeof(u32));
	exp_info(isp_subdev->dev, "EXP-WINCMD: %s (0x%08x) %u payload words %*ph -> ret %d\n",
		 name, cmd_id, count, (int)(count * sizeof(u32)), words, ret);
}

/* patch 0041: what Windows sends right after a successful START_STREAM (fw_if_set_adaptive_rta) */
static void isp4sd_exp_after_start_stream(struct isp4_subdev *isp_subdev)
{
	isp4sd_exp_win_cmd(isp_subdev, EXP_WIN_ADAPTIVE_RTA, EXP_CMD_ID_SET_ADAPTIVE_RTA,
			   "SET_ADAPTIVE_RTA", (const u32 []){ 0, 3, 3, 3, 0x00010101 }, 5);
}

static struct device *exp_sensor_dev;		/* reference held while the sensor streams */
static struct v4l2_subdev *exp_sensor_sd;

/*
 * The ov13b10 driver turns every I2C failure into -EIO (findings section 34).  Do the two
 * transfers itself, directly on the adapter, and log the real return values:
 *   - read of the 24-bit chip ID at register 0x300a (expect 56 0d 42)
 *   - write of 0x00 to register 0x0100 (MODE_SELECT = standby: harmless)
 * i2c_transfer() returns the number of messages done, or a negative errno (-ENXIO no
 * acknowledge, -ETIMEDOUT bus timeout, -EAGAIN arbitration lost, -EREMOTEIO ...).
 */
static void isp4sd_exp_i2c_probe(struct device *dev, struct i2c_client *client, const char *when)
{
	u8 rd_reg[2] = { 0x30, 0x0a };
	u8 id[3] = { 0, 0, 0 };
	u8 wr_buf[3] = { 0x01, 0x00, 0x00 };
	struct i2c_msg rd[2] = {
		{ .addr = client->addr, .flags = 0, .len = sizeof(rd_reg), .buf = rd_reg },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = sizeof(id), .buf = id },
	};
	struct i2c_msg wr = { .addr = client->addr, .flags = 0, .len = sizeof(wr_buf), .buf = wr_buf };
	int r_rd, r_wr;

	r_rd = i2c_transfer(client->adapter, rd, 2);
	r_wr = i2c_transfer(client->adapter, &wr, 1);

	exp_info(dev,
		 "EXP-SENSOR: raw I2C (%s) adapter %s addr 0x%02x: chip-id read ret %d (id %02x %02x %02x, want 56 0d 42), write 0x0100=0 ret %d, sensor runtime-active %d\n",
		 when, client->adapter->name, client->addr, r_rd, id[0], id[1], id[2], r_wr,
		 pm_runtime_active(&client->dev));
}

/* patch 0025: one 8-bit register of the sensor over raw I2C (16-bit big-endian register address) */
static int isp4sd_exp_rd8(struct i2c_client *client, u16 reg, u8 *val)
{
	u8 addr[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msg[2] = {
		{ .addr = client->addr, .flags = 0, .len = sizeof(addr), .buf = addr },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	int ret = i2c_transfer(client->adapter, msg, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static int isp4sd_exp_wr8(struct i2c_client *client, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	struct i2c_msg msg = { .addr = client->addr, .flags = 0, .len = sizeof(buf), .buf = buf };
	int ret = i2c_transfer(client->adapter, &msg, 1);

	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

/*
 * EXPERIMENT (GZ302, patch 0040): focus motor (VCM) init. Windows' sensor plug-in (start_OV13B ->
 * 0x140004d48) writes two 2-byte frames to the motor at I2C 0x0c at every stream start, with the sensor
 * powered and before the mode table: DF 5B, then 19 04 (VcmMode 1, the Windows default; = park the lens
 * at position 400 with the slope bit). Linux never did, and the lens never moved (findings 105, 107).
 *   vcm_init=0 off (default), 1 = Windows (DF 5B, 19 04), 2 = mode-0 variant (DF 5B, 19 00).
 */
#define EXP_VCM_I2C_ADDR	0x0c

static unsigned int vcm_init;
module_param(vcm_init, uint, 0444);
MODULE_PARM_DESC(vcm_init,
		 "EXPERIMENT: init the focus motor at I2C 0x0c like Windows at stream start: 0 off, 1 DF 5B + 19 04 (Windows), 2 DF 5B + 19 00");

/* one 2-byte frame to the motor (8-bit register, 8-bit value), on the sensor's adapter */
static int isp4sd_exp_vcm_wr(struct i2c_client *client, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msg = { .addr = EXP_VCM_I2C_ADDR, .flags = 0, .len = sizeof(buf), .buf = buf };
	int ret = i2c_transfer(client->adapter, &msg, 1);

	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

/* Caller makes sure the sensor (and so the motor supply) is powered. -ENXIO = the motor did not answer. */
static void isp4sd_exp_vcm_init(struct device *dev, struct i2c_client *client, const char *when)
{
	u8 park = vcm_init == 2 ? 0x00 : 0x04;
	int r1, r2;

	if (!vcm_init)
		return;
	r1 = isp4sd_exp_vcm_wr(client, 0xdf, 0x5b);
	r2 = isp4sd_exp_vcm_wr(client, 0x19, park);
	exp_info(dev, "EXP-VCM: init at I2C 0x%02x %s (vcm_init %u): DF 5B -> %d, 19 %02X -> %d\n",
		 EXP_VCM_I2C_ADDR, when, vcm_init, r1, park, r2);
}

/*
 * EXPERIMENT (GZ302, patch 0042): move the lens from the host, bypassing the firmware, to find out
 * whether the motor itself works (findings 109). Right after s_stream(1) succeeds the host writes one
 * position frame in the format of the Windows init (mode 1): {pos >> 4 & 0x3f, (pos & 0xf) << 4 | 0x04},
 * e.g. 400 -> 19 04, 1023 -> 3F F4, 0 -> 00 04. -1 (default) = off.
 */
static int vcm_test_pos = -1;
module_param(vcm_test_pos, int, 0444);
MODULE_PARM_DESC(vcm_test_pos,
		 "EXPERIMENT: after s_stream(1), write this lens position 0..1023 to the motor at I2C 0x0c from the host (mode-1 frame); -1 = off");

static void isp4sd_exp_vcm_test(struct device *dev, struct i2c_client *client)
{
	unsigned int pos;
	u8 b1, b2;
	int ret;

	if (vcm_test_pos < 0)
		return;
	pos = min(vcm_test_pos, 1023);
	b1 = (pos >> 4) & 0x3f;
	b2 = ((pos & 0xf) << 4) | 0x04;
	ret = isp4sd_exp_vcm_wr(client, b1, b2);
	exp_info(dev, "EXP-VCM: host test write, position %u: %02X %02X -> %d\n", pos, b1, b2, ret);
}

/*
 * EXPERIMENT (GZ302, patch 0043): host-driven manual focus behind V4L2_CID_FOCUS_ABSOLUTE.
 * At every sensor start: Windows' motor init (DF 5B) and then the current position as a mode-1 frame
 * (with the default 400 that is 19 04, i.e. exactly what Windows sends). While the sensor streams, a
 * control change is written to the motor at once. exp_focus_lock serialises control changes against
 * sensor start/stop; exp_focus_client is only set while the sensor is powered and streaming.
 */
static bool focus_control = true;
module_param(focus_control, bool, 0444);
MODULE_PARM_DESC(focus_control,
		 "EXPERIMENT: manual focus control (V4L2 focus_absolute 0..1023) written to the motor by the host; replaces vcm_init (default on)");

/*
 * patch 0045: while the firmware streams, the host must not use the shared I2C controller (it
 * corrupts the sensor, findings 114); live focus changes then go through the firmware with
 * SEND_I2C_MSG. 0 = old direct host write (unsafe while streaming, for comparison only).
 */
static bool focus_via_fw = true;
module_param(focus_via_fw, bool, 0444);
MODULE_PARM_DESC(focus_via_fw,
		 "EXPERIMENT: live focus changes while streaming go through the firmware (SEND_I2C_MSG) instead of the host I2C controller (default on)");

static DEFINE_MUTEX(exp_focus_lock);
static u32 exp_focus_pos = ISP4SD_FOCUS_DEFAULT;
static struct i2c_client *exp_focus_client;
static struct isp4_interface *exp_focus_ispif;	/* patch 0045 */

static int isp4sd_exp_focus_write(struct i2c_client *client, u32 pos)
{
	return isp4sd_exp_vcm_wr(client, (pos >> 4) & 0x3f, ((pos & 0xf) << 4) | 0x04);
}

bool isp4sd_focus_control_enabled(void)
{
	return focus_control;
}

void isp4sd_set_focus(s32 pos)
{
	int ret;

	mutex_lock(&exp_focus_lock);
	exp_focus_pos = clamp(pos, 0, ISP4SD_FOCUS_MAX);
	if (exp_focus_client && focus_via_fw && exp_focus_ispif) {
		/* patch 0045: mode-1 frame {pos >> 4 & 0x3f, (pos & 0xf) << 4 | 4} as register + data */
		ret = isp4if_fw_i2c_write(exp_focus_ispif, EXP_VCM_I2C_ADDR, (exp_focus_pos >> 4) & 0x3f,
					  ((exp_focus_pos & 0xf) << 4) | 0x04);
		if (ret)
			dev_warn(&exp_focus_client->dev, "EXP-FOCUS: firmware I2C write of position %u failed: %d\n",
				 exp_focus_pos, ret);
		else
			dev_dbg(&exp_focus_client->dev, "EXP-FOCUS: position %u via firmware\n", exp_focus_pos);
	} else if (exp_focus_client) {
		ret = isp4sd_exp_focus_write(exp_focus_client, exp_focus_pos);
		dev_dbg(&exp_focus_client->dev, "EXP-FOCUS: position %u -> %d\n", exp_focus_pos, ret);
	}
	mutex_unlock(&exp_focus_lock);
}

/* Sensor powered (caller's responsibility): motor init + current position; else the old vcm_init. */
static void isp4sd_exp_focus_start(struct device *dev, struct i2c_client *client, const char *when)
{
	int r1, r2;

	if (!focus_control) {
		isp4sd_exp_vcm_init(dev, client, when);
		return;
	}
	mutex_lock(&exp_focus_lock);
	r1 = isp4sd_exp_vcm_wr(client, 0xdf, 0x5b);
	r2 = isp4sd_exp_focus_write(client, exp_focus_pos);
	mutex_unlock(&exp_focus_lock);
	exp_info(dev, "EXP-FOCUS: motor init %s: DF 5B -> %d, position %u -> %d\n",
		 when, r1, exp_focus_pos, r2);
}

/* from now on control changes go straight to the motor (NULL = sensor stopped) */
static void isp4sd_exp_focus_attach(struct i2c_client *client, struct isp4_interface *ispif)
{
	mutex_lock(&exp_focus_lock);
	exp_focus_client = focus_control ? client : NULL;
	exp_focus_ispif = exp_focus_client ? ispif : NULL;	/* patch 0045 */
	mutex_unlock(&exp_focus_lock);
}

/* PLL, stream and MIPI timing registers that differ or matter between the Windows and Linux tables */
static const u16 exp_pll_regs[] = {
	0x0100, 0x0303, 0x0305, 0x0321, 0x0323, 0x0324, 0x0325, 0x0326, 0x0327, 0x3011, 0x4800, 0x4837,
	0x3600, 0x380f, 0x3813, 0x3774, 0x3823, 0x3827,	/* patch 0033: mode registers of sensor_pll=3 */
};

static void isp4sd_exp_pll_dump(struct device *dev, struct i2c_client *client, const char *when)
{
	char line[256];
	int n = 0;

	for (unsigned int i = 0; i < ARRAY_SIZE(exp_pll_regs); i++) {
		u8 v = 0;
		int r = isp4sd_exp_rd8(client, exp_pll_regs[i], &v);

		if (r)
			n += scnprintf(line + n, sizeof(line) - n, " 0x%04x=ERR%d", exp_pll_regs[i], r);
		else
			n += scnprintf(line + n, sizeof(line) - n, " 0x%04x=%02x", exp_pll_regs[i], v);
	}
	exp_info(dev, "EXP-SENSOR: PLL regs (%s):%s\n", when, line);
}

/*
 * Windows PLL values (findings section 3 table, section 58).  Software standby first: an OmniVision sensor
 * should not change its PLL while streaming.  Returns the first I2C error or 0.
 */
static int isp4sd_exp_pll_apply_windows(struct device *dev, struct i2c_client *client)
{
	static const struct { u16 reg; u8 val; } seq[] = {
		{ 0x0100, 0x00 },	/* software standby */
		{ 0x0303, 0x01 },
		{ 0x0305, 0x46 },
		{ 0x0325, 0x55 },
	};
	int ret;

	for (unsigned int i = 0; i < ARRAY_SIZE(seq); i++) {
		ret = isp4sd_exp_wr8(client, seq[i].reg, seq[i].val);
		if (ret) {
			dev_err(dev, "EXP-SENSOR: PLL write 0x%04x=0x%02x failed: %d\n", seq[i].reg, seq[i].val, ret);
			return ret;
		}
		if (i == 0)
			usleep_range(5000, 6000);	/* let the current frame end before touching the PLL */
	}
	if (sensor_pll >= 3) {
		/*
		 * patch 0033: Windows mode values that differ from the Linux table (section 59), still in standby.
		 * Exposure/gain registers (0x3501/0x3502/0x3508/0x3509) are left out: the firmware's AE writes them.
		 */
		static const struct { u16 reg; u8 val; } mode[] = {
			{ 0x3813, 0x08 },	/* vertical window offset (Linux 0x09): Bayer phase */
			{ 0x3600, 0x1a },	/* analog (Linux 0x16) */
			{ 0x380f, 0x94 },	/* frame length low byte, 0x0c94 = 3220 lines (Linux 0x7c) */
			{ 0x3011, 0x7c },	/* only in the Windows table */
			{ 0x3774, 0x2d },	/* only in the Windows table */
			{ 0x3823, 0x18 },	/* only in the Windows table */
			{ 0x3827, 0x01 },	/* only in the Windows table */
		};

		for (unsigned int i = 0; i < ARRAY_SIZE(mode); i++) {
			ret = isp4sd_exp_wr8(client, mode[i].reg, mode[i].val);
			if (ret) {
				dev_err(dev, "EXP-SENSOR: mode write 0x%04x=0x%02x failed: %d\n",
					mode[i].reg, mode[i].val, ret);
				return ret;
			}
		}
		exp_info(dev, "EXP-SENSOR: applied the Windows mode registers (0x3813=08 0x3600=1a 0x380f=94 0x3011=7c 0x3774=2d 0x3823=18 0x3827=01)\n");
	}
	usleep_range(5000, 6000);		/* PLL lock */
	ret = isp4sd_exp_wr8(client, 0x0100, 0x01);	/* stream on */
	exp_info(dev, "EXP-SENSOR: applied the Windows PLL values (0x0303=01 0x0305=46 0x0325=55), stream on -> %d\n", ret);
	return ret;
}

/*
 * EXPERIMENT (GZ302, patch 0026): safety limit for live tests.  With the Windows PLL the CSI-2 data decodes, the
 * ISP's MIPI write DMA overflows (INTR_LATENCY_BUFFER_LEAKING, ~110 ms after START_STREAM) and ~2 s later the ISP
 * firmware and the SMU hang, freezing the machine (findings sections 59/60).  With sensor_burst_ms=N the sensor is
 * put into software standby (0x0100=0, raw I2C) N ms after it started streaming, so the data flood ends early.
 * Use it with start_sensor=1 (sensor started after START_STREAM is queued): with start_sensor=2 and phy_init the
 * sensor starts ~1.1 s before START_STREAM and the burst would end before the pipeline runs.
 */
static unsigned int sensor_burst_ms;
module_param(sensor_burst_ms, uint, 0444);
MODULE_PARM_DESC(sensor_burst_ms,
		 "EXPERIMENT: put the sensor into software standby N ms after it started streaming (0 = off; use with start_sensor=1)");

static struct i2c_client *exp_burst_client;	/* sensor to stop, set while the work is armed */
static struct device *exp_burst_log_dev;

static void isp4sd_exp_burst_fn(struct work_struct *work)
{
	int ret = exp_burst_client ? isp4sd_exp_wr8(exp_burst_client, 0x0100, 0x00) : -ENODEV;

	if (exp_burst_log_dev)
		exp_info(exp_burst_log_dev, "EXP-SENSOR: burst limit: sensor put into software standby after %u ms -> %d\n",
			 sensor_burst_ms, ret);
}

static DECLARE_DELAYED_WORK(exp_burst_work, isp4sd_exp_burst_fn);

/*
 * EXPERIMENT (GZ302, patch 0034, tuning step 4): the D-PHY diagnostics of patch 0022 sleep 1.1 s inside
 * the buffer-send path (start_streaming), during which the host cannot queue buffers and frames are
 * dropped ("Preview buffer is not available").  phy_diag=0 skips them.  Default 1 = as before.
 */
static bool phy_diag;	/* patch 0035: default off (no sleeps in the send path) */
module_param(phy_diag, bool, 0444);
MODULE_PARM_DESC(phy_diag, "EXPERIMENT: D-PHY state dumps (+100 ms, +1 s) after the sensor starts; 0 skips the 1.1 s of sleeps");

static void isp4sd_exp_sensor_start(struct device *dev, struct isp4_interface *ispif, const char *when)
{
	struct device *sdev;
	struct i2c_client *client;
	struct v4l2_subdev *sd;
	int ret;
	int pm_ref = -1;

	if (!start_sensor || exp_sensor_dev)
		return;

	sdev = bus_find_device_by_name(&i2c_bus_type, NULL, EXP_SENSOR_I2C_NAME);
	if (!sdev) {
		dev_err(dev, "EXP-SENSOR: no I2C device %s\n", EXP_SENSOR_I2C_NAME);
		return;
	}

	client = i2c_verify_client(sdev);
	sd = client ? i2c_get_clientdata(client) : NULL;
	if (!sd) {
		dev_err(dev, "EXP-SENSOR: %s has no subdev in its client data (driver not bound?)\n",
			EXP_SENSOR_I2C_NAME);
		put_device(sdev);
		return;
	}

	if (sensor_pm_cycle) {
		int r_susp, r_res;

		isp4sd_exp_i2c_probe(dev, client, "before pm cycle");
		r_susp = pm_runtime_force_suspend(&client->dev);
		r_res = pm_runtime_force_resume(&client->dev);
		exp_info(dev, "EXP-SENSOR: forced runtime PM cycle of the sensor: suspend %d, resume %d, runtime-active %d\n",
			 r_susp, r_res, pm_runtime_active(&client->dev));
		isp4sd_exp_i2c_probe(dev, client, "after pm cycle");
	}

	isp4sd_exp_i2c_probe(dev, client, "before s_stream");

	/*
	 * Windows starts the D-PHY receiver with the sensor powered and before the sensor streams.  Hold a
	 * runtime-PM reference on the sensor across the PHY start and s_stream, so it is not powered down in between.
	 */
	if (isp4if_phy_init_enabled()) {
		pm_ref = pm_runtime_resume_and_get(&client->dev);
		exp_info(dev, "EXP-SENSOR: powered the sensor for the PHY start: %d\n", pm_ref);
		ret = isp4if_phy_init(ispif);
		exp_info(dev, "EXP-SENSOR: PHY start returned %d\n", ret);
	}

	/* patch 0040: like Windows, with the sensor powered and before it streams (needs the PM reference) */
	if (pm_ref >= 0)
		isp4sd_exp_focus_start(dev, client, "before s_stream");	/* patch 0043 */

	ret = v4l2_subdev_call(sd, video, s_stream, 1);
	exp_info(dev, "EXP-SENSOR: %s s_stream(1) %s START_STREAM -> %d\n", sd->name, when, ret);
	/* patch 0040: without the PHY start's PM reference, init the motor once the sensor is powered by s_stream */
	if (!ret && pm_ref < 0)
		isp4sd_exp_focus_start(dev, client, "after s_stream");	/* patch 0043 */
	if (!ret)
		isp4sd_exp_vcm_test(dev, client);	/* patch 0042 */
	if (!ret)
		isp4sd_exp_focus_attach(client, ispif);	/* patch 0043, 0045 */
	if (!ret && sensor_pll) {
		/* patch 0025: what the sensor really got, and optionally the Windows PLL values */
		isp4sd_exp_pll_dump(dev, client, "after s_stream");
		if (sensor_pll >= 2 && !isp4sd_exp_pll_apply_windows(dev, client))
			isp4sd_exp_pll_dump(dev, client, "after Windows PLL");
	}
	if (!ret && sensor_burst_ms) {
		/* patch 0026: end the sensor's output after a short burst (runs from a workqueue) */
		exp_burst_client = client;
		exp_burst_log_dev = dev;
		schedule_delayed_work(&exp_burst_work, msecs_to_jiffies(sensor_burst_ms));
		exp_info(dev, "EXP-SENSOR: burst limit armed: sensor standby in %u ms\n", sensor_burst_ms);
	}
	if (!ret && isp4if_phy_init_enabled() && phy_diag) {	/* patch 0034: phy_diag=0 skips the sleeps */
		/* patch 0022: D-PHY state right after, 100 ms and 1 s after the sensor started streaming */
		isp4if_phy_diag(ispif, "after s_stream");
		msleep(100);
		isp4if_phy_diag(ispif, "+100ms");
		msleep(900);
		isp4if_phy_diag(ispif, "+1s");
	}
	if (pm_ref >= 0)
		pm_runtime_put(&client->dev);
	if (ret) {
		isp4sd_exp_i2c_probe(dev, client, "after failed s_stream");
		put_device(sdev);
		return;
	}

	exp_sensor_dev = sdev;
	exp_sensor_sd = sd;
}

static void isp4sd_exp_af_cancel(void);	/* patch 0050, defined with the AF drive below */

static void isp4sd_exp_sensor_stop(struct device *dev)
{
	int ret;

	/* patch 0026: the burst work must not run on a sensor that is being stopped */
	cancel_delayed_work_sync(&exp_burst_work);
	exp_burst_client = NULL;

	if (!exp_sensor_dev)
		return;

	isp4sd_exp_af_cancel();			/* patch 0050: no AF write in flight past this point */
	isp4sd_exp_focus_attach(NULL, NULL);	/* patch 0043: no motor writes once the sensor stops */
	ret = v4l2_subdev_call(exp_sensor_sd, video, s_stream, 0);
	exp_info(dev, "EXP-SENSOR: %s s_stream(0) -> %d\n", exp_sensor_sd->name, ret);
	put_device(exp_sensor_dev);
	exp_sensor_dev = NULL;
	exp_sensor_sd = NULL;
}

#define ISP4SD_PERFORMANCE_STATE_LOW 0
#define ISP4SD_PERFORMANCE_STATE_HIGH 1

/* align 32KB */
#define ISP4SD_META_BUF_SIZE ALIGN(sizeof(struct isp4fw_meta_info), 0x8000)

/*
 * EXPERIMENT (GZ302, patch 0027): the ISP firmware asks the host for four buffers of type 13 every time a stream
 * is set up (BUF_MGR "PopFrontBuffer: BUF|queue empty:type=13"), and the Linux driver never sends any.  type 13 =
 * BUFFER_TYPE_META_DATA in the Windows driver (findings section 65).  Windows (isp_send_metadata_buf) allocates 6
 * buffers of 0x20000 bytes, NOT cleared, and sends them with SEND_BUFFER right before the 6 META_INFO (type 10)
 * buffers, vmid_space space = 4, before START_STREAM.  meta_data_bufs=N sends N of them (0 = off, max 6).
 */
#define EXP_BUFFER_TYPE_META_DATA	13
#define EXP_META_DATA_BUF_SIZE		0x20000
#define EXP_META_DATA_BUF_MAX		6

static unsigned int meta_data_bufs = 6;	/* patch 0035: like Windows */
module_param(meta_data_bufs, uint, 0444);
MODULE_PARM_DESC(meta_data_bufs,
		 "EXPERIMENT: send N (max 6) type-13 META_DATA buffers of 128 KiB before START_STREAM, like Windows (0 = off)");

struct exp_meta_data_buf {
	void *handle;
	u64 gpu_addr;
	void *cpu_addr;
};

static struct exp_meta_data_buf exp_meta_data[EXP_META_DATA_BUF_MAX];
static unsigned int exp_meta_data_cnt;

/*
 * EXPERIMENT (GZ302, patch 0034, tuning step 2): the firmware takes one META_DATA buffer per frame
 * ("LoadFrameMetaData: No meta data buffer is available" once per frame after the first 6) and the
 * Linux driver never gives them back.  Windows re-sends a META_INFO + META_DATA pair after every
 * frame-done (ResendMetaInFrameDone, same index for both).  The frame-done meta info does not say
 * which META_DATA buffer was used, so they are re-sent round-robin in submission order (INFERENCE:
 * the firmware consumes them in order, one per frame).  Needs meta_data_bufs.
 */
static bool meta_data_resend = true;	/* patch 0035: default on */
module_param(meta_data_resend, bool, 0444);
MODULE_PARM_DESC(meta_data_resend,
		 "EXPERIMENT: re-send one type-13 META_DATA buffer (round-robin) after every frame-done, like Windows");

static unsigned int exp_meta_data_next;	/* next META_DATA buffer to re-send */

/*
 * EXPERIMENT (GZ302, patch 0049): the firmware's per-frame AF state, read-only (findings 124/125). Windows'
 * isp_fw_resp_frame_done (amdcamera.sys 0x14001211c) reads, when meta-info word 0x108 is non-zero, a 3A result
 * buffer whose GPU address is the u64 at meta-info byte 0x45c, and logs from it +0x88 (AF state), +0x110 (the
 * value that steps 256 -> 1856 during Windows' AF scan; INFERENCE: lens target 0..4095) and +0x70, +0xa0, +0xa4,
 * +0xbc (float, printed as hex), +0xc0, +0xe8, +0x11c, +0x174 (meaning unknown). INFERENCE: that buffer is one of
 * our META_DATA (type 13) buffers; the log says which one, or "not ours".
 *   af_meta_log=N logs the first N frames of every stream (0 = off, default).
 */
static unsigned int af_meta_log;
module_param(af_meta_log, uint, 0444);
MODULE_PARM_DESC(af_meta_log,
		 "EXPERIMENT: log the firmware's AF state and lens target from the 3A result buffer for the first N frames of each stream (0 = off)");

static unsigned int exp_af_meta_frames;	/* frames logged in this stream */

/* the 3A result buffer this frame's meta info points to, if it is one of ours (index in *idx); else NULL */
static const u8 *isp4sd_exp_3a_buf(const void *meta_info, unsigned int *idx)
{
	const u8 *mi = meta_info;
	u64 addr = get_unaligned_le64(mi + 0x45c);
	unsigned int i;

	for (i = 0; i < exp_meta_data_cnt; i++) {
		if (exp_meta_data[i].gpu_addr == addr) {
			*idx = i;
			return exp_meta_data[i].cpu_addr;
		}
	}
	return NULL;
}

static void isp4sd_exp_af_meta_log(struct device *dev, const void *meta_info)
{
	const u8 *mi = meta_info, *d;
	u32 valid = get_unaligned_le32(mi + 0x108 * 4);
	u64 addr = get_unaligned_le64(mi + 0x45c);
	unsigned int i = 0;

	if (exp_af_meta_frames >= af_meta_log)
		return;
	exp_af_meta_frames++;
	d = isp4sd_exp_3a_buf(meta_info, &i);
	if (!valid || !d) {
		exp_info(dev, "EXP-AFMETA: poc %u: word 0x108 = %u, 3A buffer 0x%llx %s\n",
			 get_unaligned_le32(mi), valid, addr, d ? "" : "(not one of our META_DATA buffers)");
		return;
	}
	exp_info(dev, "EXP-AFMETA: poc %u buf %u: af_state %u lens %u | 70 %u a0 %u a4 %u bc %08x c0 %u e8 %u 11c %u 174 %u\n",
		 get_unaligned_le32(mi), i, get_unaligned_le32(d + 0x88), get_unaligned_le32(d + 0x110),
		 get_unaligned_le32(d + 0x70), get_unaligned_le32(d + 0xa0), get_unaligned_le32(d + 0xa4),
		 get_unaligned_le32(d + 0xbc), get_unaligned_le32(d + 0xc0), get_unaligned_le32(d + 0xe8),
		 get_unaligned_le32(d + 0x11c), get_unaligned_le32(d + 0x174));
}

/*
 * EXPERIMENT (GZ302, patch 0050): close the AF loop on the host (findings 126). The firmware's AF computes a
 * lens target every frame (3A result +0x110, 0..4095) but never writes the motor. af_drive=1 takes that target
 * at frame-done, converts it to the motor scale (/4, 0..1023, as the firmware's own "virtual position(4092) ->
 * hw_position (1023)") and, when it changed, writes it through isp4sd_set_focus() (firmware I2C while
 * streaming, patch 0045). The write is a sync stream command whose answer comes on this same response thread,
 * so it runs from a work item; a newer target replaces one not yet written. Default 0 = off; while on, the AF
 * owns the lens (focus_absolute changes are overwritten by the next target).
 */
static unsigned int af_drive;	/* patch 0052: 0 off, 1 firmware target (0050/0051), 2 host PDAF servo */
module_param(af_drive, uint, 0444);
MODULE_PARM_DESC(af_drive,
		 "EXPERIMENT: drive the focus motor via firmware I2C: 0 off (default), 1 the firmware's AF target, 2 host servo on the firmware's phase-detection defocus (interim fix)");

static int exp_af_pending = -1;		/* motor position to write, -1 = none (xchg) */
static int exp_af_last = -1;		/* last position queued in this stream */

static void isp4sd_exp_af_work_fn(struct work_struct *work)
{
	int pos = xchg(&exp_af_pending, -1);

	if (pos >= 0)
		isp4sd_set_focus(pos);
}

static DECLARE_WORK(exp_af_work, isp4sd_exp_af_work_fn);

/*
 * EXPERIMENT (GZ302, patch 0051): the AF target is on the firmware's algorithm scale (0..4095); patch 0050's /4
 * overdrove the lens (findings 128). Algorithm -> Real: piecewise-linear through the six pairs the AF library
 * prints for this module (findings 128). Real -> motor code: a line through two MEASURED anchors from the lens
 * sweeps (findings 130): Real 940 (infinity) -> af_inf_code, Real 2220 (macro) -> af_macro_code; the firmware's
 * own Real/4 is about 2x too steep toward macro. The code is clamped to [af_inf_code - 50, af_macro_code + 60].
 */
static int af_inf_code = 248;
module_param(af_inf_code, int, 0444);
MODULE_PARM_DESC(af_inf_code, "EXPERIMENT: motor code for infinity focus (firmware Real 940), measured 248");

static int af_macro_code = 407;
module_param(af_macro_code, int, 0444);
MODULE_PARM_DESC(af_macro_code, "EXPERIMENT: motor code for macro focus (firmware Real 2220), from the measured slope 407");

static const u16 exp_af_alg[] = { 0, 755, 1063, 3668, 3805, 4095 };
static const u16 exp_af_real[] = { 700, 860, 940, 2220, 2348, 2620 };

static int isp4sd_exp_af_alg_to_real(u32 alg)
{
	unsigned int i = 1;

	alg = min(alg, 4095U);
	while (i < ARRAY_SIZE(exp_af_alg) - 1 && alg > exp_af_alg[i])
		i++;
	return exp_af_real[i - 1] + (int)(alg - exp_af_alg[i - 1]) * (exp_af_real[i] - exp_af_real[i - 1]) /
	       (exp_af_alg[i] - exp_af_alg[i - 1]);
}

/* the lens's useful range from the two anchors (patch 0051); shared with the servo of patch 0052 */
static int isp4sd_exp_af_clamp(int code)
{
	return clamp(code, max(af_inf_code - 50, 0), min(af_macro_code + 60, ISP4SD_FOCUS_MAX));
}

static int isp4sd_exp_af_real_to_code(int real)
{
	return isp4sd_exp_af_clamp(af_inf_code + (real - 940) * (af_macro_code - af_inf_code) / (2220 - 940));
}

/*
 * EXPERIMENT (GZ302, patch 0052), INTERIM FIX: af_drive=2. On Windows the firmware moves the lens itself; on Linux
 * its move step never runs and it keeps believing the lens has not moved, so its target settles short of focus
 * (findings 132). Its phase-detection defocus (3A result +0xe8, s32) is measured from the image and is right
 * wherever the lens is: positive below focus, negative past it, about -2.6 per motor code near focus. So the host
 * moves the lens itself: every second frame (the lens moves and the next measurement must see it), after the first
 * 8 frames (Windows' AF starts at frame 9; the first readings jump while exposure ramps up),
 *   code += defocus / af_pd_div, limited to +-af_pd_max_step, no move while |defocus| < af_pd_deadband,
 * clamped to the lens range. Starts from the current focus_absolute position. The firmware's AF state stays
 * meaningless (it still thinks the lens never moved); its low-texture fallback is not used.
 */
static unsigned int af_pd_div = 6;
module_param(af_pd_div, uint, 0444);
MODULE_PARM_DESC(af_pd_div, "EXPERIMENT: af_drive=2: lens step = defocus / af_pd_div (default 6, ~40% of the error per step)");

static unsigned int af_pd_max_step = 25;
module_param(af_pd_max_step, uint, 0444);
MODULE_PARM_DESC(af_pd_max_step, "EXPERIMENT: af_drive=2: largest lens step in motor codes (default 25)");

static unsigned int af_pd_deadband = 15;
module_param(af_pd_deadband, uint, 0444);
MODULE_PARM_DESC(af_pd_deadband, "EXPERIMENT: af_drive=2: no move while |defocus| is below this (default 15)");

static unsigned int exp_af_frames;	/* frames seen by the servo in this stream */

/* returns the new motor code, or -1 for "no move this frame" */
static int isp4sd_exp_af_servo(struct device *dev, const void *meta_info, const u8 *d)
{
	s32 defocus = (s32)get_unaligned_le32(d + 0xe8);
	int step;

	if (exp_af_last < 0)
		exp_af_last = READ_ONCE(exp_focus_pos);
	exp_af_frames++;
	if (exp_af_frames <= 8 || (exp_af_frames & 1))
		return -1;
	if (abs(defocus) < af_pd_deadband)
		return -1;
	step = clamp(defocus / (int)max(af_pd_div, 1U), -(int)af_pd_max_step, (int)af_pd_max_step);
	if (!step)
		return -1;
	exp_info(dev, "EXP-AF: poc %u: defocus %d -> code %d -> %d\n", get_unaligned_le32(meta_info), defocus,
		 exp_af_last, isp4sd_exp_af_clamp(exp_af_last + step));
	return isp4sd_exp_af_clamp(exp_af_last + step);
}

static void isp4sd_exp_af_cancel(void)
{
	cancel_work_sync(&exp_af_work);
	exp_af_pending = -1;
}

static void isp4sd_exp_af_drive(struct device *dev, const void *meta_info)
{
	unsigned int idx;
	const u8 *d;
	int real, pos;
	u32 alg;

	if (!af_drive || !get_unaligned_le32((const u8 *)meta_info + 0x108 * 4))
		return;
	d = isp4sd_exp_3a_buf(meta_info, &idx);
	if (!d)
		return;
	if (af_drive == 2) {				/* patch 0052 */
		pos = isp4sd_exp_af_servo(dev, meta_info, d);
		if (pos < 0 || pos == exp_af_last)
			return;
		exp_af_last = pos;
		xchg(&exp_af_pending, pos);
		schedule_work(&exp_af_work);
		return;
	}
	alg = get_unaligned_le32(d + 0x110);
	real = isp4sd_exp_af_alg_to_real(alg);		/* patch 0051 */
	pos = isp4sd_exp_af_real_to_code(real);
	if (pos == exp_af_last)
		return;
	exp_info(dev, "EXP-AF: poc %u: AF state %u, target %u -> real %d -> motor %d\n",
		 get_unaligned_le32(meta_info), get_unaligned_le32(d + 0x88), alg, real, pos);
	exp_af_last = pos;
	xchg(&exp_af_pending, pos);
	schedule_work(&exp_af_work);
}

static void isp4sd_exp_meta_data_free(void)
{
	for (unsigned int i = 0; i < exp_meta_data_cnt; i++)
		isp_kernel_buffer_free(&exp_meta_data[i].handle, &exp_meta_data[i].gpu_addr,
				       &exp_meta_data[i].cpu_addr);
	exp_meta_data_cnt = 0;
}

#define to_isp4_subdev(sd) container_of(sd, struct isp4_subdev, sdev)

static const char *isp4sd_entity_name = "amd isp4";

static const char *isp4sd_thread_name[ISP4SD_MAX_FW_RESP_STREAM_NUM] = {
	"amd_isp4_thread_global",
	"amd_isp4_thread_stream1",
};

static void isp4sd_module_enable(struct isp4_subdev *isp_subdev, bool enable)
{
	if (skip_enable_gpio) {
		dev_info_once(isp_subdev->dev, "EXP: skip_enable_gpio=1, enable_isp GPIO is not driven\n");
		return;
	}

	if (isp_subdev->enable_gpio) {
		gpiod_set_value(isp_subdev->enable_gpio, enable ? 1 : 0);
		dev_dbg(isp_subdev->dev, "%s isp_subdev module\n",
			enable ? "enable" : "disable");
	}
}

static int isp4sd_setup_fw_mem_pool(struct isp4_subdev *isp_subdev)
{
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct isp4fw_cmd_send_buffer buf_type;
	struct device *dev = isp_subdev->dev;
	int ret;

	if (!ispif->fw_mem_pool) {
		dev_err(dev, "fail to alloc mem pool\n");
		return -ENOMEM;
	}

	/*
	 * The struct will be shared with ISP FW, use memset() to guarantee
	 * padding bits are zeroed, since this is not guaranteed on all
	 * compilers.
	 */
	memset(&buf_type, 0, sizeof(buf_type));
	buf_type.buffer_type = ISP4FW_BUFFER_TYPE_MEM_POOL;
	buf_type.buffer.vmid_space.bit.space = ISP4FW_ADDR_SPACE_TYPE_GPU_VA;
	isp4if_split_addr64(ispif->fw_mem_pool->gpu_mc_addr,
			    &buf_type.buffer.buf_base_a_lo,
			    &buf_type.buffer.buf_base_a_hi);
	buf_type.buffer.buf_size_a = ispif->fw_mem_pool->mem_size;

	ret = isp4if_send_command(ispif, ISP4FW_CMD_ID_SEND_BUFFER,
				  &buf_type, sizeof(buf_type));
	if (ret) {
		dev_err(dev, "send fw mem pool 0x%llx(%u) fail %d\n",
			ispif->fw_mem_pool->gpu_mc_addr,
			buf_type.buffer.buf_size_a, ret);
		return ret;
	}

	dev_dbg(dev, "send fw mem pool 0x%llx(%u) suc\n",
		ispif->fw_mem_pool->gpu_mc_addr, buf_type.buffer.buf_size_a);

	return 0;
}

static int isp4sd_set_stream_path(struct isp4_subdev *isp_subdev)
{
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct isp4fw_cmd_set_stream_cfg cmd;
	struct device *dev = isp_subdev->dev;

	/*
	 * The struct will be shared with ISP FW, use memset() to guarantee
	 * padding bits are zeroed, since this is not guaranteed on all
	 * compilers.
	 */
	memset(&cmd, 0, sizeof(cmd));
	cmd.stream_cfg.mipi_pipe_path_cfg.isp4fw_sensor_id =
		ISP4FW_SENSOR_ID_ON_MIPI0;
	cmd.stream_cfg.mipi_pipe_path_cfg.b_enable = true;
	cmd.stream_cfg.isp_pipe_path_cfg.isp_pipe_id =
		ISP4FW_MIPI0_ISP_PIPELINE_ID;

	cmd.stream_cfg.b_enable_tnr = true;
	dev_dbg(dev, "isp4fw_sensor_id %d, pipeId 0x%x EnableTnr %u\n",
		cmd.stream_cfg.mipi_pipe_path_cfg.isp4fw_sensor_id,
		cmd.stream_cfg.isp_pipe_path_cfg.isp_pipe_id,
		cmd.stream_cfg.b_enable_tnr);

	return isp4if_send_command(ispif, ISP4FW_CMD_ID_SET_STREAM_CONFIG,
				   &cmd, sizeof(cmd));
}

static int isp4sd_send_meta_buf(struct isp4_subdev *isp_subdev)
{
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct isp4fw_cmd_send_buffer buf_type;
	struct device *dev = isp_subdev->dev;

	/*
	 * The struct will be shared with ISP FW, use memset() to guarantee
	 * padding bits are zeroed, since this is not guaranteed on all
	 * compilers.
	 */
	memset(&buf_type, 0, sizeof(buf_type));

	/* patch 0027: type-13 META_DATA buffers first, as Windows does (allocated once, kept until module unload) */
	if (meta_data_bufs) {
		unsigned int want = min_t(unsigned int, meta_data_bufs, EXP_META_DATA_BUF_MAX);

		while (exp_meta_data_cnt < want) {
			struct exp_meta_data_buf *b = &exp_meta_data[exp_meta_data_cnt];
			int ret = isp_kernel_buffer_alloc(dev, EXP_META_DATA_BUF_SIZE, &b->handle, &b->gpu_addr,
							  &b->cpu_addr);

			if (ret) {
				dev_err(dev, "EXP: alloc of type-13 buffer %u failed: %d\n", exp_meta_data_cnt, ret);
				return ret;
			}
			exp_meta_data_cnt++;
		}
		for (unsigned int i = 0; i < want; i++) {
			int ret;

			buf_type.buffer_type = EXP_BUFFER_TYPE_META_DATA;
			buf_type.buffer.vmid_space.bit.space = ISP4FW_ADDR_SPACE_TYPE_GPU_VA;
			isp4if_split_addr64(exp_meta_data[i].gpu_addr, &buf_type.buffer.buf_base_a_lo,
					    &buf_type.buffer.buf_base_a_hi);
			buf_type.buffer.buf_size_a = EXP_META_DATA_BUF_SIZE;
			ret = isp4if_send_command(ispif, ISP4FW_CMD_ID_SEND_BUFFER, &buf_type, sizeof(buf_type));
			if (ret) {
				dev_err(dev, "EXP: send type-13 buffer %u failed: %d\n", i, ret);
				return ret;
			}
		}
		exp_info(dev, "EXP: sent %u type-13 META_DATA buffers of 0x%x bytes\n", want, EXP_META_DATA_BUF_SIZE);
		exp_meta_data_next = 0;		/* patch 0034: round-robin re-send starts at the first buffer */
		exp_af_meta_frames = 0;		/* patch 0049: log the first frames of every stream */
		exp_af_last = -1;		/* patch 0050: first AF target of the stream is always written */
		exp_af_frames = 0;		/* patch 0052: servo frame count restarts */
		memset(&buf_type, 0, sizeof(buf_type));
	}

	for (unsigned int i = 0; i < ISP4IF_MAX_STREAM_BUF_COUNT; i++) {
		struct isp4if_gpu_mem_info *meta_info_buf =
				isp_subdev->ispif.meta_info_buf[i];
		int ret;

		if (!meta_info_buf) {
			dev_err(dev, "fail for no meta info buf(%u)\n", i);
			return -ENOMEM;
		}

		buf_type.buffer_type = ISP4FW_BUFFER_TYPE_META_INFO;
		buf_type.buffer.vmid_space.bit.space =
			ISP4FW_ADDR_SPACE_TYPE_GPU_VA;
		isp4if_split_addr64(meta_info_buf->gpu_mc_addr,
				    &buf_type.buffer.buf_base_a_lo,
				    &buf_type.buffer.buf_base_a_hi);
		buf_type.buffer.buf_size_a = meta_info_buf->mem_size;
		ret = isp4if_send_command(ispif, ISP4FW_CMD_ID_SEND_BUFFER,
					  &buf_type, sizeof(buf_type));
		if (ret) {
			dev_err(dev, "send meta info(%u) fail\n", i);
			return ret;
		}
	}

	dev_dbg(dev, "send meta info suc\n");
	return 0;
}

static bool isp4sd_get_str_out_prop(struct isp4_subdev *isp_subdev,
				    struct isp4fw_image_prop *out_prop,
				    struct v4l2_subdev_state *state, u32 pad)
{
	struct device *dev = isp_subdev->dev;
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_state_get_format(state, pad, 0);
	if (!format) {
		dev_err(dev, "fail get subdev state format\n");
		return false;
	}

	switch (format->code) {
	case MEDIA_BUS_FMT_YUYV8_1_5X8:
		out_prop->image_format = ISP4FW_IMAGE_FORMAT_NV12;
		out_prop->width = format->width;
		out_prop->height = format->height;
		out_prop->luma_pitch = format->width;
		out_prop->chroma_pitch = out_prop->width;
		break;
	case MEDIA_BUS_FMT_YUYV8_1X16:
		out_prop->image_format = ISP4FW_IMAGE_FORMAT_YUV422INTERLEAVED;
		out_prop->width = format->width;
		out_prop->height = format->height;
		out_prop->luma_pitch = format->width * 2;
		out_prop->chroma_pitch = 0;
		break;
	default:
		dev_err(dev, "fail for bad image format:0x%x\n",
			format->code);
		return false;
	}

	if (!out_prop->width || !out_prop->height)
		return false;

	return true;
}

static int isp4sd_kickoff_stream(struct isp4_subdev *isp_subdev, u32 w, u32 h)
{
	struct isp4sd_sensor_info *sensor_info = &isp_subdev->sensor_info;
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct device *dev = isp_subdev->dev;

	if (sensor_info->status == ISP4SD_START_STATUS_STARTED)
		return 0;

	if (sensor_info->status == ISP4SD_START_STATUS_START_FAIL) {
		dev_err(dev, "fail for previous start fail\n");
		return -EINVAL;
	}

	dev_dbg(dev, "w:%u,h:%u\n", w, h);

	if (isp4sd_send_meta_buf(isp_subdev)) {
		dev_err(dev, "fail to send meta buf\n");
		sensor_info->status = ISP4SD_START_STATUS_START_FAIL;
		return -EINVAL;
	}

	/* EXPERIMENT (patch 0038): Windows sends its 3A setup (AE/AWB/AF modes) right after the META buffers */
	if (win_cmds & EXP_WIN_3A_CONFIG)
		isp4if_send_3a_config(ispif);

	/* EXPERIMENT (patch 0029): Windows enables MMHUB prefetch (global command) before the fps range */
	isp4sd_exp_win_cmd(isp_subdev, EXP_WIN_PREFETCH, EXP_CMD_ID_ENABLE_PREFETCH, "ENABLE_PREFETCH",
			   (const u32 []){ prefetch_flags & 1, (prefetch_flags >> 1) & 1,
					   (prefetch_flags >> 2) & 1 }, 3);	/* patch 0030 */

	/* EXPERIMENT (patch 0028): Windows sends the fps and integration time ranges after the META buffers */
	isp4sd_exp_win_cmd(isp_subdev, EXP_WIN_FPS_RANGE, EXP_CMD_ID_SET_FPS_RANGE,
			   "SET_FPS_RANGE", (const u32 []){ 15000, 30000 }, 2);
	isp4sd_exp_win_cmd(isp_subdev, EXP_WIN_ITIME_RANGE, EXP_CMD_ID_SET_ITIME_RANGE,
			   "SET_ITIME_RANGE", (const u32 []){ 40, 66666 }, 2);

	sensor_info->status = ISP4SD_START_STATUS_OFF;

	if (!sensor_info->start_stream_cmd_sent &&
	    sensor_info->buf_sent_cnt >= min_start_bufs) {
		int ret;

		if (start_sensor == 2)
			isp4sd_exp_sensor_start(dev, ispif, "before");

		ret = isp4if_send_command(ispif, ISP4FW_CMD_ID_START_STREAM,
					  NULL, 0);
		if (ret) {
			dev_err(dev, "EXP: START_STREAM failed (kickoff), ret %d, buf_sent %u, min %u\n",
				ret, sensor_info->buf_sent_cnt, min_start_bufs);
			return ret;
		}

		exp_info(dev, "EXP: START_STREAM queued to fw (kickoff), buf_sent %u, min %u\n",
			 sensor_info->buf_sent_cnt, min_start_bufs);
		isp4sd_exp_after_start_stream(isp_subdev);	/* patch 0041 */
		isp4if_dump_rings(ispif, "after START_STREAM queued (kickoff)");
		sensor_info->start_stream_cmd_sent = true;
		if (start_sensor == 1)
			isp4sd_exp_sensor_start(dev, ispif, "after");
	} else {
		dev_dbg(dev,
			"no send START_STREAM, start_sent %u, buf_sent %u\n",
			sensor_info->start_stream_cmd_sent,
			sensor_info->buf_sent_cnt);
	}

	return 0;
}

static int isp4sd_setup_output(struct isp4_subdev *isp_subdev,
			       struct v4l2_subdev_state *state, u32 pad)
{
	struct isp4sd_output_info *output_info =
			&isp_subdev->sensor_info.output_info;
	struct isp4sd_sensor_info *sensor_info = &isp_subdev->sensor_info;
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct isp4fw_cmd_set_out_ch_prop cmd_ch_prop;
	struct isp4fw_cmd_enable_out_ch cmd_ch_en;
	struct device *dev = isp_subdev->dev;
	int ret;

	if (output_info->start_status == ISP4SD_START_STATUS_STARTED)
		return 0;

	if (output_info->start_status == ISP4SD_START_STATUS_START_FAIL) {
		dev_err(dev, "fail for previous start fail\n");
		return -EINVAL;
	}

	/*
	 * The struct will be shared with ISP FW, use memset() to guarantee
	 * padding bits are zeroed, since this is not guaranteed on all
	 * compilers.
	 */
	memset(&cmd_ch_prop, 0, sizeof(cmd_ch_prop));
	cmd_ch_prop.ch = ISP4FW_ISP_PIPE_OUT_CH_PREVIEW;

	if (!isp4sd_get_str_out_prop(isp_subdev,
				     &cmd_ch_prop.image_prop, state, pad)) {
		dev_err(dev, "fail to get out prop\n");
		return -EINVAL;
	}

	dev_dbg(dev, "channel:%s,fmt %s,w:h=%u:%u,lp:%u,cp%u\n",
		isp4dbg_get_out_ch_str(cmd_ch_prop.ch),
		isp4dbg_get_img_fmt_str(cmd_ch_prop.image_prop.image_format),
		cmd_ch_prop.image_prop.width, cmd_ch_prop.image_prop.height,
		cmd_ch_prop.image_prop.luma_pitch,
		cmd_ch_prop.image_prop.chroma_pitch);

	ret = isp4if_send_command(ispif, ISP4FW_CMD_ID_SET_OUT_CHAN_PROP,
				  &cmd_ch_prop, sizeof(cmd_ch_prop));
	if (ret) {
		output_info->start_status = ISP4SD_START_STATUS_START_FAIL;
		dev_err(dev, "fail to set out prop\n");
		return ret;
	}

	/* EXPERIMENT (patch 0028): Windows sends the frame rate ratio {channel, 1} between PROP and ENABLE */
	isp4sd_exp_win_cmd(isp_subdev, EXP_WIN_RATE_RATIO, EXP_CMD_ID_SET_OUT_CHAN_FRAME_RATE_RATIO,
			   "SET_OUT_CHAN_FRAME_RATE_RATIO", (const u32 []){ cmd_ch_prop.ch, 1 }, 2);

	/*
	 * The struct will be shared with ISP FW, use memset() to guarantee
	 * padding bits are zeroed, since this is not guaranteed on all
	 * compilers.
	 */
	memset(&cmd_ch_en, 0, sizeof(cmd_ch_en));
	cmd_ch_en.ch = ISP4FW_ISP_PIPE_OUT_CH_PREVIEW;
	cmd_ch_en.is_enable = true;
	ret = isp4if_send_command(ispif, ISP4FW_CMD_ID_ENABLE_OUT_CHAN,
				  &cmd_ch_en, sizeof(cmd_ch_en));
	if (ret) {
		output_info->start_status = ISP4SD_START_STATUS_START_FAIL;
		dev_err(dev, "fail to enable channel\n");
		return ret;
	}

	dev_dbg(dev, "enable channel %s\n",
		isp4dbg_get_out_ch_str(cmd_ch_en.ch));

	/* EXPERIMENT (patch 0028): Windows sets the AE scene mode (0 = DEFAULT) right after ENABLE_OUT_CHAN */
	isp4sd_exp_win_cmd(isp_subdev, EXP_WIN_SCENE_MODE, EXP_CMD_ID_SET_SCENE_MODE,
			   "SET_SCENE_MODE", (const u32 []){ 0 }, 1);

	if (!sensor_info->start_stream_cmd_sent) {
		ret = isp4sd_kickoff_stream(isp_subdev,
					    cmd_ch_prop.image_prop.width,
					    cmd_ch_prop.image_prop.height);
		if (ret) {
			dev_err(dev, "kickoff stream fail %d\n", ret);
			return ret;
		}
		/*
		 * sensor_info->start_stream_cmd_sent will be set to true
		 * 1. in isp4sd_kickoff_stream, if app first send buffer then
		 * start stream
		 * 2. in isp_set_stream_buf, if app first start stream, then
		 * send buffer because ISP FW has the requirement, host needs
		 * to send buffer before send start stream cmd
		 */
		if (sensor_info->start_stream_cmd_sent) {
			sensor_info->status = ISP4SD_START_STATUS_STARTED;
			output_info->start_status = ISP4SD_START_STATUS_STARTED;
			dev_dbg(dev, "kickoff stream suc,start cmd sent\n");
		}
	} else {
		dev_dbg(dev, "stream running, no need kickoff\n");
		output_info->start_status = ISP4SD_START_STATUS_STARTED;
	}

	dev_dbg(dev, "setup output suc\n");
	return 0;
}

/*
 * EXPERIMENT (GZ302, patch 0047): module calibration (M2M) data, step 1: read it. Windows' sensor plug-in
 * (get_m2m_data_strix_OV13B, sensorOV13B.sys 0x1400039c0; findings 120) reads 0x1b8c bytes of the camera
 * module's memory (I2C 0x50, 16-bit offsets; INFERENCE: an EEPROM) in three parts, each CRC-32 checked:
 *   0x00 header (0x20 bytes, CRC at +0x1c), 0x20 header 2 (0x2c bytes, CRC at +0x48: five u16 block sizes at
 *   +0x28/+0x2c/+0x30/+0x34/+0x38, flags at +0x22..+0x24), 0x4c the blocks (each ends in its CRC-32).
 * It caches the result and sends it to the firmware as SET_M2M_CAL_DATA (step 2, not here). We have never
 * sent it; the firmware logs "No valid AF calibration data" and an AWB calibration error.
 *   m2m_read=0 off, 1 read once + check the CRCs (EXP-M2M lines; default since patch 0054), 2 also hex-dump it.
 * The read happens before START_STREAM, while the firmware does not use the I2C bus.
 */
#define EXP_M2M_I2C_ADDR	0x50
#define EXP_M2M_SIZE		0x1b8c
#define EXP_M2M_CHUNK		256

static unsigned int m2m_read = 1;	/* patch 0054: on by default, like Windows (findings 122, 123, 140) */
module_param(m2m_read, uint, 0444);
MODULE_PARM_DESC(m2m_read,
		 "EXPERIMENT: read the camera module calibration (I2C 0x50) before the stream starts: 0 off, 1 read + CRC check (default), 2 also hex dump");

static u8 exp_m2m_buf[EXP_M2M_SIZE];	/* cached after the first read, like Windows */
static bool exp_m2m_done;

/* one read at a 16-bit offset: write the offset (big-endian), then read len bytes */
static int isp4sd_exp_m2m_rd(struct i2c_client *client, u16 off, u8 *buf, u16 len)
{
	u8 addr[2] = { off >> 8, off & 0xff };
	struct i2c_msg msgs[2] = {
		{ .addr = EXP_M2M_I2C_ADDR, .flags = 0, .len = sizeof(addr), .buf = addr },
		{ .addr = EXP_M2M_I2C_ADDR, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));

	return ret == ARRAY_SIZE(msgs) ? 0 : (ret < 0 ? ret : -EIO);
}

/* Windows' checksum: standard CRC-32 (as zlib), over an even number of bytes */
static u32 isp4sd_exp_m2m_crc(const u8 *p, u32 len)
{
	return ~crc32_le(~0U, p, len & ~1U);
}

static void isp4sd_exp_m2m_check(struct device *dev, const u8 *b)
{
	static const u8 size_off[5] = { 0x28, 0x2c, 0x30, 0x34, 0x38 };
	u32 off = 0x4c, total = 0;
	unsigned int i;

	exp_info(dev, "EXP-M2M: header %*ph\n", 0x20, b);
	exp_info(dev, "EXP-M2M: header CRC %08x, stored %08x; header 2 CRC %08x, stored %08x\n",
		 isp4sd_exp_m2m_crc(b, 0x1c), get_unaligned_le32(b + 0x1c),
		 isp4sd_exp_m2m_crc(b + 0x20, 0x28), get_unaligned_le32(b + 0x48));
	for (i = 0; i < ARRAY_SIZE(size_off); i++)
		total += get_unaligned_le16(b + size_off[i]);
	exp_info(dev, "EXP-M2M: flags +0x22..+0x24 = %02x %02x %02x, block sizes %u %u %u %u %u (total %u, max %u)\n",
		 b[0x22], b[0x23], b[0x24],
		 get_unaligned_le16(b + 0x28), get_unaligned_le16(b + 0x2c), get_unaligned_le16(b + 0x30),
		 get_unaligned_le16(b + 0x34), get_unaligned_le16(b + 0x38), total, EXP_M2M_SIZE - 0x4c);
	if (!total || total > EXP_M2M_SIZE - 0x4c)
		return;
	for (i = 0; i < ARRAY_SIZE(size_off); i++) {
		u32 sz = get_unaligned_le16(b + size_off[i]);

		if (sz >= 4)
			exp_info(dev, "EXP-M2M: block %u at 0x%04x, %u bytes: CRC %08x, stored %08x\n", i, off, sz,
				 isp4sd_exp_m2m_crc(b + off, sz - 4), get_unaligned_le32(b + off + sz - 4));
		off += sz;
	}
}

static void isp4sd_exp_m2m_read(struct device *dev)
{
	struct device *sdev;
	struct i2c_client *client;
	int ret = 0, pm;
	u32 off;

	if (!m2m_read || exp_m2m_done)
		return;
	sdev = bus_find_device_by_name(&i2c_bus_type, NULL, EXP_SENSOR_I2C_NAME);
	client = sdev ? i2c_verify_client(sdev) : NULL;
	if (!client) {
		dev_err(dev, "EXP-M2M: no I2C device %s\n", EXP_SENSOR_I2C_NAME);
		goto out;
	}

	pm = pm_runtime_resume_and_get(&client->dev);	/* the module memory shares the sensor's supply */
	for (off = 0; off < EXP_M2M_SIZE && !ret; off += EXP_M2M_CHUNK)
		ret = isp4sd_exp_m2m_rd(client, off, exp_m2m_buf + off, min_t(u32, EXP_M2M_SIZE - off, EXP_M2M_CHUNK));
	if (pm >= 0)
		pm_runtime_put(&client->dev);
	exp_info(dev, "EXP-M2M: read %u bytes from I2C 0x%02x (sensor power ref %d) -> %d\n",
		 EXP_M2M_SIZE, EXP_M2M_I2C_ADDR, pm, ret);
	if (ret)
		goto out;

	exp_m2m_done = true;
	isp4sd_exp_m2m_check(dev, exp_m2m_buf);
	if (m2m_read >= 2)
		print_hex_dump(KERN_INFO, "EXP-M2M-DUMP: ", DUMP_PREFIX_OFFSET, 32, 1,
			       exp_m2m_buf, EXP_M2M_SIZE, false);
out:
	put_device(sdev);
}

/*
 * EXPERIMENT (GZ302, patch 0048): module calibration, step 2: send it. The 0x2180-byte SET_M2M_CAL_DATA
 * payload is built from the EEPROM as Windows' isp_set_m2m_calib_2_fw (amdcamera.sys 0x140018fbc) does
 * (findings 121; checked offline: same checksum as Windows on this laptop, 0x6994265e):
 *   +0x00 sensor id (0); +0x04 EEPROM 0x00..0x8e; +0x94 EEPROM 0x8e..0x98 (2-byte gap at +0x92);
 *   +0xa0 lens shading: packed (flag +0x22 = 1): 0x672 x 3 bytes -> two signed 12-bit values each, stored
 *   as s16 (isp_convert_lsc_to_unpacked), then CRC-32 of +0x84..+0x1a68 at +0x1a68; unpacked: 0x19cc bytes
 *   copied; +0x1a6c AWB calibration (0x2c, flag +0x23); +0x1a98 AF calibration (0x6e6, flag +0x24).
 * m2m_send=1 (default since patch 0054) sends it after the read (needs m2m_read >= 1), before APPLY_TUNING_SCENARIO.
 */
#define EXP_M2M_FW_SIZE		0x2180

static bool m2m_send = true;	/* patch 0054: on by default, like Windows */
module_param(m2m_send, bool, 0444);
MODULE_PARM_DESC(m2m_send,
		 "EXPERIMENT: send the module calibration to the firmware (SET_M2M_CAL_DATA) like Windows; needs m2m_read >= 1 (default on)");

static u8 exp_m2m_fw[EXP_M2M_FW_SIZE];

static void isp4sd_exp_m2m_build(const u8 *cal, u8 *out)
{
	bool packed = cal[0x22] == 1;
	const u8 *p = cal + 0x98;
	unsigned int i;

	memset(out, 0, EXP_M2M_FW_SIZE);
	put_unaligned_le32(0, out);			/* sensor id: 0 for sensor 0 */
	memcpy(out + 0x04, cal, 0x8e);
	memcpy(out + 0x94, cal + 0x8e, 0x0a);
	if (packed) {
		for (i = 0; i < 0x672; i++, p += 3) {
			put_unaligned_le16(sign_extend32((p[0] << 4) | (p[1] >> 4), 11), out + 0xa0 + 4 * i);
			put_unaligned_le16(sign_extend32(((p[1] & 0xf) << 8) | p[2], 11), out + 0xa2 + 4 * i);
		}
		put_unaligned_le32(isp4sd_exp_m2m_crc(out + 0x84, 0x19e4), out + 0x1a68);
	} else {
		memcpy(out + 0xa0, p, 0x19cc);
	}
	if (cal[0x23] == 1)
		memcpy(out + 0x1a6c, cal + (packed ? 0x13f4 : 0x1a64), 0x2c);
	if (cal[0x24] == 1)
		memcpy(out + 0x1a98, cal + (packed ? 0x1420 : 0x1a90), 0x6e6);
}

static void isp4sd_exp_m2m_send(struct isp4_subdev *isp_subdev)
{
	if (!m2m_send)
		return;
	if (!exp_m2m_done) {
		dev_warn(isp_subdev->dev, "EXP-M2M: m2m_send needs a good read (m2m_read=1); not sent\n");
		return;
	}
	isp4sd_exp_m2m_build(exp_m2m_buf, exp_m2m_fw);
	exp_info(isp_subdev->dev, "EXP-M2M: SET_M2M_CAL_DATA payload built, flags %02x %02x %02x (expect checksum 0x6994265e on the tested GZ302)\n",
		 exp_m2m_buf[0x22], exp_m2m_buf[0x23], exp_m2m_buf[0x24]);
	isp4if_send_m2m_cal(&isp_subdev->ispif, exp_m2m_fw, EXP_M2M_FW_SIZE);
}

static int isp4sd_init_stream(struct isp4_subdev *isp_subdev)
{
	struct device *dev = isp_subdev->dev;
	int ret;

	ret = isp4sd_setup_fw_mem_pool(isp_subdev);
	if (ret) {
		dev_err(dev, "fail to setup fw mem pool\n");
		return ret;
	}

	/* EXPERIMENT (patch 0044): Windows sets the firmware log levels first */
	if (win_cmds & EXP_WIN_LOG_LEVEL)
		isp4if_send_log_levels(&isp_subdev->ispif);

	/* EXPERIMENT (patch 0028): Windows sends ENABLE_RTA right after the memory pool */
	isp4sd_exp_win_cmd(isp_subdev, EXP_WIN_RTA, EXP_CMD_ID_ENABLE_RTA, "ENABLE_RTA",
			   (const u32 []){ 1 }, 1);

	/* EXPERIMENT (GZ302): Windows sends the sensor setup here, before the stream config */
	isp4if_replay_sensor_setup(&isp_subdev->ispif);

	ret = isp4sd_set_stream_path(isp_subdev);
	if (ret) {
		dev_err(dev, "fail to setup stream path\n");
		return ret;
	}

	/* EXPERIMENT (patch 0047, 0048): Windows sends SET_M2M_CAL_DATA here: read the EEPROM, then send it */
	isp4sd_exp_m2m_read(dev);
	isp4sd_exp_m2m_send(isp_subdev);

	/* EXPERIMENT (patch 0028): Windows then selects the tuning scenario (0 = preview) */
	isp4sd_exp_win_cmd(isp_subdev, EXP_WIN_TUNING_SCENARIO, EXP_CMD_ID_APPLY_TUNING_SCENARIO,
			   "APPLY_TUNING_SCENARIO", (const u32 []){ 0 }, 1);

	return 0;
}

static void isp4sd_uninit_stream(struct isp4_subdev *isp_subdev,
				 struct v4l2_subdev_state *state, u32 pad)
{
	struct isp4sd_sensor_info *sensor_info = &isp_subdev->sensor_info;
	struct isp4sd_output_info *output_info = &sensor_info->output_info;
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct v4l2_mbus_framefmt *format;

	isp4sd_exp_sensor_stop(isp_subdev->dev);	/* EXPERIMENT: no-op unless start_sensor started it */

	format = v4l2_subdev_state_get_format(state, pad, 0);
	if (!format) {
		dev_err(isp_subdev->dev, "fail to get v4l2 format\n");
	} else {
		memset(format, 0, sizeof(*format));
		format->code = MEDIA_BUS_FMT_YUYV8_1_5X8;
	}

	isp4if_clear_bufq(ispif);
	isp4if_clear_cmdq(ispif);

	sensor_info->start_stream_cmd_sent = false;
	sensor_info->buf_sent_cnt = 0;

	sensor_info->status = ISP4SD_START_STATUS_OFF;
	output_info->start_status = ISP4SD_START_STATUS_OFF;
}

static void isp4sd_fw_resp_cmd_done(struct isp4_subdev *isp_subdev,
				    enum isp4if_stream_id stream_id,
				    struct isp4fw_resp_cmd_done *para)
{
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct isp4if_cmd_element *ele =
		isp4if_rm_cmd_from_cmdq(ispif, para->cmd_seq_num, para->cmd_id);
	struct device *dev = isp_subdev->dev;

	dev_dbg(dev, "stream %d,cmd %s(0x%08x)(%d),seq %u, ele %p\n",
		stream_id,
		isp4dbg_get_cmd_str(para->cmd_id),
		para->cmd_id, para->cmd_status, para->cmd_seq_num,
		ele);

	/* EXPERIMENT (GZ302): always show the firmware's verdict, error code and 36-byte payload */
	exp_info(dev, "EXP-RESP: seq %u cmd 0x%08x status %u (0 ok, 1 fail, 2 skipped) fw_error_code 0x%04x payload %36ph\n",
		 para->cmd_seq_num, para->cmd_id, para->cmd_status,
		 para->isp4fw_error_code, para->payload);

	if (ele) {
		complete(&ele->cmd_done);
		if (refcount_dec_and_test(&ele->refcnt))
			kfree(ele);
	}
}

static struct isp4fw_meta_info *
isp4sd_get_meta_by_mc(struct isp4_subdev *isp_subdev, u64 mc)
{
	for (unsigned int i = 0; i < ISP4IF_MAX_STREAM_BUF_COUNT; i++) {
		struct isp4if_gpu_mem_info *meta_info_buf =
				isp_subdev->ispif.meta_info_buf[i];

		if (meta_info_buf->gpu_mc_addr == mc)
			return meta_info_buf->sys_addr;
	}

	return NULL;
}

static void isp4sd_send_meta_info(struct isp4_subdev *isp_subdev,
				  u64 meta_info_mc)
{
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct isp4fw_cmd_send_buffer buf_type;
	struct device *dev = isp_subdev->dev;

	if (isp_subdev->sensor_info.status != ISP4SD_START_STATUS_STARTED) {
		dev_warn(dev, "not working status %i, meta_info 0x%llx\n",
			 isp_subdev->sensor_info.status, meta_info_mc);
		return;
	}

	/*
	 * The struct will be shared with ISP FW, use memset() to guarantee
	 * padding bits are zeroed, since this is not guaranteed on all
	 * compilers.
	 */
	memset(&buf_type, 0, sizeof(buf_type));
	buf_type.buffer_type = ISP4FW_BUFFER_TYPE_META_INFO;
	buf_type.buffer.vmid_space.bit.space = ISP4FW_ADDR_SPACE_TYPE_GPU_VA;
	isp4if_split_addr64(meta_info_mc,
			    &buf_type.buffer.buf_base_a_lo,
			    &buf_type.buffer.buf_base_a_hi);
	buf_type.buffer.buf_size_a = ISP4SD_META_BUF_SIZE;

	if (isp4if_send_command(ispif, ISP4FW_CMD_ID_SEND_BUFFER,
				&buf_type, sizeof(buf_type)))
		dev_err(dev, "fail send meta_info 0x%llx\n",
			meta_info_mc);
	else
		dev_dbg(dev, "resend meta_info 0x%llx\n", meta_info_mc);
}

static void isp4sd_fw_resp_frame_done(struct isp4_subdev *isp_subdev,
				      enum isp4if_stream_id stream_id,
				      struct isp4fw_resp_param_package *para)
{
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct device *dev = isp_subdev->dev;
	struct isp4if_img_buf_node *prev;
	struct isp4fw_meta_info *meta;
	u64 mc;

	mc = isp4if_join_addr64(para->package_addr_lo, para->package_addr_hi);
	meta = isp4sd_get_meta_by_mc(isp_subdev, mc);
	if (!meta) {
		dev_err(dev, "fail to get meta from mc %llx\n", mc);
		return;
	}
	isp4sd_exp_af_meta_log(dev, meta);	/* patch 0049 */
	isp4sd_exp_af_drive(dev, meta);		/* patch 0050 */

	dev_dbg(dev, "ts:%llu,streamId:%d,poc:%u,preview_en:%u,status:%s(%i)\n",
		ktime_get_ns(), stream_id, meta->poc, meta->preview.enabled,
		isp4dbg_get_buf_done_str(meta->preview.status),
		meta->preview.status);

	if (meta->preview.enabled &&
	    (meta->preview.status == ISP4FW_BUFFER_STATUS_SKIPPED ||
	     meta->preview.status == ISP4FW_BUFFER_STATUS_DONE ||
	     meta->preview.status == ISP4FW_BUFFER_STATUS_DIRTY)) {
		prev = isp4if_dequeue_buffer(ispif);
		if (prev) {
			isp4dbg_show_bufmeta_info(dev, "prev", &meta->preview,
						  &prev->buf_info);
			isp4vid_handle_frame_done(&isp_subdev->isp_vdev,
						  &prev->buf_info);
			isp4if_dealloc_buffer_node(prev);
		} else {
			dev_err(dev, "fail null prev buf\n");
		}
	} else if (meta->preview.enabled) {
		dev_err(dev, "fail bad preview status %u(%s)\n",
			meta->preview.status,
			isp4dbg_get_buf_done_str(meta->preview.status));
	}

	if (isp_subdev->sensor_info.status == ISP4SD_START_STATUS_STARTED)
		isp4sd_send_meta_info(isp_subdev, mc);

	/* patch 0034: give one META_DATA buffer back per frame, like Windows' ResendMetaInFrameDone */
	if (meta_data_resend && exp_meta_data_cnt &&
	    isp_subdev->sensor_info.status == ISP4SD_START_STATUS_STARTED) {
		struct exp_meta_data_buf *b = &exp_meta_data[exp_meta_data_next % exp_meta_data_cnt];
		struct isp4fw_cmd_send_buffer md;

		memset(&md, 0, sizeof(md));
		md.buffer_type = EXP_BUFFER_TYPE_META_DATA;
		md.buffer.vmid_space.bit.space = ISP4FW_ADDR_SPACE_TYPE_GPU_VA;
		isp4if_split_addr64(b->gpu_addr, &md.buffer.buf_base_a_lo, &md.buffer.buf_base_a_hi);
		md.buffer.buf_size_a = EXP_META_DATA_BUF_SIZE;
		if (isp4if_send_command(ispif, ISP4FW_CMD_ID_SEND_BUFFER, &md, sizeof(md)))
			dev_err(dev, "EXP: re-send of META_DATA %u failed\n",
				exp_meta_data_next % exp_meta_data_cnt);
		exp_meta_data_next++;
	}

	dev_dbg(dev, "stream_id:%d, status:%d\n", stream_id,
		isp_subdev->sensor_info.status);
}

static void isp4sd_fw_resp_func(struct isp4_subdev *isp_subdev,
				enum isp4if_stream_id stream_id)
{
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct device *dev = isp_subdev->dev;
	struct isp4fw_resp resp;

	if (stream_id == ISP4IF_STREAM_ID_1)
		isp_fw_log_print(isp_subdev);

	while (true) {
		if (isp4if_f2h_resp(ispif, stream_id, &resp)) {
			/* Re-enable the interrupt */
			isp4_intr_enable(isp_subdev, stream_id, true);
			/*
			 * Recheck to see if there is a new response.
			 * To ensure that an in-flight interrupt is not lost,
			 * enabling the interrupt must occur _before_ checking
			 * for a new response, hence a memory barrier is needed.
			 * Disable the interrupt again if there was a new
			 * response.
			 */
			mb();
			if (likely(isp4if_f2h_resp(ispif, stream_id, &resp)))
				break;

			isp4_intr_enable(isp_subdev, stream_id, false);
		}

		switch (resp.resp_id) {
		case ISP4FW_RESP_ID_CMD_DONE:
			isp4sd_fw_resp_cmd_done(isp_subdev, stream_id,
						&resp.param.cmd_done);
			break;
		case ISP4FW_RESP_ID_NOTI_FRAME_DONE:
			isp4sd_fw_resp_frame_done(isp_subdev, stream_id,
						  &resp.param.frame_done);
			break;
		default:
			dev_err(dev, "-><- fail respid %s(0x%x)\n",
				isp4dbg_get_resp_str(resp.resp_id),
				resp.resp_id);
			break;
		}
	}
}

static s32 isp4sd_fw_resp_thread(void *context)
{
	struct isp4_subdev_thread_param *para = context;
	struct isp4_subdev *isp_subdev = para->isp_subdev;
	struct isp4sd_thread_handler *thread_ctx =
			&isp_subdev->fw_resp_thread[para->idx];
	struct device *dev = isp_subdev->dev;

	dev_dbg(dev, "[%u] fw resp thread started\n", para->idx);
	while (true) {
		wait_event_interruptible(thread_ctx->waitq,
					 thread_ctx->resp_ready);
		thread_ctx->resp_ready = false;

		if (kthread_should_stop()) {
			dev_dbg(dev, "[%u] fw resp thread quit\n", para->idx);
			break;
		}

		isp4sd_fw_resp_func(isp_subdev, para->idx);
	}

	return 0;
}

static int isp4sd_stop_resp_proc_threads(struct isp4_subdev *isp_subdev)
{
	for (unsigned int i = 0; i < ISP4SD_MAX_FW_RESP_STREAM_NUM; i++) {
		struct isp4sd_thread_handler *thread_ctx =
				&isp_subdev->fw_resp_thread[i];

		if (thread_ctx->thread) {
			kthread_stop(thread_ctx->thread);
			thread_ctx->thread = NULL;
		}
	}

	return 0;
}

static int isp4sd_start_resp_proc_threads(struct isp4_subdev *isp_subdev)
{
	struct device *dev = isp_subdev->dev;

	for (unsigned int i = 0; i < ISP4SD_MAX_FW_RESP_STREAM_NUM; i++) {
		struct isp4sd_thread_handler *thread_ctx =
				&isp_subdev->fw_resp_thread[i];

		isp_subdev->isp_resp_para[i].idx = i;
		isp_subdev->isp_resp_para[i].isp_subdev = isp_subdev;
		init_waitqueue_head(&thread_ctx->waitq);
		thread_ctx->resp_ready = false;

		thread_ctx->thread = kthread_run(isp4sd_fw_resp_thread,
						 &isp_subdev->isp_resp_para[i],
						 isp4sd_thread_name[i]);
		if (IS_ERR(thread_ctx->thread)) {
			dev_err(dev, "create thread [%d] fail\n", i);
			thread_ctx->thread = NULL;
			isp4sd_stop_resp_proc_threads(isp_subdev);
			return -EINVAL;
		}
	}

	return 0;
}

int isp4sd_pwroff_and_deinit(struct v4l2_subdev *sd)
{
	struct isp4_subdev *isp_subdev = to_isp4_subdev(sd);
	struct isp4sd_sensor_info *sensor_info = &isp_subdev->sensor_info;
	unsigned int perf_state = ISP4SD_PERFORMANCE_STATE_LOW;
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct device *dev = isp_subdev->dev;
	int ret;

	guard(mutex)(&isp_subdev->ops_mutex);
	if (sensor_info->status == ISP4SD_START_STATUS_STARTED) {
		dev_err(dev, "fail for stream still running\n");
		return -EINVAL;
	}

	sensor_info->status = ISP4SD_START_STATUS_OFF;

	if (isp_subdev->irq_enabled) {
		for (unsigned int i = 0; i < ISP4SD_MAX_FW_RESP_STREAM_NUM; i++)
			disable_irq(isp_subdev->irq[i]);
		isp_subdev->irq_enabled = false;
	}

	isp4sd_stop_resp_proc_threads(isp_subdev);
	dev_dbg(dev, "isp_subdev stop resp proc threads suc\n");

	isp4if_stop(ispif);

	ret = dev_pm_genpd_set_performance_state(dev, perf_state);
	if (ret)
		dev_err(dev,
			"fail to set isp_subdev performance state %u,ret %d\n",
			perf_state, ret);

	/* hold ccpu reset */
	isp4hw_wreg(isp_subdev->mmio, ISP_SOFT_RESET, 0);
	isp4hw_wreg(isp_subdev->mmio, ISP_POWER_STATUS, 0);
	ret = pm_runtime_put_sync(dev);
	if (ret)
		dev_err(dev, "power off isp_subdev fail %d\n", ret);
	else
		dev_dbg(dev, "power off isp_subdev suc\n");

	ispif->status = ISP4IF_STATUS_PWR_OFF;
	isp4if_clear_cmdq(ispif);
	isp4sd_module_enable(isp_subdev, false);

	/*
	 * When opening the camera, isp4sd_module_enable(isp_subdev, true) is
	 * called. Hardware requires at least a 20ms delay between disabling
	 * and enabling the module, so a sleep is added to ensure ISP stability
	 * during quick reopen scenarios.
	 */
	msleep(20);

	return 0;
}

int isp4sd_pwron_and_init(struct v4l2_subdev *sd)
{
	struct isp4_subdev *isp_subdev = to_isp4_subdev(sd);
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct device *dev = isp_subdev->dev;
	int ret;

	guard(mutex)(&isp_subdev->ops_mutex);
	if (ispif->status == ISP4IF_STATUS_FW_RUNNING) {
		dev_dbg(dev, "camera already opened, do nothing\n");
		return 0;
	}

	isp4sd_module_enable(isp_subdev, true);

	if (ispif->status < ISP4IF_STATUS_PWR_ON) {
		unsigned int perf_state = ISP4SD_PERFORMANCE_STATE_HIGH;

		ret = pm_runtime_resume_and_get(dev);
		if (ret) {
			dev_err(dev, "fail to power on isp_subdev ret %d\n",
				ret);
			goto err_module_disable;
		}

		/* ISPPG ISP Power Status */
		isp4hw_wreg(isp_subdev->mmio, ISP_POWER_STATUS, 0x7FF);
		ret = dev_pm_genpd_set_performance_state(dev, perf_state);
		if (ret) {
			dev_err(dev,
				"fail to set performance state %u, ret %d\n",
				perf_state, ret);
			goto err_power_off;
		}

		ispif->status = ISP4IF_STATUS_PWR_ON;
	}

	isp_subdev->sensor_info.start_stream_cmd_sent = false;
	isp_subdev->sensor_info.buf_sent_cnt = 0;

	ret = isp4if_start(ispif);
	if (ret) {
		dev_err(dev, "fail to start isp_subdev interface\n");
		goto err_perf_restore;
	}

	if (isp4sd_start_resp_proc_threads(isp_subdev)) {
		dev_err(dev, "isp_start_resp_proc_threads fail\n");
		goto err_stop_interface;
	}

	dev_dbg(dev, "create resp threads ok\n");

	for (unsigned int i = 0; i < ISP4SD_MAX_FW_RESP_STREAM_NUM; i++)
		enable_irq(isp_subdev->irq[i]);
	isp_subdev->irq_enabled = true;

	return 0;

err_stop_interface:
	isp4if_stop(ispif);
err_perf_restore:
	ret = dev_pm_genpd_set_performance_state(dev, ISP4SD_PERFORMANCE_STATE_LOW);
	if (ret)
		dev_err(dev, "fail to set performance state %u, ret %d\n",
			ISP4SD_PERFORMANCE_STATE_LOW, ret);
err_power_off:
	isp4hw_wreg(isp_subdev->mmio, ISP_SOFT_RESET, 0);
	isp4hw_wreg(isp_subdev->mmio, ISP_POWER_STATUS, 0);
	ret = pm_runtime_put_sync(dev);
	if (ret)
		dev_err(dev, "power off isp_subdev fail %d\n", ret);
	ispif->status = ISP4IF_STATUS_PWR_OFF;
err_module_disable:
	isp4sd_module_enable(isp_subdev, false);
	msleep(20);
	return -EINVAL;
}

static int isp4sd_stop_stream(struct isp4_subdev *isp_subdev,
			      struct v4l2_subdev_state *state, u32 pad)
{
	struct isp4sd_sensor_info *sensor_info = &isp_subdev->sensor_info;
	struct isp4sd_output_info *output_info = &sensor_info->output_info;
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct device *dev = isp_subdev->dev;

	guard(mutex)(&isp_subdev->ops_mutex);
	dev_dbg(dev, "status %i\n", output_info->start_status);

	if (output_info->start_status == ISP4SD_START_STATUS_STARTED) {
		struct isp4fw_cmd_enable_out_ch cmd_ch_disable;
		int ret;

		/*
		 * The struct will be shared with ISP FW, use memset() to
		 * guarantee padding bits are zeroed, since this is not
		 * guaranteed on all compilers.
		 */
		memset(&cmd_ch_disable, 0, sizeof(cmd_ch_disable));
		cmd_ch_disable.ch = ISP4FW_ISP_PIPE_OUT_CH_PREVIEW;
		/* `cmd_ch_disable.is_enable` is already false */
		ret = isp4if_send_command_sync(ispif,
					       ISP4FW_CMD_ID_ENABLE_OUT_CHAN,
					       &cmd_ch_disable,
					       sizeof(cmd_ch_disable));
		if (ret)
			dev_err(dev, "fail to disable stream\n");
		else
			dev_dbg(dev, "wait disable stream suc\n");

		ret = isp4if_send_command_sync(ispif, ISP4FW_CMD_ID_STOP_STREAM,
					       NULL, 0);
		if (ret)
			dev_err(dev, "fail to stop stream\n");
		else
			dev_dbg(dev, "wait stop stream suc\n");
	}

	isp4sd_uninit_stream(isp_subdev, state, pad);

	/*
	 * Return success to ensure the stop process proceeds,
	 * and disregard any errors since they are not fatal.
	 */
	return 0;
}

static int isp4sd_start_stream(struct isp4_subdev *isp_subdev,
			       struct v4l2_subdev_state *state, u32 pad)
{
	struct isp4sd_output_info *output_info =
			&isp_subdev->sensor_info.output_info;
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct device *dev = isp_subdev->dev;
	int ret;

	guard(mutex)(&isp_subdev->ops_mutex);

	if (ispif->status != ISP4IF_STATUS_FW_RUNNING) {
		dev_err(dev, "fail, bad fsm %d\n", ispif->status);
		return -EINVAL;
	}

	switch (output_info->start_status) {
	case ISP4SD_START_STATUS_OFF:
		break;
	case ISP4SD_START_STATUS_STARTED:
		dev_dbg(dev, "stream already started, do nothing\n");
		return 0;
	case ISP4SD_START_STATUS_START_FAIL:
		dev_err(dev, "stream previously failed to start\n");
		return -EINVAL;
	}

	ret = isp4sd_init_stream(isp_subdev);
	if (ret) {
		dev_err(dev, "fail to init isp_subdev stream\n");
		goto err_stop_stream;
	}

	ret = isp4sd_setup_output(isp_subdev, state, pad);
	if (ret) {
		dev_err(dev, "fail to setup output\n");
		goto err_stop_stream;
	}

	return 0;

err_stop_stream:
	isp4sd_stop_stream(isp_subdev, state, pad);
	return ret;
}

int isp4sd_ioc_send_img_buf(struct v4l2_subdev *sd,
			    struct isp4if_img_buf_info *buf_info)
{
	struct isp4_subdev *isp_subdev = to_isp4_subdev(sd);
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct isp4if_img_buf_node *buf_node;
	struct device *dev = isp_subdev->dev;
	int ret;

	guard(mutex)(&isp_subdev->ops_mutex);

	if (ispif->status != ISP4IF_STATUS_FW_RUNNING) {
		dev_err(dev, "fail send img buf for bad fsm %d\n",
			ispif->status);
		return -EINVAL;
	}

	buf_node = isp4if_alloc_buffer_node(buf_info);
	if (!buf_node) {
		dev_err(dev, "fail alloc sys img buf info node\n");
		return -ENOMEM;
	}

	ret = isp4if_queue_buffer(ispif, buf_node);
	if (ret) {
		dev_err(dev, "fail to queue image buf, %d\n", ret);
		goto error_release_buf_node;
	}

	if (!isp_subdev->sensor_info.start_stream_cmd_sent) {
		isp_subdev->sensor_info.buf_sent_cnt++;

		if (isp_subdev->sensor_info.buf_sent_cnt >= min_start_bufs) {
			if (start_sensor == 2)
				isp4sd_exp_sensor_start(dev, ispif, "before");

			ret = isp4if_send_command(ispif,
						  ISP4FW_CMD_ID_START_STREAM,
						  NULL, 0);
			if (ret) {
				dev_err(dev, "EXP: START_STREAM failed (img buf), ret %d, buf_sent %u, min %u\n",
					ret, isp_subdev->sensor_info.buf_sent_cnt,
					min_start_bufs);
				goto error_release_buf_node;
			}
			exp_info(dev, "EXP: START_STREAM queued to fw (img buf), buf_sent %u, min %u\n",
				 isp_subdev->sensor_info.buf_sent_cnt,
				 min_start_bufs);
			isp4sd_exp_after_start_stream(isp_subdev);	/* patch 0041 */
			isp4if_dump_rings(ispif, "after START_STREAM queued (img buf)");
			isp_subdev->sensor_info.start_stream_cmd_sent = true;
			isp_subdev->sensor_info.output_info.start_status =
				ISP4SD_START_STATUS_STARTED;
			isp_subdev->sensor_info.status =
				ISP4SD_START_STATUS_STARTED;
			if (start_sensor == 1)
				isp4sd_exp_sensor_start(dev, ispif, "after");
		} else {
			dev_dbg(dev,
				"no send start, required %u, buf sent %u\n",
				min_start_bufs,
				isp_subdev->sensor_info.buf_sent_cnt);
		}
	}

	return 0;

error_release_buf_node:
	isp4if_dealloc_buffer_node(buf_node);
	return ret;
}

static const struct v4l2_subdev_video_ops isp4sd_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static int isp4sd_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *format)
{
	struct isp4sd_output_info *stream_info =
		&(to_isp4_subdev(sd)->sensor_info.output_info);
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_state_get_format(sd_state, format->pad);

	if (!fmt) {
		dev_err(sd->dev, "fail to get state format\n");
		return -EINVAL;
	}

	*fmt = format->format;
	switch (fmt->code) {
	case MEDIA_BUS_FMT_YUYV8_1X16:
		stream_info->image_size = fmt->width * fmt->height * 2;
		break;
	case MEDIA_BUS_FMT_YUYV8_1_5X8:
	default:
		stream_info->image_size = fmt->width * fmt->height * 3 / 2;
		break;
	}

	if (!stream_info->image_size) {
		dev_err(sd->dev,
			"fail set pad format,code 0x%x,width %u, height %u\n",
			fmt->code, fmt->width, fmt->height);
		return -EINVAL;
	}

	dev_dbg(sd->dev, "set pad format suc, code:%x w:%u h:%u size:%u\n",
		fmt->code, fmt->width, fmt->height,
		stream_info->image_size);

	return 0;
}

static int isp4sd_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct isp4_subdev *isp_subdev = to_isp4_subdev(sd);

	return isp4sd_start_stream(isp_subdev, state, pad);
}

static int isp4sd_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct isp4_subdev *isp_subdev = to_isp4_subdev(sd);

	return isp4sd_stop_stream(isp_subdev, state, pad);
}

static const struct v4l2_subdev_pad_ops isp4sd_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = isp4sd_set_pad_format,
	.enable_streams = isp4sd_enable_streams,
	.disable_streams = isp4sd_disable_streams,
};

static const struct v4l2_subdev_ops isp4sd_subdev_ops = {
	.video = &isp4sd_video_ops,
	.pad = &isp4sd_pad_ops,
};

int isp4sd_init(struct isp4_subdev *isp_subdev, struct v4l2_device *v4l2_dev,
		int irq[ISP4SD_MAX_FW_RESP_STREAM_NUM])
{
	struct isp4sd_sensor_info *sensor_info = &isp_subdev->sensor_info;
	struct isp4_interface *ispif = &isp_subdev->ispif;
	struct device *dev = v4l2_dev->dev;
	int ret;

	isp_subdev->dev = dev;
	v4l2_subdev_init(&isp_subdev->sdev, &isp4sd_subdev_ops);
	isp_subdev->sdev.owner = THIS_MODULE;
	isp_subdev->sdev.dev = dev;
	snprintf(isp_subdev->sdev.name, sizeof(isp_subdev->sdev.name), "%s",
		 dev_name(dev));

	isp_subdev->sdev.entity.name = isp4sd_entity_name;
	isp_subdev->sdev.entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;
	isp_subdev->sdev_pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&isp_subdev->sdev.entity, 1,
				     &isp_subdev->sdev_pad);
	if (ret) {
		dev_err(dev, "fail to init isp4 subdev entity pad %d\n", ret);
		return ret;
	}

	ret = v4l2_subdev_init_finalize(&isp_subdev->sdev);
	if (ret < 0) {
		dev_err(dev, "fail to init finalize isp4 subdev %d\n",
			ret);
		return ret;
	}

	ret = v4l2_device_register_subdev(v4l2_dev, &isp_subdev->sdev);
	if (ret) {
		dev_err(dev, "fail to register isp4 subdev to V4L2 device %d\n",
			ret);
		goto err_media_clean_up;
	}

	isp4if_init(ispif, dev, isp_subdev->mmio);

	mutex_init(&isp_subdev->ops_mutex);
	sensor_info->status = ISP4SD_START_STATUS_OFF;

	/* create ISP enable gpio control; patch 0053: optional (no lookup without the amdgpu board patch) */
	isp_subdev->enable_gpio = devm_gpiod_get_optional(isp_subdev->dev,
							  "enable_isp",
							  GPIOD_OUT_LOW);
	if (IS_ERR(isp_subdev->enable_gpio)) {
		ret = PTR_ERR(isp_subdev->enable_gpio);
		dev_err(dev, "fail to get gpiod %d\n", ret);
		goto err_subdev_unreg;
	}

	for (unsigned int i = 0; i < ISP4SD_MAX_FW_RESP_STREAM_NUM; i++)
		isp_subdev->irq[i] = irq[i];

	isp_subdev->host2fw_seq_num = 1;
	ispif->status = ISP4IF_STATUS_PWR_OFF;

	ret = isp4vid_dev_init(&isp_subdev->isp_vdev, &isp_subdev->sdev);
	if (ret)
		goto err_subdev_unreg;

	return 0;

err_subdev_unreg:
	v4l2_device_unregister_subdev(&isp_subdev->sdev);
err_media_clean_up:
	v4l2_subdev_cleanup(&isp_subdev->sdev);
	media_entity_cleanup(&isp_subdev->sdev.entity);
	return ret;
}

void isp4sd_deinit(struct isp4_subdev *isp_subdev)
{
	struct isp4_interface *ispif = &isp_subdev->ispif;

	/* patch 0026: never leave the burst work pending when the module goes away */
	cancel_delayed_work_sync(&exp_burst_work);
	exp_burst_client = NULL;
	isp4sd_exp_meta_data_free();	/* patch 0027 */

	isp4vid_dev_deinit(&isp_subdev->isp_vdev);
	v4l2_device_unregister_subdev(&isp_subdev->sdev);
	media_entity_cleanup(&isp_subdev->sdev.entity);
	isp4if_deinit(ispif);
	isp4sd_module_enable(isp_subdev, false);

	ispif->status = ISP4IF_STATUS_PWR_OFF;
}
