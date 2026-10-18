#include "RSDK/Core/RetroEngine.hpp"

using namespace RSDK;

RSDKFileInfo RSDK::dataFileList[DATAFILE_COUNT];
RSDKContainer RSDK::dataPacks[DATAPACK_COUNT];

uint8 RSDK::dataPackCount      = 0;
uint16 RSDK::dataFileListCount = 0;

char RSDK::gameLogicName[0x200];

bool32 RSDK::useDataPack = false;

#if RETRO_PLATFORM == RETRO_PSP
#include <fcntl.h>
#include <unistd.h>

extern int pspLogFiles; // main.cpp, boot.txt "logfiles"
extern char pspPackName[0x40]; // main.cpp, boot.txt "pack=NAME" (to compare pack layouts)

#define PSP_PACKVIEW_COUNT  (16)
#define PSP_PACKBLOCK_SIZE  (0x8000)
#define PSP_PACKBLOCK_COUNT (2) // nested loads (an animation .bin opening its sheets) each keep a block warm

struct PSPPackView {
    int32 inUse;
    int32 packID;
    uint32 pos; // absolute offset into the pack, exactly like the FILE position it replaces
};

static PSPPackView pspPackViews[PSP_PACKVIEW_COUNT];
static int32 pspPackFD[DATAPACK_COUNT] = { -1, -1, -1, -1 };

alignas(64) static uint8 pspPackBlock[PSP_PACKBLOCK_COUNT][PSP_PACKBLOCK_SIZE];
static int32 pspPackBlockPack[PSP_PACKBLOCK_COUNT] = { -1, -1 };
static uint32 pspPackBlockStart[PSP_PACKBLOCK_COUNT];
static uint32 pspPackBlockSize[PSP_PACKBLOCK_COUNT];
static uint32 pspPackBlockUsed[PSP_PACKBLOCK_COUNT];
static uint32 pspPackClock;

PSPPackStats RSDK::pspPackStats;
unsigned long long PSPPackIoUS() { return RSDK::pspPackStats.ioUS; }

static inline PSPPackView *PSPGetPackView(FileIO *file)
{
    PSPPackView *view = (PSPPackView *)file;
    return view >= pspPackViews && view < pspPackViews + PSP_PACKVIEW_COUNT ? view : NULL;
}

extern volatile int pspResumeCount; // PSPRenderDevice.cpp: sleep mode closes open files
static int32 pspPackFDResume[DATAPACK_COUNT];

static uint32 PSPPackDeviceRead(int32 packID, uint32 offset, void *buffer, uint32 size)
{
    int32 &fd = pspPackFD[packID];
    // opened once and kept - until the PSP sleeps, which closes it: then (or whenever a read fails) it is opened again,
    // a few times over while the Memory Stick comes back
    if (fd >= 0 && pspPackFDResume[packID] != pspResumeCount) {
        close(fd);
        fd = -1;
    }

    unsigned long long start = PSPProfNow();
    int32 bytes              = -1;
    for (int32 attempt = 0; attempt < 20 && bytes < 0; ++attempt) {
        if (fd < 0) {
            pspPackFDResume[packID] = pspResumeCount;
            fd                      = open(dataPacks[packID].name, O_RDONLY);
        }
        if (fd >= 0 && lseek(fd, offset, SEEK_SET) == (off_t)offset)
            bytes = read(fd, buffer, size);
        if (bytes < 0) {
            if (fd >= 0)
                close(fd);
            fd = -1;
            sceKernelDelayThread(100 * 1000);
        }
    }
    if (bytes < 0)
        bytes = 0;

    pspPackStats.deviceReads++;
    pspPackStats.bytes += bytes;
    pspPackStats.ioUS += PSPProfNow() - start;
    return bytes;
}

static FileIO *PSPOpenPackView(int32 packID, uint32 offset)
{
    for (int32 v = 0; v < PSP_PACKVIEW_COUNT; ++v) {
        PSPPackView *view = &pspPackViews[v];
        if (!view->inUse) {
            view->inUse  = true;
            view->packID = packID;
            view->pos    = offset;
            pspPackStats.views++;
            return (FileIO *)view;
        }
    }

    pspPackStats.fallbacks++; // something leaked its FileInfo: keep working through stdio
    return NULL;
}

