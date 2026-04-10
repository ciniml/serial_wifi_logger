/*
 * WireGuard ESP-IDF 6.0 integration layer
 * Ported from WireGuard-ESP32-Arduino by Kenta Ida (fuga@fugafuga.org)
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "wireguard_esp32.h"
#include "wireguardif.h"
#include "wireguard-platform.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif_ip_addr.h"  /* IPSTR / IP2STR */
#include "nvs.h"

#include "lwip/ip.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"    /* tcpip_input declaration */

/* When CONFIG_WIREGUARD_SET_AS_DEFAULT_ROUTE is 'default n', Kconfig leaves
 * it undefined. Provide a 0 fallback so code can reference it directly. */
#ifndef CONFIG_WIREGUARD_SET_AS_DEFAULT_ROUTE
#define CONFIG_WIREGUARD_SET_AS_DEFAULT_ROUTE 0
#endif

#include <string.h>
#include <assert.h>

static const char *TAG = "wireguard";

/* --------------------------------------------------------------------------
 * Static state
 * -------------------------------------------------------------------------- */
static struct netif  s_wg_netif_struct = {0};
static struct netif *s_wg_netif = NULL;
static struct netif *s_prev_default_netif = NULL;
static uint8_t       s_peer_index = WIREGUARDIF_INVALID_INDEX;
static bool          s_initialized = false;
static bool          s_set_as_default = false;

/* --------------------------------------------------------------------------
 * NVS config loading
 * Static buffers keep string pointers alive for the tunnel lifetime.
 * -------------------------------------------------------------------------- */
#define NVS_NAMESPACE       "wireguard"
#define NVS_KEY_LOCAL_IP    "local_ip"
#define NVS_KEY_NETMASK     "local_netmask"
#define NVS_KEY_GATEWAY     "local_gateway"
#define NVS_KEY_PRIV_KEY    "private_key"
#define NVS_KEY_PEER_PUBKEY "peer_pub_key"
#define NVS_KEY_ENDPOINT    "peer_endpoint"
#define NVS_KEY_PEER_PORT   "peer_port"
#define NVS_KEY_LISTEN_PORT "listen_port"
#define NVS_KEY_KEEPALIVE   "keepalive"
#define NVS_KEY_SET_DEFAULT "set_default"
#define NVS_KEY_PSK         "preshared_key"

static char s_local_ip[16];
static char s_local_netmask[16];
static char s_local_gateway[16];
static char s_private_key[64];       /* base64 WG key = 44 chars + NUL, with margin */
static char s_peer_public_key[64];
static char s_peer_endpoint[64];
static uint8_t s_preshared_key[32];
static bool s_has_preshared_key = false;

static void load_config_from_nvs(wireguard_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);

    /* Helper macros: try NVS first, fall back to kconfig default */
#define LOAD_STR(buf, key, kdefault) do {                                   \
        size_t _sz = sizeof(buf);                                           \
        if (err == ESP_OK && nvs_get_str(h, key, buf, &_sz) == ESP_OK) {   \
            /* NVS value loaded */                                           \
        } else {                                                             \
            strlcpy(buf, kdefault, sizeof(buf));                            \
        }                                                                    \
    } while (0)

