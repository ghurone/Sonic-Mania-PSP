#include "RSDK/Core/RetroEngine.hpp"

using namespace RSDK;

#if RETRO_REV0U
#include "Legacy/ObjectLegacy.cpp"
#endif

ObjectClass RSDK::objectClassList[OBJECT_COUNT];
int32 RSDK::objectClassCount = 0;

int32 RSDK::globalObjectCount = 0;
int32 RSDK::globalObjectIDs[OBJECT_COUNT];
int32 RSDK::stageObjectIDs[OBJECT_COUNT];

EntityBase RSDK::objectEntityList[ENTITY_COUNT];
#if RETRO_PLATFORM == RETRO_PSP
uint8 RSDK::pspSlotFlags[ENTITY_COUNT];

// The pause menu (game side, PauseMenu_Draw) sets this every frame to its own draw group. While frozen under it, nothing
// below that group can change, so those groups are drawn once into pspFreezeFrame and restored from it afterwards,
// instead of redrawing the whole stage under the menu each frame (100ms+ per frame in the special stages).
// Other frozen states (deaths, cutscenes) keep animating below and never set it.
extern "C" {
int32 pspFreezeDrawGroup = -1;
}
static uint16 *pspFreezeFrame;
static bool32 pspFreezeValid;
static uint8 pspFreezeOnScreen[ENTITY_COUNT]; // onScreen as of the captured frame: objects read last frame's value

static void PSPReleaseFreezeFrame()
{
    if (pspFreezeFrame) {
        free(pspFreezeFrame);
        pspFreezeFrame = NULL;
    }
    pspFreezeValid = false;
}

// per-class update/draw time (us), indexed by scene class ID, plus tile layer drawing; reported with "PSP perf"
static unsigned long long pspClassUpd[OBJECT_COUNT];
static unsigned long long pspClassDraw[OBJECT_COUNT];
static unsigned long long pspLayerDraw;
// rotozoom shape (diagnostics): lines drawn and tiles each line crosses, bucketed
static uint32 pspRotoLines, pspRotoCrossings, pspRotoBuckets[5]; // <=8, <=16, <=32, <=64, more
static void PSPRotozoomStats(TileLayer *layer)
{
    int32 width = currentScreen->clipBound_X2 - currentScreen->clipBound_X1;
    for (int32 y = currentScreen->clipBound_Y1; y < currentScreen->clipBound_Y2; ++y) {
        ScanlineInfo *line = &scanlines[y];
        int32 x0 = line->position.x >> 20, y0 = line->position.y >> 20;
        int32 x1 = (line->position.x + line->deform.x * width) >> 20, y1 = (line->position.y + line->deform.y * width) >> 20;
        uint32 crossings = abs(x1 - x0) + abs(y1 - y0) + 1;
        pspRotoLines++;
        pspRotoCrossings += crossings;
        pspRotoBuckets[crossings <= 8 ? 0 : crossings <= 16 ? 1 : crossings <= 32 ? 2 : crossings <= 64 ? 3 : 4]++;
    }
}
static unsigned long long pspLayerPrep[LAYER_COUNT];
static unsigned long long pspLayerRaster[LAYER_COUNT];
static uint32 pspLayerCalls[LAYER_COUNT];

static void PSPPrintTopClasses(const char *label, unsigned long long *times, uint32 frames)
{
    char line[0x200];
    int32 len = sprintf(line, "PSP %s:", label);
    for (int32 n = 0; n < 5; ++n) {
        int32 best = -1;
        for (int32 c = 0; c < sceneInfo.classCount; ++c)
            if (times[c] && (best < 0 || times[c] > times[best]))
                best = c;
        if (best < 0)
            break;

        const char *name = objectClassList[stageObjectIDs[best]].name;
        len += sprintf(line + len, " %s=%u", name ? name : "?", (uint32)(times[best] / frames));
        times[best] = 0;
    }
    PrintLog(PRINT_NORMAL, "%s", line);
}

