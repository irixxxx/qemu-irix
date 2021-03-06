/*
 * QEMU System Emulator block driver
 *
 * Copyright (c) 2011 IBM Corp.
 * Copyright (c) 2012 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "block/block.h"
#include "block/blockjob_int.h"
#include "block/block_int.h"
#include "sysemu/block-backend.h"
#include "qapi/error.h"
#include "qapi/qapi-events-block-core.h"
#include "qapi/qmp/qerror.h"
#include "qemu/coroutine.h"
#include "qemu/id.h"
#include "qemu/timer.h"

/* Right now, this mutex is only needed to synchronize accesses to job->busy
 * and job->sleep_timer, such as concurrent calls to block_job_do_yield and
 * block_job_enter. */
static QemuMutex block_job_mutex;

static void block_job_lock(void)
{
    qemu_mutex_lock(&block_job_mutex);
}

static void block_job_unlock(void)
{
    qemu_mutex_unlock(&block_job_mutex);
}

static void __attribute__((__constructor__)) block_job_init(void)
{
    qemu_mutex_init(&block_job_mutex);
}

static void block_job_event_cancelled(BlockJob *job);
static void block_job_event_completed(BlockJob *job, const char *msg);
static void block_job_enter_cond(BlockJob *job, bool(*fn)(BlockJob *job));

/* Transactional group of block jobs */
struct BlockJobTxn {

    /* Is this txn being cancelled? */
    bool aborting;

    /* List of jobs */
    QLIST_HEAD(, BlockJob) jobs;

    /* Reference count */
    int refcnt;
};

static QLIST_HEAD(, BlockJob) block_jobs = QLIST_HEAD_INITIALIZER(block_jobs);

/*
 * The block job API is composed of two categories of functions.
 *
 * The first includes functions used by the monitor.  The monitor is
 * peculiar in that it accesses the block job list with block_job_get, and
 * therefore needs consistency across block_job_get and the actual operation
 * (e.g. block_job_set_speed).  The consistency is achieved with
 * aio_context_acquire/release.  These functions are declared in blockjob.h.
 *
 * The second includes functions used by the block job drivers and sometimes
 * by the core block layer.  These do not care about locking, because the
 * whole coroutine runs under the AioContext lock, and are declared in
 * blockjob_int.h.
 */

BlockJob *block_job_next(BlockJob *job)
{
    if (!job) {
        return QLIST_FIRST(&block_jobs);
    }
    return QLIST_NEXT(job, job_list);
}

BlockJob *block_job_get(const char *id)
{
    BlockJob *job;

    QLIST_FOREACH(job, &block_jobs, job_list) {
        if (job->id && !strcmp(id, job->id)) {
            return job;
        }
    }

    return NULL;
}

BlockJobTxn *block_job_txn_new(void)
{
    BlockJobTxn *txn = g_new0(BlockJobTxn, 1);
    QLIST_INIT(&txn->jobs);
    txn->refcnt = 1;
    return txn;
}

static void block_job_txn_ref(BlockJobTxn *txn)
{
    txn->refcnt++;
}

void block_job_txn_unref(BlockJobTxn *txn)
{
    if (txn && --txn->refcnt == 0) {
        g_free(txn);
    }
}

void block_job_txn_add_job(BlockJobTxn *txn, BlockJob *job)
{
    if (!txn) {
        return;
    }

    assert(!job->txn);
    job->txn = txn;

    QLIST_INSERT_HEAD(&txn->jobs, job, txn_list);
    block_job_txn_ref(txn);
}

static void block_job_pause(BlockJob *job)
{
    job->pause_count++;
}

static void block_job_resume(BlockJob *job)
{
    assert(job->pause_count > 0);
    job->pause_count--;
    if (job->pause_count) {
        return;
    }
    block_job_enter(job);
}

void block_job_ref(BlockJob *job)
{
    ++job->refcnt;
}

static void block_job_attached_aio_context(AioContext *new_context,
                                           void *opaque);
static void block_job_detach_aio_context(void *opaque);

void block_job_unref(BlockJob *job)
{
    if (--job->refcnt == 0) {
        BlockDriverState *bs = blk_bs(job->blk);
        QLIST_REMOVE(job, job_list);
        bs->job = NULL;
        block_job_remove_all_bdrv(job);
        blk_remove_aio_context_notifier(job->blk,
                                        block_job_attached_aio_context,
                                        block_job_detach_aio_context, job);
        blk_unref(job->blk);
        error_free(job->blocker);
        g_free(job->id);
        assert(!timer_pending(&job->sleep_timer));
        g_free(job);
    }
}

