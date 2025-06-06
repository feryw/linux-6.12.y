// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "xe_devcoredump.h"
#include "xe_devcoredump_types.h"

#include <linux/ascii85.h>
#include <linux/devcoredump.h>
#include <generated/utsrelease.h>

#include <drm/drm_managed.h>

#include "xe_device.h"
#include "xe_exec_queue.h"
#include "xe_force_wake.h"
#include "xe_gt.h"
#include "xe_gt_printk.h"
#include "xe_guc_ct.h"
#include "xe_guc_submit.h"
#include "xe_hw_engine.h"
#include "xe_pm.h"
#include "xe_sched_job.h"
#include "xe_vm.h"

/**
 * DOC: Xe device coredump
 *
 * Devices overview:
 * Xe uses dev_coredump infrastructure for exposing the crash errors in a
 * standardized way.
 * devcoredump exposes a temporary device under /sys/class/devcoredump/
 * which is linked with our card device directly.
 * The core dump can be accessed either from
 * /sys/class/drm/card<n>/device/devcoredump/ or from
 * /sys/class/devcoredump/devcd<m> where
 * /sys/class/devcoredump/devcd<m>/failing_device is a link to
 * /sys/class/drm/card<n>/device/.
 *
 * Snapshot at hang:
 * The 'data' file is printed with a drm_printer pointer at devcoredump read
 * time. For this reason, we need to take snapshots from when the hang has
 * happened, and not only when the user is reading the file. Otherwise the
 * information is outdated since the resets might have happened in between.
 *
 * 'First' failure snapshot:
 * In general, the first hang is the most critical one since the following hangs
 * can be a consequence of the initial hang. For this reason we only take the
 * snapshot of the 'first' failure and ignore subsequent calls of this function,
 * at least while the coredump device is alive. Dev_coredump has a delayed work
 * queue that will eventually delete the device and free all the dump
 * information.
 */

#ifdef CONFIG_DEV_COREDUMP

/* 1 hour timeout */
#define XE_COREDUMP_TIMEOUT_JIFFIES (60 * 60 * HZ)

static struct xe_device *coredump_to_xe(const struct xe_devcoredump *coredump)
{
	return container_of(coredump, struct xe_device, devcoredump);
}

static struct xe_guc *exec_queue_to_guc(struct xe_exec_queue *q)
{
	return &q->gt->uc.guc;
}

static ssize_t __xe_devcoredump_read(char *buffer, size_t count,
				     struct xe_devcoredump *coredump)
{
	struct xe_device *xe;
	struct xe_devcoredump_snapshot *ss;
	struct drm_printer p;
	struct drm_print_iterator iter;
	struct timespec64 ts;
	int i;

	xe = coredump_to_xe(coredump);
	ss = &coredump->snapshot;

	iter.data = buffer;
	iter.start = 0;
	iter.remain = count;

	p = drm_coredump_printer(&iter);

	drm_puts(&p, "**** Xe Device Coredump ****\n");
	drm_puts(&p, "kernel: " UTS_RELEASE "\n");
	drm_puts(&p, "module: " KBUILD_MODNAME "\n");

	ts = ktime_to_timespec64(ss->snapshot_time);
	drm_printf(&p, "Snapshot time: %lld.%09ld\n", ts.tv_sec, ts.tv_nsec);
	ts = ktime_to_timespec64(ss->boot_time);
	drm_printf(&p, "Uptime: %lld.%09ld\n", ts.tv_sec, ts.tv_nsec);
	drm_printf(&p, "Process: %s\n", ss->process_name);
	xe_device_snapshot_print(xe, &p);

	drm_printf(&p, "\n**** GT #%d ****\n", ss->gt->info.id);
	drm_printf(&p, "\tTile: %d\n", ss->gt->tile->id);

	drm_puts(&p, "\n**** GuC CT ****\n");
	xe_guc_ct_snapshot_print(ss->ct, &p);

	drm_puts(&p, "\n**** Contexts ****\n");
	xe_guc_exec_queue_snapshot_print(ss->ge, &p);

	drm_puts(&p, "\n**** Job ****\n");
	xe_sched_job_snapshot_print(ss->job, &p);

	drm_puts(&p, "\n**** HW Engines ****\n");
	for (i = 0; i < XE_NUM_HW_ENGINES; i++)
		if (ss->hwe[i])
			xe_hw_engine_snapshot_print(ss->hwe[i], &p);

	drm_puts(&p, "\n**** VM state ****\n");
	xe_vm_snapshot_print(ss->vm, &p);

	return count - iter.remain;
}

static void xe_devcoredump_snapshot_free(struct xe_devcoredump_snapshot *ss)
{
	int i;

	xe_guc_ct_snapshot_free(ss->ct);
	ss->ct = NULL;

	xe_guc_exec_queue_snapshot_free(ss->ge);
	ss->ge = NULL;

	xe_sched_job_snapshot_free(ss->job);
	ss->job = NULL;

	for (i = 0; i < XE_NUM_HW_ENGINES; i++)
		if (ss->hwe[i]) {
			xe_hw_engine_snapshot_free(ss->hwe[i]);
			ss->hwe[i] = NULL;
		}

	xe_vm_snapshot_free(ss->vm);
	ss->vm = NULL;
}