void PSPReportClassProfile(uint32 frames)
{
    if (!frames)
        frames = 1;
    PSPPrintTopClasses("upd", pspClassUpd, frames);
    PSPPrintTopClasses("draw", pspClassDraw, frames);
    PrintLog(PRINT_NORMAL, "PSP layers=%u", (uint32)(pspLayerDraw / frames));
    PrintLog(PRINT_NORMAL, "PSP layer profile: scene=%s/%s, prep/raster in us/frame", currentSceneFolder, currentSceneID);
    for (int32 i = 0; i < LAYER_COUNT; ++i) {
        if (!pspLayerCalls[i])
            continue;
        const char *type = "unknown";
        switch (tileLayers[i].type) {
            case LAYER_HSCROLL: type = "HScroll"; break;
            case LAYER_VSCROLL: type = "VScroll"; break;
            case LAYER_ROTOZOOM: type = "Rotozoom"; break;
            case LAYER_BASIC: type = "Basic"; break;
        }
        PrintLog(PRINT_NORMAL, "PSP layer[%d] %s: prep=%u raster=%u calls=%u", i, type,
                 (uint32)(pspLayerPrep[i] / frames), (uint32)(pspLayerRaster[i] / frames), pspLayerCalls[i]);
    }
    PSPReportTileDrawProfile();
    if (pspFacesDrawn || pspFacesCulled)
        PrintLog(PRINT_NORMAL, "PSP 3d faces/frame: drawn=%u culled=%u | us/frame add=%u sort=%u draw=%u | shaded px/frame=%u",
                 pspFacesDrawn / frames, pspFacesCulled / frames, (uint32)(pspS3DTime[0] / frames), (uint32)(pspS3DTime[1] / frames),
                 (uint32)(pspS3DTime[2] / frames), pspS3DPixels / frames);
    if (pspS3DCalls)
        PrintLog(PRINT_NORMAL, "PSP 3d records/frame: scenes=%u lit=%u reused=%u verts=%u | us total=%u light=%u queue=%u", pspS3DCalls / frames,
                 pspS3DModels / frames, pspS3DReused / frames, pspS3DVerts / frames, (uint32)(pspS3DDetail[0] / frames), (uint32)(pspS3DDetail[1] / frames),
                 (uint32)(pspS3DDetail[2] / frames));
    pspS3DCalls = pspS3DModels = pspS3DVerts = pspS3DReused = 0;
    PSPReportLitModels(frames);
    memset(pspS3DDetail, 0, sizeof(pspS3DDetail));
    if (pspRotoLines)
        PrintLog(PRINT_NORMAL, "PSP roto/frame: lines=%u tiles=%u | lines by tiles crossed <=8:%u <=16:%u <=32:%u <=64:%u more:%u", pspRotoLines / frames,
                 pspRotoCrossings / frames, pspRotoBuckets[0] / frames, pspRotoBuckets[1] / frames, pspRotoBuckets[2] / frames,
                 pspRotoBuckets[3] / frames, pspRotoBuckets[4] / frames);
    pspRotoLines = pspRotoCrossings = 0;
    memset(pspRotoBuckets, 0, sizeof(pspRotoBuckets));
    {
        extern unsigned int pspRotoGeRows, pspRotoCpuRows, pspRotoLayerW, pspRotoLayerH;
        if (pspRotoGeRows || pspRotoCpuRows)
            PrintLog(PRINT_NORMAL, "PSP roto rows/frame: ge=%u cpu=%u (layer %ux%u tiles)", pspRotoGeRows / frames, pspRotoCpuRows / frames,
                     pspRotoLayerW, pspRotoLayerH);
        pspRotoGeRows = pspRotoCpuRows = 0;
        extern unsigned int pspHorizonRowsSum[3], pspHorizonRowsMax[3], pspHorizonFrames;
        if (pspHorizonFrames)
            PrintLog(PRINT_NORMAL, "PSP horizon rows (avg/max): <2px %u/%u <3px %u/%u <4px %u/%u", pspHorizonRowsSum[0] / pspHorizonFrames,
                     pspHorizonRowsMax[0], pspHorizonRowsSum[1] / pspHorizonFrames, pspHorizonRowsMax[1], pspHorizonRowsSum[2] / pspHorizonFrames,
                     pspHorizonRowsMax[2]);
        memset(pspHorizonRowsSum, 0, sizeof(pspHorizonRowsSum));
        memset(pspHorizonRowsMax, 0, sizeof(pspHorizonRowsMax));
        pspHorizonFrames = 0;
    }
    if (pspGeSpriteCount)
        PrintLog(PRINT_NORMAL, "PSP sprites on GE/frame: %u (cache: copies=%u resets=%u)", pspGeSpriteCount / frames, pspSpriteCacheCopies,
                 pspSpriteCacheResets);
    pspGeSpriteCount = pspSpriteCacheCopies = pspSpriteCacheResets = 0;
    if (pspGeLayerSprites)
        PrintLog(PRINT_NORMAL, "PSP layers on GE: sprites/frame=%u (rotozoom: walk=%uus gewait=%uus runs=%u index rows=%u) cluts=%u in %uus",
                 pspGeLayerSprites / frames, (uint32)(pspGeRotoGenUS / frames), (uint32)(pspGeRotoFlushUS / frames), pspRotoWalkRuns / frames,
                 pspRotoIndexRows / frames, pspGeClutCount / frames, (uint32)(pspGeClutUS / frames));
    {
        extern unsigned long long pspRotoIndexUS, pspRotoBlankUS, pspGenItemUS, pspGenInitUS;
        PrintLog(PRINT_NORMAL, "PSP roto parts/frame: index rows %uus, blankmap %uus, ge rows %uus (%u rows)", (uint32)(pspRotoIndexUS / frames),
                 (uint32)(pspRotoBlankUS / frames), (uint32)(pspGenItemUS / frames), (uint32)(pspGenInitUS / frames));
        pspRotoIndexUS = pspRotoBlankUS = pspGenItemUS = pspGenInitUS = 0;
        extern unsigned int pspRotoFlatCount;
        if (pspRotoFlatCount)
            PrintLog(PRINT_NORMAL, "PSP roto rows without rotation/frame: %u (one line each)", pspRotoFlatCount / frames);
        pspRotoFlatCount = 0;
        extern unsigned int pspRotoPageRows, pspRotoPageLines, pspRotoPageCopies;
        if (pspRotoPageRows)
            PrintLog(PRINT_NORMAL, "PSP roto pages/frame: rows=%u lines=%u tile copies=%u.%02u", pspRotoPageRows / frames, pspRotoPageLines / frames,
                     pspRotoPageCopies / frames, pspRotoPageCopies * 100 / frames % 100);
        pspRotoPageRows = pspRotoPageLines = pspRotoPageCopies = 0;
    }
    pspRotoIndexRows = 0;
    PrintLog(PRINT_NORMAL, "PSP ge kicks/frame: idle=%u.%02u busy=%u.%02u", pspGeIdleKicks / frames, pspGeIdleKicks * 100 / frames % 100,
             pspGeBusySkips / frames, pspGeBusySkips * 100 / frames % 100);
    pspGeIdleKicks = pspGeBusySkips = 0;
    {
        extern unsigned int pspGeStateSeen[8];
        PrintLog(PRINT_NORMAL, "PSP ge states seen: %u %u %u %u %u", pspGeStateSeen[0], pspGeStateSeen[1], pspGeStateSeen[2], pspGeStateSeen[3],
                 pspGeStateSeen[4]);
        memset(pspGeStateSeen, 0, sizeof(pspGeStateSeen));
    }
    {
        extern unsigned int pspSpriteBankRefusals;
        PrintLog(PRINT_NORMAL, "PSP sprites refused for mixed palette banks: %u", pspSpriteBankRefusals);
        pspSpriteBankRefusals = 0;
    }
    pspRotoWalkRuns = pspGeClutCount = 0;
    pspGeClutUS       = 0;
    pspGeLayerSprites = 0;
    {
        extern uint32 pspVScrollBands;
        extern unsigned long long pspDeformWaitUS, pspDeformLoopUS;
        if (pspDeformWaitUS || pspDeformLoopUS)
            PrintLog(PRINT_NORMAL, "PSP deformed sprites in software: GE wait=%uus draw=%uus per frame", (uint32)(pspDeformWaitUS / frames),
                     (uint32)(pspDeformLoopUS / frames));
        pspDeformWaitUS = pspDeformLoopUS = 0;
        if (pspVScrollBands)
            PrintLog(PRINT_NORMAL, "PSP vscroll on GE: bands/frame=%u", pspVScrollBands / frames);
        pspVScrollBands = 0;
    }
    pspGeRotoGenUS = pspGeRotoFlushUS = 0;
    if (psp3DFlushes)
        PrintLog(PRINT_NORMAL, "PSP 3d on GE/frame: flushes=%u triangles=%u us=%u (wb=%u build=%u draw=%u copyback=%u) px=%u", psp3DFlushes / frames,
                 psp3DTriangles / frames, (uint32)(psp3DFlushUS / frames), (uint32)(psp3DFlushParts[0] / frames), (uint32)(psp3DFlushParts[1] / frames),
                 (uint32)(psp3DFlushParts[2] / frames), (uint32)(psp3DFlushParts[3] / frames), psp3DCopiedPixels / frames);
    {
        extern unsigned int psp3DItemTypes[8];
        if (psp3DFlushes)
            PrintLog(PRINT_NORMAL, "PSP 3d items/frame x10: faces=%u models=%u layers=%u sprites=%u masks=%u fills=%u", psp3DItemTypes[0] * 10 / frames,
                     psp3DItemTypes[1] * 10 / frames, psp3DItemTypes[2] * 10 / frames, psp3DItemTypes[3] * 10 / frames,
                     psp3DItemTypes[4] * 10 / frames, psp3DItemTypes[5] * 10 / frames);
        memset(psp3DItemTypes, 0, sizeof(psp3DItemTypes));
    }
    memset(psp3DFlushParts, 0, sizeof(psp3DFlushParts));
    psp3DCopiedPixels = 0;
    psp3DFlushes   = 0;
    psp3DTriangles = 0;
    psp3DFlushUS   = 0;
    pspFacesDrawn  = 0;
    pspFacesCulled = 0;
    pspS3DPixels   = 0;
    memset(pspS3DTime, 0, sizeof(pspS3DTime));
    memset(pspClassUpd, 0, sizeof(pspClassUpd));
    memset(pspClassDraw, 0, sizeof(pspClassDraw));
    memset(pspLayerPrep, 0, sizeof(pspLayerPrep));
    memset(pspLayerRaster, 0, sizeof(pspLayerRaster));
    memset(pspLayerCalls, 0, sizeof(pspLayerCalls));
    pspLayerDraw = 0;
}
#endif

EditableVarInfo *RSDK::editableVarList;
int32 RSDK::editableVarCount = 0;

TypeGroupList RSDK::typeGroups[TYPEGROUP_COUNT];

bool32 RSDK::validDraw = false;

ForeachStackInfo RSDK::foreachStackList[FOREACH_STACK_COUNT];
ForeachStackInfo *RSDK::foreachStackPtr = NULL;

#if RETRO_REV0U
#if RETRO_USE_MOD_LOADER
void RSDK::RegisterObject(Object **staticVars, const char *name, uint32 entityClassSize, uint32 staticClassSize, void (*update)(),
                          void (*lateUpdate)(), void (*staticUpdate)(), void (*draw)(), void (*create)(void *), void (*stageLoad)(),
                          void (*editorLoad)(), void (*editorDraw)(), void (*serialize)(), void (*staticLoad)(Object *))
{
    return RegisterObject_STD(staticVars, name, entityClassSize, staticClassSize, update, lateUpdate, staticUpdate, draw, create, stageLoad,
                              editorLoad, editorDraw, serialize, staticLoad);
}

void RSDK::RegisterObject_STD(Object **staticVars, const char *name, uint32 entityClassSize, uint32 staticClassSize, std::function<void()> update,
                              std::function<void()> lateUpdate, std::function<void()> staticUpdate, std::function<void()> draw,
                              std::function<void(void *)> create, std::function<void()> stageLoad, std::function<void()> editorLoad,
                              std::function<void()> editorDraw, std::function<void()> serialize, std::function<void(Object *)> staticLoad)
#else
void RSDK::RegisterObject(Object **staticVars, const char *name, uint32 entityClassSize, uint32 staticClassSize, void (*update)(),
                          void (*lateUpdate)(), void (*staticUpdate)(), void (*draw)(), void (*create)(void *), void (*stageLoad)(),
                          void (*editorLoad)(), void (*editorDraw)(), void (*serialize)(), void (*staticLoad)(Object *))
#endif
#else
#if RETRO_USE_MOD_LOADER
void RSDK::RegisterObject(Object **staticVars, const char *name, uint32 entityClassSize, uint32 staticClassSize, void (*update)(),
                          void (*lateUpdate)(), void (*staticUpdate)(), void (*draw)(), void (*create)(void *), void (*stageLoad)(),
                          void (*editorLoad)(), void (*editorDraw)(), void (*serialize)())
{
    return RegisterObject_STD(staticVars, name, entityClassSize, staticClassSize, update, lateUpdate, staticUpdate, draw, create, stageLoad,
                              editorLoad, editorDraw, serialize);
}

void RSDK::RegisterObject_STD(Object **staticVars, const char *name, uint32 entityClassSize, uint32 staticClassSize, std::function<void()> update,
                              std::function<void()> lateUpdate, std::function<void()> staticUpdate, std::function<void()> draw,
                              std::function<void(void *)> create, std::function<void()> stageLoad, std::function<void()> editorLoad,
                              std::function<void()> editorDraw, std::function<void()> serialize)
