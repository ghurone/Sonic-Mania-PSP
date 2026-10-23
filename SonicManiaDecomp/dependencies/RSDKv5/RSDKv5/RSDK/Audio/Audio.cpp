#include "RSDK/Core/RetroEngine.hpp"

using namespace RSDK;

#if RETRO_REV0U
#include "Legacy/AudioLegacy.cpp"
#endif

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "stb_vorbis/stb_vorbis.c"

stb_vorbis *vorbisInfo = NULL;
stb_vorbis_alloc vorbisAlloc;

SFXInfo RSDK::sfxList[SFX_COUNT];
ChannelInfo RSDK::channels[CHANNEL_COUNT];

char streamFilePath[0x40];
uint8 *streamBuffer    = NULL;
int32 streamBufferSize = 0;
uint32 streamStartPos  = 0;
int32 streamLoopPoint  = 0;

// SFX sample storage. PSP keeps the raw S16 data (half the memory of F32) and scales while mixing.
#if RETRO_PLATFORM == RETRO_PSP
typedef int16 SFX_SAMPLE;
#define SFX_SAMPLE_SCALE (0.75f / (float)0x8000)
#else
typedef float SFX_SAMPLE;
#define SFX_SAMPLE_SCALE (1.0f)
#endif

#define LINEAR_INTERPOLATION_LOOKUP_DIVISOR 0x40 // Determines the 'resolution' of the lookup table.
#define LINEAR_INTERPOLATION_LOOKUP_LENGTH  (TO_FIXED(1) / LINEAR_INTERPOLATION_LOOKUP_DIVISOR)

float linearInterpolationLookup[LINEAR_INTERPOLATION_LOOKUP_LENGTH];

#if RETRO_PLATFORM == RETRO_PSP
// ---------------------------------------------------------------------------------------------------------------
// Music as ATRAC3 (tools/convert_music.py): Music/<pack hash of the .ogg path>.at3 next to Data.rsdk. The PSP decodes
// ATRAC3 on its Media Engine (sceAtrac), so the main CPU no longer spends ~1-2ms a frame on Vorbis, and the track
// streams from the Memory Stick through a small buffer instead of being loaded whole into RAM at every stage load.
// Without the file the OGG plays as before.
// ---------------------------------------------------------------------------------------------------------------
#include <pspatrac3.h>
#include <psputility.h>
#include <pspiofilemgr.h>
#include <unistd.h>

#define PSP_AT3_BUFFER (64 * 1024) // ~4s at 132kbps
#define PSP_AT3_READ   (16 * 1024) // the most the audio thread reads at once

static uint8 pspAt3Buffer[PSP_AT3_BUFFER] __attribute__((aligned(64)));
static int16 pspAt3Pcm[2048] __attribute__((aligned(64))); // one decoded frame: 1024 stereo samples
// Decoded music waiting for the mixer: the audio thread decodes ahead into this ring before each mix, outside the mixer's
// lock (PSPAt3Decode), and the mixer only copies from it (PSPAt3Fill). A Media Engine that hangs in sceAtracDecodeData
// then holds up nothing but the music, where it used to hold the mixer's lock and with it the game (SPZ1, issue #5).
#define PSP_AT3_RING (8192) // int16s: 4 frames, ~2 mixes at the fastest music speed
static int16 pspAt3Ring[PSP_AT3_RING] __attribute__((aligned(64)));
static volatile uint32 pspAt3RingRead, pspAt3RingWrite; // int16s taken/added so far (indices modulo PSP_AT3_RING)
static volatile bool pspAt3Loop; // play on from streamLoopPoint when the track ends (the stream channel's loop)
static volatile bool pspAt3Done; // ended for good: once the ring is empty the channel stops
static int32 pspAt3ID     = -1;
static SceUID pspAt3File  = -1;
static int32 pspAt3Base   = 0; // where the track starts in that file (Data.psp entry, or 0 for Music/*.at3)
static SceUID pspAt3Sema  = -1;
static bool pspAt3Modules = false;
static bool pspAt3Streaming; // the file is larger than the buffer: the audio thread keeps it topped up
static bool pspAt3Ended;     // the decoder returned the last frame (looping or not)
static bool pspAt3Active;    // the stream channel plays ATRAC3 (else Vorbis)
static uint32 pspAt3NextSample; // the decoder's position, what GetChannelPos reports (like Vorbis's current_loc)
// (diagnostics: the sceAtrac call in progress, 0 = none, for PSPLink to read when the Media Engine hangs)
volatile int pspAt3Step;
#define PSP_AT3_CALL(step, call) (pspAt3Step = (step), pspAt3CallEnd(call))
static inline int pspAt3CallEnd(int result)
{
    pspAt3Step = 0;
    return result;
}

// The stream's file, opened again after the PSP slept (sleep mode closes it) or when a read fails
extern volatile int pspResumeCount; // PSPRenderDevice.cpp
static char pspAt3Path[0x100];
static int32 pspAt3FileResume;
static int32 pspAt3DecoderResume; // the resume the decoder was set up after (sleep powers the Media Engine down)
static int32 pspAt3DecodeFails;   // decodes failed in a row: the decoder is set up again past a second's worth
static volatile bool pspAt3Reset;
extern volatile int pspPowerFlags;

static void PSPAt3Reopen()
{
    if (pspAt3File >= 0)
        sceIoClose(pspAt3File);
    pspAt3FileResume = pspResumeCount;
    pspAt3File       = sceIoOpen(pspAt3Path, PSP_O_RDONLY, 0);
}

// The game's folder, for opening the music by its whole path: after sleep the audio thread, which reopens it, has no
// current directory any more (sceIoOpen of "Data.psp" then fails with 0x8002032C)
static char pspAt3Dir[0x100];

