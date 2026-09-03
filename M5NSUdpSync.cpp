/*  M5NSUdpSync.cpp  –  UDP LAN device-sync for M5 Nightscout monitor
 *
 *  Broadcasts small state packets over the local subnet so every M5NS
 *  device watching the same Nightscout URL stays in lock-step.
 *
 *  Protocol (plain text, NUL-terminated, port 50555 by default):
 *    "M5NS SYNC USR=<crc16> TYPE=SNOOZE SnoozeUntil=<epoch_ul>"
 *    "M5NS SYNC USR=<crc16> TYPE=REFRESH Interval=<seconds>"
 *
 *  TYPE= dispatch makes the format forward-compatible: devices running
 *  older firmware silently ignore TYPE values they don't recognise.
 *
 *  The CRC-16 of cfg.url acts as a namespace: devices on different NS
 *  instances share the same LAN without stepping on each other.
 *
 *  Per-type sync can be disabled independently via cfg flags:
 *    cfg.udp_sync_snooze  – gates SNOOZE broadcasts
 *    cfg.udp_sync_refresh – gates REFRESH broadcasts
 *
 *  Bug fixes vs. original inline implementation:
 *    1. Broadcast address: (localIP & mask) | ~mask  (was ~mask | gatewayIP)
 *    2. udp.begin(port) binds to 0.0.0.0 so broadcasts are received
 *       (was udp.begin(localIP, port) — unicast-only on some SDK builds)
 *    3. Retry drain moved out of draw_screen() into loop() with a 500 ms
 *       inter-retry timer, so it fires even when the screen is off
 *    4. udpSyncSendSnooze() is the single call site for snooze broadcasts
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
extern uint32_t refreshIntervalSec;
extern uint16_t calcCRC(char *str);
extern void handleAlarmsInfoLine(struct NSinfo *ns);
extern struct NSinfo ns;
// ----------------------------------------------------------------

#define UDP_SYNC_PACKET_MAX   256
#define UDP_SEND_RETRIES      3
#define UDP_RETRY_INTERVAL_MS 500   // min ms between successive retry sends

static WiFiUDP udpSync;
static bool    udpOpen               = false;
static int     pendingSnoozeRetries  = 0;
static int     pendingRefreshRetries = 0;
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
// Helper: send one raw packet to the subnet broadcast address
// ----------------------------------------------------------------
static void sendPacket(const char *payload) {
  if (!udpOpen) return;
  int port = (cfg.udp_sync_port > 0) ? cfg.udp_sync_port : 50555;
  IPAddress bcast = broadcastAddress();
  udpSync.beginPacket(bcast, port);
  udpSync.write((const uint8_t*)payload, strlen(payload) + 1); // include NUL
  udpSync.endPacket();
  Serial.printf("[UdpSync] broadcast → %s:%d  %s\r\n",
                bcast.toString().c_str(), port, payload);
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

void udpSyncSendSnooze() {
  if (!cfg.udp_sync_enabled || !cfg.udp_sync_snooze) return;
  pendingSnoozeRetries = UDP_SEND_RETRIES;
  lastRetrySendMs = 0; // fire immediately on next loop()
}

void udpSyncSendRefresh() {
  if (!cfg.udp_sync_enabled || !cfg.udp_sync_refresh) return;
  pendingRefreshRetries = UDP_SEND_RETRIES;
  lastRetrySendMs = 0;
}

void udpSyncLoop() {
  if (!cfg.udp_sync_enabled || !udpOpen) return;

  // ---- 1. Drain incoming packets ----
  uint16_t localCRC = calcCRC(cfg.url);
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

    // All sync packets share the same header prefix
    const char *header = "M5NS SYNC USR=";
    if (strncmp(buf, header, strlen(header)) != 0) continue;

    // Parse USR (URL namespace CRC) and TYPE tag
    int  rcvdCRC = 0;
    char typeTag[32] = "";
    if (sscanf(buf, "M5NS SYNC USR=%d TYPE=%31s", &rcvdCRC, typeTag) != 2) continue;
    if ((uint16_t)rcvdCRC != localCRC) {
      Serial.printf("[UdpSync] ignored (crc mismatch: rcvd=%d local=%d)\r\n",
                    rcvdCRC, (int)localCRC);
      continue;
    }

    // ---- Dispatch on TYPE ----
    if (strcmp(typeTag, "SNOOZE") == 0 && cfg.udp_sync_snooze) {
      unsigned long snzUntl = 0;
      if (sscanf(buf, "M5NS SYNC USR=%*d TYPE=SNOOZE SnoozeUntil=%lu", &snzUntl) == 1) {
        Serial.printf("[UdpSync] SNOOZE accepted: SnoozeUntil=%lu\r\n", snzUntl);
        snoozeUntil = (time_t)snzUntl;
        snoozeMult  = (snoozeUntil > 0) ? 1 : 0;
        handleAlarmsInfoLine(&ns);
      }
    } else if (strcmp(typeTag, "REFRESH") == 0 && cfg.udp_sync_refresh) {
      uint32_t interval = 0;
      if (sscanf(buf, "M5NS SYNC USR=%*d TYPE=REFRESH Interval=%u", &interval) == 1
          && interval >= 10 && interval <= 3600) {
        Serial.printf("[UdpSync] REFRESH accepted: Interval=%u s\r\n", interval);
        refreshIntervalSec = interval;
      }
    } else {
      Serial.printf("[UdpSync] unknown or disabled TYPE=%s — ignored\r\n", typeTag);
    }
  }

  // ---- 2. Send pending retry broadcasts (throttled, interleaved) ----
  if (pendingSnoozeRetries > 0 || pendingRefreshRetries > 0) {
    unsigned long now = millis();
    if (now - lastRetrySendMs >= UDP_RETRY_INTERVAL_MS) {
      uint16_t urlCRC = calcCRC(cfg.url);
      char buf[UDP_SYNC_PACKET_MAX];

      if (pendingSnoozeRetries > 0) {
        snprintf(buf, sizeof(buf),
                 "M5NS SYNC USR=%d TYPE=SNOOZE SnoozeUntil=%lu",
                 (int)urlCRC, (unsigned long)snoozeUntil);
        sendPacket(buf);
        pendingSnoozeRetries--;
      }
      if (pendingRefreshRetries > 0) {
        snprintf(buf, sizeof(buf),
                 "M5NS SYNC USR=%d TYPE=REFRESH Interval=%u",
                 (int)urlCRC, (unsigned int)refreshIntervalSec);
        sendPacket(buf);
        pendingRefreshRetries--;
      }
      lastRetrySendMs = now;
    }
  }
}

