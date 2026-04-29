/*
 * Web-UI settings: REST endpoints for runtime control of WireGuard and
 * Tailscale. The endpoints persist their inputs in the existing NVS
 * namespaces ("tailscale", "wireguard") and trigger stop/start cycles
 * on the corresponding service.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "web_settings.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#ifdef CONFIG_TAILSCALE_ENABLE
#include "tailscale_esp32.h"
#endif
#ifdef CONFIG_WIREGUARD_ENABLE
#include "wireguard_esp32.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Serializes config-mutation handlers (PUT /api/tailscale, /api/wireguard).
 * The HTTP server has multiple worker threads, so concurrent PUTs against
 * the same service would otherwise race the stop/save/start sequence. */
static SemaphoreHandle_t s_config_mutex = NULL;

/* Background reconfigure task. We don't perform stop/start synchronously
 * inside the HTTP handler because tailscale_esp32_stop() can take seconds
 * (TLS teardown + control-task termination), and esp_http_server has hard
 * recv/send timeouts that would surface as a connection drop to the user. */
typedef enum {
    RECFG_NONE = 0,
    RECFG_TAILSCALE,
    RECFG_WIREGUARD,
} reconfig_kind_t;

typedef struct {
    reconfig_kind_t kind;
    bool            enable;   /* desired post-restart state */
} reconfig_req_t;

static QueueHandle_t s_reconfig_queue = NULL;

static const char *TAG = "web_settings";

/* ------------------------------------------------------------------ */
/* NVS helpers — small wrappers around the typed nvs_get / nvs_set     */
/* APIs so callers can express simple "read bool, default X" semantics.*/
/* ------------------------------------------------------------------ */

bool web_settings_get_enabled(const char *namespace_, bool default_value)
{
    nvs_handle_t h;
    if (nvs_open(namespace_, NVS_READONLY, &h) != ESP_OK) {
        return default_value;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, "enabled", &v);
    nvs_close(h);
    if (err != ESP_OK) return default_value;
    return v != 0;
}

/* ------------------------------------------------------------------ */
/* Status helpers                                                       */
/* ------------------------------------------------------------------ */

static void wifi_get_ip(char *ip_out, size_t ip_out_len)
{
    ip_out[0] = '\0';
    /* Try the WiFi STA netif first; fall back to Ethernet (QEMU). */
    static const char * const ifkeys[] = { "WIFI_STA_DEF", "ETH_DEF", NULL };
    for (int i = 0; ifkeys[i]; i++) {
        esp_netif_t *n = esp_netif_get_handle_from_ifkey(ifkeys[i]);
        if (!n) continue;
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(n, &info) != ESP_OK) continue;
        if (info.ip.addr == 0) continue;
        snprintf(ip_out, ip_out_len, IPSTR, IP2STR(&info.ip));
        return;
    }
}

/* ------------------------------------------------------------------ */
/* NVS string read/write convenience                                    */
/* ------------------------------------------------------------------ */

static esp_err_t nvs_read_str(const char *ns, const char *key,
                              char *out, size_t out_size, const char *fallback)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = out_size;
        esp_err_t err = nvs_get_str(h, key, out, &sz);
        nvs_close(h);
        if (err == ESP_OK) return ESP_OK;
    }
    if (fallback) strlcpy(out, fallback, out_size);
    return ESP_ERR_NOT_FOUND;
}

/* Single-shot NVS writer that opens, sets, commits, closes. */
static esp_err_t nvs_write_str(const char *ns, const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_write_u8(const char *ns, const char *key, uint8_t value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Read the entire request body into a heap buffer (NUL-terminated). */
static esp_err_t recv_body(httpd_req_t *req, char **out_buf, size_t *out_len)
{
    if (req->content_len == 0 || req->content_len > 4096) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t read_total = 0;
    while (read_total < req->content_len) {
        int n = httpd_req_recv(req, buf + read_total,
                               req->content_len - read_total);
        if (n <= 0) { free(buf); return ESP_FAIL; }
        read_total += n;
    }
    buf[read_total] = '\0';
    *out_buf = buf;
    *out_len = read_total;
    return ESP_OK;
}

/* JSON error reply with a one-line message. */
static esp_err_t send_json_error(httpd_req_t *req, int status_code,
                                 const char *message)
{
    char body[160];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}",
                     message ? message : "error");
    if (n < 0) n = 0;
    httpd_resp_set_type(req, "application/json");
    /* Map common 4xx/5xx codes onto esp_http_server's preset constants. */
    switch (status_code) {
        case 400: httpd_resp_set_status(req, "400 Bad Request"); break;
        case 409: httpd_resp_set_status(req, "409 Conflict");    break;
        case 500: httpd_resp_set_status(req, "500 Internal Server Error"); break;
        default:  httpd_resp_set_status(req, "400 Bad Request"); break;
    }
    return httpd_resp_send(req, body, n);
}

