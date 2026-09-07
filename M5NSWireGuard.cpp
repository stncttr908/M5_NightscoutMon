/*  M5NSWireGuard.cpp - WireGuard client manager for M5_NightscoutMon
 *
 *  Provides embedded WireGuard VPN client integration, full-tunnel routing,
 *  custom DNS steering (e.g. internal homelab resolvers), link-state monitoring,
 *  and automatic fallback to direct Wi-Fi upon handshake timeouts.
 */

#include "M5NSWireGuard.h"
#include "M5NSconfig.h"

#include <WireGuard-ESP32.h>
#include <WiFi.h>

#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"

extern tConfig cfg;

static WireGuard wg;
static WireGuardState wgState = WG_STATE_DISABLED;
static unsigned long lastFallbackRetryMs = 0;

static ip_addr_t saved_dhcp_dns[2];
static bool dhcp_dns_saved = false;

// -----------------------------------------------------------------------------
// lwIP Netif Helpers
// -----------------------------------------------------------------------------
static struct netif* get_wg_netif() {
  struct netif *n;
  for (n = netif_list; n != NULL; n = n->next) {
    if (n->name[0] == 'w' && n->name[1] == 'g') {
      return n;
    }
  }
  return NULL;
}

static struct netif* get_sta_netif() {
  struct netif *n;
  for (n = netif_list; n != NULL; n = n->next) {
    if (n->name[0] == 's' && n->name[1] == 't') {
      return n;
    }
  }
  return NULL;
}

static bool is_wg_link_up() {
  struct netif *wgn = get_wg_netif();
  return (wgn != NULL && netif_is_link_up(wgn));
}

// -----------------------------------------------------------------------------
// DNS Management
// -----------------------------------------------------------------------------
static void save_dhcp_dns() {
  if (!dhcp_dns_saved) {
    const ip_addr_t *d0 = dns_getserver(0);
    if (d0) saved_dhcp_dns[0] = *d0;
    const ip_addr_t *d1 = dns_getserver(1);
    if (d1) saved_dhcp_dns[1] = *d1;
    dhcp_dns_saved = true;
  }
}

static void restore_dhcp_dns() {
  if (dhcp_dns_saved) {
    dns_setserver(0, &saved_dhcp_dns[0]);
    dns_setserver(1, &saved_dhcp_dns[1]);
    Serial.println("[WG] Restored DHCP DNS servers.");
  }
}

static void apply_wireguard_dns() {
  save_dhcp_dns();

  if (cfg.wireguard_dns[0] != '\0') {
    ip_addr_t primary_dns;
    if (ipaddr_aton(cfg.wireguard_dns, &primary_dns)) {
      dns_setserver(0, &primary_dns);
      Serial.printf("[WG] Set primary DNS to %s\r\n", cfg.wireguard_dns);
    }
  }
  if (cfg.wireguard_dns2[0] != '\0') {
    ip_addr_t secondary_dns;
    if (ipaddr_aton(cfg.wireguard_dns2, &secondary_dns)) {
      dns_setserver(1, &secondary_dns);
      Serial.printf("[WG] Set secondary DNS to %s\r\n", cfg.wireguard_dns2);
    }
  }
}

// -----------------------------------------------------------------------------
// Lifecycle & State Machine
// -----------------------------------------------------------------------------
bool wireguardInit() {
  wgState = WG_STATE_DISABLED;
  lastFallbackRetryMs = 0;
  return true;
}