size_t PSPFileRead(void *buffer, size_t elementSize, size_t elementCount, FileIO *file)
{
    PSPPackView *view = PSPGetPackView(file);
    if (!view)
        return fread(buffer, elementSize, elementCount, file);
    if (!elementSize)
        return 0;

    uint8 *dst  = (uint8 *)buffer;
    uint32 size = elementSize * elementCount;
    uint32 done = 0;
    while (done < size) {
        uint32 pos = view->pos;

        int32 b = -1;
        for (int32 i = 0; i < PSP_PACKBLOCK_COUNT; ++i) {
            if (pspPackBlockPack[i] == view->packID && pos - pspPackBlockStart[i] < pspPackBlockSize[i]) {
                b = i;
                break;
            }
        }

        if (b < 0) {
            uint32 remain = size - done;
            if (remain >= PSP_PACKBLOCK_SIZE) {
                // big reads (compressed layouts, sfx, music) go straight to their destination
                uint32 bytes = PSPPackDeviceRead(view->packID, pos, dst + done, remain);
                view->pos += bytes;
                done += bytes;
                break;
            }

            b = 0;
            for (int32 i = 1; i < PSP_PACKBLOCK_COUNT; ++i) {
                if (pspPackBlockUsed[i] < pspPackBlockUsed[b])
                    b = i;
            }

            uint32 start          = pos & ~(PSP_PACKBLOCK_SIZE - 1);
            pspPackBlockPack[b]   = view->packID;
            pspPackBlockStart[b]  = start;
            pspPackBlockSize[b]   = PSPPackDeviceRead(view->packID, start, pspPackBlock[b], PSP_PACKBLOCK_SIZE);
            if (pos - start >= pspPackBlockSize[b])
                break; // end of the pack
        }

        pspPackBlockUsed[b] = ++pspPackClock;
        uint32 offset       = pos - pspPackBlockStart[b];
        uint32 count        = MIN(size - done, pspPackBlockSize[b] - offset);
        memcpy(dst + done, &pspPackBlock[b][offset], count);
        view->pos += count;
        done += count;
    }

    return done / elementSize;
}

int PSPFileSeek(FileIO *file, long offset, int whence)
{
    PSPPackView *view = PSPGetPackView(file);
    if (!view)
        return fseek(file, offset, whence);

    switch (whence) {
        case SEEK_SET: view->pos = (uint32)offset; return 0;
        case SEEK_CUR: view->pos += (uint32)offset; return 0;
        default: return -1; // views never seek from the end
    }
}

long PSPFileTell(FileIO *file)
{
    PSPPackView *view = PSPGetPackView(file);
    return view ? (long)view->pos : ftell(file);
}

int PSPFileClose(FileIO *file)
{
    PSPPackView *view = PSPGetPackView(file);
    if (!view)
        return fclose(file);

    view->inUse = false;
    return 0;
}
#endif

#if RETRO_REV0U
void RSDK::DetectEngineVersion()
{
    bool32 readDataPack = useDataPack;
#if RETRO_USE_MOD_LOADER
    // mods can manually set their target engine versions if needed
    if (modSettings.versionOverride) {
        engine.version = modSettings.versionOverride;
        return;
    }

    // check if we have any mods with gameconfigs
    int32 m = 0;
    for (m = 0; m < modList.size(); ++m) {
        if (!modList[m].active)
            break;
        SetActiveMod(m);

        FileInfo checkInfo;
        InitFileInfo(&checkInfo);
        if (LoadFile(&checkInfo, "Data/Game/GameConfig.bin", FMODE_RB)) {
            readDataPack = false;
            CloseFile(&checkInfo);
            m = 0; // found one, just sets this to 0 again
            break;
        }
    }

    if (m) // didn't find a gameconfig
        SetActiveMod(-1);
#endif

    FileInfo info;
    InitFileInfo(&info);
    if (!readDataPack) {
        if (LoadFile(&info, "Data/Game/GameConfig.bin", FMODE_RB)) {
#if RETRO_USE_MOD_LOADER
            SetActiveMod(-1);
#endif
            uint32 sig = ReadInt32(&info, false);

            // GameConfig has "CFG" signature, its RSDKv5 formatted
            if (sig == RSDK_SIGNATURE_CFG) {
                engine.version = 5;
            }
            else {
                // else, assume its RSDKv4 for now
                engine.version = 4;

                // Go back to the start of the file to check v3's "Data" string, that way we can tell if its v3 or v4
                Seek_Set(&info, 0);

                uint8 length = ReadInt8(&info);
                char buffer[0x40];
                ReadBytes(&info, buffer, length);

                // the "Data" thing is actually a string, but lets treat it as a "signature" for simplicity's sake shall we?
                length     = ReadInt8(&info);
                uint32 sig = ReadInt32(&info, false);
                if (sig == RSDK_SIGNATURE_DATA && length == 4)
                    engine.version = 3;
            }

            CloseFile(&info);
        }
    }
    else {
        info.externalFile = true;
        if (LoadFile(&info, dataPacks[dataPackCount - 1].name, FMODE_RB)) {
            uint32 sig = ReadInt32(&info, false);
            if (sig == RSDK_SIGNATURE_RSDK) {
                ReadInt8(&info); // 'v'
                uint8 version = ReadInt8(&info);

                switch (version) {
                    default: break;
                    case '3': engine.version = 3; break;
                    case '4': engine.version = 4; break;
                    case '5': engine.version = 5; break;
                }
            }
            else {
                // v3 has no 'RSDK' signature
                engine.version = 3;
            }

            CloseFile(&info);
        }
    }
}
#endif

