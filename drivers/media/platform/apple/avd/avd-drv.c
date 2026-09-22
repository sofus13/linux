// SPDX-License-Identifier: GPL-2.0
/*
 * Apple Video Decoder driver
 *
 * Copyright (C) 2026 The Asahi Linux Contributors
 * Copyright (C) 2026 Sofus Forstreuter <sofus.c@icloud.com>
 *
 * Based on rkvdec driver by Collabora, Ltd.
 * Copyright (C) 2019 Collabora, Ltd.
 * Based on rkvdec driver by Google LLC. (Tomasz Figa <tfiga@chromium.org>)
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 */

#include <linux/component.h>
#include <linux/pm_runtime.h>
#include <linux/iommu.h>
#include <linux/reset.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/iopoll.h>
#include <linux/of_platform.h>

#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#include "avd.h"
#include "avd-inst.h"

static void calc_tile_meta(u32 w, u32 h, u32 bpb, u32 tile_dim,
			   u32 meta_hdr_bytes, u32 *tile, u32 *meta)
{
	u32 tiles_width, tiles_height, meta_tile_w, meta_tile_h, tile_bytes;

	tiles_width = DIV_ROUND_UP(w, tile_dim);
	tiles_height = DIV_ROUND_UP(h, tile_dim);
	tile_bytes = tile_dim * tile_dim * DIV_ROUND_UP(bpb, 8);
	*tile = ALIGN(tiles_width * tiles_height * tile_bytes, 16);

	meta_tile_w = roundup_pow_of_two(tiles_width);
	meta_tile_h = roundup_pow_of_two(tiles_height);

	*meta = ALIGN(meta_tile_w * meta_tile_h * meta_hdr_bytes, 16);
}

void fill_comp(struct avd_comp *comp, enum avd_image_fmt image_fmt, u32 width,
	       u32 height)
{
	u32 y_meta, y, uv_meta, uv;
	int bit_depth, vdiv, hdiv = 2;

	switch (image_fmt) {
	case AVD_IMG_FMT_420_10BIT:
	case AVD_IMG_FMT_422_10BIT:
		bit_depth = 10;
		break;
	default:
		bit_depth = 8;
		break;
	}

	switch (image_fmt) {
	case AVD_IMG_FMT_420_10BIT:
	case AVD_IMG_FMT_420_8BIT:
		vdiv = 2;
		break;
	default:
		vdiv = 1;
		break;
	}

	/* y has 32x32 tiles and 32 bytes of metadata per tile */
	calc_tile_meta(width, height, bit_depth, 32, 32, &y, &y_meta);
	/* uv has 16x16 tiles and 8 bytes of metadata per tile */
	calc_tile_meta(width / vdiv, height / hdiv, bit_depth * 2, 16, 8, &uv,
		       &uv_meta);

	/* output like DCP driver expects */
	comp->offsets[0] = y;
	comp->offsets[1] = 0;
	comp->offsets[2] = y + y_meta + uv;
	comp->offsets[3] = y + y_meta;

	comp->size = y_meta + y + uv_meta + uv;
}

int avd_buf_alloc(struct avd_dev *avd, struct avd_buf *buf, size_t size)
{
	if (buf->cpu && size < buf->size)
		return 0;
	else if (buf->cpu)
		avd_buf_free(avd, buf);

	if (size <= 0)
		return -ENOMEM;

	buf->size = size;
	buf->cpu =
		dma_alloc_coherent(avd->main_core->dev, buf->size, &buf->addr, GFP_KERNEL);
	return buf->cpu ? 0 : -ENOMEM;
}

void avd_buf_free(struct avd_dev *avd, struct avd_buf *buf)
{
	if (buf->cpu)
		dma_free_coherent(avd->main_core->dev, buf->size, buf->cpu, buf->addr);
	memset(buf, 0, sizeof(*buf));
}