static ssize_t xe_devcoredump_read(char *buffer, loff_t offset,
				   size_t count, void *data, size_t datalen)
{
	struct xe_devcoredump *coredump = data;
	struct xe_devcoredump_snapshot *ss;
	ssize_t byte_copied;

	if (!coredump)
		return -ENODEV;

	ss = &coredump->snapshot;

	/* Ensure delayed work is captured before continuing */
	flush_work(&ss->work);

	if (!ss->read.buffer)
		return -ENODEV;

	if (offset >= ss->read.size)
		return 0;

	byte_copied = count < ss->read.size - offset ? count :
		ss->read.size - offset;
	memcpy(buffer, ss->read.buffer + offset, byte_copied);

	return byte_copied;
}

static void xe_devcoredump_free(void *data)
{
	struct xe_devcoredump *coredump = data;

	/* Our device is gone. Nothing to do... */
	if (!data || !coredump_to_xe(coredump))
		return;

	cancel_work_sync(&coredump->snapshot.work);

	xe_devcoredump_snapshot_free(&coredump->snapshot);
	kvfree(coredump->snapshot.read.buffer);

	/* To prevent stale data on next snapshot, clear everything */
	memset(&coredump->snapshot, 0, sizeof(coredump->snapshot));
	coredump->captured = false;
	drm_info(&coredump_to_xe(coredump)->drm,
		 "Xe device coredump has been deleted.\n");
}

static void xe_devcoredump_deferred_snap_work(struct work_struct *work)
{
	struct xe_devcoredump_snapshot *ss = container_of(work, typeof(*ss), work);
	struct xe_devcoredump *coredump = container_of(ss, typeof(*coredump), snapshot);
	struct xe_device *xe = coredump_to_xe(coredump);
	unsigned int fw_ref;

	/*
	 * NB: Despite passing a GFP_ flags parameter here, more allocations are done
	 * internally using GFP_KERNEL expliictly. Hence this call must be in the worker
	 * thread and not in the initial capture call.
	 */
	dev_coredumpm_timeout(gt_to_xe(ss->gt)->drm.dev, THIS_MODULE, coredump, 0, GFP_KERNEL,
			      xe_devcoredump_read, xe_devcoredump_free,
			      XE_COREDUMP_TIMEOUT_JIFFIES);

	xe_pm_runtime_get(xe);

	/* keep going if fw fails as we still want to save the memory and SW data */
	fw_ref = xe_force_wake_get(gt_to_fw(ss->gt), XE_FORCEWAKE_ALL);
	if (!xe_force_wake_ref_has_domain(fw_ref, XE_FORCEWAKE_ALL))
		xe_gt_info(ss->gt, "failed to get forcewake for coredump capture\n");
	xe_vm_snapshot_capture_delayed(ss->vm);
	xe_guc_exec_queue_snapshot_capture_delayed(ss->ge);
	xe_force_wake_put(gt_to_fw(ss->gt), fw_ref);

	xe_pm_runtime_put(xe);

	/* Calculate devcoredump size */
	ss->read.size = __xe_devcoredump_read(NULL, INT_MAX, coredump);

	ss->read.buffer = kvmalloc(ss->read.size, GFP_USER);
	if (!ss->read.buffer)
		return;

	__xe_devcoredump_read(ss->read.buffer, ss->read.size, coredump);
	xe_devcoredump_snapshot_free(ss);
}

static void devcoredump_snapshot(struct xe_devcoredump *coredump,
				 struct xe_sched_job *job)
{
	struct xe_devcoredump_snapshot *ss = &coredump->snapshot;
	struct xe_exec_queue *q = job->q;
	struct xe_guc *guc = exec_queue_to_guc(q);
	struct xe_hw_engine *hwe;
	enum xe_hw_engine_id id;
	u32 adj_logical_mask = q->logical_mask;
	u32 width_mask = (0x1 << q->width) - 1;
	const char *process_name = "no process";

	unsigned int fw_ref;
	bool cookie;
	int i;

	ss->snapshot_time = ktime_get_real();
	ss->boot_time = ktime_get_boottime();

	if (q->vm && q->vm->xef)
		process_name = q->vm->xef->process_name;
	strscpy(ss->process_name, process_name);

	ss->gt = q->gt;
	INIT_WORK(&ss->work, xe_devcoredump_deferred_snap_work);

	cookie = dma_fence_begin_signalling();
	for (i = 0; q->width > 1 && i < XE_HW_ENGINE_MAX_INSTANCE;) {
		if (adj_logical_mask & BIT(i)) {
			adj_logical_mask |= width_mask << i;
			i += q->width;
		} else {
			++i;
		}
	}

	/* keep going if fw fails as we still want to save the memory and SW data */
	fw_ref = xe_force_wake_get(gt_to_fw(q->gt), XE_FORCEWAKE_ALL);

