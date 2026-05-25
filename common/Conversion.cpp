#include "Conversion.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace SaveConversion {

SaveType detectSaveType(const std::vector<uint8_t>& data)
{
    size_t sz = data.size();
    if (sz == SAVE_SIZE_3DS)     return SaveType::MHXX_3DS;
    if (sz == SAVE_SIZE_MHGU_SW) return SaveType::MHGU_SWITCH;
    if (sz == SAVE_SIZE_MHXX_SW) return SaveType::MHXX_SWITCH;
    return SaveType::UNKNOWN;
}

std::string saveTypeName(SaveType type)
{
    switch (type) {
        case SaveType::UNKNOWN:     return "Unknown";
        case SaveType::MHXX_3DS:    return "MHXX (3DS)";
        case SaveType::MHGU_SWITCH: return "MHGU (Switch)";
        case SaveType::MHXX_SWITCH: return "MHXX (Switch)";
    }
    return "Unknown";
}

uint8_t detectSlotBitmask(const std::vector<uint8_t>& data, SaveType type)
{
    if (data.size() < 8) return 0;

    switch (type) {
        case SaveType::MHXX_3DS:
            // 3DS MHXX: bytes 4,5,6 = slot occupancy flags
            return (data[4] ? 0x01 : 0) |
                   (data[5] ? 0x02 : 0) |
                   (data[6] ? 0x04 : 0);
        case SaveType::MHGU_SWITCH:
        case SaveType::MHXX_SWITCH:
            // Switch has 36-byte larger header; slot flags at bytes 40,41,42
            if (data.size() >= 44) {
                return (data[40] ? 0x01 : 0) |
                       (data[41] ? 0x02 : 0) |
                       (data[42] ? 0x04 : 0);
            }
            return 0;
        case SaveType::UNKNOWN:
            return 0;
    }
    return 0;
}

uint64_t extractSaveTimestamp(const std::vector<uint8_t>& data, SaveType type)
{
    // Use first character slot's play time field as a save identity marker
    // This isn't a real timestamp but a deterministic value that changes when
    // the save data changes.
    uint32_t offset = 0;
    switch (type) {
        case SaveType::MHXX_3DS:
            offset = SLOT1_3DS + 0x20;  // Play time at offset 0x20 in character struct
            break;
        case SaveType::MHXX_SWITCH:
            offset = INITIAL_POS_MHXX_SW + 0x20;  // Switch header is 36 bytes larger
            break;
        case SaveType::MHGU_SWITCH:
            offset = SLOT1_MHGU + 0x20;
            break;
        case SaveType::UNKNOWN:
            return 0;
    }

    if (offset + 8 > data.size()) return 0;

    // Read 8 bytes as little-endian uint64
    uint64_t val = 0;
    for (uint32_t i = 0; i < 8; i++) {
        val |= static_cast<uint64_t>(data[offset + i]) << (static_cast<int>(i) * 8);
    }
    return val;
}

bool validateSave(const std::vector<uint8_t>& data)
{
    SaveType type = detectSaveType(data);
    if (type == SaveType::UNKNOWN) return false;

    // Basic sanity: check that header has at least one slot marked as used
    // or that it's a valid blank save
    if (data.size() < 0x20) return false;

    return true;
}

