// Reuse a map lookup for four consecutive samples inside the same source tile.
// Only bounded steps use the grouped path; large steps retain scalar sampling.
static void PSPDrawLayerRotozoomGroups(TileLayer *layer)
{
    if (!layer->xsize || !layer->ysize)
        return;
    const int32 top = currentScreen->clipBound_Y1, bottom = currentScreen->clipBound_Y2;
    const int32 lineSize = currentScreen->clipBound_X2 - currentScreen->clipBound_X1;
    if (lineSize <= 0 || top >= bottom)
        return;
    const int32 pitch = currentScreen->pitch, shift = layer->widthShift;
    const uint32 maskX = (1U << layer->widthShift) - 1;
    const uint32 maskY = (1U << layer->heightShift) - 1;
    const uint16 *layout = layer->layout;
    ScanlineInfo *scanline = &scanlines[top];
    uint16 *frameBuffer = &currentScreen->frameBuffer[currentScreen->clipBound_X1 + top * pitch];
    for (int32 cy = top; cy < bottom; ++cy, ++scanline) {
        uint32 px = (uint32)scanline->position.x, py = (uint32)scanline->position.y;
        const uint32 dx = (uint32)scanline->deform.x, dy = (uint32)scanline->deform.y;
        const uint16 *palette = fullPalette[gfxLineBuffer[cy]];
        const bool bounded = scanline->deform.x >= -0x100000 && scanline->deform.x <= 0x100000
                          && scanline->deform.y >= -0x100000 && scanline->deform.y <= 0x100000;
        int32 cx = 0;
        if (bounded) {
            const uint32 deltaX = dx * 3, deltaY = dy * 3;
            for (; cx + 4 <= lineSize; cx += 4) {
                const uint32 endX = px + deltaX, endY = py + deltaY;
                if (!(((px ^ endX) | (py ^ endY)) & 0xFFF00000U)) {
                    const uint32 tile = layout[((px >> 20) & maskX) + (((py >> 20) & maskY) << shift)] & 0xFFF;
                    const uint8 *pixels = &tilesetPixels[tile << 8];
                    // Unsigned arithmetic preserves the engine's fixed-point
                    // wrapping, including negative scroll and 32-bit wrap.
#define PSP_ROTO_SAMPLE(i) do { \
    uint8 index = pixels[((py >> 12) & 0xF0) + ((px >> 16) & 15)]; \
    if (index) frameBuffer[i] = palette[index]; \
    px += dx; py += dy; \
} while (0)
                    PSP_ROTO_SAMPLE(0); PSP_ROTO_SAMPLE(1);
                    PSP_ROTO_SAMPLE(2); PSP_ROTO_SAMPLE(3);
#undef PSP_ROTO_SAMPLE
                }
                else {
                    for (int32 i = 0; i < 4; ++i) {
                        const uint32 tile = layout[((px >> 20) & maskX) + (((py >> 20) & maskY) << shift)] & 0xFFF;
                        uint8 index = tilesetPixels[(tile << 8) + ((py >> 12) & 0xF0) + ((px >> 16) & 15)];
                        if (index) frameBuffer[i] = palette[index];
                        px += dx; py += dy;
                    }
                }
                frameBuffer += 4;
            }
        }
        for (; cx < lineSize; ++cx) {
            const uint32 tile = layout[((px >> 20) & maskX) + (((py >> 20) & maskY) << shift)] & 0xFFF;
            uint8 index = tilesetPixels[(tile << 8) + ((py >> 12) & 0xF0) + ((px >> 16) & 15)];
            if (index) *frameBuffer = palette[index];
            px += dx; py += dy;
            ++frameBuffer;
        }
        frameBuffer += pitch - lineSize;
    }
}
