// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/binding.h>
#include <ddk/protocol/block.h>

#include <zircon/process.h>
#include <zircon/types.h>
#include <sys/param.h>
#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <threads.h>

#include "server.h"

typedef struct blkdev {
    zx_device_t* zxdev;
    zx_device_t* parent;
    block_protocol_t proto;

    mtx_t lock;
    uint32_t threadcount;
    BlockServer* bs;
    bool dead; // Release has been called; we should free memory and leave.
} blkdev_t;

static int blockserver_thread_serve(blkdev_t* bdev) TA_REL(bdev->lock) {
    BlockServer* bs = bdev->bs;
    bdev->threadcount++;
    mtx_unlock(&bdev->lock);

    blockserver_serve(bs);

    mtx_lock(&bdev->lock);
    if (bdev->bs == bs) {
        // Only nullify 'bs' if no one has replaced it yet. This is the
        // case when the blockserver shuts itself down because the fifo
        // has closed.
        bdev->bs = NULL;
    }
    bdev->threadcount--;
    bool cleanup = bdev->dead & (bdev->threadcount == 0);
    mtx_unlock(&bdev->lock);

    blockserver_free(bs);

    if (cleanup) {
        free(bdev);
    }
    return 0;
}

static int blockserver_thread(void* arg) TA_REL(((blkdev_t*)arg)->lock) {
    return blockserver_thread_serve((blkdev_t*)arg);
}

// This function conditionally acquires bdev->lock, and the code
// responsible for unlocking in the success case is on another
// thread. The analysis is not up to reasoning about this.
static zx_status_t blkdev_get_fifos(blkdev_t* bdev, void* out_buf, size_t out_len)
    TA_NO_THREAD_SAFETY_ANALYSIS {
    if (out_len < sizeof(zx_handle_t)) {
        return ZX_ERR_INVALID_ARGS;
    }
    zx_status_t status;
    mtx_lock(&bdev->lock);
    if (bdev->bs != NULL) {
        status = ZX_ERR_ALREADY_BOUND;
        goto done;
    }

    BlockServer* bs;
    if ((status = blockserver_create(&bdev->proto, out_buf, &bs)) != ZX_OK) {
        goto done;
    }

    // As soon as we launch a thread, the background thread is responsible
    // for the blockserver in the bdev->bs field.
    bdev->bs = bs;
    thrd_t thread;
    if (thrd_create(&thread, blockserver_thread, bdev) != thrd_success) {
        blockserver_free(bs);
        bdev->bs = NULL;
        status = ZX_ERR_NO_MEMORY;
        goto done;
    }
    thrd_detach(thread);

    // On success, the blockserver thread holds the lock.
    return sizeof(zx_handle_t);
done:
    mtx_unlock(&bdev->lock);
    return status;
}

static zx_status_t blkdev_attach_vmo(blkdev_t* bdev,
                                 const void* in_buf, size_t in_len,
                                 void* out_buf, size_t out_len, size_t* out_actual) {
    if ((in_len < sizeof(zx_handle_t)) || (out_len < sizeof(vmoid_t))) {
        return ZX_ERR_INVALID_ARGS;
    }

    zx_status_t status;
    mtx_lock(&bdev->lock);
    if (bdev->bs == NULL) {
        status = ZX_ERR_BAD_STATE;
        goto done;
    }

    zx_handle_t h = *(zx_handle_t*)in_buf;
    if ((status = blockserver_attach_vmo(bdev->bs, h, out_buf)) != ZX_OK) {
        goto done;
    }
    *out_actual = sizeof(vmoid_t);

done:
    mtx_unlock(&bdev->lock);
    return status;
}

static zx_status_t blkdev_alloc_txn(blkdev_t* bdev,
                                const void* in_buf, size_t in_len,
                                void* out_buf, size_t out_len, size_t* out_actual) {
    if ((in_len != 0) || (out_len < sizeof(txnid_t))) {
        return ZX_ERR_INVALID_ARGS;
    }

    zx_status_t status;
    mtx_lock(&bdev->lock);
    if (bdev->bs == NULL) {
        status = ZX_ERR_BAD_STATE;
        goto done;
    }

    if ((status = blockserver_allocate_txn(bdev->bs, out_buf)) != ZX_OK) {
        goto done;
    }
    *out_actual = sizeof(vmoid_t);

done:
    mtx_unlock(&bdev->lock);
    return status;
}

