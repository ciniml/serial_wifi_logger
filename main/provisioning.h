/*
 * SPDX-FileCopyrightText: 2026 Kenta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "freertos/event_groups.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize provisioning manager
 *
 * Must be called before is_provisioned() or start_provisioning()
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t init_provisioning_manager(void);

/**
 * @brief Start WiFi provisioning with SoftAP scheme
 *
 * @param event_group Event group to signal completion
 * @param success_bit Bit to set on successful provisioning
 * @param fail_bit Bit to set on failed provisioning
 * @return esp_err_t ESP_OK on success
 */
esp_err_t start_provisioning(EventGroupHandle_t *event_group,
                              int success_bit, int fail_bit);

/**
 * @brief Check if WiFi credentials are provisioned
 *
 * Note: init_provisioning_manager() must be called first
 *
 * @return true if provisioned, false otherwise
 */
bool is_provisioned(void);

#ifdef __cplusplus
}
#endif