static void block_job_attached_aio_context(AioContext *new_context,
                                           void *opaque)
{
    BlockJob *job = opaque;

    if (job->driver->attached_aio_context) {
        job->driver->attached_aio_context(job, new_context);
    }

    block_job_resume(job);
}

static void block_job_drain(BlockJob *job)
{
    /* If job is !job->busy this kicks it into the next pause point. */
    block_job_enter(job);

    blk_drain(job->blk);
    if (job->driver->drain) {
        job->driver->drain(job);
    }
}

static void block_job_detach_aio_context(void *opaque)
{
    BlockJob *job = opaque;

    /* In case the job terminates during aio_poll()... */
    block_job_ref(job);

    block_job_pause(job);

    while (!job->paused && !job->completed) {
        block_job_drain(job);
    }

    block_job_unref(job);
}

static char *child_job_get_parent_desc(BdrvChild *c)
{
    BlockJob *job = c->opaque;
    return g_strdup_printf("%s job '%s'",
                           BlockJobType_str(job->driver->job_type),
                           job->id);
}

static void child_job_drained_begin(BdrvChild *c)
{
    BlockJob *job = c->opaque;
    block_job_pause(job);
}

static void child_job_drained_end(BdrvChild *c)
{
    BlockJob *job = c->opaque;
    block_job_resume(job);
}

static const BdrvChildRole child_job = {
    .get_parent_desc    = child_job_get_parent_desc,
    .drained_begin      = child_job_drained_begin,
    .drained_end        = child_job_drained_end,
    .stay_at_node       = true,
};

void block_job_remove_all_bdrv(BlockJob *job)
{
    GSList *l;
    for (l = job->nodes; l; l = l->next) {
        BdrvChild *c = l->data;
        bdrv_op_unblock_all(c->bs, job->blocker);
        bdrv_root_unref_child(c);
    }
    g_slist_free(job->nodes);
    job->nodes = NULL;
}

int block_job_add_bdrv(BlockJob *job, const char *name, BlockDriverState *bs,
                       uint64_t perm, uint64_t shared_perm, Error **errp)
{
    BdrvChild *c;

    c = bdrv_root_attach_child(bs, name, &child_job, perm, shared_perm,
                               job, errp);
    if (c == NULL) {
        return -EPERM;
    }

    job->nodes = g_slist_prepend(job->nodes, c);
    bdrv_ref(bs);
    bdrv_op_block_all(bs, job->blocker);

    return 0;
}

bool block_job_is_internal(BlockJob *job)
{
    return (job->id == NULL);
}

static bool block_job_started(BlockJob *job)
{
    return job->co;
}

/**
 * All jobs must allow a pause point before entering their job proper. This
 * ensures that jobs can be paused prior to being started, then resumed later.
 */
static void coroutine_fn block_job_co_entry(void *opaque)
{
    BlockJob *job = opaque;

    assert(job && job->driver && job->driver->start);
    block_job_pause_point(job);
    job->driver->start(job);
}

static void block_job_sleep_timer_cb(void *opaque)
{
    BlockJob *job = opaque;

    block_job_enter(job);
}

void block_job_start(BlockJob *job)
{
    assert(job && !block_job_started(job) && job->paused &&
           job->driver && job->driver->start);
    job->co = qemu_coroutine_create(block_job_co_entry, job);
    job->pause_count--;
    job->busy = true;
    job->paused = false;
    bdrv_coroutine_enter(blk_bs(job->blk), job->co);
}

static void block_job_completed_single(BlockJob *job)
{
    assert(job->completed);

    if (!job->ret) {
        if (job->driver->commit) {
            job->driver->commit(job);
        }
    } else {
        if (job->driver->abort) {
            job->driver->abort(job);
        }
    }
    if (job->driver->clean) {
        job->driver->clean(job);
    }

    if (job->cb) {
        job->cb(job->opaque, job->ret);
    }

    /* Emit events only if we actually started */
    if (block_job_started(job)) {
        if (block_job_is_cancelled(job)) {
            block_job_event_cancelled(job);
        } else {
            const char *msg = NULL;
            if (job->ret < 0) {
                msg = strerror(-job->ret);
            }
            block_job_event_completed(job, msg);
        }
    }

    if (job->txn) {
        QLIST_REMOVE(job, txn_list);
        block_job_txn_unref(job->txn);
    }
    block_job_unref(job);
}