	ss->ct = xe_guc_ct_snapshot_capture(&guc->ct, true);
	ss->ge = xe_guc_exec_queue_snapshot_capture(q);
	ss->job = xe_sched_job_snapshot_capture(job);
	ss->vm = xe_vm_snapshot_capture(q->vm);

	for_each_hw_engine(hwe, q->gt, id) {
		if (hwe->class != q->hwe->class ||
		    !(BIT(hwe->logical_instance) & adj_logical_mask)) {
			ss->hwe[id] = NULL;
			continue;
		}
		ss->hwe[id] = xe_hw_engine_snapshot_capture(hwe);
	}

	queue_work(system_unbound_wq, &ss->work);

	xe_force_wake_put(gt_to_fw(q->gt), fw_ref);
	dma_fence_end_signalling(cookie);
}

/**
 * xe_devcoredump - Take the required snapshots and initialize coredump device.
 * @job: The faulty xe_sched_job, where the issue was detected.
 *
 * This function should be called at the crash time within the serialized
 * gt_reset. It is skipped if we still have the core dump device available
 * with the information of the 'first' snapshot.
 */
void xe_devcoredump(struct xe_sched_job *job)
{
	struct xe_device *xe = gt_to_xe(job->q->gt);
	struct xe_devcoredump *coredump = &xe->devcoredump;

	if (coredump->captured) {
		drm_dbg(&xe->drm, "Multiple hangs are occurring, but only the first snapshot was taken\n");
		return;
	}

	coredump->captured = true;
	devcoredump_snapshot(coredump, job);

	drm_info(&xe->drm, "Xe device coredump has been created\n");
	drm_info(&xe->drm, "Check your /sys/class/drm/card%d/device/devcoredump/data\n",
		 xe->drm.primary->index);
}

static void xe_driver_devcoredump_fini(void *arg)
{
	struct drm_device *drm = arg;

	dev_coredump_put(drm->dev);
}

int xe_devcoredump_init(struct xe_device *xe)
{
	return devm_add_action_or_reset(xe->drm.dev, xe_driver_devcoredump_fini, &xe->drm);
}

#endif

/**
 * xe_print_blob_ascii85 - print a BLOB to some useful location in ASCII85
 *
 * The output is split into multiple calls to drm_puts() because some print
 * targets, e.g. dmesg, cannot handle arbitrarily long lines. These targets may
 * add newlines, as is the case with dmesg: each drm_puts() call creates a
 * separate line.
 *
 * There is also a scheduler yield call to prevent the 'task has been stuck for
 * 120s' kernel hang check feature from firing when printing to a slow target
 * such as dmesg over a serial port.
 *
 * @p: the printer object to output to
 * @prefix: optional prefix to add to output string
 * @suffix: optional suffix to add at the end. 0 disables it and is
 *          not added to the output, which is useful when using multiple calls
 *          to dump data to @p
 * @blob: the Binary Large OBject to dump out
 * @offset: offset in bytes to skip from the front of the BLOB, must be a multiple of sizeof(u32)
 * @size: the size in bytes of the BLOB, must be a multiple of sizeof(u32)
 */
void xe_print_blob_ascii85(struct drm_printer *p, const char *prefix, char suffix,
			   const void *blob, size_t offset, size_t size)
{
	const u32 *blob32 = (const u32 *)blob;
	char buff[ASCII85_BUFSZ], *line_buff;
	size_t line_pos = 0;

#define DMESG_MAX_LINE_LEN	800
	/* Always leave space for the suffix char and the \0 */
#define MIN_SPACE		(ASCII85_BUFSZ + 2)	/* 85 + "<suffix>\0" */

	if (size & 3)
		drm_printf(p, "Size not word aligned: %zu", size);
	if (offset & 3)
		drm_printf(p, "Offset not word aligned: %zu", size);

	line_buff = kzalloc(DMESG_MAX_LINE_LEN, GFP_KERNEL);
	if (IS_ERR_OR_NULL(line_buff)) {
		drm_printf(p, "Failed to allocate line buffer: %pe", line_buff);
		return;
	}

	blob32 += offset / sizeof(*blob32);
	size /= sizeof(*blob32);

	if (prefix) {
		strscpy(line_buff, prefix, DMESG_MAX_LINE_LEN - MIN_SPACE - 2);
		line_pos = strlen(line_buff);

		line_buff[line_pos++] = ':';
		line_buff[line_pos++] = ' ';
	}

	while (size--) {
		u32 val = *(blob32++);

		strscpy(line_buff + line_pos, ascii85_encode(val, buff),
			DMESG_MAX_LINE_LEN - line_pos);
		line_pos += strlen(line_buff + line_pos);

		if ((line_pos + MIN_SPACE) >= DMESG_MAX_LINE_LEN) {
			line_buff[line_pos++] = 0;

			drm_puts(p, line_buff);

			line_pos = 0;

			/* Prevent 'stuck thread' time out errors */
			cond_resched();
		}
	}

	if (suffix)
		line_buff[line_pos++] = suffix;

	if (line_pos) {
		line_buff[line_pos++] = 0;
		drm_puts(p, line_buff);
	}

	kfree(line_buff);

#undef MIN_SPACE
#undef DMESG_MAX_LINE_LEN
}
