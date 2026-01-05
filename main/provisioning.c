/*
 * SPDX-FileCopyrightText: 2026 Kenta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <network_provisioning/manager.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "provisioning.h"

static const char *TAG = "provisioning";

extern const network_prov_scheme_t network_prov_scheme_softap;

struct prov_context {
    EventGroupHandle_t *event_group;
    int success_bit;
    int fail_bit;
    bool success;
};

static void prov_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    struct prov_context *ctx = arg;

    switch (event_id) {
    case NETWORK_PROV_START:
        ESP_LOGI(TAG, "Provisioning started");
        ESP_LOGI(TAG, "Connect to SoftAP and access provisioning page");
        break;

    case NETWORK_PROV_WIFI_CRED_RECV: {
        wifi_sta_config_t *wifi_cfg = (wifi_sta_config_t *)event_data;
        break;
    }

    case NETWORK_PROV_WIFI_CRED_SUCCESS:
        ESP_LOGI(TAG, "WiFi provisioning successful");
        ctx->success = true;
        break;

    case NETWORK_PROV_WIFI_CRED_FAIL: {
        network_prov_wifi_sta_fail_reason_t *reason = event_data;
        ESP_LOGE(TAG, "Provisioning failed: %s",
                 (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) ?
                 "Authentication failed" : "AP not found");
        ctx->success = false;
        break;
    }

    case NETWORK_PROV_END:
        ESP_LOGI(TAG, "Provisioning ended");
        network_prov_mgr_deinit();
        xEventGroupSetBits(*ctx->event_group,
                           ctx->success ? ctx->success_bit : ctx->fail_bit);
        free(ctx);
        break;

    default:
        break;
    }
}

esp_err_t init_provisioning_manager(void)
{
    // Initialize provisioning manager with SoftAP scheme
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE
    };
    return network_prov_mgr_init(config);
}

esp_err_t start_provisioning(EventGroupHandle_t *event_group,
                              int success_bit, int fail_bit)
{
    // Allocate context
    struct prov_context *ctx = malloc(sizeof(struct prov_context));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->event_group = event_group;
    ctx->success_bit = success_bit;
    ctx->fail_bit = fail_bit;
    ctx->success = false;

    // Register event handler
    ESP_ERROR_CHECK(esp_event_handler_register(
        NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
        prov_event_handler, ctx));

    // Security Level 1 with PoP
    network_prov_security_t security = NETWORK_PROV_SECURITY_1;
    const char *pop = CONFIG_PROV_POP_CODE;
    network_prov_security1_params_t *sec_params = (void *)pop;

    // Generate SoftAP SSID with device MAC
    char service_name[32];
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(service_name, sizeof(service_name), "PROV_%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "Starting SoftAP provisioning");
    ESP_LOGI(TAG, "SoftAP SSID: %s", service_name);
    ESP_LOGI(TAG, "PoP code: %s", pop);

    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
        security, (const void *)sec_params, service_name, NULL));

    return ESP_OK;
}

bool is_provisioned(void)
{
    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));
    return provisioned;
}
