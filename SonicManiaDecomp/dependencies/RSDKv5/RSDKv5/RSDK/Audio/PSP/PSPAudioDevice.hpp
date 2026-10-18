#define LockAudioDevice()   RSDK::AudioDevice::LockDevice()
#define UnlockAudioDevice() RSDK::AudioDevice::UnlockDevice()

namespace RSDK
{
class AudioDevice : public AudioDeviceBase
{
public:
    static bool32 Init();
    static void Release();

    static void FrameInit() {}

    static void LockDevice();
    static void UnlockDevice();

    inline static void HandleStreamLoad(ChannelInfo *channel, bool32 async)
    {
        // decoding the first chunk inline keeps the PSP simple (no extra stack-hungry thread)
        LoadStream(channel);
    }

private:
    static uint8 contextInitialized;

    static void InitAudioChannels();

    static int AudioThread(SceSize args, void *argp);
};
} // namespace RSDK