void PSPDecryptSelfTest();
#if RETRO_PLATFORM == RETRO_PSP
// Data.psp (tools/build_psp_data.py, from the player's own Data.rsdk): the same table as Data.rsdk (hash, offset, size)
// but files stored decrypted (the few the tool couldn't name stay encrypted) and music as ATRAC3. When it is there the
// game reads everything from it.
static bool32 PSPLoadPspPack()
{
    char path[0x100];
    sprintf_s(path, sizeof(path), "%s%s", SKU::userFileDir, ::pspPackName);
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return false;

    uint32 header[3];
    if (sceIoRead(fd, header, sizeof(header)) != sizeof(header) || header[0] != 0x50535052 /* 'RPSP' */ || header[1] != 1
        || header[2] > DATAFILE_COUNT - dataFileListCount) {
        sceIoClose(fd);
        return false;
    }

    uint32 count = header[2];
    uint8 *table = (uint8 *)malloc(count * 28);
    bool32 ok    = table && sceIoRead(fd, table, count * 28) == (int)(count * 28);
    sceIoClose(fd);
    if (!ok) {
        free(table);
        return false;
    }

    MEM_ZERO(dataPacks[dataPackCount]);
    strcpy(dataPacks[dataPackCount].name, path);
    dataPacks[dataPackCount].fileCount = count;
    for (uint32 f = 0; f < count; ++f) {
        const uint8 *e     = &table[f * 28];
        RSDKFileInfo *file = &dataFileList[dataFileListCount + f];
        for (int32 y = 0; y < 4; y++) file->hash[y] = (e[y * 4] << 24) | (e[y * 4 + 1] << 16) | (e[y * 4 + 2] << 8) | (e[y * 4 + 3] << 0);
        file->offset        = e[16] | (e[17] << 8) | (e[18] << 16) | (e[19] << 24);
        file->size          = e[20] | (e[21] << 8) | (e[22] << 16) | (e[23] << 24);
        file->pspKind       = e[24];
        file->encrypted     = file->pspKind == PSP_PACK_ENCRYPTED;
        file->useFileBuffer = false;
        file->packID        = dataPackCount;
    }
    free(table);

    PrintLog(PRINT_NORMAL, "PSPINIT: Data.psp table read (%u files)", count);
    PSPDecryptSelfTest();
    dataFileListCount += count;
    dataPackCount++;
    useDataPack = true;
    return true;
}

RSDKFileInfo *RSDK::PSPFindDataFile(const char *filename)
{
    char hashBuffer[0x400];
    StringLowerCase(hashBuffer, filename);
    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5_BUFFER(hashBuffer, hash);
    for (int32 f = 0; f < dataFileListCount; ++f) {
        if (HASH_MATCH_MD5(hash, dataFileList[f].hash))
            return &dataFileList[f];
    }
    return NULL;
}
#endif

bool32 RSDK::LoadDataPack(const char *filePath, size_t fileOffset, bool32 useBuffer)
{
#if RETRO_PLATFORM == RETRO_PSP
    if (!dataPackCount && PSPLoadPspPack())
        return true;
#endif
    MEM_ZERO(dataPacks[dataPackCount]);
    useDataPack = false;
    FileInfo info;

    char dataPackPath[0x100];
    sprintf_s(dataPackPath, sizeof(dataPackPath), "%s%s", SKU::userFileDir, filePath);

    InitFileInfo(&info);
    info.externalFile = true;
    if (LoadFile(&info, dataPackPath, FMODE_RB)) {
        uint32 sig = ReadInt32(&info, false);
        if (sig != RSDK_SIGNATURE_RSDK)
            return false;

        useDataPack = true;

        ReadInt8(&info); // 'v'
        ReadInt8(&info); // version

        strcpy(dataPacks[dataPackCount].name, dataPackPath);

        dataPacks[dataPackCount].fileCount = ReadInt16(&info);
        for (int32 f = 0; f < dataPacks[dataPackCount].fileCount; ++f) {
            uint8 b[4];
            for (int32 y = 0; y < 4; y++) {
                ReadBytes(&info, b, 4);
                dataFileList[f].hash[y] = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | (b[3] << 0);
            }

            dataFileList[f].offset = ReadInt32(&info, false);
            dataFileList[f].size   = ReadInt32(&info, false);

            dataFileList[f].encrypted = (dataFileList[f].size & 0x80000000) != 0;
            dataFileList[f].size &= 0x7FFFFFFF;
            dataFileList[f].useFileBuffer = useBuffer;
            dataFileList[f].packID        = dataPackCount;
        }

#if RETRO_PLATFORM == RETRO_PSP
        PrintLog(PRINT_NORMAL, "PSPINIT: pack table read (%d files, useBuffer=%d)", dataPacks[dataPackCount].fileCount, useBuffer);
        PSPDecryptSelfTest();
#endif
        dataPacks[dataPackCount].fileBuffer = NULL;
        if (useBuffer) {
            dataPacks[dataPackCount].fileBuffer = (uint8 *)malloc(info.fileSize);
            Seek_Set(&info, 0);
            ReadBytes(&info, dataPacks[dataPackCount].fileBuffer, info.fileSize);
        }

        dataFileListCount += dataPacks[dataPackCount].fileCount;
        dataPackCount++;

        CloseFile(&info);

        return true;
    }
    else {
        useDataPack = false;
        return false;
    }
}