#else
void RSDK::RegisterObject(Object **staticVars, const char *name, uint32 entityClassSize, uint32 staticClassSize, void (*update)(),
                          void (*lateUpdate)(), void (*staticUpdate)(), void (*draw)(), void (*create)(void *), void (*stageLoad)(),
                          void (*editorLoad)(), void (*editorDraw)(), void (*serialize)())
#endif
#endif
{
#if RETRO_PLATFORM == RETRO_PSP
    if (objectClassCount % 100 == 0)
        PrintLog(PRINT_NORMAL, "PSPINIT: RegisterObject #%d %s", objectClassCount, name);
#endif
    if (objectClassCount < OBJECT_COUNT) {
        if (entityClassSize > sizeof(EntityBase))
            PrintLog(PRINT_NORMAL, "Class exceeds max entity memory: %s", name);

        ObjectClass *classInfo = &objectClassList[objectClassCount];
        GEN_HASH_MD5(name, classInfo->hash);
        classInfo->staticVars      = staticVars;
        classInfo->entityClassSize = entityClassSize;
        classInfo->staticClassSize = staticClassSize;
        classInfo->update          = update;
        classInfo->lateUpdate      = lateUpdate;
        classInfo->staticUpdate    = staticUpdate;
        classInfo->draw            = draw;
        classInfo->create          = create;
        classInfo->stageLoad       = stageLoad;
        classInfo->editorLoad      = editorLoad;
        classInfo->editorDraw      = editorDraw;
        classInfo->serialize       = serialize;
#if RETRO_REV0U
        classInfo->staticLoad = staticLoad;
#endif

#if !RETRO_USE_ORIGINAL_CODE
        classInfo->name = name;
#endif

        ++objectClassCount;
    }
}

#if RETRO_REV02 || RETRO_USE_MOD_LOADER
void RSDK::RegisterStaticVariables(void **staticVars, const char *name, uint32 classSize)
{
    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5(name, hash);
    AllocateStorage((void **)staticVars, classSize, DATASET_STG, true);
    LoadStaticVariables((uint8 *)*staticVars, hash, 0);
}
#endif

#define ALIGN_TO(type)                                                                                                                               \
    aligned = dataPos & -(int32)sizeof(type);                                                                                                        \
    if (aligned < dataPos)                                                                                                                           \
        dataPos = aligned + sizeof(type);

void RSDK::LoadStaticVariables(uint8 *classPtr, uint32 *hash, int32 readOffset)
{
#if RETRO_PLATFORM == RETRO_PSP
    PSPLoadTimer loadTimer(PSPLOAD_STATIC);
#endif
    char fullFilePath[0x40];

    const char *hexChars = "0123456789ABCDEF";
    char classHash[]     = "00000000000000000000000000000000";

    int32 strPos = 0;
    for (int32 i = 0; i < 32; i += 4) classHash[strPos++] = hexChars[(hash[0] >> i) & 0xF];
    for (int32 i = 0; i < 32; i += 4) classHash[strPos++] = hexChars[(hash[1] >> i) & 0xF];
    for (int32 i = 0; i < 32; i += 4) classHash[strPos++] = hexChars[(hash[2] >> i) & 0xF];
    for (int32 i = 0; i < 32; i += 4) classHash[strPos++] = hexChars[(hash[3] >> i) & 0xF];

    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/Objects/Static/%s.bin", classHash);

    FileInfo info;
    InitFileInfo(&info);
    if (LoadFile(&info, fullFilePath, FMODE_RB)) {
        uint32 sig = ReadInt32(&info, false);

        if (sig != RSDK_SIGNATURE_OBJ) {
            CloseFile(&info);
            return;
        }

        int32 dataPos = readOffset;
        while (info.readPos < info.fileSize) {
            int32 type      = ReadInt8(&info);
            int32 arraySize = ReadInt32(&info, false);

            bool32 hasValues = (type & 0x80) != 0;
            type &= 0x7F;

            int32 aligned = 0;
            if (hasValues) {
                uint32 count = ReadInt32(&info, false);

                switch (type) {
                    default:
#if !RETRO_USE_ORIGINAL_CODE
                        PrintLog(PRINT_NORMAL, "Invalid static variable type: %d", type);
#endif
                        break;

                    case SVAR_UINT8:
                    case SVAR_INT8:
                        if (info.readPos + (count * sizeof(uint8)) <= info.fileSize && &classPtr[dataPos]) {
                            for (int32 i = 0; i < count * sizeof(uint8); i += sizeof(uint8)) ReadBytes(&info, &classPtr[dataPos + i], sizeof(uint8));
                        }
                        else {
                            info.readPos += count * sizeof(uint8);
                        }

                        dataPos += count * sizeof(uint8);
                        break;

                    case SVAR_UINT16:
                    case SVAR_INT16: {
                        ALIGN_TO(int16);

                        if (info.readPos + (count * sizeof(int16)) <= info.fileSize && &classPtr[dataPos]) {
                            for (int32 i = 0; i < count * sizeof(int16); i += sizeof(int16)) {
#if !RETRO_USE_ORIGINAL_CODE
                                *(int16 *)&classPtr[dataPos + i] = ReadInt16(&info);
#else
                                // This only works as intended on little-endian CPUs.
                                ReadBytes(&info, &classPtr[dataPos + i], sizeof(int16));
#endif
                            }
                        }
                        else {
                            info.readPos += count * sizeof(int16);
                        }

                        dataPos += sizeof(int16) * count;
                        break;
                    }

                    case SVAR_UINT32:
                    case SVAR_INT32: {
                        ALIGN_TO(int32);

                        if (info.readPos + (count * sizeof(int32)) <= info.fileSize && &classPtr[dataPos]) {
                            for (int32 i = 0; i < count * sizeof(int32); i += sizeof(int32)) {
#if !RETRO_USE_ORIGINAL_CODE
                                *(int32 *)&classPtr[dataPos + i] = ReadInt32(&info, false);
#else
                                // This only works as intended on little-endian CPUs.
                                ReadBytes(&info, &classPtr[dataPos + i], sizeof(int32));
#endif
                            }
                        }
                        else {
                            info.readPos += count * sizeof(int32);
                        }

                        dataPos += sizeof(int32) * count;
                        break;
                    }

                    case SVAR_BOOL: {
                        ALIGN_TO(bool32);

                        if (info.readPos + (count * sizeof(bool32)) <= info.fileSize && &classPtr[dataPos]) {
                            for (int32 i = 0; i < count * sizeof(bool32); i += sizeof(bool32)) {
#if !RETRO_USE_ORIGINAL_CODE
                                *(bool32 *)&classPtr[dataPos + i] = (bool32)ReadInt32(&info, false);
#else
                                // This only works as intended on little-endian CPUs.
                                ReadBytes(&info, &classPtr[dataPos + i], sizeof(bool32));
#endif
                            }
                        }
                        else {
                            info.readPos += count * sizeof(bool32);
                        }

                        dataPos += sizeof(bool32) * count;
                        break;
                    }
                }
            }
            else {
                switch (type) {
                    case SVAR_UINT8:
                    case SVAR_INT8: dataPos += sizeof(uint8) * arraySize; break;

                    case SVAR_UINT16:
                    case SVAR_INT16:
                        ALIGN_TO(int16);

                        dataPos += sizeof(int16) * arraySize;
                        break;

                    case SVAR_UINT32:
                    case SVAR_INT32:
                        ALIGN_TO(int32);

                        dataPos += sizeof(int32) * arraySize;
                        break;

                    case SVAR_BOOL:
                        ALIGN_TO(bool32);

                        dataPos += sizeof(bool32) * arraySize;
                        break;

                    case SVAR_POINTER:
                        ALIGN_TO(void *);

                        dataPos += sizeof(void *) * arraySize;
                        break;

                    case SVAR_VECTOR2:
                        ALIGN_TO(int32);

                        dataPos += sizeof(Vector2) * arraySize;
                        break;

                    case SVAR_STRING:
                        ALIGN_TO(void *);

                        dataPos += sizeof(String) * arraySize;
                        break;

                    case SVAR_ANIMATOR:
                        ALIGN_TO(void *);

                        dataPos += sizeof(Animator) * arraySize;
                        break;

                    case SVAR_HITBOX:
                        ALIGN_TO(int16);

                        dataPos += sizeof(Hitbox) * arraySize;
                        break;

                    case SVAR_SPRITEFRAME:
                        ALIGN_TO(int16);

                        dataPos += sizeof(GameSpriteFrame) * arraySize;
                        break;

                    default:
#if !RETRO_USE_ORIGINAL_CODE
                        PrintLog(PRINT_NORMAL, "Invalid data type: %d", type);
#endif
                        break;
                }
            }
        }

        CloseFile(&info);
    }
}

