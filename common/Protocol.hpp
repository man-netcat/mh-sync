#ifndef SYNCPROTOCOL_HPP
#define SYNCPROTOCOL_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// MH-Sync protocol - TCP-based save sync between 3DS and Switch
// Port: 42984 (0xA7E8)

namespace SyncProtocol {

constexpr uint16_t SYNC_PORT = 42984;

// Magic number: "SMNK" = SyncMoNKey
constexpr uint32_t MAGIC = 0x534D4E4B;
constexpr uint8_t PROTOCOL_VERSION = 0x01;

// Maximum chunk size for save data transfer
constexpr uint32_t MAX_CHUNK_SIZE = 65536;

// Protocol message types
enum class MessageType : uint8_t {
    HANDSHAKE       = 0x01,  // Client → Server: introduction + save metadata
    HANDSHAKE_ACK   = 0x02,  // Server → Client: response with save metadata
    SAVE_REQUEST    = 0x03,  // Client → Server: request to sync (with direction byte)
    SAVE_DATA       = 0x04,  // Both: chunk of save file data
    SAVE_DATA_ACK   = 0x05,  // Both: acknowledgment of received chunk
    SAVE_COMPLETE   = 0x06,  // Both: all chunks sent successfully
    SAVE_RESULT     = 0x07,  // Receiver → Sender: save was written (0=ok, 1=fail)
    TRANSFER_ABORT  = 0xFF   // Both: abort transfer with reason
};

// Direction for sync operation
// SEND_TO_SWITCH means 3DS pushes its save TO the Switch.
// RECEIVE_FROM_SWITCH means the Switch pushes its save TO the 3DS.
enum class SyncDirection : uint8_t {
    SEND_TO_SWITCH    = 0x00,  // 3DS sends save → Switch receives
    RECEIVE_FROM_SWITCH = 0x01 // Switch sends save → 3DS receives
};

// Platform identifiers
enum class Platform : uint8_t {
    UNKNOWN = 0x00,
    N3DS    = 0x03,   // Nintendo 3DS
    NX      = 0x04    // Nintendo Switch
};

// Game version identifiers
enum class GameVersion : uint8_t {
    UNKNOWN    = 0x00,
    MHXX_3DS   = 0x01,  // Monster Hunter XX on 3DS
    MHGU_SW    = 0x02,  // Monster Hunter Generations Ultimate on Switch
    MHXX_SW    = 0x03   // Monster Hunter XX on Switch
};

// Get GameVersion from platform + file size
inline GameVersion detectGameVersion(Platform platform, uint32_t fileSize)
{
    if (platform == Platform::N3DS && fileSize == 4726152) {
        return GameVersion::MHXX_3DS;
    }
    if (platform == Platform::NX) {
        if (fileSize == 5159100) return GameVersion::MHGU_SW;
        if (fileSize == 4726188) return GameVersion::MHXX_SW;
    }
    return GameVersion::UNKNOWN;
}

// Save metadata exchanged during handshake
struct SaveMetadata {
    Platform    platform;
    GameVersion gameVersion;
    uint64_t    timestamp;       // Unix epoch seconds of last save
    uint8_t     slotBitmask;     // Bitmask of used character slots (bit 0=slot1, etc)
    uint32_t    saveFileSize;    // Size of the system file in bytes
    uint64_t    hashPrefix;      // FNV-1a 64-bit of first 65536 bytes of the system file

    void toBytes(uint8_t* buf) const;
    static SaveMetadata fromBytes(const uint8_t* buf);
};

// Full protocol message header (8 bytes)
#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic;             // MAGIC
    uint8_t  version;           // PROTOCOL_VERSION
    uint8_t  type;              // MessageType
    uint32_t payloadLength;     // Length of payload following header
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 10, "MessageHeader must be 10 bytes");

// SAVE_DATA chunk header (8 bytes)
#pragma pack(push, 1)
struct DataChunkHeader {
    uint32_t offset;
    uint32_t size;
};
#pragma pack(pop)

