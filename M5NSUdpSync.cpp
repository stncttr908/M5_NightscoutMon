/*  M5NSUdpSync.cpp  –  UDP LAN snooze-sync for M5 Nightscout monitor
 *
 *  Broadcasts a small snooze-state packet over the local subnet whenever
 *  the user changes the snooze on one device, so every other M5NS device
 *  watching the same Nightscout URL silently follows.
 *
 *  Protocol (all in plain text, NUL-terminated, port 50555 by default):
 *    Sender  →  broadcast : "M5_Nightscout SNOOZE: USR=<crc16>, SnoozeUntil=<epoch_ul>"
 *
 *  The CRC-16 of cfg.url acts as a namespace: devices on different NS
 *  instances share the same LAN without stepping on each other.
 *
 *  Bug fixes vs. the original inline implementation:
 *    1. Broadcast address: (localIP & mask) | ~mask  (was ~mask | gatewayIP)
 *    2. udp.begin(port) binds to 0.0.0.0 so broadcasts are received
 *       (was udp.begin(localIP, port) — unicast-only on some SDK builds)
 *    3. Retry drain moved out of draw_screen() into loop() with a 500 ms
 *       inter-retry timer, so it fires even when the screen is off
 *    4. udpSyncScheduleSnooze() is the single call site; the snooze-expiry
 *       path in handleAlarmsInfoLine() also calls it so peers are notified
 *    5. Dead "Hello, M5NS here" ping-pong handshake removed
 *    6. Guard: entire feature no-ops when cfg.udp_sync_enabled == 0
 */

#include "M5NSUdpSync.h"
#include "M5NSDevice.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include "M5NSconfig.h"

// ---------- external linkage (defined in the main .ino) ----------
extern tConfig cfg;
extern time_t snoozeUntil;
extern int snoozeMult;
extern uint16_t calcCRC(char *str);
extern void handleAlarmsInfoLine(struct NSinfo *ns);
extern struct NSinfo ns;
// ----------------------------------------------------------------

#define UDP_SYNC_PACKET_MAX   256
#define UDP_SEND_RETRIES      3
#define UDP_RETRY_INTERVAL_MS 500   // min ms between successive retry sends

static WiFiUDP udpSync;
static bool    udpOpen         = false;
static int     pendingRetries  = 0;
static unsigned long lastRetrySendMs = 0;

// ----------------------------------------------------------------
// Helper: compute correct subnet broadcast address
// (localIP & subnetMask) | ~subnetMask
// ----------------------------------------------------------------
static IPAddress broadcastAddress() {
  IPAddress local  = WiFi.localIP();
  IPAddress mask   = WiFi.subnetMask();
  IPAddress bcast;
  for (int i = 0; i < 4; i++) {
    bcast[i] = (local[i] & mask[i]) | (~mask[i] & 0xFF);
  }
  return bcast;
}

// ----------------------------------------------------------------
// Helper: send one broadcast now
// ----------------------------------------------------------------
static void sendSnoozePacket() {
  if (!udpOpen) return;
  IPAddress bcast = broadcastAddress();
  int port = (cfg.udp_sync_port > 0) ? cfg.udp_sync_port : 50555;
  uint16_t urlCRC = calcCRC(cfg.url);
  unsigned long snzUntl = (unsigned long)snoozeUntil;

  udpSync.beginPacket(bcast, port);
  char buf[UDP_SYNC_PACKET_MAX];
  snprintf(buf, sizeof(buf),
           "M5_Nightscout SNOOZE: USR=%d, SnoozeUntil=%lu",
           (int)urlCRC, snzUntl);
  udpSync.write((const uint8_t*)buf, strlen(buf) + 1); // include NUL
  udpSync.endPacket();

  Serial.printf("[UdpSync] broadcast → %s:%d  USR=%d SnoozeUntil=%lu\r\n",
                bcast.toString().c_str(), port, (int)urlCRC, snzUntl);
}

// ----------------------------------------------------------------
// Public API
// ----------------------------------------------------------------

void udpSyncInit() {
  // Always stop first — safe even if not open
  if (udpOpen) {
    udpSync.stop();
    udpOpen = false;
  }
  if (!cfg.udp_sync_enabled) {
    Serial.println("[UdpSync] disabled by config");
    return;
  }
  int port = (cfg.udp_sync_port > 0) ? cfg.udp_sync_port : 50555;
  // Bind to 0.0.0.0 (wildcard) so we receive both unicast and broadcast
  if (udpSync.begin(port)) {
    udpOpen = true;
    Serial.printf("[UdpSync] listening on port %d\r\n", port);
  } else {
    Serial.println("[UdpSync] udp.begin() failed");
  }
}

void udpSyncScheduleSnooze() {
  if (!cfg.udp_sync_enabled) return;
  pendingRetries = UDP_SEND_RETRIES;
  lastRetrySendMs = 0; // fire immediately on next loop()
}

void udpSyncLoop() {
  if (!cfg.udp_sync_enabled || !udpOpen) return;

  // ---- 1. Drain incoming packets ----
  int port = (cfg.udp_sync_port > 0) ? cfg.udp_sync_port : 50555;
  int packetSize;
  while ((packetSize = udpSync.parsePacket()) > 0) {
    // Ignore our own broadcasts
    if (udpSync.remoteIP() == WiFi.localIP()) {
      udpSync.flush();
      continue;
    }

    char buf[UDP_SYNC_PACKET_MAX];
    int rd = udpSync.read(buf, sizeof(buf) - 1);
    if (rd > 0) buf[rd] = '\0';

    Serial.printf("[UdpSync] rx %d bytes from %s:%d\r\n",
                  packetSize, udpSync.remoteIP().toString().c_str(),
                  udpSync.remotePort());

    // Parse SNOOZE packet
    const char *prefix = "M5_Nightscout SNOOZE: USR=";
    if (strncmp(buf, prefix, strlen(prefix)) == 0) {
      uint16_t urlCRC = calcCRC(cfg.url);
      int      urlCRC_rcvd = 0;
      unsigned long snzUntl = 0;
      int sr = sscanf(buf, "M5_Nightscout SNOOZE: USR=%d, SnoozeUntil=%lu",
                      &urlCRC_rcvd, &snzUntl);
      if (sr == 2 && (uint16_t)urlCRC_rcvd == urlCRC) {
        Serial.printf("[UdpSync] accepted remote snooze until %lu\r\n", snzUntl);
        snoozeUntil = (time_t)snzUntl;
        // Update snoozeMult so the display icon shows correctly:
        // 0 = off, non-zero = active (exact multiplier not critical here)
        snoozeMult = (snoozeUntil > 0) ? 1 : 0;
        // Refresh alarm state display immediately
        handleAlarmsInfoLine(&ns);
      } else {
        Serial.printf("[UdpSync] ignored packet (crc mismatch or parse error: sr=%d rcvd=%d local=%d)\r\n",
                      sr, urlCRC_rcvd, (int)urlCRC);
      }
    }
  }

  // ---- 2. Send pending retry broadcasts (throttled) ----
  if (pendingRetries > 0) {
    unsigned long now = millis();
    if (now - lastRetrySendMs >= UDP_RETRY_INTERVAL_MS) {
      sendSnoozePacket();
      pendingRetries--;
      lastRetrySendMs = now;
    }
  }
}
