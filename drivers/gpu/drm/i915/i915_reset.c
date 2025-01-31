/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2008-2018 Intel Corporation
 */

#include <linux/sched/mm.h>

#include "i915_drv.h"
#include "i915_gpu_error.h"
#include "i915_reset.h"

#include "intel_guc.h"

static void engine_skip_context(struct i915_request *rq)
{
	struct intel_engine_cs *engine = rq->engine;
	struct i915_gem_context *hung_ctx = rq->gem_context;
	struct i915_timeline *timeline = rq->timeline;
	unsigned long flags;

	GEM_BUG_ON(timeline == &engine->timeline);

	spin_lock_irqsave(&engine->timeline.lock, flags);
	spin_lock(&timeline->lock);

	list_for_each_entry_continue(rq, &engine->timeline.requests, link)
		if (rq->gem_context == hung_ctx)
			i915_request_skip(rq, -EIO);

	list_for_each_entry(rq, &timeline->requests, link)
		i915_request_skip(rq, -EIO);

	spin_unlock(&timeline->lock);
	spin_unlock_irqrestore(&engine->timeline.lock, flags);
}

static void client_mark_guilty(struct drm_i915_file_private *file_priv,
			       const struct i915_gem_context *ctx)
{
	unsigned int score;
	unsigned long prev_hang;

	if (i915_gem_context_is_banned(ctx))
		score = I915_CLIENT_SCORE_CONTEXT_BAN;
	else
		score = 0;

	prev_hang = xchg(&file_priv->hang_timestamp, jiffies);
	if (time_before(jiffies, prev_hang + I915_CLIENT_FAST_HANG_JIFFIES))
		score += I915_CLIENT_SCORE_HANG_FAST;

	if (score) {
		atomic_add(score, &file_priv->ban_score);

		DRM_DEBUG_DRIVER("client %s: gained %u ban score, now %u\n",
				 ctx->name, score,
				 atomic_read(&file_priv->ban_score));
	}
}

static void context_mark_guilty(struct i915_gem_context *ctx)
{
	unsigned int score;
	bool banned, bannable;

	atomic_inc(&ctx->guilty_count);

	bannable = i915_gem_context_is_bannable(ctx);
	score = atomic_add_return(CONTEXT_SCORE_GUILTY, &ctx->ban_score);
	banned = score >= CONTEXT_SCORE_BAN_THRESHOLD;

	/* Cool contexts don't accumulate client ban score */
	if (!bannable)
		return;

	if (banned) {
		DRM_DEBUG_DRIVER("context %s: guilty %d, score %u, banned\n",
				 ctx->name, atomic_read(&ctx->guilty_count),
				 score);
		i915_gem_context_set_banned(ctx);
	}

	if (!IS_ERR_OR_NULL(ctx->file_priv))
		client_mark_guilty(ctx->file_priv, ctx);
}

static void context_mark_innocent(struct i915_gem_context *ctx)
{
	atomic_inc(&ctx->active_count);
}

static void gen3_stop_engine(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	const u32 base = engine->mmio_base;

	if (intel_engine_stop_cs(engine))
		DRM_DEBUG_DRIVER("%s: timed out on STOP_RING\n", engine->name);

	I915_WRITE_FW(RING_HEAD(base), I915_READ_FW(RING_TAIL(base)));
	POSTING_READ_FW(RING_HEAD(base)); /* paranoia */

	I915_WRITE_FW(RING_HEAD(base), 0);
	I915_WRITE_FW(RING_TAIL(base), 0);
	POSTING_READ_FW(RING_TAIL(base));

	/* The ring must be empty before it is disabled */
	I915_WRITE_FW(RING_CTL(base), 0);

	/* Check acts as a post */
	if (I915_READ_FW(RING_HEAD(base)) != 0)
		DRM_DEBUG_DRIVER("%s: ring head not parked\n",
				 engine->name);
}

static void i915_stop_engines(struct drm_i915_private *i915,
			      unsigned int engine_mask)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	if (INTEL_GEN(i915) < 3)
		return;

	for_each_engine_masked(engine, i915, engine_mask, id)
		gen3_stop_engine(engine);
}

static bool i915_in_reset(struct pci_dev *pdev)
{
	u8 gdrst;

	pci_read_config_byte(pdev, I915_GDRST, &gdrst);
	return gdrst & GRDOM_RESET_STATUS;
}

static int i915_do_reset(struct drm_i915_private *i915,
			 unsigned int engine_mask,
			 unsigned int retry)
{
	struct pci_dev *pdev = i915->drm.pdev;
	int err;

	/* Assert reset for at least 20 usec, and wait for acknowledgement. */
	pci_write_config_byte(pdev, I915_GDRST, GRDOM_RESET_ENABLE);
	usleep_range(50, 200);
	err = wait_for(i915_in_reset(pdev), 500);

	/* Clear the reset request. */
	pci_write_config_byte(pdev, I915_GDRST, 0);
	usleep_range(50, 200);
	if (!err)
		err = wait_for(!i915_in_reset(pdev), 500);

	return err;
}

static bool g4x_reset_complete(struct pci_dev *pdev)
{
	u8 gdrst;

	pci_read_config_byte(pdev, I915_GDRST, &gdrst);
	return (gdrst & GRDOM_RESET_ENABLE) == 0;
}

static int g33_do_reset(struct drm_i915_private *i915,
			unsigned int engine_mask,
			unsigned int retry)
{
	struct pci_dev *pdev = i915->drm.pdev;

	pci_write_config_byte(pdev, I915_GDRST, GRDOM_RESET_ENABLE);
	return wait_for(g4x_reset_complete(pdev), 500);
}