// Build a complete message (header + payload)
std::vector<uint8_t> buildMessage(MessageType type, const uint8_t* payload, uint32_t payloadLength);

// Build handshake message
std::vector<uint8_t> buildHandshake(const SaveMetadata& metadata);

// Build handshake acknowledgment (includes Switch's preferred direction)
std::vector<uint8_t> buildHandshakeAck(const SaveMetadata& metadata, SyncDirection preferredDirection);

// Build save request with user-chosen direction
std::vector<uint8_t> buildSaveRequest(SyncDirection direction);

// Build save data chunk
std::vector<uint8_t> buildSaveDataChunk(uint32_t offset, const uint8_t* data, uint32_t size);

// Build save data acknowledgment
std::vector<uint8_t> buildSaveDataAck(uint32_t offset, uint32_t size);

// Build save complete notification
std::vector<uint8_t> buildSaveComplete();

// Build save result (0=success, 1=failure)
std::vector<uint8_t> buildSaveResult(bool success);

// Build transfer abort with reason string
std::vector<uint8_t> buildTransferAbort(const std::string& reason);

// Parse raw received data into a message
// Returns: type, payload vector, or throws on error
struct ParsedMessage {
    MessageType type;
    std::vector<uint8_t> payload;
};
ParsedMessage parseMessage(const uint8_t* data, size_t length);

// Handshake serialization
inline void SaveMetadata::toBytes(uint8_t* buf) const
{
    buf[0] = static_cast<uint8_t>(platform);
    buf[1] = static_cast<uint8_t>(gameVersion);
    // timestamp (8 bytes, big-endian)
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = (timestamp >> (56 - i * 8)) & 0xFF;
    }
    buf[10] = slotBitmask;
    // saveFileSize (4 bytes, big-endian)
    for (int i = 0; i < 4; i++) {
        buf[11 + i] = (saveFileSize >> (24 - i * 8)) & 0xFF;
    }
    // hashPrefix (8 bytes, big-endian)
    for (int i = 0; i < 8; i++) {
        buf[15 + i] = (hashPrefix >> (56 - i * 8)) & 0xFF;
    }
}

inline SaveMetadata SaveMetadata::fromBytes(const uint8_t* buf)
{
    SaveMetadata md;
    md.platform     = static_cast<Platform>(buf[0]);
    md.gameVersion  = static_cast<GameVersion>(buf[1]);
    md.timestamp    = 0;
    for (int i = 0; i < 8; i++) {
        md.timestamp = (md.timestamp << 8) | buf[2 + i];
    }
    md.slotBitmask  = buf[10];
    md.saveFileSize = 0;
    for (int i = 0; i < 4; i++) {
        md.saveFileSize = (md.saveFileSize << 8) | buf[11 + i];
    }
    md.hashPrefix   = 0;
    for (int i = 0; i < 8; i++) {
        md.hashPrefix = (md.hashPrefix << 8) | buf[15 + i];
    }
    return md;
}

// Handshake payload size: 23 bytes metadata + 1 byte direction
constexpr size_t HANDSHAKE_METADATA_SIZE = 23;
constexpr size_t HANDSHAKE_PAYLOAD_SIZE  = 24;

// A simple non-cryptographic hash for save file identification
// Uses FNV-1a 64-bit over the first 65536 bytes (covers header + metadata only,
// NOT character slot data which starts at ~1.2MB)
inline uint64_t computeHashPrefix(const uint8_t* data, size_t size)
{
    const uint64_t FNV_OFFSET = 14695981039346656037ULL;
    const uint64_t FNV_PRIME = 1099511628211ULL;
    uint64_t hash = FNV_OFFSET;
    for (size_t i = 0; i < size && i < 65536; i++) {
        hash ^= data[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

} // namespace SyncProtocol

#endif // SYNCPROTOCOL_HPP
