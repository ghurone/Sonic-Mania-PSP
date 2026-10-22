#include "RSDK/Core/RetroEngine.hpp"

using namespace RSDK;

#if RETRO_REV0U
#include "Legacy/SpriteLegacy.cpp"
#endif

const int32 LOADING_IMAGE = 0;
const int32 LOAD_COMPLETE = 1;
const int32 LZ_MAX_CODE   = 4095;
const int32 LZ_BITS       = 12;
const int32 FIRST_CODE    = 4097;
const int32 NO_SUCH_CODE  = 4098;

int32 codeMasks[] = { 0, 1, 3, 7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095 };

int32 ReadGifCode(ImageGIF *image);
uint8 ReadGifByte(ImageGIF *image);
uint8 TraceGifPrefix(uint32 *prefix, int32 code, int32 clearCode);

void InitGifDecoder(ImageGIF *image)
{
    uint8 initCodeSize             = ReadInt8(&image->info);
    image->decoder->fileState      = LOADING_IMAGE;
    image->decoder->position       = 0;
    image->decoder->bufferSize     = 0;
    image->decoder->buffer[0]      = 0;
    image->decoder->depth          = initCodeSize;
    image->decoder->clearCode      = 1 << initCodeSize;
    image->decoder->eofCode        = image->decoder->clearCode + 1;
    image->decoder->runningCode    = image->decoder->eofCode + 1;
    image->decoder->runningBits    = initCodeSize + 1;
    image->decoder->maxCodePlusOne = 1 << image->decoder->runningBits;
    image->decoder->stackPtr       = 0;
    image->decoder->prevCode       = NO_SUCH_CODE;
    image->decoder->shiftState     = 0;
    image->decoder->shiftData      = 0;

    for (int32 i = 0; i <= LZ_MAX_CODE; ++i) image->decoder->prefix[i] = (uint8)NO_SUCH_CODE;
}
void ReadGifLine(ImageGIF *image, uint8 *line, int32 length, int32 offset)
{
    int32 i         = 0;
    int32 stackPtr  = image->decoder->stackPtr;
    int32 eofCode   = image->decoder->eofCode;
    int32 clearCode = image->decoder->clearCode;
    int32 prevCode  = image->decoder->prevCode;

    if (stackPtr != 0) {
        while (stackPtr != 0) {
            if (i >= length)
                break;

            line[offset++] = image->decoder->stack[--stackPtr];
            i++;
        }
    }

    while (i < length) {
        int32 gifCode = ReadGifCode(image);
        if (gifCode == eofCode) {
            if (i != length - 1 || image->decoder->pixelCount != 0)
                return;

            i++;
        }
        else {
            if (gifCode == clearCode) {
                for (int32 p = 0; p <= LZ_MAX_CODE; p++) image->decoder->prefix[p] = NO_SUCH_CODE;

                image->decoder->runningCode    = image->decoder->eofCode + 1;
                image->decoder->runningBits    = image->decoder->depth + 1;
                image->decoder->maxCodePlusOne = 1 << image->decoder->runningBits;

                prevCode = image->decoder->prevCode = NO_SUCH_CODE;
            }
            else {
                if (gifCode < clearCode) {
                    line[offset++] = (uint8)gifCode;
                    i++;
                }
                else {
                    if (gifCode < 0 || gifCode > LZ_MAX_CODE)
                        return;

                    int32 code = gifCode;
                    if (image->decoder->prefix[gifCode] == NO_SUCH_CODE) {
                        if (gifCode != image->decoder->runningCode - 2)
                            return;

                        code = prevCode;

                        image->decoder->suffix[image->decoder->runningCode - 2] = image->decoder->stack[stackPtr++] =
                            TraceGifPrefix(image->decoder->prefix, prevCode, clearCode);
                    }

                    int32 c = 0;
                    while (c++ <= LZ_MAX_CODE && code > clearCode && code <= LZ_MAX_CODE) {
                        image->decoder->stack[stackPtr++] = image->decoder->suffix[code];
                        code                              = image->decoder->prefix[code];
                    }

                    if (c >= LZ_MAX_CODE || code > LZ_MAX_CODE)
                        return;

                    image->decoder->stack[stackPtr++] = (uint8)code;

                    while (stackPtr != 0 && i++ < length) line[offset++] = image->decoder->stack[--stackPtr];
                }

                if (prevCode != NO_SUCH_CODE) {
                    if (image->decoder->runningCode < 2 || image->decoder->runningCode > FIRST_CODE)
                        return;

                    image->decoder->prefix[image->decoder->runningCode - 2] = prevCode;

                    if (gifCode == image->decoder->runningCode - 2)
                        image->decoder->suffix[image->decoder->runningCode - 2] = TraceGifPrefix(image->decoder->prefix, prevCode, clearCode);
                    else
                        image->decoder->suffix[image->decoder->runningCode - 2] = TraceGifPrefix(image->decoder->prefix, gifCode, clearCode);
                }

                prevCode = gifCode;
            }
        }
    }

    image->decoder->prevCode = prevCode;
    image->decoder->stackPtr = stackPtr;
}

int32 ReadGifCode(ImageGIF *image)
{
    while (image->decoder->shiftState < image->decoder->runningBits) {
        uint8 b = ReadGifByte(image);
        image->decoder->shiftData |= (uint32)b << image->decoder->shiftState;
        image->decoder->shiftState += 8;
    }

    int32 result = (int32)(image->decoder->shiftData & (uint32)codeMasks[image->decoder->runningBits]);
    image->decoder->shiftData >>= image->decoder->runningBits;
    image->decoder->shiftState -= image->decoder->runningBits;
    if (++image->decoder->runningCode > image->decoder->maxCodePlusOne && image->decoder->runningBits < LZ_BITS) {
        image->decoder->maxCodePlusOne <<= 1;
        image->decoder->runningBits++;
    }

    return result;
}