static int g4x_do_reset(struct drm_i915_private *dev_priv,
			unsigned int engine_mask,
			unsigned int retry)
{
	struct pci_dev *pdev = dev_priv->drm.pdev;
	int ret;

	/* WaVcpClkGateDisableForMediaReset:ctg,elk */
	I915_WRITE(VDECCLK_GATE_D,
		   I915_READ(VDECCLK_GATE_D) | VCP_UNIT_CLOCK_GATE_DISABLE);
	POSTING_READ(VDECCLK_GATE_D);

	pci_write_config_byte(pdev, I915_GDRST,
			      GRDOM_MEDIA | GRDOM_RESET_ENABLE);
	ret =  wait_for(g4x_reset_complete(pdev), 500);
	if (ret) {
		DRM_DEBUG_DRIVER("Wait for media reset failed\n");
		goto out;
	}

	pci_write_config_byte(pdev, I915_GDRST,
			      GRDOM_RENDER | GRDOM_RESET_ENABLE);
	ret =  wait_for(g4x_reset_complete(pdev), 500);
	if (ret) {
		DRM_DEBUG_DRIVER("Wait for render reset failed\n");
		goto out;
	}

out:
	pci_write_config_byte(pdev, I915_GDRST, 0);

	I915_WRITE(VDECCLK_GATE_D,
		   I915_READ(VDECCLK_GATE_D) & ~VCP_UNIT_CLOCK_GATE_DISABLE);
	POSTING_READ(VDECCLK_GATE_D);

	return ret;
}

static int ironlake_do_reset(struct drm_i915_private *dev_priv,
			     unsigned int engine_mask,
			     unsigned int retry)
{
	int ret;

	I915_WRITE(ILK_GDSR, ILK_GRDOM_RENDER | ILK_GRDOM_RESET_ENABLE);
	ret = intel_wait_for_register(dev_priv,
				      ILK_GDSR, ILK_GRDOM_RESET_ENABLE, 0,
				      500);
	if (ret) {
		DRM_DEBUG_DRIVER("Wait for render reset failed\n");
		goto out;
	}

	I915_WRITE(ILK_GDSR, ILK_GRDOM_MEDIA | ILK_GRDOM_RESET_ENABLE);
	ret = intel_wait_for_register(dev_priv,
				      ILK_GDSR, ILK_GRDOM_RESET_ENABLE, 0,
				      500);
	if (ret) {
		DRM_DEBUG_DRIVER("Wait for media reset failed\n");
		goto out;
	}

out:
	I915_WRITE(ILK_GDSR, 0);
	POSTING_READ(ILK_GDSR);
	return ret;
}

/* Reset the hardware domains (GENX_GRDOM_*) specified by mask */
static int gen6_hw_domain_reset(struct drm_i915_private *dev_priv,
				u32 hw_domain_mask)
{
	int err;

	/*
	 * GEN6_GDRST is not in the gt power well, no need to check
	 * for fifo space for the write or forcewake the chip for
	 * the read
	 */
	I915_WRITE_FW(GEN6_GDRST, hw_domain_mask);

	/* Wait for the device to ack the reset requests */
	err = __intel_wait_for_register_fw(dev_priv,
					   GEN6_GDRST, hw_domain_mask, 0,
					   500, 0,
					   NULL);
	if (err)
		DRM_DEBUG_DRIVER("Wait for 0x%08x engines reset failed\n",
				 hw_domain_mask);

	return err;
}

static int gen6_reset_engines(struct drm_i915_private *i915,
			      unsigned int engine_mask,
			      unsigned int retry)
{
	struct intel_engine_cs *engine;
	const u32 hw_engine_mask[I915_NUM_ENGINES] = {
		[RCS] = GEN6_GRDOM_RENDER,
		[BCS] = GEN6_GRDOM_BLT,
		[VCS] = GEN6_GRDOM_MEDIA,
		[VCS2] = GEN8_GRDOM_MEDIA2,
		[VECS] = GEN6_GRDOM_VECS,
	};
	u32 hw_mask;

	if (engine_mask == ALL_ENGINES) {
		hw_mask = GEN6_GRDOM_FULL;
	} else {
		unsigned int tmp;

		hw_mask = 0;
		for_each_engine_masked(engine, i915, engine_mask, tmp)
			hw_mask |= hw_engine_mask[engine->id];
	}

	return gen6_hw_domain_reset(i915, hw_mask);
}

static u32 gen11_lock_sfc(struct drm_i915_private *dev_priv,
			  struct intel_engine_cs *engine)
{
	u8 vdbox_sfc_access = RUNTIME_INFO(dev_priv)->vdbox_sfc_access;
	i915_reg_t sfc_forced_lock, sfc_forced_lock_ack;
	u32 sfc_forced_lock_bit, sfc_forced_lock_ack_bit;
	i915_reg_t sfc_usage;
	u32 sfc_usage_bit;
	u32 sfc_reset_bit;

	switch (engine->class) {
	case VIDEO_DECODE_CLASS:
		if ((BIT(engine->instance) & vdbox_sfc_access) == 0)
			return 0;

		sfc_forced_lock = GEN11_VCS_SFC_FORCED_LOCK(engine);
		sfc_forced_lock_bit = GEN11_VCS_SFC_FORCED_LOCK_BIT;

		sfc_forced_lock_ack = GEN11_VCS_SFC_LOCK_STATUS(engine);
		sfc_forced_lock_ack_bit  = GEN11_VCS_SFC_LOCK_ACK_BIT;

		sfc_usage = GEN11_VCS_SFC_LOCK_STATUS(engine);
		sfc_usage_bit = GEN11_VCS_SFC_USAGE_BIT;
		sfc_reset_bit = GEN11_VCS_SFC_RESET_BIT(engine->instance);
		break;

	case VIDEO_ENHANCEMENT_CLASS:
		sfc_forced_lock = GEN11_VECS_SFC_FORCED_LOCK(engine);
		sfc_forced_lock_bit = GEN11_VECS_SFC_FORCED_LOCK_BIT;

		sfc_forced_lock_ack = GEN11_VECS_SFC_LOCK_ACK(engine);
		sfc_forced_lock_ack_bit  = GEN11_VECS_SFC_LOCK_ACK_BIT;

		sfc_usage = GEN11_VECS_SFC_USAGE(engine);
		sfc_usage_bit = GEN11_VECS_SFC_USAGE_BIT;
		sfc_reset_bit = GEN11_VECS_SFC_RESET_BIT(engine->instance);
		break;

	default:
		return 0;
	}