/* "tskey-auth-XXXXXXXXXXXX...zenZ" → "tskey-auth-...zenZ". Returns the
 * full string if it's already short enough to be uninteresting. */
static void mask_auth_key(const char *src, char *dst, size_t dst_size)
{
    size_t len = strlen(src);
    if (len < 16 || dst_size < 24) {
        strlcpy(dst, "", dst_size);
        return;
    }
    /* Keep the "tskey-auth-" prefix (11 chars) and the last 4 characters. */
    snprintf(dst, dst_size, "%.11s...%s", src, src + len - 4);
}

/* ------------------------------------------------------------------ */
/* Background reconfigure worker                                        */
/* ------------------------------------------------------------------ */

static void reconfig_task(void *arg)
{
    (void)arg;
    reconfig_req_t r;
    while (xQueueReceive(s_reconfig_queue, &r, portMAX_DELAY) == pdTRUE) {
        /* Give the HTTP handler a moment to finish flushing its 200/202
         * response before we tear down a netif that the response itself
         * may be travelling over (e.g. when the request arrived through
         * the Tailscale IP we are about to disable). */
        vTaskDelay(pdMS_TO_TICKS(500));

        xSemaphoreTake(s_config_mutex, portMAX_DELAY);

        switch (r.kind) {
#ifdef CONFIG_TAILSCALE_ENABLE
        case RECFG_TAILSCALE:
            ESP_LOGI(TAG, "reconfigure: tailscale enable=%d", (int)r.enable);
            tailscale_esp32_stop();
            if (r.enable) {
                esp_err_t err = tailscale_esp32_start(NULL);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "tailscale_esp32_start: %s",
                             esp_err_to_name(err));
                }
            }
            break;
#endif
#ifdef CONFIG_WIREGUARD_ENABLE
        case RECFG_WIREGUARD:
            ESP_LOGI(TAG, "reconfigure: wireguard enable=%d", (int)r.enable);
            wireguard_esp32_stop();
            if (r.enable) {
                esp_err_t err = wireguard_esp32_start(NULL);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "wireguard_esp32_start: %s",
                             esp_err_to_name(err));
                }
            }
            break;
#endif
        default:
            break;
        }

        xSemaphoreGive(s_config_mutex);
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* GET /api/network/status                                              */
/* ------------------------------------------------------------------ */

