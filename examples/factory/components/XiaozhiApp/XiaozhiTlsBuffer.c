/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
// Use the matching IDF metadata so buffer ownership stays with mbedTLS.
#include "esp_mbedtls_dynamic_impl.h"

int __real_esp_mbedtls_free_tx_buffer(mbedtls_ssl_context *ssl);

int __wrap_esp_mbedtls_free_tx_buffer(mbedtls_ssl_context *ssl)
{
    const char *task_name = pcTaskGetName(NULL);
    if (ssl && task_name && strncmp(task_name, "xiaozhi_", 8) == 0 &&
            mbedtls_ssl_is_handshake_over(ssl) && ssl->MBEDTLS_PRIVATE(out_buf)) {
        const struct esp_mbedtls_ssl_buf *buffer = (const void *)(
            ssl->MBEDTLS_PRIVATE(out_buf) - SSL_BUF_HEAD_OFFSET_SIZE);
        // Keep a full TX buffer after the HTTP upgrade, before audio starts.
        // An idle cache is too small for TLS application records.
        if (buffer->state == ESP_MBEDTLS_SSL_BUF_CACHED &&
                buffer->len >= MBEDTLS_SSL_OUT_BUFFER_LEN) {
            return 0;
        }
    }
    // Handshakes and unrelated clients retain IDF's dynamic-buffer policy.
    return __real_esp_mbedtls_free_tx_buffer(ssl);
}
#endif
