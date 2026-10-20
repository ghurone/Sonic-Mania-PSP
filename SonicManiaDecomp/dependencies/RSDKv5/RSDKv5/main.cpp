#include "RSDK/Core/RetroEngine.hpp"
#include "main.hpp"

#if RETRO_STANDALONE
#define LinkGameLogic RSDK::LinkGameLogic
#else
#define EngineInfo RSDK::EngineInfo
#include <GameMain.h>
#define LinkGameLogic LinkGameLogicDLL
#endif

#if RETRO_PLATFORM == RETRO_WIN && !RETRO_RENDERDEVICE_SDL2

#if RETRO_RENDERDEVICE_DIRECTX9 || RETRO_RENDERDEVICE_DIRECTX11
INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, INT nShowCmd)
{
    RSDK::RenderDevice::hInstance     = hInstance;
    RSDK::RenderDevice::hPrevInstance = hPrevInstance;
    RSDK::RenderDevice::nShowCmd      = nShowCmd;

    return RSDK_main(1, &lpCmdLine, LinkGameLogic);
}
#else
INT WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, INT nShowCmd)
{
    return RSDK_main(1, &lpCmdLine, LinkGameLogic);
}
#endif

#elif RETRO_PLATFORM == RETRO_ANDROID
extern "C" {
void android_main(struct android_app *app);
}