static esp_err_t handler_status_get(httpd_req_t *req)
{
    char wifi_ip[16] = {0};
    wifi_get_ip(wifi_ip, sizeof(wifi_ip));

    bool ts_enabled = false, ts_running = false;
    char ts_ip[24] = {0};
#ifdef CONFIG_TAILSCALE_ENABLE
    ts_enabled = web_settings_get_enabled("tailscale", true);
    ts_running = tailscale_esp32_is_connected();
    if (ts_running) tailscale_esp32_get_ip(ts_ip, sizeof(ts_ip));
#endif

    bool wg_enabled = false, wg_running = false;
#ifdef CONFIG_WIREGUARD_ENABLE
    wg_enabled = web_settings_get_enabled("wireguard", false);
    wg_running = wireguard_esp32_is_peer_up();
#endif

    char body[256];
    int n = snprintf(body, sizeof(body),
        "{"
          "\"wifi\":{\"connected\":%s,\"ip\":\"%s\"},"
          "\"tailscale\":{\"enabled\":%s,\"running\":%s,\"ip\":\"%s\"},"
          "\"wireguard\":{\"enabled\":%s,\"running\":%s}"
        "}",
        wifi_ip[0] ? "true" : "false", wifi_ip,
        ts_enabled ? "true" : "false",
        ts_running ? "true" : "false",
        ts_ip,
        wg_enabled ? "true" : "false",
        wg_running ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "status formatting overflow");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

/* ------------------------------------------------------------------ */
/* GET /api/tailscale                                                   */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_TAILSCALE_ENABLE
static esp_err_t handler_tailscale_get(httpd_req_t *req)
{
    char auth_key[160] = {0};
    char hostname[64]  = {0};
    char ctrl_srv[96]  = {0};
    nvs_read_str("tailscale", "auth_key",    auth_key, sizeof(auth_key),
                 CONFIG_TAILSCALE_AUTH_KEY);
    nvs_read_str("tailscale", "hostname",    hostname, sizeof(hostname),
                 CONFIG_TAILSCALE_HOSTNAME);
    nvs_read_str("tailscale", "ctrl_server", ctrl_srv, sizeof(ctrl_srv),
                 CONFIG_TAILSCALE_CONTROL_SERVER);

    bool enabled = web_settings_get_enabled("tailscale", true);
    char hint[40] = {0};
    bool key_set = auth_key[0] != '\0';
    if (key_set) mask_auth_key(auth_key, hint, sizeof(hint));

    char body[400];
    int n = snprintf(body, sizeof(body),
        "{"
          "\"enabled\":%s,"
          "\"hostname\":\"%s\","
          "\"control_server\":\"%s\","
          "\"auth_key_set\":%s,"
          "\"auth_key_hint\":\"%s\""
        "}",
        enabled ? "true" : "false",
        hostname, ctrl_srv,
        key_set ? "true" : "false",
        hint);
    if (n < 0 || n >= (int)sizeof(body)) {
        return send_json_error(req, 500, "tailscale config formatting overflow");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

/* ------------------------------------------------------------------ */
/* PUT /api/tailscale                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t handler_tailscale_put(httpd_req_t *req)
{
    char  *body = NULL;
    size_t body_len = 0;
    esp_err_t err = recv_body(req, &body, &body_len);
    if (err != ESP_OK) {
        return send_json_error(req, 400,
                               err == ESP_ERR_INVALID_SIZE
                                   ? "request body missing or too large"
                                   : "failed to read body");
    }

    cJSON *root = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (!root) return send_json_error(req, 400, "invalid JSON");

    /* Take the global config mutex so a second concurrent PUT can't race
     * the stop/save/start sequence. */
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(8000)) != pdTRUE) {
        cJSON_Delete(root);
        return send_json_error(req, 409, "another config change is in progress");
    }

    /* --- Validate + persist ---------------------------------------- */
    bool requested_enabled = web_settings_get_enabled("tailscale", true);
    cJSON *en = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (cJSON_IsBool(en)) requested_enabled = cJSON_IsTrue(en);

    cJSON *ak = cJSON_GetObjectItemCaseSensitive(root, "auth_key");
    if (cJSON_IsString(ak) && ak->valuestring && ak->valuestring[0]) {
        if (strncmp(ak->valuestring, "tskey-", 6) != 0) {
            xSemaphoreGive(s_config_mutex);
            cJSON_Delete(root);
            return send_json_error(req, 400, "auth_key: must start with tskey-");
        }
        nvs_write_str("tailscale", "auth_key", ak->valuestring);
    }
    cJSON *hn = cJSON_GetObjectItemCaseSensitive(root, "hostname");
    if (cJSON_IsString(hn) && hn->valuestring && hn->valuestring[0]) {
        size_t L = strlen(hn->valuestring);
        if (L > 63) {
            xSemaphoreGive(s_config_mutex);
            cJSON_Delete(root);
            return send_json_error(req, 400, "hostname: too long (max 63)");
        }
        nvs_write_str("tailscale", "hostname", hn->valuestring);
    }
    cJSON *cs = cJSON_GetObjectItemCaseSensitive(root, "control_server");
    if (cJSON_IsString(cs) && cs->valuestring && cs->valuestring[0]) {
        nvs_write_str("tailscale", "ctrl_server", cs->valuestring);
    }

    nvs_write_u8("tailscale", "enabled", requested_enabled ? 1 : 0);

#ifdef CONFIG_WIREGUARD_ENABLE
    /* Mutual exclusion: enabling Tailscale forces WG manual off. */
    if (requested_enabled && web_settings_get_enabled("wireguard", false)) {
        nvs_write_u8("wireguard", "enabled", 0);
        if (wireguard_esp32_is_peer_up()) {
            wireguard_esp32_stop();
        }
    }
#endif

    cJSON_Delete(root);
    xSemaphoreGive(s_config_mutex);

    /* --- Hand off the apply step to the background worker.
     * The handler must return quickly so the HTTP socket stays alive even
     * if the stop/start cycle takes several seconds. Clients should poll
     * /api/network/status to observe the new state. */
    reconfig_req_t r = { .kind = RECFG_TAILSCALE, .enable = requested_enabled };
    if (s_reconfig_queue == NULL ||
        xQueueSend(s_reconfig_queue, &r, pdMS_TO_TICKS(100)) != pdTRUE) {
        return send_json_error(req, 500, "reconfigure queue full");
    }

    ESP_LOGI(TAG, "PUT /api/tailscale: queued reconfigure (enable=%d)",
             (int)requested_enabled);

    char body_out[96];
    int n = snprintf(body_out, sizeof(body_out),
                     "{\"ok\":true,\"requested_enabled\":%s}",
                     requested_enabled ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body_out, n);
}
#endif /* CONFIG_TAILSCALE_ENABLE */

