#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Receives "who is talking" datagrams from the YapNotifier TS3 plugin
// (shared/yap_protocol.h). Runs on its own thread and publishes immutable
// snapshots the render thread reads lock-free.
namespace yap::ts {

struct Client {
    uint16_t clid = 0;
    std::string nickname;
};

struct Snapshot {
    bool connected = false;  // heard from the plugin within the stale window
    std::vector<Client> talking;
};

// UDP port to listen on; a change takes effect within ~0.5s. Thread-safe.
void configure(int listen_port);

void start();
void stop();  // blocks until the worker thread has exited (<= ~1s)

// Never null after start(); empty + disconnected until the plugin speaks.
std::shared_ptr<const Snapshot> snapshot();

// Parses one datagram. Returns false (and leaves `out` untouched) unless it
// starts with the protocol magic. Malformed lines are skipped.
bool parse_datagram(std::string_view data, std::vector<Client>& out);

}  // namespace yap::ts
