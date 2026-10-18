#include "RSDK/Core/RetroEngine.hpp"
#if RETRO_PLATFORM == RETRO_PSP
// PSP video stub
using namespace RSDK;
FileInfo VideoManager::file;
ogg_sync_state VideoManager::oy;
ogg_page VideoManager::og;
ogg_packet VideoManager::op;
ogg_stream_state VideoManager::vo;
ogg_stream_state VideoManager::to;
th_info VideoManager::ti;
th_comment VideoManager::tc;
th_dec_ctx *VideoManager::td = NULL;
th_setup_info *VideoManager::ts = NULL;
th_pixel_fmt VideoManager::pixelFormat;
ogg_int64_t VideoManager::granulePos = 0;
bool32 VideoManager::initializing = false;

// ---------------------------------------------------------------------------------------------------------------
// Videos as JPEG frames (tools/build_psp_data.py turns each Theora .ogv in Data.psp into one, and the 1024x512 PNGs
// LoadImage shows into a single frame). The PSP decodes them in hardware (sceJpeg, in the avcodec module the music
// already loads) straight into VRAM, and FlipScreen shows the frame. The soundtrack is the music stream the game plays
// alongside (ATRAC3), and its position picks the frame, as the PC's player does with Vorbis.
//   0 'PSVD', 4 u16 version (1), 6 u16 width, 8 u16 height (480x272: what is shown), 10 u16 pitch (512: JPEG width),
//   12 u32 fps numerator, 16 u32 fps denominator, 20 u32 frame count, 24 u32 largest frame,
//   28 per frame: u32 offset (from the file's start), u32 size; then the JPEGs.
// ---------------------------------------------------------------------------------------------------------------
#include <pspjpeg.h>
#include <pspkernel.h>
#include <malloc.h>

#define PSP_VIDEO_SIGNATURE (0x44565350) // "PSVD"
#define PSP_VIDEO_PITCH     (512)
#define PSP_VIDEO_HEIGHT    (272)

static bool32 pspVideoOpen  = false;
static bool32 pspJpegReady  = false;
static uint32 *pspVideoTable = NULL;
static uint8 *pspVideoJpeg   = NULL;
static int32 pspVideoFrames, pspVideoMaxSize, pspVideoLoaded = -1;
static uint32 pspVideoFpsNum, pspVideoFpsDen;
static bool32 pspVideoPending = false; // a frame read from the file, waiting for FlipScreen to decode it
static int32 pspVideoPendingSize;
static bool32 pspVideoOneShot = false; // an image: let the decoder go once its one frame is shown
static int32 pspVideoTicks, pspVideoSkipped, pspVideoDecodeUS; // for the sync/speed log line
static unsigned long long pspVideoStartUS;

static bool32 PSPJpegStart()
{
    if (pspJpegReady)
        return true;
    int32 init = sceJpegInitMJpeg();
    if (init < 0) {
        PrintLog(PRINT_NORMAL, "PSP video: sceJpegInitMJpeg failed (%08X)", init);
        return false;
    }
    int32 create = sceJpegCreateMJpeg(PSP_VIDEO_PITCH, PSP_VIDEO_HEIGHT);
    if (create < 0) {
        PrintLog(PRINT_NORMAL, "PSP video: sceJpegCreateMJpeg failed (%08X)", create);
        sceJpegFinishMJpeg();
        return false;
    }
    pspJpegReady = true;
    return true;
}

static void PSPJpegStop()
{
    if (pspJpegReady) {
        sceJpegDeleteMJpeg();
        sceJpegFinishMJpeg();
        pspJpegReady = false;
    }
}

static void PSPVideoClose()
{
    if (pspVideoOpen)
        CloseFile(&VideoManager::file);
    free(pspVideoTable);
    free(pspVideoJpeg);
    pspVideoTable   = NULL;
    pspVideoJpeg    = NULL;
    pspVideoOpen    = false;
    pspVideoPending = false;
    PSPJpegStop();
}