#if !RETRO_USE_ORIGINAL_CODE && RETRO_REV0U
inline bool ends_with(std::string const &value, std::string const &ending)
{
    if (ending.size() > value.size())
        return false;
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}
#endif

bool32 RSDK::OpenDataFile(FileInfo *info, const char *filename)
{
    char hashBuffer[0x400];
    StringLowerCase(hashBuffer, filename);
    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5_BUFFER(hashBuffer, hash);

    for (int32 f = 0; f < dataFileListCount; ++f) {
        RSDKFileInfo *file = &dataFileList[f];

        if (!HASH_MATCH_MD5(hash, file->hash))
            continue;

        info->usingFileBuffer = file->useFileBuffer;
        if (!file->useFileBuffer) {
#if RETRO_PLATFORM == RETRO_PSP
            info->file = PSPOpenPackView(file->packID, file->offset);
            if (!info->file)
#endif
            info->file = fOpen(dataPacks[file->packID].name, "rb");
            if (!info->file) {
                PrintLog(PRINT_NORMAL, "File not found (Unable to open datapack): %s", filename);
                return false;
            }

            fSeek(info->file, file->offset, SEEK_SET);
        }
        else {
            // a bit of a hack, but it is how it is in the original
            info->file = (FileIO *)&dataPacks[file->packID].fileBuffer[file->offset];

            uint8 *fileBuffer = (uint8 *)info->file;
            info->fileBuffer  = fileBuffer;
        }

        info->fileSize   = file->size;
        info->readPos    = 0;
        info->fileOffset = file->offset;
        info->encrypted  = file->encrypted;
        memset(info->encryptionKeyA, 0, 0x10 * sizeof(uint8));
        memset(info->encryptionKeyB, 0, 0x10 * sizeof(uint8));
        if (info->encrypted) {
            GenerateELoadKeys(info, filename, info->fileSize);
            info->eKeyNo      = (info->fileSize / 4) & 0x7F;
            info->eKeyPosA    = 0;
            info->eKeyPosB    = 8;
            info->eNybbleSwap = false;
#if RETRO_PLATFORM == RETRO_PSP
            info->pspKeyIndex = 0;
#endif
        }

// PSP: every PrintLog reopens log.txt on the Memory Stick, so skip the per-file noise (hundreds of lines per scene).
#if !RETRO_USE_ORIGINAL_CODE && RETRO_PLATFORM != RETRO_PSP
        PrintLog(PRINT_NORMAL, "Loaded data file %s", filename);
#endif
        return true;
    }

#if !RETRO_USE_ORIGINAL_CODE
#if RETRO_PLATFORM == RETRO_PSP
    if (strncmp(filename, "Data/Objects/Static/", 20) != 0)
#endif
    PrintLog(PRINT_NORMAL, "Data file not found: %s", filename);
#else
    PrintLog(PRINT_NORMAL, "File not found: %s", filename);
#endif
    return false;
}