void android_main(struct android_app *ap)
{
    app                                 = ap;
    app->onAppCmd                       = AndroidCommandCallback;
    app->activity->callbacks->onKeyDown = AndroidKeyDownCallback;
    app->activity->callbacks->onKeyUp   = AndroidKeyUpCallback;

    JNISetup *jni = GetJNISetup();
    // we make sure we do it here so init can chill safely before any callbacks occur
    Paddleboat_init(jni->env, jni->thiz);

    SwappyGL_init(jni->env, jni->thiz);
    SwappyGL_setAutoSwapInterval(false);
    SwappyGL_setSwapIntervalNS(SWAPPY_SWAP_60FPS);
    SwappyGL_setMaxAutoSwapIntervalNS(SWAPPY_SWAP_60FPS);

    getFD    = jni->env->GetMethodID(jni->clazz, "getFD", "([BB)I");
    writeLog = jni->env->GetMethodID(jni->clazz, "writeLog", "([BI)V");

    setLoading = jni->env->GetMethodID(jni->clazz, "setLoadingIcon", "([B)V");
    showLoading = jni->env->GetMethodID(jni->clazz, "showLoadingIcon", "()V");
    hideLoading = jni->env->GetMethodID(jni->clazz, "hideLoadingIcon", "()V");

    setPixSize = jni->env->GetMethodID(jni->clazz, "setPixSize", "(II)V");

#if RETRO_USE_MOD_LOADER
    fsExists      = jni->env->GetMethodID(jni->clazz, "fsExists", "([B)Z");
    fsIsDir       = jni->env->GetMethodID(jni->clazz, "fsIsDir", "([B)Z");
    fsDirIter     = jni->env->GetMethodID(jni->clazz, "fsDirIter", "([B)[Ljava/lang/String;");
    fsRecurseIter = jni->env->GetMethodID(jni->clazz, "fsRecurseIter", "([B)Ljava/lang/String;");
#endif

    GameActivity_setWindowFlags(app->activity,
                                AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_TURN_SCREEN_ON | AWINDOW_FLAG_LAYOUT_NO_LIMITS | AWINDOW_FLAG_FULLSCREEN
                                    | AWINDOW_FLAG_SHOW_WHEN_LOCKED,
                                0);

    RSDK_main(0, NULL, (void *)LinkGameLogic);

    Paddleboat_destroy(jni->env);
    SwappyGL_destroy();
}
#elif RETRO_PLATFORM == RETRO_PSP
#include <pspsdk.h>
#include <malloc.h>
PSP_MODULE_INFO("SonicMania", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
int pspAutoTest = 0;
char pspPackName[0x40] = "Data.psp"; // boot.txt "pack=NAME": another Data.psp to run from (tests)
int pspProfDetail = 0; // boot.txt "prof" (see PSPProfFine)
int pspLogFiles = 0; // boot.txt "logfiles": LoadFile logs every file it opens (to lay Data.psp out in load order)
extern "C" int pspSkipShadows = 0; // boot.txt "noshadows": UFO_Shadow draws nothing (to measure what shadows cost)
extern "C" int pspStartStarPostX = 0; // boot.txt starpost=X: start at the StarPost at x=X (testing, StarPost.c)
extern "C" int pspUfoInfiniteTime = 0; // boot.txt "ufotime": the special stages' rings stop draining (testing)
extern "C" int pspShadowChunk = 1; // boot.txt shadowchunk=N: UFO shadows N to a 3D scene (UFO_Shadow.c; 1: each its own, as the original)
extern "C" int pspShadowBatchOff = 0; // boot.txt "noshadowbatch": one 3D scene per shadow, as the original (to compare)
extern "C" int pspSmogTest = 0; // boot.txt "smogtest": OOZ's smog always on (Smog.c), to measure it

// boot.txt "iobench": Memory Stick read speed from Data.psp - sequential at several request sizes, and small random reads
static void PSPMemoryStickBench()
{
    SceUID fd = sceIoOpen(pspPackName, PSP_O_RDONLY, 0);
    if (fd < 0)
        return;
    uint8 *buffer = (uint8 *)memalign(64, 0x200000);
    if (!buffer) {
        sceIoClose(fd);
        return;
    }
    const uint32 sizes[] = { 0x8000, 0x20000, 0x80000, 0x200000 };
    uint32 offset        = 0x1000000;
    for (uint32 s = 0; s < 4; ++s) {
        unsigned long long start = sceKernelGetSystemTimeWide();
        uint32 total             = 0;
        while (total < 0x800000) {
            sceIoLseek32(fd, offset + total, PSP_SEEK_SET);
            int32 got = sceIoRead(fd, buffer, sizes[s]);
            if (got <= 0)
                break;
            total += got;
        }
        unsigned long long us = sceKernelGetSystemTimeWide() - start;
        RSDK::PrintLog(RSDK::PRINT_NORMAL, "PSP iobench: %uKB requests: %u KB in %u ms = %u KB/s", sizes[s] >> 10, total >> 10, (uint32)(us / 1000),
                       (uint32)(us ? (unsigned long long)total * 1000 / us : 0));
        offset += 0x1000000; // a fresh region each time
    }
    uint32 seed = 1;
    unsigned long long start = sceKernelGetSystemTimeWide();
    for (int32 i = 0; i < 200; ++i) {
        seed = seed * 1103515245 + 12345;
        sceIoLseek32(fd, (seed >> 4) % 0x8000000, PSP_SEEK_SET);
        sceIoRead(fd, buffer, 0x1000);
    }
    RSDK::PrintLog(RSDK::PRINT_NORMAL, "PSP iobench: 200 random 4KB reads in %u ms", (uint32)((sceKernelGetSystemTimeWide() - start) / 1000));
    free(buffer);
    sceIoClose(fd);
}

// boot.txt "seekbench": what a 4KB read costs by its offset in Data.psp, each after a read at the start of the file
// (does seeking walk the FAT cluster chain from the start?)
static void PSPSeekBench()
{
    SceUID fd = sceIoOpen(pspPackName, PSP_O_RDONLY, 0);
    if (fd < 0)
        return;
    static uint8 __attribute__((aligned(64))) buffer[0x1000];
    for (int32 mb = 0; mb < 240; mb += 16) {
        unsigned long long total = 0;
        for (int32 i = 0; i < 10; ++i) {
            sceIoLseek32(fd, 0, PSP_SEEK_SET);
            sceIoRead(fd, buffer, sizeof(buffer));
            unsigned long long start = sceKernelGetSystemTimeWide();
            sceIoLseek32(fd, (mb << 20) + i * 0x10000, PSP_SEEK_SET);
            sceIoRead(fd, buffer, sizeof(buffer));
            total += sceKernelGetSystemTimeWide() - start;
        }
        unsigned long long fwd = 0;
        for (int32 i = 0; i < 10; ++i) { // forward hops from a nearby position
            sceIoLseek32(fd, (mb << 20) + i * 0x20000, PSP_SEEK_SET);
            sceIoRead(fd, buffer, sizeof(buffer));
            unsigned long long start = sceKernelGetSystemTimeWide();
            sceIoLseek32(fd, (mb << 20) + i * 0x20000 + 0x10000, PSP_SEEK_SET);
            sceIoRead(fd, buffer, sizeof(buffer));
            fwd += sceKernelGetSystemTimeWide() - start;
        }
        RSDK::PrintLog(RSDK::PRINT_NORMAL, "PSP seekbench: %3dMB: from the start %u us, 64KB forward %u us", mb, (uint32)(total / 10), (uint32)(fwd / 10));
    }
    sceIoClose(fd);
}

int main(int argc, char *argv[])
{
    // Real hardware traps invalid float ops (e.g. (int32)tanf(PI/2) in CalculateTrigAngles); PPSSPP does not.
    // The engine relies on PC semantics, so run with FPU exceptions masked. Each new thread must do the same.
    pspSdkDisableFPUExceptions();
    int clockSet = scePowerSetClockFrequency(333, 333, 166);

    // optional boot.txt: engine arguments such as "stage=GHZ;scene=1" ("autotest" scripts the pad)
    static char bootArgs[0x100];
    char *bootArgv[2] = { argv && argc > 0 ? argv[0] : (char *)"", bootArgs };
    FILE *bootFile    = fopen("boot.txt", "rb");
    if (bootFile) {
        size_t len = fread(bootArgs, 1, sizeof(bootArgs) - 1, bootFile);
        bootArgs[len] = 0;
        fclose(bootFile);
        while (len && (bootArgs[len - 1] == '\r' || bootArgs[len - 1] == '\n' || bootArgs[len - 1] == ' ')) bootArgs[--len] = 0;
        // a test run: the log goes to log.txt on the card too ("nolog" keeps it in RAM, as normal play does)
        extern int pspLogToCard, pspLogEnabled;
        pspLogEnabled = 1;
        pspLogToCard  = !strstr(bootArgs, "nolog");
        // autotest: presses through the menus too; autoplay: only plays (for boots straight into a stage)
        pspAutoTest = strstr(bootArgs, "autotest") ? 1 : (strstr(bootArgs, "autoplay") ? 2 : 0);
        if (strstr(bootArgs, "gesync"))
            pspGeSyncTest = 1;
        if (strstr(bootArgs, "nogesprites"))
            pspGeSprites = 0;
        if (strstr(bootArgs, "nogelayers"))
            pspGeLayers = 0;
        if (strstr(bootArgs, "logflush"))
            pspLogEveryReport = 1;
        if (strstr(bootArgs, "skip30"))
            pspFrameSkipMode = strstr(bootArgs, "skip30auto") ? 1 : 2;
        if (strstr(bootArgs, "musictest"))
            pspMusicTest = 1;
        if (strstr(bootArgs, "framecheck"))
            pspFrameCheck = 1;
        if (strstr(bootArgs, "nogemodels"))
            pspGeModels = 0;
        if (strstr(bootArgs, "stopat="))
            pspReplayStopAt = atoi(strstr(bootArgs, "stopat=") + 7);
        if (strstr(bootArgs, "prof"))
            pspProfDetail = 1;
        if (strstr(bootArgs, "logfiles"))
            pspLogFiles = 1;
        if (strstr(bootArgs, "noshadows"))
            pspSkipShadows = 1;
        if (strstr(bootArgs, "pagemin=")) {
            extern int pspRotoPageMin;
            pspRotoPageMin = atoi(strstr(bootArgs, "pagemin=") + 8);
        }
        if (strstr(bootArgs, "pages=")) {
            extern int pspRotoPageMax;
            pspRotoPageMax = atoi(strstr(bootArgs, "pages=") + 6);
        }
        if (strstr(bootArgs, "pagesdbg=")) {
            extern int pspRotoPagesDebug;
            pspRotoPagesDebug = atoi(strstr(bootArgs, "pagesdbg=") + 9);
        }
        if (strstr(bootArgs, "pagesrows=")) {
            extern int pspRotoPagesRowA, pspRotoPagesRowB;
            const char *r = strstr(bootArgs, "pagesrows=") + 10;
            pspRotoPagesRowA = atoi(r);
            pspRotoPagesRowB = strchr(r, ',') ? atoi(strchr(r, ',') + 1) : 9999;
        }
        if (strstr(bootArgs, "pagesnodraw")) {
            extern int pspRotoPagesNoDraw;
            pspRotoPagesNoDraw = 1;
        }
        if (strstr(bootArgs, "pagesalways")) {
            extern int pspRotoPageAB;
            pspRotoPageAB = 0;
        }
        if (strstr(bootArgs, "noflatrows")) {
            extern int pspRotoFlatRows;
            pspRotoFlatRows = 0;
        }
        if (strstr(bootArgs, "pageson")) {
            extern int pspRotoPagesOn;
            pspRotoPagesOn = 1;
        }
        if (strstr(bootArgs, "nopages")) {
            extern int pspRotoPagesOn;
            pspRotoPagesOn = 0;
        }
        if (strstr(bootArgs, "starpost="))
            pspStartStarPostX = atoi(strstr(bootArgs, "starpost=") + 9);
        if (strstr(bootArgs, "ufotime"))
            pspUfoInfiniteTime = 1;
        if (strstr(bootArgs, "shadowchunk="))
            pspShadowChunk = MAX(1, atoi(strstr(bootArgs, "shadowchunk=") + 12));
        if (strstr(bootArgs, "noshadowbatch"))
            pspShadowBatchOff = 1;
        if (strstr(bootArgs, "smogtest"))
            pspSmogTest = 1;
        if (strstr(bootArgs, "rotocached")) {
            extern int pspRotoUncached;
            pspRotoUncached = 0;
        }
        if (strstr(bootArgs, "novfpu")) {
            extern int pspRotoVfpu;
            pspRotoVfpu = 0;
        }
        if (strstr(bootArgs, "layerkickwait")) {
            extern int pspLayerKickWait;
            pspLayerKickWait = 1;
        }
        if (strstr(bootArgs, "nocoarse")) {
            extern int pspRotoCoarseOn;
            pspRotoCoarseOn = 0;
        }
        if (strstr(bootArgs, "cellmap")) {
            extern int pspRotoCellsOn;
            pspRotoCellsOn = 1;
        }
        if (strstr(bootArgs, "rotoramtest")) {
            extern int pspRotoEmit;
            pspRotoEmit = 2;
        }
        if (strstr(bootArgs, "rotonoemit")) {
            extern int pspRotoEmit;
            pspRotoEmit = 0;
        }
        if (strstr(bootArgs, "noblankmap")) {
            extern int pspRotoBlankOn;
            pspRotoBlankOn = 0;
        }
        if (strstr(bootArgs, "nooverlap"))
            pspGeOverlap = 0;
        if (strstr(bootArgs, "emuge"))
            pspEmuGeLayers = 1;
        if (strstr(bootArgs, "horizonlod=")) {
            extern int pspHorizonLod;
            pspHorizonLod = atoi(strstr(bootArgs, "horizonlod=") + 11);
        }
        if (strstr(bootArgs, "horizonlodx")) {
            extern int pspHorizonLodX;
            pspHorizonLodX = 1;
        }
        if (strstr(bootArgs, "horizonrows=")) {
            extern int pspHorizonLodMinRows;
            pspHorizonLodMinRows = atoi(strstr(bootArgs, "horizonrows=") + 12);
        }
        if (strstr(bootArgs, "cachedmodels")) {
            extern int pspGe3DUncachedModels;
            pspGe3DUncachedModels = 0;
        }
        if (strstr(bootArgs, "dumpframes=")) {
            extern int pspDumpStart, pspDumpCount, pspDumpStep;
            sscanf(strstr(bootArgs, "dumpframes=") + 11, "%d,%d,%d", &pspDumpStart, &pspDumpCount, &pspDumpStep);
        }
        if (strstr(bootArgs, "spritelog=")) {
            extern int pspSpriteLogFrame;
            pspSpriteLogFrame = atoi(strstr(bootArgs, "spritelog=") + 10);
        }
        if (strstr(bootArgs, "spriteanybank")) {
            extern int pspSpriteAnyBank;
            pspSpriteAnyBank = 1;
        }
        if (strstr(bootArgs, "pack=")) {
            const char *arg = strstr(bootArgs, "pack=") + 5;
            int32 n         = 0;
            while (*arg && *arg != ';' && n < (int32)sizeof(pspPackName) - 1) pspPackName[n++] = *arg++;
            pspPackName[n] = 0;
        }
        if (strstr(bootArgs, "gebusy=")) {
            extern int pspGeBusyState;
            pspGeBusyState = atoi(strstr(bootArgs, "gebusy=") + 7);
        }
        if (strstr(bootArgs, "gekick="))
            pspGeKickKB = atoi(strstr(bootArgs, "gekick=") + 7);
        if (strstr(bootArgs, "rotodense=")) {
            extern int pspRotoIndexDensity;
            pspRotoIndexDensity = atoi(strstr(bootArgs, "rotodense=") + 10);
        }
        if (strstr(bootArgs, "rotoidx="))
            pspRotoIndexRun = atoi(strstr(bootArgs, "rotoidx=") + 8);
        if (strstr(bootArgs, "rotorun="))
            pspRotoGeRun = atoi(strstr(bootArgs, "rotorun=") + 8);
        if (strstr(bootArgs, "rotostrip"))
            pspRotoStrips = 1;
        if (strstr(bootArgs, "rotocheck"))
            pspRotoCheck = 1;
        if (strstr(bootArgs, "rotoseg="))
            pspRotoSegment = MAX(1, atoi(strstr(bootArgs, "rotoseg=") + 8));
        if (strstr(bootArgs, "seekbench"))
            PSPSeekBench();
        if (strstr(bootArgs, "iobench"))
            PSPMemoryStickBench();
        // record=NAME / replay=NAME: the pad to/from rec_NAME.bin
        const char *recArgs[2] = { "record=", "replay=" };
        for (int32 m = 0; m < 2; ++m) {
            const char *arg = strstr(bootArgs, recArgs[m]);
            if (arg) {
                char name[0x20];
                int32 n = 0;
                for (arg += 7; *arg && *arg != ';' && n < 0x1F; ++arg) name[n++] = *arg;
                name[n] = 0;
                PSPInputRecordingInit(m ? PSP_INPUT_REPLAY : PSP_INPUT_RECORD, name);
            }
        }
        argc        = 2;
        argv        = bootArgv;
    }

    RSDK::PrintLog(RSDK::PRINT_NORMAL, "PSP clock: set %08X, cpu %d MHz, bus %d MHz", clockSet, scePowerGetCpuClockFrequencyInt(),
                   scePowerGetBusClockFrequencyInt());
    {
        // what the profiling clocks cost per call
        unsigned long long t0 = sceKernelGetSystemTimeWide(), tick;
        for (int32 i = 0; i < 1000; ++i) sceRtcGetCurrentTick(&tick);
        unsigned long long t1 = sceKernelGetSystemTimeWide();
        volatile uint32 sink = 0;
        for (int32 i = 0; i < 1000; ++i) sink += sceKernelGetSystemTimeLow();
        unsigned long long t2 = sceKernelGetSystemTimeWide();
        RSDK::PrintLog(RSDK::PRINT_NORMAL, "PSP clock cost: sceRtcGetCurrentTick %u ns, sceKernelGetSystemTimeLow %u ns", (uint32)(t1 - t0),
                       (uint32)(t2 - t1));
    }
    int32 code = RSDK_main(argc, argv, (void *)LinkGameLogic);
    RSDK::PrintLog(RSDK::PRINT_NORMAL, "RSDK_main returned %d", code);
    PSPSaveInputRecording();
    PSPFlushUserData();
    PSPFlushLog();
    sceKernelExitGame();
    return code;
}
#else
int32 main(int32 argc, char *argv[]) { return RSDK_main(argc, argv, (void *)LinkGameLogic); }
#endif

int32 RSDK_main(int32 argc, char **argv, void *linkLogicPtr)
{
    RSDK::linkGameLogic = (RSDK::LogicLinkHandle)linkLogicPtr;

    RSDK::InitCoreAPI();

    int32 exitCode = RSDK::RunRetroEngine(argc, argv);

    RSDK::ReleaseCoreAPI();

    return exitCode;
}