static zx_status_t blkdev_free_txn(blkdev_t* bdev, const void* in_buf,
                                   size_t in_len) {
    if (in_len != sizeof(txnid_t)) {
        return ZX_ERR_INVALID_ARGS;
    }

    zx_status_t status;
    mtx_lock(&bdev->lock);
    if (bdev->bs == NULL) {
        status = ZX_ERR_BAD_STATE;
        goto done;
    }

    txnid_t txnid = *(txnid_t*)in_buf;
    blockserver_free_txn(bdev->bs, txnid);
    status = ZX_OK;
done:
    mtx_unlock(&bdev->lock);
    return status;
}

static zx_status_t blkdev_fifo_close_locked(blkdev_t* bdev) {
    if (bdev->bs != NULL) {
        blockserver_shutdown(bdev->bs);
        // Ensure that the next thread to call "get_fifos" will
        // not see the previous block server.
        bdev->bs = NULL;
    }
    return ZX_OK;
}

// implement device protocol:

static zx_status_t blkdev_ioctl(void* ctx, uint32_t op, const void* cmd,
                            size_t cmdlen, void* reply, size_t max, size_t* out_actual) {
    blkdev_t* blkdev = ctx;
    switch (op) {
    case IOCTL_BLOCK_GET_FIFOS:
        return blkdev_get_fifos(blkdev, reply, max);
    case IOCTL_BLOCK_ATTACH_VMO:
        return blkdev_attach_vmo(blkdev, cmd, cmdlen, reply, max, out_actual);
    case IOCTL_BLOCK_ALLOC_TXN:
        return blkdev_alloc_txn(blkdev, cmd, cmdlen, reply, max, out_actual);
    case IOCTL_BLOCK_FREE_TXN:
        return blkdev_free_txn(blkdev, cmd, cmdlen);
    case IOCTL_BLOCK_FIFO_CLOSE: {
        mtx_lock(&blkdev->lock);
        zx_status_t status = blkdev_fifo_close_locked(blkdev);
        mtx_unlock(&blkdev->lock);
        return status;
    }
    default:
        return device_ioctl(blkdev->parent, op, cmd, cmdlen, reply, max, out_actual);
    }
}

static void blkdev_iotxn_queue(void* ctx, iotxn_t* txn) {
    blkdev_t* blkdev = ctx;
    iotxn_queue(blkdev->parent, txn);
}

static zx_off_t blkdev_get_size(void* ctx) {
    blkdev_t* blkdev = ctx;
    return device_get_size(blkdev->parent);
}

static void blkdev_unbind(void* ctx) {
    blkdev_t* blkdev = ctx;
    device_remove(blkdev->zxdev);
}

static void blkdev_release(void* ctx) {
    blkdev_t* blkdev = ctx;
    mtx_lock(&blkdev->lock);
    bool bg_thread_running = (blkdev->threadcount != 0);
    blkdev_fifo_close_locked(blkdev);
    blkdev->dead = true;
    mtx_unlock(&blkdev->lock);

    if (!bg_thread_running) {
        // If it isn't running, we need to clean up.
        // Otherwise, it'll free blkdev's memory when it's done,
        // since (1) no one else can call get_fifos anymore, and
        // (2) it'll clean up when it sees that blkdev is dead.
        free(blkdev);
    }
}

static zx_protocol_device_t blkdev_ops = {
    .version = DEVICE_OPS_VERSION,
    .ioctl = blkdev_ioctl,
    .iotxn_queue = blkdev_iotxn_queue,
    .get_size = blkdev_get_size,
    .unbind = blkdev_unbind,
    .release = blkdev_release,
};

static zx_status_t block_driver_bind(void* ctx, zx_device_t* dev) {
    blkdev_t* bdev;
    if ((bdev = calloc(1, sizeof(blkdev_t))) == NULL) {
        return ZX_ERR_NO_MEMORY;
    }
    bdev->threadcount = 0;
    mtx_init(&bdev->lock, mtx_plain);
    bdev->parent = dev;

    zx_status_t status;
    if (device_get_protocol(dev, ZX_PROTOCOL_BLOCK_CORE, &bdev->proto)) {
        status = ZX_ERR_INTERNAL;
        goto fail;
    }

   device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "block",
        .ctx = bdev,
        .ops = &blkdev_ops,
        .proto_id = ZX_PROTOCOL_BLOCK,
    };

    status = device_add(dev, &args, &bdev->zxdev);
    if (status != ZX_OK) {
        goto fail;
    }

    return ZX_OK;

fail:
    free(bdev);
    return status;
}

static zx_driver_ops_t block_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = block_driver_bind,
};

ZIRCON_DRIVER_BEGIN(block, block_driver_ops, "zircon", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_BLOCK_CORE),
ZIRCON_DRIVER_END(block)
