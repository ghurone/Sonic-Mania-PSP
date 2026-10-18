// Frozen pre-v6 HScroll, compiled only for the v7 hardware comparison.
// Keep identical to psp/backups/tiles-v6-baseline/Scene.cpp.
static void PSPDrawLayerHScrollReference(TileLayer *layer)
{
    if (!layer->xsize || !layer->ysize)
        return;

    int32 lineTileCount    = (currentScreen->pitch >> 4) - 1;
    uint8 *lineBuffer      = &gfxLineBuffer[currentScreen->clipBound_Y1];
    ScanlineInfo *scanline = &scanlines[currentScreen->clipBound_Y1];
    uint16 *frameBuffer    = &currentScreen->frameBuffer[currentScreen->pitch * currentScreen->clipBound_Y1];

    for (int32 cy = currentScreen->clipBound_Y1; cy < currentScreen->clipBound_Y2; ++cy) {
        int32 x               = scanline->position.x;
        int32 y               = scanline->position.y;
        int32 tileX           = FROM_FIXED(x);
        uint16 *activePalette = fullPalette[*lineBuffer++];

        if (tileX >= TILE_SIZE * layer->xsize)
            x = TO_FIXED(tileX - TILE_SIZE * layer->xsize);
        else if (tileX < 0)
            x = TO_FIXED(tileX + TILE_SIZE * layer->xsize);

        int32 tileRemain = TILE_SIZE - (FROM_FIXED(x) & 0xF);
        int32 sheetX     = FROM_FIXED(x) & 0xF;
        int32 sheetY     = TILE_SIZE * (FROM_FIXED(y) & 0xF);
#if RETRO_PLATFORM == RETRO_PSP
        uint16 rowBit    = 1 << (sheetY >> 4); // this scanline's row within every tile
#endif
        int32 lineRemain = currentScreen->pitch;

        int32 tx       = x >> 20;
        uint16 *layout = &layer->layout[tx + ((y >> 20) << layer->widthShift)];
        lineRemain -= tileRemain;

        if (*layout >= 0xFFFF) {
            frameBuffer += tileRemain;
        }
        else {
            uint8 *pixels = &tilesetPixels[TILE_DATASIZE * (*layout & 0xFFF) + sheetY + sheetX];
            for (int32 x = 0; x < tileRemain; ++x) {
                if (*pixels)
                    *frameBuffer = activePalette[*pixels];
                ++pixels;
                ++frameBuffer;
            }
        }

        for (int32 l = 0; l < lineTileCount; ++l) {
            ++layout;

            if (++tx == layer->xsize) {
                tx = 0;
                layout -= layer->xsize;
            }

            if (*layout < 0xFFFF) {
                uint8 *pixels = &tilesetPixels[TILE_DATASIZE * (*layout & 0xFFF) + sheetY];

#if RETRO_PLATFORM == RETRO_PSP
                uint16 slot = *layout & 0xFFF;
                if (pspTileRowOpaque[slot] & rowBit) { // no transparent pixel in this row: no per-pixel test
                    frameBuffer[0] = activePalette[pixels[0]];
                    frameBuffer[1] = activePalette[pixels[1]];
                    frameBuffer[2] = activePalette[pixels[2]];
                    frameBuffer[3] = activePalette[pixels[3]];
                    frameBuffer[4] = activePalette[pixels[4]];
                    frameBuffer[5] = activePalette[pixels[5]];
                    frameBuffer[6] = activePalette[pixels[6]];
                    frameBuffer[7] = activePalette[pixels[7]];
                    frameBuffer[8] = activePalette[pixels[8]];
                    frameBuffer[9] = activePalette[pixels[9]];
                    frameBuffer[10] = activePalette[pixels[10]];
                    frameBuffer[11] = activePalette[pixels[11]];
                    frameBuffer[12] = activePalette[pixels[12]];
                    frameBuffer[13] = activePalette[pixels[13]];
                    frameBuffer[14] = activePalette[pixels[14]];
                    frameBuffer[15] = activePalette[pixels[15]];
                }
                else if (!(pspTileRowEmpty[slot] & rowBit)) { // fully transparent rows draw nothing
#endif
                uint8 index = *pixels;
                if (index)
                    *frameBuffer = activePalette[index];

                index = pixels[1];
                if (index)
                    frameBuffer[1] = activePalette[index];

                index = pixels[2];
                if (index)
                    frameBuffer[2] = activePalette[index];

                index = pixels[3];
                if (index)
                    frameBuffer[3] = activePalette[index];

                index = pixels[4];
                if (index)
                    frameBuffer[4] = activePalette[index];

                index = pixels[5];
                if (index)
                    frameBuffer[5] = activePalette[index];

                index = pixels[6];
                if (index)
                    frameBuffer[6] = activePalette[index];

                index = pixels[7];
                if (index)
                    frameBuffer[7] = activePalette[index];

                index = pixels[8];
                if (index)
                    frameBuffer[8] = activePalette[index];

                index = pixels[9];
                if (index)
                    frameBuffer[9] = activePalette[index];

                index = pixels[10];
                if (index)
                    frameBuffer[10] = activePalette[index];

                index = pixels[11];
                if (index)
                    frameBuffer[11] = activePalette[index];

                index = pixels[12];
                if (index)
                    frameBuffer[12] = activePalette[index];

                index = pixels[13];
                if (index)
                    frameBuffer[13] = activePalette[index];

                index = pixels[14];
                if (index)
                    frameBuffer[14] = activePalette[index];

                index = pixels[15];
                if (index)
                    frameBuffer[15] = activePalette[index];
#if RETRO_PLATFORM == RETRO_PSP
                }
#endif
            }

            frameBuffer += TILE_SIZE;
            lineRemain -= TILE_SIZE;
        }

        while (lineRemain > 0) {
            ++layout;

            if (++tx == layer->xsize) {
                tx = 0;
                layout -= layer->xsize;
            }

            tileRemain = lineRemain >= TILE_SIZE ? TILE_SIZE : lineRemain;

            if (*layout >= 0xFFFF) {
                frameBuffer += tileRemain;
            }
            else {
                uint8 *pixels = &tilesetPixels[TILE_DATASIZE * (*layout & 0xFFF) + sheetY];
                for (int32 x = 0; x < tileRemain; ++x) {
                    if (*pixels)
                        *frameBuffer = activePalette[*pixels];
                    ++pixels;
                    ++frameBuffer;
                }
            }

            lineRemain -= TILE_SIZE;
        }

        ++scanline;
    }
}
