/*
 * Tailscale network map — MapResponse JSON parser and WireGuard peer manager.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tailscale_netmap.h"
#include "tailscale_derp.h"
#include "tailscale_disco.h"
#include "tailscale_keys.h"

#include "wireguard_esp32.h"   /* wireguard_esp32_add_peer / remove / update */

#include "cJSON.h"
#include "esp_log.h"
#include "lwip/ip_addr.h"

#include <string.h>
#include <stdint.h>

static const char *TAG = "ts_netmap";

/* ------------------------------------------------------------------ */
/* Peer table                                                           */
/* ------------------------------------------------------------------ */

#define TS_NETMAP_MAX_PEERS 8

typedef struct {
    bool     active;
    uint8_t  wg_index;         /* WireGuard peer index */
    uint8_t  node_pub[32];     /* WireGuard public key (raw) */
    uint8_t  disco_pub[32];    /* DISCO public key (raw) */
    char     ts_ip[20];        /* 100.x.y.z */
} netmap_peer_t;

static netmap_peer_t s_peers[TS_NETMAP_MAX_PEERS];
static char          s_self_ip[20];

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Parse "100.x.y.z/32" → "100.x.y.z" */
static void strip_prefix(const char *cidr, char *ip_out, size_t ip_out_len)
{
    strlcpy(ip_out, cidr, ip_out_len);
    char *slash = strchr(ip_out, '/');
    if (slash) *slash = '\0';
}

/* Parse "1.2.3.4:12345" → ip_addr_t + port */
static bool parse_endpoint(const char *ep_str,
                            ip_addr_t *ip_out, uint16_t *port_out)
{
    /* Find last colon (handles IPv4 only) */
    const char *colon = strrchr(ep_str, ':');
    if (!colon) return false;

    char ip_buf[40];
    size_t ip_len = (size_t)(colon - ep_str);
    if (ip_len == 0 || ip_len >= sizeof(ip_buf)) return false;
    memcpy(ip_buf, ep_str, ip_len);
    ip_buf[ip_len] = '\0';

    ip4_addr_t ip4;
    if (!ip4addr_aton(ip_buf, &ip4)) return false;
    ip_addr_copy_from_ip4(*ip_out, ip4);

    *port_out = (uint16_t)atoi(colon + 1);
    return (*port_out != 0);
}

static int find_peer_slot(const uint8_t pub[32])
{
    for (int i = 0; i < TS_NETMAP_MAX_PEERS; i++) {
        if (s_peers[i].active &&
            memcmp(s_peers[i].node_pub, pub, 32) == 0) {
            return i;
        }
    }
    return -1;
}