uint8 ReadGifByte(ImageGIF *image)
{
    uint8 c = '\0';
    if (image->decoder->fileState == LOAD_COMPLETE)
        return c;

    uint8 b;
    if (image->decoder->position == image->decoder->bufferSize) {
        b                          = ReadInt8(&image->info);
        image->decoder->bufferSize = b;
        if (image->decoder->bufferSize == 0) {
            image->decoder->fileState = LOAD_COMPLETE;
            return c;
        }

        ReadBytes(&image->info, image->decoder->buffer, image->decoder->bufferSize);
        b                        = image->decoder->buffer[0];
        image->decoder->position = 1;
    }
    else {
        b = image->decoder->buffer[image->decoder->position++];
    }

    return b;
}

uint8 TraceGifPrefix(uint32 *prefix, int32 code, int32 clearCode)
{
    int32 i = 0;
    while (code > clearCode && i++ <= LZ_MAX_CODE) code = prefix[code];

    return code;
}
#if RETRO_PLATFORM == RETRO_PSP
// Same pixels as the giflib-style decoder below for valid streams (including its code width growth), but with the state
// in locals, whole sub-blocks consumed at once, and each code's string written straight into place: the table keeps the
// prefix, first byte and length of every string, instead of re-walking prefix chains and bouncing bytes off a stack.
// Decoding sprite sheets was most of every scene load on the PSP.
#define PSP_GIF_PREFIX(entry) ((entry)&0xFFF)
#define PSP_GIF_FIRST(entry)  (((entry) >> 12) & 0xFF)
#define PSP_GIF_LENGTH(entry) ((entry) >> 20)

static void PSPReadGifPictureData(ImageGIF *image, int32 width, int32 height, bool32 interlaced, uint8 *pixels)
{
    static const int32 passStart[] = { 0, 4, 2, 1 };
    static const int32 passStep[]  = { 8, 8, 4, 2 };

    GifDecoder *decoder = image->decoder;
    FileInfo *info      = &image->info;
    uint32 *table       = decoder->prefix;
    uint8 *suffix       = decoder->suffix;
    uint8 *string       = decoder->stack; // strings that can't go straight into place are built here first
    uint8 *block        = decoder->buffer;

    // Pixels come out in file order. Without interlacing the rows are contiguous, so the whole image is one long line;
    // interlaced rows come from four passes.
    const int32 total     = width * height;
    const int32 lineWidth = interlaced ? width : total;
    int32 written         = 0;
    int32 x = 0, y = 0, pass = 0;
    uint8 *row = pixels;
#define PSP_GIF_PUT(value)                                                                                                                 \
    do {                                                                                                                                   \
        row[x] = (uint8)(value);                                                                                                           \
        ++written;                                                                                                                         \
        if (++x == lineWidth && interlaced) {                                                                                              \
            x = 0;                                                                                                                         \
            y += passStep[pass];                                                                                                           \
            while (y >= height && pass < 3) y = passStart[++pass];                                                                         \
            row = pixels + y * width;                                                                                                      \
        }                                                                                                                                  \
    } while (0)

    int32 depth = ReadInt8(info);
    if (depth >= 1 && depth <= 8) {
        const uint32 clearCode = 1 << depth;
        const uint32 eofCode   = clearCode + 1;
        for (uint32 c = 0; c < clearCode; ++c) table[c] = (1 << 20) | (c << 12) | c;

        uint32 runningCode    = eofCode + 1;
        uint32 runningBits    = depth + 1;
        uint32 maxCodePlusOne = 1 << runningBits;
        int32 prevCode        = -1;

        uint32 bits = 0, bitCount = 0;
        int32 blockPos = 0, blockSize = 0;

        while (written < total) {
            while (bitCount < runningBits) {
                if (blockPos == blockSize) {
                    blockSize = ReadInt8(info);
                    blockPos  = 0;
                    if (!blockSize)
                        break; // out of data
                    ReadBytes(info, block, blockSize);
                }
                bits |= (uint32)block[blockPos++] << bitCount;
                bitCount += 8;
            }
            if (bitCount < runningBits)
                break;

            uint32 code = bits & ((1 << runningBits) - 1);
            bits >>= runningBits;
            bitCount -= runningBits;
            if (++runningCode > maxCodePlusOne && runningBits < LZ_BITS) {
                maxCodePlusOne <<= 1;
                runningBits++;
            }

            if (code == clearCode) {
                runningCode    = eofCode + 1;
                runningBits    = depth + 1;
                maxCodePlusOne = 1 << runningBits;
                prevCode       = -1;
                continue;
            }
            if (code == eofCode)
                break;

            uint32 newCode = runningCode - 2; // the entry this code defines (from the second code after a clear)
            uint32 firstByte;
            if (code < clearCode) {
                firstByte = code;
                PSP_GIF_PUT(code);
            }
            else {
                if (prevCode < 0 || code > newCode)
                    break; // corrupt stream

                // code == newCode: the string being defined right now, prev's string plus its own first byte
                bool32 defining = code == newCode;
                uint32 entry    = table[defining ? prevCode : code];
                uint32 length   = PSP_GIF_LENGTH(entry) + (defining ? 1 : 0);
                firstByte       = PSP_GIF_FIRST(entry);

                bool32 inPlace = !interlaced && written + (int32)length <= total;
                uint8 *out     = (inPlace ? row + x : string) + length - 1;
                uint32 c       = code;
                if (defining) {
                    *out-- = (uint8)firstByte;
                    c      = prevCode;
                }
                while (c > eofCode) {
                    *out-- = suffix[c];
                    c      = PSP_GIF_PREFIX(table[c]);
                }
                *out = (uint8)c;

                if (inPlace) {
                    x += length;
                    written += length;
                }
                else {
                    for (uint32 i = 0; i < length && written < total; ++i) PSP_GIF_PUT(string[i]);
                }
            }

            if (prevCode >= 0 && newCode <= (uint32)LZ_MAX_CODE) {
                uint32 prevEntry = table[prevCode];
                suffix[newCode]  = (uint8)firstByte;
                table[newCode]   = ((PSP_GIF_LENGTH(prevEntry) + 1) << 20) | (prevEntry & 0xFF000) | (uint32)prevCode;
            }
            prevCode = code;
        }
    }

    // a short stream leaves the rest transparent, like the original decoder reading zeros past the end
    while (written < total) PSP_GIF_PUT(0);
#undef PSP_GIF_PUT
}
// Non-interlaced images (every sprite sheet) decode into one contiguous run of pixels, so each LZW code's string is
// already somewhere in the output: where its prefix string was written, one byte longer. Decoding a code is then a
// copy from earlier output instead of a walk down its prefix chain (one dependent load per pixel). Same pixels.
static void PSPReadGifPictureDataLinear(ImageGIF *image, int32 total, uint8 *pixels)
{
    static int32 codePos[LZ_MAX_CODE + 2];
    static uint16 codeLen[LZ_MAX_CODE + 2];

    GifDecoder *decoder = image->decoder;
    FileInfo *info      = &image->info;
    uint8 *block        = decoder->buffer;
    int32 written       = 0;

    int32 depth = ReadInt8(info);
    if (depth >= 1 && depth <= 8) {
        const uint32 clearCode = 1 << depth;
        const uint32 eofCode   = clearCode + 1;

        uint32 runningCode    = eofCode + 1;
        uint32 runningBits    = depth + 1;
        uint32 maxCodePlusOne = 1 << runningBits;
        int32 prevCode        = -1;
        int32 prevPos = 0, prevLen = 0;

        uint32 bits = 0, bitCount = 0;
        int32 blockPos = 0, blockSize = 0;

        while (written < total) {
            while (bitCount < runningBits) {
                if (blockPos == blockSize) {
                    blockSize = ReadInt8(info);
                    blockPos  = 0;
                    if (!blockSize)
                        break; // out of data
                    ReadBytes(info, block, blockSize);
                }
                bits |= (uint32)block[blockPos++] << bitCount;
                bitCount += 8;
            }
            if (bitCount < runningBits)
                break;

            uint32 code = bits & ((1 << runningBits) - 1);
            bits >>= runningBits;
            bitCount -= runningBits;
            if (++runningCode > maxCodePlusOne && runningBits < LZ_BITS) {
                maxCodePlusOne <<= 1;
                runningBits++;
            }

            if (code == clearCode) {
                runningCode    = eofCode + 1;
                runningBits    = depth + 1;
                maxCodePlusOne = 1 << runningBits;
                prevCode       = -1;
                continue;
            }
            if (code == eofCode)
                break;

            uint32 newCode = runningCode - 2; // the entry this code defines (from the second code after a clear)
            int32 start    = written;
            int32 length;
            if (code < clearCode) {
                pixels[written++] = (uint8)code;
                length            = 1;
            }
            else {
                if (prevCode < 0 || code > newCode)
                    break; // corrupt stream

                // code == newCode: the string being defined right now, prev's string plus its own first byte
                const uint8 *src;
                if (code == newCode) {
                    src    = &pixels[prevPos];
                    length = prevLen + 1;
                }
                else {
                    src    = &pixels[codePos[code]];
                    length = codeLen[code];
                }
                int32 copy = MIN(length, total - written);
                // prev's string then its first byte when code == newCode; the copy never overlaps its source (the
                // source ends where this string starts). Most strings are short: a call to memcpy costs more.
                int32 head = code == newCode ? MIN(prevLen, copy) : copy;
                uint8 *dst = &pixels[written];
                if (head <= 16) {
                    for (int32 i = 0; i < head; ++i) dst[i] = src[i];
                }
                else {
                    memcpy(dst, src, head);
                }
                if (copy > head)
                    dst[head] = src[0];
                written += copy;
            }

            if (prevCode >= 0 && newCode <= (uint32)LZ_MAX_CODE) {
                codePos[newCode] = prevPos;
                codeLen[newCode] = (uint16)(prevLen + 1);
            }
            prevCode = code;
            prevPos  = start;
            prevLen  = length;
        }
    }

    // a short stream leaves the rest transparent, like the original decoder reading zeros past the end
    if (written < total)
        memset(&pixels[written], 0, total - written);
}