static bool32 PSPVideoOpenFile(const char *path)
{
    PSPVideoClose();
    InitFileInfo(&VideoManager::file);
    if (!LoadFile(&VideoManager::file, path, FMODE_RB))
        return false;
    if (ReadInt32(&VideoManager::file, false) != PSP_VIDEO_SIGNATURE) {
        CloseFile(&VideoManager::file);
        return false;
    }
    pspVideoOpen = true;

    Seek_Set(&VideoManager::file, 12);
    pspVideoFpsNum  = ReadInt32(&VideoManager::file, false);
    pspVideoFpsDen  = ReadInt32(&VideoManager::file, false);
    pspVideoFrames  = ReadInt32(&VideoManager::file, false);
    pspVideoMaxSize = ReadInt32(&VideoManager::file, false);

    pspVideoTable = (uint32 *)malloc(pspVideoFrames * 2 * sizeof(uint32));
    pspVideoJpeg  = (uint8 *)memalign(64, (pspVideoMaxSize + 63) & ~63);
    if (!pspVideoFrames || !pspVideoFpsNum || !pspVideoFpsDen || !pspVideoTable || !pspVideoJpeg) {
        PrintLog(PRINT_NORMAL, "PSP video: can't play %s (%d frames, %d bytes the largest)", path, pspVideoFrames, pspVideoMaxSize);
        PSPVideoClose();
        return false;
    }
    ReadBytes(&VideoManager::file, pspVideoTable, pspVideoFrames * 2 * sizeof(uint32));

    if (!PSPJpegStart()) {
        PSPVideoClose();
        return false;
    }
    pspVideoLoaded  = -1;
    pspVideoTicks   = 0;
    pspVideoSkipped = 0;
    pspVideoStartUS = PSPProfNow();
    return true;
}

// Reads a frame's JPEG for FlipScreen to decode.
static void PSPVideoLoadFrame(int32 frame)
{
    uint32 offset = pspVideoTable[frame * 2], size = pspVideoTable[frame * 2 + 1];
    if (size > (uint32)pspVideoMaxSize)
        return;
    Seek_Set(&VideoManager::file, offset);
    ReadBytes(&VideoManager::file, pspVideoJpeg, size);
    sceKernelDcacheWritebackRange(pspVideoJpeg, (size + 63) & ~63); // the decoder reads memory, not the CPU's cache
    pspVideoPendingSize = size;
    pspVideoPending     = true;
    pspVideoLoaded      = frame;
}

bool32 RSDK::PSPVideoFramePending() { return pspVideoPending; }

bool32 RSDK::PSPVideoDecodeFrame(uint8 *target)
{
    pspVideoPending = false;
    unsigned long long start = PSPProfNow();
    int32 result    = pspJpegReady ? sceJpegDecodeMJpeg(pspVideoJpeg, pspVideoPendingSize, target, 0) : -1;
    // in case the library finishes the pixels with the CPU: get anything left in the data cache out to VRAM before the
    // GE reads it (the cache is only 16KB, so this is cheap)
    sceKernelDcacheWritebackAll();
    pspVideoDecodeUS = (int32)(PSPProfNow() - start);
    if (result < 0)
        PrintLog(PRINT_NORMAL, "PSP video: frame %d not decoded (%08X)", pspVideoLoaded, result);
    if (pspVideoOneShot) {
        pspVideoOneShot = false;
        PSPVideoClose();
    }
    return result >= 0;
}

bool32 RSDK::PSPLoadImageFrame(const char *path)
{
    if (!PSPVideoOpenFile(path))
        return false;
    PSPVideoLoadFrame(0);
    pspVideoOneShot = true;
    return true;
}

bool32 RSDK::LoadVideo(const char *filename, double startDelay, bool32 (*skipCallback)())
{
    if (sceneInfo.state == ENGINESTATE_VIDEOPLAYBACK)
        return false;

    char fullFilePath[0x80];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/Video/%s", filename);
    if (!PSPVideoOpenFile(fullFilePath)) {
        PrintLog(PRINT_NORMAL, "PSP video: %s not found or not converted (rebuild Data.psp)", fullFilePath);
        return false;
    }
    PrintLog(PRINT_NORMAL, "PSP video: %s, %d frames at %u/%u fps", filename, pspVideoFrames, pspVideoFpsNum, pspVideoFpsDen);

    engine.storedShaderID     = videoSettings.shaderID;
    videoSettings.screenCount = 0;
    engine.storedState        = sceneInfo.state;
    engine.displayTime        = 0.0;
    engine.videoStartDelay    = AudioDevice::audioState == 1 ? startDelay : 0.0;
    engine.skipCallback       = skipCallback;
    VideoManager::initializing = true; // the first frame goes up at once, whatever the time
    sceneInfo.state            = ENGINESTATE_VIDEOPLAYBACK;
    return true;
}

