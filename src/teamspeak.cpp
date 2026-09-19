#include "teamspeak.h"

#include "log.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>

namespace {
using namespace yap;
using namespace yap::ts;

// --- connection settings (written by UI thread, read by worker) -------------
std::mutex g_cfg_mutex;
std::string g_host = "127.0.0.1";
std::string g_key;
int g_port = 25639;

// --- worker state ------------------------------------------------------------
std::atomic<bool> g_stop{false};
std::thread g_thread;
std::map<uint16_t, Client> g_clients;  // owned by the worker thread only

// Render thread does a single load(); worker swaps in a fresh immutable copy.
std::atomic<std::shared_ptr<const Snapshot>> g_snapshot{std::make_shared<const Snapshot>()};

void publish(bool connected) {
    auto s = std::make_shared<Snapshot>();
    s->connected = connected;
    s->clients.reserve(g_clients.size());
    for (const auto& [id, c] : g_clients) s->clients.push_back(c);
    g_snapshot.store(std::move(s));
}

// --- minimal RAII socket ------------------------------------------------------
class Socket {
public:
    ~Socket() { close(); }

    bool connect(const std::string& host, int port) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;
        s_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        bool ok = s_ != INVALID_SOCKET && ::connect(s_, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0;
        freeaddrinfo(res);
        if (!ok) close();
        return ok;
    }

    bool send_line(std::string_view line) {
        std::string msg(line);
        msg += "\n";
        return ::send(s_, msg.data(), static_cast<int>(msg.size()), 0) == static_cast<int>(msg.size());
    }

    enum class Read { Line, Timeout, Closed };

    // Waits up to timeout_ms for a full line. ClientQuery terminates lines
    // with "\n\r"; we split on '\n' and strip any '\r'.
    Read read_line(std::string& out, int timeout_ms) {
        for (;;) {
            if (auto nl = buf_.find('\n'); nl != std::string::npos) {
                out.assign(buf_, 0, nl);
                buf_.erase(0, nl + 1);
                while (!out.empty() && out.back() == '\r') out.pop_back();
                if (out.empty()) continue;  // stray '\r' line
                return Read::Line;
            }
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(s_, &fds);
            timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            int r = ::select(0, &fds, nullptr, nullptr, &tv);
            if (r == 0) return Read::Timeout;
            if (r < 0) return Read::Closed;
            char chunk[4096];
            int n = ::recv(s_, chunk, sizeof chunk, 0);
            if (n <= 0) return Read::Closed;
            buf_.append(chunk, n);
        }
    }

    void close() {
        if (s_ != INVALID_SOCKET) closesocket(s_);
        s_ = INVALID_SOCKET;
    }

private:
    SOCKET s_ = INVALID_SOCKET;
    std::string buf_;
};

// --- protocol ------------------------------------------------------------------
// TODO(protocol): fill in. Lines we will see, in order:
//   banner ("TS3 Client", "Welcome to the TeamSpeak 3 ClientQuery interface, ...",
//           "selected schandlerid=1")
//   "error id=0 msg=ok"                          after each command
//   "notifytalkstatuschange schandlerid=1 status=1 isreceivedwhisper=0 clid=5"
//   "notifycliententerview ..." / "notifyclientleftview ..." / "notifyclientmoved ..."
//   clientlist response: "clid=5 cid=1 client_database_id=2 client_nickname=Foo|clid=6 ..."
// Values use ClientQuery escaping: \s=space \p=pipe \/=slash \=backslash.
// Mutate g_clients then call publish(true) when something visible changed.
void handle_line(std::string_view line) {
    (void)line;
}

// One connection lifetime. Returns true if we got as far as connecting
// (so the caller resets its backoff).
bool session() {
    std::string host, key;
    int port;
    {
        std::lock_guard lk(g_cfg_mutex);
        host = g_host;
        port = g_port;
        key = g_key;
    }
    if (key.empty()) return false;  // nothing to do until the user sets a key

    Socket sock;
    if (!sock.connect(host, port)) return false;
    log::info("ts: connected to {}:{}", host, port);

    // TODO(protocol): confirm exact register command / whether clientlist is
    // needed up front for nicknames.
    if (!sock.send_line("auth apikey=" + key) ||
        !sock.send_line("clientnotifyregister schandlerid=0 event=any")) {
        return true;
    }
    publish(true);

    std::string line;
    while (!g_stop) {
        switch (sock.read_line(line, 500)) {
            case Socket::Read::Line: handle_line(line); break;
            case Socket::Read::Timeout: break;
            case Socket::Read::Closed:
                log::info("ts: connection closed");
                return true;
        }
    }
    return true;
}

void run() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log::error("ts: WSAStartup failed");
        return;
    }
    int backoff_ms = 1000;
    bool logged_idle = false;
    while (!g_stop) {
        bool connected = false;
        try {
            connected = session();
        } catch (const std::exception& e) {
            log::error("ts: exception in session: {}", e.what());
        }
        g_clients.clear();
        publish(false);

        backoff_ms = connected ? 1000 : std::min(backoff_ms * 2, 15000);
        if (!connected && !logged_idle) {
            log::info("ts: not connected (TeamSpeak/ClientQuery down or no api_key); retrying");
            logged_idle = true;
        }
        if (connected) logged_idle = false;
        for (int t = 0; t < backoff_ms && !g_stop; t += 100) Sleep(100);
    }
    WSACleanup();
}
}  // namespace

namespace yap::ts {

void configure(std::string host, int port, std::string api_key) {
    std::lock_guard lk(g_cfg_mutex);
    g_host = std::move(host);
    g_port = port;
    g_key = std::move(api_key);
}

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

}  // namespace yap::ts