struct avd_decoded_buffer *
avd_get_ref_buf(struct avd_ctx *ctx, struct vb2_v4l2_buffer *dst, u64 timestamp)
{
	struct v4l2_m2m_ctx *m2m_ctx = ctx->fh.m2m_ctx;
	struct vb2_queue *cap_q = &m2m_ctx->cap_q_ctx.q;
	struct vb2_buffer *buf;

	/*
	 * If a ref is unused or invalid, address of current destination
	 * buffer is returned.
	 */
	buf = vb2_find_buffer(cap_q, timestamp);
	if (!buf)
		buf = &dst->vb2_buf;

	return vb2_to_avd_decoded_buf(buf);
}

int avd_end_segment(struct avd_ctx *ctx, bool update_submit)
{
	struct avd_job *job = &ctx->job;
	struct avd_segment *seg = &job->segments[job->num];

	/* avd_segment includes piodma_cmd which is not transferred */
	seg->piodma_cmd =
		AVD_PIODMA_CMD_SIZE((sizeof(struct avd_segment) - 8) / 4);
	seg->piodma_cmd |= AVD_PIODMA_CMD_DEST(job->dest);
	seg->piodma_cmd |= AVD_PIODMA_CMD_CONST;

	job->num++;
	if (update_submit)
		job->num_submit++;
	return job->num >= job->num_alloc;
}

int avd_init_job(struct avd_ctx *ctx, enum avd_codec codec, size_t segments)
{
	int ret = 0;
	struct avd_job *job = &ctx->job;

	job->codec = codec;
	job->dest = 0x1000;
	job->num = 0;
	job->num_submit = 0;
	job->num_alloc = segments;
	ret = avd_buf_alloc(ctx->dev, &job->buf, job->num_alloc * sizeof(*job->segments));
	job->segments = job->buf.cpu;
	memset(job->buf.cpu, 0, job->buf.size);
	return ret;
}

struct avd_cm3_job {
	enum avd_codec codec;
	u32 dest;
	u32 num;
	u32 num_submit;
	u64 iova;
	u64 insn;
};

int avd_submit_job(struct avd_ctx *ctx)
{
	struct avd_core *core = ctx->core;
	struct avd_job *job = &ctx->job;
	int submit_off = 0x100;
	struct avd_cm3_job submit = (struct avd_cm3_job) {
		.codec = job->codec,
		.dest = job->dest,
		.num = job->num,
		.num_submit = job->num_submit,
		.iova = job->buf.addr,
		.insn = ctx->inst.addr,
	};

	schedule_delayed_work(&ctx->watchdog_work, msecs_to_jiffies(2000));
	memcpy_toio(core->sram + submit_off, &submit, sizeof(submit));
	writel(submit_off, core->mbox + AVD_REG_MBOX1_SUBMIT);

	/* ???? */
	usleep_range(1000, 1000);

	return 0;
}

static int avd_core_boot(struct avd_core *core)
{
	struct avd_dev *avd = core->avd;
	u32 val;
	int ret;
	char version[64];

	if (avd->variant->revision != 3)
		dev_info_once(core->dev, "booting hw version: %04x",
			      readl_relaxed(core->ctrl));

	writel(core->sram_start, core->piodma + 0x24);
	dev_info_once(core->dev, "piodma version: %04x base: %08x",
		      readl_relaxed(core->piodma + 0xb4),
		      readl_relaxed(core->piodma + 0x24));

	memcpy_toio(core->code, avd->fw->data, avd->fw->size);

	writel_relaxed(AVD_MBOX_ENABLE, core->mbox + AVD_REG_MBOX1_STATUS);
	writel_relaxed(AVD_MBOX_ENABLE, core->mbox + AVD_REG_MBOX0_STATUS);
	writel_relaxed(AVD_MBOX0_NOT_EMPTY,
		       core->mbox + AVD_REG_MBOX_IRQ_ENABLE);
	writel_relaxed(AVD_RUN_CTRL_UNK_RUN, core->mbox + AVD_REG_RUN_CTRL);

	/* wait for cm3 to boot */
	ret = readl_poll_timeout(core->mbox + AVD_REG_FLAG0_SET, val, val == 1,
				 10, 10000);
	if (ret)
		return ret;

	memcpy_fromio(version, core->sram, sizeof(version));
	dev_info_once(core->dev, "fw version: %s\n", version);

	return 0;
}