static void PSPAt3FullPath(char *path, int32 size, const char *name)
{
    if (strchr(name, ':') || !pspAt3Dir[0])
        sprintf_s(path, size, "%s", name);
    else
        sprintf_s(path, size, "%s/%s", pspAt3Dir, name);
}

static void PSPAt3Init()
{
    if (!getcwd(pspAt3Dir, sizeof(pspAt3Dir)))
        pspAt3Dir[0] = 0;
    int avcodec = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    int atrac   = sceUtilityLoadModule(PSP_MODULE_AV_ATRAC3PLUS);
    // 0x80020139: already loaded
    pspAt3Modules = (avcodec >= 0 || avcodec == (int)0x80020139) && (atrac >= 0 || atrac == (int)0x80020139);
    if (pspAt3Sema < 0)
        pspAt3Sema = sceKernelCreateSema("rsdk_at3", 0, 1, 1, NULL);
    PrintLog(PRINT_NORMAL, "PSP dbg at3: modules %08X %08X, folder %s", avcodec, atrac, pspAt3Dir);
}

static void PSPAt3Close()
{
    if (pspAt3ID >= 0)
        PSP_AT3_CALL(1, sceAtracReleaseAtracID(pspAt3ID));
    if (pspAt3File >= 0)
        sceIoClose(pspAt3File);
    pspAt3ID     = -1;
    pspAt3File   = -1;
    pspAt3Active = false;
}

// Plays on from sample (reading in whatever the decoder needs from there when streaming).
static bool PSPAt3Seek(uint32 sample)
{
    pspAt3Ended                   = false;
    pspAt3NextSample              = sample;
    if (!pspAt3Streaming)
        return PSP_AT3_CALL(4, sceAtracResetPlayPosition(pspAt3ID, sample, 0, 0)) >= 0;

    if (pspAt3FileResume != pspResumeCount)
        PSPAt3Reopen();
    PspBufferInfo info;
    if (PSP_AT3_CALL(3, sceAtracGetBufferInfoForReseting(pspAt3ID, sample, &info)) < 0)
        return false;
    int32 first = 0, second = 0;
    if (info.uiWritableByteFirstBuf) {
        sceIoLseek32(pspAt3File, pspAt3Base + info.uiReadPositionFirstBuf, PSP_SEEK_SET);
        first = sceIoRead(pspAt3File, info.pucWritePositionFirstBuf, info.uiWritableByteFirstBuf);
    }
    if (info.uiWritableByteSecondBuf) {
        sceIoLseek32(pspAt3File, pspAt3Base + info.uiReadPositionSecondBuf, PSP_SEEK_SET);
        second = sceIoRead(pspAt3File, info.pucWritePositionSecondBuf, info.uiWritableByteSecondBuf);
    }
    return first >= 0 && second >= 0 && PSP_AT3_CALL(4, sceAtracResetPlayPosition(pspAt3ID, sample, first, second)) >= 0;
}

static bool PSPAt3Open(const char *oggPath, uint32 startSample)
{
    if (!pspAt3Modules)
        return false;

    // the track in Data.psp, else Music/<hash>.at3 next to it
    char path[0x100];
    SceUID file = -1;
    int32 base = 0, size = 0;
    RSDKFileInfo *entry = PSPFindDataFile(oggPath);
    if (entry && entry->pspKind == PSP_PACK_AT3) {
        PSPAt3FullPath(path, sizeof(path), dataPacks[entry->packID].name);
        file = sceIoOpen(path, PSP_O_RDONLY, 0);
        base = entry->offset;
        size = entry->size;
    }
    else {
        char lower[0x100];
        StringLowerCase(lower, oggPath);
        RETRO_HASH_MD5(hash);
        GEN_HASH_MD5_BUFFER(lower, hash);
        sprintf_s(path, sizeof(path), "%sMusic/%08X%08X%08X%08X.at3", SKU::userFileDir, hash[0], hash[1], hash[2], hash[3]);
        file = sceIoOpen(path, PSP_O_RDONLY, 0);
        if (file >= 0)
            size = sceIoLseek32(file, 0, PSP_SEEK_END);
    }
    if (file < 0) {
        static int32 lastError;
        if (file != lastError) // (once per kind of failure: retried often after sleep)
            PrintLog(PRINT_NORMAL, "PSP dbg at3: can't open %s (%08X)", path, file);
        lastError = file;
        return false;
    }
    sceIoLseek32(file, base, PSP_SEEK_SET);
    int32 bytes = sceIoRead(file, pspAt3Buffer, MIN(size, PSP_AT3_BUFFER));
    int32 id    = bytes > 0 ? PSP_AT3_CALL(2, sceAtracSetDataAndGetID(pspAt3Buffer, size <= PSP_AT3_BUFFER ? size : PSP_AT3_BUFFER)) : -1;
    if (id < 0) {
        PrintLog(PRINT_NORMAL, "PSP dbg at3: %s rejected (%08X), playing the OGG", path, id);
        sceIoClose(file);
        return false;
    }

    PrintLog(PRINT_NORMAL, "PSP dbg at3: %s from %s", oggPath, base ? "Data.psp" : path);
    pspAt3ID        = id;
    pspAt3Base      = base;
    pspAt3Streaming = size > PSP_AT3_BUFFER;
    pspAt3File      = file;
    strcpy(pspAt3Path, path);
    pspAt3FileResume = pspResumeCount;
    if (!pspAt3Streaming) {
        sceIoClose(file);
        pspAt3File = -1;
    }
    pspAt3RingRead                  = pspAt3RingWrite; // (emptied without zeroing: the mixer may be reading it)
    pspAt3Ended                     = false;
    pspAt3Done                      = false;
    pspAt3NextSample                = 0;
    pspAt3DecoderResume           = pspResumeCount;
    pspAt3DecodeFails             = 0;
    if (startSample)
        PSPAt3Seek(startSample);
    pspAt3Active = true;
    return true;
}

