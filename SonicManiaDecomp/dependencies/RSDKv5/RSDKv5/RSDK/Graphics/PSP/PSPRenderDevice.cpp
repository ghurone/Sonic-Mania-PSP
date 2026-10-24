// PSP render device: the engine software-renders into its framebuffers in BGR565 (the GE's 5650
// texture format, see RGB888_TO_RGB565), so the GE samples them in place - no conversion copy - and
// stretches the 424x240 image to the 480x272 LCD in narrow sprite slices. The display list for frame N
// runs while the CPU updates frame N+1; PSPWaitDisplayList() keeps the engine from drawing over the
// framebuffer before the GE is done reading it.

#include "RSDK/Scene/PSP/PSPRotoWalk.hpp"
extern int pspLogEnabled; // Debug.cpp: test runs only
#include <malloc.h>
#include <psppower.h>

#define PSP_BUF_WIDTH  (512)
#define PSP_SCR_WIDTH  (480)
#define PSP_SCR_HEIGHT (272)

#define PSP_FB_SIZE_BYTES (PSP_BUF_WIDTH * PSP_SCR_HEIGHT * 2)

// VRAM: three LCD buffers, then the engine screens (the engine draws there directly, see ScreenInfo), the depth
// buffer for 3D, and the tileset atlas (512x512 8-bit) for tile layers drawn by the GE. Screens 2 and 3 (3-4 player
// competition, impossible with the PSP's one controller) share screen 1's frame, which leaves room for the third
// LCD buffer.
#define PSP_LCD_BUFFERS       (3)
#define PSP_VRAM_LCD(i)       ((i) * PSP_FB_SIZE_BYTES)
// (the 32KB gap before the screens absorbs drawing that lands just above the first one)
#define PSP_VRAM_SCREEN(s)    (PSP_LCD_BUFFERS * PSP_FB_SIZE_BYTES + 0x8000 + ((s) < 2 ? (s) : 1) * 0x35000)
#define PSP_VRAM_SCREEN_BYTES (0x35000)
#define PSP_VRAM_DEPTH        (PSP_VRAM_SCREEN(1) + PSP_VRAM_SCREEN_BYTES)
// (margin after the 448x240 depth buffer: drawing is scissored to the screen, but keep the GE's vertices well clear)
#define PSP_VRAM_ARENA        (PSP_VRAM_DEPTH + 0x3C000) // the GE's vertices (see pspGe3DArena)
// 512x256 8-bit: sprite frames copied from their sheets for the GE. Its texture must lie wholly inside VRAM: declared
// 512 rows high from here it ran past the end, and the GE dropped that list - and the frame blit queued behind it.
#define PSP_VRAM_SPRITES      (0x1A0000)
#define PSP_SPRITE_CACHE_ROWS (256)
#define PSP_VRAM_ATLAS        (0x1C0000) // the last 256KB
// Videos and full-screen images (Graphics/Video.cpp): the hardware JPEG decoder writes the frame, 512x272 RGBA, over
// the screens and the depth buffer - nothing else draws while one plays
#define PSP_VRAM_VIDEO        (PSP_VRAM_SCREEN(0))
#define PSP_VIDEO_PITCH       (512)

static unsigned int __attribute__((aligned(16))) pspGuList[65536];

// Float vertices so the 424->480 stretch can be cut into slices with exact (fractional) edges.
struct PSPSpriteVertex {
    float u, v;
    float x, y, z;
};

// The GE texture cache is tiny: one huge stretched sprite thrashes it and costs ~20ms on hardware.
// Drawing the same blit as narrow vertical strips keeps each strip's texels resident.
#define PSP_BLIT_SLICE (64)

static RenderDevice::WindowInfo::Display pspDisplay = { PSP_SCR_WIDTH, PSP_SCR_HEIGHT, 60 };

uint64 RenderDevice::targetTicks = 0;
uint64 RenderDevice::curTicks    = 0;
uint64 RenderDevice::prevTicks   = 0;

static bool pspGuPending     = false; // a display list was kicked and its frame not yet presented
static bool pspGuReading     = false; // that list may still be reading the engine framebuffers
static bool pspGuInitialized = false;
// Triple buffering: a frame handed to the display only reaches the screen at the next vertical blank, so the blit of
// the following frame goes to the third buffer, never to one that is on screen or about to be.
static int32 pspLcdDraw               = 0; // the next blit's target
static int32 pspLcdShown              = 1; // the last buffer handed to the display
static unsigned int pspLcdShownVcount = 0; // vertical blank count right after handing it over

static volatile bool pspExitRequested = false;

static int PSPExitCallback(int arg1, int arg2, void *common)
{
    pspExitRequested = true;
    RenderDevice::isRunning = false;
    return 0;
}

// Sleep mode closes every file the game holds open (Data.psp, the music stream): their handles fail after it. Counted
// here on each resume, so the readers reopen them (Reader.cpp PSPPackDeviceRead, Audio.cpp PSPAt3Refill).
volatile int pspResumeCount = 0;
volatile int pspPowerFlags  = 0; // (diagnostics: every power event's flags, or'ed)

static int PSPPowerCallback(int unknown, int powerInfo, void *common)
{
    pspPowerFlags |= powerInfo;
    if (powerInfo & PSP_POWER_CB_RESUME_COMPLETE)
        pspResumeCount++;
    return 0;
}

static int PSPCallbackThread(SceSize args, void *argp)
{
    int cbid = sceKernelCreateCallback("Exit Callback", PSPExitCallback, NULL);
    sceKernelRegisterExitCallback(cbid);
    int pcbid = sceKernelCreateCallback("Power Callback", PSPPowerCallback, NULL);
    int slot  = scePowerRegisterCallback(-1, pcbid);
    if (slot < 0)
        slot = scePowerRegisterCallback(0, pcbid); // (older firmware: no "any free slot")
    PrintLog(PRINT_NORMAL, "PSP power callback: %d", slot);
    sceKernelSleepThreadCB();
    return 0;
}

static void PSPSetupCallbacks()
{
    int thid = sceKernelCreateThread("update_thread", PSPCallbackThread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, 0);
}

void RenderDevice::RequestExit() { isRunning = false; }

static inline bool pspGe3DReady() { return pspGuInitialized; }

bool RenderDevice::Init()
{
    PSPSetupCallbacks();

    videoSettings.windowed     = false;
    videoSettings.windowWidth  = PSP_SCR_WIDTH;
    videoSettings.windowHeight = PSP_SCR_HEIGHT;
    videoSettings.vsync        = true;
    videoSettings.refreshRate  = 60;

    // all the PSP has is a fixed LCD
    if (!SetupRendering() || !AudioDevice::Init())
        return false;

    InitInputDevices();
    return true;
}

unsigned long long pspProf[PSPPROF_MAX];
unsigned long long pspLoadProf[PSPLOAD_MAX];
unsigned long long pspLoadIO[PSPLOAD_MAX], pspLoadDecryptUS, pspLoadGifUS;
static uint32 pspProfFrames          = 0;
static unsigned long long pspLastFlip = 0;
static bool pspLate                  = false;

// Frame pacing. A scene that can't hold 60fps used to run at whatever rate it managed (frames on screen for one or two
// refreshes at random: judder, and the game itself slowed down). Instead, when one update + draw costs more than a
// refresh, the engine runs two updates per drawn frame (pspUpdatesPerDraw) and each frame stays exactly two refreshes:
// a steady 30fps at full game speed. boot.txt "noskip" disables it, "skip30" forces it.
int pspFrameSkipMode                  = 0; // 0 never, 1 automatic, 2 always (boot.txt "skip30auto"/"skip30")
int pspUpdatesPerDraw                 = 1; // what the engine runs next frame
int pspFrameUpdates                   = 1; // what the frame being drawn ran
unsigned long long pspExtraUpdateUS   = 0; // time spent in its extra updates
static int pspPendingUpdates          = 1; // updates in the frame waiting to be shown
static int pspShownUpdates            = 1; // ... and in the one on screen
static float pspFrameCostAvg          = 0; // one update + draw, us (moving average)
static uint32 pspSkipFrames           = 0; // drawn frames with two updates, this report

static uint32 pspCheckBadStamp, pspCheckBack, pspCheckRepeat, pspCheckFrames;
static uint32 pspMissedRefreshes, pspWorstFrameUS; // per report: refreshes below 60fps, slowest flip-to-flip

extern int pspGeSyncTest;
static unsigned long long pspBlitTestUS;

static void PSPProfReport()
{
    uint32 n = pspProfFrames ? pspProfFrames : 1;
    PrintLog(PRINT_NORMAL, "PSP perf (us/frame, %u frames): total=%u upd=%u draw=%u copy=%u gu=%u vblank=%u audio=%u audlock=%u", pspProfFrames,
             (uint32)(pspProf[PSPPROF_FRAME] / n), (uint32)(pspProf[PSPPROF_OBJUPDATE] / n), (uint32)(pspProf[PSPPROF_DRAW] / n),
             (uint32)(pspProf[PSPPROF_COPY] / n), (uint32)(pspProf[PSPPROF_GUWAIT] / n), (uint32)(pspProf[PSPPROF_VBLANK] / n),
             (uint32)(pspProf[PSPPROF_AUDIO] / n), (uint32)(pspProf[PSPPROF_AUDLOCK] / n));
    PrintLog(PRINT_NORMAL, "PSP smooth: missed refreshes=%u worst frame=%uus scene=%s/%s", pspMissedRefreshes, pspWorstFrameUS,
             currentSceneFolder, currentSceneID);
    pspMissedRefreshes = 0;
    pspWorstFrameUS    = 0;
    PSPReportClassProfile(n);
    if (pspGeSyncTest)
        PrintLog(PRINT_NORMAL, "PSP gesync: blit=%uus/frame", (uint32)(pspBlitTestUS / n));
    pspBlitTestUS = 0;
    if (pspSkipFrames)
        PrintLog(PRINT_NORMAL, "PSP pacing: %u of %u frames at 30fps (two updates each), cost avg=%uus", pspSkipFrames, pspProfFrames,
                 (uint32)pspFrameCostAvg);
    pspSkipFrames = 0;
    if (pspFrameCheck)
        PrintLog(PRINT_NORMAL, "PSP framecheck: frames=%u bad stamps=%u back in time=%u repeats=%u", pspCheckFrames,
                 pspCheckBadStamp, pspCheckBack, pspCheckRepeat);
    if (pspLogEveryReport)
        PSPFlushLog(); // diagnostics: keep the Memory Stick log current in case the system locks up
    memset(pspProf, 0, sizeof(pspProf));
    pspProfFrames = 0;
}

static unsigned long long pspFrameTicks = 1000000 / 60; // mirrors RenderDevice::targetTicks (private)

// Called before the engine starts drawing a frame: the previous display list samples the engine
// framebuffers directly, so it has to be finished before they are overwritten. It usually already is,
// since the GE ran while the CPU updated objects.
void PSPApplyAtlasUpdates();
unsigned long long pspGeWaitedUS; // the CPU's waits for the GE so far (see PSPRotoPagesWanted)
void PSPWaitDisplayList()
{
    if (!pspGuReading)
        return;

    PSP_PROF_BEGIN(PSPPROF_GUWAIT);
    unsigned long long start = sceKernelGetSystemTimeWide();
    const char *phase        = (const char *)pspPhase;
    pspPhase                 = "frame blit wait";
    sceGuSync(0, 0);
    pspPhase = phase;
    pspGeWaitedUS += sceKernelGetSystemTimeWide() - start;
    PSP_PROF_END(PSPPROF_GUWAIT);
    pspGuReading = false;
    PSPApplyAtlasUpdates();
}

// ---------------------------------------------------------------------------------------------------------------
// 3D on the GE.
// The engine draws its 3D models in software: transform every face vertex, light it, depth-sort the faces, rasterize
// them one by one. On the PSP that per-face work (thousands of faces a frame in the special stages) cost more than the
// rest of the frame. Here the GE does it instead:
//  - models (PSPBegin3DModel/PSPEnd3DModel): the CPU only lights each unique vertex once, with the engine's own
//    formula, and the GE transforms, projects and depth-tests the model's triangles straight from its index list;
//  - single faces (PSPQueue3DFace), for the draw modes that keep the software transform: already projected, clipped
//    to the screen and drawn in submission order.
// A batch is drawn into a VRAM copy of the screen rectangle it touches, which is then copied back to the engine
// framebuffer. Any CPU drawing (PSP_FLUSH_3D in every draw entry point) flushes the batch first, so 2D and 3D still
// layer in the engine's order.
// ---------------------------------------------------------------------------------------------------------------
#define PSP_GE3D_DEPTH   (PSP_VRAM_DEPTH) // VRAM: 16-bit depth buffer for the models
#define PSP_GE3D_ARENA   (128 * 1024)
#define PSP_GE3D_SLICES  (32) // depth buffer slices per batch: one per scene with opaque models, later scenes in front (2048 levels each: fewer z-fight)
#define PSP_GE3D_FRONT   (GU_CCW) // front-face winding for models whose volume is positive
#define PSP_GE3D_NDC     (1024) // pixels from the screen centre to the edge of the clip volume
#define PSP_ROTO_GE_RUN  (12)   // rotozoom layers averaging fewer pixels per tile than this are drawn by the CPU
#define PSP_GE3D_ITEMS   (192)

struct PSPGe3DFaceVertex {
    uint32 color; // ABGR
    int16 x, y, z;
    int16 pad;
};

enum { PSP_GE3D_FACES, PSP_GE3D_MODEL, PSP_GE3D_LAYER, PSP_GE3D_SPRITE, PSP_GE3D_MASK, PSP_GE3D_LUT };
#define PSP_INK_FILL (0x40) // face items: FillScreen's per-channel blend (factors in maskRef / tbw)
#define PSP_INK_INVERT (0x41) // face items: 2-vertex rectangles inverting the screen's bits (INK_TINT with an inverting table)
#define PSP_INK_MASKRECT (0x42) // face items: 2-vertex opaque rectangles, INK_MASKED (as masked sprites, see PSP_GE3D_MASK)

struct PSPGe3DItem {
    int32 type;
    int32 inkEffect;
    uint32 offset; // of its vertices in the arena
    uint32 count;  // face vertices, model indices or layer sprite vertices
    uint32 clutOffset; // layers: their palette bank in the arena
    int32 prim;        // layers: GU_SPRITES (tile rows) or GU_LINES (rotozoom segments)
    const uint16 *indices;
    int32 centerX, centerY;
    int32 cull; // models: 0, or which winding of the projected faces is the back side
    int32 depthSlice; // models: their scene's slice of the depth buffer
    const void *tex;  // sprites: the sheet (8-bit texels in RAM), its row stride and the rows the texture may span
    int32 tbw, texH;
    bool32 rects;  // sprites: 2-vertex GU_SPRITES rectangles (deformed sprites' scanline pieces) instead of triangles
    bool32 masked; // sprites: only where the last PSP_GE3D_MASK item found the mask colour (INK_MASKED)
    uint32 maskRef; // mask items: the mask colour as the GE's colour test sees a 5650 texel (0xBBGGRR, low bits masked)
    bool32 scissored; // sprites: drawn within the screen's clip bounds of the time (sx1..sx2, sy1..sy2), not the whole batch
    bool32 clamped;   // layers with their own texture: clamped to its edges instead of repeating (rotozoom pages)
    bool32 linear;    // layers with their own texture: stored row by row, not swizzled (rotozoom rows without rotation)
    int16 sx1, sy1, sx2, sy2;
    ScePspFMatrix4 world, projection;
};

bool psp3DPending = false;
unsigned int psp3DFlushes, psp3DTriangles;
unsigned int psp3DItemTypes[8]; // (diagnostics: items drawn per type since the last perf report)
unsigned long long psp3DFlushUS;
unsigned long long psp3DFlushParts[4]; // writeback, list build, copy in + draw, copy out (us)
unsigned int psp3DCopiedPixels;

// In VRAM: the GE fetches model vertices by index all over it, and doing that in main memory, while the CPU updated
// the next frame, made that update up to twice as long.
static uint8 *pspGe3DArena;
static PSPGe3DItem pspGe3DItems[PSP_GE3D_ITEMS];
// Two arenas and lists: while the GE draws a batch kicked mid-frame from one (PSPKick3DMid), the CPU fills the other.
// The second arena lives where a second screen would (after the rotozoom layer texture), so it is only used while a
// single screen is: every waited-for kick goes back to the first.
#define PSP_VRAM_ARENA2 (PSP_VRAM_SCREEN(1) + 0x10000)
static uint8 *pspGe3DArenas[2];
static unsigned int __attribute__((aligned(16))) pspGe3DLists[2][8192];
static int32 pspGe3DCur;      // the arena and list being filled
static bool pspGe3DInFlight;  // a batch kicked mid-frame may still be drawing (from the other arena)
int pspGeOverlap = 1;         // boot.txt "nooverlap" turns it off
unsigned int pspGeMidKicks;   // (diagnostics)
#define pspGe3DList (pspGe3DLists[pspGe3DCur])
static int pspGe3DListMax;
static uint32 pspGe3DArenaUsed;
static uint32 pspGe3DBatchId = 1; // changes whenever the arena starts over
static int32 pspGe3DItemCount;
static bool pspGe3DUsesDepth;
static int32 pspGe3DSlices, pspGe3DSlice; // slices used in this batch, and the current scene's
static uint32 pspGe3DSceneSerial = 0xFFFFFFFF;
static int32 pspGe3DX1, pspGe3DY1, pspGe3DX2, pspGe3DY2; // batch bounds, in pixels
static ScreenInfo *pspGe3DScreen;

static uint8 *PSPTileAtlas() { return (uint8 *)sceGeEdramGetAddr() + PSP_VRAM_ATLAS; }
static uint8 *PSPSpriteCacheTexture();
static inline bool pspGe3DReady();

static const ScePspFMatrix4 pspGe3DIdentity = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };

static void PSPSetGe3DInk(int32 inkEffect)
{
    switch (inkEffect) {
        default:
        case INK_NONE: sceGuDisable(GU_BLEND); break;
        case INK_BLEND:
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x808080, 0x808080);
            break;
        case INK_ALPHA:
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
            break;
        case INK_ADD:
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFF);
            break;
        case INK_SUB:
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFF);
            break;
    }
}

extern int pspFreezeTest, pspStringGCTest; // (main.cpp)

// Waits for the GE like sceGuSync, but logs (and writes the log out) if it takes absurdly long, so a hang leaves a trace.
static void PSPGeWait(const char *what)
{
    unsigned long long start = PSPProfNow();
    const char *phase        = (const char *)pspPhase;
    pspPhase                 = what; // ("flush", "overlap": waiting for the GE)
    sceGeDrawSync(0); // the GE's interrupt wakes us as soon as it is done
    pspPhase = phase;
    pspGeWaitedUS += PSPProfNow() - start;
    if (PSPProfNow() - start > 300000) {
        PrintLog(PRINT_NORMAL, "PSP GE slow in %s: %u us", what, (uint32)(PSPProfNow() - start));
        PSPFlushLog();
    }
}

static void PSPKick3D(bool wait);
void PSPFlush3D() { PSPKick3D(true); }

// The frame's last batch: queued without waiting, so the GE draws it while the CPU moves on to the next frame
// (FlipScreen queues the blit right behind it, and drawing the next frame waits for both).
int pspGeSyncTest = 0; // boot.txt "gesync": every batch waited for, so the waits add up to the GE's busy time
// The frame's last batch is finished before the frame blit is queued. Queued right behind it while it still ran, the
// blit was intermittently lost once sprites sampled their VRAM cache in that batch (the LCD kept an old frame: heavy
// flicker in the special stage), for reasons not pinned down; waiting costs ~0.7ms a frame there.
static void PSPFlush3DAsync() { PSPKick3D(true); }

// boot.txt "dumpframes=START,COUNT,STEP": the finished engine frame (screen 0, BGR565, pitch x height) written to
// host0:/frames/fNNNNN.raw every STEP frames from frame START on, COUNT times (to see glitches frame by frame; slow).
int pspDumpStart = -1, pspDumpCount = 0, pspDumpStep = 1;
int32 pspFrameIndex; // frames finished so far
int32 pspSceneStartFrame = -1; // pspFrameIndex when the last scene finished loading (RetroEngine.cpp)
static void PSPDumpFrame()
{
    int32 f = pspFrameIndex++;
    if (pspDumpStart < 0 || f < pspDumpStart || pspDumpCount <= 0 || (f - pspDumpStart) % pspDumpStep)
        return;
    pspDumpCount--;
    ScreenInfo *scr = &screens[0];
    sceKernelDcacheWritebackInvalidateAll();
    char path[64];
    sprintf(path, "host0:/frames/f%05d.raw", (int)f);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    uint32 header[2] = { (uint32)scr->pitch, (uint32)scr->size.y };
    sceIoWrite(fd, header, sizeof(header));
    // through a buffer in RAM: the USB driver can't read VRAM (writing straight from it hung the PSP)
    static uint16 __attribute__((aligned(64))) chunk[512 * 16];
    const uint16 *src = (const uint16 *)((uintptr_t)scr->frameBuffer | 0x40000000);
    for (int32 row = 0; row < scr->size.y; row += 16) {
        int32 rows = MIN(16, scr->size.y - row);
        memcpy(chunk, &src[row * scr->pitch], rows * scr->pitch * 2);
        sceKernelDcacheWritebackRange(chunk, sizeof(chunk));
        sceIoWrite(fd, chunk, rows * scr->pitch * 2);
    }
    sceIoClose(fd);
}

extern int pspGeSyncTest;
static uint32 pspArenaSumsBefore[PSP_GE3D_ARENA / 1024], pspArenaBad;
static void PSPArenaChecksum(uint32 *sums)
{
    const uint32 *p = (const uint32 *)((uintptr_t)pspGe3DArena | 0x40000000);
    for (uint32 c = 0; c < (pspGe3DArenaUsed + 1023) / 1024; ++c) {
        uint32 h = 0;
        for (int32 i = 0; i < 256; ++i) h = h * 31 + p[c * 256 + i];
        sums[c] = h;
    }
}
// Rotozoom rows crossing a tile every few pixels cost the GE walk a segment (a few hundred cycles) for each: the CPU
// samples those rows itself, as palette indices into this buffer in RAM, which the GE then draws 1:1 in the layer's
// place with the row's palette (PSPRotoIndexRow). The GE reads it later in the frame: rows are handed out until the GE
// is next idle.
#define PSP_ROTO_INDEX_ROWS (256)
static uint8 *pspRotoIndexBuffer;
static int32 pspRotoIndexUsed;
// Rows of rotozoom layers without rotation (PSPRotoFlatRow): 512 texels each, swizzled, handed out like the index rows
#define PSP_ROTO_FLAT_ROWS (256)
static uint8 *pspRotoFlatBuffer;
static int32 pspRotoFlatUsed;

static void PSPGe3DUseFirstArena()
{
    pspGe3DCur       = 0;
    pspGe3DArena     = pspGe3DArenas[0];
    pspRotoIndexUsed = 0; // (the GE is idle)
    pspRotoFlatUsed  = 0;
}

// mid: kicked mid-frame without waiting, the CPU going on with the other arena (PSPKick3DMid)
static void PSPKick3DEx(bool wait, bool mid);
static void PSPKick3D(bool wait) { PSPKick3DEx(wait, false); }

// Models in a batch draw on depth cleared to 0: put it back over what a mask item set (see PSP_GE3D_MASK)
static void PSPGeClearDepth(ScreenInfo *screen)
{
    struct PSPClearVertex {
        uint32 color;
        int16 x, y, z, pad;
    };
    PSPClearVertex *cv = (PSPClearVertex *)sceGuGetMemory(2 * sizeof(PSPClearVertex));
    cv[0].color = cv[1].color = 0;
    cv[0].x = 0, cv[0].y = 0, cv[0].z = 0;
    cv[1].x = screen->size.x, cv[1].y = screen->size.y, cv[1].z = 0;
    sceGuSendCommandi(211, (GU_DEPTH_BUFFER_BIT << 8) | 0x01); // clear mode: depth only
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, cv);
    sceGuSendCommandi(211, 0);
}

static bool PSPGeItemMasked(const PSPGe3DItem *item);