static void avd_core_shutdown(struct avd_core *core)
{
	writel_relaxed(AVD_RUN_CTRL_UNK_STOP, core->mbox + AVD_REG_RUN_CTRL);
	writel_relaxed(1, core->mbox + AVD_REG_FLAG0_CLR);
	writel_relaxed(0, core->mbox + AVD_REG_MBOX_IRQ_ENABLE);
}

static int avd_core_reset(struct avd_core *core)
{
	int ret = 0;

	ret = pm_runtime_resume_and_get(core->dev);
	if (ret < 0)
		return ret;

	ret = reset_control_reset(core->rstc);
	if (ret)
		dev_err(core->dev, "reset: failed: %d", ret);

	if (core->empty_domain) {
		/*
		 * this differs from rkvdec in both that we dont do it from an
		 * interrupt handler and that we control when the reset
		 * happens.
		 *
		 * There is still a possibility that the hw can be reset
		 * without going through the pm-domain, although i dont expect
		 * it
		 */
		iommu_detach_device(core->curr_ctx->dev->domain, core->dev);
		ret = iommu_attach_device(core->empty_domain, core->dev);
		if (ret)
			dev_warn(core->dev, "Cannot attach empty domain: %d\n", ret);
		iommu_detach_device(core->empty_domain, core->dev);
		ret = iommu_attach_device(core->curr_ctx->dev->domain, core->dev);
		if (ret)
			dev_warn(core->dev, "Cannot attach global domain: %d\n", ret);
	}

	ret = avd_core_boot(core);
	if (ret)
		dev_err(core->dev, "reset: failed to boot core %d", core->id);

	pm_runtime_put_autosuspend(core->dev);

	return ret;
}

static void avd_watchdog_func(struct work_struct *work)
{
	struct avd_core *core;
	struct avd_ctx *ctx;
	int ret;

	ctx = container_of(to_delayed_work(work), struct avd_ctx,
			   watchdog_work);
	if (!ctx)
		return;

	core = ctx->core;

	dev_err(core->dev, "Frame processing timed out!");

	writel(0, core->mbox + AVD_REG_MBOX_IRQ_ENABLE);
	ret = avd_core_reset(core);
	if (ret)
		dev_err(core->dev, "failed to reset: %d", ret);

	avd_job_finish(ctx, VB2_BUF_STATE_ERROR);
}

static irqreturn_t avd_irq_handler(int irq, void *data)
{
	struct avd_core *core = data;
	struct avd_ctx *ctx = core->curr_ctx;
	enum vb2_buffer_state state;
	u32 status;

	status = readl(core->mbox + AVD_REG_MBOX0_RETRIEVE);
	writel(AVD_MBOX0_NOT_EMPTY, core->mbox + AVD_REG_MBOX_IRQ_CLR);

	/* TODO: we should be a bit smarter about this */
	if (status & 0x10000) { /* dbg */
		dev_warn(core->dev, "no handler for IRQ: %3d",
			 status & ~0x10000);
		writel_relaxed(0, core->mbox + AVD_REG_MBOX_IRQ_ENABLE);
		return IRQ_HANDLED;
	}

	if (!ctx)
		return IRQ_HANDLED;

	if (status & 0x1000) {
		state = VB2_BUF_STATE_DONE;
	} else {
		dev_err(core->dev, "error: fw says: %x", status);
		/* let watchdog handle */
		goto done;
	}

	/* if the watchdog_work has run the work has already been submitted */
	if (cancel_delayed_work(&ctx->watchdog_work))
		avd_job_finish(ctx, state);

done:
	return IRQ_HANDLED;
}

/**
 * Return a core that is available for decoding or null if no core is found.
 * The caller should make sure to call release_core() when the core is no longer needed.
 */
struct avd_core *acquire_core(struct avd_dev *avd, struct avd_ctx *ctx)
{
	struct avd_core *core = NULL;

	guard(spinlock_irqsave)(&avd->cores_lock);

	if (avd->available_core_count) {
		core = avd->available_cores[--avd->available_core_count];

		/* Set the current core's ctx to this ctx */
		core->curr_ctx = ctx;
	}

	return core;
}