void RSDK::InitObjects()
{
    sceneInfo.entitySlot = 0;
    sceneInfo.createSlot = ENTITY_COUNT - 0x100;
    cameraCount          = 0;

#if RETRO_PLATFORM == RETRO_PSP
    // load profile: each class's StageLoad (with the sheets/sounds it loads), then the entities' Create, in us
    static unsigned long long classLoad[OBJECT_COUNT];
    memset(classLoad, 0, sizeof(classLoad));
#endif
    for (int32 o = 0; o < sceneInfo.classCount; ++o) {
#if RETRO_USE_MOD_LOADER
        currentObjectID = o;
#endif

#if RETRO_PLATFORM == RETRO_PSP
        unsigned long long start = PSPProfNow();
#endif
        if (objectClassList[stageObjectIDs[o]].stageLoad)
            objectClassList[stageObjectIDs[o]].stageLoad();
#if RETRO_PLATFORM == RETRO_PSP
        classLoad[o] = PSPProfNow() - start;
#endif
    }
#if RETRO_PLATFORM == RETRO_PSP
    PSPPrintTopClasses("stageload us", classLoad, 1);
    unsigned long long createStart = PSPProfNow();
    memset(classLoad, 0, sizeof(classLoad));
#endif

#if RETRO_USE_MOD_LOADER
    RunModCallbacks(MODCB_ONSTAGELOAD, NULL);
#endif

    for (int32 e = 0; e < ENTITY_COUNT; ++e) {
        sceneInfo.entitySlot = e;
        sceneInfo.entity     = &objectEntityList[e];

        if (sceneInfo.entity->classID) {
            if (objectClassList[stageObjectIDs[sceneInfo.entity->classID]].create) {
                sceneInfo.entity->interaction = true;
#if RETRO_PLATFORM == RETRO_PSP
                unsigned long long start = PSPProfNow();
                int32 classID            = sceneInfo.entity->classID;
#endif
                objectClassList[stageObjectIDs[sceneInfo.entity->classID]].create(NULL);
#if RETRO_PLATFORM == RETRO_PSP
                classLoad[classID] += PSPProfNow() - start;
#endif
            }
        }
    }
#if RETRO_PLATFORM == RETRO_PSP
    PrintLog(PRINT_NORMAL, "PSP create: %uus total", (uint32)(PSPProfNow() - createStart));
    PSPPrintTopClasses("create us", classLoad, 1);
#endif

    sceneInfo.state = ENGINESTATE_REGULAR;

    if (!cameraCount)
        AddCamera(&screens[0].position, TO_FIXED(screens[0].center.x), TO_FIXED(screens[0].center.y), false);
}
void RSDK::ProcessObjects()
{
    for (int32 i = 0; i < DRAWGROUP_COUNT; ++i) drawGroups[i].entityCount = 0;

    for (int32 o = 0; o < sceneInfo.classCount; ++o) {
#if RETRO_USE_MOD_LOADER
        currentObjectID = o;
#endif

        ObjectClass *classInfo = &objectClassList[stageObjectIDs[o]];
        if ((*classInfo->staticVars)->active == ACTIVE_ALWAYS || (*classInfo->staticVars)->active == ACTIVE_NORMAL) {
            if (classInfo->staticUpdate)
                classInfo->staticUpdate();
        }
    }

#if RETRO_USE_MOD_LOADER
    RunModCallbacks(MODCB_ONSTATICUPDATE, INT_TO_VOID(ENGINESTATE_REGULAR));
#endif

    for (int32 s = 0; s < cameraCount; ++s) {
        CameraInfo *camera = &cameras[s];

        if (camera->targetPos) {
            if (camera->worldRelative) {
                camera->position.x = camera->targetPos->x;
                camera->position.y = camera->targetPos->y;
            }
            else {
                camera->position.x = TO_FIXED(camera->targetPos->x);
                camera->position.y = TO_FIXED(camera->targetPos->y);
            }
        }
    }

    sceneInfo.entitySlot = 0;
    for (int32 e = 0; e < ENTITY_COUNT; ++e) {
        sceneInfo.entity = &objectEntityList[e];
        if (sceneInfo.entity->classID) {
            switch (sceneInfo.entity->active) {
                default:
                case ACTIVE_DISABLED: break;

                case ACTIVE_NEVER:
                case ACTIVE_PAUSED: sceneInfo.entity->inRange = false; break;

                case ACTIVE_ALWAYS:
                case ACTIVE_NORMAL: sceneInfo.entity->inRange = true; break;

                case ACTIVE_BOUNDS:
                    sceneInfo.entity->inRange = false;

                    for (int32 s = 0; s < cameraCount; ++s) {
                        int32 sx = abs(sceneInfo.entity->position.x - cameras[s].position.x);
                        int32 sy = abs(sceneInfo.entity->position.y - cameras[s].position.y);

                        if (sx <= sceneInfo.entity->updateRange.x + cameras[s].offset.x
                            && sy <= sceneInfo.entity->updateRange.y + cameras[s].offset.y) {
                            sceneInfo.entity->inRange = true;
                            break;
                        }
                    }
                    break;

                case ACTIVE_XBOUNDS:
                    sceneInfo.entity->inRange = false;

                    for (int32 s = 0; s < cameraCount; ++s) {
                        int32 sx = abs(sceneInfo.entity->position.x - cameras[s].position.x);

                        if (sx <= sceneInfo.entity->updateRange.x + cameras[s].offset.x) {
                            sceneInfo.entity->inRange = true;
                            break;
                        }
                    }
                    break;

                case ACTIVE_YBOUNDS:
                    sceneInfo.entity->inRange = false;

                    for (int32 s = 0; s < cameraCount; ++s) {
                        int32 sy = abs(sceneInfo.entity->position.y - cameras[s].position.y);

                        if (sy <= sceneInfo.entity->updateRange.y + cameras[s].offset.y) {
                            sceneInfo.entity->inRange = true;
                            break;
                        }
                    }
                    break;

                case ACTIVE_RBOUNDS:
                    sceneInfo.entity->inRange = false;

                    for (int32 s = 0; s < cameraCount; ++s) {
                        int32 sx = FROM_FIXED(abs(sceneInfo.entity->position.x - cameras[s].position.x));
                        int32 sy = FROM_FIXED(abs(sceneInfo.entity->position.y - cameras[s].position.y));

                        if (sx * sx + sy * sy <= sceneInfo.entity->updateRange.x + cameras[s].offset.x) {
                            sceneInfo.entity->inRange = true;
                            break;
                        }
                    }
                    break;
            }

#if RETRO_PLATFORM == RETRO_PSP
            // candidate for the typeGroups/lateUpdate passes below (they re-check inRange, as the originals do)
            pspSlotFlags[e] = (pspSlotFlags[e] & PSP_SLOT_ONSCREEN) | (sceneInfo.entity->inRange ? PSP_SLOT_INRANGE : 0);
#endif
            if (sceneInfo.entity->inRange) {
                if (objectClassList[stageObjectIDs[sceneInfo.entity->classID]].update) {
#if RETRO_PLATFORM == RETRO_PSP
                    uint16 profClass              = sceneInfo.entity->classID;
                    unsigned long long profStart  = PSPProfFine();
                    objectClassList[stageObjectIDs[sceneInfo.entity->classID]].update();
                    pspClassUpd[profClass]       += PSPProfFine() - profStart;
#else
                    objectClassList[stageObjectIDs[sceneInfo.entity->classID]].update();
#endif
                }

                if (sceneInfo.entity->drawGroup < DRAWGROUP_COUNT)
                    drawGroups[sceneInfo.entity->drawGroup].entries[drawGroups[sceneInfo.entity->drawGroup].entityCount++] = sceneInfo.entitySlot;
            }
        }
        else {
#if RETRO_PLATFORM == RETRO_PSP
            // avoid dirtying (and later writing back) the cache line of every empty slot
            if (sceneInfo.entity->inRange)
                sceneInfo.entity->inRange = false;
            pspSlotFlags[e] &= PSP_SLOT_ONSCREEN;
#else
            sceneInfo.entity->inRange = false;
#endif
        }

        sceneInfo.entitySlot++;
    }

#if RETRO_USE_MOD_LOADER
    RunModCallbacks(MODCB_ONUPDATE, INT_TO_VOID(ENGINESTATE_REGULAR));
#endif

    for (int32 i = 0; i < TYPEGROUP_COUNT; ++i) typeGroups[i].entryCount = 0;

#if RETRO_PLATFORM == RETRO_PSP
    // Same order and conditions as below, but only entities flagged by the first pass (or copied in-range since)
    // are touched; everything else is known to be out of range.
    for (int32 e = 0; e < ENTITY_COUNT; ++e) {
        if (!(pspSlotFlags[e] & PSP_SLOT_INRANGE))
            continue;

        sceneInfo.entity = &objectEntityList[e];
        if (sceneInfo.entity->inRange && sceneInfo.entity->interaction) {
            typeGroups[GROUP_ALL].entries[typeGroups[GROUP_ALL].entryCount++] = e; // All active objects

            typeGroups[sceneInfo.entity->classID].entries[typeGroups[sceneInfo.entity->classID].entryCount++] = e; // class-based groups

            if (sceneInfo.entity->group >= TYPE_COUNT)
                typeGroups[sceneInfo.entity->group].entries[typeGroups[sceneInfo.entity->group].entryCount++] = e; // extra groups
        }
    }

    // flags are re-read per slot so entities copied in-range by an earlier lateUpdate are still visited
    for (int32 e = 0; e < ENTITY_COUNT; ++e) {
        uint8 flags = pspSlotFlags[e];
        if (!flags)
            continue;

        sceneInfo.entity     = &objectEntityList[e];
        sceneInfo.entitySlot = e;
        if ((flags & PSP_SLOT_INRANGE) && sceneInfo.entity->inRange) {
            if (objectClassList[stageObjectIDs[sceneInfo.entity->classID]].lateUpdate)
                objectClassList[stageObjectIDs[sceneInfo.entity->classID]].lateUpdate();
        }

        sceneInfo.entity->onScreen = 0;
        pspSlotFlags[e] &= PSP_SLOT_INRANGE;
    }
    sceneInfo.entity     = &objectEntityList[ENTITY_COUNT - 1];
    sceneInfo.entitySlot = ENTITY_COUNT;
#else
    sceneInfo.entitySlot = 0;
    for (int32 e = 0; e < ENTITY_COUNT; ++e) {
        sceneInfo.entity = &objectEntityList[e];

        if (sceneInfo.entity->inRange && sceneInfo.entity->interaction) {
            typeGroups[GROUP_ALL].entries[typeGroups[GROUP_ALL].entryCount++] = e; // All active objects

            typeGroups[sceneInfo.entity->classID].entries[typeGroups[sceneInfo.entity->classID].entryCount++] = e; // class-based groups

            if (sceneInfo.entity->group >= TYPE_COUNT)
                typeGroups[sceneInfo.entity->group].entries[typeGroups[sceneInfo.entity->group].entryCount++] = e; // extra groups
        }

        sceneInfo.entitySlot++;
    }

    sceneInfo.entitySlot = 0;
    for (int32 e = 0; e < ENTITY_COUNT; ++e) {
        sceneInfo.entity = &objectEntityList[e];

        if (sceneInfo.entity->inRange) {
            if (objectClassList[stageObjectIDs[sceneInfo.entity->classID]].lateUpdate)
                objectClassList[stageObjectIDs[sceneInfo.entity->classID]].lateUpdate();
        }

        sceneInfo.entity->onScreen = 0;
        sceneInfo.entitySlot++;
    }
#endif

#if RETRO_USE_MOD_LOADER
    RunModCallbacks(MODCB_ONLATEUPDATE, INT_TO_VOID(ENGINESTATE_REGULAR));
#endif
}
void RSDK::ProcessPausedObjects()
{
    for (int32 i = 0; i < DRAWGROUP_COUNT; ++i) drawGroups[i].entityCount = 0;

    for (int32 o = 0; o < sceneInfo.classCount; ++o) {
#if RETRO_USE_MOD_LOADER
        currentObjectID = o;
#endif

        ObjectClass *classInfo = &objectClassList[stageObjectIDs[o]];
        if ((*classInfo->staticVars)->active == ACTIVE_ALWAYS || (*classInfo->staticVars)->active == ACTIVE_PAUSED) {
            if (classInfo->staticUpdate)
                classInfo->staticUpdate();
        }
    }

#if RETRO_USE_MOD_LOADER
    RunModCallbacks(MODCB_ONSTATICUPDATE, INT_TO_VOID(ENGINESTATE_PAUSED));
#endif

    sceneInfo.entitySlot = 0;
    for (int32 e = 0; e < ENTITY_COUNT; ++e) {
        sceneInfo.entity = &objectEntityList[e];

        if (sceneInfo.entity->classID) {
            if (sceneInfo.entity->active == ACTIVE_ALWAYS || sceneInfo.entity->active == ACTIVE_PAUSED) {
                if (objectClassList[stageObjectIDs[sceneInfo.entity->classID]].update)
                    objectClassList[stageObjectIDs[sceneInfo.entity->classID]].update();

                if (sceneInfo.entity->drawGroup < DRAWGROUP_COUNT)
                    drawGroups[sceneInfo.entity->drawGroup].entries[drawGroups[sceneInfo.entity->drawGroup].entityCount++] = sceneInfo.entitySlot;
            }
        }
        else {
            sceneInfo.entity->inRange = false;
        }

        sceneInfo.entitySlot++;
    }

#if RETRO_USE_MOD_LOADER
    RunModCallbacks(MODCB_ONUPDATE, INT_TO_VOID(ENGINESTATE_PAUSED));
#endif

    sceneInfo.entitySlot = 0;
    for (int32 e = 0; e < ENTITY_COUNT; ++e) {
        sceneInfo.entity = &objectEntityList[e];

        if (sceneInfo.entity->active == ACTIVE_ALWAYS || sceneInfo.entity->active == ACTIVE_PAUSED) {
            if (objectClassList[stageObjectIDs[sceneInfo.entity->classID]].lateUpdate)
                objectClassList[stageObjectIDs[sceneInfo.entity->classID]].lateUpdate();
        }

        sceneInfo.entity->onScreen = 0;
        sceneInfo.entitySlot++;
    }

#if RETRO_USE_MOD_LOADER
    RunModCallbacks(MODCB_ONLATEUPDATE, INT_TO_VOID(ENGINESTATE_PAUSED));
#endif
}
void RSDK::ProcessFrozenObjects()
{
    for (int32 i = 0; i < DRAWGROUP_COUNT; ++i) drawGroups[i].entityCount = 0;

    for (int32 o = 0; o < sceneInfo.classCount; ++o) {
#if RETRO_USE_MOD_LOADER
        currentObjectID = o;
#endif

        ObjectClass *classInfo = &objectClassList[stageObjectIDs[o]];
        if ((*classInfo->staticVars)->active == ACTIVE_ALWAYS || (*classInfo->staticVars)->active == ACTIVE_PAUSED) {
            if (classInfo->staticUpdate)
                classInfo->staticUpdate();
        }
    }

#if RETRO_USE_MOD_LOADER
    RunModCallbacks(MODCB_ONSTATICUPDATE, INT_TO_VOID(ENGINESTATE_FROZEN));
#endif

    for (int32 s = 0; s < cameraCount; ++s) {
        CameraInfo *camera = &cameras[s];

        if (camera->targetPos) {
            if (camera->worldRelative) {
                camera->position.x = camera->targetPos->x;
                camera->position.y = camera->targetPos->y;
            }
            else {
                camera->position.x = TO_FIXED(camera->targetPos->x);
                camera->position.y = TO_FIXED(camera->targetPos->y);
            }
        }
    }

    sceneInfo.entitySlot = 0;
    for (int32 e = 0; e < ENTITY_COUNT; ++e) {
        sceneInfo.entity = &objectEntityList[e];

        if (sceneInfo.entity->classID) {
            switch (sceneInfo.entity->active) {
                default:
                case ACTIVE_DISABLED: break;

                case ACTIVE_NEVER:
                case ACTIVE_PAUSED: sceneInfo.entity->inRange = false; break;

                case ACTIVE_ALWAYS:
                case ACTIVE_NORMAL: sceneInfo.entity->inRange = true; break;

                case ACTIVE_BOUNDS:
                    sceneInfo.entity->inRange = false;

                    for (int32 s = 0; s < cameraCount; ++s) {
                        int32 sx = abs(sceneInfo.entity->position.x - cameras[s].position.x);
                        int32 sy = abs(sceneInfo.entity->position.y - cameras[s].position.y);

                        if (sx <= sceneInfo.entity->updateRange.x + cameras[s].offset.x
                            && sy <= sceneInfo.entity->updateRange.y + cameras[s].offset.y) {
                            sceneInfo.entity->inRange = true;
                            break;
                        }
                    }
                    break;

                case ACTIVE_XBOUNDS:
                    sceneInfo.entity->inRange = false;

                    for (int32 s = 0; s < cameraCount; ++s) {
                        int32 sx = abs(sceneInfo.entity->position.x - cameras[s].position.x);

                        if (sx <= sceneInfo.entity->updateRange.x + cameras[s].offset.x) {
                            sceneInfo.entity->inRange = true;
                            break;
                        }
                    }
                    break;

                case ACTIVE_YBOUNDS:
                    sceneInfo.entity->inRange = false;

                    for (int32 s = 0; s < cameraCount; ++s) {
                        int32 sy = abs(sceneInfo.entity->position.y - cameras[s].position.y);

                        if (sy <= sceneInfo.entity->updateRange.y + cameras[s].offset.y) {
                            sceneInfo.entity->inRange = true;
                            break;
                        }
                    }
                    break;

                case ACTIVE_RBOUNDS:
                    sceneInfo.entity->inRange = false;

                    for (int32 s = 0; s < cameraCount; ++s) {
                        int32 sx = FROM_FIXED(abs(sceneInfo.entity->position.x - cameras[s].position.x));
                        int32 sy = FROM_FIXED(abs(sceneInfo.entity->position.y - cameras[s].position.y));

                        if (sx * sx + sy * sy <= sceneInfo.entity->updateRange.x + cameras[s].offset.x) {
                            sceneInfo.entity->inRange = true;
                            break;
                        }
                    }
                    break;
            }

            if (sceneInfo.entity->inRange) {
                if (sceneInfo.entity->active == ACTIVE_ALWAYS || sceneInfo.entity->active == ACTIVE_PAUSED) {
                    if (objectClassList[stageObjectIDs[sceneInfo.entity->classID]].update)
                        objectClassList[stageObjectIDs[sceneInfo.entity->classID]].update();
                }

                if (sceneInfo.entity->drawGroup < DRAWGROUP_COUNT)
                    drawGroups[sceneInfo.entity->drawGroup].entries[drawGroups[sceneInfo.entity->drawGroup].entityCount++] = sceneInfo.entitySlot;
            }
        }
        else {
            sceneInfo.entity->inRange = false;
        }

        sceneInfo.entitySlot++;
    }

#if RETRO_USE_MOD_LOADER
    RunModCallbacks(MODCB_ONUPDATE, INT_TO_VOID(ENGINESTATE_FROZEN));
#endif

    for (int32 i = 0; i < TYPEGROUP_COUNT; ++i) typeGroups[i].entryCount = 0;

    sceneInfo.entitySlot = 0;
    for (int32 e = 0; e < ENTITY_COUNT; ++e) {
        sceneInfo.entity = &objectEntityList[e];

        if (sceneInfo.entity->inRange) {
            if (sceneInfo.entity->active == ACTIVE_ALWAYS || sceneInfo.entity->active == ACTIVE_PAUSED) {
                if (objectClassList[stageObjectIDs[sceneInfo.entity->classID]].lateUpdate)
                    objectClassList[stageObjectIDs[sceneInfo.entity->classID]].lateUpdate();
            }

            if (sceneInfo.entity->interaction) {
                typeGroups[GROUP_ALL].entries[typeGroups[GROUP_ALL].entryCount++] = e; // All active entities

                typeGroups[sceneInfo.entity->classID].entries[typeGroups[sceneInfo.entity->classID].entryCount++] = e; // type-based groups

                if (sceneInfo.entity->group >= TYPE_COUNT)
                    typeGroups[sceneInfo.entity->group].entries[typeGroups[sceneInfo.entity->group].entryCount++] = e; // extra groups
            }
        }

        sceneInfo.entity->onScreen = 0;
        sceneInfo.entitySlot++;
    }

#if RETRO_USE_MOD_LOADER
    RunModCallbacks(MODCB_ONLATEUPDATE, INT_TO_VOID(ENGINESTATE_FROZEN));
#endif
}
void RSDK::ProcessObjectDrawLists()
{
#if RETRO_PLATFORM == RETRO_PSP
    // the previous frame's display list samples the framebuffers we are about to draw over
    PSPWaitDisplayList();

    int32 freezeGroup  = pspFreezeDrawGroup;
    pspFreezeDrawGroup = -1; // the menu renews it for as long as it is drawn
    bool32 freezing    = sceneInfo.state == ENGINESTATE_FROZEN && videoSettings.screenCount == 1 && freezeGroup > 0 && freezeGroup < DRAWGROUP_COUNT;
    if (!freezing)
        PSPReleaseFreezeFrame();
#endif
    if (sceneInfo.state != ENGINESTATE_LOAD && sceneInfo.state != (ENGINESTATE_LOAD | ENGINESTATE_STEPOVER)) {
        for (int32 s = 0; s < videoSettings.screenCount; ++s) {
            currentScreen             = &screens[s];
            sceneInfo.currentScreenID = s;

            for (int32 l = 0; l < DRAWGROUP_COUNT; ++l) drawGroups[l].layerCount = 0;

            for (int32 t = 0; t < LAYER_COUNT; ++t) {
                uint8 drawGroup = tileLayers[t].drawGroup[s];

                if (drawGroup < DRAWGROUP_COUNT)
                    drawGroups[drawGroup].layerDrawList[drawGroups[drawGroup].layerCount++] = t;
            }

            sceneInfo.currentDrawGroup = 0;
            for (int32 l = 0; l < DRAWGROUP_COUNT; ++l) {
#if RETRO_PLATFORM == RETRO_PSP
                bool32 reuseFrame = freezing && pspFreezeValid && l < freezeGroup;
                if (freezing && l == freezeGroup) {
                    PSP_FLUSH_3D();
                    size_t frameSize = currentScreen->pitch * currentScreen->size.y * sizeof(uint16);
                    if (pspFreezeValid) {
                        memcpy(currentScreen->frameBuffer, pspFreezeFrame, frameSize);
                    }
                    else {
                        // first frozen frame: everything below was just drawn normally, keep it
                        if (!pspFreezeFrame)
                            pspFreezeFrame = (uint16 *)malloc(frameSize);
                        if (pspFreezeFrame) {
                            memcpy(pspFreezeFrame, currentScreen->frameBuffer, frameSize);
                            pspFreezeValid = true;
                        }
                    }
                }
#endif
                if (engine.drawGroupVisible[l]) {
                    DrawList *list = &drawGroups[l];

                    if (list->hookCB)
                        list->hookCB();

                    if (list->sorted) {
                        for (int32 e = 0; e < list->entityCount; ++e) {
                            for (int32 i = list->entityCount - 1; i > e; --i) {
                                int32 slot1 = list->entries[i - 1];
                                int32 slot2 = list->entries[i];
                                if (objectEntityList[slot2].zdepth > objectEntityList[slot1].zdepth) {
                                    list->entries[i - 1] = slot2;
                                    list->entries[i]     = slot1;
                                }
                            }
                        }
                    }

                    for (int32 i = 0; i < list->entityCount; ++i) {
#if RETRO_PLATFORM == RETRO_PSP
                        if (reuseFrame) {
                            EntityBase *entity = &objectEntityList[list->entries[i]];
                            if (entity->visible) {
                                entity->onScreen = pspFreezeOnScreen[list->entries[i]];
                                if (entity->onScreen)
                                    pspSlotFlags[list->entries[i]] |= PSP_SLOT_ONSCREEN;
                            }
                            continue;
                        }
#endif
                        sceneInfo.entitySlot = list->entries[i];
                        validDraw            = false;
                        sceneInfo.entity     = &objectEntityList[list->entries[i]];
                        if (sceneInfo.entity->visible) {
#if RETRO_PLATFORM == RETRO_PSP
                            if (objectClassList[stageObjectIDs[sceneInfo.entity->classID]].draw) {
                                uint16 profClass             = sceneInfo.entity->classID;
                                unsigned long long profStart = PSPProfFine();
                                objectClassList[stageObjectIDs[sceneInfo.entity->classID]].draw();
                                pspClassDraw[profClass] += PSPProfFine() - profStart;
                                PSPKick3DIfIdle();
                            }
#else
                            if (objectClassList[stageObjectIDs[sceneInfo.entity->classID]].draw)
                                objectClassList[stageObjectIDs[sceneInfo.entity->classID]].draw();
#endif

#if RETRO_VER_EGS || RETRO_USE_DUMMY_ACHIEVEMENTS
                            if (i == list->entityCount - 1)
                                SKU::DrawAchievements();
#endif

                            sceneInfo.entity->onScreen |= validDraw << sceneInfo.currentScreenID;
#if RETRO_PLATFORM == RETRO_PSP
                            if (sceneInfo.entity->onScreen)
                                pspSlotFlags[list->entries[i]] |= PSP_SLOT_ONSCREEN;
                            if (freezing && l < freezeGroup)
                                pspFreezeOnScreen[list->entries[i]] = sceneInfo.entity->onScreen;
#endif
                        }
                    }

#if RETRO_PLATFORM == RETRO_PSP
                    int32 layerCount = reuseFrame ? 0 : list->layerCount;
#else
                    int32 layerCount = list->layerCount;
#endif
                    for (int32 i = 0; i < layerCount; ++i) {
                        TileLayer *layer = &tileLayers[list->layerDrawList[i]];

#if RETRO_USE_MOD_LOADER
                        RunModCallbacks(MODCB_ONSCANLINECB, (void *)layer->scanlineCallback);
#endif
#if RETRO_PLATFORM == RETRO_PSP
                        unsigned long long layerStart = PSPProfFine();
#endif
                        if (layer->scanlineCallback)
                            layer->scanlineCallback(scanlines);
                        else
                            ProcessParallax(layer);

#if RETRO_PLATFORM == RETRO_PSP
                        unsigned long long rasterStart = PSPProfFine();
                        bool32 onGE = false;
                        switch (layer->type) {
                            case LAYER_HSCROLL: onGE = PSPQueueLayerHScroll(layer); break;
                            case LAYER_VSCROLL: onGE = PSPQueueLayerVScroll(layer); break;
                            case LAYER_ROTOZOOM: onGE = PSPQueueLayerRotozoom(layer); break;
                            case LAYER_BASIC: onGE = PSPQueueLayerBasic(layer); break;
                            default: break;
                        }
                        if (!onGE) {
                            PSP_FLUSH_3D();
                            switch (layer->type) {
                                case LAYER_HSCROLL: DrawLayerHScroll(layer); break;
                                case LAYER_VSCROLL: DrawLayerVScroll(layer); break;
                                case LAYER_ROTOZOOM: DrawLayerRotozoom(layer); break;
                                case LAYER_BASIC: DrawLayerBasic(layer); break;
                                default: break;
                            }
                        }
#else
                        switch (layer->type) {
                            case LAYER_HSCROLL: DrawLayerHScroll(layer); break;
                            case LAYER_VSCROLL: DrawLayerVScroll(layer); break;
                            case LAYER_ROTOZOOM: DrawLayerRotozoom(layer); break;
                            case LAYER_BASIC: DrawLayerBasic(layer); break;
                            default: break;
                        }
#endif
#if RETRO_PLATFORM == RETRO_PSP
                        unsigned long long layerEnd = PSPProfFine();
                        int32 layerID = list->layerDrawList[i];
                        pspLayerDraw += layerEnd - layerStart;
                        pspLayerPrep[layerID] += rasterStart - layerStart;
                        pspLayerRaster[layerID] += layerEnd - rasterStart;
                        ++pspLayerCalls[layerID];
#endif
                    }

#if RETRO_USE_MOD_LOADER
                    RunModCallbacks(MODCB_ONDRAW, INT_TO_VOID(l));
#endif

                    if (currentScreen->clipBound_X1 > 0)
                        currentScreen->clipBound_X1 = 0;

                    if (currentScreen->clipBound_Y1 > 0)
                        currentScreen->clipBound_Y1 = 0;

                    if (currentScreen->size.x >= 0) {
                        if (currentScreen->clipBound_X2 < currentScreen->size.x)
                            currentScreen->clipBound_X2 = currentScreen->size.x;
                    }
                    else {
                        currentScreen->clipBound_X2 = 0;
                    }

                    if (currentScreen->size.y >= 0) {
                        if (currentScreen->clipBound_Y2 < currentScreen->size.y)
                            currentScreen->clipBound_Y2 = currentScreen->size.y;
                    }
                    else {
                        currentScreen->clipBound_Y2 = 0;
                    }
                }

                sceneInfo.currentDrawGroup++;
            }

#if !RETRO_USE_ORIGINAL_CODE
            if (engine.showUpdateRanges) {
                for (int32 l = 0; l < DRAWGROUP_COUNT; ++l) {
                    if (engine.drawGroupVisible[l]) {
                        DrawList *list = &drawGroups[l];
                        for (int32 i = 0; i < list->entityCount; ++i) {
                            Entity *entity     = &objectEntityList[list->entries[i]];

                            if (entity->visible || (engine.showUpdateRanges & 2)) {
                                switch (entity->active) {
                                    default:
                                    case ACTIVE_DISABLED:
                                    case ACTIVE_NEVER: break;

                                    case ACTIVE_ALWAYS:
                                    case ACTIVE_NORMAL: 
                                    case ACTIVE_PAUSED:
                                        DrawRectangle(entity->position.x, entity->position.y, TO_FIXED(1), TO_FIXED(1), 0x0000FF, 0xFF, INK_NONE,
                                                      false);
                                        break;

                                    case ACTIVE_BOUNDS:
                                        DrawLine(entity->position.x - entity->updateRange.x, entity->position.y - entity->updateRange.y,
                                                 entity->position.x + entity->updateRange.x, entity->position.y - entity->updateRange.y, 0x0000FF,
                                                 0xFF, INK_NONE, false);

                                        DrawLine(entity->position.x - entity->updateRange.x, entity->position.y + entity->updateRange.y,
                                                 entity->position.x + entity->updateRange.x, entity->position.y + entity->updateRange.y, 0x0000FF,
                                                 0xFF, INK_NONE, false);

                                        DrawLine(entity->position.x - entity->updateRange.x, entity->position.y - entity->updateRange.y,
                                                 entity->position.x - entity->updateRange.x, entity->position.y + entity->updateRange.y, 0x0000FF,
                                                 0xFF, INK_NONE, false);

                                        DrawLine(entity->position.x + entity->updateRange.x, entity->position.y - entity->updateRange.y,
                                                 entity->position.x + entity->updateRange.x, entity->position.y + entity->updateRange.y, 0x0000FF,
                                                 0xFF, INK_NONE, false);
                                        break;

                                    case ACTIVE_XBOUNDS:
                                        DrawLine(entity->position.x - entity->updateRange.x, TO_FIXED(currentScreen->position.y),
                                                 entity->position.x - entity->updateRange.x,
                                                 TO_FIXED(currentScreen->position.y + currentScreen->size.y), 0x0000FF, 0xFF, INK_NONE, false);

                                        DrawLine(entity->position.x + entity->updateRange.x, TO_FIXED(currentScreen->position.y),
                                                 entity->position.x + entity->updateRange.x,
                                                 TO_FIXED(currentScreen->position.y + currentScreen->size.y), 0x0000FF, 0xFF, INK_NONE, false);
                                        break;

                                    case ACTIVE_YBOUNDS:
                                        DrawLine(TO_FIXED(currentScreen->position.x), entity->position.y - entity->updateRange.y,
                                                 TO_FIXED(currentScreen->position.x + currentScreen->size.x),
                                                 entity->position.y - entity->updateRange.y, 0x0000FF, 0xFF, INK_NONE, false);

                                        DrawLine(TO_FIXED(currentScreen->position.x), entity->position.y + entity->updateRange.y,
                                                 TO_FIXED(currentScreen->position.x + currentScreen->size.x),
                                                 entity->position.y + entity->updateRange.y, 0x0000FF, 0xFF, INK_NONE, false);
                                        break;

                                    case ACTIVE_RBOUNDS:
                                        DrawCircleOutline(entity->position.x, entity->position.y, FROM_FIXED(entity->updateRange.x),
                                                          FROM_FIXED(entity->updateRange.x) + 1, 0x0000FF, 0xFF, INK_NONE, false);
                                        break;
                                }
                            }
                        }
                    }
                }
            }

            if (engine.showEntityInfo) {
                for (int32 l = 0; l < DRAWGROUP_COUNT; ++l) {
                    if (engine.drawGroupVisible[l]) {
                        DrawList *list = &drawGroups[l];
                        for (int32 i = 0; i < list->entityCount; ++i) {
                            Entity *entity = &objectEntityList[list->entries[i]];

                            if (entity->visible || (engine.showEntityInfo & 2)) {
                                char buffer[0x100];
                                sprintf_s(buffer, sizeof(buffer), "%s\nx: %g\ny: %g", objectClassList[stageObjectIDs[entity->classID]].name,
                                          entity->position.x / 65536.0f, entity->position.y / 65536.0f);

                                DrawDevString(buffer, FROM_FIXED(entity->position.x) - currentScreen->position.x,
                                              FROM_FIXED(entity->position.y) - currentScreen->position.y, ALIGN_LEFT, 0xF0F0F0);
                            }
                        }
                    }
                }
            }

            if (showHitboxes) {
                for (int32 i = 0; i < debugHitboxCount; ++i) {
                    DebugHitboxInfo *info = &debugHitboxList[i];
                    int32 x               = info->pos.x + TO_FIXED(info->hitbox.left);
                    int32 y               = info->pos.y + TO_FIXED(info->hitbox.top);
                    int32 w               = abs((info->pos.x + TO_FIXED(info->hitbox.right)) - x);
                    int32 h               = abs((info->pos.y + TO_FIXED(info->hitbox.bottom)) - y);

                    switch (info->type) {
                        case H_TYPE_TOUCH: DrawRectangle(x, y, w, h, info->collision ? 0x808000 : 0xFF0000, 0x60, INK_ALPHA, false); break;

                        case H_TYPE_CIRCLE:
                            DrawCircle(info->pos.x, info->pos.y, info->hitbox.left, info->collision ? 0x808000 : 0xFF0000, 0x60, INK_ALPHA, false);
                            break;

                        case H_TYPE_BOX:
                            DrawRectangle(x, y, w, h, 0x0000FF, 0x60, INK_ALPHA, false);

                            if (info->collision & 1) // top
                                DrawRectangle(x, y, w, TO_FIXED(1), 0xFFFF00, 0xC0, INK_ALPHA, false);

                            if (info->collision & 8) // bottom
                                DrawRectangle(x, y + h, w, TO_FIXED(1), 0xFFFF00, 0xC0, INK_ALPHA, false);

                            if (info->collision & 2) { // left
                                int32 sy = y;
                                int32 sh = h;

                                if (info->collision & 1) {
                                    sy += TO_FIXED(1);
                                    sh -= TO_FIXED(1);
                                }

                                if (info->collision & 8)
                                    sh -= TO_FIXED(1);

                                DrawRectangle(x, sy, TO_FIXED(1), sh, 0xFFFF00, 0xC0, INK_ALPHA, false);
                            }

                            if (info->collision & 4) { // right
                                int32 sy = y;
                                int32 sh = h;

                                if (info->collision & 1) {
                                    sy += TO_FIXED(1);
                                    sh -= TO_FIXED(1);
                                }

                                if (info->collision & 8)
                                    sh -= TO_FIXED(1);

                                DrawRectangle(x + w, sy, TO_FIXED(1), sh, 0xFFFF00, 0xC0, INK_ALPHA, false);
                            }
                            break;

                        case H_TYPE_PLAT:
                            DrawRectangle(x, y, w, h, 0x00FF00, 0x60, INK_ALPHA, false);

                            if (info->collision & 1) // top
                                DrawRectangle(x, y, w, TO_FIXED(1), 0xFFFF00, 0xC0, INK_ALPHA, false);

                            if (info->collision & 8) // bottom
                                DrawRectangle(x, y + h, w, TO_FIXED(1), 0xFFFF00, 0xC0, INK_ALPHA, false);
                            break;
                    }
                }
            }

            if (engine.showPaletteOverlay) {
                for (int32 p = 0; p < PALETTE_BANK_COUNT; ++p) {
                    int32 x = (videoSettings.pixWidth - (0x10 << 3));
                    int32 y = (SCREEN_YSIZE - (0x10 << 2));

                    for (int32 c = 0; c < PALETTE_BANK_SIZE; ++c) {
                        uint32 clr = GetPaletteEntry(p, c);

                        DrawRectangle(x + ((c & 0xF) << 1) + ((p % (PALETTE_BANK_COUNT / 2)) * (2 * 16)),
                                      y + ((c >> 4) << 1) + ((p / (PALETTE_BANK_COUNT / 2)) * (2 * 16)), 2, 2, clr, 0xFF, INK_NONE, true);
                    }
                }
            }

#endif

            currentScreen++;
            sceneInfo.currentScreenID++;
        }
    }
}