bool wireguardConnect() {
  if (!cfg.wireguard_enabled) {
    wgState = WG_STATE_DISABLED;
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WG] Cannot connect WireGuard: Wi-Fi not connected.");
    return false;
  }

  if (cfg.wireguard_local_ip[0] == '\0' || cfg.wireguard_endpoint[0] == '\0' ||
      cfg.wireguard_peer_pubkey[0] == '\0' || cfg.wireguard_privkey[0] == '\0') {
    Serial.println("[WG] Cannot connect WireGuard: Missing required configuration (IP, Endpoint, or Keys).");
    wgState = WG_STATE_DISABLED;
    return false;
  }

  IPAddress local_ip;
  if (!local_ip.fromString(cfg.wireguard_local_ip)) {
    Serial.printf("[WG] Invalid WireGuard local IP: %s\r\n", cfg.wireguard_local_ip);
    wgState = WG_STATE_DISABLED;
    return false;
  }

  save_dhcp_dns();

  Serial.printf("[WG] Starting WireGuard client...\r\n");
  Serial.printf("[WG] Local IP: %s\r\n", cfg.wireguard_local_ip);
  Serial.printf("[WG] Peer Endpoint: %s:%d\r\n", cfg.wireguard_endpoint, cfg.wireguard_port);

  // If already initialized, shut down previous interface cleanly first
  if (wg.is_initialized()) {
    wg.end();
  }

  bool begin_ok = wg.begin(
    local_ip,
    cfg.wireguard_privkey,
    cfg.wireguard_endpoint,
    cfg.wireguard_peer_pubkey,
    (uint16_t)cfg.wireguard_port
  );

  if (!begin_ok) {
    Serial.println("[WG] WireGuard begin() failed.");
    restore_dhcp_dns();
    struct netif *sta = get_sta_netif();
    if (sta) netif_set_default(sta);
    wgState = WG_STATE_FALLBACK;
    lastFallbackRetryMs = millis();
    return false;
  }

  wgState = WG_STATE_CONNECTING;

  // Handshake wait & fallback monitoring
  if (cfg.wireguard_fallback_timeout > 0) {
    Serial.printf("[WG] Waiting up to %d seconds for handshake...\r\n", cfg.wireguard_fallback_timeout);
    unsigned long startWait = millis();
    unsigned long timeoutMs = (unsigned long)cfg.wireguard_fallback_timeout * 1000;

    while ((millis() - startWait) < timeoutMs) {
      if (is_wg_link_up()) {
        wgState = WG_STATE_CONNECTED;
        apply_wireguard_dns();
        struct netif *wgn = get_wg_netif();
        if (wgn) netif_set_default(wgn);
        Serial.println("[WG] WireGuard handshake successful. Tunnel is UP.");
        return true;
      }
      delay(150);
    }

    // Handshake timed out -> Fallback to direct Wi-Fi
    Serial.printf("[WG] Handshake timed out after %d seconds. Entering fallback mode (direct Wi-Fi).\r\n", cfg.wireguard_fallback_timeout);
    struct netif *sta = get_sta_netif();
    if (sta) netif_set_default(sta);
    restore_dhcp_dns();
    wgState = WG_STATE_FALLBACK;
    lastFallbackRetryMs = millis();
    return false;
  } else {
    // No fallback timeout configured: assume full tunnel immediately
    wgState = WG_STATE_CONNECTED;
    apply_wireguard_dns();
    return true;
  }
}

void wireguardStop() {
  if (wgState != WG_STATE_DISABLED) {
    Serial.println("[WG] Stopping WireGuard client.");
    restore_dhcp_dns();
    struct netif *sta = get_sta_netif();
    if (sta) netif_set_default(sta);
    if (wg.is_initialized()) {
      wg.end();
    }
    wgState = WG_STATE_DISABLED;
  }
}

void wireguardLoop() {
  if (!cfg.wireguard_enabled) {
    if (wgState != WG_STATE_DISABLED) {
      wireguardStop();
    }
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (wgState == WG_STATE_FALLBACK) {
    if (cfg.wireguard_fallback_retry > 0 && (millis() - lastFallbackRetryMs) >= ((unsigned long)cfg.wireguard_fallback_retry * 1000)) {
      Serial.println("[WG] Fallback retry timer elapsed. Attempting tunnel reconnect...");
      wireguardConnect();
    }
  } else if (wgState == WG_STATE_CONNECTED) {
    if (!is_wg_link_up()) {
      Serial.println("[WG] Tunnel link lost. Reverting to fallback Wi-Fi.");
      struct netif *sta = get_sta_netif();
      if (sta) netif_set_default(sta);
      restore_dhcp_dns();
      wgState = WG_STATE_FALLBACK;
      lastFallbackRetryMs = millis();
    }
  } else if (wgState == WG_STATE_CONNECTING) {
    if (is_wg_link_up()) {
      Serial.println("[WG] Handshake confirmed. Tunnel is now UP.");
      wgState = WG_STATE_CONNECTED;
      apply_wireguard_dns();
      struct netif *wgn = get_wg_netif();
      if (wgn) netif_set_default(wgn);
    }
  }
}

WireGuardState wireguardGetState() {
  return wgState;
}

const char* wireguardGetStateStr() {
  switch (wgState) {
    case WG_STATE_DISABLED:   return "Disabled";
    case WG_STATE_CONNECTING: return "Connecting...";
    case WG_STATE_CONNECTED:  return "Connected";
    case WG_STATE_FALLBACK:   return "Fallback (Direct Wi-Fi)";
    default:                  return "Unknown";
  }
}

IPAddress wireguardGetIP() {
  if (wgState == WG_STATE_CONNECTED) {
    struct netif *wgn = get_wg_netif();
    if (wgn) {
      return IPAddress(ip_2_ip4(&wgn->ip_addr)->addr);
    }
  }
  return IPAddress(0, 0, 0, 0);
}
