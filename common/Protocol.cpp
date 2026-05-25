#include "Protocol.hpp"
#include <cstring>
#include <stdexcept>

namespace SyncProtocol {

std::vector<uint8_t> buildMessage(MessageType type, const uint8_t* payload, uint32_t payloadLength)
{
    std::vector<uint8_t> msg(sizeof(MessageHeader) + payloadLength);

    MessageHeader hdr;
    hdr.magic         = MAGIC;
    hdr.version       = PROTOCOL_VERSION;
    hdr.type          = static_cast<uint8_t>(type);
    hdr.payloadLength = payloadLength;

    std::memcpy(msg.data(), &hdr, sizeof(MessageHeader));
    if (payload && payloadLength > 0) {
        std::memcpy(msg.data() + sizeof(MessageHeader), payload, payloadLength);
    }

    return msg;
}

std::vector<uint8_t> buildHandshake(const SaveMetadata& metadata)
{
    uint8_t payload[HANDSHAKE_METADATA_SIZE];
    metadata.toBytes(payload);
    return buildMessage(MessageType::HANDSHAKE, payload, HANDSHAKE_METADATA_SIZE);
}

std::vector<uint8_t> buildHandshakeAck(const SaveMetadata& metadata, SyncDirection preferredDirection)
{
    uint8_t payload[HANDSHAKE_PAYLOAD_SIZE];
    metadata.toBytes(payload);
    payload[HANDSHAKE_METADATA_SIZE] = static_cast<uint8_t>(preferredDirection);
    return buildMessage(MessageType::HANDSHAKE_ACK, payload, HANDSHAKE_PAYLOAD_SIZE);
}

std::vector<uint8_t> buildSaveRequest(SyncDirection direction)
{
    uint8_t payload = static_cast<uint8_t>(direction);
    return buildMessage(MessageType::SAVE_REQUEST, &payload, 1);
}

std::vector<uint8_t> buildSaveDataChunk(uint32_t offset, const uint8_t* data, uint32_t size)
{
    DataChunkHeader chunkHdr;
    chunkHdr.offset = offset;
    chunkHdr.size   = size;

    std::vector<uint8_t> payload(sizeof(DataChunkHeader) + size);
    std::memcpy(payload.data(), &chunkHdr, sizeof(DataChunkHeader));
    std::memcpy(payload.data() + sizeof(DataChunkHeader), data, size);

    return buildMessage(MessageType::SAVE_DATA, payload.data(), static_cast<uint32_t>(payload.size()));
}

std::vector<uint8_t> buildSaveDataAck(uint32_t offset, uint32_t size)
{
    DataChunkHeader ack;
    ack.offset = offset;
    ack.size   = size;

    return buildMessage(MessageType::SAVE_DATA_ACK, reinterpret_cast<uint8_t*>(&ack), sizeof(DataChunkHeader));
}

std::vector<uint8_t> buildSaveComplete()
{
    return buildMessage(MessageType::SAVE_COMPLETE, nullptr, 0);
}

std::vector<uint8_t> buildSaveResult(bool success)
{
    uint8_t payload = success ? 0 : 1;
    return buildMessage(MessageType::SAVE_RESULT, &payload, 1);
}

std::vector<uint8_t> buildTransferAbort(const std::string& reason)
{
    return buildMessage(MessageType::TRANSFER_ABORT,
        reinterpret_cast<const uint8_t*>(reason.c_str()),
        static_cast<uint32_t>(reason.size()));
}

ParsedMessage parseMessage(const uint8_t* data, size_t length)
{
    if (length < sizeof(MessageHeader)) {
        throw std::runtime_error("Message too short");
    }

    MessageHeader hdr;
    std::memcpy(&hdr, data, sizeof(MessageHeader));

    if (hdr.magic != MAGIC) {
        throw std::runtime_error("Invalid magic number");
    }
    if (hdr.version != PROTOCOL_VERSION) {
        throw std::runtime_error("Protocol version mismatch");
    }
    if (sizeof(MessageHeader) + hdr.payloadLength > length) {
        throw std::runtime_error("Truncated message payload");
    }

    ParsedMessage msg;
    msg.type = static_cast<MessageType>(hdr.type);
    msg.payload.assign(data + sizeof(MessageHeader),
                       data + sizeof(MessageHeader) + hdr.payloadLength);

    return msg;
}

} // namespace SyncProtocol
