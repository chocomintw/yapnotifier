#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Receives state datagrams from the YapNotifier TS3 plugin
// (shared/yap_protocol.h). Runs on its own thread and publishes immutable
// snapshots the render thread reads lock-free; one-shot events go through a
// small queue the render thread drains once per frame.
namespace yap::ts {

struct User {
    uint16_t clid = 0;
    uint32_t flags = 0;  // yap::proto::Flag bits
    std::string uid;
    std::string contact_nick;  // name from the TS contact list, if any
    std::string nickname;
};

struct Snapshot {
    bool alive = false;   // heard from the plugin within the stale window
    bool legacy = false;  // plugin speaks v1: talk state only
    int conn = 0;         // 0 off, 1 connecting, 2 connected
    std::string server_name;
    uint64_t channel_id = 0;
    std::string channel;
    std::string parent;
    std::vector<User> users;
};

struct Event {
    enum Kind { Join, Leave, Switch, Conn, Whisper, Chat } kind = Join;
    std::string f[5];  // positional fields as documented in yap_protocol.h
};

// UDP port to listen on; a change takes effect within ~0.5s. Thread-safe.
void configure(int listen_port);

void start();
void stop();  // blocks until the worker thread has exited (<= ~1s)

// Never null after start(); empty + not alive until the plugin speaks.
std::shared_ptr<const Snapshot> snapshot();

// Moves queued events into `out` (appends). Render thread only.
void drain_events(std::vector<Event>& out);

// Parses one datagram into `out` (fully overwritten) and appends events.
// Returns false and leaves both untouched unless it starts with a known magic.
// Malformed lines are skipped.
bool parse_datagram(std::string_view data, Snapshot& out, std::vector<Event>& events);

}  // namespace yap::ts