/**
 * Release the core to make it available for a next job.
 */
void release_core(struct avd_dev *avd, struct avd_core *core)
{
	guard(spinlock_irqsave)(&avd->cores_lock);

	core->curr_ctx = NULL;
	avd->available_cores[avd->available_core_count++] = core;
}

static void avd_device_run(void *priv)
{
	struct avd_ctx *ctx = priv;
	const struct avd_coded_fmt_desc *desc = ctx->coded_fmt_desc;
	int ret;

	if (WARN_ON(!desc))
		return;

	ctx->core = acquire_core(ctx->dev, ctx);
	if (WARN_ON(!ctx->core))
		return;

	ret = pm_runtime_resume_and_get(ctx->core->dev);
	if (ret < 0) {
		avd_job_finish_no_pm(ctx, VB2_BUF_STATE_ERROR);
		return;
	}

	ret = desc->ops->run(ctx);
	if (ret)
		avd_job_finish(ctx, VB2_BUF_STATE_ERROR);
}

static int avd_queue_init(void *priv, struct vb2_queue *src_vq,
			  struct vb2_queue *dst_vq)
{
	struct avd_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->ops = &avd_queue_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;

	src_vq->dma_attrs = 0;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->vdev_lock;
	src_vq->dev = ctx->dev->v4l2_dev.dev;
	src_vq->supports_requests = true;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->bidirectional = true;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->dma_attrs = 0;
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &avd_queue_ops;
	dst_vq->buf_struct_size = sizeof(struct avd_decoded_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->vdev_lock;
	dst_vq->dev = ctx->dev->v4l2_dev.dev;

	return vb2_queue_init(dst_vq);
}

static int avd_open(struct file *filp)
{
	struct avd_dev *avd = video_drvdata(filp);
	struct avd_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = avd;

	ret = avd_buf_alloc(avd, &ctx->inst, AVD_FIFO_SIZE);
	if (ret)
		goto err_free_ctx;

	ret = avd_buf_alloc(avd, &ctx->pipe_state, 512);
	if (ret)
		goto err_free_ctx;

	INIT_DELAYED_WORK(&ctx->watchdog_work, avd_watchdog_func);

	avd_reset_coded_fmt(ctx);
	avd_reset_decoded_fmt(ctx);

	v4l2_fh_init(&ctx->fh, video_devdata(filp));

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(avd->m2m_dev, ctx, avd_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_free_ctx;
	}

	ret = avd_init_ctrls(ctx);
	if (ret)
		goto err_cleanup_m2m_ctx;

	v4l2_fh_add(&ctx->fh, filp);

	return 0;

err_cleanup_m2m_ctx:
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);

err_free_ctx:
	avd_buf_free(avd, &ctx->pipe_state);
	avd_buf_free(avd, &ctx->inst);
	kfree(ctx);
	return ret;
}

