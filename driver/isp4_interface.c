// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/firmware.h>
#include <linux/iopoll.h>
#include <linux/kernel_read_file.h>
#include <linux/moduleparam.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "isp4_debug.h"
#include "isp4_fw_cmd_resp.h"
#include "isp4_hw_reg.h"
#include "isp4_interface.h"
/*
 * EXPERIMENT (GZ302, patch 0037): the three payloads generated from the Windows driver (device script,
 * sensor properties, D-PHY receiver sequence) are no longer compiled in.  They are loaded at run time with
 * request_firmware() from /lib/firmware/amdgpu/; the gz302-camera-firmware package builds them from the
 * owner's Windows driver files.  A missing or malformed file is logged and that step is skipped.
 */
#define EXP_FW_SCRIPT		"amdgpu/gz302-ov13b-script.bin"
#define EXP_FW_SENSOR_PROP	"amdgpu/gz302-ov13b-sensor-prop.bin"
#define EXP_FW_PHY_SEQ		"amdgpu/gz302-ov13b-phy-seq.bin"
#define EXP_SCRIPT_SIZE		5868	/* SET_DEVICE_SCRIPT payload */
#define EXP_SENSOR_PROP_SIZE	360	/* SET_SENSOR_PROP payload */

/* gz302-ov13b-phy-seq.bin: header, then count operations (little-endian) */
#define EXP_PHY_MAGIC		0x48505a47	/* "GZPH" */
#define EXP_PHY_VERSION		1
#define ISP4_PHY_OP_RMW		0
#define ISP4_PHY_OP_WRITE	1

struct exp_phy_hdr {
	__le32 magic;
	__le32 version;
	__le32 phy;
	__le32 rate_mbps;
	__le32 lanes;
	__le32 count;
};

struct isp4_phy_op {
	__le16 reg;
	__le16 mask;
	__le16 val;
	__le16 kind;
};

static const struct firmware *isp4if_exp_get_fw(struct device *dev, const char *name, size_t want)
{
	const struct firmware *fw;
	int ret = request_firmware(&fw, name, dev);

	if (ret) {
		dev_err(dev, "EXP-FW: cannot load %s: %d (install the gz302-camera-firmware package)\n",
			name, ret);
		return NULL;
	}
	if (want && fw->size != want) {
		dev_err(dev, "EXP-FW: %s has %zu bytes, expected %zu\n", name, fw->size, want);
		release_firmware(fw);
		return NULL;
	}
	exp_info(dev, "EXP-FW: loaded %s (%zu bytes)\n", name, fw->size);
	return fw;
}

/* EXPERIMENT (GZ302): firmware-visible copy of the tuning file, see isp4if_replay_tuning() */
static struct isp4if_gpu_mem_info *exp_tuning_mem;
/* EXPERIMENT (GZ302, patch 0038): SET_3A_CONFIG payload, see isp4if_send_3a_config() */
static struct isp4if_gpu_mem_info *exp_3a_mem;
/* EXPERIMENT (GZ302, patch 0044): SET_LOG_MODULE_LEVEL payload, see isp4if_send_log_levels() */
static struct isp4if_gpu_mem_info *exp_log_mem;
/* EXPERIMENT (GZ302, patch 0045): data buffer of SEND_I2C_MSG, see isp4if_fw_i2c_write() */
static struct isp4if_gpu_mem_info *exp_i2c_mem;
/* EXPERIMENT (GZ302, patch 0048): SET_M2M_CAL_DATA payload, see isp4if_send_m2m_cal() */
static struct isp4if_gpu_mem_info *exp_m2m_mem;

/*
 * EXPERIMENT (GZ302): size of the firmware memory pool in MiB.  The stock driver sends 100 MiB
 * (ISP4FW_MEMORY_POOL_SIZE, sized for the OV05C10); the Windows driver allocates 200 MiB
 * (findings section 30) and the firmware runs out of internal buffers for the 13 MP OV13B mode with 100.
 * Patch 0035: 0 (default) = computed like Windows' allocate_fw_mempool_imp from the sensor pixel count
 * (findings section 75), 275,054,592 bytes for 4208 x 3120; any other value = that many MiB.
 */
static unsigned int fw_mem_pool_mb;
module_param(fw_mem_pool_mb, uint, 0444);
MODULE_PARM_DESC(fw_mem_pool_mb,
		 "EXPERIMENT: firmware memory pool size in MiB (0 = computed like Windows for the OV13B, 262.3 MiB)");

#define EXP_SENSOR_PIXELS	(4208 * 3120)	/* OV13B mode 0, the only mode used */

/* Windows' pool-size formula (amdcamera.sys allocate_fw_mempool_imp, flag 0), w = sensor pixel count */
static u64 isp4if_exp_pool_size(u32 w)
{
	u32 a = (2 * w + 0x7ffe) & 0xffff8000;
	u32 h = w >> 1;
	u32 b = ((h + 0x7fff) & 0xffff8000) + 0x798000;
	u32 c = (6 * w + 0x1fffc) & 0xfffe0000;
	u32 d = (h + 0x1fffc) & 0xfffe0000;

	return 5ULL * a + 4ULL * b + c + d;
}

#define ISP4IF_FW_RESP_RB_IRQ_EN_MASK \
	(ISP_SYS_INT0_EN__SYS_INT_RINGBUFFER_WPT9_EN_MASK\
	 | ISP_SYS_INT0_EN__SYS_INT_RINGBUFFER_WPT12_EN_MASK)

#define ISP4IF_FW_CMD_TIMEOUT (HZ / 2)

struct isp4if_rb_config {
	const char *name;
	u32 index;
	u32 reg_rptr;
	u32 reg_wptr;
	u32 reg_base_lo;
	u32 reg_base_hi;
	u32 reg_size;
	u32 val_size;
	u64 base_mc_addr;
	void *base_sys_addr;
};

/* FW cmd ring buffer configuration */
static struct isp4if_rb_config isp4if_cmd_rb_config[ISP4IF_STREAM_ID_MAX] = {
	{
		.name = "CMD_RB_GBL0",
		.index = 3,
		.reg_rptr = ISP_RB_RPTR4,
		.reg_wptr = ISP_RB_WPTR4,
		.reg_base_lo = ISP_RB_BASE_LO4,
		.reg_base_hi = ISP_RB_BASE_HI4,
		.reg_size = ISP_RB_SIZE4,
	},
	{
		.name = "CMD_RB_STR1",
		.index = 0,
		.reg_rptr = ISP_RB_RPTR1,
		.reg_wptr = ISP_RB_WPTR1,
		.reg_base_lo = ISP_RB_BASE_LO1,
		.reg_base_hi = ISP_RB_BASE_HI1,
		.reg_size = ISP_RB_SIZE1,
	},
	{
		.name = "CMD_RB_STR2",
		.index = 1,
		.reg_rptr = ISP_RB_RPTR2,
		.reg_wptr = ISP_RB_WPTR2,
		.reg_base_lo = ISP_RB_BASE_LO2,
		.reg_base_hi = ISP_RB_BASE_HI2,
		.reg_size = ISP_RB_SIZE2,
	},
	{
		.name = "CMD_RB_STR3",
		.index = 2,
		.reg_rptr = ISP_RB_RPTR3,
		.reg_wptr = ISP_RB_WPTR3,
		.reg_base_lo = ISP_RB_BASE_LO3,
		.reg_base_hi = ISP_RB_BASE_HI3,
		.reg_size = ISP_RB_SIZE3,
	},
};

/* FW resp ring buffer configuration */
static struct isp4if_rb_config isp4if_resp_rb_config[ISP4IF_STREAM_ID_MAX] = {
	{
		.name = "RES_RB_GBL0",
		.index = 3,
		.reg_rptr = ISP_RB_RPTR12,
		.reg_wptr = ISP_RB_WPTR12,
		.reg_base_lo = ISP_RB_BASE_LO12,
		.reg_base_hi = ISP_RB_BASE_HI12,
		.reg_size = ISP_RB_SIZE12,
	},
	{
		.name = "RES_RB_STR1",
		.index = 0,
		.reg_rptr = ISP_RB_RPTR9,
		.reg_wptr = ISP_RB_WPTR9,
		.reg_base_lo = ISP_RB_BASE_LO9,
		.reg_base_hi = ISP_RB_BASE_HI9,
		.reg_size = ISP_RB_SIZE9,
	},
	{
		.name = "RES_RB_STR2",
		.index = 1,
		.reg_rptr = ISP_RB_RPTR10,
		.reg_wptr = ISP_RB_WPTR10,
		.reg_base_lo = ISP_RB_BASE_LO10,
		.reg_base_hi = ISP_RB_BASE_HI10,
		.reg_size = ISP_RB_SIZE10,
	},
	{
		.name = "RES_RB_STR3",
		.index = 2,
		.reg_rptr = ISP_RB_RPTR11,
		.reg_wptr = ISP_RB_WPTR11,
		.reg_base_lo = ISP_RB_BASE_LO11,
		.reg_base_hi = ISP_RB_BASE_HI11,
		.reg_size = ISP_RB_SIZE11,
	},
};

/* FW log ring buffer configuration */
static struct isp4if_rb_config isp4if_log_rb_config = {
	.name = "LOG_RB",
	.index = 0,
	.reg_rptr = ISP_LOG_RB_RPTR0,
	.reg_wptr = ISP_LOG_RB_WPTR0,
	.reg_base_lo = ISP_LOG_RB_BASE_LO0,
	.reg_base_hi = ISP_LOG_RB_BASE_HI0,
	.reg_size = ISP_LOG_RB_SIZE0,
};

static struct isp4if_gpu_mem_info *
isp4if_gpu_mem_alloc(struct isp4_interface *ispif, u32 mem_size)
{
	struct isp4if_gpu_mem_info *mem_info;
	struct device *dev = ispif->dev;
	int ret;

	mem_info = kmalloc_obj(*mem_info, GFP_KERNEL);
	if (!mem_info)
		return NULL;

	mem_info->mem_size = mem_size;
	ret = isp_kernel_buffer_alloc(dev, mem_info->mem_size,
				      &mem_info->mem_handle,
				      &mem_info->gpu_mc_addr,
				      &mem_info->sys_addr);
	if (ret) {
		kfree(mem_info);
		return NULL;
	}

	return mem_info;
}

static void isp4if_gpu_mem_free(struct isp4_interface *ispif,
				struct isp4if_gpu_mem_info **mem_info_ptr)
{
	struct isp4if_gpu_mem_info *mem_info = *mem_info_ptr;
	struct device *dev = ispif->dev;

	if (!mem_info) {
		dev_err(dev, "invalid mem_info\n");
		return;
	}

	*mem_info_ptr = NULL;
	isp_kernel_buffer_free(&mem_info->mem_handle, &mem_info->gpu_mc_addr,
			       &mem_info->sys_addr);
	kfree(mem_info);
}