uint16 RSDK::FindObject(const char *name)
{
    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5(name, hash);

    for (int32 o = 0; o < sceneInfo.classCount; ++o) {
        if (HASH_MATCH_MD5(hash, objectClassList[stageObjectIDs[o]].hash))
            return o;
    }

    return TYPE_DEFAULTOBJECT;
}

int32 RSDK::GetEntityCount(uint16 classID, bool32 isActive)
{
    if (classID >= TYPE_COUNT)
        return 0;
    if (isActive)
        return typeGroups[classID].entryCount;

    int32 entityCount = 0;
    for (int32 i = 0; i < ENTITY_COUNT; ++i) {
        if (objectEntityList[i].classID == classID)
            entityCount++;
    }

    return entityCount;
}

void RSDK::ResetEntity(Entity *entity, uint16 classID, void *data)
{
    if (entity) {
        ObjectClass *info = &objectClassList[stageObjectIDs[classID]];
        memset(entity, 0, info->entityClassSize);

        if (info->create) {
            Entity *curEnt = sceneInfo.entity;

            sceneInfo.entity              = entity;
            sceneInfo.entity->interaction = true;
#if RETRO_USE_MOD_LOADER
            int32 superStore          = superLevels[inheritLevel];
            superLevels[inheritLevel] = 0;
#endif
            info->create(data);
#if RETRO_USE_MOD_LOADER
            superLevels[inheritLevel] = superStore;
#endif
            sceneInfo.entity->classID = classID;

            sceneInfo.entity = curEnt;
        }

        entity->classID = classID;
    }
}