static int avd_release(struct file *filp)
{
	struct avd_ctx *ctx = file_to_ctx(filp);

	v4l2_fh_del(&ctx->fh, filp);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	v4l2_fh_exit(&ctx->fh);
	avd_buf_free(ctx->dev, &ctx->inst);
	avd_buf_free(ctx->dev, &ctx->pipe_state);
	avd_buf_free(ctx->dev, &ctx->job.buf);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations avd_fops = {
	.owner = THIS_MODULE,
	.open = avd_open,
	.release = avd_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static const struct v4l2_m2m_ops avd_m2m_ops = {
	.device_run = avd_device_run,
};

static const struct media_device_ops avd_media_ops = {
	.req_validate = vb2_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

static int avd_v4l2_init(struct avd_dev *avd)
{
	int ret;
	struct device *dev = avd->main_core->dev;

	ret = v4l2_device_register(dev, &avd->v4l2_dev);
	if (ret) {
		dev_err(dev, "Failed to register V4L2 device\n");
		return ret;
	}

	avd->m2m_dev = v4l2_m2m_init(&avd_m2m_ops);
	if (IS_ERR(avd->m2m_dev)) {
		v4l2_err(&avd->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(avd->m2m_dev);
		goto err_unregister_v4l2;
	}

	avd->mdev.dev = dev;
	strscpy(avd->mdev.model, "avd", sizeof(avd->mdev.model));
	strscpy(avd->mdev.bus_info, "platform:avd", sizeof(avd->mdev.bus_info));
	media_device_init(&avd->mdev);
	avd->mdev.ops = &avd_media_ops;
	avd->v4l2_dev.mdev = &avd->mdev;

	avd->vdev.lock = &avd->vdev_lock;
	avd->vdev.v4l2_dev = &avd->v4l2_dev;
	avd->vdev.fops = &avd_fops;
	avd->vdev.release = video_device_release_empty;
	avd->vdev.vfl_dir = VFL_DIR_M2M;
	avd->vdev.device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M_MPLANE;
	avd->vdev.ioctl_ops = &avd_ioctl_ops;
	video_set_drvdata(&avd->vdev, avd);
	strscpy(avd->vdev.name, "avd", sizeof(avd->vdev.name));

	ret = video_register_device(&avd->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_err(&avd->v4l2_dev, "Failed to register video device\n");
		goto err_cleanup_mc;
	}

	ret = v4l2_m2m_register_media_controller(avd->m2m_dev, &avd->vdev,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret) {
		v4l2_err(&avd->v4l2_dev,
			 "Failed to initialize V4L2 M2M media controller\n");
		goto err_unregister_vdev;
	}

	ret = media_device_register(&avd->mdev);
	if (ret) {
		v4l2_err(&avd->v4l2_dev, "Failed to register media device\n");
		goto err_unregister_mc;
	}

	return 0;

err_unregister_mc:
	v4l2_m2m_unregister_media_controller(avd->m2m_dev);

err_unregister_vdev:
	video_unregister_device(&avd->vdev);

err_cleanup_mc:
	media_device_cleanup(&avd->mdev);
	v4l2_m2m_release(avd->m2m_dev);

err_unregister_v4l2:
	v4l2_device_unregister(&avd->v4l2_dev);
	return ret;
}

static void avd_v4l2_cleanup(struct avd_dev *avd)
{
	media_device_unregister(&avd->mdev);
	v4l2_m2m_unregister_media_controller(avd->m2m_dev);
	video_unregister_device(&avd->vdev);
	media_device_cleanup(&avd->mdev);
	v4l2_m2m_release(avd->m2m_dev);
	v4l2_device_unregister(&avd->v4l2_dev);
}

static const struct avd_variant avd_t8103_variant = {
	.capabilities = AVD_CAPABILITY_HEVC |
			AVD_CAPABILITY_H264 |
			AVD_CAPABILITY_VP9,
	.fw_name = "apple/avd-fw-v2-t0.bin",
	.revision = 3,
	.quirks = AVD_QUIRK_LSR | AVD_QUIRK_NO_PIPE_STATE,
};

static const struct avd_variant avd_t6000_variant = {
	.capabilities = AVD_CAPABILITY_HEVC |
			AVD_CAPABILITY_H264 |
			AVD_CAPABILITY_VP9,
	.fw_name = "apple/avd-fw-v3-t0.bin",
	.revision = 4,
	.quirks = AVD_QUIRK_LSR | AVD_QUIRK_NO_PIPE_STATE,
};

static const struct avd_variant avd_t8112_variant = {
	.capabilities = AVD_CAPABILITY_HEVC |
			AVD_CAPABILITY_H264 |
			AVD_CAPABILITY_VP9,
	.fw_name = "apple/avd-fw-v3-t1.bin",
	.revision = 4,
	.quirks = AVD_QUIRK_LSR,
};

static const struct avd_variant avd_t6020_variant = {
	.capabilities = AVD_CAPABILITY_HEVC |
			AVD_CAPABILITY_H264 |
			AVD_CAPABILITY_VP9,
	.fw_name = "apple/avd-fw-v3-t2.bin",
	.revision = 4,
};

static const struct avd_variant avd_t8122_variant = {
	.capabilities = AVD_CAPABILITY_HEVC |
			AVD_CAPABILITY_H264 |
			AVD_CAPABILITY_VP9 |
			AVD_CAPABILITY_AV1,
	.fw_name = "apple/avd-fw-v4-t0.bin",
	.revision = 4,
};

static const struct avd_variant avd_t8140_variant = {
	.capabilities = AVD_CAPABILITY_HEVC |
			AVD_CAPABILITY_H264 |
			AVD_CAPABILITY_VP9 |
			AVD_CAPABILITY_AV1,
	.fw_name = "apple/avd-fw-v5-t0.bin",
	.revision = 4,
};

static const struct avd_variant avd_t8132_variant = {
	.capabilities = AVD_CAPABILITY_HEVC |
			AVD_CAPABILITY_H264 |
			AVD_CAPABILITY_VP9 |
			AVD_CAPABILITY_AV1,
	.fw_name = "apple/avd-fw-v5-t1.bin",
	.revision = 4,
};

/* can also be derived from a version register */
static const struct of_device_id avd_of_match[] = {
	{ .compatible = "apple,t8103-avd", .data = &avd_t8103_variant },
	{ .compatible = "apple,t6000-avd", .data = &avd_t6000_variant },
	{ .compatible = "apple,t8112-avd", .data = &avd_t8112_variant },
	{ .compatible = "apple,t6020-avd", .data = &avd_t6020_variant },
	{ .compatible = "apple,t8122-avd", .data = &avd_t8122_variant },
	{ .compatible = "apple,t8132-avd", .data = &avd_t8132_variant },
	{ .compatible = "apple,t8140-avd", .data = &avd_t8140_variant },
	{},
};

MODULE_DEVICE_TABLE(of, avd_of_match);

static int avd_core_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct avd_core *core = platform_get_drvdata(pdev);
	struct avd_dev *avd = data;
	int id, ret;

	core->avd = avd;
	id = avd->core_count;
	core->id = id;
	avd->cores[id] = core;

	if (id == 0)
		avd->main_core = core;

	if (iommu_get_domain_for_dev(dev)) {
		if (!avd->domain) {
			avd->domain = iommu_get_domain_for_dev(dev);
			if (IS_ERR(avd->domain)) {
				avd->domain = NULL;
				dev_warn_once(dev, "cannot get global domain\n");
			}
		}

		if (avd->domain) {
			ret = iommu_attach_device(avd->domain, dev);
			if (ret)
				dev_warn(dev, "cannot attach global domain to core %d\n", id);
		}
	}

	release_core(avd, core);
	avd->core_count++;

	dev_info(dev, "Registered core %d\n", id);

	return 0;
}

static const struct component_ops avd_core_ops = {
	.bind = avd_core_bind,
};

static int avd_core_probe(struct platform_device *pdev)
{
	struct avd_core *core;
	int ret, irq;

	if (!pdev->dev.of_node)
		return -ENODEV;

	core = devm_kzalloc(&pdev->dev, sizeof(*core), GFP_KERNEL);
	if (!core)
		return -ENOMEM;

	platform_set_drvdata(pdev, core);
	core->dev = &pdev->dev;

	core->rstc = devm_reset_control_get_exclusive(core->dev, NULL);

	core->piodma = devm_platform_ioremap_resource_byname(pdev, "piodma");
	if (IS_ERR(core->piodma))
		return PTR_ERR(core->piodma);

	core->code = devm_platform_ioremap_resource_byname(pdev, "code");
	if (IS_ERR(core->code))
		return PTR_ERR(core->code);

	core->sram = devm_platform_ioremap_resource_byname(pdev, "sram");
	if (IS_ERR(core->sram))
		return PTR_ERR(core->sram);

	core->mbox = devm_platform_ioremap_resource_byname(pdev, "mbox");
	if (IS_ERR(core->mbox))
		return PTR_ERR(core->mbox);

	core->ctrl = devm_platform_ioremap_resource_byname(pdev, "ctrl");
	if (IS_ERR(core->ctrl))
		return PTR_ERR(core->ctrl);

	core->sram_start = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						       "sram")->start >> 4;

	if (iommu_get_domain_for_dev(core->dev)) {
		core->empty_domain = iommu_paging_domain_alloc(core->dev);
		if (IS_ERR(core->empty_domain)) {
			core->empty_domain = NULL;
			dev_warn(core->dev, "cannot alloc new empty domain");
		}
	}

	/* does it matter? */
	ret = dma_set_mask_and_coherent(core->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err(core->dev, "Failed to set DMA mask");
		return ret;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL, avd_irq_handler,
					IRQF_ONESHOT, dev_name(&pdev->dev),
					core);
	if (ret) {
		dev_err(core->dev, "Could not request IRQ 0");
		return ret;
	}

	pm_runtime_set_autosuspend_delay(core->dev, 100);
	pm_runtime_use_autosuspend(core->dev);
	pm_runtime_enable(core->dev);

	platform_set_drvdata(pdev, core);

	ret = component_add(&pdev->dev, &avd_core_ops);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register component: %d\n", ret);
		goto err_disable_runtime_pm;
	}

	return 0;

err_disable_runtime_pm:
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	if (core->empty_domain)
		iommu_domain_free(core->empty_domain);

	return ret;
}

static void avd_core_remove(struct platform_device *pdev)
{
	struct avd_core *core = platform_get_drvdata(pdev);

	component_del(&pdev->dev, &avd_core_ops);

	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	if (core->empty_domain)
		iommu_domain_free(core->empty_domain);
}

static __maybe_unused int avd_runtime_resume(struct device *dev)
{
	int ret;
	struct avd_core *core = platform_get_drvdata(to_platform_device(dev));

	ret = avd_core_boot(core);
	if (ret)
		dev_err(dev, "failed to boot core %d\n", core->id);

	return ret;
}

static __maybe_unused int avd_runtime_suspend(struct device *dev)
{
	struct avd_core *core = platform_get_drvdata(to_platform_device(dev));

	avd_core_shutdown(core);
	return 0;
}

static const struct dev_pm_ops avd_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(avd_runtime_suspend, avd_runtime_resume, NULL)
};

