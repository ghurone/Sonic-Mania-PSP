// The same samples as PSPDrawLayerRotozoomGroups, walked tile run by tile run (PSPRotoAxis): one layout lookup per run
// of pixels inside a cell, and runs over fully transparent tiles (most of the special stages' playfield) skipped whole.
int pspRotoCpuMode = 0; // 1: PSPDrawLayerRotozoomGroups instead (rotocheck compares the two), 2: only walk
uint32 pspRotoRuns, pspRotoDrawn; // (rotocheck)

static void PSPDrawLayerRotozoomRuns(TileLayer *layer)
{
    if (pspRotoCpuMode == 1) {
        PSPDrawLayerRotozoomGroups(layer);
        return;
    }
    const bool walkOnly = pspRotoCpuMode == 2; // (rotocheck: the walk's own cost)
    uint32 runs = 0, drawn = 0;
    if (!layer->xsize || !layer->ysize)
        return;
    const int32 top = currentScreen->clipBound_Y1, bottom = currentScreen->clipBound_Y2;
    const uint32 lineSize = currentScreen->clipBound_X2 - currentScreen->clipBound_X1;
    if ((int32)lineSize <= 0 || top >= bottom)
        return;
    const int32 pitch = currentScreen->pitch, shift = layer->widthShift;
    const uint32 maskX = (1U << layer->widthShift) - 1;
    const uint32 maskY = (1U << layer->heightShift) - 1;
    const uint16 *layout = layer->layout;
    ScanlineInfo *scanline = &scanlines[top];
    uint16 *rowBuffer = &currentScreen->frameBuffer[currentScreen->clipBound_X1 + top * pitch];
    for (int32 cy = top; cy < bottom; ++cy, ++scanline, rowBuffer += pitch) {
        const uint32 px = (uint32)scanline->position.x, py = (uint32)scanline->position.y;
        const uint32 dx = (uint32)scanline->deform.x, dy = (uint32)scanline->deform.y;
        const uint16 *palette = fullPalette[gfxLineBuffer[cy]];

        PSPRotoAxis axisX, axisY;
        axisX.Init(px, (int32)dx);
        axisY.Init(py, (int32)dy);
        uint32 tx = (px >> 20) & maskX, ty = (py >> 20) & maskY;
        for (uint32 k = 0; k < lineSize;) {
            uint32 end = MIN(MIN(axisX.next, axisY.next), lineSize);
            const uint32 tile = layout[tx + (ty << shift)] & 0xFFF;
            ++runs;
            if (pspTileRowEmpty[tile] != 0xFFFF && !walkOnly) {
                drawn += end - k;
                const uint8 *pixels = &tilesetPixels[tile << 8];
                uint32 x = px + k * dx, y = py + k * dy;
                uint16 *frameBuffer = &rowBuffer[k];
                for (uint32 n = end - k; n; --n) {
                    uint8 index = pixels[((y >> 12) & 0xF0) + ((x >> 16) & 15)];
                    if (index)
                        *frameBuffer = palette[index];
                    ++frameBuffer;
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
    }
    pspRotoRuns += runs, pspRotoDrawn += drawn;
}