/* ------------------------------------------------------------------ */
/* GET /api/wireguard                                                   */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_WIREGUARD_ENABLE
static esp_err_t handler_wireguard_get(httpd_req_t *req)
{
    char local_ip[20]      = {0};
    char local_netmask[20] = {0};
    char local_gateway[20] = {0};
    char peer_pub[64]      = {0};
    char peer_ep[80]       = {0};
    char ntp_server[64]    = {0};
    char private_key[64]   = {0};

    nvs_read_str("wireguard", "local_ip",      local_ip,      sizeof(local_ip),      CONFIG_WIREGUARD_LOCAL_IP);
    nvs_read_str("wireguard", "local_netmask", local_netmask, sizeof(local_netmask), CONFIG_WIREGUARD_LOCAL_NETMASK);
    nvs_read_str("wireguard", "local_gateway", local_gateway, sizeof(local_gateway), "0.0.0.0");
    nvs_read_str("wireguard", "private_key",   private_key,   sizeof(private_key),   CONFIG_WIREGUARD_PRIVATE_KEY);
    nvs_read_str("wireguard", "peer_pub_key",  peer_pub,      sizeof(peer_pub),      CONFIG_WIREGUARD_PEER_PUBLIC_KEY);
    nvs_read_str("wireguard", "peer_endpoint", peer_ep,       sizeof(peer_ep),       CONFIG_WIREGUARD_PEER_ENDPOINT);
    nvs_read_str("wireguard", "ntp_server",    ntp_server,    sizeof(ntp_server),    CONFIG_WIREGUARD_NTP_SERVER);

    /* u16 / u8 numeric fields with Kconfig fallbacks. */
    uint16_t peer_port   = CONFIG_WIREGUARD_PEER_PORT;
    uint16_t listen_port = CONFIG_WIREGUARD_LISTEN_PORT;
    uint16_t keepalive   = CONFIG_WIREGUARD_KEEPALIVE;
    uint8_t  set_default = 0;
    bool     psk_set     = false;
    nvs_handle_t h;
    if (nvs_open("wireguard", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, "peer_port",   &peer_port);
        nvs_get_u16(h, "listen_port", &listen_port);
        nvs_get_u16(h, "keepalive",   &keepalive);
        nvs_get_u8 (h, "set_default", &set_default);
        size_t psk_len = 32;
        uint8_t psk[32];
        psk_set = (nvs_get_blob(h, "preshared_key", psk, &psk_len) == ESP_OK
                   && psk_len == 32);
        nvs_close(h);
    }

    bool enabled = web_settings_get_enabled("wireguard", false);
    bool priv_set = private_key[0] != '\0';

    char body[640];
    int n = snprintf(body, sizeof(body),
        "{"
          "\"enabled\":%s,"
          "\"local_ip\":\"%s\","
          "\"local_netmask\":\"%s\","
          "\"local_gateway\":\"%s\","
          "\"peer_pub_key\":\"%s\","
          "\"peer_endpoint\":\"%s\","
          "\"peer_port\":%u,"
          "\"listen_port\":%u,"
          "\"keepalive\":%u,"
          "\"set_default\":%s,"
          "\"ntp_server\":\"%s\","
          "\"private_key_set\":%s,"
          "\"preshared_key_set\":%s"
        "}",
        enabled ? "true" : "false",
        local_ip, local_netmask, local_gateway,
        peer_pub, peer_ep,
        peer_port, listen_port, keepalive,
        set_default ? "true" : "false",
        ntp_server,
        priv_set ? "true" : "false",
        psk_set  ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(body)) {
        return send_json_error(req, 500, "wireguard config formatting overflow");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

/* ------------------------------------------------------------------ */
/* PUT /api/wireguard                                                   */
/* ------------------------------------------------------------------ */

/* Decode a hex string into a 32-byte buffer. Returns true on success. */
static bool hex32_decode(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        unsigned int v = 0;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static esp_err_t handler_wireguard_put(httpd_req_t *req)
{
    char  *body = NULL;
    size_t body_len = 0;
    esp_err_t err = recv_body(req, &body, &body_len);
    if (err != ESP_OK) {
        return send_json_error(req, 400,
                               err == ESP_ERR_INVALID_SIZE
                                   ? "request body missing or too large"
                                   : "failed to read body");
    }
    cJSON *root = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (!root) return send_json_error(req, 400, "invalid JSON");

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(8000)) != pdTRUE) {
        cJSON_Delete(root);
        return send_json_error(req, 409, "another config change is in progress");
    }

    /* enabled flag */
    bool requested_enabled = web_settings_get_enabled("wireguard", false);
    cJSON *en = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (cJSON_IsBool(en)) requested_enabled = cJSON_IsTrue(en);

    /* string fields — only update if present + non-empty */
    static const struct { const char *k; size_t maxlen; } str_fields[] = {
        { "local_ip",      19 },
        { "local_netmask", 19 },
        { "local_gateway", 19 },
        { "private_key",   63 },
        { "peer_pub_key",  63 },
        { "peer_endpoint", 79 },
        { "ntp_server",    63 },
    };
    for (size_t i = 0; i < sizeof(str_fields)/sizeof(str_fields[0]); i++) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(root, str_fields[i].k);
        if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
            if (strlen(v->valuestring) > str_fields[i].maxlen) {
                xSemaphoreGive(s_config_mutex);
                cJSON_Delete(root);
                char m[64];
                snprintf(m, sizeof(m), "%s: too long", str_fields[i].k);
                return send_json_error(req, 400, m);
            }
            nvs_write_str("wireguard", str_fields[i].k, v->valuestring);
        }
    }

    /* numeric fields */
    nvs_handle_t h;
    if (nvs_open("wireguard", NVS_READWRITE, &h) == ESP_OK) {
        cJSON *v;
        if (cJSON_IsNumber((v = cJSON_GetObjectItemCaseSensitive(root, "peer_port"))))
            nvs_set_u16(h, "peer_port",   (uint16_t)v->valuedouble);
        if (cJSON_IsNumber((v = cJSON_GetObjectItemCaseSensitive(root, "listen_port"))))
            nvs_set_u16(h, "listen_port", (uint16_t)v->valuedouble);
        if (cJSON_IsNumber((v = cJSON_GetObjectItemCaseSensitive(root, "keepalive"))))
            nvs_set_u16(h, "keepalive",   (uint16_t)v->valuedouble);
        if (cJSON_IsBool((v = cJSON_GetObjectItemCaseSensitive(root, "set_default"))))
            nvs_set_u8 (h, "set_default", cJSON_IsTrue(v) ? 1 : 0);

        /* Optional preshared key (hex). Empty string clears it. */
        cJSON *psk = cJSON_GetObjectItemCaseSensitive(root, "preshared_key");
        if (cJSON_IsString(psk) && psk->valuestring) {
            if (psk->valuestring[0] == '\0') {
                nvs_erase_key(h, "preshared_key");
            } else {
                uint8_t blob[32];
                if (!hex32_decode(psk->valuestring, blob)) {
                    nvs_close(h);
                    xSemaphoreGive(s_config_mutex);
                    cJSON_Delete(root);
                    return send_json_error(req, 400,
                        "preshared_key: must be 64 hex chars");
                }
                nvs_set_blob(h, "preshared_key", blob, sizeof(blob));
            }
        }
        nvs_commit(h);
        nvs_close(h);
    }

    nvs_write_u8("wireguard", "enabled", requested_enabled ? 1 : 0);