static void PSPAt3Decode();
extern int pspFreezeTest; // (main.cpp)
extern int32 pspFrameIndex; // (PSPRenderDevice.cpp)

// Audio thread, outside the mixer lock: tops the decoder's buffer up from the Memory Stick, and decodes ahead. The file is
// opened again after the PSP slept (which closes it) or when a read fails.
static void PSPAt3Refill()
{
    if (pspAt3Sema < 0)
        return;
    sceKernelWaitSema(pspAt3Sema, 1, NULL);
    static uint32 resumeSample;
    static int32 resumeTries;
    if (pspAt3ID >= 0 && (pspAt3DecoderResume != pspResumeCount || pspAt3Reset)) {
        PrintLog(PRINT_NORMAL, "PSP dbg at3: decoder set up again (resume %d, power flags %08X, %s)", pspResumeCount, pspPowerFlags,
                 pspAt3Reset ? "decodes kept failing" : "back from sleep");
        pspAt3Reset = false;
        // back from sleep: the Media Engine lost the decoder, so the track is set up again where it had got to (from
        // the stream channel's own file name: the music the mixer plays)
        resumeSample = pspAt3NextSample;
        resumeTries  = 2000; // (mixes: ~15 s, a try every 8 of them, while the Memory Stick comes back from sleep)
        PSPAt3Close();
    }
    if (resumeTries > 0 && pspAt3ID < 0) {
        if ((resumeTries & 7) == 0 && PSPAt3Open(streamFilePath, resumeSample)) {
            PrintLog(PRINT_NORMAL, "PSP dbg at3: music back after sleep (%d tries left)", resumeTries);
            resumeTries = 0;
        }
        else if (--resumeTries == 0) {
            PrintLog(PRINT_NORMAL, "PSP dbg at3: music not back after sleep");
        }
    }
    if (pspAt3ID >= 0 && pspAt3Streaming) {
        u8 *writePos   = NULL;
        u32 available  = 0;
        u32 readOffset = 0;
        if (PSP_AT3_CALL(6, sceAtracGetStreamDataInfo(pspAt3ID, &writePos, &available, &readOffset)) >= 0 && available > 0) {
            if (pspAt3FileResume != pspResumeCount)
                PSPAt3Reopen();
            int32 read = -1;
            if (pspAt3File >= 0 && sceIoLseek32(pspAt3File, pspAt3Base + readOffset, PSP_SEEK_SET) >= 0)
                read = sceIoRead(pspAt3File, writePos, MIN(available, (u32)PSP_AT3_READ));
            if (read < 0)
                PSPAt3Reopen(); // (tried again on the next refill, the music going on from its buffer meanwhile)
            if (read > 0)
                PSP_AT3_CALL(7, sceAtracAddStreamData(pspAt3ID, read));
        }
    }
    PSPAt3Decode();
    sceKernelSignalSema(pspAt3Sema, 1);
}

// Decodes ahead into the ring while it has room for a frame, looping at streamLoopPoint like the Vorbis path. Run by the
// audio thread before each mix, and by LoadStream for the first one, holding pspAt3Sema (the decoder is one at a time)
// but never the mixer's lock.
static void PSPAt3Decode()
{
    if (pspAt3ID < 0 || !pspAt3Active || pspAt3Done)
        return;
    if ((int32)(pspAt3RingWrite - pspAt3RingRead) < 0) // (the mixer read on past a reset of the ring: empty)
        pspAt3RingRead = pspAt3RingWrite;
    while ((int32)(pspAt3RingWrite - pspAt3RingRead) <= PSP_AT3_RING - 2048) {
        if (pspAt3Ended) {
            if (pspAt3Loop && PSPAt3Seek(streamLoopPoint))
                continue;
            pspAt3Done = true;
            break;
        }
        int n = 0, end = 0, remain = 0;
        if (pspFreezeTest == 2 && pspFrameIndex > 600) { // (testing: a Media Engine that never answers)
            pspAt3Step = 5;
            PrintLog(PRINT_NORMAL, "PSP dbg freezetest: the audio thread stays in the decoder");
            sceKernelSleepThread();
        }
        int r = PSP_AT3_CALL(5, sceAtracDecodeData(pspAt3ID, (u16 *)pspAt3Pcm, &n, &end, &remain));
        if (r < 0) {
            if (r == (int)PSP_ATRAC_ERROR_ALLDATA_WAS_DECODED) {
                pspAt3Ended = true;
                continue;
            }
            // starved (the Memory Stick fell behind): silence until the buffer catches up. For a second on end,
            // the decoder is gone (sleep powers the Media Engine down): set up again (PSPAt3Refill)
            if (++pspAt3DecodeFails == 50) {
                PrintLog(PRINT_NORMAL, "PSP dbg at3: decodes failing (%08X)", r);
                pspAt3Reset = true;
            }
            break;
        }
        pspAt3DecodeFails = 0;
        for (int32 i = 0; i < n * 2; ++i) pspAt3Ring[(pspAt3RingWrite + i) & (PSP_AT3_RING - 1)] = pspAt3Pcm[i];
        pspAt3RingWrite += n * 2; // (after the samples: the mixer reads up to here)
        pspAt3NextSample += n;
        if (end)
            pspAt3Ended = true;
        if (!n && !pspAt3Ended)
            break;
    }
}