	/*
	 * Tell the engine that a software reset is going to happen. The engine
	 * will then try to force lock the SFC (if currently locked, it will
	 * remain so until we tell the engine it is safe to unlock; if currently
	 * unlocked, it will ignore this and all new lock requests). If SFC
	 * ends up being locked to the engine we want to reset, we have to reset
	 * it as well (we will unlock it once the reset sequence is completed).
	 */
	I915_WRITE_FW(sfc_forced_lock,
		      I915_READ_FW(sfc_forced_lock) | sfc_forced_lock_bit);

	if (__intel_wait_for_register_fw(dev_priv,
					 sfc_forced_lock_ack,
					 sfc_forced_lock_ack_bit,
					 sfc_forced_lock_ack_bit,
					 1000, 0, NULL)) {
		DRM_DEBUG_DRIVER("Wait for SFC forced lock ack failed\n");
		return 0;
	}

	if (I915_READ_FW(sfc_usage) & sfc_usage_bit)
		return sfc_reset_bit;

	return 0;
}

static void gen11_unlock_sfc(struct drm_i915_private *dev_priv,
			     struct intel_engine_cs *engine)
{
	u8 vdbox_sfc_access = RUNTIME_INFO(dev_priv)->vdbox_sfc_access;
	i915_reg_t sfc_forced_lock;
	u32 sfc_forced_lock_bit;

	switch (engine->class) {
	case VIDEO_DECODE_CLASS:
		if ((BIT(engine->instance) & vdbox_sfc_access) == 0)
			return;

		sfc_forced_lock = GEN11_VCS_SFC_FORCED_LOCK(engine);
		sfc_forced_lock_bit = GEN11_VCS_SFC_FORCED_LOCK_BIT;
		break;

	case VIDEO_ENHANCEMENT_CLASS:
		sfc_forced_lock = GEN11_VECS_SFC_FORCED_LOCK(engine);
		sfc_forced_lock_bit = GEN11_VECS_SFC_FORCED_LOCK_BIT;
		break;

	default:
		return;
	}

	I915_WRITE_FW(sfc_forced_lock,
		      I915_READ_FW(sfc_forced_lock) & ~sfc_forced_lock_bit);
}

static int gen11_reset_engines(struct drm_i915_private *i915,
			       unsigned int engine_mask,
			       unsigned int retry)
{
	const u32 hw_engine_mask[I915_NUM_ENGINES] = {
		[RCS] = GEN11_GRDOM_RENDER,
		[BCS] = GEN11_GRDOM_BLT,
		[VCS] = GEN11_GRDOM_MEDIA,
		[VCS2] = GEN11_GRDOM_MEDIA2,
		[VCS3] = GEN11_GRDOM_MEDIA3,
		[VCS4] = GEN11_GRDOM_MEDIA4,
		[VECS] = GEN11_GRDOM_VECS,
		[VECS2] = GEN11_GRDOM_VECS2,
	};
	struct intel_engine_cs *engine;
	unsigned int tmp;
	u32 hw_mask;
	int ret;

	BUILD_BUG_ON(VECS2 + 1 != I915_NUM_ENGINES);

	if (engine_mask == ALL_ENGINES) {
		hw_mask = GEN11_GRDOM_FULL;
	} else {
		hw_mask = 0;
		for_each_engine_masked(engine, i915, engine_mask, tmp) {
			hw_mask |= hw_engine_mask[engine->id];
			hw_mask |= gen11_lock_sfc(i915, engine);
		}
	}

	ret = gen6_hw_domain_reset(i915, hw_mask);

	if (engine_mask != ALL_ENGINES)
		for_each_engine_masked(engine, i915, engine_mask, tmp)
			gen11_unlock_sfc(i915, engine);

	return ret;
}

static int gen8_engine_reset_prepare(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	int ret;

	I915_WRITE_FW(RING_RESET_CTL(engine->mmio_base),
		      _MASKED_BIT_ENABLE(RESET_CTL_REQUEST_RESET));

	ret = __intel_wait_for_register_fw(dev_priv,
					   RING_RESET_CTL(engine->mmio_base),
					   RESET_CTL_READY_TO_RESET,
					   RESET_CTL_READY_TO_RESET,
					   700, 0,
					   NULL);
	if (ret)
		DRM_ERROR("%s: reset request timeout\n", engine->name);

	return ret;
}

static void gen8_engine_reset_cancel(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;

	I915_WRITE_FW(RING_RESET_CTL(engine->mmio_base),
		      _MASKED_BIT_DISABLE(RESET_CTL_REQUEST_RESET));
}

static int gen8_reset_engines(struct drm_i915_private *i915,
			      unsigned int engine_mask,
			      unsigned int retry)
{
	struct intel_engine_cs *engine;
	const bool reset_non_ready = retry >= 1;
	unsigned int tmp;
	int ret;

	for_each_engine_masked(engine, i915, engine_mask, tmp) {
		ret = gen8_engine_reset_prepare(engine);
		if (ret && !reset_non_ready)
			goto skip_reset;

		/*
		 * If this is not the first failed attempt to prepare,
		 * we decide to proceed anyway.
		 *
		 * By doing so we risk context corruption and with
		 * some gens (kbl), possible system hang if reset
		 * happens during active bb execution.
		 *
		 * We rather take context corruption instead of
		 * failed reset with a wedged driver/gpu. And
		 * active bb execution case should be covered by
		 * i915_stop_engines we have before the reset.
		 */
	}

	if (INTEL_GEN(i915) >= 11)
		ret = gen11_reset_engines(i915, engine_mask, retry);
	else
		ret = gen6_reset_engines(i915, engine_mask, retry);

skip_reset:
	for_each_engine_masked(engine, i915, engine_mask, tmp)
		gen8_engine_reset_cancel(engine);

	return ret;
}