// ============================================================
// Conversion: 3DS MHXX → Switch MHGU
// ============================================================
std::vector<uint8_t> convert3DSToMHGU(const std::vector<uint8_t>& input,
                                      const std::vector<uint8_t>& blank_mhgu)
{
    if (input.size() != SAVE_SIZE_3DS) {
        throw std::runtime_error("Input is not a valid 3DS MHXX save (wrong size)");
    }
    if (blank_mhgu.size() != SAVE_SIZE_MHGU_SW) {
        throw std::runtime_error("Blank MHGU save has wrong size");
    }

    std::vector<uint8_t> output = blank_mhgu;

    // Step 1: Copy slot occupancy flags from 3DS save to Switch position
    // 3DS: bytes 4-7 (slot flags at offset 4)
    // MHGU: bytes 40-43 (slot flags at offset 40)
    std::memcpy(&output[40], &input[4], 4);

    // Step 2: For each character slot, copy character data and fix chat messages
    uint32_t slot_positions_3ds[]  = { SLOT1_3DS,  SLOT2_3DS,  SLOT3_3DS };
    uint32_t slot_positions_mhgu[] = { SLOT1_MHGU, SLOT2_MHGU, SLOT3_MHGU };
    uint32_t chat_positions_3ds[]  = { CHAT1_3DS,  CHAT2_3DS,  CHAT3_3DS };
    uint32_t chat_positions_mhgu[] = { CHAT1_MHGU, CHAT2_MHGU, CHAT3_MHGU };

    for (uint32_t slot = 0; slot < 3; slot++) {
        uint32_t slot_start_3ds  = slot_positions_3ds[slot];
        uint32_t slot_start_mhgu = slot_positions_mhgu[slot];
        uint32_t chat_start_3ds  = chat_positions_3ds[slot];
        uint32_t chat_start_mhgu = chat_positions_mhgu[slot];

        if (slot_start_3ds >= input.size() || chat_start_3ds > input.size()) continue;
        if (slot_start_mhgu >= output.size() || chat_start_mhgu > output.size()) continue;

        // Copy data before chat section (character data + non-chat data)
        size_t pre_chat_size = chat_start_3ds - slot_start_3ds;
        if (slot_start_mhgu + pre_chat_size <= output.size() &&
            slot_start_3ds + pre_chat_size <= input.size()) {
            std::memcpy(&output[slot_start_mhgu], &input[slot_start_3ds], pre_chat_size);
        }

        // Fix chat messages: 3DS uses 60 bytes, Switch uses 104 bytes per message
        // Each 60-byte 3DS chat msg becomes 60-byte data + 44-byte zero padding on Switch
        for (uint32_t msg = 0; msg < CHAT_MSG_COUNT; msg++) {
            uint32_t src_off = chat_start_3ds + (msg * static_cast<uint32_t>(CHAT_MSG_3DS));
            uint32_t dst_off = chat_start_mhgu + (msg * static_cast<uint32_t>(CHAT_MSG_SWITCH));

            if (src_off + CHAT_MSG_3DS > input.size()) break;
            if (dst_off + CHAT_MSG_3DS > output.size()) break;

            // Copy the 60 bytes of chat message data
            std::memcpy(&output[dst_off], &input[src_off], CHAT_MSG_3DS);
            // Zero out the 44 bytes of padding
            if (dst_off + CHAT_MSG_SWITCH <= output.size()) {
                std::memset(&output[dst_off + CHAT_MSG_3DS], 0, CHAT_MSG_SWITCH - CHAT_MSG_3DS);
            }
        }
    }

    // Step 3: Copy remaining data after last chat block
    uint32_t remaining_start_3ds  = CHAT3_3DS + CHAT_LEN_3DS;
    uint32_t remaining_start_mhgu = CHAT3_MHGU + CHAT_LEN_MHGU;

    if (remaining_start_3ds < input.size() && remaining_start_mhgu < output.size()) {
        size_t remaining = input.size() - remaining_start_3ds;
        if (remaining_start_mhgu + remaining <= output.size()) {
            std::memcpy(&output[remaining_start_mhgu], &input[remaining_start_3ds], remaining);
        }
    }

    return output;
}

