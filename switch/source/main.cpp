#include <switch.h>
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
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <dirent.h>

#include "Protocol.hpp"
#include "Conversion.hpp"

// --- File logging (writes to SD card alongside screen output) ---
static FILE* g_logFile = nullptr;
static std::string g_appDir = "sdmc:/";

static void closeLog()
{
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

#define LOG(fmt, ...) do { \
    printf(fmt, ##__VA_ARGS__); \
    if (g_logFile) { fprintf(g_logFile, fmt, ##__VA_ARGS__); fflush(g_logFile); } \
} while(0)

// Known title IDs
static const u64 TITLEID_MHGU_SWITCH  = 0x0100770008DD8000ULL;
static const u64 TITLEID_MHXX_SWITCH  = 0x0100F3400BEE0000ULL;

static int g_serverFd = -1;
static int g_clientFd = -1;
static AccountUid g_accountUid = {0};
// Direction always chosen by 3DS

// --- Save I/O helpers ---

static bool g_saveMounted = false;
static bool g_saveFound = false;
static const char* g_saveGameName = nullptr;
static size_t g_saveSize = 0;
static u64 g_saveTitleId = 0;

static Result mountSave(u64 titleId)
{
    Result rc = fsdevMountSaveData("mhsync", titleId, g_accountUid);
    if (R_SUCCEEDED(rc)) {
        g_saveMounted = true;
    }
    return rc;
}

static void unmountSave()
{
    if (g_saveMounted) {
        fsdevUnmountDevice("mhsync");
        g_saveMounted = false;
    }
}

static bool readSaveFile(std::vector<uint8_t>& out)
{
    if (!g_saveMounted) return false;

    FILE* f = fopen("mhsync:/system", "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return false;
    }

    out.resize(static_cast<size_t>(sz));
    fseek(f, 0, SEEK_SET);
    size_t read = fread(out.data(), 1, static_cast<size_t>(sz), f);
    fclose(f);

    return read == static_cast<size_t>(sz);
}

static bool writeSaveFile(const std::vector<uint8_t>& data)
{
    if (!g_saveMounted) return false;

    FILE* f = fopen("mhsync:/system", "wb");
    if (!f) return false;

    size_t written = fwrite(data.data(), 1, data.size(), f);
    fclose(f);

    fsdevCommitDevice("mhsync");

    return written == data.size();
}

static bool safetyBackup(const std::vector<uint8_t>& data)
{
    FILE* f = fopen("mhsync:/mhsync_backup.bin", "wb");
    if (!f) {
        LOG("  WARN: safetyBackup - could not open backup file\n");
        return false;
    }

    size_t written = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    if (written != data.size()) {
        LOG("  WARN: safetyBackup - wrote %zu/%zu bytes\n", written, data.size());
    }
    return written == data.size();
}

// --- Title discovery ---

static bool findMonsterHunterTitle()
{
    const u64 ids[] = { TITLEID_MHGU_SWITCH, TITLEID_MHXX_SWITCH };
    const char* names[] = { "MHGU", "MHXX Switch" };

    for (int i = 0; i < 2; i++) {
        Result rc = mountSave(ids[i]);
        if (R_SUCCEEDED(rc)) {
            std::vector<uint8_t> test;
            if (readSaveFile(test) && !test.empty()) {
                g_saveFound = true;
                g_saveGameName = names[i];
                g_saveSize = test.size();
                g_saveTitleId = ids[i];
                LOG("  Found: \x1b[32m%s\x1b[0m (0x%016llX)\n",
                    names[i], static_cast<long long unsigned>(ids[i]));
                LOG("  Save size: %zu bytes\n", test.size());
                return true;
            }
            unmountSave();
        }
    }

    return false;
}

// --- Networking ---

static bool recvAll(int fd, uint8_t* buf, size_t size)
{
    size_t total = 0;
    while (total < size) {
        ssize_t n = recv(fd, buf + total, size - total, 0);
        if (n <= 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

static bool sendAll(int fd, const uint8_t* data, size_t size)
{
    size_t total = 0;
    while (total < size) {
        ssize_t n = send(fd, data + total, size - total, 0);
        if (n <= 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

// --- IP display ---

static void getLocalIP(char* buf, size_t len)
{
    if (len < 1) return;
    buf[0] = '\0';

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    // Connect to a public DNS to determine the local IP (no data sent)
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    inet_pton(AF_INET, "1.1.1.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
        struct sockaddr_in local;
        socklen_t localLen = sizeof(local);
        if (getsockname(sock, reinterpret_cast<struct sockaddr*>(&local), &localLen) == 0) {
            inet_ntop(AF_INET, &local.sin_addr, buf, len);
        }
    }
    close(sock);
}

// --- Sync server ---

static bool handleSync(int clientFd)
{
    const size_t headerSize = sizeof(SyncProtocol::MessageHeader);

    LOG("\n--- Reading save ---\n");
    std::vector<uint8_t> localSave;
    if (!readSaveFile(localSave)) {
        LOG("FAILED: Could not read save\n");
        return false;
    }

    std::vector<uint8_t> backupBefore = localSave;

    SaveConversion::SaveType localType = SaveConversion::detectSaveType(localSave);
    LOG("  Local save: %s (%zu bytes)\n",
        SaveConversion::saveTypeName(localType).c_str(), localSave.size());

    if (localType != SaveConversion::SaveType::MHGU_SWITCH &&
        localType != SaveConversion::SaveType::MHXX_SWITCH) {
        LOG("FAILED: Unexpected save format\n");
        return false;
    }

    SyncProtocol::GameVersion localGV = (localType == SaveConversion::SaveType::MHGU_SWITCH)
        ? SyncProtocol::GameVersion::MHGU_SW
        : SyncProtocol::GameVersion::MHXX_SW;

    SyncProtocol::SaveMetadata localMeta;
    localMeta.platform      = SyncProtocol::Platform::NX;
    localMeta.gameVersion   = localGV;
    localMeta.timestamp     = SaveConversion::extractSaveTimestamp(localSave, localType);
    localMeta.slotBitmask   = SaveConversion::detectSlotBitmask(localSave, localType);
    localMeta.saveFileSize  = static_cast<uint32_t>(localSave.size());
    localMeta.hashPrefix    = SyncProtocol::computeHashPrefix(localSave.data(), localSave.size());
    LOG("  Slots: %d, Hash: 0x%016llX\n", localMeta.slotBitmask,
        static_cast<long long unsigned>(localMeta.hashPrefix));

    LOG("\n--- Waiting for handshake ---\n");

    uint8_t handshakeHdr[headerSize];
    if (!recvAll(clientFd, handshakeHdr, headerSize)) {
        LOG("FAILED: receive handshake header\n");
        return false;
    }

    SyncProtocol::MessageHeader hdr;
    memcpy(&hdr, handshakeHdr, headerSize);
    std::vector<uint8_t> handshakePayload(hdr.payloadLength);
    if (!recvAll(clientFd, handshakePayload.data(), hdr.payloadLength)) {
        LOG("FAILED: receive handshake payload\n");
        return false;
    }

    SyncProtocol::ParsedMessage handshake;
    try {
        std::vector<uint8_t> full;
        full.resize(headerSize + hdr.payloadLength);
        memcpy(full.data(), handshakeHdr, headerSize);
        memcpy(full.data() + headerSize, handshakePayload.data(), hdr.payloadLength);
        handshake = SyncProtocol::parseMessage(full.data(), full.size());
    } catch (std::exception& e) {
        LOG("FAILED: parse handshake: %s\n", e.what());
        return false;
    }

    if (handshake.type != SyncProtocol::MessageType::HANDSHAKE) {
        LOG("FAILED: expected HANDSHAKE, got %d\n", static_cast<int>(handshake.type));
        return false;
    }

    SyncProtocol::SaveMetadata remoteMeta = SyncProtocol::SaveMetadata::fromBytes(handshake.payload.data());
    LOG("  3DS: slots=%d hash=0x%016llX\n",
        remoteMeta.slotBitmask, static_cast<long long unsigned>(remoteMeta.hashPrefix));

    auto ackMsg = SyncProtocol::buildHandshakeAck(localMeta, SyncProtocol::SyncDirection::RECEIVE_FROM_SWITCH);
    if (!sendAll(clientFd, ackMsg.data(), ackMsg.size())) {
        LOG("FAILED: send handshake ACK\n");
        return false;
    }
    LOG("  Sent ACK\n");

    LOG("\n--- Direction from 3DS ---\n");

    uint8_t reqHdr[headerSize];
    if (!recvAll(clientFd, reqHdr, headerSize)) {
        LOG("FAILED: receive request\n");
        return false;
    }

    SyncProtocol::MessageHeader reqHeader;
    memcpy(&reqHeader, reqHdr, headerSize);
    SyncProtocol::MessageType reqType = static_cast<SyncProtocol::MessageType>(reqHeader.type);

    if (reqType == SyncProtocol::MessageType::SAVE_COMPLETE) {
        LOG("  3DS sent COMPLETE (identical saves).\n");
        return true;
    }
    if (reqType == SyncProtocol::MessageType::TRANSFER_ABORT) {
        LOG("  3DS aborted: user cancelled.\n");
        return true;
    }
    if (reqType != SyncProtocol::MessageType::SAVE_REQUEST) {
        LOG("FAILED: expected SAVE_REQUEST, got %d\n", static_cast<int>(reqType));
        return false;
    }

    if (reqHeader.payloadLength < 1) {
        LOG("FAILED: SAVE_REQUEST missing direction byte\n");
        return false;
    }
    std::vector<uint8_t> reqPayload(reqHeader.payloadLength);
    if (!recvAll(clientFd, reqPayload.data(), reqHeader.payloadLength)) {
        LOG("FAILED: read SAVE_REQUEST payload\n");
        return false;
    }
    SyncProtocol::SyncDirection direction = static_cast<SyncProtocol::SyncDirection>(reqPayload[0]);

    std::vector<uint8_t> convertedSave;

    if (direction == SyncProtocol::SyncDirection::RECEIVE_FROM_SWITCH) {
        // 3DS wants our save -> Switch sends
        LOG("  3DS chose to RECEIVE - sending Switch save...\n");

        size_t offset = 0;
        while (offset < localSave.size()) {
            size_t chunkSize = std::min<size_t>(SyncProtocol::MAX_CHUNK_SIZE,
                                                 localSave.size() - offset);
            auto chunk = SyncProtocol::buildSaveDataChunk(
                static_cast<uint32_t>(offset), localSave.data() + offset, static_cast<uint32_t>(chunkSize));

            if (!sendAll(clientFd, chunk.data(), chunk.size())) {
                LOG("FAILED: send chunk at %zu\n", offset);
                return false;
            }

            offset += chunkSize;

            // Wait for ACK
            uint8_t ackBuf[headerSize + sizeof(SyncProtocol::DataChunkHeader)];
            if (!recvAll(clientFd, ackBuf, sizeof(ackBuf))) {
                LOG("FAILED: receive ACK\n");
                return false;
            }
            LOG("  Sent %zu/%zu bytes\r", offset, localSave.size());
        }
        LOG("\n  Save sent!\n");

        // Send SAVE_COMPLETE to terminate 3DS receive loop
        auto sendComplete = SyncProtocol::buildSaveComplete();
        if (!sendAll(clientFd, sendComplete.data(), sendComplete.size())) {
            LOG("FAILED: send save complete\n");
            return false;
        }

        uint8_t resultHdr[headerSize];
        if (recvAll(clientFd, resultHdr, headerSize)) {
            SyncProtocol::MessageHeader rHdr;
            memcpy(&rHdr, resultHdr, headerSize);
            if (rHdr.type == static_cast<uint8_t>(SyncProtocol::MessageType::SAVE_RESULT) && rHdr.payloadLength >= 1) {
                std::vector<uint8_t> rPayload(rHdr.payloadLength);
                if (!recvAll(clientFd, rPayload.data(), rHdr.payloadLength)) {
                    LOG("FAILED: receive save result payload\n");
                    return false;
                }
                LOG("  3DS: save %s\n", rPayload[0] == 0 ? "written OK" : "write FAILED");
            }
        } else {
            LOG("FAILED: receive save result\n");
            return false;
        }

    } else {
        LOG("  3DS chose to SEND - receiving from 3DS...\n");

        std::vector<uint8_t> received;
        bool receiving = true;

        while (receiving) {
            uint8_t dataHdr[headerSize];
            if (!recvAll(clientFd, dataHdr, headerSize)) {
                LOG("FAILED: connection lost during receive\n");
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
                LOG("FAILED: 3DS aborted\n");
                return false;
            }
            if (dtype != SyncProtocol::MessageType::SAVE_DATA) continue;

            std::vector<uint8_t> payload(dhdr.payloadLength);
            if (!recvAll(clientFd, payload.data(), dhdr.payloadLength)) {
                LOG("FAILED: receive chunk\n");
                return false;
            }

            SyncProtocol::DataChunkHeader chunkHdr;
            memcpy(&chunkHdr, payload.data(), sizeof(chunkHdr));

            if (chunkHdr.offset + chunkHdr.size > received.size())
                received.resize(chunkHdr.offset + chunkHdr.size);
            memcpy(received.data() + chunkHdr.offset,
                   payload.data() + sizeof(chunkHdr), chunkHdr.size);

            auto ack = SyncProtocol::buildSaveDataAck(chunkHdr.offset, chunkHdr.size);
            if (!sendAll(clientFd, ack.data(), ack.size())) {
                LOG("FAILED: send chunk ACK\n");
                return false;
            }
        }

        LOG("  Received %zu bytes\n", received.size());

        SaveConversion::SaveType receivedType = SaveConversion::detectSaveType(received);
        LOG("  Received: %s\n", SaveConversion::saveTypeName(receivedType).c_str());

        if (receivedType == SaveConversion::SaveType::UNKNOWN) {
            LOG("FAILED: Unknown save format received\n");
            return false;
        }

        if (receivedType == SaveConversion::SaveType::MHXX_3DS) {
            LOG("  Converting to Switch format...\n");
            if (localType == SaveConversion::SaveType::MHGU_SWITCH) {
                convertedSave = SaveConversion::convert3DSToMHGU(received, backupBefore);
            } else {
                convertedSave = SaveConversion::convert3DSToSwitchXX(received, backupBefore);
            }
        } else {
            convertedSave = received;
            LOG("  No conversion needed\n");
        }

        if (!SaveConversion::validateSave(convertedSave)) {
            LOG("FAILED: Converted save validation failed\n");
            return false;
        }

        LOG("\n--- Safety backup ---\n");
        if (safetyBackup(backupBefore)) {
            LOG("  Backup saved\n");
        }

        LOG("\n--- Writing save ---\n");
        bool writeOk = writeSaveFile(convertedSave);
        auto resultMsg = SyncProtocol::buildSaveResult(writeOk);
        sendAll(clientFd, resultMsg.data(), resultMsg.size());
        if (!writeOk) {
            LOG("FAILED: Could not write save\n");
            return false;
        }
        LOG("  Save written!\n");
    }

    uint8_t finalHdr[headerSize];
    if (!recvAll(clientFd, finalHdr, headerSize)) {
        LOG("FAILED: receive final complete\n");
        return false;
    }

    auto completeMsg = SyncProtocol::buildSaveComplete();
    if (!sendAll(clientFd, completeMsg.data(), completeMsg.size())) {
        LOG("FAILED: send final complete\n");
        return false;
    }

    return true;
}

// --- Main ---

int main(int argc, char** argv)
{
    if (argc > 0 && argv[0] && argv[0][0]) {
        std::string a0(argv[0]);
        size_t pos = a0.rfind('/');
        if (pos != std::string::npos) {
            std::string dir = a0.substr(0, pos + 1);
            if (dir.find(':') == std::string::npos) {
                dir = "sdmc:" + dir;
            }
            g_appDir = dir;
        }
    }

    PrintConsole g_con;
    consoleInit(&g_con);

    printf("\x1b[32mMH-Sync v1.0 (Switch)\x1b[0m\n");
    printf("================================\n\n");

    Result bsdRes = socketInitializeDefault();
    if (R_FAILED(bsdRes)) {
        printf("WARN: socketInit: 0x%08X (net will fail)\n", static_cast<unsigned>(bsdRes));
    }

    if (appletGetAppletType() == AppletType_Application) {
        printf("  Mode: \x1b[32mApplication\x1b[0m\n\n");
    } else {
        printf("  Mode: \x1b[33mApplet\x1b[0m\n");
        printf("  \x1b[33mNetwork requires application mode!\x1b[0m\n");
        printf("  Hold R while launching a game to get there.\n\n");
    }
    consoleUpdate(NULL);

    Result rc;
    rc = fsInitialize();
    if (R_FAILED(rc)) {
        printf("FATAL: fsInit: 0x%08X\n", static_cast<unsigned>(rc));
        printf("Exiting...\n");
        consoleUpdate(NULL);
        svcSleepThread(3000000000LL); // 3 seconds to read error
        socketExit();
        consoleExit(&g_con);
        return 1;
    }

    // Open log file next to the app (needs fsInitialize first for FSP access)
    g_logFile = fopen((g_appDir + "mhsync.log").c_str(), "a");
    if (g_logFile) {
        fprintf(g_logFile, "MH-Sync v1.0 (Switch) - log started\n");
        fprintf(g_logFile, "================================\n\n");
        fflush(g_logFile);
    }

    rc = accountInitialize(AccountServiceType_Application);
    if (R_SUCCEEDED(rc)) {
        rc = accountGetPreselectedUser(&g_accountUid);
        if (R_FAILED(rc)) {
            LOG("WARN: accountGetPreselectedUser: 0x%08X (UID will be zero)\n", static_cast<unsigned>(rc));
        }
        accountExit();
    } else {
        LOG("WARN: accountInit: 0x%08X (UID will be zero)\n", static_cast<unsigned>(rc));
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    consoleUpdate(NULL);

    LOG("Searching for Monster Hunter saves...\n");
    bool found = findMonsterHunterTitle();
    if (!found) {
        LOG("  \x1b[33mNo MH save found\x1b[0m\n");
        LOG("  Check that MHGU/MHXX has been launched\n");
        LOG("  Expected TIDs:\n");
        LOG("    MHGU:  0x%016llX\n", static_cast<long long unsigned>(TITLEID_MHGU_SWITCH));
        LOG("    MHXX:  0x%016llX\n", static_cast<long long unsigned>(TITLEID_MHXX_SWITCH));
    }

    // Display Switch IP address so the user knows where to connect
    char ipBuf[INET_ADDRSTRLEN] = "unknown";
    getLocalIP(ipBuf, sizeof(ipBuf));
    LOG("  Switch IP: \x1b[36m%s\x1b[0m\n", ipBuf);

    LOG("\n\x1b[32mControls:\x1b[0m\n");
    LOG("  A : Start server (wait for 3DS)\n");
    LOG("  B : Exit\n\n");

    bool serverRunning = false;
    bool exitApp = false;

    auto redrawScreen = [&]() {
        printf("\x1b[H\x1b[J");
        LOG("\x1b[32mMH-Sync v1.0 (Switch)\x1b[0m\n");
        LOG("================================\n\n");
        if (appletGetAppletType() == AppletType_Application) {
            LOG("  Mode: \x1b[32mApplication\x1b[0m\n\n");
        } else {
            LOG("  Mode: \x1b[33mApplet\x1b[0m\n");
            LOG("  \x1b[33mNetwork requires application mode!\x1b[0m\n");
            LOG("  Hold R while launching a game to get there.\n\n");
        }
        LOG("Searching for Monster Hunter saves...\n");
        if (g_saveFound) {
            LOG("  Found: \x1b[32m%s\x1b[0m (0x%016llX)\n",
                g_saveGameName, static_cast<long long unsigned>(g_saveTitleId));
            LOG("  Save size: %zu bytes\n", g_saveSize);
        } else {
            LOG("  \x1b[33mNo MH save found\x1b[0m\n");
            LOG("  Check that MHGU/MHXX has been launched\n");
            LOG("  Expected TIDs:\n");
            LOG("    MHGU:  0x%016llX\n", static_cast<long long unsigned>(TITLEID_MHGU_SWITCH));
            LOG("    MHXX:  0x%016llX\n", static_cast<long long unsigned>(TITLEID_MHXX_SWITCH));
        }
        LOG("  Switch IP: \x1b[36m%s\x1b[0m\n\n", ipBuf);
        LOG("\x1b[32mControls:\x1b[0m\n");
        LOG("  A : %s\n", serverRunning ? "Listening... (B to stop)" : "Start server (wait for 3DS)");
        LOG("  B : Exit\n\n");
    };

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_B) {
            break;
        }

        if (kDown & HidNpadButton_A && !serverRunning) {
            if (!g_saveMounted) {
                LOG("  \x1b[33mNo MH save mounted\x1b[0m\n");
                continue;
            }

            int serverFd = socket(AF_INET, SOCK_STREAM, 0);
            if (serverFd < 0) {
                LOG("FAILED: socket()\n");
                continue;
            }

            int opt = 1;
            setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(SyncProtocol::SYNC_PORT);
            addr.sin_addr.s_addr = INADDR_ANY;

            if (bind(serverFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
                LOG("FAILED: bind: %s\n", strerror(errno));
                close(serverFd);
                continue;
            }

            if (listen(serverFd, 1) < 0) {
                LOG("FAILED: listen: %s\n", strerror(errno));
                close(serverFd);
                continue;
            }

            serverRunning = true;
            redrawScreen();
            g_serverFd = serverFd;

            // Poll-based accept with cancel support
            int clientFd = -1;
            while (clientFd < 0) {
                // Check for cancel
                padUpdate(&pad);
                if (padGetButtonsDown(&pad) & HidNpadButton_B) {
                    break;
                }

                struct pollfd pfd = { serverFd, POLLIN, 0 };
                int pret = poll(&pfd, 1, 100);
                if (pret > 0) {
                    struct sockaddr_in clientAddr;
                    socklen_t clientLen = sizeof(clientAddr);
                    clientFd = accept(serverFd, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
                    if (clientFd >= 0) {
                        char ipStr[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
                        LOG("  Connected from: %s\n", ipStr);

                        g_clientFd = clientFd;

                        try {
                            bool ok = handleSync(clientFd);
                            if (ok) {
                                LOG("\n\x1b[32m*** SYNC COMPLETE ***\x1b[0m\n");
                            } else {
                                LOG("\n\x1b[31m*** SYNC FAILED ***\x1b[0m\n");
                            }
                        } catch (std::exception& e) {
                            LOG("\n\x1b[31mEXCEPTION: %s\x1b[0m\n", e.what());
                        } catch (...) {
                            LOG("\n\x1b[31mUNKNOWN EXCEPTION\x1b[0m\n");
                        }

                        close(clientFd);
                        g_clientFd = -1;
                        consoleUpdate(NULL);
                        svcSleepThread(5000000000LL);
                        exitApp = true;
                        break;
                    } else {
                        LOG("FAILED: accept: %s\n", strerror(errno));
                    }
                }

                consoleUpdate(NULL);
            }

            close(serverFd);
            g_serverFd = -1;
            serverRunning = false;
            redrawScreen();

            if (exitApp) break;
        }

        consoleUpdate(NULL);
    }

    if (g_clientFd >= 0) close(g_clientFd);
    if (g_serverFd >= 0) close(g_serverFd);
    unmountSave();
    closeLog();
    fsExit();
    socketExit();
    consoleExit(&g_con);
    return 0;
}
