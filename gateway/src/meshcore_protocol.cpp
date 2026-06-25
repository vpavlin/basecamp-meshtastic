#include "meshcore_protocol.h"
#include <algorithm>

namespace meshcore {

// ---- little-endian helpers ------------------------------------------------
static void putU16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v & 0xff)); b.push_back(uint8_t((v >> 8) & 0xff)); }
static void putU32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t((v >> (8 * i)) & 0xff)); }
static uint16_t getU16(const Bytes& b, size_t o) { return uint16_t(b[o]) | (uint16_t(b[o + 1]) << 8); }
static uint32_t getU32(const Bytes& b, size_t o) {
    uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(b[o + i]) << (8 * i); return v;
}
static int32_t getI32(const Bytes& b, size_t o) { return int32_t(getU32(b, o)); }

static std::string trimNul(const Bytes& b, size_t off, size_t len) {
    std::string s;
    for (size_t i = 0; i < len && off + i < b.size(); ++i) {
        if (b[off + i] == 0) break;
        s.push_back(char(b[off + i]));
    }
    return s;
}

// ---- USB framing ----------------------------------------------------------
Bytes wrapTx(const Bytes& frame) {
    Bytes out;
    out.reserve(frame.size() + 3);
    out.push_back(DIR_TX);
    putU16(out, uint16_t(frame.size()));
    out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

void FrameReader::feed(const uint8_t* data, size_t n) { buf_.insert(buf_.end(), data, data + n); }

std::optional<Bytes> FrameReader::next() {
    // Drop any leading noise before the next RX direction byte ('>').
    if (!buf_.empty() && buf_[0] != DIR_RX) {
        size_t i = 0;
        while (i < buf_.size() && buf_[i] != DIR_RX) ++i;
        buf_.erase(buf_.begin(), buf_.begin() + i);
    }
    if (buf_.size() < 3) return std::nullopt;          // need dir + uint16 length
    const uint16_t len = getU16(buf_, 1);
    if (buf_.size() < size_t(3) + len) return std::nullopt;   // wait for the body
    Bytes frame(buf_.begin() + 3, buf_.begin() + 3 + len);
    buf_.erase(buf_.begin(), buf_.begin() + 3 + len);
    return frame;
}

// ---- command builders -----------------------------------------------------
Bytes cmdAppStart(const std::string& appName) {
    Bytes f;
    f.push_back(CMD_APP_START);
    for (int i = 0; i < 7; ++i) f.push_back(0);                 // bytes 1-7 reserved
    f.insert(f.end(), appName.begin(), appName.end());          // byte 8+ app name (optional)
    return f;
}
Bytes cmdDeviceQuery()                 { return Bytes{CMD_DEVICE_QUERY, 0x03}; }
Bytes cmdGetChannelInfo(uint8_t index) { return Bytes{CMD_GET_CHANNEL_INFO, index}; }
Bytes cmdGetMessage()                  { return Bytes{CMD_GET_MESSAGE}; }

Bytes cmdSendChannelMessage(uint8_t index, uint32_t unixTime, const std::string& text) {
    Bytes f;
    f.push_back(CMD_SEND_CHANNEL_MSG);
    f.push_back(0x00);                  // byte 1: reserved/flags
    f.push_back(index);
    putU32(f, unixTime);
    std::string s = text;
    if (int(s.size()) > MAX_CHANNEL_TEXT) s.resize(MAX_CHANNEL_TEXT);
    f.insert(f.end(), s.begin(), s.end());
    return f;
}

Bytes cmdSetChannel(uint8_t index, const std::string& name, const Bytes& secret16) {
    Bytes f;
    f.push_back(CMD_SET_CHANNEL);
    f.push_back(index);
    for (int i = 0; i < 32; ++i) f.push_back(i < int(name.size()) ? uint8_t(name[i]) : 0);        // name, null-padded
    for (int i = 0; i < 16; ++i) f.push_back(i < int(secret16.size()) ? secret16[i] : 0);          // 16-byte secret
    return f;
}

// ---- parsers --------------------------------------------------------------
uint8_t frameType(const Bytes& f) { return f.empty() ? 0xFF : f[0]; }

std::optional<SelfInfo> parseSelfInfo(const Bytes& f) {
    if (f.size() < 58 || f[0] != RESP_SELF_INFO) return std::nullopt;
    SelfInfo s;
    s.publicKey.assign(f.begin() + 4, f.begin() + 36);
    s.lat = getI32(f, 36) / 1e6;
    s.lon = getI32(f, 40) / 1e6;
    s.freqKhz = uint32_t(getI32(f, 48) / 1000);
    s.bwKhz   = uint32_t(getI32(f, 52) / 1000);
    s.sf = f[56];
    s.cr = f[57];
    s.name = trimNul(f, 58, f.size() - 58);
    return s;
}

std::optional<ChannelInfo> parseChannelInfo(const Bytes& f) {
    if (f.size() < 50 || f[0] != RESP_CHANNEL_INFO) return std::nullopt;
    ChannelInfo c;
    c.index = f[1];
    c.name  = trimNul(f, 2, 32);
    c.secret.assign(f.begin() + 34, f.begin() + 50);
    const bool zeroSecret = std::all_of(c.secret.begin(), c.secret.end(), [](uint8_t x) { return x == 0; });
    c.isEmpty = c.name.empty() && zeroSecret;
    return c;
}

std::optional<TextMessage> parseTextMessage(const Bytes& f) {
    if (f.empty()) return std::nullopt;
    TextMessage m;
    switch (f[0]) {
    case RESP_CHANNEL_MSG:            // 0x08: idx, pathLen, textType, ts(4), text
        if (f.size() < 8) return std::nullopt;
        m.isChannel = true; m.channelIndex = f[1];
        m.timestamp = getU32(f, 4);
        m.text = std::string(f.begin() + 8, f.end());
        return m;
    case RESP_CHANNEL_MSG_V3:         // 0x11: snr, 2 rsv, idx, pathLen, textType, ts(4), text
        if (f.size() < 11) return std::nullopt;
        m.isChannel = true; m.snr = int8_t(f[1]); m.channelIndex = f[4];
        m.timestamp = getU32(f, 7);
        m.text = std::string(f.begin() + 11, f.end());
        return m;
    case RESP_CONTACT_MSG: {          // 0x07: keyPrefix(6), pathLen, textType, ts(4), [sig(4)], text
        if (f.size() < 13) return std::nullopt;
        m.isChannel = false; m.pubKeyPrefix.assign(f.begin() + 1, f.begin() + 7);
        const uint8_t textType = f[8];
        m.timestamp = getU32(f, 9);
        size_t off = 13 + (textType == 2 ? 4u : 0u);
        if (off > f.size()) return std::nullopt;
        m.text = std::string(f.begin() + off, f.end());
        return m;
    }
    case RESP_CONTACT_MSG_V3: {       // 0x10: snr, 2 rsv, keyPrefix(6), pathLen, textType, ts(4), [sig(4)], text
        if (f.size() < 16) return std::nullopt;
        m.isChannel = false; m.snr = int8_t(f[1]); m.pubKeyPrefix.assign(f.begin() + 4, f.begin() + 10);
        const uint8_t textType = f[11];
        m.timestamp = getU32(f, 12);
        size_t off = 16 + (textType == 2 ? 4u : 0u);
        if (off > f.size()) return std::nullopt;
        m.text = std::string(f.begin() + off, f.end());
        return m;
    }
    default:
        return std::nullopt;
    }
}

} // namespace meshcore