#undef PSP_GIF_PREFIX
#undef PSP_GIF_FIRST
#undef PSP_GIF_LENGTH
#endif

void ReadGifPictureData(ImageGIF *image, int32 width, int32 height, bool32 interlaced, uint8 *pixels)
{
#if RETRO_PLATFORM == RETRO_PSP
#if PSP_GIF_VERIFY
    int32 dataStart = image->info.readPos;
#endif
    unsigned long long gifStart = PSPProfNow();
    if (!interlaced)
        PSPReadGifPictureDataLinear(image, width * height, pixels);
    else
        PSPReadGifPictureData(image, width, height, interlaced, pixels);
    pspLoadGifUS += PSPProfNow() - gifStart; // (includes the reads it does)
#if PSP_GIF_VERIFY
    // diagnostics: decode again with the original code, one row at a time, and compare
    static int32 verified = 0, mismatched = 0;
    Seek_Set(&image->info, dataStart);
    uint8 *line = (uint8 *)malloc(width ? width : 1);
    if (line) {
        static const int32 starts[] = { 0, 4, 2, 1 }, steps[] = { 8, 8, 4, 2 };
        int32 badRows = 0;
        InitGifDecoder(image);
        for (int32 p = interlaced ? 0 : 3; p < 4; ++p) {
            for (int32 y = interlaced ? starts[p] : 0; y < height; y += interlaced ? steps[p] : 1) {
                ReadGifLine(image, line, width, 0);
                badRows += memcmp(line, pixels + y * width, width) != 0;
            }
        }
        free(line);
        ++verified;
        if (badRows)
            ++mismatched;
        if (badRows || !(verified % 25))
            PrintLog(PRINT_NORMAL, "PSP gif verify: %dx%d interlaced=%d bad rows=%d (%d images, %d mismatched)", width, height, interlaced, badRows,
                     verified, mismatched);
    }
#endif
    return;
#endif
    int32 initialRows[] = { 0, 4, 2, 1 };
    int32 rowInc[]      = { 8, 8, 4, 2 };

    InitGifDecoder(image);
    if (interlaced) {
        for (int32 p = 0; p < 4; ++p) {
            for (int32 y = initialRows[p]; y < height; y += rowInc[p]) {
                ReadGifLine(image, pixels, width, y * width);
            }
        }
        return;
    }
    for (int32 h = 0; h < height; ++h) ReadGifLine(image, pixels, width, h * width);
}