#ifdef CONFIG_TAILSCALE_ENABLE
    if (requested_enabled && web_settings_get_enabled("tailscale", true)) {
        nvs_write_u8("tailscale", "enabled", 0);
        if (tailscale_esp32_is_connected()) {
            tailscale_esp32_stop();
        }
    }
#endif

    cJSON_Delete(root);
    xSemaphoreGive(s_config_mutex);

    reconfig_req_t r = { .kind = RECFG_WIREGUARD, .enable = requested_enabled };
    if (s_reconfig_queue == NULL ||
        xQueueSend(s_reconfig_queue, &r, pdMS_TO_TICKS(100)) != pdTRUE) {
        return send_json_error(req, 500, "reconfigure queue full");
    }

    ESP_LOGI(TAG, "PUT /api/wireguard: queued reconfigure (enable=%d)",
             (int)requested_enabled);
    char body_out[96];
    int n = snprintf(body_out, sizeof(body_out),
                     "{\"ok\":true,\"requested_enabled\":%s}",
                     requested_enabled ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body_out, n);
}
#endif /* CONFIG_WIREGUARD_ENABLE */

/* ------------------------------------------------------------------ */
/* Registration                                                         */
/* ------------------------------------------------------------------ */

esp_err_t web_settings_register(httpd_handle_t server)
{
    if (!server) return ESP_ERR_INVALID_ARG;

    if (!s_config_mutex) {
        s_config_mutex = xSemaphoreCreateMutex();
        if (!s_config_mutex) return ESP_ERR_NO_MEM;
    }
    if (!s_reconfig_queue) {
        s_reconfig_queue = xQueueCreate(4, sizeof(reconfig_req_t));
        if (!s_reconfig_queue) return ESP_ERR_NO_MEM;
        BaseType_t ok = xTaskCreate(reconfig_task, "web_recfg", 8192, NULL,
                                    5, NULL);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "reconfig_task create failed");
            return ESP_FAIL;
        }
    }

    static const httpd_uri_t uri_status = {
        .uri      = "/api/network/status",
        .method   = HTTP_GET,
        .handler  = handler_status_get,
        .user_ctx = NULL,
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri_status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/network/status failed: %s",
                 esp_err_to_name(err));
        return err;
    }

#ifdef CONFIG_TAILSCALE_ENABLE
    static const httpd_uri_t uri_ts_get = {
        .uri = "/api/tailscale", .method = HTTP_GET,
        .handler = handler_tailscale_get, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_ts_put = {
        .uri = "/api/tailscale", .method = HTTP_PUT,
        .handler = handler_tailscale_put, .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &uri_ts_get);
    httpd_register_uri_handler(server, &uri_ts_put);
#endif

#ifdef CONFIG_WIREGUARD_ENABLE
    static const httpd_uri_t uri_wg_get = {
        .uri = "/api/wireguard", .method = HTTP_GET,
        .handler = handler_wireguard_get, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_wg_put = {
        .uri = "/api/wireguard", .method = HTTP_PUT,
        .handler = handler_wireguard_put, .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &uri_wg_get);
    httpd_register_uri_handler(server, &uri_wg_put);
#endif

    ESP_LOGI(TAG, "Web settings endpoints registered");
    return ESP_OK;
}