// The mixer's stream refill: the next MIX_BUFFER_SIZE samples, from the ring (no Media Engine call under the lock).
static void PSPAt3Fill(ChannelInfo *channel)
{
    float *out   = channel->samplePtr;
    int32 filled = 0;
    while (filled < MIX_BUFFER_SIZE && (int32)(pspAt3RingWrite - pspAt3RingRead) > 0) {
        out[filled++] = pspAt3Ring[pspAt3RingRead & (PSP_AT3_RING - 1)] * (0.5f / 32768.0f); // the Vorbis path's level
        pspAt3RingRead++;
    }
    if (filled < MIX_BUFFER_SIZE) {
        if (pspAt3Done) {
            channel->state   = CHANNEL_IDLE;
            channel->soundID = -1;
        }
        memset(&out[filled], 0, (MIX_BUFFER_SIZE - filled) * sizeof(float)); // (or starved: silence meanwhile)
    }
}
#endif

#if RETRO_AUDIODEVICE_XAUDIO
#include "XAudio/XAudioDevice.cpp"
#elif RETRO_AUDIODEVICE_SDL2
#include "SDL2/SDL2AudioDevice.cpp"
#elif RETRO_AUDIODEVICE_PORT
#include "PortAudio/PortAudioDevice.cpp"
#elif RETRO_AUDIODEVICE_MINI
#include "MiniAudio/MiniAudioDevice.cpp"
#elif RETRO_AUDIODEVICE_OBOE
#include "Oboe/OboeAudioDevice.cpp"
#elif RETRO_AUDIODEVICE_PSP
#include "PSP/PSPAudioDevice.cpp"
#endif

#if RETRO_PLATFORM == RETRO_PSP
// For a freeze report (main.cpp): the audio thread, its locks and the music decoder as they are now.
static void PSPDescribeSema(SceUID uid, const char *label, char *out, int32 size)
{
    SceKernelSemaInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (uid < 0 || sceKernelReferSemaStatus(uid, &info) < 0)
        snprintf(out, size, "%s %08X: ?\n", label, uid);
    else
        snprintf(out, size, "%s %08X: count %d, %d thread(s) waiting\n", label, uid, info.currentCount, info.numWaitThreads);
}

void PSPAudioDescribe(char *out, int32 size, SceUID *audioThread)
{
    *audioThread = pspAudioThread;
    int32 n = snprintf(out, size,
                       "music: %s (%s), ATRAC call in progress: %d (0 none, 1 close, 2 open, 3/4 seek, 5 decode, 6/7 refill), "
                       "decoded ahead %u of %u\n",
                       streamFilePath, pspAt3Active ? "ATRAC3" : "OGG", pspAt3Step, (uint32)(pspAt3RingWrite - pspAt3RingRead), PSP_AT3_RING);
    if (n > 0 && n < size)
        PSPDescribeSema(pspAudioSema, "mixer lock", out + n, size - n);
    n = strlen(out);
    if (n < size)
        PSPDescribeSema(pspAt3Sema, "decoder lock", out + n, size - n);
}
#endif

uint8 AudioDeviceBase::initializedAudioChannels = false;
uint8 AudioDeviceBase::audioState               = 0;
uint8 AudioDeviceBase::audioFocus               = 0;

void AudioDeviceBase::Release()
{
    // This is missing, meaning that the garbage collector will never reclaim stb_vorbis's buffer.
#if !RETRO_USE_ORIGINAL_CODE
    stb_vorbis_close(vorbisInfo);
    vorbisInfo = NULL;
#endif
#if RETRO_PLATFORM == RETRO_PSP
    PSPAt3Close();
#endif
}

void AudioDeviceBase::ProcessAudioMixing(void *stream, int32 length)
{
    SAMPLE_FORMAT *streamF    = (SAMPLE_FORMAT *)stream;
    SAMPLE_FORMAT *streamEndF = ((SAMPLE_FORMAT *)stream) + length;

    memset(stream, 0, length * sizeof(SAMPLE_FORMAT));

    for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
        ChannelInfo *channel = &channels[c];

        switch (channel->state) {
            default:
            case CHANNEL_IDLE: break;

            case CHANNEL_SFX: {
                SFX_SAMPLE *sfxBuffer = (SFX_SAMPLE *)channel->samplePtr + channel->bufferPos;

                float volL = channel->volume, volR = channel->volume;
                if (channel->pan < 0.0f)
                    volR = (1.0f + channel->pan) * channel->volume;
                else
                    volL = (1.0f - channel->pan) * channel->volume;

                float panL = volL * engine.soundFXVolume;
                float panR = volR * engine.soundFXVolume;

                uint32 speedPercent       = 0;
                SAMPLE_FORMAT *curStreamF = streamF;
                while (curStreamF < streamEndF && streamF < streamEndF) {
                    // Perform linear interpolation.
                    SAMPLE_FORMAT sample;
#if !RETRO_USE_ORIGINAL_CODE
                    if (!sfxBuffer) // PROTECTION FOR v5U (and other mysterious crashes 👻)
                        sample = 0;
                    else
#endif
                        sample = ((float)(sfxBuffer[1] - sfxBuffer[0]) * linearInterpolationLookup[speedPercent / LINEAR_INTERPOLATION_LOOKUP_DIVISOR]
                                  + sfxBuffer[0])
                                 * SFX_SAMPLE_SCALE;

                    speedPercent += channel->speed;
                    sfxBuffer += FROM_FIXED(speedPercent);
                    channel->bufferPos += FROM_FIXED(speedPercent);
                    speedPercent %= TO_FIXED(1);

                    curStreamF[0] += sample * panL;
                    curStreamF[1] += sample * panR;
                    curStreamF += 2;

                    if (channel->bufferPos >= channel->sampleLength) {
                        if (channel->loop == (uint32)-1) {
                            channel->state   = CHANNEL_IDLE;
                            channel->soundID = -1;
                            break;
                        }
                        else {
                            channel->bufferPos -= (uint32)channel->sampleLength;
                            channel->bufferPos += channel->loop;

                            sfxBuffer = (SFX_SAMPLE *)channel->samplePtr + channel->bufferPos;
                        }
                    }
                }

                break;
            }

            case CHANNEL_STREAM: {
                SAMPLE_FORMAT *streamBuffer = &channel->samplePtr[channel->bufferPos];

                float volL = channel->volume, volR = channel->volume;
                if (channel->pan < 0.0f)
                    volR = (1.0f + channel->pan) * channel->volume;
                else
                    volL = (1.0f - channel->pan) * channel->volume;

                float panL = volL * engine.streamVolume;
                float panR = volR * engine.streamVolume;

                uint32 speedPercent       = 0;
                SAMPLE_FORMAT *curStreamF = streamF;
                while (curStreamF < streamEndF && streamF < streamEndF) {
                    speedPercent += channel->speed;
                    int32 next = FROM_FIXED(speedPercent);
                    speedPercent %= TO_FIXED(1);

                    curStreamF[0] += streamBuffer[0] * panL;
                    curStreamF[1] += streamBuffer[1] * panR;
                    curStreamF += 2;

                    streamBuffer += next * 2;
                    channel->bufferPos += next * 2;

                    if (channel->bufferPos >= channel->sampleLength) {
                        channel->bufferPos -= (uint32)channel->sampleLength;

                        streamBuffer = &channel->samplePtr[channel->bufferPos];

                        UpdateStreamBuffer(channel);
                    }
                }
                break;
            }

            case CHANNEL_LOADING_STREAM: break;
        }
    }
}