#if RETRO_PLATFORM == RETRO_PSP
// Data.psp (tools/build_psp_data.py) stores every GIF already decoded:
//   0 'PSHT', 4 u16 version (1), 6 u16 width, 8 u16 height (where a GIF has them), 10 u32 packed size, 14 u16 0,
//   16 the palette (256 x RGB), 784 the pixels (width x height palette indices) as an LZ4 block.
// Unpacking LZ4 costs a fraction of LZW decoding, and the LZ4 data is smaller than the GIF was.
#define PSP_SHEET_SIGNATURE (0x54485350) // "PSHT"

// LZ4 block format; returns the bytes written, or -1 if the data doesn't fit in dst or is malformed
static int32 PSPLz4Decode(const uint8 *src, int32 srcSize, uint8 *dst, int32 dstSize)
{
    const uint8 *ip = src, *iend = src + srcSize;
    uint8 *op = dst, *oend = dst + dstSize;
    while (ip < iend) {
        uint32 token = *ip++;
        uint32 len   = token >> 4;
        if (len == 15) {
            uint8 b;
            do {
                if (ip >= iend)
                    return -1;
                b = *ip++;
                len += b;
            } while (b == 255);
        }
        if (len > (uint32)(iend - ip) || len > (uint32)(oend - op))
            return -1;
        memcpy(op, ip, len);
        op += len;
        ip += len;
        if (ip >= iend)
            break; // the last sequence is literals only

        if (iend - ip < 2)
            return -1;
        uint32 offset = ip[0] | (ip[1] << 8);
        ip += 2;
        if (!offset || offset > (uint32)(op - dst))
            return -1;

        len = token & 15;
        if (len == 15) {
            uint8 b;
            do {
                if (ip >= iend)
                    return -1;
                b = *ip++;
                len += b;
            } while (b == 255);
        }
        len += 4;
        if (len > (uint32)(oend - op))
            return -1;

        const uint8 *match = op - offset;
        if (offset >= len) {
            memcpy(op, match, len);
            op += len;
        }
        else if (offset == 1) {
            memset(op, *match, len); // runs of one colour: the bulk of a sprite sheet's empty space
            op += len;
        }
        else {
            while (len--) *op++ = *match++;
        }
    }
    return (int32)(op - dst);
}
#endif

bool32 ImageGIF::Load(const char *fileName, bool32 loadHeader)
{
    if (!decoder)
        return false;

    if (fileName) {
        if (!LoadFile(&info, fileName, FMODE_RB))
            return false;

#if RETRO_PLATFORM == RETRO_PSP
        pspSheet = ReadInt32(&info, false) == PSP_SHEET_SIGNATURE;
#endif
        Seek_Set(&info, 6);
        width  = ReadInt16(&info);
        height = ReadInt16(&info);

        if (loadHeader)
            return true;
    }

#if RETRO_PLATFORM == RETRO_PSP
    if (pspSheet) {
        unsigned long long start = PSPProfNow();
        Seek_Set(&info, 10);
        int32 packed = ReadInt32(&info, false);
        Seek_Set(&info, 16);

        if (!palette)
            AllocateStorage((void **)&palette, 0x100 * sizeof(int32), DATASET_TMP, true);
        if (!pixels)
            AllocateStorage((void **)&pixels, width * height, DATASET_TMP, false);

        uint8 *src = NULL;
        AllocateStorage((void **)&src, MAX(packed, 0x300), DATASET_TMP, false);

        int32 size = -1;
        if (palette && pixels && src) {
            ReadBytes(&info, src, 0x300);
            for (int32 c = 0; c < 0x100; ++c) palette[c] = (src[c * 3] << 16) | (src[c * 3 + 1] << 8) | (src[c * 3 + 2] << 0);

            ReadBytes(&info, src, packed);
            size = PSPLz4Decode(src, packed, pixels, width * height);
            if (size != width * height)
                PrintLog(PRINT_NORMAL, "PSP: bad sheet data (%d of %d pixels)", size, width * height);
        }
        RemoveStorageEntry((void **)&src);
        Close();
        pspLoadGifUS += PSPProfNow() - start;
        return size == width * height;
    }
#endif

    int32 data = ReadInt8(&info);
    // int32 has_pallete  = (data & 0x80) >> 7;
    // int32 colors       = ((data & 0x70) >> 4) + 1;
    int32 palette_size = (data & 0x7) + 1;
    if (palette_size > 0)
        palette_size = 1 << palette_size;

    Seek_Cur(&info, 2);

    if (!palette)
        AllocateStorage((void **)&palette, 0x100 * sizeof(int32), DATASET_TMP, true);

    if (!pixels)
        AllocateStorage((void **)&pixels, width * height, DATASET_TMP, false);

    if (palette && pixels) {
        uint8 clr[3];
        int32 c = 0;
        do {
            ReadBytes(&info, clr, 3);
            palette[c] = (clr[0] << 16) | (clr[1] << 8) | (clr[2] << 0);
            ++c;
        } while (c != palette_size);

        uint8 buf = ReadInt8(&info);
        while (buf != ',') buf = ReadInt8(&info); // gif image start identifier

        ReadInt16(&info);
        ReadInt16(&info);
        ReadInt16(&info);
        ReadInt16(&info);
        data              = ReadInt8(&info);
        bool32 interlaced = (data & 0x40) >> 6;
        if (data >> 7 == 1) {
            int32 c = 0x80;
            do {
                ++c;
                ReadBytes(&info, clr, 3);
                palette[c] = (clr[0] << 16) | (clr[1] << 8) | (clr[2] << 0);
            } while (c != 0x100);
        }

        ReadGifPictureData(this, width, height, interlaced, pixels);

        Close();
        return true;
    }
    return false;
}

#if RETRO_PLATFORM == RETRO_ANDROID
#define _REDOFF   0
#define _GREENOFF 8
#define _BLUEOFF  16
#else
#define _REDOFF   16
#define _GREENOFF 8
#define _BLUEOFF  0
#endif

#if RETRO_REV02
void RSDK::ImagePNG::UnpackPixels_Greyscale(uint8 *pixelData)
{
    uint8 *pixels = this->pixels;
    for (int32 p = 0; p < this->width * this->height; ++p) {
        uint8 brightness = *pixelData;
        pixelData++;

        uint32 color = 0;

        // red channel
        color = brightness << _REDOFF;

        // green channel
        color |= brightness << _GREENOFF;

        // blue channel
        color |= brightness << _BLUEOFF;

        // alpha channel
        color |= brightness << 24;

        *pixels = color;

        pixels += 2;
    }
}