static void isp4if_dealloc_fw_gpumem(struct isp4_interface *ispif)
{
	isp4if_gpu_mem_free(ispif, &ispif->fw_mem_pool);
	isp4if_gpu_mem_free(ispif, &ispif->fw_cmd_resp_buf);
	isp4if_gpu_mem_free(ispif, &ispif->fw_log_buf);

	/* EXPERIMENT: only allocated when replay_sensor_setup=1 */
	if (ispif->exp_sensor_script)
		isp4if_gpu_mem_free(ispif, &ispif->exp_sensor_script);
	if (ispif->exp_sensor_prop)
		isp4if_gpu_mem_free(ispif, &ispif->exp_sensor_prop);
	if (exp_tuning_mem)
		isp4if_gpu_mem_free(ispif, &exp_tuning_mem);
	if (exp_3a_mem)		/* patch 0038 */
		isp4if_gpu_mem_free(ispif, &exp_3a_mem);
	if (exp_log_mem)	/* patch 0044 */
		isp4if_gpu_mem_free(ispif, &exp_log_mem);
	if (exp_i2c_mem)	/* patch 0045 */
		isp4if_gpu_mem_free(ispif, &exp_i2c_mem);
	if (exp_m2m_mem)	/* patch 0048 */
		isp4if_gpu_mem_free(ispif, &exp_m2m_mem);

	for (unsigned int i = 0; i < ISP4IF_MAX_STREAM_BUF_COUNT; i++)
		isp4if_gpu_mem_free(ispif, &ispif->meta_info_buf[i]);
}

static int isp4if_alloc_fw_gpumem(struct isp4_interface *ispif)
{
	struct device *dev = ispif->dev;

	u64 pool_size = fw_mem_pool_mb ? (u64)fw_mem_pool_mb * SZ_1M :
					 isp4if_exp_pool_size(EXP_SENSOR_PIXELS);

	exp_info(dev, "EXP: firmware memory pool %llu bytes (%s)\n", pool_size,
		 fw_mem_pool_mb ? "fw_mem_pool_mb" : "computed like Windows");
	ispif->fw_mem_pool = isp4if_gpu_mem_alloc(ispif, pool_size);
	if (!ispif->fw_mem_pool)
		goto error_no_memory;

	ispif->fw_cmd_resp_buf =
		isp4if_gpu_mem_alloc(ispif, ISP4IF_RB_PMBMAP_MEM_SIZE);
	if (!ispif->fw_cmd_resp_buf)
		goto error_no_memory;

	ispif->fw_log_buf =
		isp4if_gpu_mem_alloc(ispif, ISP4IF_FW_LOG_RINGBUF_SIZE);
	if (!ispif->fw_log_buf)
		goto error_no_memory;

	for (unsigned int i = 0; i < ISP4IF_MAX_STREAM_BUF_COUNT; i++) {
		ispif->meta_info_buf[i] =
			isp4if_gpu_mem_alloc(ispif, ISP4IF_META_INFO_BUF_SIZE);
		if (!ispif->meta_info_buf[i])
			goto error_no_memory;
	}

	return 0;

error_no_memory:
	dev_err(dev, "failed to allocate gpu memory\n");
	return -ENOMEM;
}

static u32 isp4if_compute_check_sum(const void *buf, size_t buf_size)
{
	const u8 *surplus_ptr;
	const u32 *buffer;
	u32 checksum = 0;
	size_t i;

	buffer = (const u32 *)buf;
	for (i = 0; i < buf_size / sizeof(u32); i++)
		checksum += buffer[i];

	surplus_ptr = (const u8 *)&buffer[i];
	/* add surplus data crc checksum */
	for (i = 0; i < buf_size % sizeof(u32); i++)
		checksum += surplus_ptr[i];

	return checksum;
}

void isp4if_clear_cmdq(struct isp4_interface *ispif)
{
	struct isp4if_cmd_element *buf_node, *tmp_node;
	LIST_HEAD(free_list);

	scoped_guard(spinlock, &ispif->cmdq_lock)
		list_splice_init(&ispif->cmdq, &free_list);

	list_for_each_entry_safe(buf_node, tmp_node, &free_list, list)
		kfree(buf_node);
}

static bool isp4if_is_cmdq_rb_full(struct isp4_interface *ispif,
				   enum isp4if_stream_id stream)
{
	struct isp4if_rb_config *rb_config = &isp4if_cmd_rb_config[stream];
	u32 rreg = rb_config->reg_rptr, wreg = rb_config->reg_wptr;
	u32 len = rb_config->val_size;
	u32 rd_ptr, wr_ptr;
	u32 bytes_free;

	rd_ptr = isp4hw_rreg(ispif->mmio, rreg);
	wr_ptr = isp4hw_rreg(ispif->mmio, wreg);

	/*
	 * Read and write pointers are equal, indicating the ring buffer
	 * is empty
	 */
	if (wr_ptr == rd_ptr)
		return false;

	if (wr_ptr > rd_ptr)
		bytes_free = len - (wr_ptr - rd_ptr);
	else
		bytes_free = rd_ptr - wr_ptr;

	/*
	 * Ignore one byte from the bytes free to prevent rd_ptr from equaling
	 * wr_ptr when the ring buffer is full, because rd_ptr == wr_ptr is
	 * supposed to indicate that the ring buffer is empty.
	 */
	return bytes_free <= sizeof(struct isp4fw_cmd);
}

struct isp4if_cmd_element *isp4if_rm_cmd_from_cmdq(struct isp4_interface *ispif,
						   u32 seq_num, u32 cmd_id)
{
	struct isp4if_cmd_element *ele;

	guard(spinlock)(&ispif->cmdq_lock);

	list_for_each_entry(ele, &ispif->cmdq, list) {
		if (ele->seq_num == seq_num && ele->cmd_id == cmd_id) {
			list_del(&ele->list);
			return ele;
		}
	}

	return NULL;
}

/* Must check that isp4if_is_cmdq_rb_full() == false before calling */
static int isp4if_insert_isp_fw_cmd(struct isp4_interface *ispif,
				    enum isp4if_stream_id stream,
				    const struct isp4fw_cmd *cmd)
{
	struct isp4if_rb_config *rb_config = &isp4if_cmd_rb_config[stream];
	u32 rreg = rb_config->reg_rptr, wreg = rb_config->reg_wptr;
	void *mem_sys = rb_config->base_sys_addr;
	const u32 cmd_sz = sizeof(*cmd);
	struct device *dev = ispif->dev;
	u32 len = rb_config->val_size;
	const void *src = cmd;
	u32 rd_ptr, wr_ptr;
	u32 bytes_to_end;

	rd_ptr = isp4hw_rreg(ispif->mmio, rreg);
	wr_ptr = isp4hw_rreg(ispif->mmio, wreg);
	if (rd_ptr >= len || wr_ptr >= len) {
		dev_err(dev,
			"rb invalid: stream=%u(%s), rd=%u, wr=%u, len=%u, cmd_sz=%u\n",
			stream, isp4dbg_get_if_stream_str(stream), rd_ptr,
			wr_ptr, len, cmd_sz);
		return -EINVAL;
	}

	bytes_to_end = len - wr_ptr;
	if (bytes_to_end >= cmd_sz) {
		/* FW cmd is just a straight copy to the write pointer */
		memcpy(mem_sys + wr_ptr, src, cmd_sz);
		isp4hw_wreg(ispif->mmio, wreg, (wr_ptr + cmd_sz) % len);
	} else {
		/*
		 * FW cmd is split because the ring buffer needs to wrap
		 * around
		 */
		memcpy(mem_sys + wr_ptr, src, bytes_to_end);
		memcpy(mem_sys, src + bytes_to_end, cmd_sz - bytes_to_end);
		isp4hw_wreg(ispif->mmio, wreg, cmd_sz - bytes_to_end);
	}

	return 0;
}

static inline enum isp4if_stream_id isp4if_get_fw_stream(u32 cmd_id)
{
	/*
	 * EXPERIMENT (GZ302, patch 0029): commands of type 0x01 (bits 31:24 of the command id) are the
	 * "global" commands (Windows STREAM_GLOBAL, e.g. ENABLE_PREFETCH 0x01030003) and belong on the global
	 * ring.  Every command the driver sent before is of type 0x02 and still goes to stream 1.
	 */
	if ((cmd_id >> 24) == 0x01)
		return ISP4IF_STREAM_ID_GLOBAL;

	return ISP4IF_STREAM_ID_1;
}

