// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddk/device.h>
#include <zircon/types.h>
#include <stddef.h>
#include <stdint.h>

extern void *tpm_base;
extern zx_handle_t irq_handle;

enum locality {
    LOCALITY0,
    LOCALITY1,
    LOCALITY2,
    LOCALITY3,
    LOCALITY4,
};

enum irq_type {
    IRQ_DATA_AVAIL = 0x01,
    IRQ_LOCALITY_CHANGE = 0x04,
};

enum tpm_result {
    TPM_SUCCESS = 0x0,
    TPM_BAD_PARAMETER = 0x3,
    TPM_DEACTIVATED = 0x6,
    TPM_DISABLED = 0x7,
    TPM_DISABLED_CMD = 0x8,
    TPM_FAIL = 0x9,
    TPM_BAD_ORDINAL = 0xa,
    TPM_RETRY = 0x800,
};

__BEGIN_CDECLS
zx_status_t tpm_bind(void* ctx, zx_device_t* parent, void** cookie);
__END_CDECLS

zx_status_t tpm_set_irq(enum locality loc, uint8_t vector);
zx_status_t tpm_enable_irq_type(enum locality loc, enum irq_type type);
zx_status_t tpm_disable_irq_type(enum locality loc, enum irq_type type);

// Returns ZX_OK if this TPM is supported and returns ZX_NOT_SUPPORTED if it is not.
// Any other error indicates an issue occurred during detection.
zx_status_t tpm_is_supported(enum locality loc);

// Request use of the given locality
zx_status_t tpm_request_use(enum locality loc);
// Wait for actual access to the locality
zx_status_t tpm_wait_for_locality(enum locality loc);

zx_status_t tpm_send_cmd(enum locality loc, uint8_t* cmd, size_t len);
// |actual| will contain the total number of bytes in the response, may be less than max_len.
zx_status_t tpm_recv_resp(enum locality loc, uint8_t* resp, size_t max_len, size_t* actual);
