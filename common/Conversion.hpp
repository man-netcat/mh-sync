/*
 *   Save conversion engine for Monster Hunter XX / Generations Ultimate.
 *   Enables binary conversion of "system" save files between:
 *     - MHXX on 3DS (4,726,152 bytes)
 *     - MHGU on Switch (5,159,100 bytes)
 *     - MHXX on Switch (4,726,188 bytes)
 *
 *   Based on reverse-engineering by the MHXXSaveEditor project
 *   and the Python converter by Alexander-Lancellott.
 *   Save data knowledge from:
 *     https://gbatemp.net/threads/mhxx-mhgu-save-manager.668510/
 */

#ifndef CONVERSION_HPP
#define CONVERSION_HPP

#include <cstdint>
#include <vector>
#include <string>

namespace SaveConversion {

// ============================================================
// Save file sizes
// ============================================================
constexpr size_t SAVE_SIZE_3DS     = 4726152;  // MHXX on 3DS
constexpr size_t SAVE_SIZE_MHGU_SW = 5159100;  // MHGU on Switch
constexpr size_t SAVE_SIZE_MHXX_SW = 4726188;  // MHXX on Switch

// ============================================================
// 3DS save file layout (MHXX)
// ============================================================
constexpr uint32_t HDR_3DS          = 0;
constexpr uint32_t SLOT_FLAGS_3DS   = 4;     // 3 bytes slot occupancy + 1 byte last slot
constexpr size_t   HDR_SIZE_3DS     = 32;

constexpr uint32_t SLOT1_3DS = 1205364;
constexpr uint32_t SLOT2_3DS = 2378801;
constexpr uint32_t SLOT3_3DS = 3552241;

constexpr uint32_t CHAT1_3DS = 2372861;
constexpr uint32_t CHAT2_3DS = 3546301;
constexpr uint32_t CHAT3_3DS = 4719741;
constexpr uint32_t CHAT_LEN_3DS = 5940;    // 99 × 60 bytes

constexpr int CHAT_MSG_3DS    = 60;         // 60 bytes per chat message on 3DS
constexpr int CHAT_MSG_SWITCH = 104;        // 104 bytes per chat message on Switch (60 + 44 padding)
constexpr int CHAT_MSG_COUNT  = 99;         // 99 shoutout slots

// ============================================================
// Switch (MHGU) save file layout
// ============================================================
constexpr uint32_t INITIAL_POS_MHGU = 1625244;
constexpr uint32_t SLOT1_MHGU = 1625244;
constexpr uint32_t SLOT2_MHGU = 2803037;
constexpr uint32_t SLOT3_MHGU = 3980833;

constexpr uint32_t CHAT1_MHGU = 2792741;
constexpr uint32_t CHAT2_MHGU = 3970537;
constexpr uint32_t CHAT3_MHGU = 5148333;
constexpr uint32_t CHAT_LEN_MHGU = 10296;   // 99 × 104 bytes

// Switch (MHXX) save is similar to 3DS but with slightly different size
constexpr uint32_t INITIAL_POS_MHXX_SW = 1205400;

// ============================================================
// API
// ============================================================

// Detect save type from raw data
enum class SaveType {
    UNKNOWN,
    MHXX_3DS,
    MHGU_SWITCH,
    MHXX_SWITCH
};
SaveType detectSaveType(const std::vector<uint8_t>& data);

// Get human-readable type name
std::string saveTypeName(SaveType type);

// Detect which character slots are used from the slot bitmask
// Returns bitmask: bit 0 = slot 1 used, bit 1 = slot 2 used, bit 2 = slot 3 used
uint8_t detectSlotBitmask(const std::vector<uint8_t>& data, SaveType type);

// Extract a simple timestamp-like identifier from save data
// Uses offset 0x20 (play time) from the first character slot
uint64_t extractSaveTimestamp(const std::vector<uint8_t>& data, SaveType type);

// ============================================================
// Conversion functions
// ============================================================

// Convert 3DS MHXX system file → Switch MHGU system file
// input: 3DS save data (4,726,152 bytes)
// blank_mhgu: blank Switch MHGU save template (5,159,100 bytes)
// Returns: converted Switch MHGU save data (5,159,100 bytes)
std::vector<uint8_t> convert3DSToMHGU(const std::vector<uint8_t>& input,
                                      const std::vector<uint8_t>& blank_mhgu);

// Convert Switch MHGU system file → 3DS MHXX system file
// input: Switch save data (5,159,100 bytes for MHGU or 4,726,188 for MHXX Switch)
// blank_3ds: blank 3DS MHXX save template (4,726,152 bytes)
// Returns: converted 3DS save data (4,726,152 bytes)
std::vector<uint8_t> convertSwitchTo3DS(const std::vector<uint8_t>& input,
                                        const std::vector<uint8_t>& blank_3ds);

// Convert Switch MHXX system file → 3DS MHXX system file (nearly identical)
std::vector<uint8_t> convertSwitchXXTo3DS(const std::vector<uint8_t>& input,
                                          const std::vector<uint8_t>& blank_3ds);

// Convert 3DS MHXX → Switch MHXX
std::vector<uint8_t> convert3DSToSwitchXX(const std::vector<uint8_t>& input,
                                          const std::vector<uint8_t>& blank_mhxx_sw);

// Validate save file size + known type check
bool validateSave(const std::vector<uint8_t>& data);

} // namespace SaveConversion

#endif // CONVERSION_HPP