void RSDK::ProcessVideo()
{
    if (!pspVideoOpen)
        return;

    bool32 finished = false;
    int32 frame     = 0;
    if (!VideoManager::initializing) {
        double streamPos = GetVideoStreamPos();
        if (streamPos <= -1.0)
            engine.displayTime += (1.0 / 60.0); // no stream: frame-step
        else
            engine.displayTime = streamPos;

        if (engine.skipCallback && engine.skipCallback())
            finished = true;

        double time = engine.displayTime - engine.videoStartDelay;
        frame       = time > 0.0 ? (int32)(time * pspVideoFpsNum / pspVideoFpsDen) : 0;
        if (frame >= pspVideoFrames)
            finished = true;
    }
    VideoManager::initializing = false;

    if (!finished && frame != pspVideoLoaded) {
        if (frame > pspVideoLoaded + 1 && pspVideoLoaded >= 0)
            pspVideoSkipped += frame - pspVideoLoaded - 1;
        PSPVideoLoadFrame(frame); // late frames are skipped, never queued up
    }

    // sync check, every ~5s: the frame on screen against the music's position
    if (++pspVideoTicks % 300 == 0 || finished)
        PrintLog(PRINT_NORMAL, "PSP video: real %.2fs, music %.2fs (%s) -> frame %d (%.2fs, delay %.1fs), %d skipped, decode %dus",
                 (PSPProfNow() - pspVideoStartUS) / 1000000.0, engine.displayTime,
                 GetVideoStreamPos() <= -1.0 ? "no stream, frame-stepped" : "ATRAC3", pspVideoLoaded,
                 pspVideoLoaded * (double)pspVideoFpsDen / pspVideoFpsNum + engine.videoStartDelay, engine.videoStartDelay, pspVideoSkipped,
                 pspVideoDecodeUS);

    if (finished) {
        PSPVideoClose();
        videoSettings.shaderID    = engine.storedShaderID;
        videoSettings.screenCount = 1;
        sceneInfo.state           = engine.storedState;
    }
}
#else


using namespace RSDK;

FileInfo VideoManager::file;

ogg_sync_state VideoManager::oy;
ogg_page VideoManager::og;
ogg_packet VideoManager::op;
ogg_stream_state VideoManager::vo;
ogg_stream_state VideoManager::to;
th_info VideoManager::ti;
th_comment VideoManager::tc;
th_dec_ctx *VideoManager::td    = NULL;
th_setup_info *VideoManager::ts = NULL;

th_pixel_fmt VideoManager::pixelFormat;
ogg_int64_t VideoManager::granulePos = 0;
bool32 VideoManager::initializing    = false;