static void block_job_cancel_async(BlockJob *job)
{
    if (job->iostatus != BLOCK_DEVICE_IO_STATUS_OK) {
        block_job_iostatus_reset(job);
    }
    if (job->user_paused) {
        /* Do not call block_job_enter here, the caller will handle it.  */
        job->user_paused = false;
        job->pause_count--;
    }
    job->cancelled = true;
}

static int block_job_finish_sync(BlockJob *job,
                                 void (*finish)(BlockJob *, Error **errp),
                                 Error **errp)
{
    Error *local_err = NULL;
    int ret;

    assert(blk_bs(job->blk)->job == job);

    block_job_ref(job);

    if (finish) {
        finish(job, &local_err);
    }
    if (local_err) {
        error_propagate(errp, local_err);
        block_job_unref(job);
        return -EBUSY;
    }
    /* block_job_drain calls block_job_enter, and it should be enough to
     * induce progress until the job completes or moves to the main thread.
    */
    while (!job->deferred_to_main_loop && !job->completed) {
        block_job_drain(job);
    }
    while (!job->completed) {
        aio_poll(qemu_get_aio_context(), true);
    }
    ret = (job->cancelled && job->ret == 0) ? -ECANCELED : job->ret;
    block_job_unref(job);
    return ret;
}

static void block_job_completed_txn_abort(BlockJob *job)
{
    AioContext *ctx;
    BlockJobTxn *txn = job->txn;
    BlockJob *other_job;

    if (txn->aborting) {
        /*
         * We are cancelled by another job, which will handle everything.
         */
        return;
    }
    txn->aborting = true;
    block_job_txn_ref(txn);

    /* We are the first failed job. Cancel other jobs. */
    QLIST_FOREACH(other_job, &txn->jobs, txn_list) {
        ctx = blk_get_aio_context(other_job->blk);
        aio_context_acquire(ctx);
    }

    /* Other jobs are effectively cancelled by us, set the status for
     * them; this job, however, may or may not be cancelled, depending
     * on the caller, so leave it. */
    QLIST_FOREACH(other_job, &txn->jobs, txn_list) {
        if (other_job != job) {
            block_job_cancel_async(other_job);
        }
    }
    while (!QLIST_EMPTY(&txn->jobs)) {
        other_job = QLIST_FIRST(&txn->jobs);
        ctx = blk_get_aio_context(other_job->blk);
        if (!other_job->completed) {
            assert(other_job->cancelled);
            block_job_finish_sync(other_job, NULL, NULL);
        }
        block_job_completed_single(other_job);
        aio_context_release(ctx);
    }

    block_job_txn_unref(txn);
}

static void block_job_completed_txn_success(BlockJob *job)
{
    AioContext *ctx;
    BlockJobTxn *txn = job->txn;
    BlockJob *other_job, *next;
    /*
     * Successful completion, see if there are other running jobs in this
     * txn.
     */
    QLIST_FOREACH(other_job, &txn->jobs, txn_list) {
        if (!other_job->completed) {
            return;
        }
    }
    /* We are the last completed job, commit the transaction. */
    QLIST_FOREACH_SAFE(other_job, &txn->jobs, txn_list, next) {
        ctx = blk_get_aio_context(other_job->blk);
        aio_context_acquire(ctx);
        assert(other_job->ret == 0);
        block_job_completed_single(other_job);
        aio_context_release(ctx);
    }
}

/* Assumes the block_job_mutex is held */
static bool block_job_timer_pending(BlockJob *job)
{
    return timer_pending(&job->sleep_timer);
}

