// Textured candidate: baseline traversal, scheduled opaque pixels and transparent-half skips.
// The v7 solid-row renderer remains a separate candidate.
static void PSPDrawLayerHScrollTextured(TileLayer *layer)
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
        const uint16 *activePalette = fullPalette[*lineBuffer++];

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
                    PSPDrawScheduledOpaqueTileRow(frameBuffer, pixels, activePalette);
                }
                else if (!(pspTileRowEmpty[slot] & rowBit)) { // fully transparent rows draw nothing
#endif
                PSPDrawMixedTileRow(frameBuffer, pixels, activePalette);
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