void RSDK::ResetEntitySlot(uint16 slot, uint16 classID, void *data)
{
    ObjectClass *object = &objectClassList[stageObjectIDs[classID]];
    slot                = slot < ENTITY_COUNT ? slot : (ENTITY_COUNT - 1);

    Entity *entity = &objectEntityList[slot];
    memset(&objectEntityList[slot], 0, object->entityClassSize);

    if (object->create) {
        Entity *curEnt = sceneInfo.entity;

        sceneInfo.entity    = entity;
        entity->interaction = true;
#if RETRO_USE_MOD_LOADER
        int32 superStore          = superLevels[inheritLevel];
        superLevels[inheritLevel] = 0;
#endif
        object->create(data);
#if RETRO_USE_MOD_LOADER
        superLevels[inheritLevel] = superStore;
#endif
        entity->classID = classID;

        sceneInfo.entity = curEnt;
    }
    else {
        entity->classID = classID;
    }
}

Entity *RSDK::CreateEntity(uint16 classID, void *data, int32 x, int32 y)
{
    ObjectClass *object = &objectClassList[stageObjectIDs[classID]];
    Entity *entity      = &objectEntityList[sceneInfo.createSlot];

    int32 permCnt = 0, loopCnt = 0;
    while (entity->classID) {
        // after 16 loops, the game says fuck it and will start overwriting non-temp objects
        if (!entity->isPermanent && loopCnt >= 16)
            break;

        if (entity->isPermanent)
            ++permCnt;

        sceneInfo.createSlot++;
        if (sceneInfo.createSlot == ENTITY_COUNT) {
            sceneInfo.createSlot = TEMPENTITY_START;
            entity               = &objectEntityList[sceneInfo.createSlot];
        }
        else {
            entity = &objectEntityList[sceneInfo.createSlot];
        }

        if (permCnt >= TEMPENTITY_COUNT)
            break;

        ++loopCnt;
    }

    memset(entity, 0, object->entityClassSize);
    entity->position.x  = x;
    entity->position.y  = y;
    entity->interaction = true;

    if (object->create) {
        Entity *curEnt = sceneInfo.entity;

        sceneInfo.entity = entity;
#if RETRO_USE_MOD_LOADER
        int32 superStore          = superLevels[inheritLevel];
        superLevels[inheritLevel] = 0;
#endif
        object->create(data);
#if RETRO_USE_MOD_LOADER
        superLevels[inheritLevel] = superStore;
#endif
        entity->classID = classID;

        sceneInfo.entity = curEnt;
    }
    else {
        entity->classID = classID;
        entity->active  = ACTIVE_NORMAL;
        entity->visible = true;
    }

    return entity;
}

