// LAN discovery for sm64coopdx.
//
// A dedicated UDP socket (separate from the game socket) is used so discovery
// traffic can never be mistaken for a game packet.
//
//   client -> 255.255.255.255:<lanPort>   "SM64CDXL" QUERY
//   server -> client (unicast)            "SM64CDXL" REPLY + server info
//
// Servers do not spam the network; they only answer probes.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "socket.h"
#include "lan_discovery.h"
#include "pc/configfile.h"
#include "pc/cliopts.h"
#include "pc/debuglog.h"
#include "pc/mods/mods.h"
#include "pc/network/version.h"
#include "pc/network/network_player.h"
#include "game/save_file.h"

#define LAN_MAGIC        "SM64CDXL"
#define LAN_MAGIC_LEN    8
#define LAN_PROTO_VER    2
#define LAN_PKT_QUERY    1
#define LAN_PKT_REPLY    2
#define LAN_SCAN_FRAMES  (2 * 30) // listen window, game runs at 30 fps

#pragma pack(push, 1)
struct LanPacket {
    char     magic[LAN_MAGIC_LEN];
    uint8_t  protoVersion;
    uint8_t  type;
    // reply fields (zero in a query)
    uint16_t gamePort;               // network byte order
    uint8_t  players;
    uint8_t  maxPlayers;
    char     hostName[LAN_NAME_LEN];
    char     mode[LAN_MODE_LEN];
    char     version[LAN_VERSION_LEN];
    uint8_t  saveSlot;               // 1-4, 0 = none
    uint16_t stars;                  // network byte order
};
#pragma pack(pop)

static SOCKET sServerSock = INVALID_SOCKET;
static SOCKET sClientSock = INVALID_SOCKET;
static unsigned int sServerGamePort = 0;
static LanFoundCallback sOnFound = NULL;
static LanFinishCallback sOnFinish = NULL;
static int sScanFramesLeft = 0;
#ifdef WINSOCK
static bool sWinsockStarted = false;
#endif

static unsigned int lan_port(void) {
    unsigned int port = gCLIOpts.lanPort ? gCLIOpts.lanPort : configLanPort;
    return (port != 0 && port <= 65535) ? port : LAN_DISCOVERY_DEFAULT_PORT;
}

static void lan_set_nonblocking(SOCKET s) {
#ifdef WINSOCK
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static SOCKET lan_make_socket(bool bindToLanPort) {
#ifdef WINSOCK
    if (!sWinsockStarted) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) { return INVALID_SOCKET; }
        sWinsockStarted = true;
    }
#endif
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        LOG_ERROR("lan: socket failed with error %d", SOCKET_LAST_ERROR);
        return INVALID_SOCKET;
    }
    lan_set_nonblocking(s);

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));

    if (bindToLanPort) {
        // allow several servers on one machine / a client on a server machine
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
#ifdef SO_REUSEPORT
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char*)&yes, sizeof(yes));
#endif
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = bindToLanPort ? htons(lan_port()) : 0; // client uses an ephemeral port
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        LOG_ERROR("lan: bind failed with error %d", SOCKET_LAST_ERROR);
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static void lan_close(SOCKET* s) {
    if (*s != INVALID_SOCKET) { closesocket(*s); *s = INVALID_SOCKET; }
}

static void lan_copy(char* dst, size_t dstLen, const char* src) {
    snprintf(dst, dstLen, "%s", src ? src : "");
}

// ---- server ----------------------------------------------------------------

void lan_discovery_server_start(unsigned int gamePort) {
    lan_discovery_server_stop();
    if (!configLanDiscovery) { return; }

    sServerSock = lan_make_socket(true);
    if (sServerSock == INVALID_SOCKET) {
        LOG_ERROR("lan: could not start discovery responder");
        return;
    }
    sServerGamePort = gamePort;
    LOG_INFO("lan: answering discovery probes on udp/%u (game port %u)", lan_port(), gamePort);
}

void lan_discovery_server_stop(void) {
    lan_close(&sServerSock);
}