extern int pspRotoPagesDebug; // (debugging, boot.txt pagesdbg=N: 1 page lines untextured, 2 from the atlas, 3 not drawn, 4 unswizzled)
// (debugging, boot.txt dumpframes: the GE batches of the frames being dumped)
#define PSP_DUMPING (pspDumpStart >= 0 && pspDumpCount > 0 && pspFrameIndex >= pspDumpStart)
static void PSPKick3DEx(bool wait, bool mid)
{
    if (PSP_DUMPING)
        PrintLog(PRINT_NORMAL, "PSP dbg f%d kick mid=%d wait=%d items=%d arena=%u cur=%d inflight=%d rect=%d,%d-%d,%d", pspFrameIndex, (int)mid,
                 (int)wait, pspGe3DItemCount, pspGe3DArenaUsed, pspGe3DCur, (int)pspGe3DInFlight, pspGe3DX1, pspGe3DY1, pspGe3DX2, pspGe3DY2);
    psp3DPending = false;
    if (!pspGe3DItemCount) {
        if (pspGe3DInFlight) {
            // nothing new, but what was kicked mid-frame must be finished all the same
            unsigned long long start = PSPProfNow();
            PSPGeWait("overlap");
            pspGe3DInFlight = false;
            PSPGe3DUseFirstArena();
            psp3DFlushUS += PSPProfNow() - start;
        }
        return;
    }

    unsigned long long start = PSPProfNow();
    PSPWaitDisplayList(); // never share the GE with the previous frame's blit
    if (pspGe3DInFlight) {
        PSPGeWait("overlap"); // one batch on the GE at a time (the other arena is then free again)
        pspGe3DInFlight = false;
    }

    ScreenInfo *screen = pspGe3DScreen;
    int32 x1 = pspGe3DX1, y1 = pspGe3DY1;
    int32 w = pspGe3DX2 - x1, h = pspGe3DY2 - y1;
    void *target = (void *)((uint8 *)screen->frameBuffer - (uint8 *)sceGeEdramGetAddr()); // the frame's VRAM offset

    // the GE reads vertices and indices from RAM and draws into the frame the CPU draws into: push out the CPU's dirty
    // lines and drop the rest, so nothing stale is read from the cache once the GE is done
    sceKernelDcacheWritebackInvalidateAll();
    if (pspGeSyncTest)
        PSPArenaChecksum(pspArenaSumsBefore);
    unsigned long long t1 = PSPProfNow();
    psp3DFlushParts[0] += t1 - start;
    psp3DCopiedPixels += w * h;

    sceGuStart(GU_DIRECT, pspGe3DList);
    sceGuDrawBufferList(GU_PSM_5650, target, screen->pitch);
    sceGuScissor(x1, y1, w, h); // this pspsdk's sceGuScissor takes a size (it ends at x + w - 1)
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DITHER);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);

    if (pspGe3DUsesDepth) {
        // closer is larger: near maps to 65535, cleared to 0 (farthest) over the batch rectangle
        sceGuDepthBuffer((void *)PSP_GE3D_DEPTH, screen->pitch);
        sceGuDepthRange(65535, 0);
        sceGuDepthFunc(GU_GEQUAL);
        // clear only the batch rectangle (sceGuClear covers the whole drawing area)
        struct PSPClearVertex {
            uint32 color;
            int16 x, y, z, pad;
        };
        PSPClearVertex *cv = (PSPClearVertex *)sceGuGetMemory(2 * sizeof(PSPClearVertex));
        cv[0].color = cv[1].color = 0;
        cv[0].x = x1, cv[0].y = y1, cv[0].z = 0;
        cv[1].x = x1 + w, cv[1].y = y1 + h, cv[1].z = 0;
        sceGuSendCommandi(211, (GU_DEPTH_BUFFER_BIT << 8) | 0x01); // clear mode: depth only
        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, cv);
        sceGuSendCommandi(211, 0);
        sceGuEnable(GU_CLIP_PLANES);
        sceGuSetMatrix(GU_VIEW, &pspGe3DIdentity);
        // the projection matrix does the engine's (v << projection) / z scaled down by PSP_GE3D_NDC so the screen stays
        // well inside the clip volume (hardware drops vertices outside it), and the viewport scales it back
        sceGuViewport(2048, 2048, 2 * PSP_GE3D_NDC, 2 * PSP_GE3D_NDC);
    }

    sceGuTexFlush(); // sheets may have been reloaded since the GE last cached them
    bool texturing = false, atlasBound = false;
    bool modelView = pspGe3DUsesDepth; // the view and viewport models project through are set (see below)
    int32 scissorX1 = x1, scissorY1 = y1, scissorX2 = x1 + w, scissorY2 = y1 + h; // what the GE clips to now
    for (int32 i = 0; i < pspGe3DItemCount; ++i) {
        PSPGe3DItem *item = &pspGe3DItems[i];
        if (!item->count)
            continue;
        psp3DItemTypes[item->type & 7]++;

        // sprites queued while the game narrowed the clip bounds (PBL_Crane's window) clip to them; everything else
        // to the batch
        int32 cx1 = x1, cy1 = y1, cx2 = x1 + w, cy2 = y1 + h;
        if (item->type == PSP_GE3D_SPRITE && item->scissored)
            cx1 = item->sx1, cy1 = item->sy1, cx2 = item->sx2, cy2 = item->sy2;
        if (cx1 != scissorX1 || cy1 != scissorY1 || cx2 != scissorX2 || cy2 != scissorY2) {
            sceGuScissor(cx1, cy1, cx2 - cx1, cy2 - cy1);
            scissorX1 = cx1, scissorY1 = cy1, scissorX2 = cx2, scissorY2 = cy2;
        }

        if (item->type == PSP_GE3D_LUT) {
            // FillScreen (PSPQueueFillScreen): the screen as a T16 texture, one pass per channel through its CLUT,
            // the other channels' bits write-protected
            sceGuDisable(GU_BLEND);
            sceGuDisable(GU_ALPHA_TEST);
            sceGuDisable(GU_DEPTH_TEST);
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(GU_PSM_T16, 0, 0, 0);
            sceGuTexImage(0, 512, 256, screen->pitch, item->tex);
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            sceGuTexWrap(GU_CLAMP, GU_CLAMP);
            const int32 strips = (screen->size.x + 63) / 64;
            struct PSPLutVertex {
                int16 u, v, x, y, z;
            };
            PSPLutVertex *lv = (PSPLutVertex *)sceGuGetMemory(2 * strips * sizeof(PSPLutVertex));
            for (int32 s = 0; s < strips; ++s) {
                int16 sx0 = s * 64, sx1 = MIN(sx0 + 64, screen->size.x);
                lv[s * 2 + 0] = { sx0, 0, sx0, 0, 0 };
                lv[s * 2 + 1] = { sx1, (int16)screen->size.y, sx1, (int16)screen->size.y, 0 };
            }
            static const struct {
                int32 shift, mask, entry, blocks;
                uint32 protect; // 0xBBGGRR bits not written
            } passes[3] = { { 0, 0x1F, 0, 4, 0xFFFF00 }, { 5, 0x3F, 32, 8, 0xFF00FF }, { 11, 0x1F, 96, 4, 0x00FFFF } };
            for (int32 p = 0; p < 3; ++p) {
                sceGuTexFlush(); // (the previous pass changed the screen it reads)
                sceGuClutMode(GU_PSM_8888, passes[p].shift, passes[p].mask, 0);
                sceGuClutLoad(passes[p].blocks, &pspGe3DArena[item->clutOffset + passes[p].entry * 4]);
                sceGuPixelMask(passes[p].protect);
                sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2 * strips, 0, lv);
            }
            sceGuPixelMask(0);
            texturing  = true;
            atlasBound = false;
            continue;
        }

        if (item->type == PSP_GE3D_MASK) {
            // INK_MASKED's test, done by the GE: the screen read back as a 5650 texture through the colour test leaves
            // depth 0 where it shows the mask colour, 65535 elsewhere (colour writes off). Masked sprites then pass only
            // where the depth is below their z (GEQUAL) and write it: each quad has a lower z than the one before
            // (PSPGeMaskZ), so - as INK_MASKED in software, testing the screen as it goes - what one drew is no longer
            // the mask colour for the ones after.
            sceGuDepthBuffer((void *)PSP_GE3D_DEPTH, screen->pitch);
            struct PSPMaskVertex {
                uint32 color;
                int16 x, y, z, pad;
            };
            PSPMaskVertex *cv = (PSPMaskVertex *)sceGuGetMemory(2 * sizeof(PSPMaskVertex));
            cv[0].color = cv[1].color = 0;
            cv[0].x = 0, cv[0].y = 0, cv[0].z = 0;
            cv[1].x = screen->size.x, cv[1].y = screen->size.y, cv[1].z = 0;
            sceGuSendCommandi(211, (GU_DEPTH_BUFFER_BIT << 8) | 0x01); // clear mode: depth only
            sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, cv);
            sceGuSendCommandi(211, 0);

            sceGuPixelMask(0xFFFFFFFF); // no colour writes
            sceGuDisable(GU_BLEND);
            sceGuDisable(GU_ALPHA_TEST);
            sceGuEnable(GU_DEPTH_TEST);
            sceGuDepthFunc(GU_ALWAYS);
            sceGuDepthMask(GU_FALSE);
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(GU_PSM_5650, 0, 0, 0);
            sceGuTexImage(0, 512, 256, screen->pitch, item->tex);
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            sceGuTexWrap(GU_CLAMP, GU_CLAMP);
            sceGuTexFlush();
            sceGuEnable(GU_COLOR_TEST);
            sceGuColorFunc(GU_NOTEQUAL, item->maskRef, 0xF8FCF8); // (the pixels that aren't the mask colour get 65535)
            const int32 strips = (screen->size.x + 63) / 64;
            struct PSPMaskTexVertex {
                int16 u, v, x, y, z;
            };
            PSPMaskTexVertex *mv = (PSPMaskTexVertex *)sceGuGetMemory(2 * strips * sizeof(PSPMaskTexVertex));
            for (int32 s = 0; s < strips; ++s) {
                int16 sx0 = s * 64, sx1 = MIN(sx0 + 64, screen->size.x);
                mv[s * 2 + 0] = { sx0, 0, sx0, 0, (int16)0xFFFF };
                mv[s * 2 + 1] = { sx1, (int16)screen->size.y, sx1, (int16)screen->size.y, (int16)0xFFFF };
            }
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2 * strips, 0, mv);
            sceGuDisable(GU_COLOR_TEST);
            sceGuPixelMask(0);
            sceGuDisable(GU_DEPTH_TEST);
            sceGuDepthFunc(GU_GEQUAL); // what the models expect
            texturing  = true;
            atlasBound = false;
            continue;
        }

        if (item->type == PSP_GE3D_SPRITE) {
            // sprites: the sheet's 8-bit texels, its palette bank as the CLUT, the ink as blending (alpha in the vertices)
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(GU_PSM_T8, 0, 0, 0);
            sceGuTexImage(0, 512, item->texH, item->tbw, item->tex);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            sceGuTexWrap(GU_CLAMP, GU_CLAMP);
            sceGuEnable(GU_ALPHA_TEST); // palette index 0 is transparent
            sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
            if (item->masked) {
                // only over the pixels the mask item left below the quad's z, writing it (see PSP_GE3D_MASK)
                sceGuDepthBuffer((void *)PSP_GE3D_DEPTH, screen->pitch);
                sceGuEnable(GU_DEPTH_TEST);
                sceGuDepthFunc(GU_GEQUAL);
                sceGuDepthMask(GU_FALSE);
            }
            else {
                sceGuDisable(GU_DEPTH_TEST);
            }
            PSPSetGe3DInk(item->inkEffect);
            sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
            sceGuClutLoad(256 / 8, &pspGe3DArena[item->clutOffset]);
            sceGuDrawArray(item->rects ? GU_SPRITES : GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                           item->count, 0, &pspGe3DArena[item->offset]);
            if (item->masked) {
                sceGuDisable(GU_DEPTH_TEST);
                sceGuDepthMask(GU_FALSE);
                sceGuDepthFunc(GU_GEQUAL); // what the models expect
                // (not while the next item is masked too: it relies on this mask, see PSPGeMaskNewItem)
                if (pspGe3DUsesDepth && !(i + 1 < pspGe3DItemCount && PSPGeItemMasked(&pspGe3DItems[i + 1])))
                    PSPGeClearDepth(screen);
            }
            texturing  = true;
            atlasBound = false;
            continue;
        }

        if (item->type == PSP_GE3D_LAYER) {
            if (item->tex) {
                // a whole layer as one texture, repeating like the layer wraps (swizzled); or rows of palette indices
                // the CPU drew (sprites, 1:1)
                const bool indexRows = item->prim == GU_SPRITES;
                sceGuEnable(GU_TEXTURE_2D);
                if (item->clamped && pspRotoPagesDebug == 1)
                    sceGuDisable(GU_TEXTURE_2D);
                sceGuTexMode(GU_PSM_T8, 0, 0, indexRows || item->linear || (item->clamped && (pspRotoPagesDebug == 4 || pspRotoPagesDebug == 7)) ? 0 : 1);
                if (item->clamped && (pspRotoPagesDebug == 2 || pspRotoPagesDebug == 7))
                    sceGuTexImage(0, 512, 512, 512, PSPTileAtlas());
                else
                sceGuTexImage(0, item->tbw, item->texH, item->tbw, item->tex);
                sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
                sceGuTexFilter(GU_NEAREST, GU_NEAREST);
                const int32 wrap = indexRows || (item->clamped && pspRotoPagesDebug != 5 && pspRotoPagesDebug != 6) ? GU_CLAMP : GU_REPEAT;
                sceGuTexWrap(wrap, wrap);
                sceGuEnable(GU_ALPHA_TEST); // palette index 0 is transparent
                sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
                sceGuDisable(GU_BLEND);
                sceGuDisable(GU_DEPTH_TEST);
                texturing  = true;
                atlasBound = false;
            }
            else if (!atlasBound) {
                sceGuEnable(GU_TEXTURE_2D);
                sceGuTexMode(GU_PSM_T8, 0, 0, 0);
                sceGuTexImage(0, 512, 512, 512, PSPTileAtlas());
                sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
                sceGuTexFilter(GU_NEAREST, GU_NEAREST);
                sceGuTexWrap(GU_CLAMP, GU_CLAMP);
                sceGuEnable(GU_ALPHA_TEST); // palette index 0 is transparent
                sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
                sceGuDisable(GU_BLEND);
                sceGuDisable(GU_DEPTH_TEST);
                texturing  = true;
                atlasBound = true;
            }
            sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
            sceGuClutLoad(256 / 8, &pspGe3DArena[item->clutOffset]);
            if (item->prim == GU_TRIANGLE_STRIP)
                sceGuDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_32BITF | GU_VERTEX_16BIT | GU_TRANSFORM_2D, item->count, 0, &pspGe3DArena[item->offset]);
            else if (item->prim == GU_LINES && item->clamped && pspRotoPagesDebug == 3)
                ; // (debugging: the state set, nothing drawn)
            else if (item->prim == GU_LINES)
                sceGuDrawArray(GU_LINES, GU_TEXTURE_32BITF | GU_VERTEX_16BIT | GU_TRANSFORM_2D, item->count, 0, &pspGe3DArena[item->offset]);
            else
                sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, item->count, 0, &pspGe3DArena[item->offset]);
            continue;
        }
        if (texturing) {
            sceGuDisable(GU_TEXTURE_2D);
            sceGuDisable(GU_ALPHA_TEST);
            texturing  = false;
            atlasBound = false;
        }
        PSPSetGe3DInk(item->inkEffect);

        if (item->type == PSP_GE3D_MODEL) {
            if (!modelView) {
                // a batch of translucent models only (the special stages' shadows): no depth set-up above, but the
                // same projection (without it they were drawn through the frame blit's viewport: nowhere to be seen)
                sceGuEnable(GU_CLIP_PLANES);
                sceGuSetMatrix(GU_VIEW, &pspGe3DIdentity);
                sceGuViewport(2048, 2048, 2 * PSP_GE3D_NDC, 2 * PSP_GE3D_NDC);
                modelView = true;
            }
            // translucent models draw in order like the software did; opaque ones resolve their own faces by depth
            if (item->inkEffect == INK_NONE) {
                // its scene's slice: closer is larger, and every later scene sits above every earlier one
                const int32 span = 65536 / PSP_GE3D_SLICES;
                sceGuDepthRange(item->depthSlice * span + span - 1, item->depthSlice * span);
                sceGuEnable(GU_DEPTH_TEST);
                sceGuDepthMask(GU_FALSE);
            }
            else {
                sceGuDisable(GU_DEPTH_TEST);
            }
            if (item->cull) {
                sceGuEnable(GU_CULL_FACE);
                sceGuFrontFace(item->cull > 0 ? PSP_GE3D_FRONT : (PSP_GE3D_FRONT == GU_CW ? GU_CCW : GU_CW));
            }
            else {
                sceGuDisable(GU_CULL_FACE);
            }
            sceGuOffset(2048 - item->centerX, 2048 - item->centerY);
            sceGuSetMatrix(GU_PROJECTION, &item->projection);
            sceGuSetMatrix(GU_MODEL, &item->world);
            sceGuDrawArray(GU_TRIANGLES, GU_INDEX_16BIT | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, item->count, item->indices,
                           &pspGe3DArena[item->offset]);
            // culling stays on for everything drawn after it otherwise: mirrored sprites (the special stages' rings,
            // flipped for half their spin) are triangles wound the other way, and vanished
            if (item->cull)
                sceGuDisable(GU_CULL_FACE);
            psp3DTriangles += item->count / 3;
        }
        else {
            sceGuDisable(GU_DEPTH_TEST);
            if (item->inkEffect == PSP_INK_FILL) { // FillScreen: colour * alpha + screen * (255 - alpha), per channel
                sceGuEnable(GU_BLEND);
                sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, item->maskRef, (uint32)item->tbw);
            }
            if (item->inkEffect == PSP_INK_MASKRECT) { // opaque, only where the mask colour still is (see PSP_GE3D_MASK)
                sceGuDisable(GU_BLEND);
                sceGuDepthBuffer((void *)PSP_GE3D_DEPTH, screen->pitch);
                sceGuEnable(GU_DEPTH_TEST);
                sceGuDepthFunc(GU_GEQUAL);
                sceGuDepthMask(GU_FALSE);
                sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, item->count, 0, &pspGe3DArena[item->offset]);
                sceGuDisable(GU_DEPTH_TEST);
                if (pspGe3DUsesDepth && !(i + 1 < pspGe3DItemCount && PSPGeItemMasked(&pspGe3DItems[i + 1])))
                    PSPGeClearDepth(screen);
                continue;
            }
            if (item->inkEffect == PSP_INK_INVERT) { // every bit of the 16-bit pixels flipped: 0xFFFF - pixel, exactly
                sceGuEnable(GU_COLOR_LOGIC_OP);
                sceGuLogicalOp(GU_INVERTED);
                sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, item->count, 0, &pspGe3DArena[item->offset]);
                sceGuDisable(GU_COLOR_LOGIC_OP);
            }
            else
            sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, item->count, 0, &pspGe3DArena[item->offset]);
            psp3DTriangles += item->count / 3;
        }
    }
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_CULL_FACE);
    // back to what the frame blit expects (SetupRendering)
    sceGuOffset(2048 - (PSP_SCR_WIDTH / 2), 2048 - (PSP_SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, PSP_SCR_WIDTH, PSP_SCR_HEIGHT);
    sceGuScissor(0, 0, PSP_SCR_WIDTH, PSP_SCR_HEIGHT);
    sceGuFinish();
    {
        int used = sceGuCheckList();
        if (used > pspGe3DListMax) {
            pspGe3DListMax = used;
            PrintLog(PRINT_NORMAL, "PSP 3d list: %d of %d bytes", used, (int)sizeof(pspGe3DList));
        }
    }
    unsigned long long t2 = PSPProfNow();
    psp3DFlushParts[1] += t2 - t1;
    if (wait && !mid) {
        PSPGeWait("flush"); // the CPU may draw over these pixels next
        if (pspGeSyncTest) {
            static uint32 after[PSP_GE3D_ARENA / 1024];
            PSPArenaChecksum(after);
            for (uint32 c = 0; c < (pspGe3DArenaUsed + 1023) / 1024; ++c) {
                if (after[c] != pspArenaSumsBefore[c]) {
                    if (pspArenaBad < 20)
                        PrintLog(PRINT_NORMAL, "PSP arena changed by the GE: chunk %u of %u (items %d)", c, (pspGe3DArenaUsed + 1023) / 1024, pspGe3DItemCount);
                    pspArenaBad++;
                    break;
                }
            }
        }
        psp3DFlushParts[2] += PSPProfNow() - t2;
    }
    else if (mid) {
        // drawn while the CPU queues the next batch into the other arena; every flush, and the next kick, wait for it
        pspGe3DInFlight = true;
        pspGeMidKicks++;
    }
    else {
        pspGuReading = true; // the next frame's drawing waits for it (PSPWaitDisplayList)
    }
    psp3DFlushes++;
    pspGe3DArenaUsed  = 0;
    pspGe3DBatchId++;
    pspGe3DItemCount  = 0;
    pspGe3DUsesDepth  = false;
    pspGe3DSlices      = 0;
    pspGe3DSceneSerial = 0xFFFFFFFF;
    if (mid) {
        pspGe3DCur ^= 1;
        pspGe3DArena = pspGe3DArenas[pspGe3DCur];
        psp3DPending = true; // (so every flush point waits for the batch in flight)
    }
    else if (wait) {
        PSPGe3DUseFirstArena();
    }
    psp3DFlushUS += PSPProfNow() - start;
}

// Between two objects' draws: once the batch has grown and the GE has nothing left to draw, hand it over, so the GE
// works through the objects while the CPU queues the rest instead of all at the end of the frame. (Not more often:
// each kick costs a cache writeback and a list, and instances of one model reuse their lit vertices within a batch.)
static void PSPKick3DMid();
int pspGeKickKB = 16; // boot.txt gekick=N (0: never)
unsigned int pspGeIdleKicks, pspGeBusySkips; // (diagnostics)
unsigned int pspGeStateSeen[8];
int pspLayerKickWait = 0; // boot.txt "layerkickwait": after a rotozoom layer, kick even if the GE is busy (waiting for it)
int pspGeBusyState = -1; // boot.txt gebusy=N: the sceGeDrawSync(1) answer that means "still drawing" (-1: any but 0)
// Whether the batch kicked mid-frame is still being drawn (a kick now would wait for it).
static bool PSPGeBusy()
{
    if (!pspGe3DInFlight)
        return false;
    int state = sceGeDrawSync(1);
    pspGeStateSeen[state & 7]++;
    return pspGeBusyState < 0 ? state != 0 : state == pspGeBusyState;
}
void PSPKick3DIfIdle()
{
    if (!pspGeKickKB || !pspGe3DItemCount || pspGe3DArenaUsed < (uint32)pspGeKickKB * 1024)
        return;
    if (PSPGeBusy()) {
        pspGeBusySkips++;
        return;
    }
    pspGeIdleKicks++;
    PSPKick3DMid();
}

// Starts drawing what is queued so far while the CPU goes on (only between layers: models of one scene must share a
// batch, for its depth buffer). Without a single screen there is no second arena: the batch then just keeps growing.
static void PSPKick3DMid()
{
    if (!pspGeOverlap || !pspGe3DItemCount || videoSettings.screenCount != 1 || pspGe3DScreen != &screens[0])
        return;
    PSPKick3DEx(false, true);
}

void PSPFlush3DRect(int x1, int y1, int x2, int y2)
{
    if (pspGe3DScreen != currentScreen || (x1 < pspGe3DX2 && x2 > pspGe3DX1 && y1 < pspGe3DY2 && y2 > pspGe3DY1))
        PSPFlush3D();
}

// Room for a new item and its vertices (flushing the batch when full); NULL if it can never fit.
static void *PSPGe3DReserve(uint32 bytes, int32 type, int32 inkEffect)
{
    bytes = (bytes + 3) & ~3; // everything in there is made of 4-byte fields
    if (bytes > PSP_GE3D_ARENA)
        return NULL;

    if (psp3DPending && pspGe3DScreen != currentScreen)
        PSPFlush3D();
    if (pspGe3DArenaUsed + bytes > PSP_GE3D_ARENA || pspGe3DItemCount == PSP_GE3D_ITEMS)
        PSPFlush3D();

    PSPGe3DItem *last = pspGe3DItemCount ? &pspGe3DItems[pspGe3DItemCount - 1] : NULL;
    bool extend       = type == PSP_GE3D_FACES && inkEffect != PSP_INK_FILL && inkEffect != PSP_INK_INVERT && inkEffect != PSP_INK_MASKRECT && last
                  && last->type == PSP_GE3D_FACES
                  && last->inkEffect == inkEffect
                  && last->offset + last->count * sizeof(PSPGe3DFaceVertex) == pspGe3DArenaUsed;
    if (!extend) {
        PSPGe3DItem *item = &pspGe3DItems[pspGe3DItemCount++];
        item->type        = type;
        item->inkEffect   = inkEffect;
        item->offset      = pspGe3DArenaUsed;
        item->count       = 0;
    }

    void *data = &pspGe3DArena[pspGe3DArenaUsed];
    pspGe3DArenaUsed += bytes;
    return data;
}