bool32 RSDK::LoadFile(FileInfo *info, const char *filename, uint8 fileMode)
{
    if (info->file)
        return false;

    char fullFilePath[0x100];
    strcpy(fullFilePath, filename);

#if RETRO_USE_MOD_LOADER
    char pathLower[0x100];
    memset(pathLower, 0, sizeof(pathLower));
    for (int32 c = 0; c < strlen(filename); ++c) pathLower[c] = tolower(filename[c]);

    bool32 addPath = false;
    int32 m        = modSettings.activeMod != -1 ? modSettings.activeMod : 0;
    for (; m < modList.size(); ++m) {
        if (modList[m].active) {
            std::map<std::string, std::string>::const_iterator iter = modList[m].fileMap.find(pathLower);
            if (iter != modList[m].fileMap.cend()) {
                if (std::find(modList[m].excludedFiles.begin(), modList[m].excludedFiles.end(), pathLower) == modList[m].excludedFiles.end()) {
                    strcpy(fullFilePath, iter->second.c_str());
                    info->externalFile = true;
                    break;
                }
                else {
                    PrintLog(PRINT_NORMAL, "[MOD] Excluded File: %s", filename);
                }
            }
        }
        if (modSettings.activeMod != -1) {
            PrintLog(PRINT_NORMAL, "[MOD] Failed to find file %s in active mod %s", filename, modList[m].id.c_str());
            // TODO return false? check original impl later
        }
    }

#if RETRO_REV0U
    if (modSettings.forceScripts && !info->externalFile) {
        if (std::string(fullFilePath).rfind("Data/Scripts/", 0) == 0 && ends_with(std::string(fullFilePath), "txt")) {
            // is a script, since those dont exist normally, load them from "scripts/"
            info->externalFile = true;
            addPath            = true;
            std::string fStr   = std::string(fullFilePath);
            fStr.erase(fStr.begin(), fStr.begin() + 5); // remove "Data/"
            StrCopy(fullFilePath, fStr.c_str());
        }
    }
#endif

#if RETRO_PLATFORM == RETRO_OSX || RETRO_PLATFORM == RETRO_ANDROID
    if (addPath) {
        char pathBuf[0x100];
        sprintf_s(pathBuf, sizeof(pathBuf), "%s%s", SKU::userFileDir, fullFilePath);
        sprintf_s(fullFilePath, sizeof(fullFilePath), "%s", pathBuf);
    }
#else
    (void)addPath;
#endif // ! RETRO_PLATFORM
#endif // ! RETRO_MOD_LOADER

#if !RETRO_USE_ORIGNAL_CODE
    // somewhat hacky that also pleases the mod gods
    if (!info->externalFile) {
        char pathBuf[0x100];
        sprintf_s(pathBuf, sizeof(pathBuf), "%s%s", SKU::userFileDir, fullFilePath);
        sprintf_s(fullFilePath, sizeof(fullFilePath), "%s", pathBuf);
    }
#endif

    if (!info->externalFile && fileMode == FMODE_RB && useDataPack) {
#if RETRO_PLATFORM == RETRO_PSP
        if (::pspLogFiles)
            PrintLog(PRINT_NORMAL, "PSP file %s/%s: %s", currentSceneFolder, currentSceneID, filename);
#endif
        return OpenDataFile(info, filename);
    }

    if (fileMode == FMODE_RB || fileMode == FMODE_WB || fileMode == FMODE_RB_PLUS) {
        info->file = fOpen(fullFilePath, openModes[fileMode - 1]);
    }

    if (!info->file) {
#if !RETRO_USE_ORIGINAL_CODE
        PrintLog(PRINT_NORMAL, "File not found: %s", fullFilePath);
#endif
        return false;
    }

    info->readPos  = 0;
    info->fileSize = 0;

    if (fileMode != FMODE_WB) {
        fSeek(info->file, 0, SEEK_END);
        info->fileSize = (int32)fTell(info->file);
        fSeek(info->file, 0, SEEK_SET);
    }
#if !RETRO_USE_ORIGINAL_CODE
    PrintLog(PRINT_NORMAL, "Loaded file %s", fullFilePath);
#endif
    return true;
}