typedef int (*reset_func)(struct drm_i915_private *,
			  unsigned int engine_mask,
			  unsigned int retry);

static reset_func intel_get_gpu_reset(struct drm_i915_private *i915)
{
	if (!i915_modparams.reset)
		return NULL;

	if (INTEL_GEN(i915) >= 8)
		return gen8_reset_engines;
	else if (INTEL_GEN(i915) >= 6)
		return gen6_reset_engines;
	else if (INTEL_GEN(i915) >= 5)
		return ironlake_do_reset;
	else if (IS_G4X(i915))
		return g4x_do_reset;
	else if (IS_G33(i915) || IS_PINEVIEW(i915))
		return g33_do_reset;
	else if (INTEL_GEN(i915) >= 3)
		return i915_do_reset;
	else
		return NULL;
}

int intel_gpu_reset(struct drm_i915_private *i915, unsigned int engine_mask)
{
	reset_func reset = intel_get_gpu_reset(i915);
	int retry;
	int ret;

	/*
	 * We want to perform per-engine reset from atomic context (e.g.
	 * softirq), which imposes the constraint that we cannot sleep.
	 * However, experience suggests that spending a bit of time waiting
	 * for a reset helps in various cases, so for a full-device reset
	 * we apply the opposite rule and wait if we want to. As we should
	 * always follow up a failed per-engine reset with a full device reset,
	 * being a little faster, stricter and more error prone for the
	 * atomic case seems an acceptable compromise.
	 *
	 * Unfortunately this leads to a bimodal routine, when the goal was
	 * to have a single reset function that worked for resetting any
	 * number of engines simultaneously.
	 */
	might_sleep_if(engine_mask == ALL_ENGINES);

	/*
	 * If the power well sleeps during the reset, the reset
	 * request may be dropped and never completes (causing -EIO).
	 */
	intel_uncore_forcewake_get(i915, FORCEWAKE_ALL);
	for (retry = 0; retry < 3; retry++) {
		/*
		 * We stop engines, otherwise we might get failed reset and a
		 * dead gpu (on elk). Also as modern gpu as kbl can suffer
		 * from system hang if batchbuffer is progressing when
		 * the reset is issued, regardless of READY_TO_RESET ack.
		 * Thus assume it is best to stop engines on all gens
		 * where we have a gpu reset.
		 *
		 * WaKBLVECSSemaphoreWaitPoll:kbl (on ALL_ENGINES)
		 *
		 * WaMediaResetMainRingCleanup:ctg,elk (presumably)
		 *
		 * FIXME: Wa for more modern gens needs to be validated
		 */
		i915_stop_engines(i915, engine_mask);

		ret = -ENODEV;
		if (reset) {
			GEM_TRACE("engine_mask=%x\n", engine_mask);
			ret = reset(i915, engine_mask, retry);
		}
		if (ret != -ETIMEDOUT || engine_mask != ALL_ENGINES)
			break;

		cond_resched();
	}
	intel_uncore_forcewake_put(i915, FORCEWAKE_ALL);

	return ret;
}

bool intel_has_gpu_reset(struct drm_i915_private *i915)
{
	return intel_get_gpu_reset(i915);
}

bool intel_has_reset_engine(struct drm_i915_private *i915)
{
	return INTEL_INFO(i915)->has_reset_engine && i915_modparams.reset >= 2;
}

int intel_reset_guc(struct drm_i915_private *i915)
{
	u32 guc_domain =
		INTEL_GEN(i915) >= 11 ? GEN11_GRDOM_GUC : GEN9_GRDOM_GUC;
	int ret;

	GEM_BUG_ON(!HAS_GUC(i915));

	intel_uncore_forcewake_get(i915, FORCEWAKE_ALL);
	ret = gen6_hw_domain_reset(i915, guc_domain);
	intel_uncore_forcewake_put(i915, FORCEWAKE_ALL);

	return ret;
}

/*
 * Ensure irq handler finishes, and not run again.
 * Also return the active request so that we only search for it once.
 */
static struct i915_request *
reset_prepare_engine(struct intel_engine_cs *engine)
{
	struct i915_request *rq;

	/*
	 * During the reset sequence, we must prevent the engine from
	 * entering RC6. As the context state is undefined until we restart
	 * the engine, if it does enter RC6 during the reset, the state
	 * written to the powercontext is undefined and so we may lose
	 * GPU state upon resume, i.e. fail to restart after a reset.
	 */
	intel_uncore_forcewake_get(engine->i915, FORCEWAKE_ALL);

	rq = engine->reset.prepare(engine);
	if (rq && rq->fence.error == -EIO)
		rq = ERR_PTR(-EIO); /* Previous reset failed! */

	return rq;
}

static int reset_prepare(struct drm_i915_private *i915)
{
	struct intel_engine_cs *engine;
	struct i915_request *rq;
	enum intel_engine_id id;
	int err = 0;

	for_each_engine(engine, i915, id) {
		rq = reset_prepare_engine(engine);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			continue;
		}

		engine->hangcheck.active_request = rq;
	}

	i915_gem_revoke_fences(i915);
	intel_uc_sanitize(i915);

	return err;
}

