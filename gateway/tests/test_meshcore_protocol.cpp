// Standalone unit test for the MeshCore companion-protocol codec. No Qt, no gateway build:
//   g++ -std=c++17 -I../src ../src/meshcore_protocol.cpp test_meshcore_protocol.cpp -o /tmp/t && /tmp/t
// Verifies command byte layouts, USB framing (incl. partial/multi-frame streams), and response parsing
// against the layouts in meshcore_protocol.h.

#include "meshcore_protocol.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace meshcore;

static int g_fail = 0, g_pass = 0;

static std::string hex(const Bytes& b) {
    std::string s; char t[4];
    for (uint8_t x : b) { snprintf(t, sizeof t, "%02x ", x); s += t; }
    if (!s.empty()) s.pop_back();
    return s;
}
static void check(bool ok, const char* name) {
    if (ok) { ++g_pass; printf("  PASS  %s\n", name); }
    else    { ++g_fail; printf("  FAIL  %s\n", name); }
}
static void eqBytes(const Bytes& got, const Bytes& want, const char* name) {
    bool ok = (got == want);
    if (!ok) printf("        got=[%s] want=[%s]\n", hex(got).c_str(), hex(want).c_str());
    check(ok, name);
}

int main() {
    printf("MeshCore protocol codec tests\n");

    // ---- command builders: exact bytes ----
    eqBytes(cmdAppStart("test"),
            Bytes{0x01, 0,0,0,0,0,0,0, 't','e','s','t'}, "cmdAppStart('test')");
    eqBytes(cmdAppStart(), Bytes{0x01, 0,0,0,0,0,0,0}, "cmdAppStart() empty");
    eqBytes(cmdDeviceQuery(), Bytes{0x16, 0x03}, "cmdDeviceQuery");
    eqBytes(cmdGetChannelInfo(2), Bytes{0x1f, 0x02}, "cmdGetChannelInfo(2)");
    eqBytes(cmdGetMessage(), Bytes{0x0a}, "cmdGetMessage");
    // 0x03,0x00,idx,ts(LE 0x12345678 -> 78 56 34 12),"hi"
    eqBytes(cmdSendChannelMessage(1, 0x12345678u, "hi"),
            Bytes{0x03,0x00,0x01, 0x78,0x56,0x34,0x12, 'h','i'}, "cmdSendChannelMessage");
    {
        Bytes sc = cmdSetChannel(3, "ch", Bytes{1,2,3});  // name+secret padded
        bool ok = sc.size() == 50 && sc[0] == 0x20 && sc[1] == 3 &&
                  sc[2] == 'c' && sc[3] == 'h' && sc[4] == 0 &&
                  sc[34] == 1 && sc[35] == 2 && sc[36] == 3 && sc[37] == 0 && sc[49] == 0;
        check(ok, "cmdSetChannel layout (50 bytes, padded)");
    }
    // text truncation at 133 chars
    check(int(cmdSendChannelMessage(0,0,std::string(200,'x')).size()) == 7 + 133,
          "cmdSendChannelMessage truncates to 133");

    // ---- USB framing (TX wrap) ----
    eqBytes(wrapTx(Bytes{0x0a}), Bytes{0x3c, 0x01,0x00, 0x0a}, "wrapTx single byte");
    eqBytes(wrapTx(cmdDeviceQuery()), Bytes{0x3c, 0x02,0x00, 0x16,0x03}, "wrapTx device query");

    // ---- FrameReader: two frames + a split, with leading noise ----
    {
        FrameReader r;
        // noise 'X', then '>' + len2 + {0x83} (msgs waiting), then start of next frame split across feeds
        Bytes part1{'X', 0x3e, 0x01,0x00, 0x83, 0x3e, 0x04,0x00, 0x08};  // frame2 len=4, body split
        Bytes part2{0x02, 0x00, 'a'};  // completes a 4-byte frame {0x08,0x02,0x00,'a'}
        r.feed(part1.data(), part1.size());
        auto f1 = r.next();
        bool okWaiting = f1 && f1->size() == 1 && (*f1)[0] == 0x83;
        check(okWaiting, "FrameReader frame1 = MSGS_WAITING (drops leading noise)");
        auto none = r.next();
        check(!none.has_value(), "FrameReader waits for split frame body");
        r.feed(part2.data(), part2.size());
        auto f2 = r.next();
        bool okSplit = f2 && (*f2 == Bytes{0x08, 0x02, 0x00, 'a'});
        check(okSplit, "FrameReader reassembles split frame2");
    }

    // ---- parseChannelInfo (0x12) ----
    {
        Bytes f; f.push_back(0x12); f.push_back(1);
        std::string nm = "LM Relayed1";
        for (int i = 0; i < 32; ++i) f.push_back(i < int(nm.size()) ? uint8_t(nm[i]) : 0);
        for (int i = 0; i < 16; ++i) f.push_back(uint8_t(0xA0 + i));   // non-zero secret
        auto ci = parseChannelInfo(f);
        check(ci && ci->index == 1 && ci->name == "LM Relayed1" && !ci->isEmpty &&
              ci->secret.size() == 16 && ci->secret[0] == 0xA0 && ci->secret[15] == 0xAF,
              "parseChannelInfo named+secret");
        // empty slot
        Bytes e; e.push_back(0x12); e.push_back(2);
        for (int i = 0; i < 48; ++i) e.push_back(0);
        auto ce = parseChannelInfo(e);
        check(ce && ce->isEmpty, "parseChannelInfo empty slot");
    }

    // ---- parseTextMessage: channel 0x08 ----
    {
        Bytes f{0x08, 0x05 /*idx*/, 0x00 /*pathLen*/, 0x00 /*textType*/};
        // ts = 0x0000003A (LE)
        f.insert(f.end(), {0x3a,0x00,0x00,0x00});
        std::string t = "hello"; f.insert(f.end(), t.begin(), t.end());
        auto m = parseTextMessage(f);
        check(m && m->isChannel && m->channelIndex == 5 && m->timestamp == 0x3a && m->text == "hello",
              "parseTextMessage channel(0x08)");
    }
    // ---- channel V3 0x11 with SNR ----
    {
        Bytes f{0x11, uint8_t(int8_t(-8)) /*snr*/, 0x00,0x00 /*rsv*/, 0x02 /*idx*/, 0x00 /*pathLen*/, 0x00 /*textType*/};
        f.insert(f.end(), {0x01,0x00,0x00,0x00});  // ts=1
        std::string t = "hi"; f.insert(f.end(), t.begin(), t.end());
        auto m = parseTextMessage(f);
        check(m && m->isChannel && m->channelIndex == 2 && m->snr == -8 && m->text == "hi",
              "parseTextMessage channelV3(0x11) snr");
    }
    // ---- contact 0x07 (textType 0, no signature) ----
    {
        Bytes f{0x07, 0xAA,0xBB,0xCC,0xDD,0xEE,0xFF /*keyPrefix*/, 0x00 /*pathLen*/, 0x00 /*textType*/};
        f.insert(f.end(), {0x09,0x00,0x00,0x00});  // ts=9
        std::string t = "dm"; f.insert(f.end(), t.begin(), t.end());
        auto m = parseTextMessage(f);
        check(m && !m->isChannel && m->pubKeyPrefix.size() == 6 && m->pubKeyPrefix[0] == 0xAA &&
              m->pubKeyPrefix[5] == 0xFF && m->timestamp == 9 && m->text == "dm",
              "parseTextMessage contact(0x07)");
    }
    // ---- contact 0x07 with signature (textType 2 -> skip 4 sig bytes) ----
    {
        Bytes f{0x07, 1,2,3,4,5,6, 0x00 /*pathLen*/, 0x02 /*textType=signed*/};
        f.insert(f.end(), {0x00,0x00,0x00,0x00});  // ts
        f.insert(f.end(), {0xDE,0xAD,0xBE,0xEF});  // 4-byte signature
        std::string t = "signed"; f.insert(f.end(), t.begin(), t.end());
        auto m = parseTextMessage(f);
        check(m && !m->isChannel && m->text == "signed", "parseTextMessage contact signed (skips sig)");
    }

    // ---- parseSelfInfo (0x05) ----
    {
        Bytes f(58, 0);
        f[0] = 0x05;
        for (int i = 0; i < 32; ++i) f[4 + i] = uint8_t(i);     // pubkey
        f[56] = 11;  // sf
        f[57] = 5;   // cr
        std::string nm = "MyNode"; f.insert(f.end(), nm.begin(), nm.end());
        auto s = parseSelfInfo(f);
        check(s && s->publicKey.size() == 32 && s->publicKey[31] == 31 &&
              s->sf == 11 && s->cr == 5 && s->name == "MyNode",
              "parseSelfInfo pubkey+sf+cr+name");
    }

    // ---- round-trip: wrap a built command, read it back through FrameReader as if echoed ----
    {
        Bytes inner = cmdGetChannelInfo(4);
        Bytes wire = wrapTx(inner);
        wire[0] = DIR_RX;                 // pretend it came back from the radio
        FrameReader r; r.feed(wire.data(), wire.size());
        auto f = r.next();
        check(f && *f == inner, "round-trip wrap -> FrameReader");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