bool32 RSDK::GetActiveEntities(uint16 group, Entity **entity)
{
    if (group >= TYPEGROUP_COUNT)
        return false;

    if (!entity)
        return false;

    if (*entity) {
        ++foreachStackPtr->id;
    }
    else {
        foreachStackPtr++;
        foreachStackPtr->id = 0;
    }

    for (Entity *nextEntity = &objectEntityList[typeGroups[group].entries[foreachStackPtr->id]]; foreachStackPtr->id < typeGroups[group].entryCount;
         ++foreachStackPtr->id, nextEntity = &objectEntityList[typeGroups[group].entries[foreachStackPtr->id]]) {
        if (nextEntity->classID == group) {
            *entity = nextEntity;
            return true;
        }
    }

    foreachStackPtr--;

    return false;
}
bool32 RSDK::GetAllEntities(uint16 classID, Entity **entity)
{
    if (classID >= OBJECT_COUNT)
        return false;

    if (!entity)
        return false;

    if (*entity) {
        ++foreachStackPtr->id;
    }
    else {
        foreachStackPtr++;
        foreachStackPtr->id = 0;
    }

    for (; foreachStackPtr->id < ENTITY_COUNT; ++foreachStackPtr->id) {
        Entity *nextEntity = &objectEntityList[foreachStackPtr->id];
        if (nextEntity->classID == classID) {
            *entity = nextEntity;
            return true;
        }
    }

    foreachStackPtr--;

    return false;
}

bool32 RSDK::CheckOnScreen(Entity *entity, Vector2 *range)
{
    if (!entity)
        return false;

    if (!range)
        range = &entity->updateRange;

    return CheckPosOnScreen(&entity->position, range);
}
bool32 RSDK::CheckPosOnScreen(Vector2 *position, Vector2 *range)
{
    if (!position || !range)
        return false;

    for (int32 s = 0; s < cameraCount; ++s) {
        int32 sx = abs(position->x - cameras[s].position.x);
        int32 sy = abs(position->y - cameras[s].position.y);

        if (sx <= range->x + cameras[s].offset.x && sy <= range->y + cameras[s].offset.y)
            return true;
    }

    return false;
}

void RSDK::ClearStageObjects()
{
    // Unload static object classes
    for (int32 o = 0; o < sceneInfo.classCount; ++o) {
        if (objectClassList[stageObjectIDs[o]].staticVars) {
            *objectClassList[stageObjectIDs[o]].staticVars = NULL;
        }
    }
}