static int isp4if_send_fw_cmd(struct isp4_interface *ispif, u32 cmd_id,
			      const void *package,
			      u32 package_size, bool sync)
{
	enum isp4if_stream_id stream = isp4if_get_fw_stream(cmd_id);
	struct isp4if_cmd_element *ele = NULL;
	struct device *dev = ispif->dev;
	struct isp4fw_cmd cmd;
	u32 seq_num;
	int ret;

	if (package_size > sizeof(cmd.cmd_param)) {
		dev_err(dev, "fail pkgsize(%u) > %zu cmd:0x%x, stream %d\n",
			package_size, sizeof(cmd.cmd_param), cmd_id, stream);
		return -EINVAL;
	}

	/*
	 * The struct will be shared with ISP FW, use memset() to guarantee
	 * padding bits are zeroed, since this is not guaranteed on all
	 * compilers.
	 */
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = cmd_id;
	switch (stream) {
	case ISP4IF_STREAM_ID_GLOBAL:
		cmd.cmd_stream_id = ISP4FW_STREAM_ID_INVALID;
		break;
	case ISP4IF_STREAM_ID_1:
		cmd.cmd_stream_id = ISP4FW_STREAM_ID_1;
		break;
	default:
		dev_err(dev, "fail bad stream id %d\n", stream);
		return -EINVAL;
	}

	/* Allocate the sync command object early and outside of the lock */
	if (sync) {
		ele = kmalloc_obj(*ele, GFP_KERNEL);
		if (!ele)
			return -ENOMEM;

		/* Get two references: one for the resp thread, one for us */
		atomic_set(&ele->refcnt, 2);
		init_completion(&ele->cmd_done);
	}

	if (package && package_size)
		memcpy(cmd.cmd_param, package, package_size);

	if (sync) {
		/* EXPERIMENT: ring state just before a synchronous command (disable channel / stop) */
		char tag[64];

		snprintf(tag, sizeof(tag), "before %s", isp4dbg_get_cmd_str(cmd_id));
		isp4if_dump_rings(ispif, tag);
	}

	scoped_guard(mutex, &ispif->isp4if_mutex) {
		ret = read_poll_timeout(isp4if_is_cmdq_rb_full, ret, !ret,
					ISP4IF_RB_FULL_SLEEP_US,
					ISP4IF_RB_FULL_TIMEOUT_US, false, ispif,
					stream);
		if (ret) {
			struct isp4if_rb_config *rb_config =
					&isp4if_resp_rb_config[stream];
			u32 rd_ptr = isp4hw_rreg(ispif->mmio,
						 rb_config->reg_rptr);
			u32 wr_ptr = isp4hw_rreg(ispif->mmio,
						 rb_config->reg_wptr);

			dev_err(dev,
				"failed to get free cmdq slot, stream %s(%d),rd %u, wr %u\n",
				isp4dbg_get_if_stream_str(stream), stream,
				rd_ptr, wr_ptr);
			ret = -ETIMEDOUT;
			goto free_ele;
		}

		seq_num = ispif->host2fw_seq_num++;
		cmd.cmd_seq_num = seq_num;
		cmd.cmd_check_sum = isp4if_compute_check_sum(&cmd, sizeof(cmd)
							     - sizeof(u32));

		/*
		 * only append the fw cmd to queue when its response needs to
		 * be waited for, currently there are only two such commands,
		 * disable channel and stop stream which are only sent after
		 * close camera
		 */
		if (ele) {
			ele->seq_num = seq_num;
			ele->cmd_id = cmd_id;
			scoped_guard(spinlock, &ispif->cmdq_lock)
				list_add_tail(&ele->list, &ispif->cmdq);
		}

		ret = isp4if_insert_isp_fw_cmd(ispif, stream, &cmd);
		if (ret) {
			dev_err(dev,
				"fail for insert_isp_fw_cmd cmd_id %s(0x%08x)\n",
				isp4dbg_get_cmd_str(cmd_id), cmd_id);
			goto err_dequeue_ele;
		}
	}

	if (ele) {
		ret = wait_for_completion_timeout(&ele->cmd_done,
						  ISP4IF_FW_CMD_TIMEOUT);
		if (!ret) {
			/* EXPERIMENT: ring state when the firmware did not answer in time */
			char tag[64];

			snprintf(tag, sizeof(tag), "TIMEOUT %s", isp4dbg_get_cmd_str(cmd_id));
			isp4if_dump_rings(ispif, tag);
			ret = -ETIMEDOUT;
			goto err_dequeue_ele;
		}

		ret = 0;
		goto put_ele_ref;
	}

	return 0;

err_dequeue_ele:
	/*
	 * Try to remove the command from the queue. If that fails, then it
	 * means the response thread is currently using the object, and we need
	 * to use the refcount to avoid a use-after-free by either side.
	 */
	if (ele && isp4if_rm_cmd_from_cmdq(ispif, seq_num, cmd_id))
		goto free_ele;

put_ele_ref:
	/* Don't free the command if we didn't put the last reference */
	if (ele && atomic_dec_return(&ele->refcnt))
		ele = NULL;

free_ele:
	kfree(ele);
	return ret;
}

static int isp4if_send_buffer(struct isp4_interface *ispif,
			      struct isp4if_img_buf_info *buf_info)
{
	struct isp4fw_cmd_send_buffer cmd;

	/*
	 * The struct will be shared with ISP FW, use memset() to guarantee
	 * padding bits are zeroed, since this is not guaranteed on all
	 * compilers.
	 */
	memset(&cmd, 0, sizeof(cmd));
	cmd.buffer_type = ISP4FW_BUFFER_TYPE_PREVIEW;
	cmd.buffer.vmid_space.bit.space = ISP4FW_ADDR_SPACE_TYPE_GPU_VA;
	isp4if_split_addr64(buf_info->planes[0].mc_addr,
			    &cmd.buffer.buf_base_a_lo,
			    &cmd.buffer.buf_base_a_hi);
	cmd.buffer.buf_size_a = buf_info->planes[0].len;

	isp4if_split_addr64(buf_info->planes[1].mc_addr,
			    &cmd.buffer.buf_base_b_lo,
			    &cmd.buffer.buf_base_b_hi);
	cmd.buffer.buf_size_b = buf_info->planes[1].len;

	isp4if_split_addr64(buf_info->planes[2].mc_addr,
			    &cmd.buffer.buf_base_c_lo,
			    &cmd.buffer.buf_base_c_hi);
	cmd.buffer.buf_size_c = buf_info->planes[2].len;

	return isp4if_send_fw_cmd(ispif, ISP4FW_CMD_ID_SEND_BUFFER, &cmd,
				  sizeof(cmd), false);
}

static void isp4if_init_rb_config(struct isp4_interface *ispif,
				  struct isp4if_rb_config *rb_config)
{
	isp4hw_wreg(ispif->mmio, rb_config->reg_rptr, 0x0);
	isp4hw_wreg(ispif->mmio, rb_config->reg_wptr, 0x0);
	isp4hw_wreg(ispif->mmio, rb_config->reg_base_lo,
		    rb_config->base_mc_addr);
	isp4hw_wreg(ispif->mmio, rb_config->reg_base_hi,
		    rb_config->base_mc_addr >> 32);
	isp4hw_wreg(ispif->mmio, rb_config->reg_size, rb_config->val_size);
}

static int isp4if_fw_init(struct isp4_interface *ispif)
{
	u32 aligned_rb_chunk_size = ISP4IF_RB_PMBMAP_MEM_CHUNK & 0xffffffc0;
	struct isp4if_rb_config *rb_config;
	u32 offset;
	unsigned int i;

	/* initialize CMD_RB streams */
	for (i = 0; i < ISP4IF_STREAM_ID_MAX; i++) {
		rb_config = (isp4if_cmd_rb_config + i);
		offset = aligned_rb_chunk_size * rb_config->index;

		rb_config->val_size = ISP4IF_FW_CMD_BUF_SIZE;
		rb_config->base_sys_addr =
			ispif->fw_cmd_resp_buf->sys_addr + offset;
		rb_config->base_mc_addr =
			ispif->fw_cmd_resp_buf->gpu_mc_addr + offset;

		isp4if_init_rb_config(ispif, rb_config);
	}

	/* initialize RESP_RB streams */
	for (i = 0; i < ISP4IF_STREAM_ID_MAX; i++) {
		rb_config = (isp4if_resp_rb_config + i);
		offset = aligned_rb_chunk_size *
			 (rb_config->index + ISP4IF_RESP_CHAN_TO_RB_OFFSET - 1);

		rb_config->val_size = ISP4IF_FW_CMD_BUF_SIZE;
		rb_config->base_sys_addr =
			ispif->fw_cmd_resp_buf->sys_addr + offset;
		rb_config->base_mc_addr =
			ispif->fw_cmd_resp_buf->gpu_mc_addr + offset;

		isp4if_init_rb_config(ispif, rb_config);
	}

	/* initialize LOG_RB stream */
	rb_config = &isp4if_log_rb_config;
	rb_config->val_size = ISP4IF_FW_LOG_RINGBUF_SIZE;
	rb_config->base_mc_addr = ispif->fw_log_buf->gpu_mc_addr;
	rb_config->base_sys_addr = ispif->fw_log_buf->sys_addr;

	isp4if_init_rb_config(ispif, rb_config);

	return 0;
}

static int isp4if_wait_fw_ready(struct isp4_interface *ispif,
				u32 isp_status_addr)
{
	struct device *dev = ispif->dev;
	u32 timeout_ms = 100;
	u32 interval_ms = 1;
	u32 reg_val;

	/* wait for FW initialize done! */
	if (!read_poll_timeout(isp4hw_rreg, reg_val, reg_val
			       & ISP_STATUS__CCPU_REPORT_MASK,
			       interval_ms * 1000, timeout_ms * 1000, false,
			       ispif->mmio, isp_status_addr))
		return 0;

	dev_err(dev, "ISP CCPU FW boot failed\n");

	return -ETIME;
}

static void isp4if_enable_ccpu(struct isp4_interface *ispif)
{
	u32 reg_val;

	reg_val = isp4hw_rreg(ispif->mmio, ISP_SOFT_RESET);
	reg_val &= (~ISP_SOFT_RESET__CCPU_SOFT_RESET_MASK);
	isp4hw_wreg(ispif->mmio, ISP_SOFT_RESET, reg_val);

	usleep_range(100, 150);

	reg_val = isp4hw_rreg(ispif->mmio, ISP_CCPU_CNTL);
	reg_val &= (~ISP_CCPU_CNTL__CCPU_HOST_SOFT_RST_MASK);
	isp4hw_wreg(ispif->mmio, ISP_CCPU_CNTL, reg_val);
}

static void isp4if_disable_ccpu(struct isp4_interface *ispif)
{
	u32 reg_val;

	reg_val = isp4hw_rreg(ispif->mmio, ISP_CCPU_CNTL);
	reg_val |= ISP_CCPU_CNTL__CCPU_HOST_SOFT_RST_MASK;
	isp4hw_wreg(ispif->mmio, ISP_CCPU_CNTL, reg_val);

	usleep_range(100, 150);

	reg_val = isp4hw_rreg(ispif->mmio, ISP_SOFT_RESET);
	reg_val |= ISP_SOFT_RESET__CCPU_SOFT_RESET_MASK;
	isp4hw_wreg(ispif->mmio, ISP_SOFT_RESET, reg_val);
}

static int isp4if_fw_boot(struct isp4_interface *ispif)
{
	struct device *dev = ispif->dev;

	if (ispif->status != ISP4IF_STATUS_PWR_ON) {
		dev_err(dev, "invalid isp power status %d\n", ispif->status);
		return -EINVAL;
	}

	isp4if_disable_ccpu(ispif);

	isp4if_fw_init(ispif);

	/* clear ccpu status */
	isp4hw_wreg(ispif->mmio, ISP_STATUS, 0x0);

	isp4if_enable_ccpu(ispif);

	if (isp4if_wait_fw_ready(ispif, ISP_STATUS)) {
		isp4if_disable_ccpu(ispif);
		return -EINVAL;
	}

	/* enable interrupts */
	isp4hw_wreg(ispif->mmio, ISP_SYS_INT0_EN,
		    ISP4IF_FW_RESP_RB_IRQ_EN_MASK);

	ispif->status = ISP4IF_STATUS_FW_RUNNING;

	dev_dbg(dev, "ISP CCPU FW boot success\n");

	return 0;
}

int isp4if_f2h_resp(struct isp4_interface *ispif, enum isp4if_stream_id stream,
		    struct isp4fw_resp *resp)
{
	struct isp4if_rb_config *rb_config = &isp4if_resp_rb_config[stream];
	u32 rreg = rb_config->reg_rptr, wreg = rb_config->reg_wptr;
	void *mem_sys = rb_config->base_sys_addr;
	const u32 resp_sz = sizeof(*resp);
	struct device *dev = ispif->dev;
	u32 len = rb_config->val_size;
	u32 rd_ptr, wr_ptr;
	u32 bytes_to_end;
	void *dst = resp;
	u32 checksum;

