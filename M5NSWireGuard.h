/*  M5NSWireGuard.h - WireGuard client manager for M5_NightscoutMon
 *
 *  Provides embedded WireGuard VPN client integration, full-tunnel routing,
 *  custom DNS steering (e.g. internal homelab resolvers), link-state monitoring,
 *  and automatic fallback to direct Wi-Fi upon handshake timeouts.
 */

#pragma once

#include <Arduino.h>
#include <IPAddress.h>

enum WireGuardState {
    WG_STATE_DISABLED = 0,
    WG_STATE_CONNECTING,
    WG_STATE_CONNECTED,
    WG_STATE_FALLBACK
};

bool wireguardInit();
bool wireguardConnect();
void wireguardStop();
void wireguardLoop();

WireGuardState wireguardGetState();
const char*    wireguardGetStateStr();
IPAddress      wireguardGetIP();