/* Returns the request if it was guilty of the hang */
static struct i915_request *
reset_request(struct intel_engine_cs *engine,
	      struct i915_request *rq,
	      bool stalled)
{
	/*
	 * The guilty request will get skipped on a hung engine.
	 *
	 * Users of client default contexts do not rely on logical
	 * state preserved between batches so it is safe to execute
	 * queued requests following the hang. Non default contexts
	 * rely on preserved state, so skipping a batch loses the
	 * evolution of the state and it needs to be considered corrupted.
	 * Executing more queued batches on top of corrupted state is
	 * risky. But we take the risk by trying to advance through
	 * the queued requests in order to make the client behaviour
	 * more predictable around resets, by not throwing away random
	 * amount of batches it has prepared for execution. Sophisticated
	 * clients can use gem_reset_stats_ioctl and dma fence status
	 * (exported via sync_file info ioctl on explicit fences) to observe
	 * when it loses the context state and should rebuild accordingly.
	 *
	 * The context ban, and ultimately the client ban, mechanism are safety
	 * valves if client submission ends up resulting in nothing more than
	 * subsequent hangs.
	 */

	if (i915_request_completed(rq)) {
		GEM_TRACE("%s pardoned global=%d (fence %llx:%lld), current %d\n",
			  engine->name, rq->global_seqno,
			  rq->fence.context, rq->fence.seqno,
			  intel_engine_get_seqno(engine));
		stalled = false;
	}

	if (stalled) {
		context_mark_guilty(rq->gem_context);
		i915_request_skip(rq, -EIO);

		/* If this context is now banned, skip all pending requests. */
		if (i915_gem_context_is_banned(rq->gem_context))
			engine_skip_context(rq);
	} else {
		/*
		 * Since this is not the hung engine, it may have advanced
		 * since the hang declaration. Double check by refinding
		 * the active request at the time of the reset.
		 */
		rq = i915_gem_find_active_request(engine);
		if (rq) {
			unsigned long flags;

			context_mark_innocent(rq->gem_context);
			dma_fence_set_error(&rq->fence, -EAGAIN);

			/* Rewind the engine to replay the incomplete rq */
			spin_lock_irqsave(&engine->timeline.lock, flags);
			rq = list_prev_entry(rq, link);
			if (&rq->link == &engine->timeline.requests)
				rq = NULL;
			spin_unlock_irqrestore(&engine->timeline.lock, flags);
		}
	}

	return rq;
}

static void reset_engine(struct intel_engine_cs *engine,
			 struct i915_request *rq,
			 bool stalled)
{
	if (rq)
		rq = reset_request(engine, rq, stalled);

	/* Setup the CS to resume from the breadcrumb of the hung request */
	engine->reset.reset(engine, rq);
}

static void gt_reset(struct drm_i915_private *i915, unsigned int stalled_mask)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	lockdep_assert_held(&i915->drm.struct_mutex);

	i915_retire_requests(i915);

	for_each_engine(engine, i915, id) {
		struct intel_context *ce;

		reset_engine(engine,
			     engine->hangcheck.active_request,
			     stalled_mask & ENGINE_MASK(id));
		ce = fetch_and_zero(&engine->last_retired_context);
		if (ce)
			intel_context_unpin(ce);

		/*
		 * Ostensibily, we always want a context loaded for powersaving,
		 * so if the engine is idle after the reset, send a request
		 * to load our scratch kernel_context.
		 *
		 * More mysteriously, if we leave the engine idle after a reset,
		 * the next userspace batch may hang, with what appears to be
		 * an incoherent read by the CS (presumably stale TLB). An
		 * empty request appears sufficient to paper over the glitch.
		 */
		if (intel_engine_is_idle(engine)) {
			struct i915_request *rq;

			rq = i915_request_alloc(engine, i915->kernel_context);
			if (!IS_ERR(rq))
				i915_request_add(rq);
		}
	}

	i915_gem_restore_fences(i915);
}

static void reset_finish_engine(struct intel_engine_cs *engine)
{
	engine->reset.finish(engine);

	intel_uncore_forcewake_put(engine->i915, FORCEWAKE_ALL);
}

static void reset_finish(struct drm_i915_private *i915)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	lockdep_assert_held(&i915->drm.struct_mutex);

	for_each_engine(engine, i915, id) {
		engine->hangcheck.active_request = NULL;
		reset_finish_engine(engine);
	}
}

static void nop_submit_request(struct i915_request *request)
{
	unsigned long flags;

	GEM_TRACE("%s fence %llx:%lld -> -EIO\n",
		  request->engine->name,
		  request->fence.context, request->fence.seqno);
	dma_fence_set_error(&request->fence, -EIO);

	spin_lock_irqsave(&request->engine->timeline.lock, flags);
	__i915_request_submit(request);
	intel_engine_write_global_seqno(request->engine, request->global_seqno);
	spin_unlock_irqrestore(&request->engine->timeline.lock, flags);
}

void i915_gem_set_wedged(struct drm_i915_private *i915)
{
	struct i915_gpu_error *error = &i915->gpu_error;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	mutex_lock(&error->wedge_mutex);
	if (test_bit(I915_WEDGED, &error->flags)) {
		mutex_unlock(&error->wedge_mutex);
		return;
	}

	if (GEM_SHOW_DEBUG() && !intel_engines_are_idle(i915)) {
		struct drm_printer p = drm_debug_printer(__func__);

		for_each_engine(engine, i915, id)
			intel_engine_dump(engine, &p, "%s\n", engine->name);
	}

	GEM_TRACE("start\n");

	/*
	 * First, stop submission to hw, but do not yet complete requests by
	 * rolling the global seqno forward (since this would complete requests
	 * for which we haven't set the fence error to EIO yet).
	 */
	for_each_engine(engine, i915, id)
		reset_prepare_engine(engine);

	/* Even if the GPU reset fails, it should still stop the engines */
	if (INTEL_GEN(i915) >= 5)
		intel_gpu_reset(i915, ALL_ENGINES);

	for_each_engine(engine, i915, id) {
		engine->submit_request = nop_submit_request;
		engine->schedule = NULL;
	}
	i915->caps.scheduler = 0;

	/*
	 * Make sure no request can slip through without getting completed by
	 * either this call here to intel_engine_write_global_seqno, or the one
	 * in nop_submit_request.
	 */
	synchronize_rcu();

	/* Mark all executing requests as skipped */
	for_each_engine(engine, i915, id)
		engine->cancel_requests(engine);

	for_each_engine(engine, i915, id) {
		reset_finish_engine(engine);
		intel_engine_wakeup(engine);
	}

	smp_mb__before_atomic();
	set_bit(I915_WEDGED, &error->flags);

	GEM_TRACE("end\n");
	mutex_unlock(&error->wedge_mutex);

	wake_up_all(&error->reset_queue);
}

