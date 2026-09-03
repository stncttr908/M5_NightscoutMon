#ifndef _M5NS_UDPSYNC_H
#define _M5NS_UDPSYNC_H

#include <Arduino.h>
#include "M5NSconfig.h"

// Call once after Wi-Fi connects (or whenever udp_sync_enabled/port changes).
// Safe to call multiple times: stops any existing socket before reopening.
void udpSyncInit();

// Call from loop() every iteration.
// Drains incoming packets and fires any pending retry broadcasts.
void udpSyncLoop();

// Schedule a SNOOZE broadcast (cfg.udp_sync_snooze must be 1).
// UDP_SEND_RETRIES packets will be sent over the next few loop() calls.
void udpSyncSendSnooze();

// Schedule a REFRESH-INTERVAL broadcast (cfg.udp_sync_refresh must be 1).
void udpSyncSendRefresh();

#endif
