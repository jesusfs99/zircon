// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/*
 * Very basic TPM driver
 *
 * Assumptions:
 * - This driver is the sole owner of the TPM hardware.  While the TPM hardware
 *   supports co-ownership, this code does not handle being kicked off the TPM.
 * - The system firmware is responsible for initializing the TPM and has
 *   already done so.
 */

#include <assert.h>
#include <endian.h>
#include <ddk/driver.h>
#include <ddk/io-buffer.h>
#include <zircon/device/tpm.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>

#include "tpm.h"
#include "tpm-commands.h"

#define TPM_PHYS_ADDRESS 0xfed40000
#define TPM_PHYS_LENGTH 0x5000

// This is arbitrary, we just want to limit the size of the response buffer
// that we need to allocate.
#define MAX_RAND_BYTES 256

static mtx_t tpm_lock = MTX_INIT;
void *tpm_base;
zx_handle_t irq_handle;
static io_buffer_t io_buffer;

// implement tpm protocol:

static zx_status_t tpm_get_random(zx_device_t* dev, void* buf, uint32_t count, size_t* actual) {
    static_assert(MAX_RAND_BYTES <= UINT32_MAX, "");
    if (count > MAX_RAND_BYTES) {
        count = MAX_RAND_BYTES;
    }
    struct tpm_getrandom_cmd cmd;
    uint32_t resp_len = tpm_init_getrandom(&cmd, count);
    struct tpm_getrandom_resp* resp = reinterpret_cast<tpm_getrandom_resp*>(malloc(resp_len));
    size_t actual_read;
    uint32_t bytes_returned;
    if (!resp) {
        return ZX_ERR_NO_MEMORY;
    }

    mtx_lock(&tpm_lock);

    zx_status_t status = tpm_send_cmd(LOCALITY0, (uint8_t*)&cmd, sizeof(cmd));
    if (status != ZX_OK) {
        goto cleanup;
    }
    status = tpm_recv_resp(LOCALITY0, (uint8_t*)&resp, resp_len, &actual_read);
    if (status != ZX_OK) {
        goto cleanup;
    }
    if (actual_read < sizeof(*resp) ||
        actual_read != betoh32(resp->hdr.total_len)) {

        status = ZX_ERR_BAD_STATE;
        goto cleanup;
    }
    bytes_returned = betoh32(resp->bytes_returned);
    if ((uint32_t)status != sizeof(*resp) + bytes_returned ||
        resp->hdr.tag != htobe16(TPM_TAG_RSP_COMMAND) ||
        bytes_returned > count ||
        resp->hdr.return_code != htobe32(TPM_SUCCESS)) {

        status = ZX_ERR_BAD_STATE;
        goto cleanup;
    }
    memcpy(buf, resp->bytes, bytes_returned);
    memset(resp->bytes, 0, bytes_returned);
    *actual = bytes_returned;
    status = ZX_OK;
cleanup:
    free(resp);
    mtx_unlock(&tpm_lock);
    return status;
}

static zx_status_t tpm_save_state(void) {
    struct tpm_savestate_cmd cmd;
    uint32_t resp_len = tpm_init_savestate(&cmd);
    struct tpm_savestate_resp resp;
    size_t actual;

    mtx_lock(&tpm_lock);

    zx_status_t status = tpm_send_cmd(LOCALITY0, (uint8_t*)&cmd, sizeof(cmd));
    if (status != ZX_OK) {
        goto cleanup;
    }
    status = tpm_recv_resp(LOCALITY0, (uint8_t*)&resp, resp_len, &actual);
    if (status != ZX_OK) {
        goto cleanup;
    }
    if (actual < sizeof(resp) ||
        actual != betoh32(resp.hdr.total_len) ||
        resp.hdr.tag != htobe16(TPM_TAG_RSP_COMMAND) ||
        resp.hdr.return_code != htobe32(TPM_SUCCESS)) {

        status = ZX_ERR_BAD_STATE;
        goto cleanup;
    }
    status = ZX_OK;
cleanup:
    mtx_unlock(&tpm_lock);
    return status;
}