void RSDK::GenerateELoadKeys(FileInfo *info, const char *key1, int32 key2)
{
    // This function splits hashes into bytes by casting their integers to byte arrays,
    // which only works as intended on little-endian CPUs.
#if !RETRO_USE_ORIGINAL_CODE
    RETRO_HASH_MD5(hash);
#else
    uint8 hash[0x10];
#endif
    char hashBuffer[0x400];

    // KeyA
    StringUpperCase(hashBuffer, key1);
#if !RETRO_USE_ORIGINAL_CODE
    GEN_HASH_MD5_BUFFER(hashBuffer, hash);

    for (int32 i = 0; i < 4; ++i)
        for (int32 j = 0; j < 4; ++j) info->encryptionKeyA[i * 4 + j] = (hash[i] >> (8 * (j ^ 3))) & 0xFF;
#else
    GEN_HASH_MD5_BUFFER(hashBuffer, (uint32 *)hash);

    for (int32 y = 0; y < 0x10; y += 4) {
        info->encryptionKeyA[y + 3] = hash[y + 0];
        info->encryptionKeyA[y + 2] = hash[y + 1];
        info->encryptionKeyA[y + 1] = hash[y + 2];
        info->encryptionKeyA[y + 0] = hash[y + 3];
    }
#endif

    // KeyB
    sprintf_s(hashBuffer, sizeof(hashBuffer), "%d", key2);
#if !RETRO_USE_ORIGINAL_CODE
    GEN_HASH_MD5_BUFFER(hashBuffer, hash);

    for (int32 i = 0; i < 4; ++i)
        for (int32 j = 0; j < 4; ++j) info->encryptionKeyB[i * 4 + j] = (hash[i] >> (8 * (j ^ 3))) & 0xFF;
#else
    GEN_HASH_MD5_BUFFER(hashBuffer, (uint32 *)hash);

    for (int32 y = 0; y < 0x10; y += 4) {
        info->encryptionKeyB[y + 3] = hash[y + 0];
        info->encryptionKeyB[y + 2] = hash[y + 1];
        info->encryptionKeyB[y + 1] = hash[y + 2];
        info->encryptionKeyB[y + 0] = hash[y + 3];
    }
#endif
}

#if RETRO_PLATFORM == RETRO_PSP
// The key schedule shared by DecryptBytes and SkipBytes, run on locals: the originals read and wrote every field through
// info for each byte, and every sfx, music track and sprite sheet goes through here (megabytes per scene load).
#define PSP_ADVANCE_KEY()                                                                                                                      do {                                                                                                                                           ++posA;                                                                                                                                    ++posB;                                                                                                                                    if (posA <= 15) {                                                                                                                              if (posB > 12) {                                                                                                                               posB = 0;                                                                                                                                  swap ^= 1;                                                                                                                             }                                                                                                                                      }                                                                                                                                          else if (posB <= 8) {                                                                                                                          posA = 0;                                                                                                                                  swap ^= 1;                                                                                                                             }                                                                                                                                          else {                                                                                                                                         keyNo = (keyNo + 2) & 0x7F;                                                                                                                if (swap) {                                                                                                                                    swap = 0;                                                                                                                                  posA = keyNo % 7;                                                                                                                          posB = (keyNo % 12) + 2;                                                                                                               }                                                                                                                                          else {                                                                                                                                         swap = 1;                                                                                                                                  posA = (keyNo % 12) + 3;                                                                                                                   posB = keyNo % 7;                                                                                                                      }                                                                                                                                      }                                                                                                                                      } while (0)

// The key schedule (keyNo, posA, posB, swap) depends only on the starting keyNo, (fileSize / 4) & 0x7F, and after a
// short lead-in repeats with a period of ~2000 bytes (tables below, worked out offline). So per file the stream is
// "nybble-swap or not, then XOR with K" for each position of that cycle: built once (into a small cache keyed by the
// file's keys), large reads then cost a table lookup per byte instead of the branchy schedule. pspKeyIndex tracks
// the position in the cycle; the eKey* fields stay in step for the small reads that keep the original loop.
static const uint16 pspKeyPre[128] = { 0, 0, 0, 0, 0, 0, 0, 16, 16, 16, 16, 5, 48, 47, 5, 6, 16, 16, 16, 16, 16, 30, 143, 16, 82, 0, 23, 0, 16, 16, 16, 16, 16, 16, 16, 163, 16, 117, 16, 73, 16, 31, 0, 16, 2, 3, 4, 16, 47, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 0, 0, 0, 0, 0, 0, 0, 163, 120, 117, 75, 73, 32, 31, 0, 0, 16, 16, 16, 16, 16, 0, 0, 0, 0, 0, 0, 0, 16, 16, 16, 16, 5, 48, 47, 5, 6, 16, 16, 16, 16, 16, 30, 143, 16, 82, 0, 23, 0, 16, 16, 16, 16, 16, 16, 16, 163, 16, 117, 16, 73, 16, 31, 0, 16 };
static const uint16 pspKeyPeriod[128] = { 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120, 1948, 2120 };

#define PSP_KEY_CYCLE_MAX (2288)
struct PSPKeyTable {
    uint8 keyA[0x10], keyB[0x10];
    int32 k0;  // starting keyNo, -1 = empty
    uint32 used;
    uint8 xorKey[PSP_KEY_CYCLE_MAX];
    uint8 swap[PSP_KEY_CYCLE_MAX];
    uint16 state[PSP_KEY_CYCLE_MAX]; // keyNo | posA << 7 | posB << 11 | swap << 15, at each position
};
static PSPKeyTable pspKeyTables[2] = { { {}, {}, -1 }, { {}, {}, -1 } };
static uint32 pspKeyClock;