static struct platform_driver avd_core_pdrv = {
	.probe = avd_core_probe,
	.remove = avd_core_remove,
	.driver = {
		.name = "avd-core",
		.of_match_table = avd_of_match,
		.pm = &avd_pm_ops,
	},
};

static int avd_bind(struct device *dev)
{
	struct avd_dev *avd = dev_get_drvdata(dev);
	int ret;

	ret = component_bind_all(dev, avd);
	if (ret) {
		dev_err(dev, "component bind failed\n");
		return ret;
	}

	ret = avd_v4l2_init(avd);
	if (ret)
		goto err_unbind;

	v4l2_m2m_set_max_parallel_jobs(avd->m2m_dev, avd->core_count);

	return 0;

err_unbind:
	component_unbind_all(dev, NULL);
	return ret;
}

static void avd_unbind(struct device *dev)
{
	struct avd_dev *avd = dev_get_drvdata(dev);

	avd_v4l2_cleanup(avd);
	release_firmware(avd->fw);
	component_unbind_all(dev, NULL);
}

static const struct component_master_ops avd_master_ops = {
	.bind = avd_bind,
	.unbind = avd_unbind,
};

static int avd_probe(struct platform_device *pdev)
{
	const struct of_device_id *match_desc = pdev->dev.platform_data;
	struct device *dev = &pdev->dev;
	struct component_match *match = NULL;
	struct device_node *core_node;
	struct avd_dev *avd;
	unsigned int num_cores = 0;
	int ret;

	if (!match_desc)
		return dev_err_probe(dev, -ENODEV, "missing platform data\n");

	for_each_compatible_node(core_node, NULL, match_desc->compatible) {
		if (!of_device_is_available(core_node))
			continue;

		of_node_get(core_node);
		component_match_add_release(dev, &match, component_release_of,
					    component_compare_of, core_node);
		num_cores++;
	}

	if (!match)
		return dev_err_probe(dev, -ENODEV,
				     "no matching available component devices found\n");

	avd = devm_kzalloc(dev, sizeof(*avd), GFP_KERNEL);
	if (!avd)
		return -ENOMEM;

	avd->cores = devm_kcalloc(dev, num_cores, sizeof(*avd->cores),
				     GFP_KERNEL);
	if (!avd->cores)
		return -ENOMEM;

	avd->available_cores = devm_kcalloc(dev, num_cores,
					       sizeof(*avd->available_cores),
					       GFP_KERNEL);
	if (!avd->available_cores)
		return -ENOMEM;

	avd->variant = match_desc->data;
	if (!avd->variant)
		return dev_err_probe(dev, -ENODEV, "failed to get match data\n");

	ret = request_firmware(&avd->fw, avd->variant->fw_name, dev);
	if (ret) {
		dev_err(dev, "failed to load firmware: %d", ret);
		return ret;
	}

	mutex_init(&avd->vdev_lock);
	spin_lock_init(&avd->cores_lock);

	dev_set_drvdata(dev, avd);

	return component_master_add_with_match(dev, &avd_master_ops, match);
}

