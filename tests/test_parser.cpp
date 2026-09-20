// Host-side check for the pure logic: datagram parser, updater helpers.
#include <cstdio>
#include <string>
#include <vector>

#include "teamspeak.h"
#include "notify.h"
#include "roster.h"
#include "update.h"
#include "yap_protocol.h"

#define CHECK(x)                                                        \
    do {                                                                \
        if (!(x)) {                                                     \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);     \
            return 1;                                                   \
        }                                                               \
    } while (0)

using yap::ts::Event;
using yap::ts::Snapshot;
using yap::ts::parse_datagram;
namespace proto = yap::proto;

int main() {
    Snapshot snap;
    snap.server_name = "stale";
    std::vector<Event> ev;

    // wrong magic: rejected, output untouched
    CHECK(!parse_datagram("NOPE\n1\tBob\n", snap, ev));
    CHECK(snap.server_name == "stale" && ev.empty());

    // v1 plugin: talk-only list, flagged legacy
    CHECK(parse_datagram("YAP1\n5\tBob\n12\tJos\xC3\xA9 the Second\n", snap, ev));
    CHECK(snap.legacy && snap.conn == 2 && snap.users.size() == 2);
    CHECK(snap.users[0].clid == 5 && snap.users[0].nickname == "Bob" && snap.users[0].flags == proto::Talking);
    CHECK(snap.users[1].clid == 12 && snap.users[1].nickname == "Jos\xC3\xA9 the Second");

    // v2 heartbeat while disconnected
    CHECK(parse_datagram("YAP2\nS\t0\t\n", snap, ev));
    CHECK(!snap.legacy && snap.conn == 0 && snap.users.empty() && snap.channel.empty() && ev.empty());

    // full v2 state: server, channel, users with flags; tabs only split up to the last field
    CHECK(parse_datagram("YAP2\n"
                         "S\t2\tMy Server\n"
                         "C\t42\tLobby\tGeneral\n"
                         "U\t1\t3\tuid1=\t\tMe\n"
                         "U\t7\t136\tuid7=\tBest Friend\tBob\tTabbed\n",
                         snap, ev));
    CHECK(snap.conn == 2 && snap.server_name == "My Server");
    CHECK(snap.channel_id == 42 && snap.parent == "Lobby" && snap.channel == "General");
    CHECK(snap.users.size() == 2);
    CHECK(snap.users[0].clid == 1 && snap.users[0].flags == (proto::Self | proto::Talking) &&
          snap.users[0].uid == "uid1=" && snap.users[0].contact_nick.empty() && snap.users[0].nickname == "Me");
    CHECK(snap.users[1].flags == (proto::Commander | proto::InputMuted) &&
          snap.users[1].contact_nick == "Best Friend" && snap.users[1].nickname == "Bob\tTabbed");

    // events: every kind, chat text keeps embedded tabs, missing fields rejected, unknown kind ignored
    CHECK(parse_datagram("YAP2\nS\t2\tX\n"
                         "E\tjoin\tLobby\tAlice\n"
                         "E\tleave\t\tBob\n"
                         "E\tswitch\t3\tOld\tNew\n"
                         "E\tconn\tX\tconnected\n"
                         "E\twhisper\tElsewhere\tCarol\n"
                         "E\tchat\tprivate\tDave\t\thi\tthere\n"
                         "E\tjoin\tonlyone\n"
                         "E\tdance\ta\tb\n"
                         "Z\tfuture line type\n",
                         snap, ev));
    CHECK(ev.size() == 6);
    CHECK(ev[0].kind == Event::Join && ev[0].f[0] == "Lobby" && ev[0].f[1] == "Alice");
    CHECK(ev[1].kind == Event::Leave && ev[1].f[0].empty() && ev[1].f[1] == "Bob");
    CHECK(ev[2].kind == Event::Switch && ev[2].f[0] == "3" && ev[2].f[1] == "Old" && ev[2].f[2] == "New");
    CHECK(ev[3].kind == Event::Conn && ev[3].f[1] == "connected");
    CHECK(ev[4].kind == Event::Whisper && ev[4].f[0] == "Elsewhere" && ev[4].f[1] == "Carol");
    CHECK(ev[5].kind == Event::Chat && ev[5].f[0] == "private" && ev[5].f[1] == "Dave" && ev[5].f[2].empty() &&
          ev[5].f[3] == "hi\tthere");
    ev.clear();

    // malformed lines skipped, good ones kept, missing trailing newline fine
    CHECK(parse_datagram("YAP2\nU\tx\t0\tu\t\tBad\nU\t3\n\nU\t4\t0\tu4\t\tOk", snap, ev));
    CHECK(snap.users.size() == 1 && snap.users[0].clid == 4 && snap.users[0].nickname == "Ok");

    // user cap
    std::string big = "YAP2\n";
    for (int i = 0; i < proto::kMaxUsers + 10; ++i) big += "U\t" + std::to_string(i + 1) + "\t0\tu\t\tN\n";
    CHECK(parse_datagram(big, snap, ev));
    CHECK(snap.users.size() == static_cast<size_t>(proto::kMaxUsers));

    // --- config: colours / enums -------------------------------------------------
    using yap::config::format_color;
    using yap::config::parse_color;
    CHECK(parse_color("#FF8000") == yap::rgba(255, 128, 0));
    CHECK(parse_color("#FF800080") == yap::rgba(255, 128, 0, 128));
    CHECK(parse_color("FF8000") == 0 && parse_color("#12") == 0 && parse_color("#GGGGGG") == 0);
    CHECK(format_color(yap::rgba(1, 2, 3, 4)) == "#01020304");
    CHECK(parse_color(format_color(yap::rgba(200, 100, 50, 25))) == yap::rgba(200, 100, 50, 25));
    CHECK(yap::config::parse_anchor("bottom_right") == yap::Anchor::BottomRight);
    CHECK(yap::config::parse_anchor("nonsense") == yap::Anchor::TopLeft);
    CHECK(yap::config::parse_icon(yap::config::to_string(yap::IconShape::SpeakerMuted)) == yap::IconShape::SpeakerMuted);

    // --- notify: templates ----------------------------------------------------------
    using yap::notify::format_template;
    CHECK(format_template("{name} joined from {from}", {{"name", "Bob"}, {"from", "Lobby"}}) == "Bob joined from Lobby");
    CHECK(format_template("{unknown} {name", {{"name", "x"}}) == "{unknown} {name");
    CHECK(format_template("plain", {}) == "plain");

    yap::Config cfg;
    yap::Notif cat;
    std::string text;
    CHECK(yap::notify::toast_for({Event::Join, {"", "Bob"}}, cfg, cat, text));
    CHECK(cat == yap::NotifJoin && text == "Bob joined from the server");
    CHECK(yap::notify::toast_for({Event::Conn, {"Srv", "connection_lost"}}, cfg, cat, text));
    CHECK(text == "TeamSpeak: connection lost");
    CHECK(!yap::notify::toast_for({Event::Chat, {"private", "Eve", "", "hi"}}, cfg, cat, text));  // off by default
    cfg.notif[yap::NotifPrivateChat].enabled = true;
    CHECK(yap::notify::toast_for({Event::Chat, {"private", "Eve", "", "hi"}}, cfg, cat, text));
    CHECK(cat == yap::NotifPrivateChat && text == "Eve: hi");

    // --- notify: queue lifecycle ----------------------------------------------------
    {
        yap::Config q;
        q.notif_in_ms = 100;
        q.notif_out_ms = 100;
        q.notif[yap::NotifJoin].hold_ms = 1000;
        q.notif_max_visible = 2;
        yap::notify::Queue queue;
        queue.push(yap::NotifJoin, "a", q);
        CHECK(queue.items().size() == 1);
        CHECK(yap::notify::Queue::alpha(queue.items()[0], q) == 0.f);
        queue.tick(50.f, q);
        CHECK(yap::notify::Queue::alpha(queue.items()[0], q) == 0.5f);
        queue.tick(100.f, q);
        CHECK(yap::notify::Queue::alpha(queue.items()[0], q) == 1.f);
        queue.push(yap::NotifJoin, "a", q);  // merged
        CHECK(queue.items().size() == 1 && queue.items()[0].count == 2);
        queue.push(yap::NotifJoin, "b", q);
        queue.push(yap::NotifJoin, "c", q);  // cap 2 evicts "a"
        CHECK(queue.items().size() == 2 && queue.items()[0].text == "b");
        queue.tick(1150.f, q);  // b: 1150 in fade-out, alpha 0.5
        CHECK(yap::notify::Queue::alpha(queue.items()[0], q) == 0.5f);
        queue.tick(100.f, q);  // b and c were pushed together, both expire
        CHECK(queue.items().empty());
        queue.mark_connected();
        queue.push(yap::NotifJoin, "flood", q);  // suppressed right after connect
        CHECK(queue.items().empty());
        queue.push(yap::NotifWhisper, "w", q);  // other categories still pass
        CHECK(queue.items().size() == 1);
        queue.tick(3000.f, q);
        queue.push(yap::NotifJoin, "later", q);
        CHECK(queue.items().size() == 1 && queue.items()[0].text == "later");
    }

    // --- roster: visibility, ordering, styling -----------------------------------------
    {
        Snapshot s;
        s.alive = true;
        s.conn = 2;
        s.users = {{1, 0, "u1", "", "zed"},
                   {2, proto::Talking, "u2", "", "amy"},
                   {3, proto::Self, "u3", "", "me"},
                   {4, proto::InputMuted, "u4", "", "bob"}};
        yap::Config r;
        auto v = yap::roster::visible(s, r);
        CHECK(v.size() == 4 && v[0]->uid == "u3" && v[1]->uid == "u1");  // self first, then channel order
        r.sort = yap::Sort::Alphabetical;
        v = yap::roster::visible(s, r);
        CHECK(v[0]->uid == "u3" && v[1]->uid == "u2" && v[2]->uid == "u4" && v[3]->uid == "u1");
        r.sort = yap::Sort::SpeakingFirst;
        v = yap::roster::visible(s, r);
        CHECK(v[0]->uid == "u3" && v[1]->uid == "u2");
        r.only_show_talking = true;
        v = yap::roster::visible(s, r);
        CHECK(v.size() == 2 && v[0]->uid == "u3" && v[1]->uid == "u2");
        r.only_show_talking = false;
        r.sort = yap::Sort::ChannelOrder;
        r.show_muted_users = false;
        r.show_local_user = false;
        v = yap::roster::visible(s, r);
        CHECK(v.size() == 2 && v[0]->uid == "u1" && v[1]->uid == "u2");

        yap::Config c;
        auto res = yap::roster::resolve({5, proto::Away | proto::InputMuted | proto::Commander, "u5", "", "n"}, c, 0.f);
        CHECK(res.name_color == c.ind[yap::IndMicMuted].text_color);  // mic muted outranks away
        CHECK(res.opacity == 1.f - c.ind[yap::IndMicMuted].dim);
        CHECK(res.leading.size() == 1 && res.leading[0].shape == yap::IconShape::Circle);
        CHECK(res.trailing.size() == 2 && res.trailing[0].shape == yap::IconShape::MicrophoneMuted &&
              res.trailing[1].shape == yap::IconShape::Moon);
        res = yap::roster::resolve({6, proto::Talking, "u6", "", "n"}, c, 1.f);
        CHECK(res.name_color == c.ind[yap::IndSpeaking].text_color);
        res = yap::roster::resolve({6, proto::Talking, "u6", "", "n"}, c, 0.5f);
        CHECK(res.name_color == yap::roster::lerp(c.text_color, c.ind[yap::IndSpeaking].text_color, 0.5f));
        c.users["u7"] = yap::UserOverride{yap::rgba(1, 2, 3), 0, yap::IconShape::Star, yap::rgba(9, 9, 9), "Ren", 1, "tag"};
        res = yap::roster::resolve({7, 0, "u7", "", "orig"}, c, 0.f);
        CHECK(res.name == "Ren" && res.name_color == yap::rgba(1, 2, 3) && res.tag == "tag");
        CHECK(res.leading.size() == 1 && res.leading[0].shape == yap::IconShape::Star);
        res = yap::roster::resolve({8, proto::Friend, "u8", "Pal", "orig"}, c, 0.f);
        CHECK(res.tag == "Pal" && res.name_color == c.friend_color);

        yap::roster::Envelope env;
        yap::Config e;
        e.speaking_attack_ms = 100;
        e.speaking_release_ms = 200;
        std::vector<yap::ts::User> us = {{1, proto::Talking, "a", "", "a"}};
        env.tick(us, 50.f, e);
        CHECK(env.level("a") == 0.5f && env.any_active());
        env.tick(us, 100.f, e);
        CHECK(env.level("a") == 1.f);
        us[0].flags = 0;
        env.tick(us, 100.f, e);
        CHECK(env.level("a") == 0.5f);
        env.tick({}, 1.f, e);
        CHECK(env.level("a") == 0.f && !env.any_active());
    }

    // --- updater: version compare ---------------------------------------------
    using yap::update::is_newer;
    CHECK(is_newer("v0.2.0", "0.1.0"));
    CHECK(is_newer("v1.0.0", "0.9.9"));
    CHECK(is_newer("0.1.1", "0.1.0"));
    CHECK(!is_newer("v0.1.0", "0.1.0"));
    CHECK(!is_newer("v0.0.9", "0.1.0"));
    CHECK(!is_newer("garbage", "0.1.0"));
    CHECK(is_newer("v0.2", "0.1.0"));  // short tags pad with zeros

    // --- updater: release JSON (shape as GitHub emits it, compact) -------------
    using yap::update::Release;
    using yap::update::parse_release;
    Release rel;
    const char* json =
        "{\"url\":\"x\",\"tag_name\":\"v0.2.0\",\"assets\":["
        "{\"name\":\"YapNotifier.ts3_plugin\",\"digest\":\"sha256:"
        "1111111111111111111111111111111111111111111111111111111111111111\","
        "\"browser_download_url\":\"https://github.com/o/r/releases/download/v0.2.0/YapNotifier.ts3_plugin\"},"
        "{\"name\":\"YapNotifier.asi\",\"size\":1,\"digest\":\"sha256:"
        "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd\","
        "\"browser_download_url\":\"https://github.com/o/r/releases/download/v0.2.0/YapNotifier.asi\"}"
        "],\"body\":\"notes\"}";
    CHECK(parse_release(json, rel));
    CHECK(rel.tag == "v0.2.0");
    CHECK(rel.asi_url == "https://github.com/o/r/releases/download/v0.2.0/YapNotifier.asi");
    CHECK(rel.asi_sha256 == "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd");

    // asset without digest -> no hash (caller refuses to install)
    CHECK(parse_release("{\"tag_name\":\"v0.3.0\",\"assets\":[{\"name\":\"YapNotifier.asi\","
                        "\"browser_download_url\":\"https://x/YapNotifier.asi\"}]}", rel));
    CHECK(rel.tag == "v0.3.0" && rel.asi_url == "https://x/YapNotifier.asi" && rel.asi_sha256.empty());

    // no .asi asset at all -> still a valid release, nothing to download
    CHECK(parse_release("{\"tag_name\":\"v0.3.0\",\"assets\":[]}", rel));
    CHECK(rel.asi_url.empty());

    // not a release payload
    CHECK(!parse_release("{\"message\":\"Not Found\"}", rel));

    std::puts("test_parser: ok");
    return 0;
}
