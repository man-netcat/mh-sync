#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>

#include "Protocol.hpp"

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000
#include "Conversion.hpp"

static const u64 TITLEIDS_MH_3DS[] = {
    0x0004000000197100ULL,  // MHXX Double Cross (JPN)
    0x0004000000187000ULL,  // MH Generations (USA)
    0x000400000014B300ULL,  // MHX / MH Cross (JPN)
};
static const int NUM_MH_3DS_TIDS = sizeof(TITLEIDS_MH_3DS) / sizeof(TITLEIDS_MH_3DS[0]);

static PrintConsole topScreen, bottomScreen;
static int g_sock = -1;
static std::u16string g_appDir = u"/";

static std::u16string asciiToUtf16(const std::string& s)
{
    std::u16string r;
    for (char c : s) r += static_cast<char16_t>(static_cast<unsigned char>(c));
    return r;
}

// --- File logging ---

static void appendLog(const char* msg)
{
    FS_Archive sdmc;
    if (R_FAILED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
        return;

    Handle file;
    Result res = FSUSER_OpenFile(&file, sdmc, fsMakePath(PATH_UTF16, (g_appDir + u"mhsync.log").data()),
                                  FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if (R_SUCCEEDED(res)) {
        u64 size = 0;
        FSFILE_GetSize(file, &size);
        FSFILE_Write(file, NULL, size, msg, static_cast<u32>(strlen(msg)), FS_WRITE_FLUSH);
        FSFILE_Close(file);
    }
    FSUSER_CloseArchive(sdmc);
}

#define LOG(fmt, ...) do { \
    printf(fmt, ##__VA_ARGS__); \
    { \
        char _logbuf[512]; \
        int _len = snprintf(_logbuf, sizeof(_logbuf), fmt, ##__VA_ARGS__); \
        if (_len > 0 && _len < static_cast<int>(sizeof(_logbuf))) { \
            appendLog(_logbuf); \
        } \
    } \
} while(0)

static Result openMHExtdata(FS_Archive* archive, u64 titleId)
{
    u32 extId = static_cast<u32>(titleId & 0xFFFFFFFF) >> 8;
    const u32 path[3] = { MEDIATYPE_SD, extId, 0 };
    return FSUSER_OpenArchive(archive, ARCHIVE_EXTDATA, { PATH_BINARY, 12, path });
}

static u64 openAnyMHExtdata(FS_Archive* archive, int* outIndex)
{
    for (int i = 0; i < NUM_MH_3DS_TIDS; i++) {
        u64 tid = TITLEIDS_MH_3DS[i];
        Result res = openMHExtdata(archive, tid);
        if (R_SUCCEEDED(res)) {
            if (outIndex) *outIndex = i;
            return tid;
        }
    }
    if (outIndex) *outIndex = -1;
    return 0;
}

static bool readSaveFile(std::vector<uint8_t>& out)
{
    FS_Archive archive;
    int tidIdx = -1;
    u64 tid = openAnyMHExtdata(&archive, &tidIdx);
    if (tid == 0) {
        LOG("  Failed to open MH extdata\n");
        return false;
    }

    Handle file;
    Result res = FSUSER_OpenFile(&file, archive, fsMakePath(PATH_UTF16, u"/system"), FS_OPEN_READ, 0);
    if (R_FAILED(res)) {
        LOG("  Failed to open system file: 0x%08X\n", static_cast<unsigned>(res));
        FSUSER_CloseArchive(archive);
        return false;
    }

    u64 fileSize = 0;
    FSFILE_GetSize(file, &fileSize);
    if (fileSize == 0 || fileSize > 10 * 1024 * 1024) {
        LOG("  Bad file size: %llu\n", fileSize);
        FSFILE_Close(file);
        FSUSER_CloseArchive(archive);
        return false;
    }

    out.resize(static_cast<size_t>(fileSize));
    u32 bytesRead = 0;
    FSFILE_Read(file, &bytesRead, 0, out.data(), static_cast<u32>(fileSize));
    FSFILE_Close(file);
    FSUSER_CloseArchive(archive);

    LOG("  Read %u bytes\n", static_cast<unsigned>(bytesRead));
    return bytesRead == fileSize;
}

static bool writeSaveFile(const std::vector<uint8_t>& data)
{
    FS_Archive archive;
    int tidIdx = -1;
    u64 tid = openAnyMHExtdata(&archive, &tidIdx);
    if (tid == 0) {
        LOG("  Failed to open MH extdata\n");
        return false;
    }

    Handle file;
    u32 fileSize = static_cast<u32>(data.size());

    // Create first, delete only on failure — never destroy the save until replacement is ready
    Result res = FSUSER_CreateFile(archive, fsMakePath(PATH_UTF16, u"/system"), 0, fileSize);
    if (R_FAILED(res)) {
        FSUSER_DeleteFile(archive, fsMakePath(PATH_UTF16, u"/system"));
        res = FSUSER_CreateFile(archive, fsMakePath(PATH_UTF16, u"/system"), 0, fileSize);
        if (R_FAILED(res)) {
            LOG("  Failed to create system file: 0x%08X\n", static_cast<unsigned>(res));
            FSUSER_CloseArchive(archive);
            return false;
        }
    }

    res = FSUSER_OpenFile(&file, archive, fsMakePath(PATH_UTF16, u"/system"),
                          FS_OPEN_WRITE, 0);
    if (R_FAILED(res)) {
        LOG("  Failed to open system for write: 0x%08X\n", static_cast<unsigned>(res));
        FSUSER_CloseArchive(archive);
        return false;
    }

    u32 bytesWritten = 0;
    Result writeRes = FSFILE_Write(file, &bytesWritten, 0, data.data(), fileSize, FS_WRITE_FLUSH);
    if (R_FAILED(writeRes)) {
        LOG("  FSFILE_Write failed: 0x%08X\n", static_cast<unsigned>(writeRes));
        FSFILE_Close(file);
        FSUSER_CloseArchive(archive);
        return false;
    }

    bool ok = (bytesWritten == fileSize);
    if (!ok) LOG("  Wrote %lu/%lu bytes\n", static_cast<unsigned long>(bytesWritten), static_cast<unsigned long>(fileSize));

    FSFILE_Close(file);
    FSUSER_CloseArchive(archive);
    return ok;
}

static std::string loadIP(void)
{
    FS_Archive sdmc;
    if (R_FAILED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
        return "";

    Handle file;
    Result res = FSUSER_OpenFile(&file, sdmc, fsMakePath(PATH_UTF16, (g_appDir + u"mhsync_ip.txt").data()), FS_OPEN_READ, 0);
    if (R_FAILED(res)) {
        FSUSER_CloseArchive(sdmc);
        return "";
    }

    u64 size = 0;
    FSFILE_GetSize(file, &size);
    if (size == 0 || size > 32) {
        FSFILE_Close(file);
        FSUSER_CloseArchive(sdmc);
        return "";
    }

    char buf[32] = {0};
    u32 bytesRead = 0;
    FSFILE_Read(file, &bytesRead, 0, buf, static_cast<u32>(size));
    FSFILE_Close(file);
    FSUSER_CloseArchive(sdmc);

    std::string ip(buf, bytesRead);
    // Trim trailing whitespace/newlines
    while (!ip.empty() && (ip.back() == '\n' || ip.back() == '\r' || ip.back() == ' '))
        ip.pop_back();
    return ip;
}

static void saveIP(const std::string& ip)
{
    FS_Archive sdmc;
    if (R_FAILED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
        return;

    Handle file;
    Result res = FSUSER_OpenFile(&file, sdmc, fsMakePath(PATH_UTF16, (g_appDir + u"mhsync_ip.txt").data()),
                                 FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if (R_SUCCEEDED(res)) {
        u32 written = 0;
        FSFILE_Write(file, &written, 0, ip.data(), static_cast<u32>(ip.size()), FS_WRITE_FLUSH);
        FSFILE_Close(file);
    }
    FSUSER_CloseArchive(sdmc);
}

static void safetyBackup(const std::vector<uint8_t>& saveData)
{
    FS_Archive sdmc;
    Result res = FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (R_FAILED(res)) {
        LOG("  WARN: safetyBackup - SDMC archive open failed: 0x%08X\n", static_cast<unsigned>(res));
        return;
    }

    Handle file;
    res = FSUSER_OpenFile(&file, sdmc, fsMakePath(PATH_UTF16, (g_appDir + u"mhsync_backup.bin").data()),
                          FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if (R_SUCCEEDED(res)) {
        u32 written = 0;
        FSFILE_Write(file, &written, 0, saveData.data(), static_cast<u32>(saveData.size()), FS_WRITE_FLUSH);
        FSFILE_Close(file);
        LOG("  Safety backup saved\n");
    } else {
        LOG("  WARN: safetyBackup - could not open backup file: 0x%08X\n", static_cast<unsigned>(res));
    }
    FSUSER_CloseArchive(sdmc);
}

static bool recvAll(int fd, uint8_t* buf, size_t size)
{
    size_t total = 0;
    while (total < size) {
        int n = recv(fd, buf + total, size - total, 0);
        if (n <= 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

static bool sendAll(int fd, const uint8_t* data, size_t size)
{
    size_t total = 0;
    while (total < size) {
        int n = send(fd, data + total, size - total, 0);
        if (n <= 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

static bool doSync(const std::string& serverIP)
{
    const size_t headerSize = sizeof(SyncProtocol::MessageHeader);

    LOG("\n--- Reading save ---\n");
    std::vector<uint8_t> localSave;
    if (!readSaveFile(localSave)) {
        LOG("FAILED: Could not read MHXX save\n");
        return false;
    }

    std::vector<uint8_t> backupBefore = localSave;

    SaveConversion::SaveType localType = SaveConversion::detectSaveType(localSave);
    LOG("  Local save: %s (%zu bytes)\n",
           SaveConversion::saveTypeName(localType).c_str(), localSave.size());

    if (localType != SaveConversion::SaveType::MHXX_3DS) {
        LOG("FAILED: Unexpected save format\n");
        return false;
    }

    SyncProtocol::SaveMetadata localMeta;
    localMeta.platform      = SyncProtocol::Platform::N3DS;
    localMeta.gameVersion   = SyncProtocol::GameVersion::MHXX_3DS;
    localMeta.timestamp     = SaveConversion::extractSaveTimestamp(localSave, localType);
    localMeta.slotBitmask   = SaveConversion::detectSlotBitmask(localSave, localType);
    localMeta.saveFileSize  = static_cast<uint32_t>(localSave.size());
    localMeta.hashPrefix    = SyncProtocol::computeHashPrefix(localSave.data(), localSave.size());
    LOG("  Slots: %d, Hash: 0x%016llX\n", localMeta.slotBitmask, localMeta.hashPrefix);

    LOG("\n--- Connecting to %s:%d ---\n", serverIP.c_str(), SyncProtocol::SYNC_PORT);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG("FAILED: socket() failed\n");
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SyncProtocol::SYNC_PORT);

    if (inet_pton(AF_INET, serverIP.c_str(), &addr.sin_addr) <= 0) {
        LOG("FAILED: Invalid IP\n");
        close(fd);
        return false;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        LOG("FAILED: connect(): %s\n", strerror(errno));
        close(fd);
        return false;
    }

    struct pollfd pfd = { fd, POLLOUT, 0 };
    int pret = poll(&pfd, 1, 10000);
    if (pret <= 0) {
        LOG("FAILED: Connection timed out\n");
        close(fd);
        return false;
    }

    fcntl(fd, F_SETFL, flags);

    int err = 0;
    socklen_t elen = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) {
        LOG("FAILED: %s\n", strerror(err));
        close(fd);
        return false;
    }

    LOG("  Connected!\n");
    g_sock = fd;

    LOG("\n--- Handshake ---\n");
    auto handshakeMsg = SyncProtocol::buildHandshake(localMeta);
    if (!sendAll(fd, handshakeMsg.data(), handshakeMsg.size())) {
        LOG("FAILED: send handshake\n");
        close(fd); g_sock = -1;
        return false;
    }
    LOG("  Sent local metadata\n");

    uint8_t ackHdr[headerSize];
    if (!recvAll(fd, ackHdr, headerSize)) {
        LOG("FAILED: receive ACK header\n");
        close(fd); g_sock = -1;
        return false;
    }

    SyncProtocol::MessageHeader hdr;
    memcpy(&hdr, ackHdr, headerSize);
    std::vector<uint8_t> ackPayload(hdr.payloadLength);
    if (!recvAll(fd, ackPayload.data(), hdr.payloadLength)) {
        LOG("FAILED: receive ACK payload\n");
        close(fd); g_sock = -1;
        return false;
    }

    SyncProtocol::ParsedMessage ack;
    try {
        std::vector<uint8_t> full;
        full.resize(headerSize + hdr.payloadLength);
        memcpy(full.data(), ackHdr, headerSize);
        memcpy(full.data() + headerSize, ackPayload.data(), hdr.payloadLength);
        ack = SyncProtocol::parseMessage(full.data(), full.size());
    } catch (std::exception& e) {
        LOG("FAILED: parse ACK: %s\n", e.what());
        close(fd); g_sock = -1;
        return false;
    }

    if (ack.type != SyncProtocol::MessageType::HANDSHAKE_ACK) {
        LOG("FAILED: expected HANDSHAKE_ACK, got %d\n", static_cast<int>(ack.type));
        close(fd); g_sock = -1;
        return false;
    }

    SyncProtocol::SaveMetadata remoteMeta = SyncProtocol::SaveMetadata::fromBytes(ack.payload.data());
    LOG("  Remote: platform=%d game=%d slots=%d hash=0x%016llX\n",
           static_cast<int>(remoteMeta.platform), static_cast<int>(remoteMeta.gameVersion),
           remoteMeta.slotBitmask, remoteMeta.hashPrefix);

    LOG("\n--- User Choice ---\n");
    bool same = (localMeta.hashPrefix == remoteMeta.hashPrefix);

    LOG("\n=== Save Comparison ===\n\n");
    LOG("Local (3DS):\n  Game: MHXX\n  Slots: %d\n  Hash: 0x%016llX\n",
           localMeta.slotBitmask, localMeta.hashPrefix);
    LOG("\nRemote (Switch):\n  Game: ");
    switch (remoteMeta.gameVersion) {
        case SyncProtocol::GameVersion::MHGU_SW: LOG("MHGU\n"); break;
        case SyncProtocol::GameVersion::MHXX_SW: LOG("MHXX Switch\n"); break;
        case SyncProtocol::GameVersion::MHXX_3DS: LOG("MHXX 3DS\n"); break;
        case SyncProtocol::GameVersion::UNKNOWN: LOG("Unknown\n"); break;
    }
    LOG("  Slots: %d\n  Hash: 0x%016llX\n",
           remoteMeta.slotBitmask, remoteMeta.hashPrefix);

    if (same) {
        LOG("\n\x1b[33mSaves are IDENTICAL.\x1b[0m\n");
    }

    consoleSelect(&bottomScreen);
    LOG("\nChoose sync direction:\n\n");
    LOG("  \x1b[32mA\x1b[0m : Send from 3DS to Switch\n");
    LOG("  \x1b[32mX\x1b[0m : Receive from Switch to 3DS\n");
    LOG("  \x1b[32mB\x1b[0m : Cancel\n");
    if (same) {
        LOG("\n(Saves match - force sync if desired)\n");
    }

    SyncProtocol::SyncDirection direction;
    bool cancelled = false;
    bool confirmed = false;

    // Direction choice loop
    while (!cancelled && !confirmed) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_A) {
            direction = SyncProtocol::SyncDirection::SEND_TO_SWITCH;
        } else if (kDown & KEY_X) {
            direction = SyncProtocol::SyncDirection::RECEIVE_FROM_SWITCH;
        } else if (kDown & KEY_B) {
            cancelled = true;
            break;
        } else {
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
            continue;
        }

        LOG("\n  Chose: %s\n", direction == SyncProtocol::SyncDirection::SEND_TO_SWITCH
            ? "Send from 3DS to Switch" : "Receive from Switch to 3DS");
        consoleSelect(&bottomScreen);
        LOG("\n\x1b[33mConfirm:\x1b[0m\n\n");
        LOG("  %s\n\n",
            direction == SyncProtocol::SyncDirection::SEND_TO_SWITCH
            ? "Send from 3DS to Switch?" : "Receive from Switch to 3DS?");
        LOG("  \x1b[32mA\x1b[0m : Yes, proceed\n");
        LOG("  \x1b[32mB\x1b[0m : No, go back\n");

        while (true) {
            hidScanInput();
            u32 kDown2 = hidKeysDown();
            if (kDown2 & KEY_A) {
                confirmed = true;
                break;
            }
            if (kDown2 & KEY_B) {
                // Go back to direction choice
                consoleSelect(&topScreen);
                LOG("  Cancelled, choose again.\n");
                consoleSelect(&bottomScreen);
                LOG("\nChoose sync direction:\n\n");
                LOG("  \x1b[32mA\x1b[0m : Send from 3DS to Switch\n");
                LOG("  \x1b[32mX\x1b[0m : Receive from Switch to 3DS\n");
                LOG("  \x1b[32mB\x1b[0m : Cancel\n");
                break;
            }
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }
    }

    consoleSelect(&topScreen);

    if (cancelled) {
        LOG("\n  Sync cancelled by user.\n");
        auto abortMsg = SyncProtocol::buildTransferAbort("User cancelled");
        sendAll(fd, abortMsg.data(), abortMsg.size());
        close(fd); g_sock = -1;
        return false;
    }

    std::vector<uint8_t> convertedSave;
    bool sendMode = (direction == SyncProtocol::SyncDirection::SEND_TO_SWITCH);

    if (sendMode) {
        LOG("  3DS will SEND save to Switch...\n");
    } else {
        LOG("  3DS will RECEIVE save from Switch...\n");
    }

    auto reqMsg = SyncProtocol::buildSaveRequest(direction);
    if (!sendAll(fd, reqMsg.data(), reqMsg.size())) {
        LOG("FAILED: send save request\n");
        close(fd); g_sock = -1;
        return false;
    }

    if (sendMode) {
        size_t offset = 0;
        while (offset < localSave.size()) {
            size_t chunkSize = std::min<size_t>(SyncProtocol::MAX_CHUNK_SIZE,
                                                 localSave.size() - offset);
            auto chunk = SyncProtocol::buildSaveDataChunk(
                static_cast<uint32_t>(offset), localSave.data() + offset, static_cast<uint32_t>(chunkSize));

            if (!sendAll(fd, chunk.data(), chunk.size())) {
                LOG("FAILED: send data at offset %zu\n", offset);
                close(fd); g_sock = -1;
                return false;
            }

            offset += chunkSize;

            uint8_t ackBuf[headerSize + sizeof(SyncProtocol::DataChunkHeader)];
            if (!recvAll(fd, ackBuf, sizeof(ackBuf))) {
                LOG("FAILED: receive chunk ACK\n");
                close(fd); g_sock = -1;
                return false;
            }
            LOG("  Sent %zu/%zu bytes\r", offset, localSave.size());
        }
        LOG("\n  Save sent!\n");
        auto completeMsg = SyncProtocol::buildSaveComplete();
        if (!sendAll(fd, completeMsg.data(), completeMsg.size())) {
            LOG("FAILED: send complete message\n");
            close(fd); g_sock = -1;
            return false;
        }

        uint8_t resultHdr[headerSize];
        if (recvAll(fd, resultHdr, headerSize)) {
            SyncProtocol::MessageHeader rHdr;
            memcpy(&rHdr, resultHdr, headerSize);
            if (rHdr.type == static_cast<uint8_t>(SyncProtocol::MessageType::SAVE_RESULT) && rHdr.payloadLength >= 1) {
                std::vector<uint8_t> rPayload(rHdr.payloadLength);
                recvAll(fd, rPayload.data(), rHdr.payloadLength);
                LOG("  Switch: save %s\n", rPayload[0] == 0 ? "written OK" : "write FAILED");
            }
        } else {
            LOG("FAILED: receive save result\n");
            close(fd); g_sock = -1;
            return false;
        }
    } else {
        std::vector<uint8_t> received;
        bool receiving = true;

        while (receiving) {
            uint8_t dataHdr[headerSize];
            if (!recvAll(fd, dataHdr, headerSize)) {
                LOG("FAILED: connection lost during receive\n");
                close(fd); g_sock = -1;
                return false;
            }

            SyncProtocol::MessageHeader dhdr;
            memcpy(&dhdr, dataHdr, headerSize);
            SyncProtocol::MessageType dtype = static_cast<SyncProtocol::MessageType>(dhdr.type);

            if (dtype == SyncProtocol::MessageType::SAVE_COMPLETE) {
                receiving = false;
                break;
            }
            if (dtype == SyncProtocol::MessageType::TRANSFER_ABORT) {
                LOG("FAILED: Switch aborted transfer\n");
                close(fd); g_sock = -1;
                return false;
            }
            if (dtype != SyncProtocol::MessageType::SAVE_DATA) continue;

            std::vector<uint8_t> payload(dhdr.payloadLength);
            if (!recvAll(fd, payload.data(), dhdr.payloadLength)) {
                LOG("FAILED: receive chunk payload\n");
                close(fd); g_sock = -1;
                return false;
            }

            SyncProtocol::DataChunkHeader chunkHdr;
            memcpy(&chunkHdr, payload.data(), sizeof(chunkHdr));

            if (chunkHdr.offset + chunkHdr.size > received.size())
                received.resize(chunkHdr.offset + chunkHdr.size);
            memcpy(received.data() + chunkHdr.offset,
                   payload.data() + sizeof(chunkHdr), chunkHdr.size);

            auto chunkAck = SyncProtocol::buildSaveDataAck(chunkHdr.offset, chunkHdr.size);
            if (!sendAll(fd, chunkAck.data(), chunkAck.size())) {
                LOG("FAILED: send chunk ACK\n");
                close(fd); g_sock = -1;
                return false;
            }
        }

        LOG("  Received %zu bytes\n", received.size());

        LOG("\n--- Converting ---\n");
        SaveConversion::SaveType receivedType = SaveConversion::detectSaveType(received);
        LOG("  Received: %s\n", SaveConversion::saveTypeName(receivedType).c_str());

        if (receivedType == SaveConversion::SaveType::UNKNOWN) {
            LOG("FAILED: Unknown save format received\n");
            close(fd); g_sock = -1;
            return false;
        }

        if (receivedType == SaveConversion::SaveType::MHGU_SWITCH) {
            convertedSave = SaveConversion::convertSwitchTo3DS(received, backupBefore);
            LOG("  Converted MHGU -> MHXX 3DS\n");
        } else if (receivedType == SaveConversion::SaveType::MHXX_SWITCH) {
            convertedSave = SaveConversion::convertSwitchXXTo3DS(received, backupBefore);
            LOG("  Converted MHXX Switch -> MHXX 3DS\n");
        } else {
            convertedSave = received;
            LOG("  No conversion needed\n");
        }

        if (!SaveConversion::validateSave(convertedSave)) {
            LOG("FAILED: Converted save validation failed\n");
            close(fd); g_sock = -1;
            return false;
        }

        LOG("\n--- Safety backup ---\n");
        safetyBackup(backupBefore);

        LOG("\n--- Writing save ---\n");
        bool writeOk = writeSaveFile(convertedSave);
        auto resultMsg = SyncProtocol::buildSaveResult(writeOk);
        sendAll(fd, resultMsg.data(), resultMsg.size());
        if (!writeOk) {
            LOG("FAILED: Could not write save\n");
            close(fd); g_sock = -1;
            return false;
        }
        LOG("  Save written!\n");
    }

    auto complete = SyncProtocol::buildSaveComplete();
    if (!sendAll(fd, complete.data(), complete.size())) {
        LOG("FAILED: send final complete\n");
        close(fd); g_sock = -1;
        return false;
    }

    close(fd);
    g_sock = -1;
    return true;
}

int main(int argc, char* argv[])
{
    {
        const char* arg0 = nullptr;
        if (argc > 0 && argv && argv[0] && argv[0][0]) {
            arg0 = argv[0];
        } else {
            const char* raw = envGetSystemArgList();
            if (raw && raw[0]) {
                arg0 = raw;
            }
        }
        if (arg0) {
            std::string a0(arg0);
            size_t colon = a0.find(':');
            if (colon != std::string::npos && colon + 1 < a0.size() && a0[colon + 1] == '/') {
                a0 = a0.substr(colon + 1);
            }
            size_t pos = a0.rfind('/');
            if (pos != std::string::npos) {
                g_appDir = asciiToUtf16(a0.substr(0, pos + 1));
            }
        }
    }

    gfxInitDefault();
    gfxSet3D(false);
    consoleInit(GFX_TOP, &topScreen);
    consoleInit(GFX_BOTTOM, &bottomScreen);

    bool acInited = R_SUCCEEDED(acInit());
    if (!acInited) {
        LOG("  \x1b[33mWARN: acInit failed - network may not work\x1b[0m\n");
    }
    bool ndmuInited = R_SUCCEEDED(ndmuInit());
    if (!ndmuInited) {
        LOG("  \x1b[33mWARN: ndmuInit failed - network may not work\x1b[0m\n");
    }

    Result socRes = 0;
    uint32_t* socBuf = static_cast<uint32_t*>(memalign(SOC_ALIGN, SOC_BUFFERSIZE));
    if (!socBuf) {
        LOG("FATAL: soc memalign failed\n");
        LOG("Press START to exit\n");
        gfxFlushBuffers();
        gfxSwapBuffers();
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        if (ndmuInited) ndmuExit();
        if (acInited)   acExit();
        gfxExit();
        return 1;
    }
    if ((socRes = socInit(socBuf, SOC_BUFFERSIZE)) != 0) {
        LOG("FATAL: socInit failed: 0x%08X\n", static_cast<unsigned>(socRes));
        LOG("Press START to exit\n");
        gfxFlushBuffers();
        gfxSwapBuffers();
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        free(socBuf);
        if (ndmuInited) ndmuExit();
        if (acInited)   acExit();
        gfxExit();
        return 1;
    }
    // socInit sets buffer to no-access until socExit restores permissions

    LOG("\x1b[32mMH-Sync v1.0 (3DS)\x1b[0m\n");
    LOG("================================\n\n");

    {
        FS_Archive testArch;
        int foundIdx = -1;
        u64 foundTid = openAnyMHExtdata(&testArch, &foundIdx);
        if (foundTid != 0) {
            LOG("  Extdata: \x1b[32mFOUND\x1b[0m (index %d)\n", foundIdx);
            FSUSER_CloseArchive(testArch);
        } else {
            LOG("  Extdata: \x1b[33mNOT FOUND\x1b[0m\n");
            LOG("  TIDs tried:\n");
            for (int i = 0; i < NUM_MH_3DS_TIDS; i++)
                LOG("    %d: 0x%016llX\n", i, TITLEIDS_MH_3DS[i]);
            LOG("  Make sure MH installed on SD card\n");
        }
    }

    consoleSelect(&bottomScreen);
    LOG("\x1b[32mMH-Sync v1.0 (3DS) - Controls\x1b[0m\n\n");
    LOG("  X : Set Switch IP\n");
    LOG("  A : Start sync\n");
    LOG("  B : Exit\n\n");
    consoleSelect(&topScreen);

    std::string targetIP = loadIP();
    if (!targetIP.empty())
        LOG("  Saved IP: %s\n", targetIP.c_str());
    bool syncResult = false;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_B) {
            break;
        }

        if (kDown & KEY_X) {
            SwkbdState swkbd;
            swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, 15);
            swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
            if (!targetIP.empty())
                swkbdSetInitialText(&swkbd, targetIP.c_str());
            swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false);
            swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "OK", true);

            char buf[16];
            SwkbdButton button = swkbdInputText(&swkbd, buf, sizeof(buf));
            if (button == SWKBD_BUTTON_RIGHT) {
                targetIP = buf;
                saveIP(targetIP);
                consoleSelect(&topScreen);
                LOG("\x1b[32m  IP set: %s (saved)\x1b[0m\n", targetIP.c_str());
                consoleSelect(&bottomScreen);
                LOG("  IP: %s\n", targetIP.c_str());
            }
        }

        if (kDown & KEY_A) {
            if (targetIP.empty()) {
                consoleSelect(&topScreen);
                LOG("  \x1b[33mSet IP first (X button)\x1b[0m\n");
                consoleSelect(&bottomScreen);
            } else {
                consoleSelect(&topScreen);
                LOG("\n\x1b[34m--- Sync started ---\x1b[0m\n");
                try {
                    syncResult = doSync(targetIP);
                } catch (std::exception& e) {
                    LOG("\n\x1b[31mEXCEPTION: %s\x1b[0m\n", e.what());
                    syncResult = false;
                } catch (...) {
                    LOG("\n\x1b[31mUNKNOWN EXCEPTION\x1b[0m\n");
                    syncResult = false;
                }
                if (syncResult) {
                    LOG("\n\x1b[32m*** SYNC COMPLETE ***\x1b[0m\n");
                } else {
                    LOG("\n\x1b[31m*** SYNC FAILED ***\x1b[0m\n");
                }
                consoleSelect(&bottomScreen);
                LOG("  %s\n", syncResult ? "SUCCESS" : "FAILED");
                gfxFlushBuffers();
                gfxSwapBuffers();
                gspWaitForVBlank();
                svcSleepThread(5000000000LL);
                break;
            }
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    if (g_sock >= 0) close(g_sock);
    socExit();
    free(socBuf);
    if (ndmuInited) ndmuExit();
    if (acInited)   acExit();
    gfxExit();
    return 0;
}