	rd_ptr = isp4hw_rreg(ispif->mmio, rreg);
	wr_ptr = isp4hw_rreg(ispif->mmio, wreg);
	if (rd_ptr >= len || wr_ptr >= len)
		goto err_rb_invalid;

	/*
	 * Read and write pointers are equal, indicating the ring buffer is
	 * empty
	 */
	if (rd_ptr == wr_ptr)
		return -ENODATA;

	bytes_to_end = len - rd_ptr;
	if (bytes_to_end >= resp_sz) {
		/* FW response is just a straight copy from the read pointer */
		if (wr_ptr > rd_ptr && wr_ptr - rd_ptr < resp_sz)
			goto err_rb_invalid;

		memcpy(dst, mem_sys + rd_ptr, resp_sz);
		isp4hw_wreg(ispif->mmio, rreg, (rd_ptr + resp_sz) % len);
	} else {
		/*
		 * FW response is split because the ring buffer wrapped
		 * around
		 */
		if (wr_ptr > rd_ptr || wr_ptr < resp_sz - bytes_to_end)
			goto err_rb_invalid;

		memcpy(dst, mem_sys + rd_ptr, bytes_to_end);
		memcpy(dst + bytes_to_end, mem_sys, resp_sz - bytes_to_end);
		isp4hw_wreg(ispif->mmio, rreg, resp_sz - bytes_to_end);
	}

	checksum = isp4if_compute_check_sum(resp, resp_sz - sizeof(u32));
	if (checksum != resp->resp_check_sum) {
		dev_err(dev, "resp checksum 0x%x,should 0x%x,rptr %u,wptr %u\n",
			checksum, resp->resp_check_sum, rd_ptr, wr_ptr);
		dev_err(dev, "%s(%u), seqNo %u, resp_id %s(0x%x)\n",
			isp4dbg_get_if_stream_str(stream), stream,
			resp->resp_seq_num, isp4dbg_get_resp_str(resp->resp_id),
			resp->resp_id);
		return -EINVAL;
	}

	return 0;

err_rb_invalid:
	dev_err(dev,
		"rb invalid: stream=%u(%s), rd=%u, wr=%u, len=%u, resp_sz=%u\n",
		stream, isp4dbg_get_if_stream_str(stream), rd_ptr, wr_ptr, len,
		resp_sz);
	return -EINVAL;
}

/*
 * EXPERIMENT (GZ302 bring-up, not for upstream).
 *
 * Log the read/write pointers of the command and response rings plus the interrupt registers,
 * so a silent firmware can be told apart from responses that were written but never read:
 *   - cmd rd != cmd wr  : the firmware has not consumed every command the host queued.
 *   - resp rd != resp wr: the firmware wrote responses the host has not consumed; the first
 *                         unread one is decoded here without moving the read pointer.
 * Pointers are byte offsets into the ring. The union fields are printed raw: they only mean
 * something when resp_id is ISP4FW_RESP_ID_CMD_DONE.
 */
/*
 * EXPERIMENT (GZ302, patch 0024): the ISP's own pin controller (drivers/pinctrl/pinctrl-amdisp.c) has three
 * output-only pins, GPIO_0/1/2 = "sensor0/1/2 control", at ISP offsets 0x6613c, 0x66140 and 0x6618c; the output
 * value is bit 4.  Linux only drives sensor0.  They are printed in every EXP-RING snapshot, to see which one is
 * high while the front-camera LED is lit (findings section 53).  The window lies outside ispif->mmio (which ends at
 * 0x629a4), so it is mapped once in isp4if_init() (process context) with a plain devm_ioremap().
 */
#define EXP_ISPGPIO_BASE	0x6613c
#define EXP_ISPGPIO_SIZE	0x54
#define EXP_ISPGPIO_0		0x00
#define EXP_ISPGPIO_1		0x04
#define EXP_ISPGPIO_2		0x50
#define EXP_ISPGPIO_OUT_BIT	4

static void __iomem *exp_ispgpio;

void isp4if_dump_rings(struct isp4_interface *ispif, const char *when)
{
	struct device *dev = ispif->dev;

	/* Also drain the firmware log ring: it is otherwise only read when a response arrives. */
	isp_fw_log_dump(container_of(ispif, struct isp4_subdev, ispif), when);

	exp_info(dev, "EXP-RING[%s]: INT0_EN 0x%08x INT0_STATUS 0x%08x ISP_STATUS 0x%08x\n",
		 when, isp4hw_rreg(ispif->mmio, ISP_SYS_INT0_EN),
		 isp4hw_rreg(ispif->mmio, ISP_SYS_INT0_STATUS),
		 isp4hw_rreg(ispif->mmio, ISP_STATUS));

	if (exp_ispgpio) {
		u32 g0 = readl(exp_ispgpio + EXP_ISPGPIO_0);
		u32 g1 = readl(exp_ispgpio + EXP_ISPGPIO_1);
		u32 g2 = readl(exp_ispgpio + EXP_ISPGPIO_2);

		exp_info(dev, "EXP-RING[%s]: ISP sensor-control pins gpio0 0x%08x gpio1 0x%08x gpio2 0x%08x (output bit 4: %u %u %u)\n",
			 when, g0, g1, g2, (g0 >> EXP_ISPGPIO_OUT_BIT) & 1,
			 (g1 >> EXP_ISPGPIO_OUT_BIT) & 1, (g2 >> EXP_ISPGPIO_OUT_BIT) & 1);
	}

	for (int s = ISP4IF_STREAM_ID_GLOBAL; s <= ISP4IF_STREAM_ID_1; s++) {
		const struct isp4if_rb_config *c = &isp4if_cmd_rb_config[s];
		const struct isp4if_rb_config *r = &isp4if_resp_rb_config[s];
		u32 c_rd = isp4hw_rreg(ispif->mmio, c->reg_rptr);
		u32 c_wr = isp4hw_rreg(ispif->mmio, c->reg_wptr);
		u32 r_rd = isp4hw_rreg(ispif->mmio, r->reg_rptr);
		u32 r_wr = isp4hw_rreg(ispif->mmio, r->reg_wptr);

		exp_info(dev, "EXP-RING[%s]: %s rd %u wr %u | %s rd %u wr %u | ring size %u\n",
			 when, c->name, c_rd, c_wr, r->name, r_rd, r_wr, r->val_size);

		if (r_rd != r_wr && r->base_sys_addr && r_rd < r->val_size &&
		    r->val_size - r_rd >= sizeof(struct isp4fw_resp)) {
			const struct isp4fw_resp *p = r->base_sys_addr + r_rd;

			exp_info(dev, "EXP-RING[%s]:   unread on %s: seq %u resp_id 0x%x cmd_id 0x%x cmd_status %u\n",
				 when, r->name, p->resp_seq_num, p->resp_id,
				 p->param.cmd_done.cmd_id, p->param.cmd_done.cmd_status);
		}
	}
}

/*
 * EXPERIMENT (GZ302 bring-up, not for upstream): replay of the Windows sensor setup.
 *
 * Windows sends, before SET_STREAM_CONFIG: SET_DEVICE_SCRIPT, then (not for the OV13B, whose
 * embedded-property getter reports "unsupported") SET_SENSOR_EMB_PROP, then SET_SENSOR_PROP.
 * Each is an "indirect" command: the payload sits in firmware-visible memory and the command
 * carries {GPU address lo, hi, size, checksum}.  Linux sends none of them.
 *
 * Off unless replay_sensor_setup=1.  Commands are sent synchronously so the log shows, per
 * command, whether the firmware answered within ISP4IF_FW_CMD_TIMEOUT; a timeout is only logged.
 */
#define EXP_CMD_ID_SET_SENSOR_PROP	0x02090001
#define EXP_CMD_ID_SET_DEVICE_SCRIPT	0x02090002

static bool replay_sensor_setup = true;	/* patch 0035: default on */
module_param(replay_sensor_setup, bool, 0644);
MODULE_PARM_DESC(replay_sensor_setup,
		 "EXPERIMENT: send the Windows sensor setup (script + sensor properties) before SET_STREAM_CONFIG");

/*
 * EXPERIMENT (GZ302, patch 0032): every device-script function fails in the firmware with
 * "SvmSetI2c: Set i2c addr mode not 7bit nor 10bit" / "SvmRun: set I2C arg fail", even
 * SensorConvertAGain, which has no I2C instruction: the script VM sets up its I2C target from a
 * per-sensor descriptor before running any function, and ours is invalid (findings section 86).
 * The SET_DEVICE_SCRIPT payload ends in four 20-byte descriptors at 0x169c + k*0x14, laid out like
 * the firmware's own print "i2cSensor={deviceId,enRestart,deviceAddrType,regAddrType,deviceAddr}";
 * we send them all zero.  INFERENCE: the type codes are those of get_i2c_config_OV13B in the
 * Windows plug-in, 0x102 for the sensor at 0x10 and 0x101 for the focus motor at 0x0c, read as
 * (deviceAddrType << 8 | regAddrType), i.e. 1 = 7-bit device address, 2 = 16-bit registers.
 *   script_i2c=0 off (as before), 1 descriptor 0 = sensor, 2 also descriptor 1 = focus motor,
 *   3 all four descriptors = sensor.  script_i2c_restart is written to enRestart.
 */
#define EXP_SCRIPT_I2C_DESC_OFF	0x169c
#define EXP_SCRIPT_I2C_DESC_SIZE	0x14

static unsigned int script_i2c = 2;	/* patch 0035: sensor + focus motor */
module_param(script_i2c, uint, 0444);
MODULE_PARM_DESC(script_i2c,
		 "EXPERIMENT: fill the SET_DEVICE_SCRIPT I2C descriptors: 0 off, 1 sensor, 2 sensor + focus motor, 3 all four = sensor");

static unsigned int script_i2c_restart = 1;
module_param(script_i2c_restart, uint, 0444);
MODULE_PARM_DESC(script_i2c_restart, "EXPERIMENT: enRestart value for script_i2c (default 1)");

/*
 * EXPERIMENT (GZ302, patch 0040): Windows' sensor plug-in (get_fw_script_ctrl_OV13B) puts its VcmMode
 * (registry default 1) into the SET_DEVICE_SCRIPT tail at 0x1618, which we send as 0 (findings 107;
 * INFERENCE: it is the "vcm mode" argument of the script's LensSetPos, 1 = with the slope bit).
 * -1 (default) = leave the payload as built.
 */
#define EXP_SCRIPT_VCM_MODE_OFF	0x1618

static int script_vcm_mode = -1;
module_param(script_vcm_mode, int, 0444);
MODULE_PARM_DESC(script_vcm_mode,
		 "EXPERIMENT: VcmMode written into the SET_DEVICE_SCRIPT payload at 0x1618 (Windows: 1); -1 = leave as built");

