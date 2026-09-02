#ifndef _M5NS_UDPSYNC_H
#define _M5NS_UDPSYNC_H

#include <Arduino.h>
#include "M5NSconfig.h"

// Call once after Wi-Fi connects (or whenever udp_sync_enabled changes).
// Safe to call multiple times: stops any existing socket before reopening.
void udpSyncInit();

// Call from loop() every iteration.
// Drains incoming packets and fires any pending retry broadcasts.
void udpSyncLoop();

// Mark that the local snooze state has changed and the broadcast should go out.
// UDP_SEND_RETRIES packets will be sent over the next few loop() calls.
void udpSyncScheduleSnooze();

#endif