void RSDK::ImagePNG::UnpackPixels_GreyscaleA(uint8 *pixelData)
{
    color *pixels = (color *)this->pixels;
    for (int32 p = 0; p < this->width * this->height; ++p) {
        uint8 brightness = *pixelData;
        pixelData++;

        uint32 color = 0;

        // red channel
        color = brightness << _REDOFF;

        // green channel
        color |= brightness << _GREENOFF;

        // blue channel
        color |= brightness << _BLUEOFF;

        // alpha channel
        color |= 0xFF << 24;

        *pixels = color;

        pixels++;
    }
}

void RSDK::ImagePNG::UnpackPixels_Indexed(uint8 *pixelData)
{
    color *pixels = (color *)this->pixels;
    for (int32 p = 0; p < this->width * this->height; ++p) {
        pixels[p] = palette[pixelData[p]] | 0xFF000000;
    }
}

void RSDK::ImagePNG::UnpackPixels_RGB(uint8 *pixelData)
{
    color *pixels = (color *)this->pixels;
    for (int32 p = 0; p < this->width * this->height; ++p) {
        uint32 color = 0;

        // R
        color = *pixelData << _REDOFF;
        pixelData++;

        // G
        color |= *pixelData << _GREENOFF;
        pixelData++;

        // B
        color |= *pixelData << _BLUEOFF;
        pixelData++;

        // A
        color |= 0xFF << 24;

        *pixels++ = color;
    }
}

void RSDK::ImagePNG::UnpackPixels_RGBA(uint8 *pixelData)
{
    color *pixels = (color *)this->pixels;
    for (int32 p = 0; p < this->width * this->height; ++p) {
        uint32 color = 0;

        // R
        color |= *pixelData << _REDOFF;
        pixelData++;

        // G
        color |= *pixelData << _GREENOFF;
        pixelData++;

        // B
        color |= *pixelData << _BLUEOFF;
        pixelData++;

        // A
        color |= *pixelData << 24;
        pixelData++;

        *pixels++ = color;
    }
}

// from: https://raw.githubusercontent.com/lvandeve/lodepng/master/lodepng.cpp - paethPredictor()
uint8 paethPredictor(int16 a, int16 b, int16 c)
{
    int16 pa = abs(b - c);
    int16 pb = abs(a - c);
    int16 pc = abs(a + b - c - c);
    /* return input value associated with smallest of pa, pb, pc (with certain priority if equal) */
    if (pb < pa) {
        a  = b;
        pa = pb;
    }

    return (pc < pa) ? c : a;
}

void RSDK::ImagePNG::Unfilter(uint8 *recon)
{
    int32 bpp = (this->bitDepth + 7) >> 3;
    switch (this->colorFormat) {
        default: break;

        case PNGCLR_RGB: bpp *= sizeof(color) - 1; break;

        case PNGCLR_GREYSCALEA: bpp *= 2 * sizeof(uint8); break;

        case PNGCLR_RGBA: bpp *= sizeof(color); break;
    }

    int32 pitch     = bpp * this->width;
    uint8 *scanline = recon;

    for (int32 y = 0; y < this->height; ++y) {
        int32 filter = *scanline++;

        // prev scanline
        uint8 *precon = y ? &recon[-pitch] : NULL;

        switch (filter) {
            default:
#if !RETRO_USE_ORIGINAL_CODE
                PrintLog(PRINT_NORMAL, "Invalid PNG Filter: %d", filter);
                return;
#else
                // [Fallthrough]
#endif
            case PNGFILTER_NONE:
                for (int32 c = 0; c < pitch; ++c) {
                    recon[c] = scanline[c];
                }
                break;

            case PNGFILTER_SUB:
                for (int32 c = 0; c < bpp; ++c) {
                    recon[c] = scanline[c];
                }

                for (int32 c = bpp, p = 0; c < pitch; ++c, ++p) {
                    recon[c] = scanline[c] + recon[p];
                }
                break;

            case PNGFILTER_UP:
                if (precon) {
                    for (int32 c = 0; c < pitch; ++c) {
                        recon[c] = precon[c] + scanline[c];
                    }
                }
                else {
                    for (int32 c = 0; c < pitch; ++c) {
                        recon[c] = scanline[c];
                    }
                }
                break;

            case PNGFILTER_AVG:
                if (precon) {
                    for (int32 c = 0; c < bpp; ++c) {
                        recon[c] = scanline[c] + (precon[c] >> 1);
                    }

                    for (int32 c = bpp, p = 0; c < pitch; ++c, ++p) {
                        recon[c] = scanline[c] + ((recon[p] + precon[c]) >> 1);
                    }
                }
                else {
                    for (int32 c = 0; c < bpp; ++c) {
                        recon[c] = scanline[c];
                    }

                    for (int32 c = bpp, p = 0; c < pitch; ++c, ++p) {
                        recon[c] = scanline[c] + (recon[p] >> 1);
                    }
                }
                break;

            case PNGFILTER_PAETH:
                if (precon) {
                    for (int32 c = 0; c < bpp; ++c) {
                        recon[c] = (scanline[c] + precon[c]);
                    }

                    for (int32 c = bpp, p = 0; c < pitch; ++c, ++p) {
                        recon[c] = (scanline[c] + paethPredictor(recon[c - bpp], precon[c], precon[p]));
                    }
                }
                else {
                    for (int32 c = 0; c < bpp; ++c) {
                        recon[c] = scanline[c];
                    }

                    for (int32 c = bpp, p = 0; c < pitch; ++c, ++p) {
                        recon[c] = scanline[c] + recon[p];
                    }
                }
                break;
        }

        recon += pitch;
        scanline += pitch;
    }
}

bool32 RSDK::ImagePNG::AllocatePixels()
{
    dataSize = sizeof(color) * height * (width + 1);
    if (!pixels) {
        AllocateStorage((void **)&pixels, dataSize, DATASET_TMP, false);

        if (!pixels) {
            Close();
            return false;
        }
    }

    return true;
}