#define LOAD_U16(field, key, kdefault) do {                                 \
        uint16_t _v;                                                        \
        if (err == ESP_OK && nvs_get_u16(h, key, &_v) == ESP_OK) {         \
            field = _v;                                                     \
        } else {                                                             \
            field = (uint16_t)(kdefault);                                   \
        }                                                                    \
    } while (0)

    LOAD_STR(s_local_ip,        NVS_KEY_LOCAL_IP,    CONFIG_WIREGUARD_LOCAL_IP);
    LOAD_STR(s_local_netmask,   NVS_KEY_NETMASK,     CONFIG_WIREGUARD_LOCAL_NETMASK);
    LOAD_STR(s_local_gateway,   NVS_KEY_GATEWAY,     "0.0.0.0");
    LOAD_STR(s_private_key,     NVS_KEY_PRIV_KEY,    CONFIG_WIREGUARD_PRIVATE_KEY);
    LOAD_STR(s_peer_public_key, NVS_KEY_PEER_PUBKEY, CONFIG_WIREGUARD_PEER_PUBLIC_KEY);
    LOAD_STR(s_peer_endpoint,   NVS_KEY_ENDPOINT,    CONFIG_WIREGUARD_PEER_ENDPOINT);

    LOAD_U16(cfg->peer_port,   NVS_KEY_PEER_PORT,   CONFIG_WIREGUARD_PEER_PORT);
    LOAD_U16(cfg->listen_port, NVS_KEY_LISTEN_PORT, CONFIG_WIREGUARD_LISTEN_PORT);
    LOAD_U16(cfg->keepalive,   NVS_KEY_KEEPALIVE,   CONFIG_WIREGUARD_KEEPALIVE);

    /* set_as_default: NVS u8, then Kconfig bool */
    {
        uint8_t sd = 0;
        if (err == ESP_OK && nvs_get_u8(h, NVS_KEY_SET_DEFAULT, &sd) == ESP_OK) {
            cfg->set_as_default = (sd != 0);
        } else {
            cfg->set_as_default = CONFIG_WIREGUARD_SET_AS_DEFAULT_ROUTE;
        }
    }

    /* Optional 32-byte preshared key blob */
    s_has_preshared_key = false;
    if (err == ESP_OK) {
        size_t psk_len = sizeof(s_preshared_key);
        if (nvs_get_blob(h, NVS_KEY_PSK, s_preshared_key, &psk_len) == ESP_OK
                && psk_len == sizeof(s_preshared_key)) {
            s_has_preshared_key = true;
        }
    }

    cfg->local_ip       = s_local_ip;
    cfg->local_netmask  = s_local_netmask;
    cfg->local_gateway  = s_local_gateway;
    cfg->private_key    = s_private_key;
    cfg->peer_public_key = s_peer_public_key;
    cfg->peer_endpoint  = s_peer_endpoint;
    cfg->preshared_key  = s_has_preshared_key ? s_preshared_key : NULL;

    if (err == ESP_OK) {
        nvs_close(h);
    }

#undef LOAD_STR
#undef LOAD_U16
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

