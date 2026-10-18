// PSP audio: a dedicated high priority thread runs the engine's mixer and pushes 16-bit stereo
// through the hardware sample rate converter channel.

#include <pspsdk.h>

#define PSP_AUDIO_FRAMES (MIX_BUFFER_SIZE / AUDIO_CHANNELS)

uint8 AudioDevice::contextInitialized;

static SceUID pspAudioSema       = -1;
static SceUID pspAudioThread     = -1;
static int pspAudioChannel       = -1;
static volatile bool pspAudioRun = false;

static float pspMixBuffer[MIX_BUFFER_SIZE];
static short pspOutBuffer[2][MIX_BUFFER_SIZE] __attribute__((aligned(64)));

void AudioDevice::LockDevice()
{
    if (pspAudioSema < 0)
        return;

    if (sceKernelGetThreadId() == pspAudioThread) {
        sceKernelWaitSema(pspAudioSema, 1, NULL);
        return;
    }

    // game-side callers (PlaySfx, channel attributes...): account for time spent waiting on the mixer
    PSP_PROF_BEGIN(PSPPROF_AUDLOCK);
    sceKernelWaitSema(pspAudioSema, 1, NULL);
    PSP_PROF_END(PSPPROF_AUDLOCK);
}

void AudioDevice::UnlockDevice()
{
    if (pspAudioSema >= 0)
        sceKernelSignalSema(pspAudioSema, 1);
}

int AudioDevice::AudioThread(SceSize args, void *argp)
{
    // FPU control state is per thread; mask exceptions here too (see main.cpp).
    pspSdkDisableFPUExceptions();

    int bufIndex = 0;

    while (pspAudioRun) {
        short *out = pspOutBuffer[bufIndex];
        PSPAt3Refill(); // Memory Stick reads stay outside the mixer lock

        unsigned long long mixStart = PSPProfNow();
        LockDevice();
        memset(pspMixBuffer, 0, sizeof(pspMixBuffer));
        ProcessAudioMixing(pspMixBuffer, MIX_BUFFER_SIZE);
        UnlockDevice();

        pspProf[PSPPROF_AUDIO] += (PSPProfNow() - mixStart) / 2; // ~2 mixes per video frame at 22kHz
        for (int32 i = 0; i < MIX_BUFFER_SIZE; ++i) {
            float sample = pspMixBuffer[i];
            if (sample > 1.0f)
                sample = 1.0f;
            else if (sample < -1.0f)
                sample = -1.0f;
            out[i] = (short)(sample * 32767.0f);
        }

        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, out);
        bufIndex ^= 1;
    }

    return 0;
}

bool32 AudioDevice::Init()
{
    if (!contextInitialized) {
        contextInitialized = true;
        InitAudioChannels();
    }

    audioState = false;
    PSPAt3Init();

    if (pspAudioSema < 0)
        pspAudioSema = sceKernelCreateSema("rsdk_audio", 0, 1, 1, NULL);

    pspAudioChannel = sceAudioSRCChReserve(PSP_AUDIO_FRAMES, AUDIO_FREQUENCY, 2);
    if (pspAudioChannel < 0) {
        PrintLog(PRINT_NORMAL, "ERROR: Unable to open audio device! (0x%08X)", pspAudioChannel);
        return true;
    }

    pspAudioRun    = true;
    // Must outrank the main thread (0x20): the PSP scheduler does not time-slice equal priorities, so an
    // equal-priority mixer only ran when the game blocked and starved (dropouts) once frames stopped waiting.
    // The thread sleeps in sceAudioSRCOutputBlocking between mixes, so it only takes the CPU it needs.
    pspAudioThread = sceKernelCreateThread("rsdk_audio", AudioThread, 0x12, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (pspAudioThread >= 0) {
        sceKernelStartThread(pspAudioThread, 0, NULL);
        audioState = true;
    }
    else {
        PrintLog(PRINT_NORMAL, "ERROR: Unable to create audio thread!");
    }

    return true;
}

void AudioDevice::Release()
{
    pspAudioRun = false;

    if (pspAudioThread >= 0) {
        sceKernelWaitThreadEnd(pspAudioThread, NULL);
        sceKernelDeleteThread(pspAudioThread);
        pspAudioThread = -1;
    }

    if (pspAudioChannel >= 0) {
        sceAudioSRCChRelease();
        pspAudioChannel = -1;
    }

    LockAudioDevice();
    AudioDeviceBase::Release();
    UnlockAudioDevice();
}

void AudioDevice::InitAudioChannels() { AudioDeviceBase::InitAudioChannels(); }
