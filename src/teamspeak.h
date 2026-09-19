#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// TeamSpeak 3 ClientQuery client. Runs on its own thread, reconnects forever,
// and publishes immutable snapshots the render thread reads lock-free.
namespace yap::ts {

struct Client {
    uint16_t clid = 0;
    std::string nickname;
    uint64_t channel = 0;
    bool talking = false;
};

struct Snapshot {
    bool connected = false;
    std::vector<Client> clients;
};

// Connection settings; picked up on the next (re)connect. Thread-safe.
void configure(std::string host, int port, std::string api_key);

void start();
void stop();  // blocks until the worker thread has exited (<= ~1s)

// Never null after start(); empty snapshot while disconnected.
std::shared_ptr<const Snapshot> snapshot();

}  // namespace yap::ts
