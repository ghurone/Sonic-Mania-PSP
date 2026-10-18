#ifndef PSP_STUBS_H
#define PSP_STUBS_H

// PSP build: libogg/libtheora are not available, so video playback is stubbed out.
// These are only the opaque types that Video.hpp refers to.

typedef long long ogg_int64_t;

struct ogg_sync_state {
    int dummy;
};
struct ogg_page {
    int dummy;
};
struct ogg_packet {
    int dummy;
};
struct ogg_stream_state {
    int dummy;
};

struct th_info {
    int dummy;
};
struct th_comment {
    int dummy;
};
struct th_dec_ctx;
struct th_setup_info;
typedef int th_pixel_fmt;

#endif