/*
 * EXPERIMENT (GZ302, patch 0046): the whole tail that Windows' get_fw_script_ctrl_OV13B fills (findings 118).
 * Our script body is identical to Windows'; only these five words differ. With them and script_i2c=2 the
 * checksum must be 0x05234c5f, the value in the Windows trace. Meanings are INFERENCE.
 */
static bool script_tail;
module_param(script_tail, bool, 0444);
MODULE_PARM_DESC(script_tail,
		 "EXPERIMENT: write the Windows SET_DEVICE_SCRIPT tail words (0x15ec=10, 0x15f0=1176, 0x1614=2, 0x1618=1, 0x1640=1); default off");

static const struct {
	u32 off;
	u32 val;
} exp_script_tail[] = {
	{ 0x15ec, 10 },		/* sensor group: line time in us (mode 0) */
	{ 0x15f0, 1176 },	/* sensor group: line length in pixels */
	{ 0x1614, 2 },		/* sensor group, last word */
	{ 0x1618, 1 },		/* lens group: VcmMode */
	{ 0x1640, 1 },		/* lens group, last word */
};

static void isp4if_exp_put_i2c_desc(u8 *payload, unsigned int k, u32 addr, u32 code)
{
	u32 *d = (u32 *)(payload + EXP_SCRIPT_I2C_DESC_OFF + k * EXP_SCRIPT_I2C_DESC_SIZE);

	/* d[0] = deviceId: left as built (the sensor index, 0) */
	d[1] = script_i2c_restart;
	d[2] = (code >> 8) & 0xff;	/* deviceAddrType */
	d[3] = code & 0xff;		/* regAddrType */
	d[4] = addr;			/* deviceAddr */
}

static void isp4if_exp_script_i2c(struct device *dev, u8 *payload)
{
	unsigned int k;

	isp4if_exp_put_i2c_desc(payload, 0, 0x10, 0x102);
	if (script_i2c == 2)
		isp4if_exp_put_i2c_desc(payload, 1, 0x0c, 0x101);
	if (script_i2c == 3)
		for (k = 1; k < 4; k++)
			isp4if_exp_put_i2c_desc(payload, k, 0x10, 0x102);

	exp_info(dev, "EXP-SCRIPT-I2C: mode %u, descriptors %*ph\n", script_i2c,
		 4 * EXP_SCRIPT_I2C_DESC_SIZE, payload + EXP_SCRIPT_I2C_DESC_OFF);
}

static void isp4if_replay_one(struct isp4_interface *ispif, u32 cmd_id, const char *what,
			      const void *data, u32 len,
			      struct isp4if_gpu_mem_info **slot)
{
	struct device *dev = ispif->dev;
	struct isp4if_gpu_mem_info *mem;
	u32 pkg[4];
	int ret;

	mem = isp4if_gpu_mem_alloc(ispif, ALIGN(len, SZ_4K));
	if (!mem) {
		dev_err(dev, "EXP-REPLAY: %s: no GPU memory for %u bytes\n", what, len);
		return;
	}
	*slot = mem;

	memset(mem->sys_addr, 0, mem->mem_size);
	memcpy(mem->sys_addr, data, len);

	/* patch 0032: fill the script's I2C descriptors before the checksum is computed */
	if (cmd_id == EXP_CMD_ID_SET_DEVICE_SCRIPT && script_i2c &&
	    len >= EXP_SCRIPT_I2C_DESC_OFF + 4 * EXP_SCRIPT_I2C_DESC_SIZE)
		isp4if_exp_script_i2c(dev, mem->sys_addr);

	/* patch 0046: Windows' tail words; before 0040 so an explicit script_vcm_mode still wins */
	if (cmd_id == EXP_CMD_ID_SET_DEVICE_SCRIPT && script_tail) {
		for (unsigned int i = 0; i < ARRAY_SIZE(exp_script_tail); i++)
			if (len >= exp_script_tail[i].off + sizeof(u32))
				*(u32 *)(mem->sys_addr + exp_script_tail[i].off) = exp_script_tail[i].val;
		exp_info(dev, "EXP-SCRIPT-TAIL: Windows tail written (expect checksum 0x05234c5f with script_i2c=2)\n");
	}

	/* patch 0040: VcmMode in the script tail, also before the checksum */
	if (cmd_id == EXP_CMD_ID_SET_DEVICE_SCRIPT && script_vcm_mode >= 0 &&
	    len >= EXP_SCRIPT_VCM_MODE_OFF + sizeof(u32)) {
		*(u32 *)(mem->sys_addr + EXP_SCRIPT_VCM_MODE_OFF) = script_vcm_mode;
		exp_info(dev, "EXP-SCRIPT-VCM: VcmMode %d written at payload 0x%x\n",
			 script_vcm_mode, EXP_SCRIPT_VCM_MODE_OFF);
	}

	isp4if_split_addr64(mem->gpu_mc_addr, &pkg[0], &pkg[1]);
	pkg[2] = len;
	pkg[3] = isp4if_compute_check_sum(mem->sys_addr, len);

	ret = isp4if_send_command_sync(ispif, cmd_id, pkg, sizeof(pkg));
	exp_info(dev,
		 "EXP-REPLAY: %s: cmd 0x%08x, %u bytes at GPU 0x%llx, checksum 0x%08x -> %s (%d)\n",
		 what, cmd_id, len, mem->gpu_mc_addr, pkg[3],
		 ret ? "NO ANSWER / FAILED" : "answered", ret);
}

/*
 * EXPERIMENT (GZ302, patch 0038): SET_3A_CONFIG, which Windows sends once per stream start (after
 * the META buffers, before ENABLE_PREFETCH) and Linux never did, so the firmware's autofocus stayed
 * idle (findings section 101). 92 bytes, sent indirect like the sensor setup. Windows kickoff values,
 * read from isp_setup_3a and confirmed by the values Windows logged: flags 0x223, AE auto, flicker
 * auto, AWB auto, AF mode 4 = CONTINUOUS_VIDEO. Only the fields whose flag bit is set are used.
 */
#define EXP_CMD_ID_SET_3A_CONFIG	0x02020001

#define EXP_3A_F_AE_MODE	0x001
#define EXP_3A_F_FLICKER	0x002
#define EXP_3A_F_AWB_MODE	0x020
#define EXP_3A_F_AF_MODE	0x200
#define EXP_3A_F_LENS_POS	0x1000	/* patch 0039 */

struct isp4if_exp_3a_config {
	u32 flags;		/* 0x00 which fields below are valid */
	u32 ae_mode;		/* 0x04 1 manual, 2 auto */
	u32 flicker;		/* 0x08 1 off, 2 50 Hz, 3 60 Hz, 4 auto */
	u32 ev_comp[2];		/* 0x0c {numerator, denominator}, flag 0x004 */
	u32 itime;		/* 0x14 exposure time, flag 0x008 */
	u32 iso;		/* 0x18 flag 0x010 */
	u32 awb_mode;		/* 0x1c 1 auto, 2 manual gains, 3 light source, 4 colour temperature */
	u32 rsvd20[4];		/* 0x20 */
	u32 awb_param;		/* 0x30 flag 0x080 */
	u32 common_para;	/* 0x34 flag 0x100 */
	u32 af_mode;		/* 0x38 1 manual, 2 one shot, 3 continuous picture, 4 continuous video */
	u32 backlight;		/* 0x3c flag 0x400 */
	u32 rsvd40;		/* 0x40 */
	u32 pair[4];		/* 0x44 two {valid, value} pairs, flag 0x800 (not set by Windows here) */
	u32 lens_pos;		/* 0x54 manual focus position, flag 0x1000 */
	u32 af_scene;		/* 0x58 0 normal, 1 macro, flag 0x2000 */
} __packed;

static unsigned int af_mode = 4;
module_param(af_mode, uint, 0444);
MODULE_PARM_DESC(af_mode,
		 "EXPERIMENT: AF mode in SET_3A_CONFIG (win_cmds bit 0x80): 0 = leave AF out (flags 0x23), 1 manual, 2 one shot, 3 continuous picture, 4 continuous video (Windows, default)");

/*
 * EXPERIMENT (GZ302, patch 0039): manual focus, to test whether the firmware can move the lens at all
 * (findings section 103). Windows' isp_fw_set_lens_pos (0x140026d74) sends SET_3A_CONFIG with flags
 * 0x1200 (lens position + AF mode), AF mode 1 = MANUAL and the position at +0x54. The AF library's
 * range is 0..4095, 300 = infinity, 4095 = macro. -1 (default) = off, af_mode is used as before.
 */
static int lens_pos = -1;
module_param(lens_pos, int, 0444);
MODULE_PARM_DESC(lens_pos,
		 "EXPERIMENT: manual lens position 0..4095 sent in SET_3A_CONFIG (win_cmds bit 0x80; 300 = infinity, 4095 = macro); -1 = off");

void isp4if_send_3a_config(struct isp4_interface *ispif)
{
	struct isp4if_exp_3a_config cfg = {
		.flags = EXP_3A_F_AE_MODE | EXP_3A_F_FLICKER | EXP_3A_F_AWB_MODE,
		.ae_mode = 2,
		.flicker = 4,
		.awb_mode = 1,
	};

	BUILD_BUG_ON(sizeof(cfg) != 0x5c);
	if (af_mode) {
		cfg.flags |= EXP_3A_F_AF_MODE;
		cfg.af_mode = af_mode;
	}
	/* patch 0039: manual focus overrides af_mode, in the form Windows uses */
	if (lens_pos >= 0) {
		cfg.flags |= EXP_3A_F_AF_MODE | EXP_3A_F_LENS_POS;
		cfg.af_mode = 1;
		cfg.lens_pos = min(lens_pos, 4095);
	}

	/* a previous stream start left its payload buffer; the firmware has read it by now */
	if (exp_3a_mem)
		isp4if_gpu_mem_free(ispif, &exp_3a_mem);
	exp_info(ispif->dev, "EXP-3A: SET_3A_CONFIG flags 0x%x, AE %u, flicker %u, AWB %u, AF %u, lens %u\n",
		 cfg.flags, cfg.ae_mode, cfg.flicker, cfg.awb_mode, cfg.af_mode, cfg.lens_pos);
	isp4if_replay_one(ispif, EXP_CMD_ID_SET_3A_CONFIG, "SET_3A_CONFIG", &cfg, sizeof(cfg),
			  &exp_3a_mem);
}

/*
 * EXPERIMENT (GZ302, patch 0048): SET_M2M_CAL_DATA (0x02090005, stream, {GPU addr lo, hi, size, checksum},
 * sync), the module calibration Windows sends between SET_STREAM_CONFIG and APPLY_TUNING_SCENARIO
 * (findings 121). The payload (0x2180 bytes) is built by isp4_subdev.c from the module EEPROM.
 */
