// Host-side check for the datagram parser: the one piece of pure logic here.
#include <cstdio>
#include <vector>

#include "teamspeak.h"

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

    std::puts("test_parser: ok");
    return 0;
}
