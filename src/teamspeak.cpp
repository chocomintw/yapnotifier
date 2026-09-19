#include "teamspeak.h"

#include "log.h"
#include "yap_protocol.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <atomic>
#include <charconv>
#include <thread>

namespace {
using namespace yap;
using namespace yap::ts;

std::atomic<int> g_port{proto::kPort};
std::atomic<bool> g_stop{false};
std::thread g_thread;

// Render thread does a single load(); worker swaps in a fresh immutable copy.
std::atomic<std::shared_ptr<const Snapshot>> g_snapshot{std::make_shared<const Snapshot>()};

void publish(bool connected, std::vector<Client> talking) {
    auto s = std::make_shared<Snapshot>();
    s->connected = connected;
    s->talking = std::move(talking);
    g_snapshot.store(std::move(s));
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
    bool connected = false;
    char buf[65536];
    std::vector<Client> talking;

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
            if (n > 0 && parse_datagram({buf, static_cast<size_t>(n)}, talking)) {
                last_rx = GetTickCount64();
                if (!connected) log::info("ts: plugin connected");
                connected = true;
                publish(true, std::move(talking));
                talking.clear();
            }
        } else if (connected && GetTickCount64() - last_rx > proto::kStaleAfterMs) {
            log::info("ts: plugin went quiet; clearing");
            connected = false;
            publish(false, {});
        }
    }
    if (sock != INVALID_SOCKET) closesocket(sock);
    WSACleanup();
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

bool parse_datagram(std::string_view data, std::vector<Client>& out) {
    constexpr std::string_view magic = proto::kMagic;
    if (!data.starts_with(magic)) return false;
    data.remove_prefix(magic.size());
    out.clear();
    while (!data.empty()) {
        size_t nl = data.find('\n');
        std::string_view line = data.substr(0, nl);
        data.remove_prefix(nl == std::string_view::npos ? data.size() : nl + 1);
        size_t tab = line.find('\t');
        if (tab == std::string_view::npos) continue;
        Client c;
        auto [end, ec] = std::from_chars(line.data(), line.data() + tab, c.clid);
        if (ec != std::errc{} || end != line.data() + tab) continue;
        c.nickname = line.substr(tab + 1);
        out.push_back(std::move(c));
    }
    return true;
}

}  // namespace yap::ts