static void avd_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &avd_master_ops);
}

static struct platform_driver avd_pdrv = {
	.probe = avd_probe,
	.remove = avd_remove,
	.driver = {
		.name = "avd",
	},
};

static bool avd_of_has_available_node(const char *compat)
{
	struct device_node *node;

	for_each_compatible_node(node, NULL, compat) {
		if (of_device_is_available(node)) {
			of_node_put(node);
			return true;
		}
	}

	return false;
}

static int avd_create_platform_device(struct platform_device **ppdev,
				      const struct of_device_id *match)
{
	struct platform_device *pdev;
	int ret;

	pdev = platform_device_alloc(match->compatible, PLATFORM_DEVID_NONE);
	if (!pdev)
		return -ENOMEM;

	ret = platform_device_add_data(pdev, match, sizeof(*match));
	if (ret)
		goto free_platform_device;

	ret = platform_device_add(pdev);
	if (ret)
		goto free_platform_device;

	ret = device_driver_attach(&avd_pdrv.driver, &pdev->dev);
	if (ret)
		goto del_platform_device;

	*ppdev = pdev;

	return 0;

del_platform_device:
	platform_device_del(pdev);
free_platform_device:
	platform_device_put(pdev);
	return ret;
}

static struct platform_device *master_pdevs[ARRAY_SIZE(avd_of_match) - 1];