bool32 RSDK::LoadVideo(const char *filename, double startDelay, bool32 (*skipCallback)())
{
    if (ENGINE_VERSION == 5 && sceneInfo.state == ENGINESTATE_VIDEOPLAYBACK)
        return false;
#if RETRO_REV0U
    if (ENGINE_VERSION == 3 && RSDK::Legacy::gameMode == RSDK::Legacy::v3::ENGINE_VIDEOWAIT)
        return false;
#endif

    char fullFilePath[0x80];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/Video/%s", filename);

    InitFileInfo(&VideoManager::file);
    if (LoadFile(&VideoManager::file, fullFilePath, FMODE_RB)) {
        // Init
        ogg_sync_init(&VideoManager::oy);

        th_comment_init(&VideoManager::tc);
        th_info_init(&VideoManager::ti);

        int32 theora_p = 0;
        char *buffer   = NULL;

        // Parse header stuff
        bool32 finishedHeader = false;
        while (!finishedHeader) {
            buffer    = ogg_sync_buffer(&VideoManager::oy, 0x1000);
            int32 ret = (int32)ReadBytes(&VideoManager::file, buffer, 0x1000);
            ogg_sync_wrote(&VideoManager::oy, 0x1000);

            if (ret == 0)
                break;

            while (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) {
                ogg_stream_state test;

                /* is this a mandated initial header? If not, stop parsing */
                if (!ogg_page_bos(&VideoManager::og)) {
                    /* don't leak the page; get it into the appropriate stream */
                    ogg_stream_pagein(&VideoManager::to, &VideoManager::og);
                    finishedHeader = true;
                    break;
                }

                ogg_stream_init(&test, ogg_page_serialno(&VideoManager::og));
                ogg_stream_pagein(&test, &VideoManager::og);
                ogg_stream_packetout(&test, &VideoManager::op);

                // identify codec
                if (!theora_p && th_decode_headerin(&VideoManager::ti, &VideoManager::tc, &VideoManager::ts, &VideoManager::op) >= 0) {
                    // theora
                    memcpy(&VideoManager::to, &test, sizeof(test));
                    theora_p = 1;
                }
                else {
                    // we dont care (possibly vorbis)
                    ogg_stream_clear(&test);
                }
            }
        }

        if (theora_p) {
            VideoManager::ts = NULL;
            theora_p         = 1;
            while (theora_p && theora_p < 3) {
                int32 ret;

                while (theora_p && (theora_p < 3) && (ret = ogg_stream_packetout(&VideoManager::to, &VideoManager::op))) {
                    if (ret < 0) {
#if !RETRO_USE_ORIGINAL_CODE
                        PrintLog(PRINT_NORMAL, "ERROR: failed to parse theora stream headers. corrupted stream?");
#endif

                        theora_p = 0;
                        break;
                    }

                    if (!th_decode_headerin(&VideoManager::ti, &VideoManager::tc, &VideoManager::ts, &VideoManager::op)) {
#if !RETRO_USE_ORIGINAL_CODE
                        PrintLog(PRINT_NORMAL, "ERROR: failed to parse theora stream headers. corrupted stream?");
#endif

                        theora_p = 0;
                        break;
                    }

                    theora_p++;
                }

                if (!theora_p)
                    break;

                /* The header pages/packets will arrive before anything else we
                   care about, or the stream is not obeying spec */

                if (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) {
                    ogg_stream_pagein(&VideoManager::to, &VideoManager::og);
                }
                else {
                    buffer    = ogg_sync_buffer(&VideoManager::oy, 0x1000);
                    int32 ret = (int32)ReadBytes(&VideoManager::file, buffer, 0x1000);
                    ogg_sync_wrote(&VideoManager::oy, 0x1000);
                    if (ret == 0) {
#if !RETRO_USE_ORIGINAL_CODE
                        PrintLog(PRINT_NORMAL, "ERROR: Reached end of file while searching for codec headers.");
#endif

                        theora_p = 0;
                    }
                }
            }

            if (!theora_p) {
                th_info_clear(&VideoManager::ti);
                th_comment_clear(&VideoManager::tc);
                th_setup_free(VideoManager::ts);
            }
            else {
                VideoManager::td          = th_decode_alloc(&VideoManager::ti, VideoManager::ts);
                VideoManager::pixelFormat = VideoManager::ti.pixel_fmt;

                int32 ppLevelMax = 0;
                th_decode_ctl(VideoManager::td, TH_DECCTL_GET_PPLEVEL_MAX, &ppLevelMax, sizeof(int32));
                int32 ppLevel = 0;
                th_decode_ctl(VideoManager::td, TH_DECCTL_SET_PPLEVEL, &ppLevel, sizeof(int32));

                th_setup_free(VideoManager::ts);

                engine.storedShaderID     = videoSettings.shaderID;
                videoSettings.screenCount = 0;

                if (ENGINE_VERSION == 5)
                    engine.storedState = sceneInfo.state;
#if RETRO_REV0U
                else if (ENGINE_VERSION == 3)
                    engine.storedState = RSDK::Legacy::gameMode;
#endif
                engine.displayTime         = 0.0;
                VideoManager::initializing = true;
                VideoManager::granulePos   = 0;

                engine.displayTime     = 0.0;
                engine.videoStartDelay = 0.0;
                if (AudioDevice::audioState == 1)
                    engine.videoStartDelay = startDelay;

                switch (VideoManager::pixelFormat) {
                    default: break;
                    case TH_PF_420: videoSettings.shaderID = SHADER_YUV_420; break;
                    case TH_PF_422: videoSettings.shaderID = SHADER_YUV_422; break;
                    case TH_PF_444: videoSettings.shaderID = SHADER_YUV_444; break;
                }

                engine.skipCallback = NULL;
                ProcessVideo();
                engine.skipCallback = skipCallback;

                changedVideoSettings = false;
                if (ENGINE_VERSION == 5)
                    sceneInfo.state = ENGINESTATE_VIDEOPLAYBACK;
#if RETRO_REV0U
                else if (ENGINE_VERSION == 3)
                    RSDK::Legacy::gameMode = RSDK::Legacy::v3::ENGINE_VIDEOWAIT;
#endif

                return true;
            }
        }

        CloseFile(&VideoManager::file);
    }

    return false;
}