bool i915_gem_unset_wedged(struct drm_i915_private *i915)
{
	struct i915_gpu_error *error = &i915->gpu_error;
	struct i915_timeline *tl;
	bool ret = false;

	lockdep_assert_held(&i915->drm.struct_mutex);

	if (!test_bit(I915_WEDGED, &error->flags))
		return true;

	if (!i915->gt.scratch) /* Never full initialised, recovery impossible */
		return false;

	mutex_lock(&error->wedge_mutex);

	GEM_TRACE("start\n");

	/*
	 * Before unwedging, make sure that all pending operations
	 * are flushed and errored out - we may have requests waiting upon
	 * third party fences. We marked all inflight requests as EIO, and
	 * every execbuf since returned EIO, for consistency we want all
	 * the currently pending requests to also be marked as EIO, which
	 * is done inside our nop_submit_request - and so we must wait.
	 *
	 * No more can be submitted until we reset the wedged bit.
	 */
	list_for_each_entry(tl, &i915->gt.timelines, link) {
		struct i915_request *rq;

		rq = i915_gem_active_peek(&tl->last_request,
					  &i915->drm.struct_mutex);
		if (!rq)
			continue;

		/*
		 * We can't use our normal waiter as we want to
		 * avoid recursively trying to handle the current
		 * reset. The basic dma_fence_default_wait() installs
		 * a callback for dma_fence_signal(), which is
		 * triggered by our nop handler (indirectly, the
		 * callback enables the signaler thread which is
		 * woken by the nop_submit_request() advancing the seqno
		 * and when the seqno passes the fence, the signaler
		 * then signals the fence waking us up).
		 */
		if (dma_fence_default_wait(&rq->fence, true,
					   MAX_SCHEDULE_TIMEOUT) < 0)
			goto unlock;
	}
	i915_retire_requests(i915);
	GEM_BUG_ON(i915->gt.active_requests);

	intel_engines_sanitize(i915, false);

	/*
	 * Undo nop_submit_request. We prevent all new i915 requests from
	 * being queued (by disallowing execbuf whilst wedged) so having
	 * waited for all active requests above, we know the system is idle
	 * and do not have to worry about a thread being inside
	 * engine->submit_request() as we swap over. So unlike installing
	 * the nop_submit_request on reset, we can do this from normal
	 * context and do not require stop_machine().
	 */
	intel_engines_reset_default_submission(i915);
	i915_gem_contexts_lost(i915);

	GEM_TRACE("end\n");

	smp_mb__before_atomic(); /* complete takeover before enabling execbuf */
	clear_bit(I915_WEDGED, &i915->gpu_error.flags);
	ret = true;
unlock:
	mutex_unlock(&i915->gpu_error.wedge_mutex);

	return ret;
}

/**
 * i915_reset - reset chip after a hang
 * @i915: #drm_i915_private to reset
 * @stalled_mask: mask of the stalled engines with the guilty requests
 * @reason: user error message for why we are resetting
 *
 * Reset the chip.  Useful if a hang is detected. Marks the device as wedged
 * on failure.
 *
 * Caller must hold the struct_mutex.
 *
 * Procedure is fairly simple:
 *   - reset the chip using the reset reg
 *   - re-init context state
 *   - re-init hardware status page
 *   - re-init ring buffer
 *   - re-init interrupt state
 *   - re-init display
 */
void i915_reset(struct drm_i915_private *i915,
		unsigned int stalled_mask,
		const char *reason)
{
	struct i915_gpu_error *error = &i915->gpu_error;
	int ret;
	int i;

	GEM_TRACE("flags=%lx\n", error->flags);

	might_sleep();
	lockdep_assert_held(&i915->drm.struct_mutex);
	assert_rpm_wakelock_held(i915);
	GEM_BUG_ON(!test_bit(I915_RESET_BACKOFF, &error->flags));

	if (!test_bit(I915_RESET_HANDOFF, &error->flags))
		return;

	/* Clear any previous failed attempts at recovery. Time to try again. */
	if (!i915_gem_unset_wedged(i915))
		goto wakeup;

	if (reason)
		dev_notice(i915->drm.dev, "Resetting chip for %s\n", reason);
	error->reset_count++;

	ret = reset_prepare(i915);
	if (ret) {
		dev_err(i915->drm.dev, "GPU recovery failed\n");
		goto taint;
	}

	if (!intel_has_gpu_reset(i915)) {
		if (i915_modparams.reset)
			dev_err(i915->drm.dev, "GPU reset not supported\n");
		else
			DRM_DEBUG_DRIVER("GPU reset disabled\n");
		goto error;
	}

	for (i = 0; i < 3; i++) {
		ret = intel_gpu_reset(i915, ALL_ENGINES);
		if (ret == 0)
			break;

		msleep(100);
	}
	if (ret) {
		dev_err(i915->drm.dev, "Failed to reset chip\n");
		goto taint;
	}

	/* Ok, now get things going again... */

	/*
	 * Everything depends on having the GTT running, so we need to start
	 * there.
	 */
	ret = i915_ggtt_enable_hw(i915);
	if (ret) {
		DRM_ERROR("Failed to re-enable GGTT following reset (%d)\n",
			  ret);
		goto error;
	}

	gt_reset(i915, stalled_mask);
	intel_overlay_reset(i915);

	/*
	 * Next we need to restore the context, but we don't use those
	 * yet either...
	 *
	 * Ring buffer needs to be re-initialized in the KMS case, or if X
	 * was running at the time of the reset (i.e. we weren't VT
	 * switched away).
	 */
	ret = i915_gem_init_hw(i915);
	if (ret) {
		DRM_ERROR("Failed to initialise HW following reset (%d)\n",
			  ret);
		goto error;
	}

	i915_queue_hangcheck(i915);

finish:
	reset_finish(i915);
wakeup:
	clear_bit(I915_RESET_HANDOFF, &error->flags);
	wake_up_bit(&error->flags, I915_RESET_HANDOFF);
	return;

taint:
	/*
	 * History tells us that if we cannot reset the GPU now, we
	 * never will. This then impacts everything that is run
	 * subsequently. On failing the reset, we mark the driver
	 * as wedged, preventing further execution on the GPU.
	 * We also want to go one step further and add a taint to the
	 * kernel so that any subsequent faults can be traced back to
	 * this failure. This is important for CI, where if the
	 * GPU/driver fails we would like to reboot and restart testing
	 * rather than continue on into oblivion. For everyone else,
	 * the system should still plod along, but they have been warned!
	 */
	add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
error:
	i915_gem_set_wedged(i915);
	i915_retire_requests(i915);
	goto finish;
}

