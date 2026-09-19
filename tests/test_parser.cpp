// Host-side check for the datagram parser: the one piece of pure logic here.
#include <cstdio>
#include <vector>

#include "teamspeak.h"
#include "update.h"

#define CHECK(x)                                                        \
    do {                                                                \
        if (!(x)) {                                                     \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);     \
            return 1;                                                   \
        }                                                               \
    } while (0)

using yap::ts::Client;
using yap::ts::parse_datagram;

int main() {
    std::vector<Client> out{{99, "stale"}};

    // wrong magic: rejected, output untouched
    CHECK(!parse_datagram("NOPE\n1\tBob\n", out));
    CHECK(out.size() == 1 && out[0].clid == 99);

    // heartbeat with nobody talking
    CHECK(parse_datagram("YAP1\n", out));
    CHECK(out.empty());

    // normal payload, UTF-8 and spaces in names
    CHECK(parse_datagram("YAP1\n5\tBob\n12\tJos\xC3\xA9 the Second\n", out));
    CHECK(out.size() == 2);
    CHECK(out[0].clid == 5 && out[0].nickname == "Bob");
    CHECK(out[1].clid == 12 && out[1].nickname == "Jos\xC3\xA9 the Second");

    // missing trailing newline still parses the last line
    CHECK(parse_datagram("YAP1\n7\tEve", out));
    CHECK(out.size() == 1 && out[0].clid == 7 && out[0].nickname == "Eve");

    // malformed lines are skipped, good ones kept
    CHECK(parse_datagram("YAP1\nnotanumber\tX\n\n3\n4\tOk\n", out));
    CHECK(out.size() == 1 && out[0].clid == 4 && out[0].nickname == "Ok");

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