#define EXP_CMD_ID_SET_M2M_CAL_DATA	0x02090005

void isp4if_send_m2m_cal(struct isp4_interface *ispif, const void *buf, u32 len)
{
	/* a previous stream start left its payload buffer; the firmware has read it by now */
	if (exp_m2m_mem)
		isp4if_gpu_mem_free(ispif, &exp_m2m_mem);
	isp4if_replay_one(ispif, EXP_CMD_ID_SET_M2M_CAL_DATA, "SET_M2M_CAL_DATA", buf, len, &exp_m2m_mem);
}

/*
 * EXPERIMENT (GZ302, patch 0044): SET_LOG_MODULE_LEVEL (0x01020009, GLOBAL, indirect), to make the
 * firmware log what happens to the lens moves (findings 111, 113). Payload (116 bytes, checked against
 * the checksum 0x77577775 in Windows' trace): 112 bytes of 4-bit levels, the level of firmware module
 * id M in nibble M-1 (low nibble of byte (M-1)/2 when M-1 is even, high nibble when odd), then u32 3.
 * Windows: every module 3, the nine RTA sub-modules 0x2e..0x36 = 1 (which silences the RTA library;
 * 0 = its output as seen on Linux so far). Module ids: 9 AF_MGR, 0x12 DEVICE_SCRIPT, 0x13 DEVICE_MGR,
 * 0x22 AF_ALGO, 0x29 SENSOR_CTRL, 0x2b RTA, 0x34 RTA_AF. INFERENCE: 3 = info, 4 = debug; the firmware
 * logs "out of range" for a value above its maximum.
 */
#define EXP_CMD_ID_SET_LOG_MODULE_LEVEL	0x01020009
#define EXP_LOG_LEVEL_BYTES		112
#define EXP_LOG_MAX_MODULE		(EXP_LOG_LEVEL_BYTES * 2)

static char *fw_log_mod = "";
module_param(fw_log_mod, charp, 0444);
MODULE_PARM_DESC(fw_log_mod,
		 "EXPERIMENT: firmware module log levels on top of Windows' 3, as id:level pairs, e.g. 0x12:4,0x29:4 (win_cmds bit 0x200)");

static unsigned int fw_log_rta;	/* 0 = keep the RTA library output Linux already shows */
module_param(fw_log_rta, uint, 0444);
MODULE_PARM_DESC(fw_log_rta,
		 "EXPERIMENT: level of the nine RTA sub-modules 0x2e..0x36 (Windows 1 = quiet; default 0 = as before)");

static void isp4if_exp_set_nibble(u8 *lv, unsigned int id, unsigned int level)
{
	unsigned int n = id - 1;

	if (n & 1)
		lv[n >> 1] = (lv[n >> 1] & 0x0f) | (level << 4);
	else
		lv[n >> 1] = (lv[n >> 1] & 0xf0) | level;
}

void isp4if_send_log_levels(struct isp4_interface *ispif)
{
	struct device *dev = ispif->dev;
	u8 buf[EXP_LOG_LEVEL_BYTES + sizeof(u32)];
	char *copy, *cur, *tok;
	unsigned int id, level;

	memset(buf, 0x33, EXP_LOG_LEVEL_BYTES);		/* every module 3, as Windows */
	*(u32 *)(buf + EXP_LOG_LEVEL_BYTES) = 3;	/* Windows' tail value (meaning not decoded) */
	for (id = 0x2e; id <= 0x36; id++)
		isp4if_exp_set_nibble(buf, id, fw_log_rta & 0xf);

	/* id:level pairs; a bad pair is logged and skipped */
	copy = kstrdup(fw_log_mod, GFP_KERNEL);
	cur = copy;
	while (cur && (tok = strsep(&cur, ",")) != NULL) {
		char *lvl = strchr(tok, ':');

		if (!*tok)
			continue;
		if (!lvl) {
			dev_warn(dev, "EXP-LOG: ignoring \"%s\" (want id:level)\n", tok);
			continue;
		}
		*lvl++ = '\0';
		if (kstrtouint(tok, 0, &id) || kstrtouint(lvl, 0, &level) || !id ||
		    id > EXP_LOG_MAX_MODULE || level > 0xf) {
			dev_warn(dev, "EXP-LOG: ignoring \"%s:%s\"\n", tok, lvl);
			continue;
		}
		isp4if_exp_set_nibble(buf, id, level);
		exp_info(dev, "EXP-LOG: firmware module 0x%x log level %u\n", id, level);
	}
	kfree(copy);

	if (exp_log_mem)
		isp4if_gpu_mem_free(ispif, &exp_log_mem);
	isp4if_replay_one(ispif, EXP_CMD_ID_SET_LOG_MODULE_LEVEL, "SET_LOG_MODULE_LEVEL", buf,
			  sizeof(buf), &exp_log_mem);
}

/*
 * EXPERIMENT (GZ302, patch 0045): an I2C write done BY THE FIRMWARE (CMD_ID_SEND_I2C_MSG 0x02090003,
 * handler StreamCmdDevSendI2CMsg), so the host never touches the shared I2C controller while the
 * firmware streams (host transfers then corrupt the sensor, findings 114). Layout from Windows'
 * write sender (0x140002484, sync, 36 bytes, timeout 1 s; findings 116): controller 0, op 2 = write,
 * restart 0, address type 1 = 7-bit, register type 1 = 8-bit, slave, register, length, first data
 * byte, and the GPU address of a 4 KiB buffer that holds the data (Windows fills both).
 * A 2-byte motor frame {b1, b2} is register b1 with data b2, length 1, like the script's WI2C.
 */
#define EXP_CMD_ID_SEND_I2C_MSG	0x02090003

struct isp4if_exp_i2c_msg {
	u32 device;		/* 0x00 ISP I2C controller index (0) */
	u32 op;			/* 0x04 1 = read, 2 = write */
	u32 restart;		/* 0x08 repeated start (write: 0) */
	u32 addr_type;		/* 0x0c 1 = 7-bit address */
	u32 reg_type;		/* 0x10 1 = 8-bit register, 2 = 16-bit */
	u16 slave;		/* 0x14 7-bit I2C address */
	u16 reg;		/* 0x16 register address */
	u16 len;		/* 0x18 data bytes */
	u8 data0;		/* 0x1a first data byte, inline */
	u8 pad;			/* 0x1b */
	u32 buf_lo;		/* 0x1c GPU address of the data buffer */
	u32 buf_hi;		/* 0x20 */
} __packed;

int isp4if_fw_i2c_write(struct isp4_interface *ispif, u8 slave, u8 reg, u8 val)
{
	struct isp4if_exp_i2c_msg msg = {
		.device = 0,
		.op = 2,
		.restart = 0,
		.addr_type = 1,
		.reg_type = 1,
		.slave = slave,
		.reg = reg,
		.len = 1,
		.data0 = val,
	};
	u32 lo, hi;
	int ret;

	BUILD_BUG_ON(sizeof(msg) != 0x24);
	if (!exp_i2c_mem) {
		exp_i2c_mem = isp4if_gpu_mem_alloc(ispif, SZ_4K);
		if (!exp_i2c_mem)
			return -ENOMEM;
	}
	memset(exp_i2c_mem->sys_addr, 0, exp_i2c_mem->mem_size);
	*(u8 *)exp_i2c_mem->sys_addr = val;
	isp4if_split_addr64(exp_i2c_mem->gpu_mc_addr, &lo, &hi);	/* no pointers into the packed struct */
	msg.buf_lo = lo;
	msg.buf_hi = hi;

	ret = isp4if_send_command_sync(ispif, EXP_CMD_ID_SEND_I2C_MSG, &msg, sizeof(msg));
	dev_dbg(ispif->dev, "EXP-FWI2C: write 0x%02x reg 0x%02x = 0x%02x -> %d\n", slave, reg, val, ret);
	return ret;
}

/*
 * SET_SENSOR_PROP was rejected with fw_error_code 0x1b (findings section 18).  Send altered copies
 * of the payload and watch whether the code changes (EXP-RESP line): a different code means a
 * different check failed.
 *   0 = as built from get_caps_OV13B (360 bytes)
 *   1 = only the caps structure, without the 8-byte {sensor id, 0} prefix (352 bytes)
 *   2 = sensor id 1 instead of 0
 *   3 = 360 zero bytes (shows what the firmware says to garbage)
 */
static unsigned int sensor_prop_variant;
module_param(sensor_prop_variant, uint, 0644);
MODULE_PARM_DESC(sensor_prop_variant,
		 "EXPERIMENT: SET_SENSOR_PROP payload: 0 as built, 1 caps only (352 B), 2 sensor id 1, 3 all zero");

static void isp4if_replay_sensor_prop(struct isp4_interface *ispif)
{
	const struct firmware *fw;
	u32 len = EXP_SENSOR_PROP_SIZE;
	const u8 *data;
	u8 *copy;

	/* patch 0037: payload from /lib/firmware instead of a compiled-in table */
	fw = isp4if_exp_get_fw(ispif->dev, EXP_FW_SENSOR_PROP, EXP_SENSOR_PROP_SIZE);
	if (!fw)
		return;
	data = fw->data;

	copy = kmemdup(fw->data, len, GFP_KERNEL);
	if (!copy) {
		dev_err(ispif->dev, "EXP-REPLAY: SET_SENSOR_PROP: no memory for the variant copy\n");
		release_firmware(fw);
		return;
	}

	switch (sensor_prop_variant) {
	case 0:
		break;
	case 1:
		data = copy + 8;
		len -= 8;
		break;
	case 2:
		copy[0] = 1;
		data = copy;
		break;
	case 3:
		memset(copy, 0, len);
		data = copy;
		break;
	default:
		dev_warn(ispif->dev, "EXP-REPLAY: unknown sensor_prop_variant %u, using 0\n",
			 sensor_prop_variant);
		break;
	}

	exp_info(ispif->dev, "EXP-REPLAY: SET_SENSOR_PROP variant %u, %u bytes\n",
		 sensor_prop_variant, len);
	isp4if_replay_one(ispif, EXP_CMD_ID_SET_SENSOR_PROP, "SET_SENSOR_PROP", data, len,
			  &ispif->exp_sensor_prop);
	kfree(copy);
	release_firmware(fw);
}

/*
 * Does 0x1b mean "this command is not supported" (findings section 19)?  Send a command ID that
 * cannot exist and compare the firmware's error code in the EXP-RESP line with the 0x1b of
 * SET_SENSOR_PROP.  Same code = the firmware treats SET_SENSOR_PROP like an unknown command.
 * 0 = off.  Accepts decimal or 0x.. hex.
 */
static unsigned int bogus_cmd;
module_param(bogus_cmd, uint, 0644);
MODULE_PARM_DESC(bogus_cmd,
		 "EXPERIMENT: also send this (invalid) command ID, e.g. 0x0badbad, to learn the firmware's error code for unknown commands; 0 = off");

