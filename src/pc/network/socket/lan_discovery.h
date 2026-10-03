#ifndef LAN_DISCOVERY_H
#define LAN_DISCOVERY_H

#include <stdbool.h>
#include <stdint.h>

// Default UDP port used ONLY for LAN discovery (not the game port).
#define LAN_DISCOVERY_DEFAULT_PORT 7778

#define LAN_NAME_LEN    32
#define LAN_MODE_LEN    64
#define LAN_VERSION_LEN 32
#define LAN_MAX_RESULTS 32

struct LanServerInfo {
    char     ip[64];                  // dotted IPv4 string of the sender
    uint16_t gamePort;                // port to join with
    uint8_t  players;
    uint8_t  maxPlayers;
    char     hostName[LAN_NAME_LEN];
    char     mode[LAN_MODE_LEN];
    char     version[LAN_VERSION_LEN];
    uint8_t  saveSlot;                // 1-4, 0 if no save loaded
    uint16_t stars;                   // total stars in that save
};

typedef void (*LanFoundCallback)(const struct LanServerInfo* info);
typedef void (*LanFinishCallback)(void);

// ---- server side -----------------------------------------------------------
// Start answering LAN discovery probes. gamePort is the port clients should join.
void lan_discovery_server_start(unsigned int gamePort);
void lan_discovery_server_stop(void);

// ---- client side -----------------------------------------------------------
// Broadcasts a probe to 255.255.255.255:<lan port> (local servers are found too). Replies arrive via the callback from lan_discovery_update().
bool lan_discovery_scan_begin(LanFoundCallback onFound, LanFinishCallback onFinish);
void lan_discovery_scan_end(void);
bool lan_discovery_scan_active(void);
// onFinish fires once, after the listen window (about 2 seconds) elapses.

// ---- shared ----------------------------------------------------------------
// Call once per frame (from network_update()).
void lan_discovery_update(void);

#endif