static void lan_server_update(void) {
    if (sServerSock == INVALID_SOCKET) { return; }

    for (int guard = 0; guard < 16; guard++) {
        struct LanPacket in;
        struct sockaddr_in from;
        RX_ADDR_SIZE_TYPE fromLen = sizeof(from);
        int rc = recvfrom(sServerSock, (char*)&in, sizeof(in), 0, (struct sockaddr*)&from, &fromLen);
        if (rc <= 0) { break; }
        if (rc < (int)(LAN_MAGIC_LEN + 2)) { continue; }
        if (memcmp(in.magic, LAN_MAGIC, LAN_MAGIC_LEN) != 0) { continue; }
        if (in.protoVersion != LAN_PROTO_VER || in.type != LAN_PKT_QUERY) { continue; }

        struct LanPacket out;
        memset(&out, 0, sizeof(out));
        memcpy(out.magic, LAN_MAGIC, LAN_MAGIC_LEN);
        out.protoVersion = LAN_PROTO_VER;
        out.type         = LAN_PKT_REPLY;
        out.gamePort     = htons((uint16_t)sServerGamePort);
        out.players      = network_player_connected_count();
        out.maxPlayers   = (uint8_t)configAmountOfPlayers;
        lan_copy(out.hostName, sizeof(out.hostName), configPlayerName);
        lan_copy(out.version,  sizeof(out.version),  get_version());
        char mode[LAN_MODE_LEN] = "";
        mods_get_main_mod_name(mode, sizeof(mode));
        lan_copy(out.mode, sizeof(out.mode), mode);

        extern s16 gCurrSaveFileNum;
        if (gCurrSaveFileNum >= 1 && gCurrSaveFileNum <= 4) {
            out.saveSlot = (uint8_t)gCurrSaveFileNum;
            out.stars = htons((uint16_t)save_file_get_total_star_count(gCurrSaveFileNum - 1, COURSE_MIN - 1, COURSE_MAX - 1));
        }

        sendto(sServerSock, (const char*)&out, sizeof(out), 0, (struct sockaddr*)&from, sizeof(from));
    }
}

// ---- client ----------------------------------------------------------------

static void lan_send_probe(const struct sockaddr_in* dest) {
    struct LanPacket q;
    memset(&q, 0, sizeof(q));
    memcpy(q.magic, LAN_MAGIC, LAN_MAGIC_LEN);
    q.protoVersion = LAN_PROTO_VER;
    q.type = LAN_PKT_QUERY;
    sendto(sClientSock, (const char*)&q, sizeof(q), 0, (const struct sockaddr*)dest, sizeof(*dest));
}

bool lan_discovery_scan_begin(LanFoundCallback onFound, LanFinishCallback onFinish) {
    lan_discovery_scan_end();

    sClientSock = lan_make_socket(false);
    if (sClientSock == INVALID_SOCKET) { return false; }
    sOnFound = onFound;
    sOnFinish = onFinish;
    sScanFramesLeft = LAN_SCAN_FRAMES;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(lan_port());

    // 1) limited broadcast
    dest.sin_addr.s_addr = htonl(INADDR_BROADCAST); // 255.255.255.255
    lan_send_probe(&dest);

    // (a broadcast is also delivered to sockets on this machine, so a server
    //  running locally is found without a separate loopback probe)

    LOG_INFO("lan: probe sent to 255.255.255.255:%u", lan_port());
    return true;
}

void lan_discovery_scan_end(void) {
    lan_close(&sClientSock);
    sOnFound = NULL;
    sOnFinish = NULL;
    sScanFramesLeft = 0;
}

bool lan_discovery_scan_active(void) {
    return sClientSock != INVALID_SOCKET;
}

static void lan_client_update(void) {
    if (sClientSock == INVALID_SOCKET) { return; }
    bool finishing = (sScanFramesLeft == 1);
    if (sScanFramesLeft > 0) { sScanFramesLeft--; }

    for (int guard = 0; guard < 32; guard++) {
        struct LanPacket in;
        struct sockaddr_in from;
        RX_ADDR_SIZE_TYPE fromLen = sizeof(from);
        int rc = recvfrom(sClientSock, (char*)&in, sizeof(in), 0, (struct sockaddr*)&from, &fromLen);
        if (rc <= 0) { break; }
        if (rc != (int)sizeof(in)) { continue; }
        if (memcmp(in.magic, LAN_MAGIC, LAN_MAGIC_LEN) != 0) { continue; }
        if (in.protoVersion != LAN_PROTO_VER || in.type != LAN_PKT_REPLY) { continue; }

        struct LanServerInfo info;
        memset(&info, 0, sizeof(info));
        inet_ntop(AF_INET, &from.sin_addr, info.ip, sizeof(info.ip));
        info.gamePort   = ntohs(in.gamePort);
        info.players    = in.players;
        info.maxPlayers = in.maxPlayers;
        in.hostName[LAN_NAME_LEN - 1] = '\0';
        in.mode[LAN_MODE_LEN - 1] = '\0';
        in.version[LAN_VERSION_LEN - 1] = '\0';
        lan_copy(info.hostName, sizeof(info.hostName), in.hostName);
        lan_copy(info.mode,     sizeof(info.mode),     in.mode);
        lan_copy(info.version,  sizeof(info.version),  in.version);
        info.saveSlot = in.saveSlot;
        info.stars    = ntohs(in.stars);

        if (sOnFound) { sOnFound(&info); }
    }

    if (finishing) {
        LanFinishCallback done = sOnFinish;
        lan_discovery_scan_end(); // stop listening before notifying the UI
        if (done) { done(); }
    }
}

void lan_discovery_update(void) {
    lan_server_update();
    lan_client_update();
}