esp_err_t wireguard_esp32_start(const wireguard_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "WireGuard already started");
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. Resolve config: caller-provided or load from NVS + Kconfig */
    wireguard_config_t local_cfg;
    if (config == NULL) {
        load_config_from_nvs(&local_cfg);
        config = &local_cfg;
    }

    /* 2. Validate mandatory fields */
    if (!config->private_key || config->private_key[0] == '\0') {
        ESP_LOGE(TAG, "WireGuard private key not configured");
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->peer_public_key || config->peer_public_key[0] == '\0') {
        ESP_LOGE(TAG, "WireGuard peer public key not configured");
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->peer_endpoint || config->peer_endpoint[0] == '\0') {
        ESP_LOGE(TAG, "WireGuard peer endpoint not configured");
        return ESP_ERR_INVALID_ARG;
    }

    /* 3. DNS resolve peer endpoint with retries */
    ip_addr_t endpoint_ip = IPADDR4_INIT_BYTES(0, 0, 0, 0);
    bool resolved = false;
    for (int retry = 0; retry < 5 && !resolved; retry++) {
        struct addrinfo hint = {0};
        struct addrinfo *res = NULL;
        if (lwip_getaddrinfo(config->peer_endpoint, NULL, &hint, &res) == 0) {
            struct in_addr addr4 = ((struct sockaddr_in *)(res->ai_addr))->sin_addr;
            inet_addr_to_ip4addr(ip_2_ip4(&endpoint_ip), &addr4);
            lwip_freeaddrinfo(res);
            resolved = true;
        } else {
            ESP_LOGW(TAG, "DNS lookup failed for '%s' (attempt %d/5)", config->peer_endpoint, retry + 1);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    if (!resolved) {
        ESP_LOGE(TAG, "Failed to resolve peer endpoint: %s", config->peer_endpoint);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Peer endpoint %s -> " IPSTR, config->peer_endpoint, IP2STR(ip_2_ip4(&endpoint_ip)));

    /* 4. Initialise WireGuard platform (entropy/RNG) */
    wireguard_platform_init();

    /* 5. Build wireguardif init data */
    struct wireguardif_init_data wg_init = {
        .private_key = config->private_key,
        .listen_port = config->listen_port,
        .bind_netif  = NULL,
    };

    /* 6. Convert IP strings to lwIP address types */
    ip4_addr_t ipaddr4, netmask4, gateway4;
    ip4addr_aton(config->local_ip,      &ipaddr4);
    ip4addr_aton(config->local_netmask, &netmask4);
    ip4addr_aton(config->local_gateway, &gateway4);

    /* 7. Add WireGuard netif.
     *    Use tcpip_input (not ip_input) for ESP-IDF 5+/6+ — packets must
     *    enter the TCP/IP stack via the tcpip thread dispatcher.          */
    s_wg_netif = netif_add(&s_wg_netif_struct,
                           &ipaddr4,
                           &netmask4,
                           &gateway4,
                           &wg_init,
                           wireguardif_init,
                           tcpip_input);
    if (s_wg_netif == NULL) {
        ESP_LOGE(TAG, "netif_add failed — check private key format");
        return ESP_FAIL;
    }
    netif_set_up(s_wg_netif);

    /* 8. Configure and add peer */
    struct wireguardif_peer peer;
    wireguardif_peer_init(&peer);
    peer.public_key    = config->peer_public_key;
    peer.preshared_key = config->preshared_key;
    peer.endpoint_ip   = endpoint_ip;
    peer.endport_port  = config->peer_port;
    peer.keep_alive    = config->keepalive;
    /* Allow all traffic through tunnel (0.0.0.0/0) */
    ip_addr_set_zero_ip4(&peer.allowed_ip);
    ip_addr_set_zero_ip4(&peer.allowed_mask);

    wireguardif_add_peer(s_wg_netif, &peer, &s_peer_index);
    if (s_peer_index == WIREGUARDIF_INVALID_INDEX) {
        ESP_LOGE(TAG, "wireguardif_add_peer failed — check peer public key format");
        netif_remove(s_wg_netif);
        s_wg_netif = NULL;
        return ESP_FAIL;
    }

    /* 9. Initiate outbound handshake */
    wireguardif_connect(s_wg_netif, s_peer_index);

    /* 10. Optionally set as default route */
    s_set_as_default = config->set_as_default;
    if (s_set_as_default) {
        s_prev_default_netif = netif_default;
        netif_set_default(s_wg_netif);
        ESP_LOGI(TAG, "WireGuard set as default route");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WireGuard tunnel started: local=%s peer=%s:%d listen=%d",
             config->local_ip, config->peer_endpoint, config->peer_port, config->listen_port);
    return ESP_OK;
}

esp_err_t wireguard_esp32_stop(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_set_as_default) {
        netif_set_default(s_prev_default_netif);
        s_prev_default_netif = NULL;
        s_set_as_default = false;
    }

    wireguardif_disconnect(s_wg_netif, s_peer_index);
    wireguardif_remove_peer(s_wg_netif, s_peer_index);
    s_peer_index = WIREGUARDIF_INVALID_INDEX;
    wireguardif_shutdown(s_wg_netif);
    netif_remove(s_wg_netif);
    s_wg_netif = NULL;

    s_initialized = false;
    ESP_LOGI(TAG, "WireGuard tunnel stopped");
    return ESP_OK;
}

bool wireguard_esp32_is_peer_up(void)
{
    if (!s_initialized || s_peer_index == WIREGUARDIF_INVALID_INDEX) {
        return false;
    }
    ip_addr_t peer_ip;
    uint16_t peer_port;
    return (wireguardif_peer_is_up(s_wg_netif, s_peer_index, &peer_ip, &peer_port) == ERR_OK);
}
