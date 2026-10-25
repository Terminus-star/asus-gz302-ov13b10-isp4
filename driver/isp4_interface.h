/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#ifndef _ISP4_INTERFACE_H_
#define _ISP4_INTERFACE_H_

#include <drm/amd/isp.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

struct isp4fw_resp;

#define ISP4IF_RB_MAX 25
#define ISP4IF_RESP_CHAN_TO_RB_OFFSET 9
#define ISP4IF_RB_PMBMAP_MEM_SIZE (SZ_16M - 1)
#define ISP4IF_RB_PMBMAP_MEM_CHUNK \
	(ISP4IF_RB_PMBMAP_MEM_SIZE / (ISP4IF_RB_MAX - 1))
#define ISP4IF_HOST2FW_COMMAND_SIZE sizeof(struct isp4fw_cmd)
#define ISP4IF_MAX_NUM_HOST2FW_COMMAND 40
#define ISP4IF_FW_CMD_BUF_SIZE \
	(ISP4IF_MAX_NUM_HOST2FW_COMMAND * ISP4IF_HOST2FW_COMMAND_SIZE)
#define ISP4IF_RB_FULL_SLEEP_US (33 * USEC_PER_MSEC)
#define ISP4IF_RB_FULL_TIMEOUT_US (10 * ISP4IF_RB_FULL_SLEEP_US)

#define ISP4IF_META_INFO_BUF_SIZE ALIGN(sizeof(struct isp4fw_meta_info), 0x8000)
#define ISP4IF_MAX_STREAM_BUF_COUNT 8

#define ISP4IF_FW_LOG_RINGBUF_SIZE SZ_2M

enum isp4if_stream_id {
	ISP4IF_STREAM_ID_GLOBAL = 0,
	ISP4IF_STREAM_ID_1 = 1,
	ISP4IF_STREAM_ID_MAX = 4
};

enum isp4if_status {
	ISP4IF_STATUS_PWR_OFF,
	ISP4IF_STATUS_PWR_ON,
	ISP4IF_STATUS_FW_RUNNING,
	ISP4IF_FSM_STATUS_MAX
};

struct isp4if_gpu_mem_info {
	u64 mem_size;
	u64 gpu_mc_addr;
	void *sys_addr;
	void *mem_handle;
};

struct isp4if_img_buf_info {
	struct {
		void *sys_addr;
		u64 mc_addr;
		u32 len;
	} planes[3];
};

struct isp4if_img_buf_node {
	struct list_head node;
	struct isp4if_img_buf_info buf_info;
};

struct isp4if_cmd_element {
	struct list_head list;
	u32 seq_num;
	u32 cmd_id;
	struct completion cmd_done;
	atomic_t refcnt;
};

struct isp4_interface {
	struct device *dev;
	void __iomem *mmio;

	spinlock_t cmdq_lock; /* used for cmdq access */
	spinlock_t bufq_lock; /* used for bufq access */
	struct mutex isp4if_mutex; /* used to send fw cmd and read fw log */

	struct list_head cmdq; /* commands sent to fw */
	struct list_head bufq; /* buffers sent to fw */

	enum isp4if_status status;
	u32 host2fw_seq_num;

	/* ISP fw buffers */
	struct isp4if_gpu_mem_info *fw_log_buf;
	/* EXPERIMENT (GZ302): firmware-visible copies of the replayed Windows sensor setup */
	struct isp4if_gpu_mem_info *exp_sensor_script;
	struct isp4if_gpu_mem_info *exp_sensor_prop;
	struct isp4if_gpu_mem_info *fw_cmd_resp_buf;
	struct isp4if_gpu_mem_info *fw_mem_pool;
	struct isp4if_gpu_mem_info *meta_info_buf[ISP4IF_MAX_STREAM_BUF_COUNT];
};

static inline void isp4if_split_addr64(u64 addr, u32 *lo, u32 *hi)
{
	if (lo)
		*lo = addr & 0xffffffff;

	if (hi)
		*hi = addr >> 32;
}

static inline u64 isp4if_join_addr64(u32 lo, u32 hi)
{
	return (((u64)hi) << 32) | (u64)lo;
}

int isp4if_f2h_resp(struct isp4_interface *ispif, enum isp4if_stream_id stream,
		    struct isp4fw_resp *resp);

/* EXPERIMENT (GZ302 bring-up): log ring-buffer and interrupt state, see isp4_interface.c */
void isp4if_dump_rings(struct isp4_interface *ispif, const char *when);

/* EXPERIMENT (GZ302 bring-up): host-side MIPI D-PHY receiver start like Windows, no-op unless phy_init=1 */
bool isp4if_phy_init_enabled(void);
int isp4if_phy_init(struct isp4_interface *ispif);
/* EXPERIMENT (patch 0022): read-only D-PHY state snapshot, no-op unless phy_init=1 */
void isp4if_phy_diag(struct isp4_interface *ispif, const char *label);

/* EXPERIMENT (GZ302 bring-up): replay of the Windows sensor setup, no-op unless replay_sensor_setup=1 */
int isp4if_replay_sensor_setup(struct isp4_interface *ispif);

/* EXPERIMENT (GZ302, patch 0038): Windows' SET_3A_CONFIG at stream start (starts the firmware's AF) */
void isp4if_send_3a_config(struct isp4_interface *ispif);

/* EXPERIMENT (GZ302, patch 0044): firmware log levels per module (SET_LOG_MODULE_LEVEL) */
void isp4if_send_log_levels(struct isp4_interface *ispif);

/* EXPERIMENT (GZ302, patch 0045): 8-bit-register I2C write performed by the firmware (SEND_I2C_MSG) */
int isp4if_fw_i2c_write(struct isp4_interface *ispif, u8 slave, u8 reg, u8 val);

/* EXPERIMENT (GZ302, patch 0048): module calibration (SET_M2M_CAL_DATA), payload built by isp4_subdev.c */
void isp4if_send_m2m_cal(struct isp4_interface *ispif, const void *buf, u32 len);

int isp4if_send_command(struct isp4_interface *ispif, u32 cmd_id,
			const void *package, u32 package_size);

int isp4if_send_command_sync(struct isp4_interface *ispif, u32 cmd_id,
			     const void *package, u32 package_size);

struct isp4if_cmd_element *isp4if_rm_cmd_from_cmdq(struct isp4_interface *ispif,
						   u32 seq_num, u32 cmd_id);

void isp4if_clear_cmdq(struct isp4_interface *ispif);

void isp4if_clear_bufq(struct isp4_interface *ispif);

void isp4if_dealloc_buffer_node(struct isp4if_img_buf_node *buf_node);

struct isp4if_img_buf_node *
isp4if_alloc_buffer_node(struct isp4if_img_buf_info *buf_info);

struct isp4if_img_buf_node *isp4if_dequeue_buffer(struct isp4_interface *ispif);

int isp4if_queue_buffer(struct isp4_interface *ispif,
			struct isp4if_img_buf_node *buf_node);

int isp4if_stop(struct isp4_interface *ispif);

int isp4if_start(struct isp4_interface *ispif);

int isp4if_deinit(struct isp4_interface *ispif);

int isp4if_init(struct isp4_interface *ispif, struct device *dev,
		void __iomem *isp_mmio);

#endif /* _ISP4_INTERFACE_H_ */