void AudioDeviceBase::InitAudioChannels()
{
    for (int32 i = 0; i < CHANNEL_COUNT; ++i) {
        channels[i].soundID = -1;
        channels[i].state   = CHANNEL_IDLE;
    }

    // Compute a lookup table of floating-point linear interpolation delta scales,
    // to speed-up the process of converting from fixed-point to floating-point.
    for (int32 i = 0; i < LINEAR_INTERPOLATION_LOOKUP_LENGTH; ++i) linearInterpolationLookup[i] = i / (float)LINEAR_INTERPOLATION_LOOKUP_LENGTH;

    GEN_HASH_MD5("Stream Channel 0", sfxList[SFX_COUNT - 1].hash);
    sfxList[SFX_COUNT - 1].scope              = SCOPE_GLOBAL;
    sfxList[SFX_COUNT - 1].maxConcurrentPlays = 1;
    sfxList[SFX_COUNT - 1].length             = MIX_BUFFER_SIZE;
    AllocateStorage((void **)&sfxList[SFX_COUNT - 1].buffer, MIX_BUFFER_SIZE * sizeof(SAMPLE_FORMAT), DATASET_MUS, false);

    initializedAudioChannels = true;
}

void RSDK::UpdateStreamBuffer(ChannelInfo *channel)
{
#if RETRO_PLATFORM == RETRO_PSP
    if (pspAt3Active) {
        PSPAt3Fill(channel);
        return;
    }
    if (!vorbisInfo) {
        // (the ATRAC3 track being set up again after sleep: silence meanwhile)
        memset(channel->samplePtr, 0, MIX_BUFFER_SIZE * sizeof(float));
        return;
    }
#endif
    int32 bufferRemaining = MIX_BUFFER_SIZE;
    float *buffer         = channel->samplePtr;

    for (int32 s = 0; s < MIX_BUFFER_SIZE;) {
        int32 samples = stb_vorbis_get_samples_float_interleaved(vorbisInfo, 2, buffer, bufferRemaining) * 2;
        if (!samples) {
            if (channel->loop == 1 && stb_vorbis_seek_frame(vorbisInfo, streamLoopPoint)) {
                // we're looping & the seek was successful, get more samples
            }
            else {
                channel->state   = CHANNEL_IDLE;
                channel->soundID = -1;
                memset(buffer, 0, sizeof(float) * bufferRemaining);

                break;
            }
        }

        s += samples;
        buffer += samples;
        bufferRemaining = MIX_BUFFER_SIZE - s;
    }

    for (int32 i = 0; i < MIX_BUFFER_SIZE; ++i) channel->samplePtr[i] *= 0.5f;
}