static void isp4if_replay_bogus_cmd(struct isp4_interface *ispif)
{
	u32 pkg[4] = { 0, 0, 0, 0 };
	int ret;

	if (!bogus_cmd)
		return;

	ret = isp4if_send_command_sync(ispif, bogus_cmd, pkg, sizeof(pkg));
	exp_info(ispif->dev, "EXP-REPLAY: BOGUS cmd 0x%08x (4 zero words) -> %s (%d)\n", bogus_cmd,
		 ret ? "NO ANSWER / FAILED" : "answered", ret);
}

/*
 * EXPERIMENT (GZ302): start the MIPI D-PHY receiver from the host, as the Windows driver does for the
 * Strix variants (findings section 41, function startup_seq_cdphy_rx).  The firmware arms the receiver but
 * counts zero frame starts although the sensor streams, and the Linux ISP4 driver has no PHY code.
 * The analog register list (since patch 0037 the file amdgpu/gz302-ov13b-phy-seq.bin) was evaluated offline for phy 0, 1120 Mbps, 4 lanes.
 * Off unless phy_init=1; called from the sensor start helper, with the sensor powered.
 *
 * Registers are ISP byte offsets, as in the Windows driver.  NOTE the mapping: ispif->mmio only covers
 * 0x0-0x629a4 (amdgpu's "isp_4_1_1_reg" resource); the PHY controller (0x66700-0x67430) is a separate resource
 * ("isp_4_1_1_phy0_reg") and the global PHY reset 0x66108 lies in neither, so the first version of this code
 * oopsed on 0x66108.  The range 0x66000-0x67fff is therefore mapped separately below, from the physical start
 * of the capture device's register resource.  Per-PHY controller at 0x66700 + 0x80 * phy for phy 0/1 (phy 0 is
 * used): +0x00 control (3 = enable), +0x04 mode (0 = D-PHY), +0x10 lane enable mask, +0x14 lane reset, +0x1c
 * status (bit 0 = ready), +0x24/+0x28 indirect PHY register address/data.  0x66108 is the global PHY reset
 * (bit = phy).  Lane stop-state status lives at 0x68008, outside any mapped range, so it is read through the
 * index/data pair at 0x61f08/0x61f0c, which lies inside ispif->mmio.
 */
#define EXP_PHY_WIN		0x66000
#define EXP_PHY_WIN_SIZE	0x2000
#define EXP_PHY_GLOBAL_RESET	0x66108
#define EXP_PHY_BASE		0x66700
#define EXP_PHY_STOPSTATE	0x68008
#define EXP_IND_INDEX		0x61f08
#define EXP_IND_DATA		0x61f0c

static bool phy_init = true;	/* patch 0035: default on */
module_param(phy_init, bool, 0444);
MODULE_PARM_DESC(phy_init,
		 "EXPERIMENT: start the MIPI D-PHY receiver from the host like Windows (needs start_sensor)");

bool isp4if_phy_init_enabled(void)
{
	return phy_init;
}

/* w = mapping of ISP offsets EXP_PHY_WIN.. (see above); off = absolute ISP byte offset inside that range */
static u32 phy_rd32(void __iomem *w, u32 off)
{
	return readl(w + (off - EXP_PHY_WIN));
}

static void phy_wr32(void __iomem *w, u32 off, u32 val)
{
	writel(val, w + (off - EXP_PHY_WIN));
}

static u16 exp_phy_rd(void __iomem *w, u16 reg)
{
	phy_wr32(w, EXP_PHY_BASE + 0x24, reg);
	return phy_rd32(w, EXP_PHY_BASE + 0x28) & 0xffff;
}

static void exp_phy_wr(void __iomem *w, u16 reg, u16 val)
{
	phy_wr32(w, EXP_PHY_BASE + 0x24, reg);
	phy_wr32(w, EXP_PHY_BASE + 0x28, val);
}

/* Windows FUN_14005131c: replace the bits selected by mask with val shifted to the lowest set bit of mask. */
static void exp_phy_rmw(void __iomem *w, u16 reg, u16 mask, u16 val)
{
	u16 old = exp_phy_rd(w, reg);
	unsigned int sh = mask ? __ffs(mask) : 0;

	exp_phy_wr(w, reg, (old & ~mask) | ((val << sh) & mask));
}

int isp4if_phy_init(struct isp4_interface *ispif)
{
	void __iomem *m = ispif->mmio;		/* only for the index/data pair at 0x61f08/0x61f0c */
	struct device *dev = ispif->dev;
	const struct exp_phy_hdr *hdr;
	const struct isp4_phy_op *ops;
	const struct firmware *fw;
	u32 phy, rate, lanes, count;
	u32 lane_mask, lane_rst;	/* 4 lanes: 0x1f and 0xf */
	struct resource *res;
	void __iomem *w;
	u32 r, ready = 0, stop = 0, want;
	unsigned int i;

	if (!phy_init)
		return 0;

	/* patch 0037: the analog register list comes from /lib/firmware (gz302-camera-firmware) */
	fw = isp4if_exp_get_fw(dev, EXP_FW_PHY_SEQ, 0);
	if (!fw)
		return -ENOENT;
	hdr = (const struct exp_phy_hdr *)fw->data;
	if (fw->size < sizeof(*hdr) || le32_to_cpu(hdr->magic) != EXP_PHY_MAGIC ||
	    le32_to_cpu(hdr->version) != EXP_PHY_VERSION) {
		dev_err(dev, "EXP-PHY: %s: bad header\n", EXP_FW_PHY_SEQ);
		release_firmware(fw);
		return -EINVAL;
	}
	phy = le32_to_cpu(hdr->phy);
	rate = le32_to_cpu(hdr->rate_mbps);
	lanes = le32_to_cpu(hdr->lanes);
	count = le32_to_cpu(hdr->count);
	if (phy > 3 || lanes < 1 || lanes > 4 || fw->size != sizeof(*hdr) + (size_t)count * sizeof(*ops)) {
		dev_err(dev, "EXP-PHY: %s: phy %u, lanes %u, %u operations do not match %zu bytes\n",
			EXP_FW_PHY_SEQ, phy, lanes, count, fw->size);
		release_firmware(fw);
		return -EINVAL;
	}
	ops = (const struct isp4_phy_op *)(fw->data + sizeof(*hdr));
	lane_mask = (2U << lanes) - 1;
	lane_rst = (1U << lanes) - 1;

	/* ISP offset 0 is the start of the capture device's first memory resource (see the comment above) */
	res = platform_get_resource(to_platform_device(dev), IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "EXP-PHY: no memory resource on the capture device\n");
		release_firmware(fw);
		return -ENODEV;
	}
	w = ioremap(res->start + EXP_PHY_WIN, EXP_PHY_WIN_SIZE);
	if (!w) {
		dev_err(dev, "EXP-PHY: cannot map the PHY register range\n");
		release_firmware(fw);
		return -ENOMEM;
	}
	exp_info(dev, "EXP-PHY: mapped ISP offsets 0x%x-0x%x (physical 0x%llx)\n", EXP_PHY_WIN,
		 EXP_PHY_WIN + EXP_PHY_WIN_SIZE - 1, (unsigned long long)(res->start + EXP_PHY_WIN));

	exp_info(dev, "EXP-PHY: starting the D-PHY receiver (phy %u, %u Mbps, %u lanes), %u register operations\n",
		 phy, rate, lanes, count);
	exp_info(dev, "EXP-PHY: before: global reset 0x%08x, ctl 0x%08x, status 0x%08x\n",
		 phy_rd32(w, EXP_PHY_GLOBAL_RESET), phy_rd32(w, EXP_PHY_BASE),
		 phy_rd32(w, EXP_PHY_BASE + 0x1c));

	/* All-ones on every register means the block is not reachable (powered down, wrong address): do not write. */
	if (phy_rd32(w, EXP_PHY_GLOBAL_RESET) == 0xffffffff && phy_rd32(w, EXP_PHY_BASE + 0x1c) == 0xffffffff) {
		dev_err(dev, "EXP-PHY: PHY registers read as all-ones; not touching them\n");
		iounmap(w);
		release_firmware(fw);
		return -EIO;
	}

	/* 1. pulse the PHY's bit in the global reset register */
	r = phy_rd32(w, EXP_PHY_GLOBAL_RESET);
	phy_wr32(w, EXP_PHY_GLOBAL_RESET, r | BIT(phy));
	usleep_range(1000, 1500);
	r = phy_rd32(w, EXP_PHY_GLOBAL_RESET);
	phy_wr32(w, EXP_PHY_GLOBAL_RESET, r & ~BIT(phy));

	/* 2. disable, lane masks, D-PHY mode */
	phy_wr32(w, EXP_PHY_BASE + 0x00, 0);
	phy_wr32(w, EXP_PHY_BASE + 0x14, lane_rst | 0x10);
	phy_wr32(w, EXP_PHY_BASE + 0x10, lane_mask);
	phy_wr32(w, EXP_PHY_BASE + 0x04, 0);

	/* 3. analog configuration */
	for (i = 0; i < count; i++) {
		const struct isp4_phy_op *op = &ops[i];

		if (le16_to_cpu(op->kind) == ISP4_PHY_OP_RMW)
			exp_phy_rmw(w, le16_to_cpu(op->reg), le16_to_cpu(op->mask), le16_to_cpu(op->val));
		else
			exp_phy_wr(w, le16_to_cpu(op->reg), le16_to_cpu(op->val));
	}
	release_firmware(fw);	/* the operations are not needed after this point */

	/* 4. enable and wait for "PHY ready" (Windows waits without a limit; here at most about 100 ms) */
	phy_wr32(w, EXP_PHY_BASE + 0x00, 3);
	for (i = 0; i < 100; i++) {
		usleep_range(1000, 1500);
		ready = phy_rd32(w, EXP_PHY_BASE + 0x1c);
		if (ready & 1)
			break;
	}

	/* 5. lane stop-state: bits 15:8 of the status must contain the lane mask (here: at most 5 tries, then carry on) */
	want = lane_mask;
	for (i = 0; i < 5; i++) {
		usleep_range(1000, 1500);
		isp4hw_wreg(m, EXP_IND_INDEX, EXP_PHY_STOPSTATE);
		stop = isp4hw_rreg(m, EXP_IND_DATA);
		if (((stop >> 8) & want) == want)
			break;
	}

	/* 6. release the lane resets (keep nothing of the 0x1f bits) */
	phy_wr32(w, EXP_PHY_BASE + 0x14, lane_rst & 0xffffffe0);

	exp_info(dev, "EXP-PHY: done: PHY ready %s (status 0x%08x), lane stop-state 0x%08x (want bits 15:8 = 0x%02x: %s), after: ctl 0x%08x\n",
		 (ready & 1) ? "yes" : "NO", ready, stop, want, (((stop >> 8) & want) == want) ? "yes" : "NO",
		 phy_rd32(w, EXP_PHY_BASE));
	iounmap(w);
	return (ready & 1) ? 0 : -ETIMEDOUT;
}

