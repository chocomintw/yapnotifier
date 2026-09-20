#include "teamspeak.h"

#include "log.h"
#include "yap_protocol.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <atomic>
#include <charconv>
#include <mutex>
#include <thread>

namespace {
using namespace yap;
using namespace yap::ts;

std::atomic<int> g_port{proto::kPort};
std::atomic<bool> g_stop{false};
std::thread g_thread;

// Render thread does a single load(); worker swaps in a fresh immutable copy.
std::atomic<std::shared_ptr<const Snapshot>> g_snapshot{std::make_shared<const Snapshot>()};

std::mutex g_events_mutex;
std::vector<Event> g_events;
constexpr size_t kMaxQueuedEvents = 64;

void publish(Snapshot s) { g_snapshot.store(std::make_shared<const Snapshot>(std::move(s))); }

void enqueue(std::vector<Event>& evs) {
    if (evs.empty()) return;
    std::lock_guard lk(g_events_mutex);
    for (auto& e : evs) g_events.push_back(std::move(e));
    if (g_events.size() > kMaxQueuedEvents)
        g_events.erase(g_events.begin(), g_events.begin() + (g_events.size() - kMaxQueuedEvents));
    evs.clear();
}

SOCKET bind_udp(int port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        log::error("ts: bind 127.0.0.1:{} failed ({})", port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    log::info("ts: listening on 127.0.0.1:{}", port);
    return s;
}

void run() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log::error("ts: WSAStartup failed");
        return;
    }
    SOCKET sock = INVALID_SOCKET;
    int bound_port = 0;
    ULONGLONG last_rx = 0;
    bool alive = false;
    static char buf[65536];
    Snapshot snap;
    std::vector<Event> events;

    while (!g_stop) {
        const int want = g_port.load();
        if (sock == INVALID_SOCKET || bound_port != want) {
            if (sock != INVALID_SOCKET) closesocket(sock);
            sock = bind_udp(want);
            bound_port = want;
            if (sock == INVALID_SOCKET) {
                for (int i = 0; i < 20 && !g_stop; ++i) Sleep(100);  // ponytail: fixed 2s retry
                continue;
            }
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        timeval tv{0, 500 * 1000};
        int r = select(0, &fds, nullptr, nullptr, &tv);
        if (r > 0) {
            int n = recv(sock, buf, sizeof buf, 0);
            if (n > 0 && parse_datagram({buf, static_cast<size_t>(n)}, snap, events)) {
                last_rx = GetTickCount64();
                if (!alive) log::info("ts: plugin connected{}", snap.legacy ? " (v1 plugin, talk state only)" : "");
                alive = true;
                snap.alive = true;
                enqueue(events);
                publish(snap);
            }
        } else if (alive && GetTickCount64() - last_rx > proto::kStaleAfterMs) {
            log::info("ts: plugin went quiet; clearing");
            alive = false;
            publish(Snapshot{});
        }
    }
    if (sock != INVALID_SOCKET) closesocket(sock);
    WSACleanup();
}

// Splits `line` on tabs into at most `n` fields; the last field takes the rest.
// Returns how many fields were filled.
int split(std::string_view line, std::string_view* out, int n) {
    int i = 0;
    while (i < n - 1) {
        size_t tab = line.find('\t');
        if (tab == std::string_view::npos) break;
        out[i++] = line.substr(0, tab);
        line.remove_prefix(tab + 1);
    }
    out[i++] = line;
    return i;
}

template <class T>
bool to_num(std::string_view s, T& v) {
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} && end == s.data() + s.size();
}

bool parse_v1(std::string_view data, Snapshot& out) {
    out = Snapshot{};
    out.legacy = true;
    out.conn = 2;
    while (!data.empty()) {
        size_t nl = data.find('\n');
        std::string_view line = data.substr(0, nl);
        data.remove_prefix(nl == std::string_view::npos ? data.size() : nl + 1);
        size_t tab = line.find('\t');
        if (tab == std::string_view::npos) continue;
        User u;
        if (!to_num(line.substr(0, tab), u.clid)) continue;
        u.flags = proto::Talking;
        u.nickname = line.substr(tab + 1);
        out.users.push_back(std::move(u));
    }
    return true;
}

struct EventKind {
    std::string_view name;
    Event::Kind kind;
    int fields;
};
constexpr EventKind kEventKinds[] = {
    {"join", Event::Join, 2},    {"leave", Event::Leave, 2},     {"switch", Event::Switch, 3},
    {"conn", Event::Conn, 2},    {"whisper", Event::Whisper, 2}, {"chat", Event::Chat, 4},
};

// f[0] is the kind; positional fields follow.
bool parse_event(const std::string_view* f, int n, Event& e) {
    for (const auto& k : kEventKinds) {
        if (f[0] != k.name) continue;
        if (n - 1 < k.fields) return false;
        e.kind = k.kind;
        for (int i = 0; i < k.fields; ++i) e.f[i] = f[1 + i];
        return true;
    }
    return false;
}
}  // namespace

namespace yap::ts {

void configure(int listen_port) { g_port = listen_port; }

void start() {
    if (g_thread.joinable()) return;
    g_stop = false;
    g_thread = std::thread(run);
}

void stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

std::shared_ptr<const Snapshot> snapshot() { return g_snapshot.load(); }

void drain_events(std::vector<Event>& out) {
    std::lock_guard lk(g_events_mutex);
    for (auto& e : g_events) out.push_back(std::move(e));
    g_events.clear();
}

bool parse_datagram(std::string_view data, Snapshot& out, std::vector<Event>& events) {
    constexpr std::string_view magic = proto::kMagic, magic_v1 = proto::kMagicV1;
    if (data.starts_with(magic_v1)) return parse_v1(data.substr(magic_v1.size()), out);
    if (!data.starts_with(magic)) return false;
    data.remove_prefix(magic.size());
    out = Snapshot{};
    while (!data.empty()) {
        size_t nl = data.find('\n');
        std::string_view line = data.substr(0, nl);
        data.remove_prefix(nl == std::string_view::npos ? data.size() : nl + 1);
        if (line.size() < 2 || line[1] != '\t') continue;
        std::string_view f[6];
        std::string_view body = line.substr(2);
        switch (line[0]) {
            case 'S': {
                if (split(body, f, 2) < 2 || !to_num(f[0], out.conn)) continue;
                out.server_name = f[1];
                break;
            }
            case 'C': {
                if (split(body, f, 3) < 3 || !to_num(f[0], out.channel_id)) continue;
                out.parent = f[1];
                out.channel = f[2];
                break;
            }
            case 'U': {
                if (out.users.size() >= static_cast<size_t>(proto::kMaxUsers)) continue;
                User u;
                if (split(body, f, 5) < 5 || !to_num(f[0], u.clid) || !to_num(f[1], u.flags)) continue;
                u.uid = f[2];
                u.contact_nick = f[3];
                u.nickname = f[4];
                out.users.push_back(std::move(u));
                break;
            }
            case 'E': {
                Event e;
                if (parse_event(f, split(body, f, 5), e)) events.push_back(std::move(e));
                break;
            }
            default:
                break;  // unknown line type: a newer plugin, ignore
        }
    }
    return true;
}

}  // namespace yap::ts