void RSDK::ProcessVideo()
{
    bool32 finished = false;
    double curTime  = 0;
    if (!VideoManager::initializing) {
        double streamPos = GetVideoStreamPos();

        if (streamPos <= -1.0)
            engine.displayTime += (1.0 / 60.0); // deltaTime frame-step
        else
            engine.displayTime = streamPos;

        curTime = th_granule_time(VideoManager::td, VideoManager::granulePos);

#if RETRO_USE_MOD_LOADER
        RunModCallbacks(MODCB_ONVIDEOSKIPCB, (void *)engine.skipCallback);
#endif
        if (engine.skipCallback && engine.skipCallback()) {
            finished = true;
        }
    }

    if (!finished && (VideoManager::initializing || engine.displayTime >= engine.videoStartDelay + curTime)) {
        while (ogg_stream_packetout(&VideoManager::to, &VideoManager::op) <= 0) {
            char *buffer = ogg_sync_buffer(&VideoManager::oy, 0x1000);
            // if we're playing and reached the end of file
            if (!ReadBytes(&VideoManager::file, buffer, 0x1000) && !VideoManager::initializing) {
                finished = true;
                break;
            }

            ogg_sync_wrote(&VideoManager::oy, 0x1000);

            while (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) ogg_stream_pagein(&VideoManager::to, &VideoManager::og);
        }

        if (!finished && !th_decode_packetin(VideoManager::td, &VideoManager::op, &VideoManager::granulePos)) {
            th_ycbcr_buffer yuv;
            th_decode_ycbcr_out(VideoManager::td, yuv);

            int32 dataPos = (VideoManager::ti.pic_x & 0xFFFFFFFE) + (VideoManager::ti.pic_y & 0xFFFFFFFE) * yuv[0].stride;
            switch (VideoManager::pixelFormat) {
                default: break;

                case TH_PF_444:
                    RenderDevice::SetupVideoTexture_YUV444(yuv[0].width, yuv[0].height, &yuv[0].data[dataPos], &yuv[1].data[dataPos],
                                                           &yuv[2].data[dataPos], yuv[0].stride, yuv[1].stride, yuv[2].stride);
                    break;

                case TH_PF_422:
                    RenderDevice::SetupVideoTexture_YUV422(yuv[0].width, yuv[0].height, &yuv[0].data[dataPos],
                                                           &yuv[1].data[yuv[1].stride * VideoManager::ti.pic_y + (VideoManager::ti.pic_x >> 1)],
                                                           &yuv[2].data[yuv[1].stride * VideoManager::ti.pic_y + (VideoManager::ti.pic_x >> 1)],
                                                           yuv[0].stride, yuv[1].stride, yuv[2].stride);
                    break;

                case TH_PF_420:
                    RenderDevice::SetupVideoTexture_YUV420(
                        yuv[0].width, yuv[0].height, &yuv[0].data[dataPos],
                        &yuv[1].data[yuv[1].stride * (VideoManager::ti.pic_y >> 1) + (VideoManager::ti.pic_x >> 1)],
                        &yuv[2].data[yuv[1].stride * (VideoManager::ti.pic_y >> 1) + (VideoManager::ti.pic_x >> 1)], yuv[0].stride, yuv[1].stride,
                        yuv[2].stride);
                    break;
            }
        }

        VideoManager::initializing = false;
    }

    if (finished) {
        CloseFile(&VideoManager::file);

        // Flush everything out
        while (ogg_sync_pageout(&VideoManager::oy, &VideoManager::og) > 0) ogg_stream_pagein(&VideoManager::to, &VideoManager::og);

        ogg_stream_clear(&VideoManager::to);
        th_decode_free(VideoManager::td);
        th_comment_clear(&VideoManager::tc);
        th_info_clear(&VideoManager::ti);
        ogg_sync_clear(&VideoManager::oy);

        videoSettings.shaderID    = engine.storedShaderID;
        videoSettings.screenCount = 1;
        if (ENGINE_VERSION == 5)
            sceneInfo.state = engine.storedState;
#if RETRO_REV0U
        else if (ENGINE_VERSION == 3)
            RSDK::Legacy::gameMode = engine.storedState;
#endif
    }
}

#endif