void RSDK::LoadStream(ChannelInfo *channel)
{
    if (channel->state != CHANNEL_LOADING_STREAM)
        return;

    stb_vorbis_close(vorbisInfo);
    vorbisInfo = NULL;

#if RETRO_PLATFORM == RETRO_PSP
    // (not forever: should the audio thread be stuck in the Media Engine with the decoder, the game goes on silent)
    pspPhase        = "music load";
    SceUInt at3Wait = 1000000;
    if (pspAt3Sema >= 0 && sceKernelWaitSema(pspAt3Sema, 1, &at3Wait) < 0) {
        PrintLog(PRINT_NORMAL, "PSP dbg at3: decoder busy for a second (step %d), %s not played", pspAt3Step, streamFilePath);
        channel->state   = CHANNEL_IDLE;
        channel->soundID = -1;
        return;
    }
    PSPAt3Close();
    pspAt3Loop = channel->loop == 1;
    bool32 at3 = PSPAt3Open(streamFilePath, streamStartPos);
    if (at3) {
        streamBufferSize = 0;
        PSPAt3Decode();
        UpdateStreamBuffer(channel);
        channel->state = CHANNEL_STREAM;
    }
    if (pspAt3Sema >= 0)
        sceKernelSignalSema(pspAt3Sema, 1);
    if (at3)
        return;
#endif

    FileInfo info;
    InitFileInfo(&info);

    if (LoadFile(&info, streamFilePath, FMODE_RB)) {
        streamBufferSize = info.fileSize;
        streamBuffer     = NULL;
        AllocateStorage((void **)&streamBuffer, info.fileSize, DATASET_MUS, false);
        if (!streamBuffer) {
            PrintLog(PRINT_NORMAL, "Unable to allocate music stream: %s (%d bytes)", streamFilePath, info.fileSize);
            CloseFile(&info);
            channel->state = CHANNEL_IDLE;
            return;
        }
        ReadBytes(&info, streamBuffer, streamBufferSize);
        CloseFile(&info);

        if (streamBufferSize > 0) {
            vorbisAlloc.alloc_buffer_length_in_bytes = 512 * 1024; // 512KiB
            AllocateStorage((void **)&vorbisAlloc.alloc_buffer, 512 * 1024, DATASET_MUS, false);
            if (!vorbisAlloc.alloc_buffer) {
                PrintLog(PRINT_NORMAL, "Unable to allocate vorbis scratch for %s", streamFilePath);
                channel->state = CHANNEL_IDLE;
                return;
            }

            vorbisInfo = stb_vorbis_open_memory(streamBuffer, streamBufferSize, NULL, &vorbisAlloc);
            if (vorbisInfo) {
                if (streamStartPos)
                    stb_vorbis_seek(vorbisInfo, streamStartPos);
                UpdateStreamBuffer(channel);

                channel->state = CHANNEL_STREAM;
            }
        }
    }

    if (channel->state == CHANNEL_LOADING_STREAM)
        channel->state = CHANNEL_IDLE;
}

int32 RSDK::PlayStream(const char *filename, uint32 slot, uint32 startPos, uint32 loopPoint, bool32 loadASync)
{
    if (!engine.streamsEnabled)
        return -1;

    if (slot >= CHANNEL_COUNT) {
        for (int32 c = 0; c < CHANNEL_COUNT && slot >= CHANNEL_COUNT; ++c) {
            if (channels[c].soundID == -1 && channels[c].state != CHANNEL_LOADING_STREAM) {
                slot = c;
            }
        }

        // as a last resort, run through all channels
        // pick the channel closest to being finished
        if (slot >= CHANNEL_COUNT) {
            uint32 len = 0xFFFFFFFF;
            for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
                if (channels[c].sampleLength < len && channels[c].state != CHANNEL_LOADING_STREAM) {
                    slot = c;
                    len  = (uint32)channels[c].sampleLength;
                }
            }
        }
    }

    if (slot >= CHANNEL_COUNT)
        return -1;

    ChannelInfo *channel = &channels[slot];

    LockAudioDevice();

    channel->soundID      = 0xFF;
    channel->loop         = loopPoint != 0;
    channel->priority     = 0xFF;
    channel->state        = CHANNEL_LOADING_STREAM;
    channel->pan          = 0.0f;
    channel->volume       = 1.0f;
    channel->sampleLength = sfxList[SFX_COUNT - 1].length;
    channel->samplePtr    = sfxList[SFX_COUNT - 1].buffer;
    channel->bufferPos    = 0;
    channel->speed        = TO_FIXED(1);

    sprintf_s(streamFilePath, sizeof(streamFilePath), "Data/Music/%s", filename);
    streamStartPos  = startPos;
    streamLoopPoint = loopPoint;

#if RETRO_PLATFORM == RETRO_PSP
    // Reading a multi-MB track off the Memory Stick under the device lock froze every sound for the whole read.
    // The mixer skips CHANNEL_LOADING_STREAM, and the one decoder (vorbisInfo) is only touched by the mixer through
    // a CHANNEL_STREAM channel: retire any other stream here, then load without holding the lock.
    for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
        if (c != (int32)slot && channels[c].state == CHANNEL_STREAM) {
            channels[c].state   = CHANNEL_IDLE;
            channels[c].soundID = -1;
        }
    }
    UnlockAudioDevice();

    unsigned long long loadStart = PSPProfNow();
    AudioDevice::HandleStreamLoad(channel, loadASync);
    PrintLog(PRINT_NORMAL, "PSP dbg stream %s: %ums (%u KB)", filename, (uint32)((PSPProfNow() - loadStart) / 1000), (uint32)streamBufferSize >> 10);
#else
    AudioDevice::HandleStreamLoad(channel, loadASync);

    UnlockAudioDevice();
#endif

    return slot;
}

#define WAV_SIG_HEADER (0x46464952) // RIFF
#define WAV_SIG_DATA   (0x61746164) // data

