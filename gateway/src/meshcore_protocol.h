#ifndef MESHCORE_PROTOCOL_H
#define MESHCORE_PROTOCOL_H

// MeshCore Companion Radio protocol codec (USB serial). Pure C++/STL (no Qt) so it can be unit-tested
// standalone; MeshCoreRadio wraps it around QSerialPort. Byte layouts per the MeshCore Companion
// protocol (docs.meshcore.io/companion_protocol + the Companion-Radio-Protocol wiki).
//
// USB framing: each frame is wrapped as  <dir-byte> <uint16 LE length> <inner frame>.
//   host -> radio : dir byte '<' (0x3C)
//   radio -> host : dir byte '>' (0x3E)
// The first byte of an inner frame is the command code (host->radio) or response/push code
// (radio->host). Multi-byte integers are little-endian; strings are UTF-8.

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace meshcore {

using Bytes = std::vector<uint8_t>;

// Command codes (host -> radio)
enum Cmd : uint8_t {
    CMD_APP_START        = 0x01,
    CMD_SEND_CHANNEL_MSG = 0x03,
    CMD_GET_CONTACTS     = 0x04,
    CMD_SET_DEVICE_TIME  = 0x06,
    CMD_SEND_SELF_ADVERT = 0x07,
    CMD_SET_ADVERT_NAME  = 0x08,
    CMD_GET_MESSAGE      = 0x0A,
    CMD_SET_RADIO_PARAMS = 0x0B,
    CMD_SET_RADIO_TX_POWER = 0x0C,
    CMD_DEVICE_QUERY     = 0x16,
    CMD_GET_CHANNEL_INFO = 0x1F,
    CMD_SET_CHANNEL      = 0x20,
};

// Response / push codes (radio -> host)
enum Resp : uint8_t {
    RESP_OK             = 0x00,
    RESP_ERROR          = 0x01,
    RESP_CONTACTS_START = 0x02,   // begins a GET_CONTACTS reply
    RESP_CONTACT        = 0x03,   // one contact entry (disambiguated from CMD_SEND_CHANNEL_MSG by direction)
    RESP_END_OF_CONTACTS= 0x04,   // ends a GET_CONTACTS reply
    RESP_SELF_INFO      = 0x05,
    RESP_MSG_SENT       = 0x06,
    RESP_CONTACT_MSG    = 0x07,
    RESP_CHANNEL_MSG    = 0x08,
    RESP_NO_MORE_MSGS   = 0x0A,   // same value as CMD_GET_MESSAGE; disambiguated by direction
    RESP_DEVICE_INFO    = 0x0D,
    RESP_CONTACT_MSG_V3 = 0x10,
    RESP_CHANNEL_MSG_V3 = 0x11,
    RESP_CHANNEL_INFO   = 0x12,
    PUSH_ADVERT         = 0x80,   // a new advert packet was received (32-byte pubkey follows)
    PUSH_MSGS_WAITING   = 0x83,
    PUSH_NEW_ADVERT     = 0x8A,   // new contact advert (manual_add_contacts mode)
};

constexpr uint8_t DIR_TX = 0x3C;   // '<'  host -> radio
constexpr uint8_t DIR_RX = 0x3E;   // '>'  radio -> host
constexpr int     MAX_CHANNEL_TEXT = 133;

// ---- USB framing ----------------------------------------------------------
// Wrap an inner command frame for USB transmission: '<' + uint16 LE len + frame.
Bytes wrapTx(const Bytes& frame);

// Incremental de-framer for the RX stream. Feed raw serial bytes; pop complete inner frames.
class FrameReader {
public:
    void feed(const uint8_t* data, size_t n);
    // Returns the next complete inner frame (command/response code + payload), or nullopt if none yet.
    std::optional<Bytes> next();
private:
    Bytes buf_;
};

// ---- command builders (inner frame; wrap with wrapTx() before sending) -----
Bytes cmdAppStart(const std::string& appName = "");
Bytes cmdDeviceQuery();
Bytes cmdGetChannelInfo(uint8_t index);
Bytes cmdGetMessage();
Bytes cmdSendChannelMessage(uint8_t index, uint32_t unixTime, const std::string& text);
Bytes cmdSetChannel(uint8_t index, const std::string& name, const Bytes& secret16);
Bytes cmdGetContacts(uint32_t since = 0);          // since = last lastmod received (0 = full list)
Bytes cmdSendSelfAdvert(bool flood = true);        // broadcast our advert so neighbours add us as a contact
Bytes cmdSetAdvertName(const std::string& name);   // set our adv_name (rides along in future adverts)
Bytes cmdSetDeviceTime(uint32_t epochSecs);        // set the radio RTC (so our adverts carry a fresh timestamp)
Bytes cmdSetRadioParams(uint32_t freqKhz, uint32_t bwHz, uint8_t sf, uint8_t cr);  // freqKhz=MHz*1000, bwHz=kHz*1000
Bytes cmdSetRadioTxPower(uint8_t dbm);

// ---- parsed responses -----------------------------------------------------
struct SelfInfo {
    Bytes        publicKey;     // 32 bytes
    double       lat = 0, lon = 0;
    uint32_t     freqKhz = 0, bwKhz = 0;
    uint8_t      sf = 0, cr = 0;
    std::string  name;
};

struct ChannelInfo {
    uint8_t      index = 0;
    std::string  name;          // trimmed of null padding
    Bytes        secret;        // 16 bytes (all-zero => public/unused)
    bool         isEmpty = true;   // no name and zero secret
};

struct Contact {
    Bytes        publicKey;     // 32 bytes
    uint8_t      type = 0;      // ADV_TYPE_*
    uint8_t      flags = 0;
    std::string  name;          // adv_name
    uint32_t     lastAdvert = 0;
    double       lat = 0, lon = 0;
};

struct TextMessage {
    bool         isChannel = true;   // true: channel message; false: direct/contact message
    uint8_t      channelIndex = 0;   // valid when isChannel
    Bytes        pubKeyPrefix;       // 6 bytes, valid when !isChannel
    int8_t       snr = 0;            // raw (divide by 4 for dB); 0 when not provided
    uint32_t     timestamp = 0;
    std::string  text;
};

// First byte (frame type), or 0xFF if the frame is empty.
uint8_t frameType(const Bytes& f);

std::optional<SelfInfo>    parseSelfInfo(const Bytes& f);     // 0x05
std::optional<ChannelInfo> parseChannelInfo(const Bytes& f);  // 0x12
std::optional<Contact>     parseContact(const Bytes& f);      // 0x03 (148-byte entry)
// Handles 0x07 / 0x08 / 0x10 / 0x11 (contact + channel, with/without SNR).
std::optional<TextMessage> parseTextMessage(const Bytes& f);

} // namespace meshcore

#endif // MESHCORE_PROTOCOL_H