static inline uint32 PSPKeyAdvance(uint32 index, uint32 count, int32 k0)
{
    uint32 end = pspKeyPre[k0] + pspKeyPeriod[k0];
    index += count;
    if (index >= end)
        index = pspKeyPre[k0] + (index - pspKeyPre[k0]) % pspKeyPeriod[k0];
    return index;
}

static PSPKeyTable *PSPGetKeyTable(FileInfo *info, int32 k0)
{
    PSPKeyTable *t = NULL;
    for (int32 i = 0; i < 2; ++i) {
        PSPKeyTable *c = &pspKeyTables[i];
        if (c->k0 == k0 && !memcmp(c->keyA, info->encryptionKeyA, 0x10) && !memcmp(c->keyB, info->encryptionKeyB, 0x10)) {
            c->used = ++pspKeyClock;
            return c;
        }
    }
    t = pspKeyTables[0].used <= pspKeyTables[1].used ? &pspKeyTables[0] : &pspKeyTables[1];
    memcpy(t->keyA, info->encryptionKeyA, 0x10);
    memcpy(t->keyB, info->encryptionKeyB, 0x10);
    t->k0   = k0;
    t->used = ++pspKeyClock;

    uint8 keyNo = (uint8)k0, posA = 0, posB = 8, swap = 0;
    uint32 end = pspKeyPre[k0] + pspKeyPeriod[k0];
    for (uint32 i = 0; i < end; ++i) {
        uint8 k1     = keyNo ^ info->encryptionKeyB[posB];
        t->swap[i]   = swap;
        t->xorKey[i] = (swap ? (uint8)((k1 << 4) | (k1 >> 4)) : k1) ^ info->encryptionKeyA[posA];
        t->state[i]  = keyNo | (posA << 7) | (posB << 11) | (swap << 15);
        PSP_ADVANCE_KEY();
    }
    return t;
}

static inline void PSPSetKeyState(FileInfo *info, uint16 state)
{
    info->eKeyNo      = state & 0x7F;
    info->eKeyPosA    = (state >> 7) & 0xF;
    info->eKeyPosB    = (state >> 11) & 0xF;
    info->eNybbleSwap = state >> 15;
}

void RSDK::DecryptBytes(FileInfo *info, void *buffer, size_t size)
{
    int32 k0    = (info->fileSize / 4) & 0x7F;
    uint8 *data = (uint8 *)buffer;
    if (size >= 64) {
        unsigned long long decryptStart = PSPProfNow();
        PSPKeyTable *t   = PSPGetKeyTable(info, k0);
        uint32 index     = info->pspKeyIndex;
        const uint32 end = pspKeyPre[k0] + pspKeyPeriod[k0], pre = pspKeyPre[k0];
        for (size_t i = 0; i < size; ++i) {
            uint8 value = data[i];
            if (t->swap[index])
                value = (uint8)((value << 4) | (value >> 4));
            data[i] = value ^ t->xorKey[index];
            if (++index == end)
                index = pre;
        }
        info->pspKeyIndex = (uint16)index;
        PSPSetKeyState(info, t->state[index]);
        pspLoadDecryptUS += PSPProfNow() - decryptStart;
        return;
    }

    uint8 keyNo = info->eKeyNo, posA = info->eKeyPosA, posB = info->eKeyPosB, swap = info->eNybbleSwap;
    const uint8 *keyA = info->encryptionKeyA;
    const uint8 *keyB = info->encryptionKeyB;

    uint8 *end = data + size;
    while (data < end) {
        uint8 value = *data ^ keyNo ^ keyB[posB];
        if (swap)
            value = (uint8)((value << 4) | (value >> 4));
        *data++ = value ^ keyA[posA];
        PSP_ADVANCE_KEY();
    }

    info->eKeyNo      = keyNo;
    info->eKeyPosA    = posA;
    info->eKeyPosB    = posB;
    info->eNybbleSwap = swap;
    info->pspKeyIndex = (uint16)PSPKeyAdvance(info->pspKeyIndex, (uint32)size, k0);
}