void block_job_set_speed(BlockJob *job, int64_t speed, Error **errp)
{
    Error *local_err = NULL;
    int64_t old_speed = job->speed;

    if (!job->driver->set_speed) {
        error_setg(errp, QERR_UNSUPPORTED);
        return;
    }
    job->driver->set_speed(job, speed, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    job->speed = speed;
    if (speed <= old_speed) {
        return;
    }

    /* kick only if a timer is pending */
    block_job_enter_cond(job, block_job_timer_pending);
}

void block_job_complete(BlockJob *job, Error **errp)
{
    /* Should not be reachable via external interface for internal jobs */
    assert(job->id);
    if (job->pause_count || job->cancelled ||
        !block_job_started(job) || !job->driver->complete) {
        error_setg(errp, "The active block job '%s' cannot be completed",
                   job->id);
        return;
    }

    job->driver->complete(job, errp);
}

void block_job_user_pause(BlockJob *job)
{
    job->user_paused = true;
    block_job_pause(job);
}

bool block_job_user_paused(BlockJob *job)
{
    return job->user_paused;
}

void block_job_user_resume(BlockJob *job)
{
    if (job && job->user_paused && job->pause_count > 0) {
        block_job_iostatus_reset(job);
        job->user_paused = false;
        block_job_resume(job);
    }
}

void block_job_cancel(BlockJob *job)
{
    if (block_job_started(job)) {
        block_job_cancel_async(job);
        block_job_enter(job);
    } else {
        block_job_completed(job, -ECANCELED);
    }
}

/* A wrapper around block_job_cancel() taking an Error ** parameter so it may be
 * used with block_job_finish_sync() without the need for (rather nasty)
 * function pointer casts there. */
static void block_job_cancel_err(BlockJob *job, Error **errp)
{
    block_job_cancel(job);
}

int block_job_cancel_sync(BlockJob *job)
{
    return block_job_finish_sync(job, &block_job_cancel_err, NULL);
}

void block_job_cancel_sync_all(void)
{
    BlockJob *job;
    AioContext *aio_context;

    while ((job = QLIST_FIRST(&block_jobs))) {
        aio_context = blk_get_aio_context(job->blk);
        aio_context_acquire(aio_context);
        block_job_cancel_sync(job);
        aio_context_release(aio_context);
    }
}

int block_job_complete_sync(BlockJob *job, Error **errp)
{
    return block_job_finish_sync(job, &block_job_complete, errp);
}

BlockJobInfo *block_job_query(BlockJob *job, Error **errp)
{
    BlockJobInfo *info;

    if (block_job_is_internal(job)) {
        error_setg(errp, "Cannot query QEMU internal jobs");
        return NULL;
    }
    info = g_new0(BlockJobInfo, 1);
    info->type      = g_strdup(BlockJobType_str(job->driver->job_type));
    info->device    = g_strdup(job->id);
    info->len       = job->len;
    info->busy      = atomic_read(&job->busy);
    info->paused    = job->pause_count > 0;
    info->offset    = job->offset;
    info->speed     = job->speed;
    info->io_status = job->iostatus;
    info->ready     = job->ready;
    return info;
}

static void block_job_iostatus_set_err(BlockJob *job, int error)
{
    if (job->iostatus == BLOCK_DEVICE_IO_STATUS_OK) {
        job->iostatus = error == ENOSPC ? BLOCK_DEVICE_IO_STATUS_NOSPACE :
                                          BLOCK_DEVICE_IO_STATUS_FAILED;
    }
}

static void block_job_event_cancelled(BlockJob *job)
{
    if (block_job_is_internal(job)) {
        return;
    }

    qapi_event_send_block_job_cancelled(job->driver->job_type,
                                        job->id,
                                        job->len,
                                        job->offset,
                                        job->speed,
                                        &error_abort);
}

static void block_job_event_completed(BlockJob *job, const char *msg)
{
    if (block_job_is_internal(job)) {
        return;
    }

    qapi_event_send_block_job_completed(job->driver->job_type,
                                        job->id,
                                        job->len,
                                        job->offset,
                                        job->speed,
                                        !!msg,
                                        msg,
                                        &error_abort);
}

/*
 * API for block job drivers and the block layer.  These functions are
 * declared in blockjob_int.h.
 */

void *block_job_create(const char *job_id, const BlockJobDriver *driver,
                       BlockDriverState *bs, uint64_t perm,
                       uint64_t shared_perm, int64_t speed, int flags,
                       BlockCompletionFunc *cb, void *opaque, Error **errp)
{
    BlockBackend *blk;
    BlockJob *job;
    int ret;

    if (bs->job) {
        error_setg(errp, QERR_DEVICE_IN_USE, bdrv_get_device_name(bs));
        return NULL;
    }

    if (job_id == NULL && !(flags & BLOCK_JOB_INTERNAL)) {
        job_id = bdrv_get_device_name(bs);
        if (!*job_id) {
            error_setg(errp, "An explicit job ID is required for this node");
            return NULL;
        }
    }

    if (job_id) {
        if (flags & BLOCK_JOB_INTERNAL) {
            error_setg(errp, "Cannot specify job ID for internal block job");
            return NULL;
        }

        if (!id_wellformed(job_id)) {
            error_setg(errp, "Invalid job ID '%s'", job_id);
            return NULL;
        }

        if (block_job_get(job_id)) {
            error_setg(errp, "Job ID '%s' already in use", job_id);
            return NULL;
        }
    }

    blk = blk_new(perm, shared_perm);
    ret = blk_insert_bs(blk, bs, errp);
    if (ret < 0) {
        blk_unref(blk);
        return NULL;
    }

    job = g_malloc0(driver->instance_size);
    job->driver        = driver;
    job->id            = g_strdup(job_id);
    job->blk           = blk;
    job->cb            = cb;
    job->opaque        = opaque;
    job->busy          = false;
    job->paused        = true;
    job->pause_count   = 1;
    job->refcnt        = 1;
    aio_timer_init(qemu_get_aio_context(), &job->sleep_timer,
                   QEMU_CLOCK_REALTIME, SCALE_NS,
                   block_job_sleep_timer_cb, job);

    error_setg(&job->blocker, "block device is in use by block job: %s",
               BlockJobType_str(driver->job_type));
    block_job_add_bdrv(job, "main node", bs, 0, BLK_PERM_ALL, &error_abort);
    bs->job = job;

    bdrv_op_unblock(bs, BLOCK_OP_TYPE_DATAPLANE, job->blocker);

    QLIST_INSERT_HEAD(&block_jobs, job, job_list);

    blk_add_aio_context_notifier(blk, block_job_attached_aio_context,
                                 block_job_detach_aio_context, job);

    /* Only set speed when necessary to avoid NotSupported error */
    if (speed != 0) {
        Error *local_err = NULL;

        block_job_set_speed(job, speed, &local_err);
        if (local_err) {
            block_job_unref(job);
            error_propagate(errp, local_err);
            return NULL;
        }
    }
    return job;
}

void block_job_pause_all(void)
{
    BlockJob *job = NULL;
    while ((job = block_job_next(job))) {
        AioContext *aio_context = blk_get_aio_context(job->blk);

        aio_context_acquire(aio_context);
        block_job_ref(job);
        block_job_pause(job);
        aio_context_release(aio_context);
    }
}

void block_job_early_fail(BlockJob *job)
{
    block_job_unref(job);
}

void block_job_completed(BlockJob *job, int ret)
{
    assert(blk_bs(job->blk)->job == job);
    assert(!job->completed);
    job->completed = true;
    job->ret = ret;
    if (!job->txn) {
        block_job_completed_single(job);
    } else if (ret < 0 || block_job_is_cancelled(job)) {
        block_job_completed_txn_abort(job);
    } else {
        block_job_completed_txn_success(job);
    }
}

static bool block_job_should_pause(BlockJob *job)
{
    return job->pause_count > 0;
}

/* Yield, and schedule a timer to reenter the coroutine after @ns nanoseconds.
 * Reentering the job coroutine with block_job_enter() before the timer has
 * expired is allowed and cancels the timer.
 *
 * If @ns is (uint64_t) -1, no timer is scheduled and block_job_enter() must be
 * called explicitly. */
static void block_job_do_yield(BlockJob *job, uint64_t ns)
{
    block_job_lock();
    if (ns != -1) {
        timer_mod(&job->sleep_timer, ns);
    }
    job->busy = false;
    block_job_unlock();
    qemu_coroutine_yield();

    /* Set by block_job_enter before re-entering the coroutine.  */
    assert(job->busy);
}

void coroutine_fn block_job_pause_point(BlockJob *job)
{
    assert(job && block_job_started(job));

    if (!block_job_should_pause(job)) {
        return;
    }
    if (block_job_is_cancelled(job)) {
        return;
    }

    if (job->driver->pause) {
        job->driver->pause(job);
    }

    if (block_job_should_pause(job) && !block_job_is_cancelled(job)) {
        job->paused = true;
        block_job_do_yield(job, -1);
        job->paused = false;
    }

    if (job->driver->resume) {
        job->driver->resume(job);
    }
}

void block_job_resume_all(void)
{
    BlockJob *job, *next;

    QLIST_FOREACH_SAFE(job, &block_jobs, job_list, next) {
        AioContext *aio_context = blk_get_aio_context(job->blk);

        aio_context_acquire(aio_context);
        block_job_resume(job);
        block_job_unref(job);
        aio_context_release(aio_context);
    }
}

/*
 * Conditionally enter a block_job pending a call to fn() while
 * under the block_job_lock critical section.
 */
static void block_job_enter_cond(BlockJob *job, bool(*fn)(BlockJob *job))
{
    if (!block_job_started(job)) {
        return;
    }
    if (job->deferred_to_main_loop) {
        return;
    }

    block_job_lock();
    if (job->busy) {
        block_job_unlock();
        return;
    }

    if (fn && !fn(job)) {
        block_job_unlock();
        return;
    }

    assert(!job->deferred_to_main_loop);
    timer_del(&job->sleep_timer);
    job->busy = true;
    block_job_unlock();
    aio_co_wake(job->co);
}

void block_job_enter(BlockJob *job)
{
    block_job_enter_cond(job, NULL);
}

bool block_job_is_cancelled(BlockJob *job)
{
    return job->cancelled;
}

void block_job_sleep_ns(BlockJob *job, int64_t ns)
{
    assert(job->busy);

    /* Check cancellation *before* setting busy = false, too!  */
    if (block_job_is_cancelled(job)) {
        return;
    }

    if (!block_job_should_pause(job)) {
        block_job_do_yield(job, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + ns);
    }

    block_job_pause_point(job);
}

void block_job_yield(BlockJob *job)
{
    assert(job->busy);

    /* Check cancellation *before* setting busy = false, too!  */
    if (block_job_is_cancelled(job)) {
        return;
    }

    if (!block_job_should_pause(job)) {
        block_job_do_yield(job, -1);
    }

    block_job_pause_point(job);
}

void block_job_iostatus_reset(BlockJob *job)
{
    if (job->iostatus == BLOCK_DEVICE_IO_STATUS_OK) {
        return;
    }
    assert(job->user_paused && job->pause_count > 0);
    job->iostatus = BLOCK_DEVICE_IO_STATUS_OK;
}

void block_job_event_ready(BlockJob *job)
{
    job->ready = true;

    if (block_job_is_internal(job)) {
        return;
    }

    qapi_event_send_block_job_ready(job->driver->job_type,
                                    job->id,
                                    job->len,
                                    job->offset,
                                    job->speed, &error_abort);
}

BlockErrorAction block_job_error_action(BlockJob *job, BlockdevOnError on_err,
                                        int is_read, int error)
{
    BlockErrorAction action;

    switch (on_err) {
    case BLOCKDEV_ON_ERROR_ENOSPC:
    case BLOCKDEV_ON_ERROR_AUTO:
        action = (error == ENOSPC) ?
                 BLOCK_ERROR_ACTION_STOP : BLOCK_ERROR_ACTION_REPORT;
        break;
    case BLOCKDEV_ON_ERROR_STOP:
        action = BLOCK_ERROR_ACTION_STOP;
        break;
    case BLOCKDEV_ON_ERROR_REPORT:
        action = BLOCK_ERROR_ACTION_REPORT;
        break;
    case BLOCKDEV_ON_ERROR_IGNORE:
        action = BLOCK_ERROR_ACTION_IGNORE;
        break;
    default:
        abort();
    }
    if (!block_job_is_internal(job)) {
        qapi_event_send_block_job_error(job->id,
                                        is_read ? IO_OPERATION_TYPE_READ :
                                        IO_OPERATION_TYPE_WRITE,
                                        action, &error_abort);
    }
    if (action == BLOCK_ERROR_ACTION_STOP) {
        /* make the pause user visible, which will be resumed from QMP. */
        block_job_user_pause(job);
        block_job_iostatus_set_err(job, error);
    }
    return action;
}

typedef struct {
    BlockJob *job;
    AioContext *aio_context;
    BlockJobDeferToMainLoopFn *fn;
    void *opaque;
} BlockJobDeferToMainLoopData;

static void block_job_defer_to_main_loop_bh(void *opaque)
{
    BlockJobDeferToMainLoopData *data = opaque;
    AioContext *aio_context;

    /* Prevent race with block_job_defer_to_main_loop() */
    aio_context_acquire(data->aio_context);

    /* Fetch BDS AioContext again, in case it has changed */
    aio_context = blk_get_aio_context(data->job->blk);
    if (aio_context != data->aio_context) {
        aio_context_acquire(aio_context);
    }

    data->fn(data->job, data->opaque);

    if (aio_context != data->aio_context) {
        aio_context_release(aio_context);
    }

    aio_context_release(data->aio_context);

    g_free(data);
}

void block_job_defer_to_main_loop(BlockJob *job,
                                  BlockJobDeferToMainLoopFn *fn,
                                  void *opaque)
{
    BlockJobDeferToMainLoopData *data = g_malloc(sizeof(*data));
    data->job = job;
    data->aio_context = blk_get_aio_context(job->blk);
    data->fn = fn;
    data->opaque = opaque;
    job->deferred_to_main_loop = true;

    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            block_job_defer_to_main_loop_bh, data);
}