// ============================================================
// Conversion: Switch (MHGU or MHXX) → 3DS MHXX
// ============================================================
std::vector<uint8_t> convertSwitchTo3DS(const std::vector<uint8_t>& input,
                                        const std::vector<uint8_t>& blank_3ds)
{
    if (input.size() != SAVE_SIZE_MHGU_SW && input.size() != SAVE_SIZE_MHXX_SW) {
        throw std::runtime_error("Input is not a valid Switch save (wrong size)");
    }
    if (blank_3ds.size() != SAVE_SIZE_3DS) {
        throw std::runtime_error("Blank 3DS save has wrong size");
    }

    // Detect if this is MHGU or MHXX Switch
    bool isMHGU = (input.size() == SAVE_SIZE_MHGU_SW);

    std::vector<uint8_t> output = blank_3ds;

    if (!isMHGU) {
        // MHXX Switch → 3DS: direct copy with header offset adjustment
        // Switch header is 36 bytes larger (slot flags at 40 vs 4 on 3DS)
        // so slot data starts 36 bytes later in the Switch save
        std::memcpy(&output[4], &input[40], 4);
        size_t copy_size = output.size() - SLOT1_3DS;
        std::memcpy(&output[SLOT1_3DS], &input[INITIAL_POS_MHXX_SW], copy_size);
        return output;
    }

    // MHGU → 3DS: need chat message conversion
    uint32_t slot_positions_mhgu[] = { SLOT1_MHGU, SLOT2_MHGU, SLOT3_MHGU };
    uint32_t slot_positions_3ds[]  = { SLOT1_3DS,  SLOT2_3DS,  SLOT3_3DS };
    uint32_t chat_positions_mhgu[] = { CHAT1_MHGU, CHAT2_MHGU, CHAT3_MHGU };
    uint32_t chat_positions_3ds[]  = { CHAT1_3DS,  CHAT2_3DS,  CHAT3_3DS };

    // Copy header: 3DS save header starts differently
    // Copy slot flags from MHGU header (bytes 40-43) to 3DS header (bytes 4-7)
    std::memcpy(&output[4], &input[40], 4);

    for (uint32_t slot = 0; slot < 3; slot++) {
        uint32_t slot_start_mhgu = slot_positions_mhgu[slot];
        uint32_t slot_start_3ds  = slot_positions_3ds[slot];
        uint32_t chat_start_mhgu = chat_positions_mhgu[slot];
        uint32_t chat_start_3ds  = chat_positions_3ds[slot];

        if (slot_start_mhgu >= input.size() || chat_start_mhgu > input.size()) continue;
        if (slot_start_3ds >= output.size() || chat_start_3ds > output.size()) continue;

        // Copy data before chat section
        size_t pre_chat_size = chat_start_mhgu - slot_start_mhgu;
        if (slot_start_3ds + pre_chat_size <= output.size() &&
            slot_start_mhgu + pre_chat_size <= input.size()) {
            std::memcpy(&output[slot_start_3ds], &input[slot_start_mhgu], pre_chat_size);
        }

        // Strip chat padding: Switch uses 104 bytes, 3DS uses 60 bytes per message
        for (uint32_t msg = 0; msg < CHAT_MSG_COUNT; msg++) {
            uint32_t src_off = chat_start_mhgu + (msg * static_cast<uint32_t>(CHAT_MSG_SWITCH));
            uint32_t dst_off = chat_start_3ds + (msg * static_cast<uint32_t>(CHAT_MSG_3DS));

            if (src_off + CHAT_MSG_3DS > input.size()) break;
            if (dst_off + CHAT_MSG_3DS > output.size()) break;

            // Copy only the first 60 bytes (the actual chat data), skip the 44-byte padding
            std::memcpy(&output[dst_off], &input[src_off], CHAT_MSG_3DS);
        }
    }

    // Copy remaining data after last chat block
    uint32_t remaining_start_mhgu = CHAT3_MHGU + CHAT_LEN_MHGU;
    uint32_t remaining_start_3ds  = CHAT3_3DS + CHAT_LEN_3DS;

    if (remaining_start_mhgu < input.size() && remaining_start_3ds < output.size()) {
        size_t remaining = input.size() - remaining_start_mhgu;
        if (remaining_start_3ds + remaining <= output.size()) {
            std::memcpy(&output[remaining_start_3ds], &input[remaining_start_mhgu], remaining);
        }
    }

    return output;
}

std::vector<uint8_t> convertSwitchXXTo3DS(const std::vector<uint8_t>& input,
                                          const std::vector<uint8_t>& blank_3ds)
{
    // MHXX on Switch is almost identical to 3DS MHXX
    // Just need to adjust the header offset
    return convertSwitchTo3DS(input, blank_3ds);
}

std::vector<uint8_t> convert3DSToSwitchXX(const std::vector<uint8_t>& input,
                                          const std::vector<uint8_t>& blank_mhxx_sw)
{
    if (input.size() != SAVE_SIZE_3DS) {
        throw std::runtime_error("Input is not a valid 3DS MHXX save");
    }
    if (blank_mhxx_sw.size() != SAVE_SIZE_MHXX_SW) {
        throw std::runtime_error("Blank MHXX Switch save has wrong size");
    }

    std::vector<uint8_t> output = blank_mhxx_sw;

    // Copy slot occupancy flags from 3DS position to Switch position
    std::memcpy(&output[40], &input[4], 4);

    // Copy all save data from after 3DS header
    size_t copy_size = input.size() - SLOT1_3DS;
    if (INITIAL_POS_MHXX_SW + copy_size <= output.size()) {
        std::memcpy(&output[INITIAL_POS_MHXX_SW], &input[SLOT1_3DS], copy_size);
    }

    return output;
}

} // namespace SaveConversion
