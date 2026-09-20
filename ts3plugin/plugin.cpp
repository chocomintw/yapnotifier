// TeamSpeak 3 client plugin: mirrors the active tab's channel roster, per-client
// state and one-shot events to the YapNotifier .asi as UDP datagrams on
// localhost. See shared/yap_protocol.h for the format.
//
// Model: every roster-affecting event triggers a full resync from the SDK
// (channels are small; correctness over cleverness). Talk/whisper state is not
// a client property, so it lives in two sets and is OR'd in at send time.
#include <WinSock2.h>
#include <WS2tcpip.h>

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "teamspeak/public_rare_definitions.h"
#include "ts3_functions.h"
#include "ts_contacts.hpp"
#include "version.h"
#include "yap_protocol.h"

#define PLUGIN_API_VERSION 26
#define EXPORT extern "C" __declspec(dllexport)

namespace {
namespace proto = yap::proto;
TS3Functions ts3{};

struct Client {
    anyID clid = 0;
    uint32_t flags = 0;
    std::string uid, contact_nick, nick;
};

std::mutex g_mutex;  // guards everything in this block
uint64 g_schid = 0;  // the tab we mirror
int g_conn = 0;      // 0 off, 1 connecting, 2 connected
std::string g_server_name;
uint64 g_channel = 0;
std::string g_channel_name, g_parent_name;
std::vector<Client> g_roster;
std::set<anyID> g_talking, g_whispering;
std::vector<std::string> g_events;  // E lines for the next datagram

std::map<std::string, tsro::Contact> g_contacts;
std::vector<std::string> g_contact_blobs;
ULONGLONG g_contacts_read_ms = 0;
std::string g_config_dir;

SOCKET g_sock = INVALID_SOCKET;
sockaddr_in g_dest{};
std::thread g_heartbeat;
std::atomic<bool> g_stop{false};

// --- SDK helpers ----------------------------------------------------------------
std::string clean(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string str_var(uint64 schid, anyID clid, size_t prop) {
    char* v = nullptr;
    if (ts3.getClientVariableAsString(schid, clid, prop, &v) != ERROR_ok || !v) return {};
    std::string s(v);
    ts3.freeMemory(v);
    return s;
}

int int_var(uint64 schid, anyID clid, size_t prop) {
    int v = 0;
    return ts3.getClientVariableAsInt(schid, clid, prop, &v) == ERROR_ok ? v : 0;
}

std::string display_name(uint64 schid, anyID clid) {
    char buf[512] = {};
    if (ts3.getClientDisplayName(schid, clid, buf, sizeof buf) == ERROR_ok && buf[0]) return buf;
    return str_var(schid, clid, CLIENT_NICKNAME);
}

std::string channel_name(uint64 schid, uint64 chid) {
    if (!chid) return {};
    char* v = nullptr;
    if (ts3.getChannelVariableAsString(schid, chid, CHANNEL_NAME, &v) != ERROR_ok || !v) return {};
    std::string s(v);
    ts3.freeMemory(v);
    return s;
}

anyID own_id(uint64 schid) {
    anyID me = 0;
    return ts3.getClientID(schid, &me) == ERROR_ok ? me : 0;
}

// --- contacts (TS3 keeps friends in settings.db, not in the plugin API) ---------
void refresh_contacts_locked(bool force) {
    ULONGLONG now = GetTickCount64();
    if (!force && g_contacts_read_ms && now - g_contacts_read_ms < 10000) return;
    g_contacts_read_ms = now;
    std::vector<tsro::Contact> contacts;
    std::string error;
    std::vector<std::string> blobs;
    if (!tsro::read_contacts(g_config_dir, contacts, error, nullptr, &blobs)) return;
    g_contacts.clear();
    for (auto& c : contacts) g_contacts.emplace(c.unique_id, std::move(c));
    g_contact_blobs = std::move(blobs);
}

void stamp_contact_locked(Client& c) {
    auto it = g_contacts.find(c.uid);
    if (it == g_contacts.end()) {
        tsro::Contact found;
        for (const auto& blob : g_contact_blobs) {
            if (!tsro::contact_from_blob(blob, c.uid, found)) continue;
            it = g_contacts.emplace(c.uid, std::move(found)).first;
            break;
        }
    }
    if (it == g_contacts.end()) return;
    if (it->second.kind == tsro::ContactKind::Friend) c.flags |= proto::Friend;
    if (it->second.kind == tsro::ContactKind::Blocked) c.flags |= proto::Blocked;
    c.contact_nick = clean(it->second.nickname);
}

// --- state --------------------------------------------------------------------
int conn_state(uint64 schid) {
    int st = STATUS_DISCONNECTED;
    if (!schid || ts3.getConnectionStatus(schid, &st) != ERROR_ok) return 0;
    if (st == STATUS_DISCONNECTED) return 0;
    if (st == STATUS_CONNECTED || st == STATUS_CONNECTION_ESTABLISHED) return 2;
    return 1;
}

void resync_locked() {
    g_roster.clear();
    g_channel = 0;
    g_channel_name.clear();
    g_parent_name.clear();
    g_server_name.clear();
    g_conn = conn_state(g_schid);
    if (g_conn != 2) return;

    char* sv = nullptr;
    if (ts3.getServerVariableAsString(g_schid, VIRTUALSERVER_NAME, &sv) == ERROR_ok && sv) {
        g_server_name = clean(sv);
        ts3.freeMemory(sv);
    }
    anyID me = own_id(g_schid);
    if (!me || ts3.getChannelOfClient(g_schid, me, &g_channel) != ERROR_ok) return;
    g_channel_name = clean(channel_name(g_schid, g_channel));
    uint64 parent = 0;
    if (ts3.getParentChannelOfChannel(g_schid, g_channel, &parent) == ERROR_ok)
        g_parent_name = clean(channel_name(g_schid, parent));

    anyID* list = nullptr;
    if (ts3.getChannelClientList(g_schid, g_channel, &list) != ERROR_ok || !list) return;
    for (int i = 0; list[i] && i < proto::kMaxUsers; ++i) {
        Client c;
        c.clid = list[i];
        c.uid = clean(str_var(g_schid, c.clid, CLIENT_UNIQUE_IDENTIFIER));
        c.nick = clean(display_name(g_schid, c.clid));
        const bool self = c.clid == me;
        if (self) c.flags |= proto::Self;
        if (int_var(g_schid, c.clid, CLIENT_INPUT_MUTED)) c.flags |= proto::InputMuted;
        if (int_var(g_schid, c.clid, CLIENT_OUTPUT_MUTED) || int_var(g_schid, c.clid, CLIENT_OUTPUTONLY_MUTED))
            c.flags |= proto::OutputMuted;
        if (!int_var(g_schid, c.clid, CLIENT_INPUT_HARDWARE)) c.flags |= proto::HardwareOff;
        if (int_var(g_schid, c.clid, CLIENT_AWAY)) c.flags |= proto::Away;
        if (int_var(g_schid, c.clid, CLIENT_IS_RECORDING)) c.flags |= proto::Recording;
        if (int_var(g_schid, c.clid, CLIENT_IS_CHANNEL_COMMANDER)) c.flags |= proto::Commander;
        if (int_var(g_schid, c.clid, CLIENT_IS_PRIORITY_SPEAKER)) c.flags |= proto::Priority;
        if (!int_var(g_schid, c.clid, CLIENT_IS_TALKER)) c.flags |= proto::Suppressed;
        if (self) {
            int v = 0;
            if (ts3.getClientSelfVariableAsInt(g_schid, CLIENT_INPUT_DEACTIVATED, &v) == ERROR_ok && v)
                c.flags |= proto::InputMuted;
        } else if (int_var(g_schid, c.clid, CLIENT_IS_MUTED)) {
            c.flags |= proto::LocallyMuted;
        }
        stamp_contact_locked(c);
        g_roster.push_back(std::move(c));
    }
    ts3.freeMemory(list);
}

void send_locked() {
    if (g_sock == INVALID_SOCKET) return;
    std::string d = proto::kMagic;
    d += "S\t" + std::to_string(g_conn) + '\t' + g_server_name + '\n';
    if (g_conn == 2) {
        d += "C\t" + std::to_string(g_channel) + '\t' + g_parent_name + '\t' + g_channel_name + '\n';
        for (const auto& c : g_roster) {
            uint32_t flags = c.flags;
            if (g_talking.count(c.clid)) flags |= proto::Talking;
            if (g_whispering.count(c.clid)) flags |= proto::Whisper;
            d += "U\t" + std::to_string(c.clid) + '\t' + std::to_string(flags) + '\t' + c.uid + '\t' +
                 c.contact_nick + '\t' + c.nick + '\n';
        }
    }
    for (const auto& e : g_events) d += e;
    g_events.clear();
    sendto(g_sock, d.data(), static_cast<int>(d.size()), 0, reinterpret_cast<const sockaddr*>(&g_dest),
           sizeof g_dest);
}

void event_locked(std::string line) { g_events.push_back("E\t" + std::move(line) + '\n'); }

void resync_and_send_locked() {
    resync_locked();
    send_locked();
}

// Name of a client that may already be gone from the SDK's view: cached roster first.
std::string name_of_locked(uint64 schid, anyID clid) {
    for (const auto& c : g_roster)
        if (c.clid == clid) return c.nick;
    return clean(display_name(schid, clid));
}

// Every move-shaped callback lands here. `reason` is only used when we ourselves leave.
void on_move(uint64 schid, anyID clid, uint64 old_ch, uint64 new_ch, const char* reason) {
    std::lock_guard lk(g_mutex);
    if (schid != g_schid) return;
    const anyID me = own_id(schid);
    if (clid == me) {
        if (new_ch == 0) {
            if (reason) event_locked("conn\t" + g_server_name + '\t' + reason);
        } else {
            const std::string prev = g_channel_name;
            resync_locked();
            event_locked("switch\t" + std::to_string(g_roster.size()) + '\t' + prev + '\t' + g_channel_name);
            send_locked();
            return;
        }
    } else if (g_channel) {
        if (new_ch == g_channel && old_ch != g_channel)
            event_locked("join\t" + clean(channel_name(schid, old_ch)) + '\t' + name_of_locked(schid, clid));
        else if (old_ch == g_channel && new_ch != g_channel)
            event_locked("leave\t" + clean(channel_name(schid, new_ch)) + '\t' + name_of_locked(schid, clid));
    }
    resync_and_send_locked();
}

std::string config_dir() {
    char buf[1024] = {};
    ts3.getConfigPath(buf, sizeof buf);
    return buf;
}

std::string truncate_utf8(std::string s, size_t max) {
    if (s.size() <= max) return s;
    s.resize(max);
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    return s;
}

void chat_event(uint64 schid, const char* category, const char* from, const char* text) {
    std::lock_guard lk(g_mutex);
    if (schid != g_schid) return;
    std::string msg = truncate_utf8(clean(text ? text : ""), proto::kMaxChatChars);
    event_locked(std::string("chat\t") + category + '\t' + clean(from ? from : "") + '\t' + g_channel_name + '\t' + msg);
    send_locked();
}
}  // namespace

// --- required exports ---------------------------------------------------------
EXPORT const char* ts3plugin_name() { return "YapNotifier"; }
EXPORT const char* ts3plugin_version() { return YAP_VERSION; }
EXPORT int ts3plugin_apiVersion() { return PLUGIN_API_VERSION; }
EXPORT const char* ts3plugin_author() { return "YapNotifier"; }
EXPORT const char* ts3plugin_description() {
    return "Mirrors your channel, who is talking, mute/away state, notifications and chat to the "
           "YapNotifier FiveM overlay (localhost UDP).";
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
    g_dest.sin_port = htons(proto::kPort);
    inet_pton(AF_INET, "127.0.0.1", &g_dest.sin_addr);

    {
        std::lock_guard lk(g_mutex);
        g_config_dir = config_dir();
        refresh_contacts_locked(true);
        // Adopt whatever tab is active: the plugin is usually enabled mid-session.
        g_schid = ts3.getCurrentServerConnectionHandlerID();
        resync_locked();
    }

    g_stop = false;
    g_heartbeat = std::thread([] {
        while (!g_stop) {
            {
                std::lock_guard lk(g_mutex);
                send_locked();
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
        g_conn = 0;
        g_roster.clear();
        g_events.clear();
        send_locked();  // tell the overlay we're gone, don't wait for the stale timeout
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    WSACleanup();
}

// --- connection / tab -----------------------------------------------------------
EXPORT void ts3plugin_currentServerConnectionChanged(uint64 schid) {
    std::lock_guard lk(g_mutex);
    g_schid = schid;
    g_talking.clear();
    g_whispering.clear();
    resync_and_send_locked();
}

EXPORT void ts3plugin_onConnectStatusChangeEvent(uint64 schid, int new_status, unsigned int error_number) {
    std::lock_guard lk(g_mutex);
    if (schid != g_schid) return;
    const int prev = g_conn;
    if (new_status == STATUS_DISCONNECTED) {
        g_talking.clear();
        g_whispering.clear();
    }
    if (new_status == STATUS_CONNECTION_ESTABLISHED) refresh_contacts_locked(true);
    resync_locked();
    if (g_conn != prev) {
        const char* status = g_conn == 2 ? "connected" : g_conn == 1 ? "connecting"
                                       : error_number ? "connection_lost" : "disconnected";
        event_locked("conn\t" + g_server_name + '\t' + status);
    }
    send_locked();
}

EXPORT void ts3plugin_onServerStopEvent(uint64 schid, const char*) {
    std::lock_guard lk(g_mutex);
    if (schid != g_schid) return;
    event_locked("conn\t" + g_server_name + "\tserver_shutdown");
    send_locked();
}

// --- roster ---------------------------------------------------------------------
EXPORT void ts3plugin_onClientMoveEvent(uint64 schid, anyID clid, uint64 old_ch, uint64 new_ch, int, const char*) {
    on_move(schid, clid, old_ch, new_ch, nullptr);
}
EXPORT void ts3plugin_onClientMoveSubscriptionEvent(uint64 schid, anyID, uint64, uint64, int) {
    // Visibility change, not a move: no join toast, just make the roster complete.
    std::lock_guard lk(g_mutex);
    if (schid == g_schid) resync_and_send_locked();
}
EXPORT void ts3plugin_onClientMoveTimeoutEvent(uint64 schid, anyID clid, uint64 old_ch, uint64 new_ch, int,
                                               const char*) {
    on_move(schid, clid, old_ch, new_ch, "connection_lost");
}
EXPORT void ts3plugin_onClientMoveMovedEvent(uint64 schid, anyID clid, uint64 old_ch, uint64 new_ch, int, anyID,
                                             const char*, const char*, const char*) {
    on_move(schid, clid, old_ch, new_ch, nullptr);
}
EXPORT void ts3plugin_onClientKickFromChannelEvent(uint64 schid, anyID clid, uint64 old_ch, uint64 new_ch, int,
                                                   anyID, const char*, const char*, const char*) {
    on_move(schid, clid, old_ch, new_ch, nullptr);
}
EXPORT void ts3plugin_onClientKickFromServerEvent(uint64 schid, anyID clid, uint64 old_ch, uint64 new_ch, int,
                                                  anyID, const char*, const char*, const char*) {
    on_move(schid, clid, old_ch, new_ch, "kicked");
}
EXPORT void ts3plugin_onClientBanFromServerEvent(uint64 schid, anyID clid, uint64 old_ch, uint64 new_ch, int,
                                                 anyID, const char*, const char*, uint64, const char*) {
    on_move(schid, clid, old_ch, new_ch, "banned");
}

EXPORT void ts3plugin_onTalkStatusChangeEvent(uint64 schid, int status, int is_whisper, anyID clid) {
    std::lock_guard lk(g_mutex);
    if (schid != g_schid) return;
    const bool talking = status == STATUS_TALKING;
    if (talking) {
        g_talking.insert(clid);
        if (is_whisper) g_whispering.insert(clid);
    } else {
        g_talking.erase(clid);
        g_whispering.erase(clid);
    }
    if (talking && is_whisper) {
        bool in_channel = false;
        for (const auto& c : g_roster) in_channel |= c.clid == clid;
        if (!in_channel) {
            uint64 ch = 0;
            ts3.getChannelOfClient(schid, clid, &ch);
            event_locked("whisper\t" + clean(channel_name(schid, ch)) + '\t' + clean(display_name(schid, clid)));
        }
    }
    send_locked();
}

EXPORT void ts3plugin_onUpdateClientEvent(uint64 schid, anyID, anyID, const char*, const char*) {
    std::lock_guard lk(g_mutex);
    if (schid == g_schid) resync_and_send_locked();
}
EXPORT void ts3plugin_onClientSelfVariableUpdateEvent(uint64 schid, int, const char*, const char*) {
    std::lock_guard lk(g_mutex);
    if (schid == g_schid) resync_and_send_locked();
}
EXPORT void ts3plugin_onClientDisplayNameChanged(uint64 schid, anyID, const char*, const char*) {
    std::lock_guard lk(g_mutex);
    if (schid == g_schid) resync_and_send_locked();
}
EXPORT void ts3plugin_onUpdateChannelEvent(uint64 schid, uint64) {
    std::lock_guard lk(g_mutex);
    if (schid == g_schid) resync_and_send_locked();
}
EXPORT void ts3plugin_onUpdateChannelEditedEvent(uint64 schid, uint64, anyID, const char*, const char*) {
    std::lock_guard lk(g_mutex);
    if (schid == g_schid) resync_and_send_locked();
}
EXPORT void ts3plugin_onChannelSubscribeFinishedEvent(uint64 schid) {
    std::lock_guard lk(g_mutex);
    if (schid != g_schid) return;
    refresh_contacts_locked(false);
    resync_and_send_locked();
}

// --- chat -----------------------------------------------------------------------
EXPORT int ts3plugin_onTextMessageEvent(uint64 schid, anyID target_mode, anyID, anyID, const char* from_name,
                                        const char*, const char* message, int ff_ignored) {
    if (ff_ignored) return 0;  // the client is ignoring this sender; so do we
    const char* cat = target_mode == TextMessageTarget_CLIENT    ? "private"
                      : target_mode == TextMessageTarget_CHANNEL ? "channel"
                      : target_mode == TextMessageTarget_SERVER  ? "server"
                                                                 : nullptr;
    if (cat) chat_event(schid, cat, from_name, message);
    return 0;  // never consume: TeamSpeak still shows the message
}

EXPORT int ts3plugin_onClientPokeEvent(uint64 schid, anyID, const char* poker_name, const char*, const char* message,
                                       int ff_ignored) {
    if (!ff_ignored) chat_event(schid, "poke", poker_name, message);
    return 0;
}