static void PSPGe3DGrowBounds(int32 x1, int32 y1, int32 x2, int32 y2)
{
    if (!psp3DPending) {
        pspGe3DScreen = currentScreen;
        pspGe3DX1 = x1, pspGe3DY1 = y1, pspGe3DX2 = x2, pspGe3DY2 = y2;
        psp3DPending = true;
    }
    else {
        pspGe3DX1 = MIN(pspGe3DX1, x1), pspGe3DY1 = MIN(pspGe3DY1, y1);
        pspGe3DX2 = MAX(pspGe3DX2, x2), pspGe3DY2 = MAX(pspGe3DY2, y2);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Tile layers on the GE.
// The tileset (unflipped tiles) is kept as a 512x512 8-bit texture in VRAM, the palette bank of each band of
// scanlines becomes the GE's CLUT (index 0 transparent through the alpha test), and an HScroll layer is drawn as
// 16-pixel-wide sprites, one per tile per run of scanlines sharing the same scroll, row continuity and palette bank.
// Flipped tiles mirror their texture coordinates. The CPU only walks the layout; the GE does all the pixels.
// ---------------------------------------------------------------------------------------------------------------
struct PSPGeSpriteVertex {
    uint16 u, v;
    int16 x, y, z;
};

int pspGeLayers       = 1;
int pspLogEveryReport = 0; // boot.txt "logflush"
 // boot.txt "nogelayers" turns it off, for comparisons
unsigned int pspGeLayerSprites;
unsigned long long pspGeRotoGenUS, pspGeRotoFlushUS, pspGeClutUS;
unsigned int pspRotoWalkRuns, pspGeClutCount; // (diagnostics)
unsigned long long pspGenItemUS, pspGenInitUS;
unsigned int pspRotoGeRows, pspRotoCpuRows, pspRotoLayerW, pspRotoLayerH; // diagnostics // rotozoom layers: CPU walk vs GE waits inside it

// Tiles changed while the previous frame's last batch may still be sampling the atlas (animated tiles are updated by
// objects' Update, during that batch): copied in once it is done, before this frame queues any layer.
static int32 pspAtlasDirtyLo = TILE_COUNT, pspAtlasDirtyHi = 0;
static uint32 pspTileSerial = 1; // changes with every tileset change

static void PSPCopyTilesToAtlas(int tile, int count)
{
    uint8 *atlas = PSPTileAtlas();
    struct AtlasFlush { // (written back on the way out, see PSPCacheSpriteFrame)
        uint8 *start;
        uint32 size;
        ~AtlasFlush() { sceKernelDcacheWritebackRange(start, size); }
    } atlasFlush = { &atlas[((tile >> 5) << 4) * 512], (uint32)((((tile + count - 1) >> 5) - (tile >> 5) + 1) * 16 * 512) };
    for (int32 t = tile; t < tile + count; ++t) {
        const uint32 *src = (const uint32 *)&tilesetPixels[t * TILE_DATASIZE];
        uint8 *dst        = &atlas[((t >> 5) << 4) * 512 + ((t & 31) << 4)];
        for (int32 r = 0; r < TILE_SIZE; ++r, src += 4, dst += 512) {
            uint32 *row = (uint32 *)dst;
            row[0]      = src[0];
            row[1]      = src[1];
            row[2]      = src[2];
            row[3]      = src[3];
        }
    }
}

void PSPApplyAtlasUpdates()
{
    if (pspAtlasDirtyLo < pspAtlasDirtyHi)
        PSPCopyTilesToAtlas(pspAtlasDirtyLo, pspAtlasDirtyHi - pspAtlasDirtyLo);
    pspAtlasDirtyLo = TILE_COUNT;
    pspAtlasDirtyHi = 0;
}

// Copies tiles [tile, tile + count) of the unflipped tileset into the atlas (32 tiles per row).
void PSPUpdateTileAtlas(int tile, int count)
{
    if (tile < 0)
        tile = 0;
    if (tile + count > TILE_COUNT)
        count = TILE_COUNT - tile;
    if (count <= 0)
        return;

    pspTileSerial++;
    PSP_FLUSH_3D(); // this frame's queued layers sample the old tiles
    if (pspGuReading) {
        pspAtlasDirtyLo = MIN(pspAtlasDirtyLo, tile);
        pspAtlasDirtyHi = MAX(pspAtlasDirtyHi, tile + count);
        return;
    }
    PSPApplyAtlasUpdates();
    PSPCopyTilesToAtlas(tile, count);
}

// A fresh layer item for one palette bank: the CLUT is a snapshot of the bank as it is now, like the software path
// reading the palette while drawing.
static int32 pspGeLayerX1, pspGeLayerY1, pspGeLayerX2, pspGeLayerY2; // the layer being queued
static void PSPKick3DMid();
static void PSPGeLayerFlush()
{
    if (pspGeOverlap && videoSettings.screenCount == 1 && pspGe3DScreen == &screens[0])
        PSPKick3DMid();
    else
        PSPFlush3D();
    PSPGe3DGrowBounds(pspGeLayerX1, pspGeLayerY1, pspGeLayerX2, pspGeLayerY2); // what follows still belongs to it
}

// Rotozoom segments: one textured line per run of pixels sampling the same tile (float texel coordinates).
struct PSPGeLineVertex {
    float u, v;
    int16 x, y, z;
    int16 pad;
};

// The layer item to append to for this palette bank and primitive, starting a new one (with a snapshot of the bank as
// the CLUT, like the software path reading the palette while drawing) when needed. Guarantees room for reserveBytes
// of vertices after it.
static PSPGe3DItem *PSPGeLayerItem(int32 bank, int32 prim, uint32 reserveBytes, const void *tex = NULL, int32 texW = 0, int32 texH = 0)
{
    uint32 vertexSize = prim != GU_SPRITES ? sizeof(PSPGeLineVertex) : sizeof(PSPGeSpriteVertex);
    PSPGe3DItem *last = pspGe3DItemCount ? &pspGe3DItems[pspGe3DItemCount - 1] : NULL;
    if (last && last->type == PSP_GE3D_LAYER && last->inkEffect == bank && last->prim == prim && last->tex == tex
        && last->offset + last->count * vertexSize == pspGe3DArenaUsed && pspGe3DArenaUsed + reserveBytes <= PSP_GE3D_ARENA)
        return last;

    // The bank's CLUT: the one already in this batch while the bank is unchanged, else a fresh copy (converted again
    // only when the bank changed since it last was).
    static uint32 clutBatch[PALETTE_BANK_COUNT], clutAt[PALETTE_BANK_COUNT];
    static uint16 clutSource[PALETTE_BANK_COUNT][PALETTE_BANK_SIZE];
    static uint32 clutConverted[PALETTE_BANK_COUNT][PALETTE_BANK_SIZE];
    static bool clutValid[PALETTE_BANK_COUNT];
    unsigned long long clutStart = PSPProfFine();
    bank &= PALETTE_BANK_COUNT - 1;
    const uint16 *colors = fullPalette[bank];
    const bool unchanged = clutValid[bank] && !memcmp(clutSource[bank], colors, sizeof(clutSource[bank]));
    bool reuse           = unchanged && clutBatch[bank] == pspGe3DBatchId;
    // (vertices on whole cache lines: see the rotozoom walk's uncached stores)
    uint32 vertexOffset = (pspGe3DArenaUsed + 63) & ~63, clutOffset = vertexOffset;
    if (!reuse)
        vertexOffset += 1024;
    if (vertexOffset + reserveBytes > PSP_GE3D_ARENA || pspGe3DItemCount == PSP_GE3D_ITEMS) {
        PSPGeLayerFlush();
        reuse      = false;
        clutOffset = 0;
        vertexOffset = 1024;
    }
    if (reuse) {
        clutOffset = clutAt[bank];
    }
    else {
        pspGeClutCount++;
        if (!unchanged) {
            uint32 *conv = clutConverted[bank];
            conv[0]      = 0; // transparent
            for (int32 c = 1; c < 0x100; ++c) {
                uint32 color = colors[c]; // R in the low bits, like the GE's 5650
                uint32 r = color & 0x1F, g = (color >> 5) & 0x3F, b = (color >> 11) & 0x1F;
                conv[c] = 0xFF000000 | (((b << 3) | (b >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2));
            }
            memcpy(clutSource[bank], colors, sizeof(clutSource[bank]));
            clutValid[bank] = true;
        }
        memcpy(&pspGe3DArena[clutOffset], clutConverted[bank], 1024);
        clutBatch[bank] = pspGe3DBatchId;
        clutAt[bank]    = clutOffset;
    }
    pspGeClutUS += PSPProfFine() - clutStart;
    PSPGe3DItem *item = &pspGe3DItems[pspGe3DItemCount++];
    item->type        = PSP_GE3D_LAYER;
    item->inkEffect   = bank; // layers have no ink: the field keys the bank
    item->prim        = prim;
    item->tex         = tex; // NULL: the tile atlas
    item->clamped     = false;
    item->linear      = false;
    item->tbw         = texW;
    item->texH        = texH;
    item->clutOffset  = clutOffset;
    item->offset      = vertexOffset;
    item->count       = 0;
    pspGe3DArenaUsed  = item->offset;
    return item;
}

// One band of screen rows [sy, sy + rows) showing rows [tileY, tileY + rows) of one row of tiles: tiles from layout[tx]
// on (wrapping at the layer width), the first one starting at screen x sx, cropped to [xClip1, xClip2).
static void PSPGeEmitTileRow(TileLayer *layer, const uint16 *layout, int32 tx, int32 sx, int32 xClip1, int32 xClip2, int32 tileY, int32 rows,
                             int32 sy, int32 bank)
{
    uint32 maxBytes = (((xClip2 - sx) >> 4) + 2) * 2 * sizeof(PSPGeSpriteVertex);
    PSPGe3DItem *item = PSPGeLayerItem(bank, GU_SPRITES, maxBytes);

    PSPGeSpriteVertex *v     = (PSPGeSpriteVertex *)&pspGe3DArena[pspGe3DArenaUsed];
    PSPGeSpriteVertex *start = v;
    for (; sx < xClip2; sx += TILE_SIZE) {
        uint16 entry = layout[tx];
        if (++tx == layer->xsize)
            tx = 0;
        if (entry >= 0xFFFF)
            continue;

        int32 tile = entry & 0x3FF;
        int32 tu = (tile & 31) << 4, tv = (tile >> 5) << 4;
        int32 cropL = sx < xClip1 ? xClip1 - sx : 0;
        int32 cropR = sx + TILE_SIZE > xClip2 ? sx + TILE_SIZE - xClip2 : 0;
        int32 u0, u1, v0, v1;
        if (entry & 0x400) { // FLIP_X variant: mirrored columns
            u0 = tu + TILE_SIZE - cropL;
            u1 = tu + cropR;
        }
        else {
            u0 = tu + cropL;
            u1 = tu + TILE_SIZE - cropR;
        }
        if (entry & 0x800) { // FLIP_Y variant: mirrored rows
            v0 = tv + TILE_SIZE - tileY;
            v1 = v0 - rows;
        }
        else {
            v0 = tv + tileY;
            v1 = v0 + rows;
        }

        v[0].u = u0, v[0].v = v0, v[0].x = sx + cropL, v[0].y = sy, v[0].z = 0;
        v[1].u = u1, v[1].v = v1, v[1].x = sx + TILE_SIZE - cropR, v[1].y = sy + rows, v[1].z = 0;
        v += 2;
    }

    uint32 used = (uint32)(v - start);
    item->count += used;
    pspGe3DArenaUsed += used * sizeof(PSPGeSpriteVertex);
    pspGeLayerSprites += used / 2;
}

static bool32 PSPGeLayerBegin(TileLayer *layer, int32 x1, int32 y1, int32 x2, int32 y2)
{
    if (psp3DPending && pspGe3DScreen != currentScreen)
        PSPFlush3D();
    if (PSP_DUMPING)
        PrintLog(PRINT_NORMAL, "PSP dbg f%d layer %d rows %d-%d items=%d arena=%u cur=%d", pspFrameIndex, (int32)(layer - tileLayers), y1, y2,
                 pspGe3DItemCount, pspGe3DArenaUsed, pspGe3DCur);
    pspGeLayerX1 = x1, pspGeLayerY1 = y1, pspGeLayerX2 = MIN(x2, PSP_BUF_WIDTH), pspGeLayerY2 = y2;
    PSPGe3DGrowBounds(pspGeLayerX1, pspGeLayerY1, pspGeLayerX2, pspGeLayerY2);
    return true;
}

// Same pixels as DrawLayerHScroll: per scanline, the layer row at position.y scrolled to position.x (wrapped once
// into the layer width), drawn across the whole pitch, in the scanline's palette bank.
bool32 RSDK::PSPQueueLayerHScroll(TileLayer *layer)
{
    if (!pspGeLayers)
        return false;
    if (!layer->xsize || !layer->ysize)
        return true;

    ScreenInfo *screen = currentScreen;
    int32 top = screen->clipBound_Y1, bottom = screen->clipBound_Y2;
    if (top >= bottom)
        return true;

    const int32 pitch      = screen->pitch;
    const int32 layerWidth = TILE_SIZE * layer->xsize;
    const int32 rowMask    = (1 << layer->heightShift) - 1;
    PSPGeLayerBegin(layer, 0, top, pitch, bottom);

    for (int32 cy = top; cy < bottom;) {
        ScanlineInfo *line = &scanlines[cy];
        int32 x            = FROM_FIXED(line->position.x);
        if (x >= layerWidth)
            x -= layerWidth;
        else if (x < 0)
            x += layerWidth;
        int32 y    = FROM_FIXED(line->position.y);
        uint8 bank = gfxLineBuffer[cy];

        int32 end = cy + 1;
        for (; end < bottom; ++end) {
            ScanlineInfo *next = &scanlines[end];
            int32 nx           = FROM_FIXED(next->position.x);
            if (nx >= layerWidth)
                nx -= layerWidth;
            else if (nx < 0)
                nx += layerWidth;
            if (nx != x || FROM_FIXED(next->position.y) != y + (end - cy) || gfxLineBuffer[end] != bank)
                break;
        }

        // the band covers layer rows y..y+height-1, split where it crosses into the next row of tiles
        for (int32 rowStart = y; rowStart < y + (end - cy);) {
            int32 tileY = rowStart & 15;
            int32 rows  = MIN(16 - tileY, y + (end - cy) - rowStart);
            PSPGeEmitTileRow(layer, &layer->layout[((rowStart >> 4) & rowMask) << layer->widthShift], x >> 4, -(x & 15), 0, pitch, tileY, rows,
                             cy + (rowStart - y), bank);
            rowStart += rows;
        }
        cy = end;
    }
    return true;
}

// Same pixels as DrawLayerVScroll: per screen column, the layer column at position.x drawn from position.y (wrapped
// once into the layer height) down the whole screen height, in scanline 0's palette bank. In software that is a pixel
// at a time down each column (~20ms a frame for TMZ's and the Metal Sonic race's backgrounds); here neighbouring
// columns that show neighbouring layer columns at the same height make one band, drawn as rows of tile sprites.
uint32 pspVScrollBands;
bool32 RSDK::PSPQueueLayerVScroll(TileLayer *layer)
{
    if (!pspGeLayers)
        return false;
    if (!layer->xsize || !layer->ysize)
        return true;

    ScreenInfo *screen = currentScreen;
    int32 x1 = screen->clipBound_X1, x2 = MIN(screen->clipBound_X2, PSP_BUF_WIDTH);
    int32 height = screen->size.y;
    if (x1 >= x2 || height <= 0)
        return true;

    const int32 layerHeight = TILE_SIZE * layer->ysize;
    const uint8 bank        = gfxLineBuffer[0];
    PSPGeLayerBegin(layer, x1, 0, x2, height);

    for (int32 cx = x1; cx < x2;) {
        int32 x = FROM_FIXED(scanlines[cx].position.x);
        int32 y = FROM_FIXED(scanlines[cx].position.y);
        if (y >= layerHeight)
            y -= layerHeight;
        else if (y < 0)
            y += layerHeight;

        int32 end = cx + 1;
        for (; end < x2; ++end) {
            int32 ny = FROM_FIXED(scanlines[end].position.y);
            if (ny >= layerHeight)
                ny -= layerHeight;
            else if (ny < 0)
                ny += layerHeight;
            if (FROM_FIXED(scanlines[end].position.x) != x + (end - cx) || ny != y)
                break;
        }

        // the band shows layer columns x.. at screen columns cx..end-1, rows y.. (wrapping at the layer height)
        for (int32 sy = 0; sy < height;) {
            int32 ly    = (y + sy) % layerHeight;
            int32 tileY = ly & 15;
            int32 rows  = MIN(16 - tileY, height - sy);
            PSPGeEmitTileRow(layer, &layer->layout[(ly >> 4) << layer->widthShift], x >> 4, cx - (x & 15), cx, end, tileY, rows, sy, bank);
            sy += rows;
        }
        pspVScrollBands++;
        cx = end;
    }
    return true;
}

// Same pixels as DrawLayerBasic: rows of tiles from the first scanline's y on (wrapping at the layer height), each row
// scrolled by the x of the scanline it starts on, across the clip bounds, always in palette bank 0.
bool32 RSDK::PSPQueueLayerBasic(TileLayer *layer)
{
    if (!pspGeLayers)
        return false;
    if (!layer->xsize || !layer->ysize)
        return true;

    ScreenInfo *screen = currentScreen;
    int32 x1 = screen->clipBound_X1, x2 = screen->clipBound_X2;
    int32 top = screen->clipBound_Y1, bottom = screen->clipBound_Y2;
    if (x1 >= x2 || top >= bottom)
        return true;

    PSPGeLayerBegin(layer, x1, top, x2, bottom);

    int32 ty    = (FROM_FIXED(scanlines[top].position.y) >> 4) % layer->ysize;
    int32 tileY = FROM_FIXED(scanlines[top].position.y) & 15;
    if (ty < 0)
        ty += layer->ysize;

    for (int32 cy = top; cy < bottom;) {
        int32 rows = MIN(16 - tileY, bottom - cy);
        int32 x    = x1 + FROM_FIXED(scanlines[cy].position.x);
        int32 tx   = (x >> 4) % layer->xsize;
        if (tx < 0)
            tx += layer->xsize;
        PSPGeEmitTileRow(layer, &layer->layout[ty << layer->widthShift], tx, x1 - (x & 15), x1, x2, tileY, rows, cy, 0);

        cy += rows;
        tileY = 0;
        if (++ty == layer->ysize)
            ty = 0;
    }
    return true;
}

// Same samples as DrawLayerRotozoom: each scanline walks the layer from position by deform per pixel (16.16, with
// 32-bit wraparound) and takes the texel under each step. Instead of looking every pixel up, the walk is cut where it
// crosses into another tile, and each piece becomes one textured line on the GE with the texel coordinates at its
// ends; the GE interpolates the same straight walk in between.
// PPSSPP answers devctl on "kemulator:" (a real PSP has no such device). Its renderers draw the last pixel of a line,
// which the PSP's GE leaves out: rotozoom segments then spill a texel of the neighbouring tile (dots along cloud edges).
static int pspEmulator = -1;
static bool PSPIsEmulator()
{
    if (pspEmulator < 0) {
        u32 answer = 0;
        pspEmulator = sceIoDevctl("kemulator:", 3 /* IS_EMULATOR */, NULL, 0, &answer, sizeof(answer)) >= 0 && answer;
        PrintLog(PRINT_NORMAL, "PSP: running on %s", pspEmulator ? "an emulator" : "hardware");
    }
    return pspEmulator != 0;
}

// Rotozoom layers that fit in a texture (the special stages' 3D floor and roof: 16x16 tiles, repeating) are drawn
// from a copy of the whole layer in VRAM that the GE wraps around: a few lines per scanline, however many tiles they
// cross. The copy lives where a second screen would (only one is in use whenever this path runs).
#define PSP_VRAM_LAYERTEX (PSP_VRAM_SCREEN(1))
int pspRotoSegment = 480; // pixels per line (boot.txt rotoseg=N): whole rows measured as exact as 32-pixel pieces, and cheaper for the GE
int pspRotoCheck   = 0;  // boot.txt "rotocheck": see PSPRotoCheck
int pspRotoEmit     = 1; // boot.txt "rotonoemit": walk without queuing anything (measures the walk alone)
int pspRotoUncached = 1; // boot.txt "rotocached" turns it off
int pspEmuGeLayers = 0;  // boot.txt "emuge": rotozoom layers on the GE in an emulator too
int pspRotoGeRun   = -1; // boot.txt rotorun=N; -1: 0 (all on the GE) when batches overlap the CPU, else PSP_ROTO_GE_RUN
int pspRotoStrips  = 0;  // boot.txt "rotostrip": rows as 1-pixel-tall triangle strips instead of lines
static TileLayer *pspRotoTexLayer;
static uint32 pspRotoTexSerial;
static uint16 pspRotoTexLayout[32 * 32];

static const uint8 *PSPRotoLayerTexture(TileLayer *layer, int32 texW, int32 texH)
{
    uint8 *tex           = (uint8 *)sceGeEdramGetAddr() + PSP_VRAM_LAYERTEX;
    const int32 tilesX   = texW / TILE_SIZE, tilesY = texH / TILE_SIZE;
    const uint32 entries = tilesX * tilesY;
    if (pspRotoTexLayer == layer && pspRotoTexSerial == pspTileSerial && !memcmp(pspRotoTexLayout, layer->layout, entries * sizeof(uint16)))
        return tex;

    // the GE may still be drawing from the old copy
    if (psp3DPending)
        PSPFlush3D();
    sceGeDrawSync(0);
    // the same tileset entries the software walk reads (flipped variants included, empty entries as tile 0xFFF),
    // swizzled: 16x8 texel blocks, so steps across rows stay in the GE's texture cache too (a tile is two blocks)
    for (int32 ty = 0; ty < tilesY; ++ty) {
        for (int32 tx = 0; tx < tilesX; ++tx) {
            const uint32 *src = (const uint32 *)&tilesetPixels[(layer->layout[tx + (ty << layer->widthShift)] & 0xFFF) << 8];
            for (int32 r = 0; r < TILE_SIZE; ++r, src += 4) {
                uint32 *dst = (uint32 *)&tex[((ty * 2 + (r >> 3)) * tilesX + tx) * 128 + (r & 7) * 16];
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
        }
    }
    sceKernelDcacheWritebackRange(tex, texW * texH);
    memcpy(pspRotoTexLayout, layer->layout, entries * sizeof(uint16));
    pspRotoTexLayer  = layer;
    pspRotoTexSerial = pspTileSerial;
    return tex;
}

static bool32 PSPQueueLayerRotozoomTexture(TileLayer *layer, int32 x1, int32 x2, int32 top, int32 bottom)
{
    const int32 texW = TILE_SIZE << layer->widthShift, texH = TILE_SIZE << layer->heightShift;
    if (texW > 512 || texH > 512 || texW * texH > 0x10000 || videoSettings.screenCount != 1 || currentScreen != &screens[0])
        return false;
    const uint8 *tex = PSPRotoLayerTexture(layer, texW, texH);

    PSPGeLayerBegin(layer, x1, top, x2, bottom);
    unsigned long long start = PSPProfNow(), flushBefore = psp3DFlushUS;

    const int32 lineSize = x2 - x1, seg = pspRotoSegment;
    const uint32 pieces  = (lineSize + seg - 1) / seg;
    const uint32 maskU = ((uint32)texW << 16) - 1, maskV = ((uint32)texH << 16) - 1;
    const float texel  = 1.0f / 65536.0f;
    for (int32 cy = top; cy < bottom; ++cy) {
        ScanlineInfo *line = &scanlines[cy];
        uint32 px = (uint32)line->position.x, py = (uint32)line->position.y;
        int32 dx = line->deform.x, dy = line->deform.y;
        float du = (float)dx * texel, dv = (float)dy * texel;
        // whole textures ahead of the start keep every coordinate positive, whichever way the walk heads
        uint32 ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        const bool strip = pspRotoStrips && ax <= 0x100000 && ay <= 0x100000; // (16.16 sums along the whole row stay in range)
        const uint32 span = strip ? lineSize : seg;
        float offU = (float)(texW * (1 + (uint32)(((uint64)(span + 1) * ax) >> 16) / texW));
        float offV = (float)(texH * (1 + (uint32)(((uint64)(span + 1) * ay) >> 16) / texH));

        if (strip) {
            // the row as one strip, 1 pixel tall: a top and a bottom vertex at each segment boundary, joined to the
            // previous row of the item by two degenerate vertices. Coordinates continue across the row (no rewrap).
            PSPGe3DItem *item    = PSPGeLayerItem(gfxLineBuffer[cy], GU_TRIANGLE_STRIP, (2 * pieces + 4) * sizeof(PSPGeLineVertex), tex, texW, texH);
            PSPGeLineVertex *v   = (PSPGeLineVertex *)&pspGe3DArena[pspGe3DArenaUsed];
            PSPGeLineVertex *beg = v;
            int32 su = (int32)(px & maskU) + (int32)offU * 0x10000 - dx / 2, sv = (int32)(py & maskV) + (int32)offV * 0x10000 - dy / 2;
            for (int32 k = 0;; k += seg) {
                if (k > lineSize)
                    k = lineSize;
                float u = (float)(su + k * dx) * texel, vv = (float)(sv + k * dy) * texel;
                if (k == 0 && item->count) {
                    v[0] = v[-1]; // degenerate join
                    v[1].u = u, v[1].v = vv, v[1].x = x1, v[1].y = cy, v[1].z = 0;
                    v += 2;
                }
                v[0].u = u, v[0].v = vv, v[0].x = x1 + k, v[0].y = cy, v[0].z = 0;
                v[1].u = u, v[1].v = vv, v[1].x = x1 + k, v[1].y = cy + 1, v[1].z = 0;
                v += 2;
                if (k == lineSize)
                    break;
            }
            uint32 used = (uint32)(v - beg);
            item->count += used;
            pspGe3DArenaUsed += used * sizeof(PSPGeLineVertex);
            pspGeLayerSprites += pieces;
            continue;
        }

        PSPGe3DItem *item    = PSPGeLayerItem(gfxLineBuffer[cy], GU_LINES, pieces * 2 * sizeof(PSPGeLineVertex), tex, texW, texH);
        PSPGeLineVertex *v   = (PSPGeLineVertex *)&pspGe3DArena[pspGe3DArenaUsed];
        PSPGeLineVertex *beg = v;
        for (int32 k = 0; k < lineSize; k += seg) {
            int32 n  = MIN(seg, lineSize - k);
            float u0 = (float)(px & maskU) * texel + offU, v0 = (float)(py & maskV) * texel + offV;
            // the GE samples at pixel centres: the ends sit half a step outside the engine's first and last samples
            v[0].u = u0 - 0.5f * du, v[0].v = v0 - 0.5f * dv, v[0].x = x1 + k, v[0].y = cy, v[0].z = 0;
            v[1].u = u0 + ((float)n - 0.5f) * du, v[1].v = v0 + ((float)n - 0.5f) * dv, v[1].x = x1 + k + n, v[1].y = cy, v[1].z = 0;
            v += 2;
            px += (uint32)n * (uint32)dx;
            py += (uint32)n * (uint32)dy;
        }
        uint32 used = (uint32)(v - beg);
        item->count += used;
        pspGe3DArenaUsed += used * sizeof(PSPGeLineVertex);
        pspGeLayerSprites += used / 2;
    }
    pspRotoGeRows += bottom - top;
    unsigned long long flushed = psp3DFlushUS - flushBefore;
    pspGeRotoGenUS += (PSPProfNow() - start) - flushed;
    pspGeRotoFlushUS += flushed;
    if (pspLayerKickWait || !PSPGeBusy()) // (else the batch keeps growing: kicking now would wait for the GE)
        PSPKick3DMid();
    return true;
}

// boot.txt "rotocheck": every 64th rotozoom layer, a band of 32 rows is drawn by the software walk and by the GE path
// from the same starting picture, and the two results are compared pixel by pixel (logged).
static void PSPRotoCheck(TileLayer *layer)
{
    static bool checking;
    static uint32 layerCalls[LAYER_COUNT];
    int32 id = (int32)(layer - tileLayers);
    if (id < 0 || id >= LAYER_COUNT)
        return;
    uint32 calls = ++layerCalls[id];
    static uint16 *buffers;
    const int32 band = 32;
    if (checking || (calls & 31) != 1) // every 32nd call of each layer
        return;
    ScreenInfo *screen = currentScreen;
    const int32 pitch = screen->pitch;
    int32 x1 = screen->clipBound_X1, x2 = screen->clipBound_X2, top = screen->clipBound_Y1, bottom = screen->clipBound_Y2;
    if (!buffers)
        buffers = (uint16 *)malloc(4 * band * pitch * sizeof(uint16));
    if (!buffers || bottom - top <= 0)
        return;
    int32 y1 = top + (int32)(((calls >> 5) * band) % (uint32)(bottom - top));
    int32 y2 = MIN(y1 + band, bottom);
    uint16 *fb    = (uint16 *)((uintptr_t)screen->frameBuffer | 0x40000000); // uncached: sees what the GE wrote
    uint16 *saved = buffers, *cpu = buffers + band * pitch, *ge = cpu + band * pitch, *ref = ge + band * pitch;
    const uint32 bytes = (y2 - y1) * pitch * sizeof(uint16);

    checking = true;
    PSPFlush3D();
    sceKernelDcacheWritebackInvalidateAll();
    memcpy(saved, &fb[y1 * pitch], bytes);
    screen->clipBound_Y1 = y1;
    screen->clipBound_Y2 = y2;
    extern int pspRotoCpuMode;
    pspRotoCpuMode = 1;
    DrawLayerRotozoom(layer);
    pspRotoCpuMode = 0;
    sceKernelDcacheWritebackInvalidateAll();
    memcpy(ref, &fb[y1 * pitch], bytes);
    memcpy(&fb[y1 * pitch], saved, bytes);
    unsigned long long cpuStart = PSPProfNow();
    DrawLayerRotozoom(layer);
    uint32 cpuUS = (uint32)(PSPProfNow() - cpuStart);
    sceKernelDcacheWritebackInvalidateAll();
    memcpy(cpu, &fb[y1 * pitch], bytes);
    // the same walk again into cached RAM, then into the framebuffer again: what the VRAM stores cost
    uint16 *frame       = screen->frameBuffer;
    screen->frameBuffer = ge - y1 * pitch;
    unsigned long long ramStart = PSPProfNow();
    DrawLayerRotozoom(layer);
    uint32 ramUS        = (uint32)(PSPProfNow() - ramStart);
    screen->frameBuffer = frame;
    sceKernelDcacheWritebackInvalidateRange(ge, bytes);
    extern uint32 pspRotoRuns, pspRotoDrawn;
    pspRotoRuns = pspRotoDrawn = 0;
    unsigned long long againStart = PSPProfNow();
    DrawLayerRotozoom(layer);
    uint32 againUS = (uint32)(PSPProfNow() - againStart);
    uint32 runs = pspRotoRuns, drawn = pspRotoDrawn;
    pspRotoCpuMode = 2;
    unsigned long long walkStart = PSPProfNow();
    DrawLayerRotozoom(layer);
    uint32 walkUS  = (uint32)(PSPProfNow() - walkStart);
    pspRotoCpuMode = 0;
    sceKernelDcacheWritebackInvalidateAll();
    memcpy(&fb[y1 * pitch], saved, bytes);
    unsigned long long geStart = PSPProfNow(), genBefore = pspGeRotoGenUS;
    uint32 segsBefore = pspGeLayerSprites, runsBefore = pspRotoWalkRuns;
    unsigned long long itemBefore = pspGenItemUS, initBefore = pspGenInitUS;
    if (!RSDK::PSPQueueLayerRotozoom(layer))
        DrawLayerRotozoom(layer);
    PSPFlush3D();
    uint32 geUS = (uint32)(PSPProfNow() - geStart), genUS = (uint32)(pspGeRotoGenUS - genBefore);
    uint32 geSegs = pspGeLayerSprites - segsBefore, geRuns = pspRotoWalkRuns - runsBefore;
    sceKernelDcacheWritebackInvalidateAll();
    memcpy(ge, &fb[y1 * pitch], bytes);
    screen->clipBound_Y1 = top;
    screen->clipBound_Y2 = bottom;
    checking = false;

    uint32 diffs = 0, total = 0, runDiffs = 0;
    int32 fx = -1, fy = -1;
    for (int32 y = 0; y < y2 - y1; ++y) {
        for (int32 x = x1; x < x2; ++x) {
            ++total;
            runDiffs += cpu[y * pitch + x] != ref[y * pitch + x];
            if (ref[y * pitch + x] != ge[y * pitch + x]) {
                if (!diffs++)
                    fx = x, fy = y;
            }
        }
    }
    if (diffs)
        PrintLog(PRINT_NORMAL, "PSP rotocheck: layer %d rows %d-%d: %u of %u px differ (first at %d,%d: cpu %04X ge %04X) ge %uus | runs: %u differ, %uus (ram %uus, again %uus, walk %uus; %u runs, %u px drawn) gen %uus %u runs %u segs (item %uus init %uus)",
                 (int32)(layer - tileLayers), y1, y2, diffs, total, fx, y1 + fy, ref[fy * pitch + fx], ge[fy * pitch + fx], geUS, runDiffs, cpuUS,
                 ramUS, againUS, walkUS, runs, drawn, genUS, geRuns, geSegs, (uint32)(pspGenItemUS - itemBefore), (uint32)(pspGenInitUS - initBefore));
    else
        PrintLog(PRINT_NORMAL, "PSP rotocheck: layer %d rows %d-%d: all %u px equal, ge %uus | runs: %u differ, %uus (ram %uus, again %uus, walk %uus; %u runs, %u px drawn) gen %uus %u runs %u segs (item %uus init %uus)",
                 (int32)(layer - tileLayers), y1, y2, total, geUS, runDiffs, cpuUS, ramUS, againUS, walkUS, runs, drawn, genUS, geRuns, geSegs, (uint32)(pspGenItemUS - itemBefore), (uint32)(pspGenInitUS - initBefore));
}

// A bit per cell of a big rotozoom layer, set where the cell shows nothing (pspTileRowEmpty): the walk skips those
// cells without reading the layout, whose 512KB miss the cache on nearly every cell entered (~200 cycles each). The bits
// are kept in blocks of 32x16 cells per 64-byte line, so those of the cells a frame walks over stay in the cache.
// Rebuilt when the layer or the tiles change; a few rows are refreshed every call, in case objects write the layout.
int pspRotoBlankOn = 1; // boot.txt "noblankmap" turns it off
struct PSPRotoBlankMap {
    TileLayer *layer;
    const uint16 *layout;
    int32 wShift, hShift;
    uint32 serial, size, refreshRow, builtAt, used;
    uint32 *bits;
    bool disabled;
};
static PSPRotoBlankMap pspRotoBlank[2];
// After the bits per cell, a bit per block of 8x8 cells that all show nothing: the walk then crosses the whole block at
// once (PSPRotoWalkRowGE). Set by PSPRotoBlankBits for the layer it was asked about (NULL: none).
int pspRotoCoarseOn = 1; // boot.txt "nocoarse" turns it off
static const uint32 *pspRotoCoarse;

static uint32 PSPRotoBlankWords(TileLayer *layer)
{
    const int32 w = 1 << layer->widthShift, h = 1 << layer->heightShift;
    return MAX(1, w >> 5) * ((h + 15) >> 4) * 16;
}

static void PSPRotoCoarseRows(TileLayer *layer, uint32 *bits, int32 by0, int32 by1)
{
    if (layer->widthShift < 3 || layer->heightShift < 3)
        return;
    const int32 w = 1 << layer->widthShift, blocksX = MAX(1, w >> 5), cbShift = layer->widthShift - 3;
    uint32 *coarse = bits + PSPRotoBlankWords(layer);
    for (int32 by = by0; by < by1; ++by) {
        for (int32 bx = 0; bx < (w >> 3); ++bx) {
            const int32 tx = bx << 3;
            uint32 all     = 0xFF;
            for (int32 r = 0; r < 8; ++r) {
                const int32 ty = (by << 3) + r;
                all &= (bits[(((ty >> 4) * blocksX + (tx >> 5)) << 4) + (ty & 15)] >> (tx & 31)) & 0xFF;
            }
            const uint32 idx = ((uint32)by << cbShift) + bx;
            if (all == 0xFF)
                coarse[idx >> 5] |= 1U << (idx & 31);
            else
                coarse[idx >> 5] &= ~(1U << (idx & 31));
        }
    }
}

static void PSPRotoBlankRows(TileLayer *layer, uint32 *bits, int32 y0, int32 y1)
{
    const int32 w = 1 << layer->widthShift, blocksX = MAX(1, w >> 5);
    for (int32 ty = y0; ty < y1; ++ty) {
        const uint16 *row = &layer->layout[ty << layer->widthShift];
        uint32 *words     = &bits[(((ty >> 4) * blocksX) << 4) + (ty & 15)];
        for (int32 bx = 0; bx < blocksX; ++bx, row += 32) {
            uint32 word = 0;
            for (int32 b = 0; b < 32 && bx * 32 + b < w; ++b) word |= (uint32)(pspTileRowEmpty[row[b] & 0xFFF] == 0xFFFF) << b;
            words[bx << 4] = word;
        }
    }
}

static const uint32 *PSPRotoBlankBits(TileLayer *layer)
{
    if (!pspRotoBlankOn)
        return NULL;
    const int32 w = 1 << layer->widthShift, h = 1 << layer->heightShift;
    const uint32 size = PSPRotoBlankWords(layer) * 4 + (((w >> 3) * (h >> 3) + 31) >> 5) * 4 + 4;
    static uint32 calls;
    pspRotoCoarse = NULL;
    ++calls;
    PSPRotoBlankMap *map = NULL;
    for (int32 m = 0; m < 2; ++m) {
        if (pspRotoBlank[m].layer == layer && pspRotoBlank[m].layout == layer->layout && pspRotoBlank[m].wShift == layer->widthShift
            && pspRotoBlank[m].hShift == layer->heightShift)
            map = &pspRotoBlank[m];
    }
    if (map && map->disabled)
        return NULL;
    if (!map || map->serial != pspTileSerial) {
        if (map && calls - map->builtAt < 120) {
            map->disabled = true; // its tiles keep changing: a rebuild every time would cost more than it saves
            PrintLog(PRINT_NORMAL, "PSP rotozoom blank map off for layer %d (tiles keep changing)", (int32)(layer - tileLayers));
            return NULL;
        }
        if (!map)
            map = pspRotoBlank[0].used <= pspRotoBlank[1].used ? &pspRotoBlank[0] : &pspRotoBlank[1];
        if (map->size < size) {
            free(map->bits);
            map->bits = (uint32 *)malloc(size);
            map->size = map->bits ? size : 0;
        }
        if (!map->bits)
            return NULL;
        PSPRotoBlankRows(layer, map->bits, 0, h);
        PSPRotoCoarseRows(layer, map->bits, 0, h >> 3);
        map->layer      = layer;
        map->layout     = layer->layout;
        map->wShift     = layer->widthShift;
        map->hShift     = layer->heightShift;
        map->serial     = pspTileSerial;
        map->builtAt    = calls;
        map->refreshRow = 0;
        map->disabled   = false;
    }
    else {
        int32 y0 = map->refreshRow, y1 = MIN(h, y0 + 1);
        PSPRotoBlankRows(layer, map->bits, y0, y1);
        PSPRotoCoarseRows(layer, map->bits, y0 >> 3, (y0 >> 3) + 1);
        map->refreshRow = y1 >= h ? 0 : y1;
    }
    map->used = calls;
    if (pspRotoCoarseOn && layer->widthShift >= 3 && layer->heightShift >= 3)
        pspRotoCoarse = map->bits + PSPRotoBlankWords(layer);
    return map->bits;
}

int pspHorizonLod   = 0; // boot.txt horizonlod=N: rows averaging fewer pixels per tile than this are drawn in pairs, from the first's samples (0: never)
int pspHorizonLodX  = 0; // boot.txt "horizonlodx": those rows also sampled every other pixel (the GE stretches them back)
int pspHorizonLodMinRows = 0; // boot.txt horizonrows=K: only when the layer has at least K such rows (the horizon high up)
unsigned int pspHorizonRowsSum[3], pspHorizonRowsMax[3], pspHorizonFrames; // (diagnostics: rows under 2, 3, 4 px per tile)
int pspRotoIndexRun = 8; // boot.txt rotoidx=N: rows averaging fewer pixels per tile than this go through the index buffer (0: never)
unsigned int pspRotoIndexRows; // (diagnostics)
unsigned long long pspRotoIndexUS, pspRotoBlankUS, pspRotoClassUS;

// Whether a row goes through the index buffer also depends on how much of it shows: the GE walk skips the cells that
// show nothing almost for free, the index row samples every pixel. Per layer and scanline, the percentage of the cells
// crossed that showed something, the last time that row was drawn.
int pspRotoIndexDensity = 75; // boot.txt rotodense=N (75: best measured over Pinball and UFO1-7 once the GE walk got cheaper)
static uint8 pspRotoRowDensity[LAYER_COUNT][SCREEN_YSIZE];

// One row of the layer as palette indices (0 where nothing shows), the same samples as DrawLayerRotozoom.
// Returns the percentage of the cells crossed that show something.
static uint32 PSPRotoIndexRow(TileLayer *layer, const ScanlineInfo *line, uint8 *dst, uint32 lineSize, const uint32 *blank, uint32 blocksX)
{
    uint32 runs = 0, shown = 0;
    const uint32 maskX = (1U << layer->widthShift) - 1, maskY = (1U << layer->heightShift) - 1;
    const int32 shift = layer->widthShift;
    const uint32 px = (uint32)line->position.x, py = (uint32)line->position.y;
    const uint32 dx = (uint32)line->deform.x, dy = (uint32)line->deform.y;
    PSPRotoAxis axisX, axisY;
    axisX.Init(px, (int32)dx);
    axisY.Init(py, (int32)dy);
    uint32 tx = (px >> 20) & maskX, ty = (py >> 20) & maskY;
    for (uint32 k = 0; k < lineSize;) {
        uint32 end = MIN(MIN(axisX.next, axisY.next), lineSize);
        uint32 tile = 0xFFF;
        if (!blank || !((blank[(((ty >> 4) * blocksX + (tx >> 5)) << 4) + (ty & 15)] >> (tx & 31)) & 1))
            tile = layer->layout[tx + (ty << shift)] & 0xFFF;
        ++runs;
        if (pspTileRowEmpty[tile] == 0xFFFF) {
            for (uint32 i = k; i < end; ++i) dst[i] = 0;
        }
        else {
            ++shown;
            const uint8 *pixels = &tilesetPixels[tile << 8];
            uint32 x = px + k * dx, y = py + k * dy;
            for (uint32 i = k; i < end; ++i) {
                dst[i] = pixels[((y >> 12) & 0xF0) + ((x >> 16) & 15)];
                x += dx;
                y += dy;
            }
        }
        k = end;
        while (axisX.next == k) {
            tx = (tx + axisX.step) & maskX;
            axisX.Advance();
        }
        while (axisY.next == k) {
            ty = (ty + axisY.step) & maskY;
            axisY.Advance();
        }
    }
    return runs ? shown * 100 / runs : 0;
}

// One scanline of a rotozoom layer as GE line segments, one per run of pixels inside a tile that shows something (the
// layout read only for cells the blank map doesn't rule out). Kept apart from its caller, with few values live, so the
// walk stays in registers. Returns the vertices written.
// One line vertex (16 bytes, 16-byte aligned) built in the VFPU and stored at once: u and v given in 16.16 doubled
// (scaled by 2^-17 in the conversion itself, exactly like the float multiply), x/y packed, z and pad 0.
int pspRotoVfpu = 1; // boot.txt "novfpu" turns it off
static inline void PSPVfpuLineVertex(void *dst, int32 u, int32 v, uint32 xy)
{
    __asm__ volatile("mtv %1, S000\n"
                     "mtv %2, S001\n"
                     "mtv %3, S002\n"
                     "mtv $0, S003\n"
                     "vi2f.p C000, C000, 1/2^17\n"
                     "sv.q C000, 0(%0)\n"
                     :
                     : "r"(dst), "r"(u), "r"(v), "r"(xy)
                     : "memory");
}

struct PSPRotoRowWalk {
    const uint16 *layout;
    const uint32 *blank;
    uint32 maskX, maskY, shift, bxShift;
    const uint32 *coarse; // 8x8-cell blocks that show nothing (or NULL)
    uint32 cbShift;
    const uint8 *cells; // PSPRotoCellMap (or NULL)
    const uint16 *entries;
    uint32 stride, xsize;
};
// From a cell that shows nothing: the pixels until the walk leaves its 8x8-cell block when nothing in the whole block
// shows either, else 0. (Apart from the walk, so its values don't take the walk's registers.)
static __attribute__((noinline)) uint32 PSPRotoBlockSkip(const PSPRotoRowWalk &w, uint32 px, uint32 py, int32 dx, int32 dy, uint32 k, uint32 tx,
                                                         uint32 ty)
{
    if (!w.coarse)
        return 0;
    const uint32 idx = ((ty >> 3) << w.cbShift) + (tx >> 3);
    if (!((w.coarse[idx >> 5] >> (idx & 31)) & 1))
        return 0;
    const uint32 x = px + k * (uint32)dx, y = py + k * (uint32)dy;
    uint32 n = 0xFFFFFFFF;
    if (dx) {
        const uint32 a    = dx < 0 ? (uint32)-dx : (uint32)dx;
        const uint32 dist = dx > 0 ? 0x800000 - (x & 0x7FFFFF) : (x & 0x7FFFFF) + 1;
        n                 = MIN(n, (dist + a - 1) / a);
    }
    if (dy) {
        const uint32 a    = dy < 0 ? (uint32)-dy : (uint32)dy;
        const uint32 dist = dy > 0 ? 0x800000 - (y & 0x7FFFFF) : (y & 0x7FFFFF) + 1;
        n                 = MIN(n, (dist + a - 1) / a);
    }
    return n;
}

static __attribute__((noinline)) uint32 PSPRotoWalkRowGE(const PSPRotoRowWalk &w, uint32 px, uint32 py, int32 dx, int32 dy, uint32 lineSize,
                                                         int32 x1, int32 cy, PSPGeLineVertex *__restrict__ out, uint32 *outRuns,
                                                         uint32 *outShown)
{
    const uint16 *layout = w.layout;
    const uint32 *blank  = w.blank;
    const uint8 *cells   = w.cells;
    const uint32 stride = w.stride, xsize = w.xsize;

    const uint32 maskX = w.maskX, maskY = w.maskY, shift = w.shift, bxShift = w.bxShift;
    const float halfTexel = 1.0f / 131072.0f;
    const int32 E = 0x800, T = 0x100000; // 1/32 of a texel; the tile size (16.16)
    const uint32 yWord = (uint32)(uint16)cy << 16; // y in the high half of the x/y word (z and pad are 0)
    PSPGeLineVertex *v = out;

    PSPRotoAxis axisX, axisY;
    axisX.Init(px, dx);
    axisY.Init(py, dy);
    uint32 tx = (px >> 20) & maskX, ty = (py >> 20) & maskY;
    uint32 runs = 0, shown = 0;
    for (uint32 k = 0; k < lineSize;) {
        const uint32 end = MIN(MIN(axisX.next, axisY.next), lineSize);
        ++runs;
        if (blank && ((blank[((((ty >> 4) << bxShift) + (tx >> 5)) << 4) + (ty & 15)] >> (tx & 31)) & 1)) {
            // nothing shows here; if nothing does in the whole block of 8x8 cells either, cross the block at once
            // (the map read afresh each time rather than kept in a register the walk needs)
            const uint32 *coarse = *(const uint32 *const volatile *)&w.coarse;
            const uint32 idx     = ((ty >> 3) << w.cbShift) + (tx >> 3);
            const uint32 n       = coarse && ((coarse[idx >> 5] >> (idx & 31)) & 1) ? PSPRotoBlockSkip(w, px, py, dx, dy, k, tx, ty) : 0;
            if (n) {
                if (n >= lineSize - k)
                    break;
                k += n;
                while (axisX.next <= k) {
                    tx = (tx + axisX.step) & maskX;
                    axisX.Advance();
                }
                while (axisY.next <= k) {
                    ty = (ty + axisY.step) & maskY;
                    axisY.Advance();
                }
                continue;
            }
        }
        else {
            uint32 entry, code = 255;
            if (cells && tx < xsize)
                code = cells[(ty >> 2) * stride + ((tx >> 4) << 6) + ((ty & 3) << 4) + (tx & 15)];
            if (code != 255)
                entry = w.entries[code]; // (code 0: nothing shows, skipped below)
            else
                entry = layout[tx + (ty << shift)];
            if (code && pspTileRowEmpty[entry & 0xFFF] != 0xFFFF) {
                ++shown;
                const uint32 run = end - k;
                const uint32 sx = px + k * (uint32)dx, sy = py + k * (uint32)dy;
                // The GE samples at pixel centres: the line's ends sit half a step outside the engine's first and last
                // samples, which are kept a hair inside the tile (the GE interpolates with less precision, and would
                // otherwise sometimes land in the neighbouring tile of the atlas). In integers at twice 16.16, so half
                // a step is exact: one conversion per coordinate.
                const int32 u0 = sx & 0xFFFFF, v0 = sy & 0xFFFFF;
                const int32 uN = u0 + (int32)(run - 1) * dx, vN = v0 + (int32)(run - 1) * dy;
                int32 ua = 2 * MIN(MAX(u0, E), T - E) - dx, ub = 2 * MIN(MAX(uN, E), T - E) + dx;
                int32 va = 2 * MIN(MAX(v0, E), T - E) - dy, vb = 2 * MIN(MAX(vN, E), T - E) + dy;
                if (entry & 0x400) { // FLIP_X variant
                    ua = 2 * T - ua;
                    ub = 2 * T - ub;
                }
                if (entry & 0x800) { // FLIP_Y variant
                    va = 2 * T - va;
                    vb = 2 * T - vb;
                }
                const int32 tile = entry & 0x3FF;
                const int32 tu = (tile & 31) << 21, tv = (tile >> 5) << 21; // its place in the atlas (16 texels, doubled 16.16)
                if (pspRotoVfpu) {
                    PSPVfpuLineVertex(v, tu + ua, tv + va, yWord | (uint16)(x1 + k));
                    PSPVfpuLineVertex(v + 1, tu + ub, tv + vb, yWord | (uint16)(x1 + end));
                }
                else {
                    uint32 *word = (uint32 *)v;
                    ((float *)word)[0] = (float)(tu + ua) * halfTexel;
                    ((float *)word)[1] = (float)(tv + va) * halfTexel;
                    word[2]            = yWord | (uint16)(x1 + k);
                    word[3]            = 0;
                    ((float *)word)[4] = (float)(tu + ub) * halfTexel;
                    ((float *)word)[5] = (float)(tv + vb) * halfTexel;
                    word[6]            = yWord | (uint16)(x1 + end);
                    word[7]            = 0;
                }
                v += 2;
            }
        }
        k = end;
        while (axisX.next == k) {
            tx = (tx + axisX.step) & maskX;
            axisX.Advance();
        }
        while (axisY.next == k) {
            ty = (ty + axisY.step) & maskY;
            axisY.Advance();
        }
    }
    *outRuns  = runs;
    *outShown = shown;
    return (uint32)(v - out);
}

// A byte per cell of a big rotozoom layer, in blocks of 16x4 cells per 64-byte cache line: 0 where nothing shows,
// else an index into a small table of the layer's layout entries (255: too many kinds, read the layout). Cells the walk
// meets one after the other, and those of the next scanlines, then mostly share cache lines, where the layout (1KB a
// row) costs a cache miss (~190 cycles) for nearly every cell. Rebuilt like the blank map; freed with the scene.
int pspRotoCellsOn = 0; // boot.txt "cellmap" turns it on (measured: ~6% of the walk in UFO7, nothing in UFO6, for 256-384KB of RAM)
struct PSPRotoCellMap {
    TileLayer *layer;
    const uint16 *layout;
    int32 wShift, hShift;
    uint32 serial, builtAt, used, refreshRow, size, stride, nextCode;
    uint8 *cells;
    uint16 entries[256];
    uint8 codeOf[TILE_COUNT * 4];
    bool disabled;
};
static PSPRotoCellMap *pspRotoCells;

// The rows nearest the camera through pages of a big layer (the special stages' playfield, 512x512 tiles): the layer
// cut into 512x512-pixel pages (32x32 tiles, as the layer lies), a few copied into textures, so a row is one GE line
// per page it crosses (like a small layer's whole texture, PSPQueueLayerRotozoomTexture) instead of one segment per
// tile. Each page slot remembers the tileset entry it holds: only tiles that differ (a new page, the camera moving on)
// are copied in, all before this layer's lines are queued - when the previous frame's lists are done reading them
// (PSPWaitDisplayList, before any drawing). The textures live in the stage's storage (no VRAM is left; the special
// stages leave ~6MB of it unused), taken only while there is room without moving anything else.
int pspRotoPagesOn  = 1; // boot.txt "nopages" turns it off, "pageson" on
int pspRotoPageMin  = 6; // boot.txt pagemin=N: only rows averaging at least N pixels per tile (farther ones minify: slow for the GE from RAM)
int pspRotoPageMax  = 12; // boot.txt pages=N: textures at most
int pspRotoPagesNoDraw = 0;
int pspRotoPagesDebug = 0;
int pspRotoPagesRowA = 0, pspRotoPagesRowB = 9999; // (debugging, boot.txt pagesrows=A,B: page lines only for rows [A, B))
unsigned int pspRotoPageRows, pspRotoPageLines, pspRotoPageCopies; // (diagnostics)
#define PSP_ROTO_PAGE_TILES  (32)
#define PSP_ROTO_PAGE_SHIFT  (25) // a page is 32 tiles of 16 pixels, in 16.16
#define PSP_ROTO_PAGES       (12)
#define PSP_ROTO_PAGE_PIECES (12) // most pages one row may cross
#define PSP_ROTO_PAGE_EMPTY  (0x1000) // slot key: a tile that shows nothing (all zero)
#define PSP_ROTO_PAGE_NONE   (0xFF)   // piece of row without a texture: walked like before
#define PSP_ROTO_PAGE_BYTES  (512 * 512)
struct PSPRotoPage {
    uint8 *tex;    // 512x512 8-bit, swizzled
    uint32 id;     // the page it holds: (layer << 24) | (y << 16) | x (0xFFFFFFFF: none)
    uint32 usedAt; // the last layer call it served
    int32 usedFrame; // the frame of that call: its lines may still be drawn from it until the frame is shown
    uint32 minX, minY, maxX, maxY; // that call: the bounds of its samples (16.16 within the page)
    uint16 slots[PSP_ROTO_PAGE_TILES * PSP_ROTO_PAGE_TILES]; // the tileset entry each slot holds (0xFEFE: nothing yet)
};
struct PSPRotoPiece {
    uint16 k0, k1; // the row's samples [k0, k1)
    uint8 page;
};
static PSPRotoPage pspRotoPages[PSP_ROTO_PAGES];
static uint32 *pspRotoPageMem;   // the storage block (released, the engine's way, by setting it to NULL)
static uint32 *pspRotoPageMemAt; // where it was when the textures were laid out (a defragment moves it)
static int32 pspRotoPageCount;   // textures in it
static bool pspRotoPagesFailed;  // no room (until the next scene)
static uint32 pspRotoPageSerial, pspRotoPageCall;

// Pages trade CPU work for GE work, and the GE samples them slowly from RAM: worth it only while the CPU is what holds
// the frame back (UFO6 gains ~3ms; UFO7, held back by the GE, loses ~1ms). Which it is shows in the frame time itself:
// while frames run late, every 4s, for 2s, they alternate with and without pages, 4 at a time (so both see the same
// part of the stage), timed from one call of the layer (once a frame) to the next; the faster way is kept after.
int pspRotoPageAB = 1; // boot.txt "pagesalways": no comparisons, pages whenever possible
static bool PSPRotoPagesWanted()
{
    enum { COMPARE = 120, KEEP = 240 };
    static uint32 phase;
    static unsigned long long last, time[2], kept;
    static uint32 timed[2];
    static bool chosen = true, previous = true;
    static int32 decidedFrame = -1;
    if (!pspRotoPageAB)
        return true;
    if (decidedFrame == pspFrameIndex)
        return previous; // (the frame's other layers go the same way)
    decidedFrame = pspFrameIndex;
    const unsigned long long now = sceKernelGetSystemTimeWide();
    const uint32 frame = (uint32)MIN(now - last, 100000ULL);
    last = now;
    // the frame just timed was drawn the way chosen at the previous call (the first frame of a block still carries the
    // previous block's queue: left out)
    if (phase < COMPARE) {
        if (phase > 0 && ((phase - 1) & 3))
            time[previous] += frame, timed[previous]++;
    }
    else {
        kept += frame;
    }
    bool use = chosen;
    if (phase < COMPARE) {
        use = !((phase >> 2) & 1);
    }
    else if (phase == COMPARE) {
        const uint32 with = timed[1] ? (uint32)(time[1] / timed[1]) : 0, without = timed[0] ? (uint32)(time[0] / timed[0]) : 0;
        chosen = with + 200 < without;
        PrintLog(PRINT_NORMAL, "PSP rotozoom pages: %s (frames %u us with, %u us without)", chosen ? "on" : "off", with, without);
        time[0] = time[1] = 0;
        timed[0] = timed[1] = 0;
        kept    = 0;
        use     = chosen;
    }
    if (++phase == COMPARE + KEEP) {
        // frames on time (60 a second): no need to compare again yet
        phase = kept / (KEEP - 1) < 17000 ? COMPARE + 1 : 0;
        kept  = 0;
    }
    previous = use;
    if (pspDumpStart >= 0 && pspDumpCount > 0 && pspFrameIndex >= pspDumpStart) // (boot.txt dumpframes: which way each frame was drawn)
        PrintLog(PRINT_NORMAL, "PSP dump frame %d: pages %s", pspFrameIndex, use ? "on" : "off");
    return use;
}

void PSPRotoFreeMaps()
{
    pspRotoTexLayer = NULL; // copied again from the layer itself: same pointer and tiles needn't mean same VRAM contents
    if (pspRotoCells) {
        free(pspRotoCells->cells);
        free(pspRotoCells);
        pspRotoCells = NULL;
    }
    if (pspRotoPageMem)
        PSPWaitDisplayList(); // (the last frame's list may still sample it)
    pspRotoPageMem     = NULL; // (the stage's storage is reset with the scene; else the engine collects it)
    pspRotoPageCount   = 0;
    pspRotoPagesFailed = false;
}

static bool PSPRotoPagesReady(TileLayer *layer)
{
    if (!pspRotoPageMem) {
        if (pspRotoPagesFailed)
            return false;
        // only room the stage leaves well alone: 2MB more stays free, and nothing is defragmented for it
        DataStorage *storage = &dataStorage[DATASET_STG];
        const uint32 used = storage->usedStorage * sizeof(uint32), reserve = 2 << 20, extra = 64 + 64;
        int32 count = used + reserve + extra < storage->storageLimit ? (storage->storageLimit - used - reserve - extra) / PSP_ROTO_PAGE_BYTES : 0;
        count = MIN(count, MIN(pspRotoPageMax, PSP_ROTO_PAGES));
        if (count >= 2 && storage->entryCount < STORAGE_ENTRY_COUNT)
            AllocateStorage((void **)&pspRotoPageMem, count * PSP_ROTO_PAGE_BYTES + 64, DATASET_STG, false);
        if (!pspRotoPageMem) {
            pspRotoPagesFailed = true;
            PrintLog(PRINT_NORMAL, "PSP rotozoom pages: no room (stage storage %u/%u KB)", used >> 10, storage->storageLimit >> 10);
            return false;
        }
        pspRotoPageCount = count;
        pspRotoPageMemAt = NULL;
        PrintLog(PRINT_NORMAL, "PSP dbg rotozoom pages: %d (%d KB of stage storage, %p-%p; pool %p-%p)", count, count * PSP_ROTO_PAGE_BYTES >> 10,
                 pspRotoPageMem, (uint8 *)pspRotoPageMem + count * PSP_ROTO_PAGE_BYTES + 64, storage->memoryTable,
                 (uint8 *)storage->memoryTable + storage->storageLimit);
    }
    if (pspRotoPageMemAt != pspRotoPageMem) { // new, or moved: lay the textures out again
        uint8 *base = (uint8 *)(((uintptr_t)pspRotoPageMem + 63) & ~(uintptr_t)63);
        for (int32 i = 0; i < pspRotoPageCount; ++i) {
            pspRotoPages[i].tex    = base + i * PSP_ROTO_PAGE_BYTES;
            pspRotoPages[i].id        = 0xFFFFFFFF;
            pspRotoPages[i].usedAt    = 0;
            pspRotoPages[i].usedFrame = -1;
        }
        pspRotoPageMemAt = pspRotoPageMem;
    }
    if (pspRotoPageSerial != pspTileSerial) {
        for (int32 i = 0; i < pspRotoPageCount; ++i) pspRotoPages[i].id = 0xFFFFFFFF;
        pspRotoPageSerial = pspTileSerial;
    }
    return true;
}

// The texture holding page id for this call, taking the one unused longest; -1 when all serve this frame already (a
// texture another layer used this frame is still to be drawn from: UFO7 draws its sky and its track from pages).
static int32 PSPRotoPageFor(uint32 id)
{
    int32 victim = -1;
    for (int32 i = 0; i < pspRotoPageCount; ++i) {
        PSPRotoPage *page = &pspRotoPages[i];
        if (page->id == id) {
            victim = i;
            break;
        }
        if (page->usedFrame != pspFrameIndex && (victim < 0 || page->usedAt < pspRotoPages[victim].usedAt))
            victim = i;
    }
    if (victim < 0)
        return -1;
    PSPRotoPage *page = &pspRotoPages[victim];
    if (page->id != id) {
        page->id = id;
        memset(page->slots, 0xFE, sizeof(page->slots));
    }
    page->usedFrame = pspFrameIndex;
    if (page->usedAt != pspRotoPageCall) {
        page->usedAt = pspRotoPageCall;
        page->minX = page->minY = 0xFFFFFFFF;
        page->maxX = page->maxY = 0;
    }
    return victim;
}

// Cuts a row into the pages its samples cross (the engine's samples: position + k * deform, k < lineSize), pieces
// left without a texture (all taken by nearer rows) marked PSP_ROTO_PAGE_NONE. false when it crosses too many pages,
// or none of its pieces got a texture; full: all of them did.
static bool PSPRotoRowPages(TileLayer *layer, const ScanlineInfo *line, uint32 lineSize, PSPRotoPiece *pieces, uint8 *count, bool *full)
{
    const uint32 mask   = (1U << PSP_ROTO_PAGE_SHIFT) - 1;
    const uint32 pagesX = ((1U << layer->widthShift) >> 5) - 1, pagesY = ((1U << layer->heightShift) >> 5) - 1;
    const uint32 layerKey = (uint32)(layer - tileLayers) << 24;
    uint32 x = (uint32)line->position.x, y = (uint32)line->position.y;
    const int32 dx = line->deform.x, dy = line->deform.y;
    const uint32 ax = dx < 0 ? (uint32)-dx : (uint32)dx, ay = dy < 0 ? (uint32)-dy : (uint32)dy;
    uint32 n = 0, textured = 0;
    for (uint32 k = 0; k < lineSize;) {
        if (n == PSP_ROTO_PAGE_PIECES)
            return false;
        // samples until the next page edge on each axis (as PSPRotoAxis::Init, on pages instead of tiles)
        uint32 run = lineSize - k;
        if (dx) {
            const uint32 dist = dx > 0 ? (mask + 1) - (x & mask) : (x & mask) + 1;
            run = MIN(run, (dist + ax - 1) / ax);
        }
        if (dy) {
            const uint32 dist = dy > 0 ? (mask + 1) - (y & mask) : (y & mask) + 1;
            run = MIN(run, (dist + ay - 1) / ay);
        }
        const int32 slot = PSPRotoPageFor(layerKey | (((y >> PSP_ROTO_PAGE_SHIFT) & pagesY) << 16) | ((x >> PSP_ROTO_PAGE_SHIFT) & pagesX));
        if (slot >= 0) {
            PSPRotoPage *page = &pspRotoPages[slot];
            const uint32 sx = x & mask, sy = y & mask;
            const uint32 ex = (x + (run - 1) * (uint32)dx) & mask, ey = (y + (run - 1) * (uint32)dy) & mask;
            page->minX = MIN(page->minX, MIN(sx, ex)), page->maxX = MAX(page->maxX, MAX(sx, ex));
            page->minY = MIN(page->minY, MIN(sy, ey)), page->maxY = MAX(page->maxY, MAX(sy, ey));
            ++textured;
        }
        pieces[n].k0   = (uint16)k;
        pieces[n].k1   = (uint16)(k + run);
        pieces[n].page = slot >= 0 ? (uint8)slot : PSP_ROTO_PAGE_NONE;
        ++n;
        k += run;
        x += run * (uint32)dx;
        y += run * (uint32)dy;
    }
    *count = (uint8)n;
    *full  = textured == n;
    return textured != 0;
}

// Copies into a page the tiles under this call's samples (with a margin for the GE's interpolation) that it lacks.
static void PSPRotoPageTiles(TileLayer *layer, PSPRotoPage *page)
{
    const uint32 margin = 0x2000, mask = (1U << PSP_ROTO_PAGE_SHIFT) - 1;
    const uint32 tx0 = (page->minX > margin ? page->minX - margin : 0) >> 20, tx9 = MIN(page->maxX + margin, mask) >> 20;
    const uint32 ty0 = (page->minY > margin ? page->minY - margin : 0) >> 20, ty9 = MIN(page->maxY + margin, mask) >> 20;
    const uint32 baseX = (page->id & 0xFFFF) * PSP_ROTO_PAGE_TILES, baseY = ((page->id >> 16) & 0xFF) * PSP_ROTO_PAGE_TILES;
    for (uint32 ty = ty0; ty <= ty9; ++ty) {
        const uint16 *row = &layer->layout[(baseY + ty) << layer->widthShift];
        uint16 *slots     = &page->slots[ty * PSP_ROTO_PAGE_TILES];
        uint8 *blockRow   = &page->tex[ty * 2 * PSP_ROTO_PAGE_TILES * 128];
        for (uint32 tx = tx0; tx <= tx9; ++tx) {
            // the entry the software walk samples (empty entries as tile 0xFFF, like the whole-layer texture)
            uint32 key = row[baseX + tx] & 0xFFF;
            if (pspTileRowEmpty[key] == 0xFFFF)
                key = PSP_ROTO_PAGE_EMPTY;
            if (slots[tx] == key)
                continue;
            slots[tx] = key;
            pspRotoPageCopies++;
            // swizzled: the tile's two 16x8 blocks, rows 0-7 and 8-15, a row of blocks apart
            uint32 *dst0 = (uint32 *)&blockRow[tx * 128], *dst1 = (uint32 *)&blockRow[(PSP_ROTO_PAGE_TILES + tx) * 128];
            if (key == PSP_ROTO_PAGE_EMPTY) {
                memset(dst0, 0, 128);
                memset(dst1, 0, 128);
            }
            else {
                const uint32 *src = (const uint32 *)&tilesetPixels[key << 8];
                memcpy(dst0, src, 128);
                memcpy(dst1, src + 32, 128);
            }
        }
    }
}

static void PSPRotoCellRows(PSPRotoCellMap *map, TileLayer *layer, int32 y0, int32 y1)
{
    const int32 xsize = MIN(layer->xsize, 1 << layer->widthShift);
    for (int32 ty = y0; ty < y1; ++ty) {
        const uint16 *row = &layer->layout[ty << layer->widthShift];
        uint8 *line       = &map->cells[(ty >> 2) * map->stride + ((ty & 3) << 4)];
        for (int32 tx = 0; tx < xsize; ++tx) {
            uint32 key = row[tx] & 0xFFF;
            uint8 code = 0;
            if (pspTileRowEmpty[key] != 0xFFFF) {
                code = map->codeOf[key];
                if (!code) {
                    if (map->nextCode < 255) {
                        code                   = (uint8)map->nextCode++;
                        map->codeOf[key]       = code;
                        map->entries[code]     = (uint16)key;
                    }
                    else {
                        code = 255;
                    }
                }
            }
            line[((tx >> 4) << 6) + (tx & 15)] = code;
        }
    }
}

static const PSPRotoCellMap *PSPRotoCellBits(TileLayer *layer)
{
    if (!pspRotoCellsOn || layer->widthShift < 4)
        return NULL;
    const int32 h = 1 << layer->heightShift, xsize = MIN(layer->xsize, 1 << layer->widthShift);
    const uint32 stride = (uint32)((xsize + 15) >> 4) << 6, size = stride * ((h + 3) >> 2);
    if (size > 400 * 1024)
        return NULL;
    static uint32 calls;
    ++calls;
    PSPRotoCellMap *map = pspRotoCells;
    if (!map) {
        map = pspRotoCells = (PSPRotoCellMap *)calloc(1, sizeof(PSPRotoCellMap));
        if (!map)
            return NULL;
    }
    bool same = map->layer == layer && map->layout == layer->layout && map->wShift == layer->widthShift && map->hShift == layer->heightShift;
    if (same && map->disabled)
        return NULL;
    if (!same || map->serial != pspTileSerial) {
        if (same && calls - map->builtAt < 120) {
            map->disabled = true; // its tiles keep changing
            PrintLog(PRINT_NORMAL, "PSP rotozoom cell map off for layer %d (tiles keep changing)", (int32)(layer - tileLayers));
            return NULL;
        }
        if (map->size < size) {
            free(map->cells);
            map->cells = (uint8 *)memalign(64, size);
            map->size  = map->cells ? size : 0;
        }
        if (!map->cells)
            return NULL;
        memset(map->codeOf, 0, sizeof(map->codeOf));
        map->nextCode = 1;
        map->stride   = stride;
        PSPRotoCellRows(map, layer, 0, h);
        map->layer      = layer;
        map->layout     = layer->layout;
        map->wShift     = layer->widthShift;
        map->hShift     = layer->heightShift;
        map->serial     = pspTileSerial;
        map->builtAt    = calls;
        map->refreshRow = 0;
        map->disabled   = false;
    }
    else {
        PSPRotoCellRows(map, layer, map->refreshRow, map->refreshRow + 1);
        map->refreshRow = map->refreshRow + 1 >= (uint32)h ? 0 : map->refreshRow + 1;
    }
    return map;
}

// Rows without rotation (deform.y 0) read a single row of the layer's pixels, scaled: that row, 512 texels (a narrower
// layer repeated across them), as the software sampler reads it (tileset entries with their flips, empty tiles as 0),
// becomes a row of the CPU-drawn rows buffer, and the GE draws the screen row from it as one line - instead of a
// segment per tile crossed (FBZ1's inside background: 5.9ms of segments, 240 rows a frame).
int pspRotoFlatRows = 1; // boot.txt "noflatrows" turns it off
unsigned int pspRotoFlatCount; // (diagnostics)
static void PSPRotoFlatRow(TileLayer *layer, uint32 py, int32 rowID)
{
    const uint32 y      = (py >> 16) & ((TILE_SIZE << layer->heightShift) - 1);
    const uint16 *row   = &layer->layout[(y >> 4) << layer->widthShift];
    const uint32 tiles  = 1U << layer->widthShift, sub = (y & 15) << 4;
    // swizzled 512-texel rows: block (rowID / 8, column / 16) holds 128 bytes, a row of it 16
    uint8 *band = &pspRotoFlatBuffer[(rowID >> 3) * (512 / 16) * 128 + (rowID & 7) * 16];
    for (uint32 c = 0; c < 512 / TILE_SIZE; ++c) {
        uint32 *out      = (uint32 *)&band[c * 128];
        const uint32 key = row[c & (tiles - 1)] & 0xFFF; // (a narrower layer repeats across the 512 texels)
        if (pspTileRowEmpty[key] == 0xFFFF) {
            out[0] = out[1] = out[2] = out[3] = 0;
            continue;
        }
        const uint32 *src = (const uint32 *)&tilesetPixels[(key << 8) + sub];
        out[0] = src[0], out[1] = src[1], out[2] = src[2], out[3] = src[3];
    }
}

bool32 RSDK::PSPQueueLayerRotozoom(TileLayer *layer)
{
    if (!pspGeLayers || (PSPIsEmulator() && !pspEmuGeLayers))
        return false; // (in an emulator the CPU's exact pixels cost nothing that matters)
    if (!layer->xsize || !layer->ysize)
        return true;

    ScreenInfo *screen = currentScreen;
    int32 x1 = screen->clipBound_X1, x2 = screen->clipBound_X2;
    int32 top = screen->clipBound_Y1, bottom = screen->clipBound_Y2;
    int32 lineSize = x2 - x1;
    if (lineSize <= 0 || top >= bottom)
        return true;

    if (pspRotoCheck)
        PSPRotoCheck(layer);
    if (PSPQueueLayerRotozoomTexture(layer, x1, x2, top, bottom))
        return true;

    // Each tile crossed costs a GE segment (a few hundred cycles to build), each pixel costs the CPU sampler a few
    // dozen: rows that cross tiles every few pixels (the special stage floor towards the horizon) stay on the CPU, the
    // rest (its near half, most of its pixels) go to the GE.
    static bool onGE[SCREEN_YSIZE];
    static bool viaIndex[SCREEN_YSIZE];
    static bool halfRes[SCREEN_YSIZE];
    int32 geRows = 0, cpuRows = 0, geTop = bottom, geBottom = top;
    const bool overlap = pspGeOverlap && videoSettings.screenCount == 1 && screen == &screens[0];
    const int32 layerID = (int32)(layer - tileLayers) & (LAYER_COUNT - 1);
    const int32 geRun  = pspRotoGeRun >= 0 ? pspRotoGeRun : (overlap ? 0 : PSP_ROTO_GE_RUN);
    if (overlap && pspRotoIndexRun && !pspRotoIndexBuffer)
        pspRotoIndexBuffer = (uint8 *)memalign(64, 512 * PSP_ROTO_INDEX_ROWS);
    const bool indexRows = overlap && pspRotoIndexRun && pspRotoIndexBuffer && lineSize <= 512;
    int32 indexLeft      = PSP_ROTO_INDEX_ROWS - pspRotoIndexUsed;
    // how far the horizon reaches: the rows crossing a tile every 2, 3, 4 pixels or less
    uint32 farRows[3] = { 0, 0, 0 }, lodRows = 0;
    for (int32 cy = top; cy < bottom; ++cy) {
        const ScanlineInfo *line = &scanlines[cy];
        uint64 ax = (uint64)(line->deform.x < 0 ? -(int64)line->deform.x : line->deform.x) * lineSize;
        uint64 ay = (uint64)(line->deform.y < 0 ? -(int64)line->deform.y : line->deform.y) * lineSize;
        uint32 crossings = (uint32)((ax + ay) >> 20) + 1;
        for (int32 n = 0; n < 3; ++n)
            farRows[n] += (uint32)lineSize < (uint32)(n + 2) * crossings;
        lodRows += (uint32)lineSize < (uint32)pspHorizonLod * crossings;
    }
    for (int32 n = 0; n < 3; ++n) {
        pspHorizonRowsSum[n] += farRows[n];
        pspHorizonRowsMax[n] = MAX(pspHorizonRowsMax[n], farRows[n]);
    }
    pspHorizonFrames++;
    const int32 horizonLod = (int32)lodRows >= pspHorizonLodMinRows ? pspHorizonLod : 0;
    for (int32 cy = top; cy < bottom; ++cy) {
        ScanlineInfo *line = &scanlines[cy];
        // cell edges crossed ~ distance walked / 16 pixels, on each axis
        uint64 ax        = (uint64)(line->deform.x < 0 ? -(int64)line->deform.x : line->deform.x) * lineSize;
        uint64 ay        = (uint64)(line->deform.y < 0 ? -(int64)line->deform.y : line->deform.y) * lineSize;
        uint32 crossings = (uint32)((ax + ay) >> 20) + 1;
        onGE[cy]         = (uint32)lineSize >= (uint32)geRun * crossings;
        halfRes[cy]      = indexRows && indexLeft > 0 && (uint32)lineSize < (uint32)horizonLod * crossings;
        viaIndex[cy]     = halfRes[cy]
                       || (indexRows && indexLeft > 0 && (uint32)lineSize < (uint32)pspRotoIndexRun * crossings
                           && pspRotoRowDensity[layerID][cy] >= pspRotoIndexDensity);
        if (viaIndex[cy])
            --indexLeft, onGE[cy] = true;
        if (onGE[cy]) {
            geRows++;
            geTop    = MIN(geTop, cy);
            geBottom = MAX(geBottom, cy + 1);
        }
        else {
            cpuRows++;
        }
    }
    pspRotoGeRows += geRows;
    pspRotoCpuRows += cpuRows;
    pspRotoLayerW = layer->xsize, pspRotoLayerH = layer->ysize;
    if (!geRows)
        return false;

    // rows through pages (see PSPRotoPage), the nearest first (they cross the most tiles) while textures last
    static uint8 pageRuns[SCREEN_YSIZE];
    static PSPRotoPiece pagePieces[SCREEN_YSIZE][PSP_ROTO_PAGE_PIECES];
    bool paged = false;
    if (pspRotoPagesOn && overlap && !pspRotoCheck && layer->widthShift >= 5 && layer->heightShift >= 5 && lineSize <= 512
        && PSPRotoPagesWanted() && PSPRotoPagesReady(layer)) {
        ++pspRotoPageCall;
        for (int32 cy = bottom - 1; cy >= top; --cy) {
            // (a row sampled by the CPU stays so unless all of it goes through pages)
            bool full    = false;
            pageRuns[cy] = 0;
            const ScanlineInfo *line = &scanlines[cy];
            const uint64 ax = (uint64)(line->deform.x < 0 ? -(int64)line->deform.x : line->deform.x) * lineSize;
            const uint64 ay = (uint64)(line->deform.y < 0 ? -(int64)line->deform.y : line->deform.y) * lineSize;
            if ((uint32)lineSize < (uint32)pspRotoPageMin * ((uint32)((ax + ay) >> 20) + 1))
                continue;
            if (!onGE[cy] || halfRes[cy] || !PSPRotoRowPages(layer, &scanlines[cy], lineSize, pagePieces[cy], &pageRuns[cy], &full)
                || (viaIndex[cy] && !full)) {
                pageRuns[cy] = 0;
                continue;
            }
            viaIndex[cy] = false;
            paged        = true;
        }
        if (paged) {
            for (int32 i = 0; i < pspRotoPageCount; ++i) {
                if (pspRotoPages[i].usedAt == pspRotoPageCall && pspRotoPages[i].minX <= pspRotoPages[i].maxX)
                    PSPRotoPageTiles(layer, &pspRotoPages[i]);
            }
        }
        if (PSP_DUMPING) {
            int32 rows = 0, pieces = 0;
            for (int32 cy = top; cy < bottom; ++cy) rows += pageRuns[cy] != 0, pieces += pageRuns[cy];
            PrintLog(PRINT_NORMAL, "PSP dbg f%d roto layer %d: %dx%d tiles (shift %d,%d) rows %d-%d, paged rows %d pieces %d", pspFrameIndex,
                     layerID, layer->xsize, layer->ysize, layer->widthShift, layer->heightShift, top, bottom, rows, pieces);
            for (int32 i = 0; i < pspRotoPageCount; ++i) {
                PSPRotoPage *page = &pspRotoPages[i];
                if (page->usedAt == pspRotoPageCall)
                    PrintLog(PRINT_NORMAL, "PSP dbg   page %d id %08X tex %p bounds %X,%X-%X,%X", i, page->id, page->tex, page->minX, page->minY,
                             page->maxX, page->maxY);
            }
        }
    }

    if (cpuRows) {
        // what lies under these rows (earlier layers) must be drawn first
        if (psp3DPending)
            PSPFlush3D();
        int32 clipY1 = screen->clipBound_Y1, clipY2 = screen->clipBound_Y2;
        for (int32 cy = top; cy < bottom;) {
            if (onGE[cy]) {
                ++cy;
                continue;
            }
            int32 end = cy;
            while (end < bottom && !onGE[end]) ++end;
            screen->clipBound_Y1 = cy;
            screen->clipBound_Y2 = end;
            DrawLayerRotozoom(layer);
            cy = end;
        }
        screen->clipBound_Y1 = clipY1;
        screen->clipBound_Y2 = clipY2;
    }

    PSPGeLayerBegin(layer, x1, geTop, x2, geBottom);
    unsigned long long rotoStart = PSPProfNow(), flushBefore = psp3DFlushUS;

    const uint32 maskX = (1U << layer->widthShift) - 1, maskY = (1U << layer->heightShift) - 1;
    const int32 shift  = layer->widthShift;
    const float halfTexel = 1.0f / 131072.0f;
    unsigned long long blankStart = PSPProfFine();
    const uint32 *blank   = PSPRotoBlankBits(layer);
    pspRotoBlankUS += PSPProfFine() - blankStart;
    PSPRotoRowWalk walk;
    walk.layout  = layer->layout;
    walk.blank   = blank;
    walk.maskX   = maskX;
    walk.maskY   = maskY;
    walk.shift   = shift;
    walk.bxShift = layer->widthShift > 5 ? layer->widthShift - 5 : 0;
    walk.coarse  = blank ? pspRotoCoarse : NULL;
    walk.cbShift = layer->widthShift >= 3 ? layer->widthShift - 3 : 0;
    const PSPRotoCellMap *cellMap = PSPRotoCellBits(layer);
    walk.cells   = cellMap ? cellMap->cells : NULL;
    walk.entries = cellMap ? cellMap->entries : NULL;
    walk.stride  = cellMap ? cellMap->stride : 0;
    walk.xsize   = MIN(layer->xsize, 1 << layer->widthShift);
    const uint32 blocksX  = MAX(1, (1 << layer->widthShift) >> 5);

    if (paged && !pspRotoPagesNoDraw) { // (pspRotoPagesNoDraw, boot.txt "pagesnodraw": debugging, pages prepared but not drawn)
        // page by page (an item per page and palette bank), a line per piece of row: its ends half a step outside the
        // engine's first and last samples (the GE samples at pixel centres), clamped to the page (a sample straying
        // past its edge takes the edge texel instead of the far side's)
        const uint32 mask = (1U << PSP_ROTO_PAGE_SHIFT) - 1;
        const float texel = 1.0f / 65536.0f;
        float dbgMinU = 1e9f, dbgMaxU = -1e9f, dbgMinV = 1e9f, dbgMaxV = -1e9f, dbgSpan = 0, dbgDu = 0, dbgDv = 0;
        int32 dbgSpanRow = -1, dbgSpanLen = 0;
        for (int32 i = 0; i < pspRotoPageCount; ++i) {
            PSPRotoPage *page = &pspRotoPages[i];
            if (page->usedAt != pspRotoPageCall)
                continue;
            for (int32 cy = top; cy < bottom; ++cy) {
                for (uint32 n = 0; n < pageRuns[cy]; ++n) {
                    const PSPRotoPiece *piece = &pagePieces[cy][n];
                    if (piece->page != i || cy < pspRotoPagesRowA || cy >= pspRotoPagesRowB)
                        continue;
                    const ScanlineInfo *line = &scanlines[cy];
                    const int32 dx = line->deform.x, dy = line->deform.y;
                    const float du = (float)dx * texel, dv = (float)dy * texel;
                    const float pageOff = pspRotoPagesDebug == 6 ? 512.0f : 0.0f; // (debugging: coordinates kept positive)
                    const float u0 = (float)(((uint32)line->position.x + piece->k0 * (uint32)dx) & mask) * texel + pageOff;
                    const float v0 = (float)(((uint32)line->position.y + piece->k0 * (uint32)dy) & mask) * texel + pageOff;
                    const float run = (float)(piece->k1 - piece->k0);
                    PSPGe3DItem *item  = PSPGeLayerItem(gfxLineBuffer[cy], GU_LINES, 2 * sizeof(PSPGeLineVertex), page->tex, 512, 512);
                    item->clamped      = true;
                    PSPGeLineVertex *v = (PSPGeLineVertex *)&pspGe3DArena[pspGe3DArenaUsed];
                    v[0].u = u0 - 0.5f * du, v[0].v = v0 - 0.5f * dv, v[0].x = x1 + piece->k0, v[0].y = cy, v[0].z = 0;
                    v[1].u = u0 + (run - 0.5f) * du, v[1].v = v0 + (run - 0.5f) * dv, v[1].x = x1 + piece->k1, v[1].y = cy, v[1].z = 0;
                    if (pspRotoPagesDebug == 8) // (debugging: within one tile, as the walk's segments)
                        v[0].u = 2.0f, v[0].v = 2.0f, v[1].u = 14.0f, v[1].v = 14.0f;
                    if (PSP_DUMPING && ((cy >= 146 && cy < 156) || (cy >= 176 && cy < 180)))
                        PrintLog(PRINT_NORMAL, "PSP dbg L r%d p%d it%d bank%d x%d-%d uv %.2f,%.2f-%.2f,%.2f", cy, i, (int32)(item - pspGe3DItems), gfxLineBuffer[cy],
                                 v[0].x, v[1].x, v[0].u, v[0].v, v[1].u, v[1].v);
                    if (PSP_DUMPING) {
                        for (int32 e = 0; e < 2; ++e) {
                            dbgMinU = MIN(dbgMinU, v[e].u), dbgMaxU = MAX(dbgMaxU, v[e].u);
                            dbgMinV = MIN(dbgMinV, v[e].v), dbgMaxV = MAX(dbgMaxV, v[e].v);
                        }
                        float span = fabsf(v[1].u - v[0].u) + fabsf(v[1].v - v[0].v);
                        if (span > dbgSpan)
                            dbgSpan = span, dbgSpanRow = cy, dbgSpanLen = piece->k1 - piece->k0, dbgDu = du, dbgDv = dv;
                    }
                    item->count += 2;
                    pspGe3DArenaUsed += 2 * sizeof(PSPGeLineVertex);
                    pspGeLayerSprites++;
                    pspRotoPageLines++;
                }
            }
        }
        for (int32 cy = top; cy < bottom; ++cy) pspRotoPageRows += pageRuns[cy] != 0;
        if (PSP_DUMPING)
            PrintLog(PRINT_NORMAL, "PSP dbg f%d page lines: u %.2f..%.2f v %.2f..%.2f; longest texel span %.1f (row %d, %d px, du %.3f dv %.3f)",
                     pspFrameIndex, dbgMinU, dbgMaxU, dbgMinV, dbgMaxV, dbgSpan, dbgSpanRow, dbgSpanLen, dbgDu, dbgDv);
    }

    static bool flatRow[SCREEN_YSIZE];
    if (pspRotoFlatRows && overlap && !pspRotoFlatBuffer)
        pspRotoFlatBuffer = (uint8 *)memalign(64, 512 * PSP_ROTO_FLAT_ROWS);
    const bool flatOK = pspRotoFlatRows && overlap && pspRotoFlatBuffer && lineSize <= 512 && (TILE_SIZE << layer->widthShift) <= 512;
    for (int32 cy = geTop; cy < geBottom; ++cy) {
        flatRow[cy] = false;
        const ScanlineInfo *line = &scanlines[cy];
        if (!flatOK || !onGE[cy] || viaIndex[cy] || (paged && pageRuns[cy]) || line->deform.y || !line->deform.x
            || pspRotoFlatUsed >= PSP_ROTO_FLAT_ROWS)
            continue;
        const int32 row = pspRotoFlatUsed++;
        PSPRotoFlatRow(layer, (uint32)line->position.y, row);
        // the line's ends half a step outside the engine's first and last samples (the GE samples at pixel centres);
        // whole rows ahead of the start keep every coordinate positive, whichever way it heads; v at the row's centre
        const int32 dx     = line->deform.x;
        const uint32 ax    = dx < 0 ? -dx : dx;
        const float texel  = 1.0f / 65536.0f;
        const float offU   = (float)(512 * (1 + (uint32)(((uint64)(lineSize + 1) * ax) >> 16) / 512));
        const float du     = (float)dx * texel;
        const float u0     = (float)((uint32)line->position.x & ((512U << 16) - 1)) * texel + offU;
        PSPGe3DItem *item  = PSPGeLayerItem(gfxLineBuffer[cy], GU_LINES, 2 * sizeof(PSPGeLineVertex), pspRotoFlatBuffer, 512, PSP_ROTO_FLAT_ROWS);
        PSPGeLineVertex *v = (PSPGeLineVertex *)&pspGe3DArena[pspGe3DArenaUsed];
        v[0].u = u0 - 0.5f * du, v[0].v = (float)row + 0.5f, v[0].x = x1, v[0].y = cy, v[0].z = 0;
        v[1].u = u0 + ((float)lineSize - 0.5f) * du, v[1].v = (float)row + 0.5f, v[1].x = x2, v[1].y = cy, v[1].z = 0;
        item->count += 2;
        pspGe3DArenaUsed += 2 * sizeof(PSPGeLineVertex);
        pspGeLayerSprites++;
        pspRotoFlatCount++;
        flatRow[cy] = true;
    }

    for (int32 cy = geTop; cy < geBottom; ++cy) {
        if (!onGE[cy] || flatRow[cy])
            continue;
        ScanlineInfo *line = &scanlines[cy];
        uint32 px = (uint32)line->position.x, py = (uint32)line->position.y;
        int32 dx = line->deform.x, dy = line->deform.y;

        if (viaIndex[cy]) {
            unsigned long long idxStart = PSPProfFine();
            int32 row = pspRotoIndexUsed++;
            // (horizonlodx: half the samples, twice the step; the sprite below stretches them over the row)
            const bool halfX = halfRes[cy] && pspHorizonLodX;
            ScanlineInfo halfLine;
            if (halfX) {
                halfLine = *line;
                halfLine.deform.x *= 2;
                halfLine.deform.y *= 2;
            }
            const uint32 samples = halfX ? (lineSize + 1) >> 1 : lineSize;
            pspRotoRowDensity[layerID][cy] = PSPRotoIndexRow(layer, halfX ? &halfLine : line, &pspRotoIndexBuffer[row * 512], samples, blank, blocksX);
            // far towards the horizon (a tile every couple of pixels, each one a cache miss or two): optionally the row
            // below shows this one again (boot.txt horizonlod), so only every other one is sampled
            const int32 rows = halfRes[cy] && cy + 1 < geBottom && halfRes[cy + 1] ? 2 : 1;
            PSPGe3DItem *item = PSPGeLayerItem(gfxLineBuffer[cy], GU_SPRITES, 2 * sizeof(PSPGeSpriteVertex), pspRotoIndexBuffer, 512, PSP_ROTO_INDEX_ROWS);
            PSPGeSpriteVertex *sv = (PSPGeSpriteVertex *)&pspGe3DArena[pspGe3DArenaUsed];
            sv[0].u = 0, sv[0].v = row, sv[0].x = x1, sv[0].y = cy, sv[0].z = 0;
            sv[1].u = samples, sv[1].v = row + 1, sv[1].x = x2, sv[1].y = cy + rows, sv[1].z = 0;
            item->count += 2;
            pspGe3DArenaUsed += 2 * sizeof(PSPGeSpriteVertex);
            pspRotoIndexRows++;
            pspRotoIndexUS += PSPProfFine() - idxStart;
            cy += rows - 1;
            continue;
        }

        // the whole row, or (through pages) only its pieces without a texture
        uint16 walkFrom[PSP_ROTO_PAGE_PIECES], walkTo[PSP_ROTO_PAGE_PIECES];
        uint32 walks = 1;
        walkFrom[0] = 0, walkTo[0] = (uint16)lineSize;
        if (paged && pageRuns[cy]) {
            walks = 0;
            for (uint32 n = 0; n < pageRuns[cy]; ++n) {
                if (pagePieces[cy][n].page == PSP_ROTO_PAGE_NONE)
                    walkFrom[walks] = pagePieces[cy][n].k0, walkTo[walks++] = pagePieces[cy][n].k1;
            }
            if (!walks)
                continue;
        }

        unsigned long long rowStart = PSPProfFine();
        PSPGe3DItem *item = PSPGeLayerItem(gfxLineBuffer[cy], GU_LINES, lineSize * 2 * sizeof(PSPGeLineVertex));
        // stored uncached: a cached store to a line not in the cache first reads it from VRAM. Safe since no cached
        // write ever touches these lines (the item starts on a fresh line, only this walk appends to its kind of item)
        PSPGeLineVertex *v   = (PSPGeLineVertex *)((uintptr_t)&pspGe3DArena[pspGe3DArenaUsed] | (pspRotoUncached ? 0x40000000 : 0));
        PSPGeLineVertex *beg = v;

        uint32 rowRuns = 0, rowShown = 0;
        if (pspRotoEmit == 2) {
            // (measuring only: the same walk storing its vertices to RAM, dropped; the layer isn't drawn)
            static PSPGeLineVertex __attribute__((aligned(64))) scratch[1024];
            PSPRotoWalkRowGE(walk, px, py, dx, dy, lineSize, x1, cy, scratch, &rowRuns, &rowShown);
        }
        else if (pspRotoEmit) {
            for (uint32 w = 0; w < walks; ++w) {
                const uint32 k = walkFrom[w];
                uint32 runs = 0, shown = 0;
                v += PSPRotoWalkRowGE(walk, px + k * (uint32)dx, py + k * (uint32)dy, dx, dy, walkTo[w] - k, x1 + k, cy, v, &runs, &shown);
                rowRuns += runs, rowShown += shown;
            }
        }

        pspRotoWalkRuns += rowRuns;
        if (walks == 1 && walkTo[0] - walkFrom[0] == lineSize)
            pspRotoRowDensity[layerID][cy] = rowRuns ? rowShown * 100 / rowRuns : 0;
        uint32 used = (uint32)(v - beg);
        item->count += used;
        pspGe3DArenaUsed += used * sizeof(PSPGeLineVertex);
        pspGeLayerSprites += used / 2;
        pspGenItemUS += PSPProfFine() - rowStart, pspGenInitUS++;
    }
    unsigned long long flushed = psp3DFlushUS - flushBefore;
    pspGeRotoGenUS += (PSPProfNow() - rotoStart) - flushed;
    pspGeRotoFlushUS += flushed;
    if (pspLayerKickWait || !PSPGeBusy()) // (else the batch keeps growing: kicking now would wait for the GE)
        PSPKick3DMid();
    return true;
}

// INK_SUB in software takes away the source's complement, (31 - channel) * alpha, not the source (Drawing.cpp's
// subtractLookupTable): the GE subtracts the colour given, so that is the inverted one (GHZ2's pools, 0xA0E8F0, were
// drawn black instead of blue).
static inline uint32 PSPGe3DSubColor(uint32 abgr, int32 inkEffect) { return inkEffect == INK_SUB ? abgr ^ 0x00FFFFFF : abgr; }

// The software rasterizer's ink handling: the ink to draw with, INK_COUNT-style -1 when the GE can't do it (tint,
// masked...), -2 when nothing would be drawn. alpha is adjusted the same way DrawFace does.
int32 RSDK::PSPGe3DInk(int32 inkEffect, int32 *alpha)
{
    switch (inkEffect) {
        case INK_NONE:
        case INK_BLEND: *alpha = 0xFF; return inkEffect;
        case INK_ALPHA:
            if (*alpha > 0xFF) {
                *alpha = 0xFF;
                return INK_NONE;
            }
            return *alpha <= 0 ? -2 : INK_ALPHA;
        case INK_ADD:
        case INK_SUB:
            if (*alpha > 0xFF)
                *alpha = 0xFF;
            return *alpha <= 0 ? -2 : inkEffect;
        default: return -1;
    }
}

// Only opaque models use the depth buffer (translucent ones draw in order without it), so only they take a slice: a
// special stage frame has ~100 scenes, mostly shadows (blended), and running out of slices forced a GE wait.
static inline bool PSPGe3DNeedsSlice(int32 inkEffect, uint32 scene) { return inkEffect == INK_NONE && scene != pspGe3DSceneSerial; }

PSPGe3DModelVertex *RSDK::PSPBegin3DModel(int32 vertCount, int32 inkEffect, uint32 scene)
{
    if (PSPGe3DNeedsSlice(inkEffect, scene) && pspGe3DSlices == PSP_GE3D_SLICES)
        PSPFlush3D(); // out of depth slices: start over on a cleared depth buffer

    // the vertices start on a fresh cache line: they are stored uncached (see PSPGe3DUncached)
    uint32 aligned = (pspGe3DArenaUsed + 63) & ~63;
    if (aligned + sizeof(PSPGe3DModelVertex) * vertCount <= PSP_GE3D_ARENA)
        pspGe3DArenaUsed = aligned;
    PSPGe3DModelVertex *verts = (PSPGe3DModelVertex *)PSPGe3DReserve(sizeof(PSPGe3DModelVertex) * vertCount, PSP_GE3D_MODEL, inkEffect);
    if (verts && ((uintptr_t)verts & 63))
        return NULL; // (can't happen: a flush starts the arena over)
    if (!verts)
        return NULL;

    if (PSPGe3DNeedsSlice(inkEffect, scene)) { // (a flush in PSPGe3DReserve resets the serial too)
        pspGe3DSceneSerial = scene;
        pspGe3DSlice       = pspGe3DSlices++;
    }
    pspGe3DItems[pspGe3DItemCount - 1].count      = 0; // not drawable until PSPEnd3DModel
    pspGe3DItems[pspGe3DItemCount - 1].depthSlice = pspGe3DSlice;
    return verts;
}

uint32 RSDK::PSPGe3DBatch() { return pspGe3DBatchId; }
// The uncached alias of a model's vertices from PSPBegin3DModel: a cached store to a line not in the cache would first read
// it from VRAM (~190 cycles a line); these lines hold nothing else and no cached write touched them since the last kick.
int pspGe3DUncachedModels = 1; // boot.txt "cachedmodels" turns it off
PSPGe3DModelVertex *RSDK::PSPGe3DUncached(PSPGe3DModelVertex *verts)
{
    return pspGe3DUncachedModels ? (PSPGe3DModelVertex *)((uintptr_t)verts | 0x40000000) : verts;
}
uint32 RSDK::PSPGe3DOffset(const void *data) { return (uint32)((const uint8 *)data - pspGe3DArena); }

bool32 RSDK::PSPReuse3DModel(uint32 batch, uint32 offset, int32 inkEffect, uint32 scene)
{
    if (batch != pspGe3DBatchId || (PSPGe3DNeedsSlice(inkEffect, scene) && pspGe3DSlices == PSP_GE3D_SLICES))
        return false;
    if (!PSPGe3DReserve(0, PSP_GE3D_MODEL, inkEffect))
        return false;
    if (batch != pspGe3DBatchId) { // the item list was full: that flushed the vertices too
        pspGe3DItemCount--;
        return false;
    }

    if (PSPGe3DNeedsSlice(inkEffect, scene)) {
        pspGe3DSceneSerial = scene;
        pspGe3DSlice       = pspGe3DSlices++;
    }
    PSPGe3DItem *item = &pspGe3DItems[pspGe3DItemCount - 1];
    item->offset      = offset;
    item->count       = 0;
    item->depthSlice  = pspGe3DSlice;
    return true;
}

void RSDK::PSPEnd3DModel(const uint16 *indices, int32 indexCount, const Matrix *matWorld, int32 projectionX, int32 projectionY, int32 x1,
                         int32 y1, int32 x2, int32 y2, int32 cull, float zNear, float zFar)
{
    PSPGe3DItem *item = &pspGe3DItems[pspGe3DItemCount - 1];
    item->indices     = indices;
    item->count       = indexCount;
    item->cull        = cull;
    item->centerX     = currentScreen->center.x;
    item->centerY     = currentScreen->center.y;

    // the engine: v' = (m * v) >> 8 + translation, all 8.8 fixed point
    const float s = 1.0f / 256.0f;
    item->world.x = { matWorld->values[0][0] * s, matWorld->values[1][0] * s, matWorld->values[2][0] * s, 0 };
    item->world.y = { matWorld->values[0][1] * s, matWorld->values[1][1] * s, matWorld->values[2][1] * s, 0 };
    item->world.z = { matWorld->values[0][2] * s, matWorld->values[1][2] * s, matWorld->values[2][2] * s, 0 };
    item->world.w = { (float)matWorld->values[0][3], (float)matWorld->values[1][3], (float)matWorld->values[2][3], 1 };

    // screen = center + (x << projectionX) / z, center - (y << projectionY) / z: clip = (x << px, y << py, depth, z)
    // with depth mapped from the engine's near limit (0x100, faces closer were dropped) to far away
    item->projection.x = { (float)(1 << projectionX) / PSP_GE3D_NDC, 0, 0, 0 };
    item->projection.y = { 0, (float)(1 << projectionY) / PSP_GE3D_NDC, 0, 0 };
    item->projection.z = { 0, 0, (zFar + zNear) / (zFar - zNear), 1 };
    item->projection.w = { 0, 0, -2.0f * zFar * zNear / (zFar - zNear), 0 };

    if (item->inkEffect == INK_NONE)
        pspGe3DUsesDepth = true;
    PSPGe3DGrowBounds(x1, y1, x2, y2);
}

void RSDK::PSPEnd3DModelOrtho(const uint16 *indices, int32 indexCount, const Matrix *matWorld, int32 x1, int32 y1, int32 x2, int32 y2,
                              float zNear, float zFar)
{
    // the world matrix exactly as PSPEnd3DModel sets it up
    PSPEnd3DModel(indices, indexCount, matWorld, 0, 0, x1, y1, x2, y2, 0, zNear, zFar);
    PSPGe3DItem *item = &pspGe3DItems[pspGe3DItemCount - 1];

    // screen = (v.x / 256 - position.x, v.y / 256 - position.y), with the viewport's centre at the screen centre and
    // y up in clip space; depth from zNear (-1) to zFar (+1), w = 1 (no perspective divide)
    const float k  = 1.0f / (256.0f * PSP_GE3D_NDC);
    const float cx = (float)(currentScreen->position.x + currentScreen->center.x);
    const float cy = (float)(currentScreen->position.y + currentScreen->center.y);
    item->projection.x = { k, 0, 0, 0 };
    item->projection.y = { 0, -k, 0, 0 };
    item->projection.z = { 0, 0, 2.0f / (zFar - zNear), 0 };
    item->projection.w = { -cx / PSP_GE3D_NDC, cy / PSP_GE3D_NDC, -(zFar + zNear) / (zFar - zNear), 1 };
}

// ---------------------------------------------------------------------------------------------------------------
// Sprites on the GE.
// A sprite drawn by the CPU over GE work (spheres and rings over the special stage's models, say) used to make the CPU
// wait for the GE to finish first. Drawn by the GE instead, they join the batch in order: the sheet's 8-bit texels are
// sampled straight from RAM through the palette bank (index 0 transparent), on a quad mapped exactly like the
// software sampler. Inks other than none/blend/alpha/add/sub, palettes changing within the sprite, or a clip
// rectangle smaller than the screen keep the software path.
// ---------------------------------------------------------------------------------------------------------------
int pspGeSprites = 1; // boot.txt "nogesprites" turns it off
unsigned int pspGeSpriteCount;

struct PSPGeSpriteTriVertex {
    float u, v;
    uint32 color;
    float x, y, z;
};

// A palette bank's CLUT for this batch, shared by every sprite using the bank while it doesn't change. INK_SUB sprites
// have their own (the bank inverted, see PSPGe3DSubColor), kept after the plain ones.
static uint16 pspGeClutPalette[PALETTE_BANK_COUNT * 2][PALETTE_BANK_SIZE];
static uint32 pspGeClutBatch[PALETTE_BANK_COUNT * 2], pspGeClutOffset[PALETTE_BANK_COUNT * 2];

static void PSPGeWriteClut(uint32 *clut, int32 bank, bool invert)
{
    const uint16 *colors = fullPalette[bank];
    clut[0]              = 0; // transparent
    for (int32 c = 1; c < 0x100; ++c) {
        uint32 color = colors[c]; // R in the low bits, like the GE's 5650
        if (invert)
            color ^= 0xFFFF;
        uint32 r = color & 0x1F, g = (color >> 5) & 0x3F, b = (color >> 11) & 0x1F;
        clut[c] = 0xFF000000 | (((b << 3) | (b >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2));
    }
}

// Sprite frames cached in VRAM. Sampling the sheets in RAM made the GE compete with the CPU for main memory (the
// update of the next frame ran up to twice as long while the GE drew the sprites) and the GE slower too. Frames are
// copied into a 512x512 texture the first time they are drawn, packed in shelves, and found again by sheet and rect;
// when it is full the cache starts over (after the GE is done with it).
#define PSP_SPRITE_CACHE_SLOTS (1024)
struct PSPSpriteCacheEntry {
    uint32 sheet; // the sheet's file hash mixed with its slot
    uint32 pos;   // sprX | sprY << 16
    uint32 size;  // width | height << 16, 0 = empty slot
    uint16 x, y;  // where it is in the cache
};
static PSPSpriteCacheEntry pspSpriteCache[PSP_SPRITE_CACHE_SLOTS];
static int32 pspSpriteCacheUsed, pspSpriteShelfX, pspSpriteShelfY, pspSpriteShelfH;
unsigned int pspSpriteCacheResets, pspSpriteCacheCopies;

static uint8 *PSPSpriteCacheTexture() { return (uint8 *)sceGeEdramGetAddr() + PSP_VRAM_SPRITES; }

static void PSPResetSpriteCache()
{
    if (psp3DPending)
        PSPFlush3D();
    PSPWaitDisplayList(); // nothing may still be sampling it
    memset(pspSpriteCache, 0, sizeof(pspSpriteCache));
    pspSpriteCacheUsed = pspSpriteShelfX = pspSpriteShelfY = pspSpriteShelfH = 0;
    pspSpriteCacheResets++;
}

// Where frame (sprX, sprY, width, height) of the sheet sits in the cache, copying it in if needed; false: not cacheable.
static bool PSPCacheSpriteFrame(GFXSurface *surface, int32 sprX, int32 sprY, int32 width, int32 height, int32 *cx, int32 *cy)
{
    if (width <= 0 || height <= 0 || width > 256 || height > 256 || sprX < 0 || sprY < 0 || sprX + width > surface->width
        || sprY + height > surface->height)
        return false;

    uint32 sheet = *(const uint32 *)surface->hash ^ (uint32)(surface - gfxSurface) * 0x9E3779B9u; // file and slot
    uint32 pos = (uint32)sprX | ((uint32)sprY << 16), size = (uint32)width | ((uint32)height << 16);
    uint32 slot = ((sheet * 2654435761u) ^ (pos * 2246822519u) ^ (size * 3266489917u)) >> 22; // 10 bits
    for (int32 probe = 0; probe < PSP_SPRITE_CACHE_SLOTS; ++probe, slot = (slot + 1) & (PSP_SPRITE_CACHE_SLOTS - 1)) {
        PSPSpriteCacheEntry *e = &pspSpriteCache[slot];
        if (!e->size)
            break;
        if (e->sheet == sheet && e->pos == pos && e->size == size) {
            *cx = e->x, *cy = e->y;
            return true;
        }
    }

    // room: the current shelf, else a new one below it; a full cache (or table) starts over
    if (pspSpriteShelfX + width > 512) {
        pspSpriteShelfY += pspSpriteShelfH;
        pspSpriteShelfX = pspSpriteShelfH = 0;
    }
    if (pspSpriteShelfY + height > PSP_SPRITE_CACHE_ROWS || pspSpriteCacheUsed >= PSP_SPRITE_CACHE_SLOTS * 3 / 4)
        PSPResetSpriteCache();

    int32 x = pspSpriteShelfX, y = pspSpriteShelfY;
    pspSpriteShelfX += (width + 3) & ~3;
    pspSpriteShelfH = MAX(pspSpriteShelfH, height);

    const uint8 *src = &surface->pixels[sprX + sprY * surface->width];
    uint8 *dst       = &PSPSpriteCacheTexture()[x + y * 512];
    for (int32 r = 0; r < height; ++r, src += surface->width, dst += 512) memcpy(dst, src, width);
    // push it out of the CPU cache now: the range form also tells emulators (PPSSPP) the texture changed
    sceKernelDcacheWritebackRange(&PSPSpriteCacheTexture()[y * 512], height * 512);
    pspSpriteCacheCopies++;

    slot = ((sheet * 2654435761u) ^ (pos * 2246822519u) ^ (size * 3266489917u)) >> 22;
    while (pspSpriteCache[slot].size) slot = (slot + 1) & (PSP_SPRITE_CACHE_SLOTS - 1);
    PSPSpriteCacheEntry *e = &pspSpriteCache[slot];
    e->sheet = sheet, e->pos = pos, e->size = size, e->x = (uint16)x, e->y = (uint16)y;
    pspSpriteCacheUsed++;
    *cx = x, *cy = y;
    return true;
}

// The item to append this sprite's vertices to; NULL when it can't be queued.
static PSPGe3DItem *PSPGeSpriteItem(const void *tex, int32 tbw, int32 texH, int32 bank, int32 ink, uint32 bytes, bool32 rects = false,
                                    bool32 masked = false)
{
    if (psp3DPending && pspGe3DScreen != currentScreen)
        PSPFlush3D();

    const ScreenInfo *scr = currentScreen;
    const struct {
        int16 x1, y1, x2, y2;
    } clip                = { (int16)scr->clipBound_X1, (int16)scr->clipBound_Y1, (int16)scr->clipBound_X2, (int16)scr->clipBound_Y2 };
    const bool32 scissored = clip.x1 != 0 || clip.y1 != 0 || clip.x2 != scr->size.x || clip.y2 != scr->size.y;

    for (int32 attempt = 0; attempt < 2; ++attempt) {
        PSPGe3DItem *last = pspGe3DItemCount ? &pspGe3DItems[pspGe3DItemCount - 1] : NULL;
        const int32 clut  = bank + (ink == INK_SUB ? PALETTE_BANK_COUNT : 0);
        bool clutValid    = pspGeClutBatch[clut] == pspGe3DBatchId && !memcmp(pspGeClutPalette[clut], fullPalette[bank], sizeof(pspGeClutPalette[clut]));
        if (last && last->type == PSP_GE3D_SPRITE && last->tex == tex && last->tbw == tbw && last->texH == texH && last->prim == bank && last->inkEffect == ink
            && last->rects == rects && last->masked == masked && clutValid && last->scissored == scissored
            && (!scissored || (last->sx1 == clip.x1 && last->sy1 == clip.y1 && last->sx2 == clip.x2 && last->sy2 == clip.y2))
            && last->clutOffset == pspGeClutOffset[clut] && last->offset + last->count * sizeof(PSPGeSpriteTriVertex) == pspGe3DArenaUsed
            && pspGe3DArenaUsed + bytes <= PSP_GE3D_ARENA)
            return last;

        uint32 need = (clutValid ? 0 : 1024 + 16) + bytes + 16;
        if (pspGe3DArenaUsed + need > PSP_GE3D_ARENA || pspGe3DItemCount == PSP_GE3D_ITEMS) {
            if (attempt)
                return NULL;
            PSPFlush3D();
            continue;
        }

        if (!clutValid) {
            uint32 clutOffset = (pspGe3DArenaUsed + 15) & ~15;
            PSPGeWriteClut((uint32 *)&pspGe3DArena[clutOffset], bank, ink == INK_SUB);
            memcpy(pspGeClutPalette[clut], fullPalette[bank], sizeof(pspGeClutPalette[clut]));
            pspGeClutBatch[clut]  = pspGe3DBatchId;
            pspGeClutOffset[clut] = clutOffset;
            pspGe3DArenaUsed      = clutOffset + 1024;
        }

        PSPGe3DItem *item = &pspGe3DItems[pspGe3DItemCount++];
        item->type        = PSP_GE3D_SPRITE;
        item->inkEffect   = ink;
        item->prim        = bank;
        item->clutOffset  = pspGeClutOffset[clut];
        item->tex         = tex;
        item->tbw         = tbw;
        item->texH        = texH;
        item->rects       = rects;
        item->masked      = masked;
        item->scissored   = scissored;
        item->sx1 = clip.x1, item->sy1 = clip.y1, item->sx2 = clip.x2, item->sy2 = clip.y2;
        item->offset      = (pspGe3DArenaUsed + 3) & ~3;
        item->count       = 0;
        pspGe3DArenaUsed  = item->offset;
        return item;
    }
    return NULL;
}

// INK_MASKED sprites: the depth each next masked quad draws at (lower each time; see PSP_GE3D_MASK)
static float pspGeMaskZ = 65534.0f;

// The sprite item for masked sprites, with a mask item in front of it whenever it is a new one: it reads the screen as
// everything queued before has drawn it (the GE draws in order). NULL: draw it in software.
static bool PSPGeItemMasked(const PSPGe3DItem *item)
{
    return (item->type == PSP_GE3D_SPRITE && item->masked) || (item->type == PSP_GE3D_FACES && item->inkEffect == PSP_INK_MASKRECT);
}

// A mask item put in front of the (still empty) item just made, unless the item before it is masked too (then its
// mask still holds: what it drew wrote depth). false: no room (the new item is undone).
static bool PSPGeMaskNewItem()
{
    if (pspGe3DItemCount >= 2 && PSPGeItemMasked(&pspGe3DItems[pspGe3DItemCount - 2]))
        return true;
    if (pspGe3DItemCount == PSP_GE3D_ITEMS) {
        pspGe3DItemCount--;
        return false;
    }
    ScreenInfo *screen  = currentScreen;
    PSPGe3DItem *maskItem = &pspGe3DItems[pspGe3DItemCount - 1];
    PSPGe3DItem *newItem  = maskItem + 1;
    *newItem              = *maskItem;
    memset(maskItem, 0, sizeof(*maskItem));
    maskItem->type    = PSP_GE3D_MASK;
    maskItem->count   = 1;
    maskItem->tex     = screen->frameBuffer;
    uint32 m16        = maskColor; // R in the low bits, like the GE's 5650
    maskItem->maskRef = ((m16 & 0x1F) << 3) | (((m16 >> 5) & 0x3F) << 10) | (((m16 >> 11) & 0x1F) << 19);
    pspGe3DItemCount++;
    PSPGe3DGrowBounds(0, 0, screen->size.x, screen->size.y);
    pspGeMaskZ = 65534.0f;
    return true;
}

// INK_MASKED's DrawRect: an opaque rectangle over the pixels still showing the mask colour. false: software.
bool32 RSDK::PSPQueueMaskedRect(int32 x1, int32 y1, int32 x2, int32 y2, uint32 color)
{
    if (!pspGeSprites || !pspGe3DReady())
        return false;
    ScreenInfo *screen = currentScreen;
    x1 = MAX(x1, screen->clipBound_X1), x2 = MIN(x2, screen->clipBound_X2);
    y1 = MAX(y1, screen->clipBound_Y1), y2 = MIN(y2, screen->clipBound_Y2);
    if (x1 >= x2 || y1 >= y2)
        return true;
    // the mask colour over itself changes nothing (UIDiorama starts that way): those pixels stay maskable
    uint16 c16 = (uint16)(((color >> 19) & 0x1F) | (((color >> 10) & 0x3F) << 5) | (((color >> 3) & 0x1F) << 11));
    if (c16 == (uint16)maskColor)
        return true;
    int32 before         = pspGe3DItemCount;
    PSPGe3DFaceVertex *v = (PSPGe3DFaceVertex *)PSPGe3DReserve(2 * sizeof(PSPGe3DFaceVertex), PSP_GE3D_FACES, PSP_INK_MASKRECT);
    if (!v)
        return false;
    if (pspGe3DItemCount != before && !PSPGeMaskNewItem())
        return false;
    PSPGe3DItem *item = &pspGe3DItems[pspGe3DItemCount - 1];
    int16 z           = (int16)(uint16)pspGeMaskZ;
    if (pspGeMaskZ > 1.0f)
        pspGeMaskZ -= 1.0f;
    v = (PSPGe3DFaceVertex *)&pspGe3DArena[item->offset + item->count * sizeof(PSPGe3DFaceVertex)];
    v[0].color = v[1].color = 0xFF000000 | ((color & 0xFF) << 16) | (color & 0xFF00) | ((color >> 16) & 0xFF);
    v[0].x = (int16)x1, v[0].y = (int16)y1, v[0].z = z, v[0].pad = 0;
    v[1].x = (int16)x2, v[1].y = (int16)y2, v[1].z = z, v[1].pad = 0;
    item->count += 2;
    PSPGe3DGrowBounds(x1, y1, x2, y2);
    return true;
}

static PSPGe3DItem *PSPGeMaskedSpriteItem(const void *tex, int32 tbw, int32 texH, int32 bank, int32 ink, uint32 bytes, bool32 rects)
{
    int32 before      = pspGe3DItemCount;
    PSPGe3DItem *item = PSPGeSpriteItem(tex, tbw, texH, bank, ink, bytes, rects, true);
    if (!item || pspGe3DItemCount == before)
        return item; // (the masked item before this one, nothing drawn in between: its mask still holds)
    if (!PSPGeMaskNewItem())
        return NULL;
    return &pspGe3DItems[pspGe3DItemCount - 1];
}

// One sprite as a quad: screen corners qx/qy (top-left, top-right, bottom-left, bottom-right) showing sheet texel
// corners qu/qv. Pixels sample at their centres, so a corner at (x, y) maps to what the software samples for the pixel
// whose top-left is at (x, y). true: queued (or nothing to draw), false: draw it in software.
int pspSpriteLogFrame = -1; // boot.txt spritelog=N: every quad sprite queued in frame N is logged
int pspSpriteAnyBank = 0; // boot.txt "spriteanybank" (test): sprites over rows of several palette banks still go to the GE
unsigned int pspSpriteBankRefusals;
static bool32 PSPQueueSpriteQuad(int32 sheetID, const float *qx, const float *qy, float *qu, float *qv, int32 inkEffect, int32 alpha, int32 sprX,
                                 int32 sprY, int32 sprW, int32 sprH)
{
    if (!pspGeSprites || !pspGe3DReady())
        return false;

    const bool32 masked = inkEffect == INK_MASKED;
    switch (inkEffect) {
        case INK_NONE:
        case INK_BLEND: alpha = 0xFF; break;
        case INK_MASKED: alpha = 0xFF, inkEffect = INK_NONE; break; // opaque, wherever the mask colour still is
        case INK_ALPHA:
        case INK_ADD:
        case INK_SUB: alpha = CLAMP(alpha, 0, 0xFF); break;
        default: return false;
    }

    ScreenInfo *screen = currentScreen; // (a narrower clip is the sprite item's scissor, see PSPGeSpriteItem)
    if (screen->clipBound_X1 >= screen->clipBound_X2 || screen->clipBound_Y1 >= screen->clipBound_Y2)
        return true;

    float minX = qx[0], maxX = qx[0], minY = qy[0], maxY = qy[0];
    float minU = qu[0], maxU = qu[0], minV = qv[0], maxV = qv[0];
    for (int32 c = 1; c < 4; ++c) {
        minX = MIN(minX, qx[c]), maxX = MAX(maxX, qx[c]);
        minY = MIN(minY, qy[c]), maxY = MAX(maxY, qy[c]);
        minU = MIN(minU, qu[c]), maxU = MAX(maxU, qu[c]);
        minV = MIN(minV, qv[c]), maxV = MAX(maxV, qv[c]);
    }
    int32 x1 = MAX((int32)floorf(minX), screen->clipBound_X1), x2 = MIN((int32)ceilf(maxX), screen->clipBound_X2);
    int32 y1 = MAX((int32)floorf(minY), screen->clipBound_Y1), y2 = MIN((int32)ceilf(maxY), screen->clipBound_Y2);
    if (x1 >= x2 || y1 >= y2)
        return true;

    int32 bank = gfxLineBuffer[y1];
    for (int32 y = y1 + 1; y < y2 && !pspSpriteAnyBank; ++y) {
        if (gfxLineBuffer[y] != bank) {
            pspSpriteBankRefusals++;
            return false;
        }
    }

    GFXSurface *surface = &gfxSurface[sheetID];
    int32 width         = surface->width;
    if (!surface->pixels || width < 16 || (width & 15) || width > 1024 || minU < 0 || minV < 0)
        return false;

    const uint8 *base = surface->pixels;
    float offU = 0, offV = 0;
    int32 cx, cy;
    if (PSPCacheSpriteFrame(surface, sprX, sprY, sprW, sprH, &cx, &cy)) {
        // the frame's copy in VRAM
        base  = PSPSpriteCacheTexture();
        width = 512;
        offU  = (float)(sprX - cx);
        offV  = (float)(sprY - cy);
    }
    else if (width > 512 || surface->height > 512) {
        // texture base: the sheet itself when it fits a 512x512 texture, else the rows (and 16-texel column) the sprite starts at
        int32 row = (int32)minV, col = width > 512 ? ((int32)minU & ~15) : 0;
        base += row * width + col;
        offU = (float)col, offV = (float)row;
    }
    uint32 misalign = (uint32)((uintptr_t)base & 15); // the GE wants 16-byte aligned textures
    base -= misalign;
    offU -= (float)misalign;
    if (maxU - offU > 512.0f || maxV - offV > 512.0f)
        return false;

    // The texture must not claim memory past what it can read (the GE drops a list whose texture runs off the end of
    // VRAM, and the frame blit queued behind it): the VRAM cache is exactly its rows, a sheet the power of two covering
    // the rows this sprite uses.
    int32 texH = PSP_SPRITE_CACHE_ROWS;
    if (base != PSPSpriteCacheTexture()) {
        texH = 1;
        while (texH < (int32)ceilf(maxV - offV) && texH < 512) texH <<= 1;
    }
    PSPGe3DItem *item = masked ? PSPGeMaskedSpriteItem(base, width, texH, bank, inkEffect, 6 * sizeof(PSPGeSpriteTriVertex), false)
                               : PSPGeSpriteItem(base, width, texH, bank, inkEffect, 6 * sizeof(PSPGeSpriteTriVertex));
    if (!item)
        return false;
    const float z = masked ? pspGeMaskZ : 0.0f;
    if (masked && pspGeMaskZ > 1.0f)
        pspGeMaskZ -= 1.0f;

    if (pspFrameIndex == pspSpriteLogFrame)
        PrintLog(PRINT_NORMAL, "PSP sprite f%d sheet %d spr %d,%d %dx%d quad (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f) uv (%.2f,%.2f)-(%.2f,%.2f) base %08X w %d off %.1f,%.1f ink %d a %d bank %d item %d",
                 pspFrameIndex, sheetID, sprX, sprY, sprW, sprH, qx[0], qy[0], qx[1], qy[1], qx[2], qy[2], qx[3], qy[3], minU, minV, maxU, maxV,
                 (uint32)(uintptr_t)base, width, offU, offV, inkEffect, alpha, bank, pspGe3DItemCount);
    static const uint8 order[6] = { 0, 1, 2, 2, 1, 3 };
    PSPGeSpriteTriVertex *v     = (PSPGeSpriteTriVertex *)&pspGe3DArena[pspGe3DArenaUsed];
    uint32 color                = ((uint32)alpha << 24) | 0xFFFFFF;
    for (int32 i = 0; i < 6; ++i) {
        int32 c    = order[i];
        v[i].u     = qu[c] - offU;
        v[i].v     = qv[c] - offV;
        v[i].color = color;
        v[i].x     = qx[c];
        v[i].y     = qy[c];
        v[i].z     = z;
    }
    item->count += 6;
    pspGe3DArenaUsed += 6 * sizeof(PSPGeSpriteTriVertex);
    pspGeSpriteCount++;
    PSPGe3DGrowBounds(x1, y1, x2, y2);
    return true;
}

// DrawSpriteFlipped's sprite (before its clipping): sheet rect (sprX, sprY, width, height) at (x, y), mirrored by direction.
bool32 RSDK::PSPQueueSpriteRect(int32 x, int32 y, int32 width, int32 height, int32 sprX, int32 sprY, int32 direction, int32 inkEffect, int32 alpha,
                                int32 sheetID)
{
    if (width <= 0 || height <= 0)
        return true;
    // a sprite over 512 texels a side (TMZ1's 1024x1024 Monarch halves) can't be one texture: only the part on screen
    // is, and the screen is smaller than that
    if (width > 512 || height > 512) {
        ScreenInfo *screen = currentScreen;
        int32 x1 = MAX(x, 0), y1 = MAX(y, 0), x2 = MIN(x + width, screen->size.x), y2 = MIN(y + height, screen->size.y);
        if (x1 >= x2 || y1 >= y2)
            return true;
        if (x2 - x1 > 512 || y2 - y1 > 512)
            return false;
        int32 offX = x1 - x, offY = y1 - y, w = x2 - x1, h = y2 - y1;
        int32 tx = (direction & FLIP_X) ? sprX + width - offX - w : sprX + offX;
        int32 ty = (direction & FLIP_Y) ? sprY + height - offY - h : sprY + offY;
        return PSPQueueSpriteRect(x1, y1, w, h, tx, ty, direction, inkEffect, alpha, sheetID);
    }
    float qx[4] = { (float)x, (float)(x + width), (float)x, (float)(x + width) };
    float qy[4] = { (float)y, (float)y, (float)(y + height), (float)(y + height) };
    float u0 = (float)sprX, u1 = (float)(sprX + width), v0 = (float)sprY, v1 = (float)(sprY + height);
    if (direction & FLIP_X) {
        float t = u0;
        u0 = u1, u1 = t;
    }
    if (direction & FLIP_Y) {
        float t = v0;
        v0 = v1, v1 = t;
    }
    float qu[4] = { u0, u1, u0, u1 };
    float qv[4] = { v0, v0, v1, v1 };
    return PSPQueueSpriteQuad(sheetID, qx, qy, qu, qv, inkEffect, alpha, sprX, sprY, width, height);
}

// DrawSpriteRotozoom's sprite: the pixel at (sx, sy) samples (texX + deltaX * (sx - x) - deltaXLen * (sy - y),
// texY + deltaY * (sx - x) + deltaYLen * (sy - y)) (16.16), when inside the sheet rect (sprX, sprY, width, height).
// The quad is that rect mapped back to the screen. (Measured from the sprite's origin: the engine's own start values,
// taken at the clipped corner, overflow 32 bits for tiny or far off-screen sprites - harmless for its wrapping
// arithmetic, not for this.)
// The same quad PSPQueueSpriteAffine makes for DrawSpriteRotozoom's sprite when it isn't rotated: texel (u, v) is drawn at
// x + (u * 65536 - texX) / deltaX, y + (v * 65536 - texY) / deltaY, deltaX = 128 * (512 * 512 / scaleX) (16.16 per pixel).
bool32 RSDK::PSPQueueSpriteScaled(int32 x, int32 y, int32 texX, int32 texY, int32 sprX, int32 sprY, int32 width, int32 height, int32 scaleX,
                                  int32 scaleY, int32 direction, int32 inkEffect, int32 alpha, int32 sheetID)
{
    if (scaleX <= 0 || scaleY <= 0 || width <= 0 || height <= 0)
        return false;
    const int32 fullScaleX = 0x40000 / scaleX, fullScaleY = 0x40000 / scaleY; // (512.0 / scale) * 512.0, truncated
    if (!fullScaleX || !fullScaleY)
        return false;
    float originX = (float)texX * 65536.0f, originY = (float)texY * 65536.0f;
    float stepX = (float)fullScaleX * 128.0f, stepY = (float)fullScaleY * 128.0f;
    if (direction == FLIP_X) {
        originX -= (float)(fullScaleX >> 1);
        stepX = -stepX;
    }
    const float invX = 1.0f / stepX, invY = 1.0f / stepY;
    const float inset = 1.0f / 64.0f; // (as PSPQueueSpriteAffine)
    float tu[4] = { sprX + inset, sprX + width - inset, sprX + inset, sprX + width - inset };
    float tv[4] = { sprY + inset, sprY + inset, sprY + height - inset, sprY + height - inset };
    float qx[4], qy[4];
    for (int32 c = 0; c < 4; ++c) {
        qx[c] = (float)x + (tu[c] * 65536.0f - originX) * invX + 0.5f;
        qy[c] = (float)y + (tv[c] * 65536.0f - originY) * invY + 0.5f;
    }
    return PSPQueueSpriteQuad(sheetID, qx, qy, tu, tv, inkEffect, alpha, sprX, sprY, width, height);
}

bool32 RSDK::PSPQueueSpriteAffine(int32 x, int32 y, int32 texX, int32 texY, int32 deltaX, int32 deltaY, int32 deltaXLen, int32 deltaYLen,
                                  int32 sprX, int32 sprY, int32 width, int32 height, int32 inkEffect, int32 alpha, int32 sheetID)
{
    float det = (float)deltaX * (float)deltaYLen + (float)deltaXLen * (float)deltaY;
    if (fabsf(det) < 1.0f || width <= 0 || height <= 0)
        return false;

    // a hair inside the rect: the GE's interpolation must never reach the neighbouring sprite on the sheet
    const float inset = 1.0f / 64.0f;
    float tu[4] = { sprX + inset, sprX + width - inset, sprX + inset, sprX + width - inset };
    float tv[4] = { sprY + inset, sprY + inset, sprY + height - inset, sprY + height - inset };
    float qx[4], qy[4];
    for (int32 c = 0; c < 4; ++c) {
        float dx = tu[c] * 65536.0f - (float)texX;
        float dy = tv[c] * 65536.0f - (float)texY;
        float u  = (dx * (float)deltaYLen + (float)deltaXLen * dy) / det;
        float v  = ((float)deltaX * dy - (float)deltaY * dx) / det;
        qx[c]    = (float)x + u + 0.5f;
        qy[c]    = (float)y + v + 0.5f;
    }
    return PSPQueueSpriteQuad(sheetID, qx, qy, tu, tv, inkEffect, alpha, sprX, sprY, width, height);
}

// DrawDeformedSprite: every scanline samples the sheet (wrapping, it is a power of two) from position by deform per
// pixel. A whole-screen effect done a pixel at a time (OOZ's smog, blended over everything) is costly on the CPU;
// here each scanline becomes a few one-pixel-high quads, cut where the walk wraps around the sheet. Only scanlines
// that walk horizontally (deform.y == 0, which is what the game uses) are taken; the first one that can't be queued
// and all after it are left to the software loop.
int32 RSDK::PSPQueueDeformedSprite(uint16 sheetID, int32 inkEffect, int32 alpha)
{
    ScreenInfo *screen  = currentScreen;
    int32 y             = screen->clipBound_Y1;
    GFXSurface *surface = &gfxSurface[sheetID];
    const int32 w = surface->width, h = surface->height;
    if (!pspGeSprites || !pspGe3DReady() || !surface->pixels || w < 16 || (w & (w - 1)) || (h & (h - 1)) || w > 512
        || h > PSP_SPRITE_CACHE_ROWS)
        return y;
    if (screen->clipBound_X1 != 0 || screen->clipBound_X2 != screen->size.x)
        return y;
    const bool32 masked = inkEffect == INK_MASKED;
    switch (inkEffect) {
        case INK_NONE:
        case INK_BLEND: alpha = 0xFF; break;
        case INK_MASKED: alpha = 0xFF, inkEffect = INK_NONE; break; // opaque, wherever the mask colour is (below)
        case INK_ALPHA:
        case INK_ADD:
        case INK_SUB: alpha = CLAMP(alpha, 0, 0xFF); break;
        default: return y;
    }

    // the whole sheet in the VRAM sprite cache once; every piece is a 2-vertex sprite into it
    int32 cx, cy;
    if (!PSPCacheSpriteFrame(surface, 0, 0, w, h, &cx, &cy))
        return y;

    if (masked) {
        // the mask item reads the screen back after everything queued before it, in the same list: the GE draws in
        // order, and the texture cache is flushed first (the CPU's own drawing was written back when its batch began)
        if (!PSPGeMaskedSpriteItem(PSPSpriteCacheTexture(), 512, PSP_SPRITE_CACHE_ROWS, gfxLineBuffer[y], inkEffect, 0, true))
            return y;
    }
    const void *tex    = PSPSpriteCacheTexture();
    const uint32 color = ((uint32)alpha << 24) | 0xFFFFFF;
    const int32 width  = screen->size.x;
    const uint32 wMask = ((uint32)w << 16) - 1; // the sheet's width in 16.16, less one
    const float texel  = 1.0f / 65536.0f;
    const float z      = masked ? 1.0f : 0.0f; // masked pieces pass the depth test only where the mask item left 0
    PSPGe3DItem *item  = NULL;
    int32 itemBank     = -1;

    for (; y < screen->clipBound_Y2; ++y) {
        const ScanlineInfo *line = &scanlines[y];
        if (line->deform.y)
            return y;
        const int32 dx  = line->deform.x;
        const uint32 ax = dx < 0 ? -dx : dx;
        // pieces this scanline needs, at most: one per wrap around the sheet, plus the partial ones at either end
        uint32 pieces = (uint32)(((uint64)width * ax) >> 16) / w + 2;
        if (pieces > 64)
            return y;
        // (the same item as the line before while it has the same palette bank and room: nothing else was queued)
        const uint32 bytes = pieces * 2 * sizeof(PSPGeSpriteTriVertex);
        if (!item || itemBank != gfxLineBuffer[y] || pspGe3DArenaUsed + bytes > PSP_GE3D_ARENA || &pspGe3DItems[pspGe3DItemCount - 1] != item) {
            // a masked item that starts a new batch (the arena filled up mid-screen) needs its own mask item: without
            // it the rest of the screen tested against a cleared depth buffer and the sheet covered everything below
            // (UFO7's plasma over the whole track)
            item = masked ? PSPGeMaskedSpriteItem(tex, 512, PSP_SPRITE_CACHE_ROWS, gfxLineBuffer[y], inkEffect, bytes, true)
                          : PSPGeSpriteItem(tex, 512, PSP_SPRITE_CACHE_ROWS, gfxLineBuffer[y], inkEffect, bytes, true, masked);
            if (!item)
                return y;
            itemBank = gfxLineBuffer[y];
        }
        PSPGeSpriteTriVertex *v = (PSPGeSpriteTriVertex *)&pspGe3DArena[pspGe3DArenaUsed];
        PSPGeSpriteTriVertex *start = v;

        const float tv = (float)(cy + ((line->position.y >> 16) & (h - 1))) + 0.5f;
        uint32 pos     = (uint32)line->position.x; // 16.16, as the software walk (wrapping like its 32 bits)
        for (int32 sx = 0; sx < width && (uint32)(v - start) < pieces * 2;) {
            uint32 u = pos & wMask; // where in the sheet's width
            // pixels until the walk wraps around the sheet: ceil(distance / step)
            int32 n = width - sx;
            if (dx > 0)
                n = MIN(n, (int32)((wMask + 1 - u + ax - 1) / ax));
            else if (dx < 0)
                n = MIN(n, (int32)((u + ax - 1) / ax));
            n          = MAX(n, 1);
            int32 uEnd = (int32)u + dx * n;
            uEnd       = CLAMP(uEnd, 0, (int32)(wMask + 1));
            v[0].u = cx + (float)u * texel, v[0].v = tv, v[0].color = color, v[0].x = (float)sx, v[0].y = (float)y, v[0].z = z;
            v[1].u = cx + (float)uEnd * texel, v[1].v = tv, v[1].color = color, v[1].x = (float)(sx + n), v[1].y = (float)(y + 1), v[1].z = z;
            v += 2;
            sx += n;
            pos += (uint32)dx * (uint32)n;
        }
        uint32 used = (uint32)(v - start);
        item->count += used;
        pspGe3DArenaUsed += used * sizeof(PSPGeSpriteTriVertex);
        pspGeSpriteCount += used / 2;
    }
    PSPGe3DGrowBounds(0, screen->clipBound_Y1, width, y);
    return y;
}

// One clipped polygon vertex: position in pixels and colour channels 0-255.
struct PSPClipVertex {
    float x, y, r, g, b;
};

// Sutherland-Hodgman against one edge of the clip rectangle; colours interpolate linearly like the software spans.
static int32 PSPClipPolygon(const PSPClipVertex *in, int32 count, PSPClipVertex *out, int32 axis, float bound, bool keepGreater)
{
    int32 outCount = 0;
    for (int32 i = 0; i < count; ++i) {
        const PSPClipVertex *a = &in[i];
        const PSPClipVertex *b = &in[i + 1 == count ? 0 : i + 1];
        float va = axis ? a->y : a->x;
        float vb = axis ? b->y : b->x;
        bool aIn = keepGreater ? va >= bound : va <= bound;
        bool bIn = keepGreater ? vb >= bound : vb <= bound;

        if (aIn)
            out[outCount++] = *a;
        if (aIn != bIn) {
            float t          = (bound - va) / (vb - va);
            PSPClipVertex *v = &out[outCount++];
            v->x             = a->x + (b->x - a->x) * t;
            v->y             = a->y + (b->y - a->y) * t;
            v->r             = a->r + (b->r - a->r) * t;
            v->g             = a->g + (b->g - a->g) * t;
            v->b             = a->b + (b->b - a->b) * t;
            if (axis)
                v->y = bound;
            else
                v->x = bound;
        }
    }
    return outCount;
}

// Queues one screen-space face (16.16 positions, as DrawFace/DrawBlendedFace take them). colors: one 0xRRGGBB per
// vertex, or NULL for a flat face of flatColor. Returns false when the face needs the software path (the caller then
// draws it there, which flushes the batch first).
int pspGeFaces = 1; // boot.txt "nogefaces": faces and rectangles drawn in software (for comparisons)
bool32 RSDK::PSPQueue3DFace(Vector2 *vertices, uint32 *colors, uint32 flatColor, int32 vertCount, int32 alpha, int32 inkEffect)
{
    if (!pspGeFaces || vertCount < 3 || vertCount > 4)
        return false;

    inkEffect = PSPGe3DInk(inkEffect, &alpha);
    if (inkEffect == -1)
        return false;
    if (inkEffect == -2)
        return true;

    // positions are truncated to whole pixels like FROM_FIXED in the software rasterizer
    PSPClipVertex bufA[12], bufB[12];
    for (int32 v = 0; v < vertCount; ++v) {
        uint32 c  = colors ? colors[v] : flatColor;
        bufA[v].x = (float)(vertices[v].x >> 16);
        bufA[v].y = (float)(vertices[v].y >> 16);
        bufA[v].r = (float)((c >> 16) & 0xFF);
        bufA[v].g = (float)((c >> 8) & 0xFF);
        bufA[v].b = (float)(c & 0xFF);
    }

    float clipX1 = (float)currentScreen->clipBound_X1, clipX2 = (float)currentScreen->clipBound_X2;
    float clipY1 = (float)currentScreen->clipBound_Y1, clipY2 = (float)currentScreen->clipBound_Y2;
    if (clipX1 >= clipX2 || clipY1 >= clipY2)
        return true;

    int32 count = vertCount;
    count       = PSPClipPolygon(bufA, count, bufB, 0, clipX1, true);
    count       = PSPClipPolygon(bufB, count, bufA, 0, clipX2, false);
    count       = PSPClipPolygon(bufA, count, bufB, 1, clipY1, true);
    count       = PSPClipPolygon(bufB, count, bufA, 1, clipY2, false);
    if (count < 3)
        return true; // nothing on screen

    float minX = bufA[0].x, maxX = bufA[0].x, minY = bufA[0].y, maxY = bufA[0].y;
    for (int32 v = 1; v < count; ++v) {
        minX = MIN(minX, bufA[v].x);
        maxX = MAX(maxX, bufA[v].x);
        minY = MIN(minY, bufA[v].y);
        maxY = MAX(maxY, bufA[v].y);
    }
    int32 x1 = (int32)minX, y1 = (int32)minY;
    int32 x2 = MIN((int32)maxX + 1, currentScreen->clipBound_X2), y2 = MIN((int32)maxY + 1, currentScreen->clipBound_Y2);
    if (x2 <= x1 || y2 <= y1)
        return true;

    int32 triVerts         = (count - 2) * 3;
    PSPGe3DFaceVertex *dst = (PSPGe3DFaceVertex *)PSPGe3DReserve(sizeof(PSPGe3DFaceVertex) * triVerts, PSP_GE3D_FACES, inkEffect);
    if (!dst)
        return false;

    PSPGe3DFaceVertex poly[12];
    for (int32 v = 0; v < count; ++v) {
        PSPGe3DFaceVertex *out = &poly[v];
        out->x                 = (int16)(bufA[v].x + 0.5f);
        out->y                 = (int16)(bufA[v].y + 0.5f);
        out->z                 = 0;
        out->pad               = 0;
        out->color             = PSPGe3DSubColor(((uint32)alpha << 24) | ((uint32)bufA[v].b << 16) | ((uint32)bufA[v].g << 8) | (uint32)bufA[v].r, inkEffect);
    }

    for (int32 t = 1; t + 1 < count; ++t) {
        *dst++ = poly[0];
        *dst++ = poly[t];
        *dst++ = poly[t + 1];
    }
    pspGe3DItems[pspGe3DItemCount - 1].count += triVerts;
    PSPGe3DGrowBounds(x1, y1, x2, y2);
    return true;
}

// INK_TINT through a table that inverts every colour (FXRuby's: table[0xFFFF - c] = c): the GE's logic op does exactly
// that to the screen, so those shapes go to the GE as spans. The table is checked whole whenever it changes pointer, and
// at a few places on every call.
static const uint16 *pspTintChecked[4];
static bool pspTintInverse[4];
static int32 pspTintNext;
bool32 RSDK::PSPTintIsInverse()
{
    if (!pspGeSprites || !pspGe3DReady() || !tintLookupTable)
        return false;
    const uint16 *t = tintLookupTable;
    if (t[0] != 0xFFFF || t[0x1234] != 0xEDCB || t[0xFFFF] != 0)
        return false;
    for (int32 i = 0; i < 4; ++i) {
        if (pspTintChecked[i] == t)
            return pspTintInverse[i];
    }
    int32 i           = pspTintNext++ & 3; // (the last few tables seen: some stages switch between several)
    pspTintChecked[i] = t;
    pspTintInverse[i] = true;
    for (int32 c = 0; c < 0x10000 && pspTintInverse[i]; ++c) pspTintInverse[i] = t[c] == (uint16)(0xFFFF - c);
    return pspTintInverse[i];
}

static PSPGe3DFaceVertex *pspInvertVerts;
static int32 pspInvertLeft, pspInvertX1, pspInvertY1, pspInvertX2, pspInvertY2;
bool32 RSDK::PSPInvertBegin(int32 maxSpans)
{
    if (maxSpans <= 0)
        return false;
    pspInvertVerts = (PSPGe3DFaceVertex *)PSPGe3DReserve(sizeof(PSPGe3DFaceVertex) * 2 * maxSpans, PSP_GE3D_FACES, PSP_INK_INVERT);
    if (!pspInvertVerts)
        return false;
    pspInvertLeft = maxSpans;
    pspInvertX1 = pspInvertY1 = 0x7FFF;
    pspInvertX2 = pspInvertY2 = -1;
    return true;
}
void RSDK::PSPInvertSpan(int32 y, int32 x1, int32 x2, int32 rows)
{
    if (x2 <= x1 || !pspInvertLeft)
        return;
    pspInvertLeft--;
    PSPGe3DFaceVertex *v = pspInvertVerts;
    v[0].color = v[1].color = 0xFFFFFFFF;
    v[0].x = (int16)x1, v[0].y = (int16)y, v[0].z = 0, v[0].pad = 0;
    v[1].x = (int16)x2, v[1].y = (int16)(y + rows), v[1].z = 0, v[1].pad = 0;
    pspInvertVerts += 2;
    pspGe3DItems[pspGe3DItemCount - 1].count += 2;
    pspInvertX1 = MIN(pspInvertX1, x1), pspInvertX2 = MAX(pspInvertX2, x2);
    pspInvertY1 = MIN(pspInvertY1, y), pspInvertY2 = MAX(pspInvertY2, y + rows);
}
void RSDK::PSPInvertEnd()
{
    if (pspInvertX2 > pspInvertX1)
        PSPGe3DGrowBounds(pspInvertX1, pspInvertY1, pspInvertX2, pspInvertY2);
}

// FillScreen on the GE, exactly: the software blends each channel on its own through 5-bit tables (green too), so the
// result is a lookup table per channel. The GE reads the screen back as a 16-bit indexed texture three times, each time
// indexing a CLUT with one channel's bits and writing only that channel (PSP_GE3D_LUT).
bool32 RSDK::PSPQueueFillScreen(uint32 color, int32 alphaR, int32 alphaG, int32 alphaB)
{
    if (!pspGeSprites || !pspGe3DReady())
        return false;
    ScreenInfo *screen = currentScreen;
    uint8 *data        = (uint8 *)PSPGe3DReserve(512 + 16, PSP_GE3D_LUT, 0);
    if (!data)
        return false;
    uint32 *clut      = (uint32 *)(((uintptr_t)data + 15) & ~(uintptr_t)15);
    PSPGe3DItem *item = &pspGe3DItems[pspGe3DItemCount - 1];
    item->clutOffset  = (uint32)((uint8 *)clut - pspGe3DArena);
    item->tex         = screen->frameBuffer;
    item->count       = 1;
    // (BGR565: red is the low lane, blue the high one; FillScreen's lanes are named after the PC's RGB565)
    const uint16 *keepR = &blendLookupTable[0x20 * (0xFF - alphaR)], *keepG = &blendLookupTable[0x20 * (0xFF - alphaG)];
    const uint16 *keepB = &blendLookupTable[0x20 * (0xFF - alphaB)];
    const uint32 addR = blendLookupTable[0x20 * alphaR + (((color >> 16) & 0xFF) >> 3)];
    const uint32 addG = blendLookupTable[0x20 * alphaG + (((color >> 8) & 0xFF) >> 3)];
    const uint32 addB = blendLookupTable[0x20 * alphaB + ((color & 0xFF) >> 3)];
    for (int32 i = 0; i < 32; ++i) {
        clut[i]      = 0xFF000000 | (((keepR[i] + addR) & 0x1F) << 3);
        clut[96 + i] = 0xFF000000 | ((((keepB[i] + addB) & 0x1F) << 3) << 16);
    }
    for (int32 i = 0; i < 64; ++i) clut[32 + i] = 0xFF000000 | (((((keepG[i >> 1] + addG) & 0x1F) << 1) << 2) << 8);
    PSPGe3DGrowBounds(0, 0, screen->size.x, screen->size.y);
    return true;
}

// Diagnostics (boot.txt "framecheck"): every blit stamps a frame number into the LCD buffer's bottom-right pixel, and
// before a buffer is shown we check that it holds the frame we think it does, and that the image (hashed) is not one
// shown a few frames earlier - the LCD going back in time.
int pspFrameCheck = 0;
int pspMusicTest  = 0; // boot.txt "musictest"
static uint16 pspLcdStamp[PSP_LCD_BUFFERS];
static uint16 pspStampCounter;
static uint32 pspCheckHashes[6];

struct PSPStampVertex {
    uint32 color;
    int16 x, y, z, pad;
};

static void PSPStampFrame()
{
    uint16 c = pspLcdStamp[pspLcdDraw] = ++pspStampCounter;
    uint32 r = (c & 31) << 3, g = ((c >> 5) & 63) << 2, b = ((c >> 11) & 31) << 3;
    PSPStampVertex *v = (PSPStampVertex *)sceGuGetMemory(2 * sizeof(PSPStampVertex));
    v[0].color = v[1].color = 0xFF000000 | (b << 16) | (g << 8) | r;
    v[0].x = PSP_SCR_WIDTH - 2, v[0].y = PSP_SCR_HEIGHT - 1, v[0].z = 0;
    v[1].x = PSP_SCR_WIDTH, v[1].y = PSP_SCR_HEIGHT, v[1].z = 0;
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DITHER);
    sceGuDisable(GU_BLEND);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
}

static void PSPCheckFrame(int32 buffer)
{
    const uint16 *px = (const uint16 *)(((uintptr_t)sceGeEdramGetAddr() + PSP_VRAM_LCD(buffer)) | 0x40000000); // uncached
    uint16 stamp     = px[(PSP_SCR_HEIGHT - 1) * PSP_BUF_WIDTH + PSP_SCR_WIDTH - 1];
    if (stamp != pspLcdStamp[buffer]) {
        if (pspCheckBadStamp < 20)
            PrintLog(PRINT_NORMAL, "PSP framecheck: buffer %d holds stamp %u, expected %u", buffer, stamp, pspLcdStamp[buffer]);
        pspCheckBadStamp++;
    }
    uint32 hash = 2166136261u;
    for (int32 y = 0; y < PSP_SCR_HEIGHT - 8; y += 8) {
        const uint32 *row = (const uint32 *)&px[y * PSP_BUF_WIDTH];
        for (int32 x = 0; x < PSP_SCR_WIDTH / 2; ++x) hash = (hash ^ row[x]) * 16777619u;
    }
    if (hash == pspCheckHashes[0]) {
        pspCheckRepeat++;
    }
    else {
        for (int32 k = 1; k < 6; ++k) {
            if (hash == pspCheckHashes[k]) {
                if (pspCheckBack < 20)
                    PrintLog(PRINT_NORMAL, "PSP framecheck: frame %u shows the image of %d frames ago", pspCheckFrames, k + 1);
                pspCheckBack++;
                break;
            }
        }
    }
    memmove(&pspCheckHashes[1], &pspCheckHashes[0], 5 * sizeof(uint32));
    pspCheckHashes[0] = hash;
    pspCheckFrames++;
}

// Waits for the display list kicked by the previous FlipScreen and shows its frame. The GE normally
// finished long ago (it ran while the CPU updated/drew the current frame), so this rarely blocks.
static void PSPPresentPending()
{
    if (!pspGuPending)
        return;

    PSPWaitDisplayList();
    pspGuPending = false;

    unsigned long long now = PSPProfNow();
    if (pspLastFlip) {
        unsigned long long elapsed = now - pspLastFlip;
        pspLate                    = elapsed > pspFrameTicks * pspPendingUpdates;

        // what one update + draw costs: the work since the last present, minus the extra updates
        unsigned long long cost = elapsed - MIN(elapsed, pspExtraUpdateUS);
        cost                    = MIN(cost, 40000ULL); // a load or a hitch shouldn't swing the average for long
        pspFrameCostAvg += ((float)cost - pspFrameCostAvg) * 0.1f;
    }
    pspExtraUpdateUS = 0;
    if (pspFrameSkipMode == 1) {
        if (pspUpdatesPerDraw == 1 && pspFrameCostAvg > 17000.0f)
            pspUpdatesPerDraw = 2;
        else if (pspUpdatesPerDraw == 2 && pspFrameCostAvg < 14000.0f)
            pspUpdatesPerDraw = 1;
    }
    else {
        pspUpdatesPerDraw = pspFrameSkipMode;
    }

    // Pace: the frame on screen stays for as many refreshes as the game time the new one advances (one per update).
    // Late frames go out at the next vertical blank instead of waiting for another one.
    if (videoSettings.vsync) {
        unsigned int target = pspLcdShownVcount + pspPendingUpdates;
        if ((int32)(sceDisplayGetVcount() - target) < 0) {
            PSP_PROF_BEGIN(PSPPROF_VBLANK);
            while ((int32)(sceDisplayGetVcount() - target) < 0) sceDisplayWaitVblankStart();
            PSP_PROF_END(PSPPROF_VBLANK);
        }
    }

    // the previous frame must have reached the screen first (it only does at a vertical blank), or the buffer it
    // replaced could still be on screen; when on time we just waited for one anyway
    if (sceDisplayGetVcount() == pspLcdShownVcount) {
        PSP_PROF_BEGIN(PSPPROF_VBLANK);
        sceDisplayWaitVblankStart();
        PSP_PROF_END(PSPPROF_VBLANK);
    }
    int32 shown = pspLcdDraw;
    if (pspFrameCheck)
        PSPCheckFrame(shown);
    sceDisplaySetFrameBuf((uint8 *)sceGeEdramGetAddr() + PSP_VRAM_LCD(shown), PSP_BUF_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_565,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);
    uint32 vcount = sceDisplayGetVcount();
    // refreshes this frame stayed up beyond the ones its updates paid for: each is a moment below 60fps
    if (pspLcdShownVcount && vcount - pspLcdShownVcount > (uint32)pspShownUpdates) {
        pspMissedRefreshes += vcount - pspLcdShownVcount - pspShownUpdates;
        // (test runs: each late frame of a scene's first 10 seconds, to tell them from the load itself)
        static int32 lateLogged, lateScene = -1;
        if (lateScene != pspSceneStartFrame)
            lateScene = pspSceneStartFrame, lateLogged = 0;
        if (::pspLogEnabled && pspFrameIndex - pspSceneStartFrame < 600 && lateLogged++ < 40)
            PrintLog(PRINT_NORMAL, "PSP late frame %d of the scene: +%u refreshes", pspFrameIndex - pspSceneStartFrame,
                     vcount - pspLcdShownVcount - pspShownUpdates);
    }
    pspLcdShownVcount = vcount;
    pspLcdDraw        = (PSP_LCD_BUFFERS * (PSP_LCD_BUFFERS - 1) / 2) - shown - pspLcdShown; // neither of them
    pspLcdShown       = shown;
    pspShownUpdates   = pspPendingUpdates;
    if (pspShownUpdates > 1)
        pspSkipFrames++;

    now = PSPProfNow();
    if (pspLastFlip) {
        pspProf[PSPPROF_FRAME] += now - pspLastFlip;
        if (now - pspLastFlip > pspWorstFrameUS)
            pspWorstFrameUS = (uint32)(now - pspLastFlip);
    }
    pspLastFlip = now;

    if (++pspProfFrames >= 120)
        PSPProfReport();
}

void RenderDevice::CopyFrameBuffer()
{
    // The GE reads the framebuffers straight from main memory; only push out what the CPU still holds
    // in its (16KB) data cache. Flushing the whole cache is cheaper than walking the 200KB+ range.
    // the frame's last GE batch is queued by FlipScreen, after presenting the previous frame
    PSP_PROF_BEGIN(PSPPROF_COPY);
    sceKernelDcacheWritebackAll();
    PSP_PROF_END(PSPPROF_COPY);
}

static inline void PSPDrawScreen(int32 s, int32 dx, int32 dy, int32 dw, int32 dh)
{
    const ScreenInfo *scr = &screens[s];
    int32 srcW            = scr->size.x;
    int32 srcH            = scr->size.y;
    if (srcW <= 0 || srcH <= 0)
        return;

    // the engine framebuffer is 64-byte aligned with a pitch that is a multiple of 16 pixels, as the GE requires
    sceGuTexImage(0, PSP_BUF_WIDTH, 256, scr->pitch, scr->frameBuffer);

    int32 slices              = (srcW + PSP_BLIT_SLICE - 1) / PSP_BLIT_SLICE;
    struct PSPSpriteVertex *v = (struct PSPSpriteVertex *)sceGuGetMemory(2 * slices * sizeof(struct PSPSpriteVertex));
    float scaleX              = (float)dw / (float)srcW;

    for (int32 i = 0; i < slices; ++i) {
        int32 sx0 = i * PSP_BLIT_SLICE;
        int32 sx1 = sx0 + PSP_BLIT_SLICE < srcW ? sx0 + PSP_BLIT_SLICE : srcW;

        PSPSpriteVertex *a = &v[i * 2 + 0];
        PSPSpriteVertex *b = &v[i * 2 + 1];
        a->u               = (float)sx0;
        a->v               = 0.0f;
        a->x               = dx + sx0 * scaleX;
        a->y               = (float)dy;
        a->z               = 0.0f;
        b->u               = (float)sx1;
        b->v               = (float)srcH;
        b->x               = dx + sx1 * scaleX;
        b->y               = (float)(dy + dh);
        b->z               = 0.0f;
    }

    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2 * slices, 0, v);
}

void RenderDevice::FlipScreen()
{
    pspPhase = "present";
    if (pspStringGCTest && pspFrameIndex % 120 == 119) { // (testing: as if the strings' storage had filled up)
        // the entities' strings the collector is about to take for unused: registered to a slot that no longer points at them
        DataStorage *strings = &dataStorage[DATASET_STR];
        int32 orphans = 0, owned = 0;
        for (uint32 e = 0; e < strings->entryCount; ++e) {
            uint8 *owner = (uint8 *)strings->dataEntries[e];
            if (owner >= (uint8 *)objectEntityList && owner < (uint8 *)&objectEntityList[ENTITY_COUNT]) {
                ++owned;
                if (*strings->dataEntries[e] != strings->storageEntries[e])
                    ++orphans;
            }
        }
        PrintLog(PRINT_NORMAL, "PSP dbg strgc: frame %d, %d entity strings, %d orphaned; collected", pspFrameIndex, owned, orphans);
        DefragmentAndGarbageCollectStorage(DATASET_STR);
    }
    if (pspFreezeTest == 1 && pspFrameIndex == 600) { // (testing: the main thread waits on the mixer's lock forever)
        PrintLog(PRINT_NORMAL, "PSP dbg freezetest: the main thread takes the mixer's lock twice");
        AudioDevice::LockDevice();
        AudioDevice::LockDevice();
    }
    if (windowRefreshDelay > 0) {
        windowRefreshDelay--;
        if (!windowRefreshDelay)
            UpdateGameWindow();
        return;
    }

    // present last frame (its list is done by now), then queue this frame's last GE batch and its blit, and return
    // without waiting: the GE finishes the frame while the CPU updates the next one
    PSPPresentPending();
    if (psp3DPending)
        PSPFlush3DAsync();
    PSPDumpFrame();

    float dimAmount = videoSettings.dimMax * videoSettings.dimPercent;
    uint32 dim      = (uint32)(dimAmount * 255.0f);
    if (dim > 255)
        dim = 255;

    // video/image mode: a new frame is decoded straight into VRAM once the GE is done with that memory
    static bool32 videoShown = false;
    if (videoSettings.screenCount < 1) {
        if (PSPVideoFramePending()) {
            PSPWaitDisplayList();
            sceGeDrawSync(0);
            videoShown = PSPVideoDecodeFrame((uint8 *)sceGeEdramGetAddr() + PSP_VRAM_VIDEO);
            pspRotoTexLayer = NULL; // the frame (RGBA) covers screen 1 too, where the rotozoom layer's copy was
        }
    }
    else {
        videoShown = false; // the screens have drawn over it
    }

    sceGuStart(GU_DIRECT, pspGuList);
    sceGuDrawBufferList(GU_PSM_5650, (void *)PSP_VRAM_LCD(pspLcdDraw), PSP_BUF_WIDTH);
    // everything the blit relies on, whatever the batches before it left behind
    sceGuScissor(0, 0, PSP_SCR_WIDTH, PSP_SCR_HEIGHT);
    sceGuOffset(2048 - (PSP_SCR_WIDTH / 2), 2048 - (PSP_SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, PSP_SCR_WIDTH, PSP_SCR_HEIGHT);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuTexFlush(); // the frame it samples was just drawn by the GE

    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_DEPTH_TEST);

    if (videoSettings.screenCount < 1) {
        sceGuClearColor(0xFF000000);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        if (videoShown) {
            // images fade in and out through dimMax, like the PC's image shader; videos play at full brightness
            uint32 shade = sceneInfo.state == ENGINESTATE_SHOWIMAGE ? dim : 0xFF;
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(GU_PSM_8888, 0, 0, 0);
            sceGuTexImage(0, 512, 512, PSP_VIDEO_PITCH, (uint8 *)sceGeEdramGetAddr() + PSP_VRAM_VIDEO);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            sceGuTexWrap(GU_CLAMP, GU_CLAMP);
            sceGuTexFlush();
            sceGuColor(0xFF000000 | (shade << 16) | (shade << 8) | shade);

            // 1:1 copy in 64-pixel strips (texture cache friendly)
            const int32 strips = PSP_SCR_WIDTH / 64 + (PSP_SCR_WIDTH % 64 ? 1 : 0);
            struct VideoVertex {
                int16 u, v;
                int16 x, y, z;
            };
            VideoVertex *v = (VideoVertex *)sceGuGetMemory(2 * strips * sizeof(VideoVertex));
            for (int32 s = 0; s < strips; ++s) {
                int16 x0 = s * 64, x1 = MIN(x0 + 64, PSP_SCR_WIDTH);
                v[s * 2 + 0] = { x0, 0, x0, 0, 0 };
                v[s * 2 + 1] = { x1, PSP_SCR_HEIGHT, x1, PSP_SCR_HEIGHT, 0 };
            }
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2 * strips, 0, v);
        }
    }
    else {
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_5650, 0, 0, 0);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuColor(0xFF000000 | (dim << 16) | (dim << 8) | dim);

        switch (videoSettings.screenCount) {
            default:
            case 1: PSPDrawScreen(0, 0, 0, PSP_SCR_WIDTH, PSP_SCR_HEIGHT); break;

            case 2:
                PSPDrawScreen(0, 0, 0, PSP_SCR_WIDTH, PSP_SCR_HEIGHT / 2);
                PSPDrawScreen(1, 0, PSP_SCR_HEIGHT / 2, PSP_SCR_WIDTH, PSP_SCR_HEIGHT / 2);
                break;

            case 3:
            case 4:
                PSPDrawScreen(0, 0, 0, PSP_SCR_WIDTH / 2, PSP_SCR_HEIGHT / 2);
                PSPDrawScreen(1, PSP_SCR_WIDTH / 2, 0, PSP_SCR_WIDTH / 2, PSP_SCR_HEIGHT / 2);
                PSPDrawScreen(2, 0, PSP_SCR_HEIGHT / 2, PSP_SCR_WIDTH / 2, PSP_SCR_HEIGHT / 2);
                if (videoSettings.screenCount == 4)
                    PSPDrawScreen(3, PSP_SCR_WIDTH / 2, PSP_SCR_HEIGHT / 2, PSP_SCR_WIDTH / 2, PSP_SCR_HEIGHT / 2);
                break;
        }
    }
    if (pspFrameCheck)
        PSPStampFrame();
    sceGuFinish();
    pspGuPending      = true;
    if (pspGeSyncTest) {
        unsigned long long blitStart = PSPProfNow();
        sceGuSync(0, 0);
        pspBlitTestUS += PSPProfNow() - blitStart;
    }
    pspPendingUpdates = pspFrameUpdates;
    pspFrameUpdates   = 1;
    pspGuReading = true;
}

void RenderDevice::Release(bool32 isRefresh)
{
    if (!isRefresh) {
        if (scanlines)
            free(scanlines);
        scanlines = NULL;

        if (pspGuInitialized) {
            PSPWaitDisplayList();
            pspGuPending = false;
            sceGuTerm();
            pspGuInitialized = false;
        }
    }
}

void RenderDevice::RefreshWindow()
{
    // The PSP LCD cannot be reconfigured; just reset the screen state the way a window change would.
    videoSettings.windowState = WINDOWSTATE_ACTIVE;
    engine.inFocus            = 1;
}

void RenderDevice::GetWindowSize(int32 *width, int32 *height)
{
    if (width)
        *width = PSP_SCR_WIDTH;
    if (height)
        *height = PSP_SCR_HEIGHT;
}

void RenderDevice::InitFPSCap()
{
    targetTicks   = 1000000 / (videoSettings.refreshRate ? videoSettings.refreshRate : 60);
    pspFrameTicks = targetTicks;
    curTicks      = 0;
    prevTicks   = 0;

    u64 now;
    sceRtcGetCurrentTick(&now);
    prevTicks = now;
}

bool RenderDevice::CheckFPSCap()
{
    u64 now;
    sceRtcGetCurrentTick(&now);
    curTicks = now; // the PSP RTC tick resolution is 1us

    return curTicks >= prevTicks + targetTicks;
}

void RenderDevice::UpdateFPSCap()
{
    // carry the remainder so we hold a true 60Hz, but never try to catch up more than a frame
    prevTicks += targetTicks;
    if (curTicks > prevTicks + targetTicks)
        prevTicks = curTicks;
}

void RenderDevice::InitVertexBuffer() {}

bool RenderDevice::InitGraphicsAPI()
{
    videoSettings.shaderSupport = false;

    viewSize.x = PSP_SCR_WIDTH;
    viewSize.y = PSP_SCR_HEIGHT;

    int32 screenWidth = videoSettings.pixWidth;
    if (screenWidth <= 0 || screenWidth > SCREEN_XMAX)
        screenWidth = DEFAULT_PIXWIDTH;

    pspGe3DArenas[0] = (uint8 *)sceGeEdramGetAddr() + PSP_VRAM_ARENA;
    pspGe3DArenas[1] = (uint8 *)sceGeEdramGetAddr() + PSP_VRAM_ARENA2;
    PSPGe3DUseFirstArena();
    for (int32 s = 0; s < SCREEN_COUNT; ++s) {
        screens[s].size.y = videoSettings.pixHeight;

        screens[s].frameBuffer = (uint16 *)((uint8 *)sceGeEdramGetAddr() + PSP_VRAM_SCREEN(s));
        memset(screens[s].frameBuffer, 0, PSP_VRAM_SCREEN_BYTES);
        SetScreenSize(s, screenWidth, screens[s].size.y);
    }

    pixelSize.x = screens[0].size.x;
    pixelSize.y = screens[0].size.y;

    textureSize.x = 512.0;
    textureSize.y = 256.0;

    lastShaderID = -1;
    engine.inFocus          = 1;
    videoSettings.viewportX = 0;
    videoSettings.viewportY = 0;
    videoSettings.viewportW = 1.0 / viewSize.x;
    videoSettings.viewportH = 1.0 / viewSize.y;

    return true;
}

void RenderDevice::LoadShader(const char *fileName, bool32 linear) { PrintLog(PRINT_NORMAL, "This render device does not support shaders!"); }

bool RenderDevice::InitShaders()
{
    for (int32 s = 0; s < SHADER_COUNT; ++s) shaderList[s].linear = true;

    shaderList[0].linear = false;
    videoSettings.shaderID = 0;
    shaderCount            = 1;
    return true;
}

#if PSP_VRAM_BENCH
// Diagnostics: how fast the CPU draws into VRAM compared with RAM, with the access patterns of the software renderer
// (plain 16-bit stores like tiles/sprites, and read-modify-write like blended sprites).
static void PSPBenchFill(const char *label, uint16 *buffer, uint32 pixels, bool flushCache)
{
    unsigned long long t0 = PSPProfNow();
    for (int32 r = 0; r < 4; ++r)
        for (uint32 i = 0; i < pixels; ++i) buffer[i] = (uint16)(i + r);
    if (flushCache)
        sceKernelDcacheWritebackAll();
    unsigned long long t1 = PSPProfNow();
    for (int32 r = 0; r < 4; ++r)
        for (uint32 i = 0; i < pixels; ++i) buffer[i] = ((buffer[i] & 0xF7DE) >> 1) + 0x1234;
    if (flushCache)
        sceKernelDcacheWritebackAll();
    unsigned long long t2 = PSPProfNow();
    PrintLog(PRINT_NORMAL, "PSP vram bench %s: store=%uus/frame rmw=%uus/frame", label, (uint32)((t1 - t0) / 4), (uint32)((t2 - t1) / 4));
}

static void PSPBenchVram()
{
    const uint32 pixels = 448 * 240;
    uint16 *ram         = (uint16 *)malloc(pixels * 2);
    if (ram) {
        PSPBenchFill("ram", ram, pixels, true);
        free(ram);
    }
    uint8 *vram = (uint8 *)sceGeEdramGetAddr() + 0x100000;
    PSPBenchFill("vram cached", (uint16 *)vram, pixels, true);
    PSPBenchFill("vram uncached", (uint16 *)((uintptr_t)vram | 0x40000000), pixels, false);
}
#endif

bool RenderDevice::SetupRendering()
{
#if PSP_VRAM_BENCH
    PSPBenchVram();
#endif
    if (!pspGuInitialized) {
        memset(sceGeEdramGetAddr(), 0, PSP_LCD_BUFFERS * PSP_FB_SIZE_BYTES);
        sceKernelDcacheWritebackInvalidateAll();

        sceGuInit();
        sceGuStart(GU_DIRECT, pspGuList);
        sceGuDrawBuffer(GU_PSM_5650, (void *)PSP_VRAM_LCD(pspLcdDraw), PSP_BUF_WIDTH);
        sceGuDispBuffer(PSP_SCR_WIDTH, PSP_SCR_HEIGHT, (void *)PSP_VRAM_LCD(pspLcdShown), PSP_BUF_WIDTH);
        sceGuOffset(2048 - (PSP_SCR_WIDTH / 2), 2048 - (PSP_SCR_HEIGHT / 2));
        sceGuViewport(2048, 2048, PSP_SCR_WIDTH, PSP_SCR_HEIGHT);
        sceGuScissor(0, 0, PSP_SCR_WIDTH, PSP_SCR_HEIGHT);
        sceGuEnable(GU_SCISSOR_TEST);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDisable(GU_CULL_FACE);
        sceGuDisable(GU_BLEND);
        sceGuClearColor(0xFF000000);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuDisplay(GU_TRUE);
        pspLcdShownVcount = sceDisplayGetVcount();
        pspGuInitialized  = true;
    }

    displayInfo.displays = &pspDisplay;
    GetDisplays();

    if (!InitGraphicsAPI() || !InitShaders())
        return false;

    int32 size = videoSettings.pixWidth >= SCREEN_YSIZE ? videoSettings.pixWidth : SCREEN_YSIZE;
    scanlines  = (ScanlineInfo *)malloc(size * sizeof(ScanlineInfo));
    memset(scanlines, 0, size * sizeof(ScanlineInfo));

    videoSettings.windowState = WINDOWSTATE_ACTIVE;
    videoSettings.dimMax      = 1.0;
    videoSettings.dimPercent  = 1.0;

    return true;
}

void RenderDevice::GetDisplays()
{
    displayInfo.displays = &pspDisplay;
    displayCount         = 1;
    displayWidth[0]      = PSP_SCR_WIDTH;
    displayHeight[0]     = PSP_SCR_HEIGHT;
}

bool RenderDevice::ProcessEvents()
{
    if (pspExitRequested) {
        PrintLog(PRINT_NORMAL, "PSP exit callback received; stopping engine loop");
        isRunning = false;
    }

    return true;
}

void RenderDevice::SetupImageTexture(int32 width, int32 height, uint8 *imagePixels) {}

void RenderDevice::SetupVideoTexture_YUV420(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                            int32 strideV)
{
}
void RenderDevice::SetupVideoTexture_YUV422(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                            int32 strideV)
{
}
void RenderDevice::SetupVideoTexture_YUV444(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                            int32 strideV)
{
}