static zx_status_t tpm_device_ioctl(void* ctx, uint32_t op,
                                    const void* in_buf, size_t in_len,
                                    void* out_buf, size_t out_len, size_t* out_actual) {
    switch (op) {
        case IOCTL_TPM_SAVE_STATE: return tpm_save_state();
    }
    return ZX_ERR_NOT_SUPPORTED;
}

// implement device protocol:
zx_protocol_device_t tpm_device_proto = {
    DEVICE_OPS_VERSION,
    nullptr, // get_protocol
    nullptr, // open
    nullptr, // openat
    nullptr, // close
    nullptr, // unbind
    nullptr, // release
    nullptr, // read
    nullptr, // write
    nullptr, // iotxn_queue
    nullptr, // get_size
    tpm_device_ioctl,
    nullptr, // suspend
    nullptr, // resume
    nullptr, // rxrpc
};


//TODO: bind against hw, not misc
zx_status_t tpm_bind(void* ctx, zx_device_t* parent, void** cookie) {
#if defined(__x86_64__) || defined(__i386__)
    zx_status_t status = io_buffer_init_physical(&io_buffer, TPM_PHYS_ADDRESS, TPM_PHYS_LENGTH,
                                                 get_root_resource(), ZX_CACHE_POLICY_UNCACHED);
    if (status != ZX_OK) {
        return status;
    }
    tpm_base = io_buffer_virt(&io_buffer);

    status = tpm_is_supported(LOCALITY0);
    if (status != ZX_OK) {
        return status;
    }

    device_add_args_t args;
    memset(&args, 0, sizeof(args));
    args.version = DEVICE_ADD_ARGS_VERSION;
    args.name = "tpm";
    args.ops = &tpm_device_proto;
    args.proto_id = ZX_PROTOCOL_TPM;

    zx_device_t* dev;
    status =  device_add(parent, &args, &dev);
    if (status != ZX_OK) {
        return status;
    }

    uint8_t buf[32] = { 0 };
    size_t bytes_read;

    // tpm_request_use will fail if we're not at least 30ms past _TPM_INIT.
    // The system firmware performs the init, so it's safe to assume that
    // is 30 ms past.  If we're on systems where we need to do init,
    // we need to wait up to 30ms for the TPM_ACCESS register to be valid.
    status = tpm_request_use(LOCALITY0);
    if (status != ZX_OK) {
        goto cleanup_device;
    }

    status = tpm_wait_for_locality(LOCALITY0);
    if (status != ZX_OK) {
        goto cleanup_device;
    }

    // Configure interrupts
    status = tpm_set_irq(LOCALITY0, 10);
    if (status != ZX_OK) {
        goto cleanup_device;
    }

    status = zx_interrupt_create(get_root_resource(), 10, ZX_INTERRUPT_REMAP_IRQ, &irq_handle);
    if (status != ZX_OK) {
        goto cleanup_device;
    }

    status = tpm_enable_irq_type(LOCALITY0, IRQ_DATA_AVAIL);
    if (status < 0) {
        goto cleanup_device;
    }
    status = tpm_enable_irq_type(LOCALITY0, IRQ_LOCALITY_CHANGE);
    if (status < 0) {
        goto cleanup_device;
    }

    // Make a best-effort attempt to give the kernel some more entropy
    // TODO(security): Perform a more recurring seeding
    status = tpm_get_random(dev, buf, static_cast<uint32_t>(sizeof(buf)), &bytes_read);
    if (status == ZX_OK) {
        zx_cprng_add_entropy(buf, bytes_read);
        memset(buf, 0, sizeof(buf));
    }

    return ZX_OK;

cleanup_device:
    if (irq_handle > 0) {
        zx_handle_close(irq_handle);
    }
    device_remove(dev);
    return status;
#else
    return ZX_ERR_NOT_SUPPORTED;
#endif
}