void RSDK::SkipBytes(FileInfo *info, int32 size)
{
    if (size <= 0)
        return;
    int32 k0     = (info->fileSize / 4) & 0x7F;
    uint32 index = PSPKeyAdvance(info->pspKeyIndex, (uint32)size, k0);
    if (size >= 64) {
        PSPKeyTable *t = PSPGetKeyTable(info, k0);
        info->pspKeyIndex = (uint16)index;
        PSPSetKeyState(info, t->state[index]);
        return;
    }

    uint8 keyNo = info->eKeyNo, posA = info->eKeyPosA, posB = info->eKeyPosB, swap = info->eNybbleSwap;
    for (; size > 0; --size) PSP_ADVANCE_KEY();

    info->eKeyNo      = keyNo;
    info->eKeyPosA    = posA;
    info->eKeyPosB    = posB;
    info->eNybbleSwap = swap;
    info->pspKeyIndex = (uint16)index;
}
// Startup check: the cycle tables against the original schedule (byte at a time) on a few made-up files.
void PSPDecryptSelfTest()
{
    static uint8 fast[5000], slow[5000];
    bool ok = true;
    for (int32 trial = 0; trial < 4 && ok; ++trial) {
        FileInfo a, b;
        memset(&a, 0, sizeof(a));
        a.fileSize = 5000 + trial * 1237;
        for (int32 i = 0; i < 0x10; ++i) {
            a.encryptionKeyA[i] = (uint8)(i * 37 + trial * 11);
            a.encryptionKeyB[i] = (uint8)(i * 91 + trial * 5 + 3);
        }
        a.eKeyNo   = (a.fileSize / 4) & 0x7F;
        a.eKeyPosB = 8;
        b          = a;
        for (int32 i = 0; i < 5000; ++i) fast[i] = slow[i] = (uint8)(i * 7 + trial);
        DecryptBytes(&a, fast, 1000);
        SkipBytes(&a, 777);
        DecryptBytes(&a, fast + 1777, 5000 - 1777);
        for (int32 i = 0; i < 1000; ++i) DecryptBytes(&b, &slow[i], 1);
        for (int32 i = 0; i < 777; ++i) SkipBytes(&b, 1);
        for (int32 i = 1777; i < 5000; ++i) DecryptBytes(&b, &slow[i], 1);
        ok = !memcmp(fast, slow, 1000) && !memcmp(fast + 1777, slow + 1777, 5000 - 1777) && a.eKeyNo == b.eKeyNo && a.eKeyPosA == b.eKeyPosA
             && a.eKeyPosB == b.eKeyPosB && a.eNybbleSwap == b.eNybbleSwap;
    }
    PrintLog(PRINT_NORMAL, "PSP decrypt self-test: %s", ok ? "ok" : "FAILED");
}
#undef PSP_ADVANCE_KEY
#else
void RSDK::DecryptBytes(FileInfo *info, void *buffer, size_t size)
{
    if (size) {
        uint8 *data = (uint8 *)buffer;
        while (size > 0) {
            *data ^= info->eKeyNo ^ info->encryptionKeyB[info->eKeyPosB];
            if (info->eNybbleSwap)
                *data = ((*data << 4) + (*data >> 4)) & 0xFF;
            *data ^= info->encryptionKeyA[info->eKeyPosA];

            info->eKeyPosA++;
            info->eKeyPosB++;

            if (info->eKeyPosA <= 15) {
                if (info->eKeyPosB > 12) {
                    info->eKeyPosB = 0;
                    info->eNybbleSwap ^= 1;
                }
            }
            else if (info->eKeyPosB <= 8) {
                info->eKeyPosA = 0;
                info->eNybbleSwap ^= 1;
            }
            else {
                info->eKeyNo += 2;
                info->eKeyNo &= 0x7F;

                if (info->eNybbleSwap) {
                    info->eNybbleSwap = false;

                    info->eKeyPosA = info->eKeyNo % 7;
                    info->eKeyPosB = (info->eKeyNo % 12) + 2;
                }
                else {
                    info->eNybbleSwap = true;

                    info->eKeyPosA = (info->eKeyNo % 12) + 3;
                    info->eKeyPosB = info->eKeyNo % 7;
                }
            }

            ++data;
            --size;
        }
    }
}
void RSDK::SkipBytes(FileInfo *info, int32 size)
{
    if (size) {
        while (size > 0) {
            info->eKeyPosA++;
            info->eKeyPosB++;

            if (info->eKeyPosA <= 15) {
                if (info->eKeyPosB > 12) {
                    info->eKeyPosB = 0;
                    info->eNybbleSwap ^= 1;
                }
            }
            else if (info->eKeyPosB <= 8) {
                info->eKeyPosA = 0;
                info->eNybbleSwap ^= 1;
            }
            else {
                info->eKeyNo += 2;
                info->eKeyNo &= 0x7F;

                if (info->eNybbleSwap) {
                    info->eNybbleSwap = false;

                    info->eKeyPosA = info->eKeyNo % 7;
                    info->eKeyPosB = (info->eKeyNo % 12) + 2;
                }
                else {
                    info->eNybbleSwap = true;

                    info->eKeyPosA = (info->eKeyNo % 12) + 3;
                    info->eKeyPosB = info->eKeyNo % 7;
                }
            }

            --size;
        }
    }
}
#endif
