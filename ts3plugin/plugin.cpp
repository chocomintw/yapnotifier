// TeamSpeak 3 client plugin: forwards "who is talking" to the YapNotifier .asi
// as UDP datagrams on localhost. See shared/yap_protocol.h for the format.
#include <WinSock2.h>
#include <WS2tcpip.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "ts3_functions.h"
#include "yap_protocol.h"

#define PLUGIN_API_VERSION 26
#define EXPORT extern "C" __declspec(dllexport)

namespace {
TS3Functions ts3{};

// clients seen talking, keyed by (server connection, client id)
using Key = std::pair<uint64, anyID>;
std::mutex g_mutex;
std::map<Key, std::string> g_talking;

SOCKET g_sock = INVALID_SOCKET;
sockaddr_in g_dest{};
std::thread g_heartbeat;
std::atomic<bool> g_stop{false};

void send_state_locked() {
    if (g_sock == INVALID_SOCKET) return;
    std::string d = yap::proto::kMagic;
    for (const auto& [key, name] : g_talking) {
        d += std::to_string(key.second);
        d += '\t';
        d += name;
        d += '\n';
    }
    sendto(g_sock, d.data(), static_cast<int>(d.size()), 0,
           reinterpret_cast<const sockaddr*>(&g_dest), sizeof g_dest);
}

void erase_client(uint64 schid, anyID clid) {
    std::lock_guard lk(g_mutex);
    if (g_talking.erase({schid, clid})) send_state_locked();
}

bool is_own_client(uint64 schid, anyID clid) {
    anyID me = 0;
    return ts3.getClientID(schid, &me) == ERROR_ok && me == clid;
}
}  // namespace

// --- required exports ---------------------------------------------------------
EXPORT const char* ts3plugin_name() { return "YapNotifier"; }
EXPORT const char* ts3plugin_version() { return "0.1.0"; }
EXPORT int ts3plugin_apiVersion() { return PLUGIN_API_VERSION; }
EXPORT const char* ts3plugin_author() { return "YapNotifier"; }
EXPORT const char* ts3plugin_description() {
    return "Sends who-is-talking to the YapNotifier FiveM overlay (localhost UDP).";
}
EXPORT void ts3plugin_setFunctionPointers(const TS3Functions funcs) { ts3 = funcs; }

EXPORT int ts3plugin_init() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }
    g_dest.sin_family = AF_INET;
    g_dest.sin_port = htons(yap::proto::kPort);
    inet_pton(AF_INET, "127.0.0.1", &g_dest.sin_addr);

    g_stop = false;
    g_heartbeat = std::thread([] {
        while (!g_stop) {
            {
                std::lock_guard lk(g_mutex);
                send_state_locked();
            }
            for (int i = 0; i < 10 && !g_stop; ++i) Sleep(100);
        }
    });
    return 0;
}

EXPORT void ts3plugin_shutdown() {
    g_stop = true;
    if (g_heartbeat.joinable()) g_heartbeat.join();
    {
        std::lock_guard lk(g_mutex);
        g_talking.clear();
        send_state_locked();  // tell the overlay we're gone, don't wait for the stale timeout
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    WSACleanup();
}

// --- events -------------------------------------------------------------------
EXPORT void ts3plugin_onTalkStatusChangeEvent(uint64 schid, int status, int /*isReceivedWhisper*/, anyID clid) {
    if (is_own_client(schid, clid)) return;  // it's a "who's talking" overlay; you know when it's you
    if (status != STATUS_TALKING) {
        erase_client(schid, clid);
        return;
    }
    char* name = nullptr;
    if (ts3.getClientVariableAsString(schid, clid, CLIENT_NICKNAME, &name) != ERROR_ok) return;
    std::string nick(name);
    ts3.freeMemory(name);
    std::lock_guard lk(g_mutex);
    g_talking[{schid, clid}] = std::move(nick);
    send_state_locked();
}

// A client leaving our view (newChannelID == 0) will never send a
// "not talking" event, so drop them here.
EXPORT void ts3plugin_onClientMoveEvent(uint64 schid, anyID clid, uint64, uint64 new_channel, int, const char*) {
    if (new_channel == 0) erase_client(schid, clid);
}
EXPORT void ts3plugin_onClientMoveTimeoutEvent(uint64 schid, anyID clid, uint64, uint64 new_channel, int, const char*) {
    if (new_channel == 0) erase_client(schid, clid);
}
EXPORT void ts3plugin_onClientMoveMovedEvent(uint64 schid, anyID clid, uint64, uint64 new_channel, int, anyID,
                                             const char*, const char*, const char*) {
    if (new_channel == 0) erase_client(schid, clid);
}
EXPORT void ts3plugin_onClientKickFromServerEvent(uint64 schid, anyID clid, uint64, uint64, int, anyID,
                                                  const char*, const char*, const char*) {
    erase_client(schid, clid);
}

EXPORT void ts3plugin_onConnectStatusChangeEvent(uint64 schid, int new_status, unsigned int) {
    if (new_status != STATUS_DISCONNECTED) return;
    std::lock_guard lk(g_mutex);
    bool changed = false;
    for (auto it = g_talking.begin(); it != g_talking.end();) {
        if (it->first.first == schid) {
            it = g_talking.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) send_state_locked();
}