static int alloc_peer_slot(void)
{
    for (int i = 0; i < TS_NETMAP_MAX_PEERS; i++) {
        if (!s_peers[i].active) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* DERP home node state (forward-declared before parse_derpmap)        */
/* ------------------------------------------------------------------ */

static ts_derp_node_t s_derp_home;
static bool           s_derp_home_valid = false;

/* ------------------------------------------------------------------ */
/* DERPMap parsing — extract home server for Region 1 (lowest latency) */
/* ------------------------------------------------------------------ */

static void parse_derpmap(const cJSON *derp_map_obj)
{
    if (!cJSON_IsObject(derp_map_obj)) return;

    cJSON *regions = cJSON_GetObjectItemCaseSensitive(derp_map_obj, "Regions");
    if (!cJSON_IsObject(regions)) return;

    /* Pick region with the numerically smallest ID as "home" */
    int best_id = INT32_MAX;
    ts_derp_node_t best_node = {0};
    bool found = false;

    cJSON *region;
    cJSON_ArrayForEach(region, regions) {
        if (!cJSON_IsObject(region)) continue;
        cJSON *region_id_j = cJSON_GetObjectItemCaseSensitive(region, "RegionID");
        int region_id = cJSON_IsNumber(region_id_j) ? (int)region_id_j->valuedouble : INT32_MAX;
        if (region_id >= best_id) continue;

        cJSON *nodes = cJSON_GetObjectItemCaseSensitive(region, "Nodes");
        if (!cJSON_IsArray(nodes) || cJSON_GetArraySize(nodes) == 0) continue;
        cJSON *node = cJSON_GetArrayItem(nodes, 0);
        if (!cJSON_IsObject(node)) continue;

        cJSON *host_j = cJSON_GetObjectItemCaseSensitive(node, "HostName");
        cJSON *port_j = cJSON_GetObjectItemCaseSensitive(node, "DERPPort");
        cJSON *stun_j = cJSON_GetObjectItemCaseSensitive(node, "STUNPort");
        if (!cJSON_IsString(host_j)) continue;

        best_id = region_id;
        strlcpy(best_node.hostname, host_j->valuestring,
                sizeof(best_node.hostname));
        best_node.derp_port = cJSON_IsNumber(port_j) ? (uint16_t)port_j->valuedouble : 443;
        best_node.stun_port = cJSON_IsNumber(stun_j) ? (uint16_t)stun_j->valuedouble : 3478;
        found = true;
    }

    if (found) {
        ESP_LOGI(TAG, "DERP home server: %s:%d", best_node.hostname, best_node.derp_port);
        s_derp_home       = best_node;
        s_derp_home_valid = true;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

bool ts_netmap_get_derp_home(ts_derp_node_t *node_out)
{
    if (!s_derp_home_valid) return false;
    *node_out = s_derp_home;
    return true;
}

esp_err_t ts_netmap_apply(const char *json_str, size_t json_len)
{
    cJSON *root = cJSON_ParseWithLength(json_str, json_len);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse MapResponse JSON");
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- Self node -------------------------------------------------- */
    cJSON *self_node = cJSON_GetObjectItemCaseSensitive(root, "Node");
    if (cJSON_IsObject(self_node)) {
        cJSON *addrs = cJSON_GetObjectItemCaseSensitive(self_node, "Addresses");
        if (cJSON_IsArray(addrs) && cJSON_GetArraySize(addrs) > 0) {
            cJSON *addr0 = cJSON_GetArrayItem(addrs, 0);
            if (cJSON_IsString(addr0)) {
                strip_prefix(addr0->valuestring, s_self_ip, sizeof(s_self_ip));
                ESP_LOGI(TAG, "Self Tailscale IP: %s", s_self_ip);

                /* Update WireGuard interface address */
                wireguard_esp32_set_address(s_self_ip, "255.255.255.255");
            }
        }
    }

    /* ---- DERPMap ---------------------------------------------------- */
    cJSON *derpmap = cJSON_GetObjectItemCaseSensitive(root, "DERPMap");
    if (derpmap) {
        parse_derpmap(derpmap);
    }

    /* ---- Peers ------------------------------------------------------ */
    cJSON *peers = cJSON_GetObjectItemCaseSensitive(root, "Peers");
    if (!cJSON_IsArray(peers)) {
        ESP_LOGI(TAG, "No Peers array in MapResponse");
        cJSON_Delete(root);
        return ESP_OK; /* Empty map is valid */
    }
    ESP_LOGI(TAG, "Peers in MapResponse: %d", cJSON_GetArraySize(peers));

    /* Mark all active peers for potential removal */
    bool keep[TS_NETMAP_MAX_PEERS] = {false};

    cJSON *peer_j;
    cJSON_ArrayForEach(peer_j, peers) {
        if (!cJSON_IsObject(peer_j)) continue;

        /* WireGuard public key: "Key": "nodekey:hexhex..." */
        cJSON *key_j = cJSON_GetObjectItemCaseSensitive(peer_j, "Key");
        if (!cJSON_IsString(key_j)) continue;

        uint8_t node_pub[32];
        if (!ts_key_from_hex(key_j->valuestring, node_pub)) {
            ESP_LOGW(TAG, "Failed to decode peer key: %s", key_j->valuestring);
            continue;
        }

        /* DISCO key */
        uint8_t disco_pub[32] = {0};
        cJSON *disco_j = cJSON_GetObjectItemCaseSensitive(peer_j, "DiscoKey");
        if (cJSON_IsString(disco_j)) {
            ts_key_from_hex(disco_j->valuestring, disco_pub);
        }

        /* Tailscale IP (first Addresses entry) */
        char ts_ip[20] = {0};
        cJSON *addrs_j = cJSON_GetObjectItemCaseSensitive(peer_j, "Addresses");
        if (cJSON_IsArray(addrs_j) && cJSON_GetArraySize(addrs_j) > 0) {
            cJSON *a0 = cJSON_GetArrayItem(addrs_j, 0);
            if (cJSON_IsString(a0)) {
                strip_prefix(a0->valuestring, ts_ip, sizeof(ts_ip));
            }
        }

        /* Endpoint: prefer direct UDP endpoint; fall back to DERP pseudo-IP */
        ip_addr_t ep_ip = IPADDR4_INIT(0);
        uint16_t  ep_port = 0;
        bool have_direct_ep = false;
        cJSON *eps_j = cJSON_GetObjectItemCaseSensitive(peer_j, "Endpoints");
        if (cJSON_IsArray(eps_j) && cJSON_GetArraySize(eps_j) > 0) {
            cJSON *ep0 = cJSON_GetArrayItem(eps_j, 0);
            if (cJSON_IsString(ep0)) {
                if (parse_endpoint(ep0->valuestring, &ep_ip, &ep_port))
                    have_direct_ep = true;
            }
        }
        /* If no direct endpoint, use DERP pseudo-IP (127.3.3.40:region) */
        if (!have_direct_ep) {
            cJSON *derp_j = cJSON_GetObjectItemCaseSensitive(peer_j, "DERP");
            if (cJSON_IsString(derp_j)) {
                /* Format: "127.3.3.40:region" */
                const char *colon = strrchr(derp_j->valuestring, ':');
                if (colon) {
                    ep_port = (uint16_t)atoi(colon + 1);
                    /* DERP pseudo-IP: 127.3.3.40 */
                    ip4_addr_t derp4;
                    ip4addr_aton("127.3.3.40", &derp4);
                    ip_addr_copy_from_ip4(ep_ip, derp4);
                }
            }
        }

        /* Encode public key as base64 for wireguard_esp32_add_peer */
        char pub_b64[64];
        size_t b64_len = sizeof(pub_b64);
        extern bool wireguard_base64_encode(const uint8_t *, size_t, char *, size_t *);
        wireguard_base64_encode(node_pub, 32, pub_b64, &b64_len);

        /* Peer already known? */
        int slot = find_peer_slot(node_pub);
        if (slot >= 0) {
            keep[slot] = true;
            /* Update endpoint if it changed */
            if (!ip_addr_isany(&ep_ip) && ep_port != 0) {
                wireguard_esp32_update_endpoint(s_peers[slot].wg_index,
                                               &ep_ip, ep_port);
            }
        } else {
            /* New peer */
            slot = alloc_peer_slot();
            if (slot < 0) {
                ESP_LOGW(TAG, "Peer table full, dropping peer %s", ts_ip);
                continue;
            }

            /* Allowed IP: peer's 100.x.y.z/32 */
            ip4_addr_t allowed4, mask4;
            if (ts_ip[0] && ip4addr_aton(ts_ip, &allowed4)) {
                ip4addr_aton("255.255.255.255", &mask4);
            } else {
                ip4_addr_set_zero(&allowed4);
                ip4_addr_set_zero(&mask4);
            }
            ip_addr_t allowed_ip, allowed_mask;
            ip_addr_copy_from_ip4(allowed_ip, allowed4);
            ip_addr_copy_from_ip4(allowed_mask, mask4);

            uint8_t wg_idx;
            esp_err_t err = wireguard_esp32_add_peer(
                pub_b64,
                &allowed_ip,
                &allowed_mask,
                ip_addr_isany(&ep_ip) ? NULL : &ep_ip,
                ep_port,
                25,   /* keepalive: 25s */
                &wg_idx);

            if (err == ESP_OK) {
                memcpy(s_peers[slot].node_pub,  node_pub,  32);
                memcpy(s_peers[slot].disco_pub, disco_pub, 32);
                strlcpy(s_peers[slot].ts_ip, ts_ip, sizeof(s_peers[slot].ts_ip));
                s_peers[slot].wg_index = wg_idx;
                s_peers[slot].active   = true;
                keep[slot] = true;
                ESP_LOGI(TAG, "Added peer %s (wg_idx=%d, direct=%d)",
                         ts_ip, wg_idx, have_direct_ep);

                /* Probe direct path if peer has DISCO key + direct endpoint */
                if (have_direct_ep) {
                    bool disco_set = false;
                    for (int b = 0; b < 32; b++) if (disco_pub[b]) { disco_set = true; break; }
                    if (disco_set) {
                        ts_disco_ping(wg_idx, disco_pub, &ep_ip, ep_port);
                    }
                }
            }
        }
    }

    /* Remove peers no longer in the map */
    for (int i = 0; i < TS_NETMAP_MAX_PEERS; i++) {
        if (s_peers[i].active && !keep[i]) {
            ESP_LOGI(TAG, "Removing peer %s (wg_idx=%d)",
                     s_peers[i].ts_ip, s_peers[i].wg_index);
            wireguard_esp32_remove_peer(s_peers[i].wg_index);
            s_peers[i].active = false;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void ts_netmap_get_self_ip(char *ip_str, size_t ip_str_len)
{
    strlcpy(ip_str, s_self_ip, ip_str_len);
}
