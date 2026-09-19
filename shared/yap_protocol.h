#pragma once
// Wire format shared by the TS3 plugin (sender) and the .asi (receiver).
//
// One UDP datagram to 127.0.0.1:kPort carries the COMPLETE current list of
// talking clients. Sent on every change and once per second as a heartbeat,
// so a late-starting receiver converges within a second and a dead sender is
// detected by silence.
//
//   YAP1\n
//   <clid>\t<nickname>\n      (zero or more)
//
// Nicknames are UTF-8 and never contain '\t' or '\n' (TS3 forbids both).
namespace yap::proto {
constexpr int kPort = 25640;
constexpr char kMagic[] = "YAP1\n";
constexpr int kStaleAfterMs = 3000;
}  // namespace yap::proto