/*
 * EXPERIMENT (GZ302, patch 0022): read-only snapshot of the D-PHY receiver state, to tell "the sensor is not
 * transmitting" (data lanes stay in the LP-11 stop state) from "the sensor transmits but the receiver cannot decode it"
 * (lanes leave stop state).  Logs the 13 direct dwords of the PHY controller (0x66700-0x66730), the global PHY reset
 * and the lane stop-state.  Only reads; but the status registers may be clear-on-read, so a snapshot can clear a
 * latched bit (observer effect).  Same separate ioremap window as isp4if_phy_init().  Needs phy_init=1.
 */
void isp4if_phy_diag(struct isp4_interface *ispif, const char *label)
{
	struct device *dev = ispif->dev;
	struct resource *res;
	void __iomem *w;
	u32 stop;
	unsigned int i;

	if (!phy_init)
		return;

	res = platform_get_resource(to_platform_device(dev), IORESOURCE_MEM, 0);
	if (!res)
		return;
	w = ioremap(res->start + EXP_PHY_WIN, EXP_PHY_WIN_SIZE);
	if (!w)
		return;

	isp4hw_wreg(ispif->mmio, EXP_IND_INDEX, EXP_PHY_STOPSTATE);
	stop = isp4hw_rreg(ispif->mmio, EXP_IND_DATA);
	exp_info(dev, "EXP-PHYDIAG[%s]: global reset 0x%08x, lane stop-state 0x%08x (bits 15:8 = 0x%02x)\n", label,
		 phy_rd32(w, EXP_PHY_GLOBAL_RESET), stop, (stop >> 8) & 0xff);
	for (i = 0; i <= 0x30; i += 4)
		exp_info(dev, "EXP-PHYDIAG[%s]: +0x%02x = 0x%08x\n", label, i, phy_rd32(w, EXP_PHY_BASE + i));
	iounmap(w);
}

/*
 * SET_TUNING_DATA (0x02050008), decoded from amdcamera.sys (findings section 28).  The firmware
 * log says "Host setfile buf queue empty" / "RTA setfile buf empty" and fails START_STREAM
 * (error 0x27) because the host never delivers the sensor tuning file.  Windows copies the file
 * (OV13B_3001_ROG_SRX.atb, 565520 bytes) to offset 0 of a firmware-visible buffer and sends a
 * 12-word parameter block:
 *     [0] 14 (buffer type, "type=14" in the firmware log)   [1] 0   [2] 4 (unknown, copied)
 *     [3],[4] GPU address lo/hi   [5] size   [6]..[8] second blob (address lo/hi, size) = 0 here
 * The file is read at run time from the path in tuning_file, so no Windows data is in the source.
 * Off when tuning_file is empty.
 */
#define EXP_CMD_ID_SET_TUNING_DATA	0x02050008
#define EXP_TUNING_BUF_TYPE		14
#define EXP_TUNING_AREA			0x300000	/* Windows' buffer size for the first blob */

static char *tuning_file = "/lib/firmware/amdgpu/OV13B_3001_ROG_SRX.atb";	/* patch 0035 */
module_param(tuning_file, charp, 0444);
MODULE_PARM_DESC(tuning_file,
		 "EXPERIMENT: path of the sensor tuning file (OV13B_3001_ROG_SRX.atb) to send with SET_TUNING_DATA; needs replay_sensor_setup=1");

static void isp4if_replay_tuning(struct isp4_interface *ispif)
{
	struct device *dev = ispif->dev;
	struct isp4if_gpu_mem_info *mem;
	u32 pkg[12] = { 0 };
	size_t size = 0;
	void *data = NULL;
	ssize_t rd;
	int ret;

	if (!tuning_file || !*tuning_file)
		return;

	rd = kernel_read_file_from_path(tuning_file, 0, &data, EXP_TUNING_AREA, &size,
					READING_FIRMWARE);
	if (rd < 0) {
		dev_err(dev, "EXP-REPLAY: SET_TUNING_DATA: cannot read %s: %zd\n", tuning_file, rd);
		return;
	}

	/* A previous init of this module load may have left its buffer behind. */
	if (exp_tuning_mem)
		isp4if_gpu_mem_free(ispif, &exp_tuning_mem);

	mem = isp4if_gpu_mem_alloc(ispif, EXP_TUNING_AREA);
	if (!mem) {
		dev_err(dev, "EXP-REPLAY: SET_TUNING_DATA: no GPU memory for %u bytes\n", EXP_TUNING_AREA);
		vfree(data);
		return;
	}
	exp_tuning_mem = mem;

	memset(mem->sys_addr, 0, mem->mem_size);
	memcpy(mem->sys_addr, data, size);
	vfree(data);

	pkg[0] = EXP_TUNING_BUF_TYPE;
	pkg[2] = 4;
	isp4if_split_addr64(mem->gpu_mc_addr, &pkg[3], &pkg[4]);
	pkg[5] = size;

	ret = isp4if_send_command_sync(ispif, EXP_CMD_ID_SET_TUNING_DATA, pkg, sizeof(pkg));
	exp_info(dev, "EXP-REPLAY: SET_TUNING_DATA: %zu bytes from %s at GPU 0x%llx -> %s (%d)\n",
		 size, tuning_file, mem->gpu_mc_addr, ret ? "NO ANSWER / FAILED" : "answered", ret);
}

int isp4if_replay_sensor_setup(struct isp4_interface *ispif)
{
	const struct firmware *fw;

	if (!replay_sensor_setup)
		return 0;

	exp_info(ispif->dev, "EXP-REPLAY: sending the Windows sensor setup before SET_STREAM_CONFIG\n");
	isp4if_dump_rings(ispif, "replay: before");

	/* patch 0037: the compiled device script comes from /lib/firmware */
	fw = isp4if_exp_get_fw(ispif->dev, EXP_FW_SCRIPT, EXP_SCRIPT_SIZE);
	if (fw) {
		isp4if_replay_one(ispif, EXP_CMD_ID_SET_DEVICE_SCRIPT, "SET_DEVICE_SCRIPT",
				  fw->data, fw->size, &ispif->exp_sensor_script);
		release_firmware(fw);
	}
	isp4if_replay_tuning(ispif);	/* Windows order: script, (embedded prop skipped), tuning, sensor prop */
	isp4if_replay_sensor_prop(ispif);
	isp4if_replay_bogus_cmd(ispif);

	isp4if_dump_rings(ispif, "replay: after");

	/* Never fail the bring-up because of the experiment: carry on so the result can be observed. */
	return 0;
}

int isp4if_send_command(struct isp4_interface *ispif, u32 cmd_id,
			const void *package, u32 package_size)
{
	return isp4if_send_fw_cmd(ispif, cmd_id, package, package_size, false);
}

int isp4if_send_command_sync(struct isp4_interface *ispif, u32 cmd_id,
			     const void *package, u32 package_size)
{
	return isp4if_send_fw_cmd(ispif, cmd_id, package, package_size, true);
}

void isp4if_clear_bufq(struct isp4_interface *ispif)
{
	struct isp4if_img_buf_node *buf_node, *tmp_node;
	LIST_HEAD(free_list);

	scoped_guard(spinlock, &ispif->bufq_lock)
		list_splice_init(&ispif->bufq, &free_list);

	list_for_each_entry_safe(buf_node, tmp_node, &free_list, node)
		kfree(buf_node);
}

void isp4if_dealloc_buffer_node(struct isp4if_img_buf_node *buf_node)
{
	kfree(buf_node);
}

struct isp4if_img_buf_node *
isp4if_alloc_buffer_node(struct isp4if_img_buf_info *buf_info)
{
	struct isp4if_img_buf_node *node;

	node = kmalloc_obj(*node, GFP_KERNEL);
	if (node)
		node->buf_info = *buf_info;

	return node;
}

struct isp4if_img_buf_node *isp4if_dequeue_buffer(struct isp4_interface *ispif)
{
	struct isp4if_img_buf_node *buf_node;

	guard(spinlock)(&ispif->bufq_lock);

	buf_node = list_first_entry_or_null(&ispif->bufq, typeof(*buf_node),
					    node);
	if (buf_node)
		list_del(&buf_node->node);

	return buf_node;
}

int isp4if_queue_buffer(struct isp4_interface *ispif,
			struct isp4if_img_buf_node *buf_node)
{
	int ret;

	ret = isp4if_send_buffer(ispif, &buf_node->buf_info);
	if (ret)
		return ret;

	scoped_guard(spinlock, &ispif->bufq_lock)
		list_add_tail(&buf_node->node, &ispif->bufq);

	return 0;
}

int isp4if_stop(struct isp4_interface *ispif)
{
	isp4if_disable_ccpu(ispif);

	isp4if_dealloc_fw_gpumem(ispif);

	return 0;
}

int isp4if_start(struct isp4_interface *ispif)
{
	int ret;

	ret = isp4if_alloc_fw_gpumem(ispif);
	if (ret)
		return ret;

	ret = isp4if_fw_boot(ispif);
	if (ret)
		goto failed_fw_boot;

	return 0;

failed_fw_boot:
	isp4if_dealloc_fw_gpumem(ispif);
	return ret;
}

int isp4if_deinit(struct isp4_interface *ispif)
{
	isp4if_clear_cmdq(ispif);

	isp4if_clear_bufq(ispif);

	mutex_destroy(&ispif->isp4if_mutex);

	return 0;
}

int isp4if_init(struct isp4_interface *ispif, struct device *dev,
		void __iomem *isp_mmio)
{
	struct resource *res;

	ispif->dev = dev;
	ispif->mmio = isp_mmio;

	/* patch 0024: map the ISP sensor-control pin registers for the EXP-RING snapshots (see above) */
	exp_ispgpio = NULL;
	res = platform_get_resource(to_platform_device(dev), IORESOURCE_MEM, 0);
	if (res)
		exp_ispgpio = devm_ioremap(dev, res->start + EXP_ISPGPIO_BASE, EXP_ISPGPIO_SIZE);
	if (!exp_ispgpio)
		dev_warn(dev, "EXP: ISP sensor-control pin registers not mapped, they will not be logged\n");

	spin_lock_init(&ispif->cmdq_lock); /* used for cmdq access */
	spin_lock_init(&ispif->bufq_lock); /* used for bufq access */
	mutex_init(&ispif->isp4if_mutex); /* used for commands sent to ispfw */

	INIT_LIST_HEAD(&ispif->cmdq);
	INIT_LIST_HEAD(&ispif->bufq);

	return 0;
}