static inline int intel_gt_reset_engine(struct drm_i915_private *i915,
					struct intel_engine_cs *engine)
{
	return intel_gpu_reset(i915, intel_engine_flag(engine));
}

/**
 * i915_reset_engine - reset GPU engine to recover from a hang
 * @engine: engine to reset
 * @msg: reason for GPU reset; or NULL for no dev_notice()
 *
 * Reset a specific GPU engine. Useful if a hang is detected.
 * Returns zero on successful reset or otherwise an error code.
 *
 * Procedure is:
 *  - identifies the request that caused the hang and it is dropped
 *  - reset engine (which will force the engine to idle)
 *  - re-init/configure engine
 */
int i915_reset_engine(struct intel_engine_cs *engine, const char *msg)
{
	struct i915_gpu_error *error = &engine->i915->gpu_error;
	struct i915_request *active_request;
	int ret;

	GEM_TRACE("%s flags=%lx\n", engine->name, error->flags);
	GEM_BUG_ON(!test_bit(I915_RESET_ENGINE + engine->id, &error->flags));

	active_request = reset_prepare_engine(engine);
	if (IS_ERR_OR_NULL(active_request)) {
		/* Either the previous reset failed, or we pardon the reset. */
		ret = PTR_ERR(active_request);
		goto out;
	}

	if (msg)
		dev_notice(engine->i915->drm.dev,
			   "Resetting %s for %s\n", engine->name, msg);
	error->reset_engine_count[engine->id]++;

	if (!engine->i915->guc.execbuf_client)
		ret = intel_gt_reset_engine(engine->i915, engine);
	else
		ret = intel_guc_reset_engine(&engine->i915->guc, engine);
	if (ret) {
		/* If we fail here, we expect to fallback to a global reset */
		DRM_DEBUG_DRIVER("%sFailed to reset %s, ret=%d\n",
				 engine->i915->guc.execbuf_client ? "GuC " : "",
				 engine->name, ret);
		goto out;
	}

	/*
	 * The request that caused the hang is stuck on elsp, we know the
	 * active request and can drop it, adjust head to skip the offending
	 * request to resume executing remaining requests in the queue.
	 */
	reset_engine(engine, active_request, true);

	/*
	 * The engine and its registers (and workarounds in case of render)
	 * have been reset to their default values. Follow the init_ring
	 * process to program RING_MODE, HWSP and re-enable submission.
	 */
	ret = engine->init_hw(engine);
	if (ret)
		goto out;

out:
	intel_engine_cancel_stop_cs(engine);
	reset_finish_engine(engine);
	return ret;
}

static void i915_reset_device(struct drm_i915_private *i915,
			      u32 engine_mask,
			      const char *reason)
{
	struct i915_gpu_error *error = &i915->gpu_error;
	struct kobject *kobj = &i915->drm.primary->kdev->kobj;
	char *error_event[] = { I915_ERROR_UEVENT "=1", NULL };
	char *reset_event[] = { I915_RESET_UEVENT "=1", NULL };
	char *reset_done_event[] = { I915_ERROR_UEVENT "=0", NULL };
	struct i915_wedge_me w;

	kobject_uevent_env(kobj, KOBJ_CHANGE, error_event);

	DRM_DEBUG_DRIVER("resetting chip\n");
	kobject_uevent_env(kobj, KOBJ_CHANGE, reset_event);

	/* Use a watchdog to ensure that our reset completes */
	i915_wedge_on_timeout(&w, i915, 5 * HZ) {
		intel_prepare_reset(i915);

		error->reason = reason;
		error->stalled_mask = engine_mask;

		/* Signal that locked waiters should reset the GPU */
		smp_mb__before_atomic();
		set_bit(I915_RESET_HANDOFF, &error->flags);
		wake_up_all(&error->wait_queue);

		/*
		 * Wait for anyone holding the lock to wakeup, without
		 * blocking indefinitely on struct_mutex.
		 */
		do {
			if (mutex_trylock(&i915->drm.struct_mutex)) {
				i915_reset(i915, engine_mask, reason);
				mutex_unlock(&i915->drm.struct_mutex);
			}
		} while (wait_on_bit_timeout(&error->flags,
					     I915_RESET_HANDOFF,
					     TASK_UNINTERRUPTIBLE,
					     1));

		error->stalled_mask = 0;
		error->reason = NULL;

		intel_finish_reset(i915);
	}

	if (!test_bit(I915_WEDGED, &error->flags))
		kobject_uevent_env(kobj, KOBJ_CHANGE, reset_done_event);
}