void RSDK::ImagePNG::ProcessScanlines()
{
    uint8 *pixelsPtr = NULL;
    switch (colorFormat) {
        case PNGCLR_GREYSCALE:
        case PNGCLR_INDEXED: pixelsPtr = &pixels[3 * width * height]; break;

        case PNGCLR_RGB: pixelsPtr = &pixels[1 * width * height]; break;

        case PNGCLR_GREYSCALEA: pixelsPtr = &pixels[2 * width * height]; break;

        case PNGCLR_RGBA:
        default: pixelsPtr = &pixels[0 * width * height]; break;
    }

    // the original v5 decodes IDAT sections one at a time, so the compressed buffer is extracted into pixels every time here
    // again, this is a BAD idea!! IDAT chunks should be stored and processed as a group at the end of reading

    // decode all loaded IDAT chunks into pixels
    Uncompress((uint8 **)&chunkBuffer, chunkSize, (uint8 **)&pixelsPtr, dataSize);

    Unfilter(pixelsPtr);

    switch (colorFormat) {
        case PNGCLR_GREYSCALE: UnpackPixels_Greyscale(pixelsPtr); break;

        case PNGCLR_RGB: UnpackPixels_RGB(pixelsPtr); break;

        case PNGCLR_INDEXED: UnpackPixels_Indexed(pixelsPtr); break;

        case PNGCLR_GREYSCALEA: UnpackPixels_GreyscaleA(pixelsPtr); break;

        case PNGCLR_RGBA: UnpackPixels_RGBA(pixelsPtr); break;

        default: break;
    }
}

// PNG format signature
#define PNG_SIGNATURE 0xA1A0A0D474E5089LL // PNG (and other bytes I don't care about)

// PNG chunk header signatures
#define PNG_SIG_HEADER  0x52444849 // IHDR
#define PNG_SIG_END     0x444E4549 // IEND
#define PNG_SIG_PALETTE 0x45544C50 // PLTE
#define PNG_SIG_DATA    0x54414449 // IDAT

bool32 RSDK::ImagePNG::Load(const char *fileName, bool32 loadHeader)
{
    if (fileName) {
        if (LoadFile(&info, fileName, FMODE_RB)) {
            if (ReadInt64(&info) == PNG_SIGNATURE) {
                while (true) {
                    chunkSize   = ReadInt32(&info, true);
                    chunkHeader = ReadInt32(&info, false);

                    bool32 finished = false;
                    if (chunkHeader == PNG_SIG_HEADER && chunkSize == 13) {
                        width       = ReadInt32(&info, true);
                        height      = ReadInt32(&info, true);
                        bitDepth    = ReadInt8(&info);
                        colorFormat = ReadInt8(&info);
                        compression = ReadInt8(&info);
                        filter      = ReadInt8(&info);
                        interlaced  = ReadInt8(&info);
                        if (interlaced || bitDepth != 8) {
                            Close();
                            return false;
                        }
                        depth = 32;

#if !RETRO_USE_ORIGINAL_CODE
                        // image size should be enough space to hold all the IDAT chunks
                        AllocateStorage((void **)&chunkBuffer, sizeof(color) * height * (width + 1), DATASET_TMP, true);
                        dataSize = 0;
#endif

                        if (loadHeader)
                            return true;
                    }
                    else if (chunkHeader == PNG_SIG_END) {
                        finished = true;
                    }
                    else if (chunkHeader == PNG_SIG_PALETTE) {
                        int32 colorCnt = chunkSize / 3;
                        if (!(chunkSize % 3)) {
                            chunkSize = colorCnt;
                            if (colorCnt <= 0x100) {
                                if (!palette)
                                    AllocateStorage((void **)&palette, sizeof(uint32) * colorCnt, DATASET_TMP, true);

                                uint8 channels[3];
                                for (int32 c = 0; c < colorCnt; ++c) {
                                    ReadBytes(&info, channels, 3 * sizeof(uint8));
                                    palette[c] = (channels[0] << 16) | (channels[1] << 8) | (channels[2] << 0);
                                }
                            }
                        }
                    }
                    else if (chunkHeader == PNG_SIG_DATA) {
#if RETRO_USE_ORIGINAL_CODE
                        if (!AllocatePixels())
                            return false;

                        // read this chunk into the chunk buffer storage (we're processing each IDAT section by itself
                        // this is a BAD idea!!! though it's kept here for reference as to how the original v5 works
                        AllocateStorage((void **)&chunkBuffer, chunkSize, DATASET_TMP, false);
                        ReadBytes(&info, chunkBuffer, chunkSize);

                        // decode the scanlines into usable RGBA pixels
                        ProcessScanlines();
#else
                        // read this chunk into the chunk buffer storage (we process em all at the end)
                        dataSize += ReadBytes(&info, chunkBuffer + dataSize, chunkSize);
#endif
                    }
                    else {
                        Seek_Cur(&info, chunkSize);
                    }

                    chunkCRC = ReadInt32(&info, false);

                    if (finished) {
                        Close();

#if !RETRO_USE_ORIGINAL_CODE
                        // copy this over, since we only "borrowed" it after all :)
                        // chunkSize is the size of chunkBuffer
                        chunkSize = dataSize;
                        if (!AllocatePixels())
                            return false;

                        // decode the scanlines into usable RGBA pixels
                        ProcessScanlines();

                        RemoveStorageEntry((void **)&chunkBuffer);
#endif

                        return true;
                    }
                }
            }
            else {
                Close();
            }
        }
    }

    return false;
}
#endif

