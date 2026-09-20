#pragma once
// Wire format shared by the TS3 plugin (sender) and the .asi (receiver).
//
// One UDP datagram to 127.0.0.1:kPort carries the COMPLETE current state of
// the active TeamSpeak tab. Sent on every change and once per second as a
// heartbeat, so a late-starting receiver converges within a second and a dead
// sender is detected by silence. Lines are tab-separated; the LAST field of a
// line is the remainder of the line, so free text always goes last. The plugin
// replaces \t \n \r inside free text with spaces.
//
//   YAP2\n
//   S\t<conn>\t<server_name>\n                          conn: 0 off, 1 connecting, 2 connected
//   C\t<channel_id>\t<parent_name>\t<channel_name>\n     only while connected
//   U\t<clid>\t<flags>\t<uid>\t<contact_nick>\t<nick>\n  every client in the own channel
//   E\t<kind>\t...\n                                     one-shot events, never in heartbeats
//
// Event kinds (fields after <kind>):
//   join\t<from_channel>\t<name>            empty from = joined the server
//   leave\t<to_channel>\t<name>             empty to   = left the server
//   switch\t<count>\t<prev>\t<channel>      own client moved
//   conn\t<server_name>\t<status>           connecting|connected|disconnected|timeout|kicked|banned|server_shutdown
//   whisper\t<from_channel>\t<name>         empty from = own channel
//   chat\t<category>\t<sender>\t<channel>\t<text>   category: channel|server|private|poke
//
// Chat crosses loopback to the same user's game process regardless of what the
// overlay chooses to show; the overlay filters by category (private off by
// default). ponytail: a reverse subscribe datagram would let the plugin filter
// at the source if that ever matters.
//
// The .asi still accepts the v1 talk-only format ("YAP1\n" + "<clid>\t<nick>\n"
// lines) so a self-updated .asi keeps working with an old plugin.
#include <cstdint>

namespace yap::proto {
constexpr int kPort = 25640;
constexpr char kMagic[] = "YAP2\n";
constexpr char kMagicV1[] = "YAP1\n";
constexpr int kStaleAfterMs = 3000;
constexpr int kMaxUsers = 256;      // U lines per datagram (64 KiB UDP ceiling)
constexpr int kMaxChatChars = 200;  // chat text is truncated by the plugin

enum Flag : uint32_t {
    Self = 1u << 0,
    Talking = 1u << 1,
    Whisper = 1u << 2,  // whispering to us
    InputMuted = 1u << 3,
    OutputMuted = 1u << 4,
    Away = 1u << 5,
    Recording = 1u << 6,
    Commander = 1u << 7,
    Priority = 1u << 8,
    LocallyMuted = 1u << 9,
    HardwareOff = 1u << 10,  // no input device
    Suppressed = 1u << 11,   // not a talker in a moderated channel
    Friend = 1u << 12,
    Blocked = 1u << 13,
};
}  // namespace yap::proto