void RSDK::LoadSfxToSlot(char *filename, uint8 slot, uint8 plays, uint8 scope)
{
#if RETRO_PLATFORM == RETRO_PSP
    PSPLoadTimer loadTimer(PSPLOAD_SFX);
#endif
    FileInfo info;
    InitFileInfo(&info);

    char fullFilePath[0x80];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/SoundFX/%s", filename);

    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5(filename, hash);

    if (LoadFile(&info, fullFilePath, FMODE_RB)) {
        HASH_COPY_MD5(sfxList[slot].hash, hash);
        sfxList[slot].scope              = scope;
        sfxList[slot].maxConcurrentPlays = plays;

        uint8 type = fullFilePath[strlen(fullFilePath) - 1];
        if (type == 'v' || type == 'V') { // A very loose way of checking that we're trying to load a '.wav' file.
            uint32 signature = ReadInt32(&info, false);

            if (signature == WAV_SIG_HEADER) {
                ReadInt32(&info, false); // chunk size
                ReadInt32(&info, false); // WAVE
                ReadInt32(&info, false); // FMT
#if !RETRO_USE_ORIGINAL_CODE
                int32 chunkSize = ReadInt32(&info, false); // chunk size
#else
                ReadInt32(&info, false); // chunk size
#endif
                ReadInt16(&info);        // audio format
                ReadInt16(&info);        // channels
                ReadInt32(&info, false); // sample rate
                ReadInt32(&info, false); // bytes per sec
                ReadInt16(&info);        // block align
                ReadInt16(&info);        // format

                Seek_Set(&info, 34);
                uint16 sampleBits = ReadInt16(&info);

#if !RETRO_USE_ORIGINAL_CODE
                // Original code added to help fix some issues
                Seek_Set(&info, 20 + chunkSize);
#endif

                // Find the data header
                int32 loop = 0;
                while (true) {
                    signature = ReadInt32(&info, false);
                    if (signature == WAV_SIG_DATA)
                        break;

                    loop += 4;
                    if (loop >= 0x40) {
                        if (loop != 0x100) {
                            CloseFile(&info);
                            // There's a bug here: `sfxList[id].scope` is not reset to `SCOPE_NONE`,
                            // meaning that the game will consider the SFX valid and allow it to be played.
                            // This can cause a crash because the SFX is incomplete.
#if !RETRO_USE_ORIGINAL_CODE
                            PrintLog(PRINT_ERROR, "Unable to read sfx: %s", filename);
#endif
                            return;
                        }
                        else {
                            break;
                        }
                    }
                }

                uint32 length = ReadInt32(&info, false);
                if (sampleBits == 16)
                    length /= 2;

                AllocateStorage((void **)&sfxList[slot].buffer, sizeof(SFX_SAMPLE) * length, DATASET_SFX, false);
                if (!sfxList[slot].buffer) {
                    CloseFile(&info);
#if !RETRO_USE_ORIGINAL_CODE
                    PrintLog(PRINT_ERROR, "Unable to allocate SFX buffer: %s (%u samples)", filename, length);
#endif
                    return;
                }
                sfxList[slot].length = length;

#if RETRO_PLATFORM == RETRO_PSP
                // Keep the S16 data as-is; the 0.75 gain and F32 conversion happen in the mixer (SFX_SAMPLE_SCALE).
                // 8-bit samples end up 25% quieter than on PC, since their full gain would overflow S16.
                int16 *buffer = (int16 *)sfxList[slot].buffer;
                if (sampleBits == 8) {
                    // one read instead of one per byte: the bytes land in the upper half of the buffer and widen in place,
                    // front to back (each sample written stays below the next byte read)
                    uint8 *bytes = (uint8 *)buffer + length;
                    ReadBytes(&info, bytes, length);
                    for (int32 s = 0; s < length; ++s) buffer[s] = (int16)((bytes[s] - 0x80) << 8);
                }
                else {
                    ReadBytes(&info, buffer, length * sizeof(int16));
                }
#else
                // Convert the sample data to F32 format
                float *buffer = (float *)sfxList[slot].buffer;
                if (sampleBits == 8) {
                    // 8-bit sample. Convert from U8 to S8, and then from S8 to F32.
                    for (int32 s = 0; s < length; ++s) {
                        int32 sample = ReadInt8(&info);
                        *buffer++    = (sample - 0x80) / (float)0x80;
                    }
                }
                else {
                    // 16-bit sample. Convert from S16 to F32.
                    for (int32 s = 0; s < length; ++s) {
                        // For some reason, the game performs sign-extension manually here.
                        // Note that this is different from the 8-bit format's unsigned-to-signed conversion.
                        int32 sample = (uint16)ReadInt16(&info);

                        if (sample > 0x7FFF)
                            sample = (sample & 0x7FFF) - 0x8000;

                        *buffer++ = (sample / (float)0x8000) * 0.75f;
                    }
                }
#endif
            }
#if !RETRO_USE_ORIGINAL_CODE
            else {
                PrintLog(PRINT_ERROR, "Invalid header in sfx: %s", filename);
            }
#endif
        }
#if !RETRO_USE_ORIGINAL_CODE
        else {
            // what the
            PrintLog(PRINT_ERROR, "Could not find header in sfx: %s", filename);
        }
#endif
    }
#if !RETRO_USE_ORIGINAL_CODE
    else {
        PrintLog(PRINT_ERROR, "Unable to open sfx: %s", filename);
    }
#endif

    CloseFile(&info);
}

void RSDK::LoadSfx(char *filename, uint8 plays, uint8 scope)
{
    // Find an empty sound slot.
    uint16 id = -1;
    for (uint32 i = 0; i < SFX_COUNT; ++i) {
        if (sfxList[i].scope == SCOPE_NONE) {
            id = i;
            break;
        }
    }

    if (id != (uint16)-1)
        LoadSfxToSlot(filename, id, plays, scope);
}