static int __init avd_init(void)
{
	unsigned int i;
	int ret;

	ret = platform_driver_register(&avd_core_pdrv);
	if (ret)
		return ret;

	ret = platform_driver_register(&avd_pdrv);
	if (ret)
		goto unregister_core_driver;

	for (i = 0; i < ARRAY_SIZE(master_pdevs); i++) {
		if (!avd_of_has_available_node(avd_of_match[i].compatible))
			continue;

		ret = avd_create_platform_device(&master_pdevs[i],
						 &avd_of_match[i]);
		if (ret)
			goto unregister_platform_devices;
	}

	return 0;

unregister_platform_devices:
	for (i = 0; i < ARRAY_SIZE(master_pdevs); i++) {
		if (master_pdevs[i]) {
			platform_device_unregister(master_pdevs[i]);
			master_pdevs[i] = NULL;
		}
	}
	platform_driver_unregister(&avd_pdrv);
unregister_core_driver:
	platform_driver_unregister(&avd_core_pdrv);
	return ret;
}
module_init(avd_init);

static void __exit avd_exit(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(master_pdevs); i++) {
		if (master_pdevs[i]) {
			platform_device_unregister(master_pdevs[i]);
			master_pdevs[i] = NULL;
		}
	}
	platform_driver_unregister(&avd_pdrv);
	platform_driver_unregister(&avd_core_pdrv);
}
module_exit(avd_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Video Decoder driver");
MODULE_FIRMWARE("apple/avd-fw-v2-t0.bin");
MODULE_FIRMWARE("apple/avd-fw-v3-t0.bin");
MODULE_FIRMWARE("apple/avd-fw-v3-t1.bin");
MODULE_FIRMWARE("apple/avd-fw-v3-t2.bin");
MODULE_FIRMWARE("apple/avd-fw-v4-t0.bin");
MODULE_FIRMWARE("apple/avd-fw-v5-t0.bin");
MODULE_FIRMWARE("apple/avd-fw-v5-t1.bin");