#if !RETRO_REV02
bool32 RSDK::ImageTGA::Load(const char *fileName, bool32 loadHeader)
{
    if (LoadFile(&info, fileName, FMODE_RB)) {
        // header
        uint8 idLength = ReadInt8(&info);

        // color map type
        uint8 colormaptype = ReadInt8(&info);

        // image type
        uint8 datatypecode = ReadInt8(&info);

        // color map specification
        int16 colormaporigin = ReadInt16(&info);
        int16 colormaplength = ReadInt16(&info);
        uint8 colormapdepth  = ReadInt8(&info);

        // image specification
        int16 originX    = ReadInt16(&info);
        int16 originY    = ReadInt16(&info);
        width            = ReadInt16(&info);
        height           = ReadInt16(&info);
        uint8 bpp        = ReadInt8(&info);
        uint8 descriptor = ReadInt8(&info);

        bool32 reverse = (~descriptor >> 4) & 1;
        if (bpp >= 16) {
            if (idLength)
                Seek_Cur(&info, idLength);

            AllocateStorage((void **)&pixels, sizeof(uint32) * height * width, DATASET_TMP, false);
            uint32 *pixelsPtr = (uint32 *)pixels;
            if (reverse)
                pixelsPtr += (height * width) - width;

            int32 x = 0;
            switch (datatypecode) {
                case 2: // Uncompressed, RGB images
                    switch (bpp) {
                        case 16:
                            for (int32 i = 0; i < height * width; ++i) {
                                uint8 channels[2];
                                ReadBytes(&info, channels, sizeof(uint16));

                                uint16 color16 = channels[0] + (channels[1] << 8);
                                *pixelsPtr     = 0;

                                if (color16 & 0x8000) { // alpha bit (0 = invisible, 1 = visible)
                                    uint32 R = (color16 >> 10) & 0x1F;
                                    uint32 G = (color16 >> 5) & 0x1F;
                                    uint32 B = (color16 >> 0) & 0x1F;

                                    R = (R << 3) | (R >> 2);
                                    G = (G << 3) | (G >> 2);
                                    B = (B << 3) | (B >> 2);

                                    *pixelsPtr = (R << 16) | (G << 8) | (B << 0);
                                }

                                pixelsPtr++;

                                if (reverse && ++x == width) {
                                    x = 0;
                                    pixelsPtr -= width << 1;
                                }
                            }
                            break;

                        case 24:
                            for (int32 i = 0; i < height * width; ++i) {
                                uint8 channels[3];
                                ReadBytes(&info, channels, sizeof(color) - 1);

                                *pixelsPtr = (channels[0] << 0) | (channels[1] << 8) | (channels[2] << 16) | (0xFF << 24);
                                pixelsPtr++;

                                if (reverse && ++x == width) {
                                    x = 0;
                                    pixelsPtr -= width << 1;
                                }
                            }
                            break;

                        case 32:
                            for (int32 i = 0; i < height * width; ++i) {
                                uint8 channels[4];
                                ReadBytes(&info, channels, sizeof(color));

                                *pixelsPtr = (channels[0] << 0) | (channels[1] << 8) | (channels[2] << 16) | (channels[3] << 24);
                                pixelsPtr++;

                                if (reverse && ++x == width) {
                                    x = 0;
                                    pixelsPtr -= width << 1;
                                }
                            }
                            break;
                    }
                    break;

                case 10: // Runlength encoded RGB images
                    switch (bpp) {
                        case 16: {
                            uint8 channels[2];
                            memset(channels, 0, sizeof(channels));

                            uint8 count        = 0;
                            bool32 decodingRLE = false;
                            for (int32 p = 0; p < height * width; ++p) {
                                if (count) {
                                    if (!decodingRLE)
                                        ReadBytes(&info, channels, sizeof(uint16));

                                    --count;
                                }
                                else {
                                    count       = ReadInt8(&info);
                                    decodingRLE = count & 0x80;
                                    count &= 0x7F;

                                    ReadBytes(&info, channels, sizeof(uint16));
                                }

                                uint16 color16 = channels[0] + (channels[1] << 8);
                                *pixelsPtr     = 0;

                                if (color16 & 0x8000) { // alpha bit (0 = invisible, 1 = visible)
                                    uint32 R = (color16 >> 10) & 0x1F;
                                    uint32 G = (color16 >> 5) & 0x1F;
                                    uint32 B = (color16 >> 0) & 0x1F;

                                    R = (R << 3) | (R >> 2);
                                    G = (G << 3) | (G >> 2);
                                    B = (B << 3) | (B >> 2);

                                    *pixelsPtr = (R << 16) | (G << 8) | (B << 0);
                                }

                                ++pixelsPtr;
                                if (reverse && ++x == width) {
                                    x = 0;
                                    pixelsPtr -= width << 1;
                                }
                            }
                            break;
                        }

                        case 24: {
                            uint8 channels[3];
                            memset(channels, 0, sizeof(channels));

                            uint8 count        = 0;
                            bool32 decodingRLE = false;
                            for (int32 p = 0; p < height * width; ++p) {
                                if (count) {
                                    if (!decodingRLE)
                                        ReadBytes(&info, channels, sizeof(color) - 1);

                                    --count;
                                }
                                else {
                                    count       = ReadInt8(&info);
                                    decodingRLE = count & 0x80;
                                    count &= 0x7F;

                                    ReadBytes(&info, channels, sizeof(color) - 1);
                                }

                                *pixelsPtr = (channels[0] << 0) | (channels[1] << 8) | (channels[2] << 16) | (0xFF << 24);
                                pixelsPtr++;

                                if (reverse && ++x == width) {
                                    x = 0;
                                    pixelsPtr -= width << 1;
                                }
                            }
                            break;
                        }

                        case 32: {
                            uint8 channels[sizeof(color)];
                            memset(channels, 0, sizeof(channels));

                            uint8 count        = 0;
                            bool32 decodingRLE = false;
                            for (int32 p = 0; p < height * width; ++p) {
                                if (count) {
                                    if (!decodingRLE)
                                        ReadBytes(&info, channels, sizeof(uint32));

                                    --count;
                                }
                                else {
                                    count       = ReadInt8(&info);
                                    decodingRLE = count & 0x80;
                                    count &= 0x7F;

                                    ReadBytes(&info, channels, sizeof(color));
                                }

                                *pixelsPtr = (channels[0] << 0) | (channels[1] << 8) | (channels[2] << 16) | (channels[3] << 24);
                                pixelsPtr++;

                                if (reverse && ++x == width) {
                                    x = 0;
                                    pixelsPtr -= width << 1;
                                }
                            }
                            break;
                        }
                    }
                    break;
            }

            Close();
            return true;
        }
    }

    return false;
}
#endif

