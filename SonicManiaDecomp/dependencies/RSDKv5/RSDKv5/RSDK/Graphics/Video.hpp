#ifndef VIDEO_H
#define VIDEO_H

namespace RSDK
{

struct VideoManager {
    static FileInfo file;

    static ogg_sync_state oy;
    static ogg_page og;
    static ogg_stream_state vo;
    static ogg_stream_state to;
    static ogg_packet op;
    static th_info ti;
    static th_comment tc;
    static th_dec_ctx *td;
    static th_setup_info *ts;

    static th_pixel_fmt pixelFormat;
    static ogg_int64_t granulePos;
    static bool32 initializing;
};

bool32 LoadVideo(const char *filename, double startDelay, bool32 (*skipCallback)());
void ProcessVideo();

#if RETRO_PLATFORM == RETRO_PSP
// Data.psp's videos and full-screen images are JPEG frames (see Video.cpp). FlipScreen asks whether a new frame is
// waiting and, once the GE has let go of the VRAM, has it decoded there.
bool32 PSPVideoFramePending();
bool32 PSPVideoDecodeFrame(uint8 *target);
// LoadImage: shows Data.psp's version of an image (one JPEG frame); false if the file isn't one
bool32 PSPLoadImageFrame(const char *path);
#endif

} // namespace RSDK

#endif // VIDEO_H