void i915_clear_error_registers(struct drm_i915_private *dev_priv)
{
	u32 eir;

	if (!IS_GEN(dev_priv, 2))
		I915_WRITE(PGTBL_ER, I915_READ(PGTBL_ER));

	if (INTEL_GEN(dev_priv) < 4)
		I915_WRITE(IPEIR, I915_READ(IPEIR));
	else
		I915_WRITE(IPEIR_I965, I915_READ(IPEIR_I965));

	I915_WRITE(EIR, I915_READ(EIR));
	eir = I915_READ(EIR);
	if (eir) {
		/*
		 * some errors might have become stuck,
		 * mask them.
		 */
		DRM_DEBUG_DRIVER("EIR stuck: 0x%08x, masking\n", eir);
		I915_WRITE(EMR, I915_READ(EMR) | eir);
		I915_WRITE(IIR, I915_MASTER_ERROR_INTERRUPT);
	}

	if (INTEL_GEN(dev_priv) >= 8) {
		I915_WRITE(GEN8_RING_FAULT_REG,
			   I915_READ(GEN8_RING_FAULT_REG) & ~RING_FAULT_VALID);
		POSTING_READ(GEN8_RING_FAULT_REG);
	} else if (INTEL_GEN(dev_priv) >= 6) {
		struct intel_engine_cs *engine;
		enum intel_engine_id id;

		for_each_engine(engine, dev_priv, id) {
			I915_WRITE(RING_FAULT_REG(engine),
				   I915_READ(RING_FAULT_REG(engine)) &
				   ~RING_FAULT_VALID);
		}
		POSTING_READ(RING_FAULT_REG(dev_priv->engine[RCS]));
	}
}

/**
 * i915_handle_error - handle a gpu error
 * @i915: i915 device private
 * @engine_mask: mask representing engines that are hung
 * @flags: control flags
 * @fmt: Error message format string
 *
 * Do some basic checking of register state at error time and
 * dump it to the syslog.  Also call i915_capture_error_state() to make
 * sure we get a record and make it available in debugfs.  Fire a uevent
 * so userspace knows something bad happened (should trigger collection
 * of a ring dump etc.).
 */
void i915_handle_error(struct drm_i915_private *i915,
		       u32 engine_mask,
		       unsigned long flags,
		       const char *fmt, ...)
{
	struct intel_engine_cs *engine;
	intel_wakeref_t wakeref;
	unsigned int tmp;
	char error_msg[80];
	char *msg = NULL;

	if (fmt) {
		va_list args;

		va_start(args, fmt);
		vscnprintf(error_msg, sizeof(error_msg), fmt, args);
		va_end(args);

		msg = error_msg;
	}

	/*
	 * In most cases it's guaranteed that we get here with an RPM
	 * reference held, for example because there is a pending GPU
	 * request that won't finish until the reset is done. This
	 * isn't the case at least when we get here by doing a
	 * simulated reset via debugfs, so get an RPM reference.
	 */
	wakeref = intel_runtime_pm_get(i915);

	engine_mask &= INTEL_INFO(i915)->ring_mask;

	if (flags & I915_ERROR_CAPTURE) {
		i915_capture_error_state(i915, engine_mask, msg);
		i915_clear_error_registers(i915);
	}

	/*
	 * Try engine reset when available. We fall back to full reset if
	 * single reset fails.
	 */
	if (intel_has_reset_engine(i915) &&
	    !i915_terminally_wedged(&i915->gpu_error)) {
		for_each_engine_masked(engine, i915, engine_mask, tmp) {
			BUILD_BUG_ON(I915_RESET_MODESET >= I915_RESET_ENGINE);
			if (test_and_set_bit(I915_RESET_ENGINE + engine->id,
					     &i915->gpu_error.flags))
				continue;

			if (i915_reset_engine(engine, msg) == 0)
				engine_mask &= ~intel_engine_flag(engine);

			clear_bit(I915_RESET_ENGINE + engine->id,
				  &i915->gpu_error.flags);
			wake_up_bit(&i915->gpu_error.flags,
				    I915_RESET_ENGINE + engine->id);
		}
	}

	if (!engine_mask)
		goto out;

	/* Full reset needs the mutex, stop any other user trying to do so. */
	if (test_and_set_bit(I915_RESET_BACKOFF, &i915->gpu_error.flags)) {
		wait_event(i915->gpu_error.reset_queue,
			   !test_bit(I915_RESET_BACKOFF,
				     &i915->gpu_error.flags));
		goto out;
	}

	/* Prevent any other reset-engine attempt. */
	for_each_engine(engine, i915, tmp) {
		while (test_and_set_bit(I915_RESET_ENGINE + engine->id,
					&i915->gpu_error.flags))
			wait_on_bit(&i915->gpu_error.flags,
				    I915_RESET_ENGINE + engine->id,
				    TASK_UNINTERRUPTIBLE);
	}

	i915_reset_device(i915, engine_mask, msg);

	for_each_engine(engine, i915, tmp) {
		clear_bit(I915_RESET_ENGINE + engine->id,
			  &i915->gpu_error.flags);
	}

	clear_bit(I915_RESET_BACKOFF, &i915->gpu_error.flags);
	wake_up_all(&i915->gpu_error.reset_queue);

out:
	intel_runtime_pm_put(i915, wakeref);
}

static void i915_wedge_me(struct work_struct *work)
{
	struct i915_wedge_me *w = container_of(work, typeof(*w), work.work);

	dev_err(w->i915->drm.dev,
		"%s timed out, cancelling all in-flight rendering.\n",
		w->name);
	i915_gem_set_wedged(w->i915);
}

void __i915_init_wedge(struct i915_wedge_me *w,
		       struct drm_i915_private *i915,
		       long timeout,
		       const char *name)
{
	w->i915 = i915;
	w->name = name;

	INIT_DELAYED_WORK_ONSTACK(&w->work, i915_wedge_me);
	schedule_delayed_work(&w->work, timeout);
}

void __i915_fini_wedge(struct i915_wedge_me *w)
{
	cancel_delayed_work_sync(&w->work);
	destroy_delayed_work_on_stack(&w->work);
	w->i915 = NULL;
}