uint16 RSDK::LoadSpriteSheet(const char *filename, uint8 scope)
{
#if RETRO_PLATFORM == RETRO_PSP
    PSPLoadTimer loadTimer(PSPLOAD_SHEET);
#endif
    char fullFilePath[0x100];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/Sprites/%s", filename);

    if (!scope || scope > SCOPE_STAGE)
        return -1;

    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5(filename, hash);

    for (int32 i = 0; i < SURFACE_COUNT; ++i) {
        if (HASH_MATCH_MD5(gfxSurface[i].hash, hash)) {
            return i;
        }
    }

    uint16 id = -1;
    for (id = 0; id < SURFACE_COUNT; ++id) {
        if (gfxSurface[id].scope == SCOPE_NONE)
            break;
    }

    if (id >= SURFACE_COUNT)
        return -1;


    GFXSurface *surface = &gfxSurface[id];
    ImageGIF image;

    if (image.Load(fullFilePath, true)) {
        surface->scope    = scope;
        surface->width    = image.width;
        surface->height   = image.height;
        surface->lineSize = 0;
        memcpy(surface->hash, hash, 4 * sizeof(int32));

        int32 w = surface->width;
        if (w > 1) {
            int32 ls = 0;
            do {
                w >>= 1;
                ++ls;
            } while (w > 1);
            surface->lineSize = ls;
        }

        surface->pixels = NULL;
        AllocateStorage((void **)&surface->pixels, surface->width * surface->height, DATASET_STG, false);
#if !RETRO_USE_ORIGINAL_CODE
        // Bug details: On a failed allocation, image.pixels will end up being reallocated in image.Load().
        // Pixel data would then be loaded in this temporary buffer, but surface->pixels would never point to the actual data.
        // This issue would only happen on cases where the STG mempool is full, such as ports with lower storage limits
        // or with mods that use a lot of spritesheets.
        // As a last resort, let's try a new allocation in TMP for surface->pixels.
        // NOTE: This is a workaround, and will still cause a crash if the TMP allocation fails as well.
        if (!surface->pixels) {
#if RETRO_PLATFORM == RETRO_PSP
            // (temporary storage is reused by later loads: the sheet's pixels get overwritten, its sprites vanish)
            PrintLog(PRINT_NORMAL, "PSP dbg stage storage full: sheet %s (%dx%d) went to temporary storage", fullFilePath, surface->width, surface->height);
#endif
            AllocateStorage((void **)&surface->pixels, surface->width * surface->height, DATASET_TMP, false);
        }
#endif
        image.pixels = surface->pixels;
        image.Load(NULL, false);

#if RETRO_USE_ORIGINAL_CODE
        image.palette = NULL;
        image.decoder = NULL;
#endif
        image.Close();

        return id;
    }
    else {
#if RETRO_USE_ORIGINAL_CODE
        image.palette = NULL;
        image.decoder = NULL;
#endif
        image.pixels  = NULL;
        image.Close();
        return -1;
    }
}

bool32 RSDK::LoadImage(const char *filename, double displayLength, double fadeSpeed, bool32 (*skipCallback)())
{
    char fullFilePath[0x100];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/Images/%s", filename);

#if RETRO_PLATFORM == RETRO_PSP
    // Data.psp holds these 1024x512 images as one JPEG frame (decoding the PNG would take 2MB of RAM the PSP lacks)
    if (PSPLoadImageFrame(fullFilePath)) {
        engine.displayTime        = displayLength;
        engine.storedShaderID     = videoSettings.shaderID;
        engine.storedState        = sceneInfo.state;
        videoSettings.dimMax      = 0.0;
        videoSettings.shaderID    = SHADER_RGB_IMAGE;
        videoSettings.screenCount = 0; // "Image Display Mode"
        engine.skipCallback       = skipCallback;
        sceneInfo.state           = ENGINESTATE_SHOWIMAGE;
        engine.imageFadeSpeed     = fadeSpeed / 60.0;
        return true;
    }
#endif

#if RETRO_REV02
    ImagePNG image;
#else
    ImageTGA image;
#endif
    InitFileInfo(&image.info);

#if RETRO_REV02
    if (image.Load(fullFilePath, false)) {
        if (image.width == RETRO_VIDEO_TEXTURE_W && image.height == RETRO_VIDEO_TEXTURE_H) {
            RenderDevice::SetupImageTexture(image.width, image.height, image.pixels);
        }
#if !RETRO_USE_ORIGINAL_CODE
        else {
            PrintLog(PRINT_NORMAL, "ERROR: Images must be 1024x512!");
        }
#endif

        engine.displayTime        = displayLength;
        engine.storedShaderID     = videoSettings.shaderID;
        engine.storedState        = sceneInfo.state;
        videoSettings.dimMax      = 0.0;
        videoSettings.shaderID    = SHADER_RGB_IMAGE;
        videoSettings.screenCount = 0; // "Image Display Mode"
        engine.skipCallback       = skipCallback;
        sceneInfo.state           = ENGINESTATE_SHOWIMAGE;
        engine.imageFadeSpeed     = fadeSpeed / 60.0;

        image.pixels = NULL;
        image.Close();
        return true;
    }
#elif !RETRO_REV02
    if (image.Load(fullFilePath, true)) {
        if (image.width == RETRO_VIDEO_TEXTURE_W && image.height == RETRO_VIDEO_TEXTURE_H) {
            RenderDevice::SetupImageTexture(image.width, image.height, image.pixels);
        }
#if !RETRO_USE_ORIGINAL_CODE
        else {
            PrintLog(PRINT_NORMAL, "ERROR: Images must be 1024x512!");
        }
#endif

        engine.displayTime        = displayLength;
        engine.storedShaderID     = videoSettings.shaderID;
        engine.storedState        = sceneInfo.state;
        videoSettings.dimMax      = 0.0;
        videoSettings.shaderID    = SHADER_RGB_IMAGE;
        videoSettings.screenCount = 0; // "Image Display Mode"
        engine.skipCallback       = skipCallback;
        sceneInfo.state           = ENGINESTATE_SHOWIMAGE;
        engine.imageFadeSpeed     = fadeSpeed / 60.0;

        image.pixels = NULL;
        image.Close();
        return true;
    }
#endif
    else {
        image.pixels = NULL;
        image.Close();
    }
    return false;
}