int32 RSDK::PlaySfx(uint16 sfx, uint32 loopPoint, uint32 priority)
{
    if (sfx >= SFX_COUNT || !sfxList[sfx].scope)
        return -1;

#if !RETRO_USE_ORIGINAL_CODE
    // a failed load (e.g. SFX pool exhausted) leaves the slot scoped but bufferless
    if (!sfxList[sfx].buffer || !sfxList[sfx].length)
        return -1;
#endif

    uint8 count = 0;
    for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
        if (channels[c].soundID == sfx)
            ++count;
    }

    int8 slot = -1;
    // if we've hit the max, replace the oldest one
    if (count >= sfxList[sfx].maxConcurrentPlays) {
        int32 highestStackID = 0;
        for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
            int32 stackID = sfxList[sfx].playCount - channels[c].playIndex;
            if (stackID > highestStackID && channels[c].soundID == sfx) {
                slot           = c;
                highestStackID = stackID;
            }
        }
    }

    // if we don't have a slot yet, try to pick any channel that's not currently playing
    for (int32 c = 0; c < CHANNEL_COUNT && slot < 0; ++c) {
        if (channels[c].soundID == -1 && channels[c].state != CHANNEL_LOADING_STREAM) {
            slot = c;
        }
    }

    // as a last resort, run through all channels
    // pick the channel closest to being finished AND with lower priority
    if (slot < 0) {
        uint32 len = 0xFFFFFFFF;
        for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
            if (channels[c].sampleLength < len && priority > channels[c].priority && channels[c].state != CHANNEL_LOADING_STREAM) {
                slot = c;
                len  = (uint32)channels[c].sampleLength;
            }
        }
    }

    if (slot == -1)
        return -1;

    LockAudioDevice();

    channels[slot].state        = CHANNEL_SFX;
    channels[slot].bufferPos    = 0;
    channels[slot].samplePtr    = sfxList[sfx].buffer;
    channels[slot].sampleLength = sfxList[sfx].length;
    channels[slot].volume       = 1.0f;
    channels[slot].pan          = 0.0f;
    channels[slot].speed        = TO_FIXED(1);
    channels[slot].soundID      = sfx;
    if (loopPoint >= 2)
        channels[slot].loop = loopPoint;
    else
        channels[slot].loop = loopPoint - 1;
    channels[slot].priority  = priority;
    channels[slot].playIndex = sfxList[sfx].playCount++;

    UnlockAudioDevice();

    return slot;
}

void RSDK::SetChannelAttributes(uint8 channel, float volume, float panning, float speed)
{
    if (channel < CHANNEL_COUNT) {
        volume                   = fminf(4.0f, volume);
        volume                   = fmaxf(0.0f, volume);
        channels[channel].volume = volume;

        panning               = fminf(1.0f, panning);
        panning               = fmaxf(-1.0f, panning);
        channels[channel].pan = panning;

        if (speed > 0.0f)
            channels[channel].speed = (int32)(speed * TO_FIXED(1));
        else if (speed == 1.0f)
            channels[channel].speed = TO_FIXED(1);
    }
}

uint32 RSDK::GetChannelPos(uint32 channel)
{
    if (channel >= CHANNEL_COUNT)
        return 0;

    if (channels[channel].state == CHANNEL_SFX)
        return channels[channel].bufferPos;

    if (channels[channel].state == CHANNEL_STREAM) {
#if RETRO_PLATFORM == RETRO_PSP
        // music jingles (drowning, invincibility, 1UP...) save the stage track's position here to resume it: with an
        // ATRAC3 track there is no Vorbis decoder to ask (reading it crashed the game as the drowning countdown began)
        if (pspAt3Active)
            return pspAt3NextSample - MAX((int32)(pspAt3RingWrite - pspAt3RingRead), 0) / 2; // (less what is decoded ahead)
#endif
        if (!vorbisInfo || !vorbisInfo->current_loc_valid || vorbisInfo->current_loc < 0)
            return 0;

        return vorbisInfo->current_loc;
    }

    return 0;
}

double RSDK::GetVideoStreamPos()
{
#if RETRO_PLATFORM == RETRO_PSP
    if (channels[0].state == CHANNEL_STREAM && AudioDevice::audioState && pspAt3Active) {
        // what is being heard rather than what has been decoded: take off what is decoded ahead in the ring, and the
        // mixer's buffers (~1.5 buffers of 1024 samples: the channel's, half used on average, and the one playing) less
        // the video's own delay to the screen (decoded at the end of the frame, shown at the next vsync), ~1 buffer
        int32 heard = (int32)pspAt3NextSample - (int32)(pspAt3RingWrite - pspAt3RingRead) / 2 - MIX_BUFFER_SIZE / 2;
        return heard > 0 ? heard / (double)AUDIO_FREQUENCY : 0.0;
    }
#endif
    if (channels[0].state == CHANNEL_STREAM && AudioDevice::audioState && AudioDevice::initializedAudioChannels && vorbisInfo
        && vorbisInfo->current_loc_valid) {
        return vorbisInfo->current_loc / (double)AUDIO_FREQUENCY;
    }

    return -1.0;
}

void RSDK::ClearStageSfx()
{
    LockAudioDevice();

    for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
        if (channels[c].state == CHANNEL_SFX || channels[c].state == (CHANNEL_SFX | CHANNEL_PAUSED)) {
            channels[c].soundID = -1;
            channels[c].state   = CHANNEL_IDLE;
        }
    }

    // Unload stage SFX
    for (int32 s = 0; s < SFX_COUNT; ++s) {
        if (sfxList[s].scope >= SCOPE_STAGE) {
            MEM_ZERO(sfxList[s]);
            sfxList[s].scope = SCOPE_NONE;
        }
    }

    UnlockAudioDevice();
}

#if RETRO_USE_MOD_LOADER
void RSDK::ClearGlobalSfx()
{
    LockAudioDevice();

    for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
        if (channels[c].state == CHANNEL_SFX || channels[c].state == (CHANNEL_SFX | CHANNEL_PAUSED)) {
            channels[c].soundID = -1;
            channels[c].state   = CHANNEL_IDLE;
        }
    }

    // Unload global SFX
    for (int32 s = 0; s < SFX_COUNT; ++s) {
        // clear global sfx (do NOT clear the stream channel 0 slot)
        if (sfxList[s].scope == SCOPE_GLOBAL && s != SFX_COUNT - 1) {
            MEM_ZERO(sfxList[s]);
            sfxList[s].scope = SCOPE_NONE;
        }
    }

    UnlockAudioDevice();
}
#endif
