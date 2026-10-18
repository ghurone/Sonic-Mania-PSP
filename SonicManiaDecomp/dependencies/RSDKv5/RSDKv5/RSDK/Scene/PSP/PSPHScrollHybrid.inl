// v8 solid traversal; retain uniform fills, schedule only detailed opaque rows.
static void PSPDrawLayerHScrollHybrid(TileLayer *layer)
{
    if (!layer->xsize || !layer->ysize)
        return;

    // Framebuffer writes cannot alter the screen/layer configuration. Keep
    // these invariants local instead of reloading them in the tile loop.
    const int32 pitch       = currentScreen->pitch;
    const int32 clipTop     = currentScreen->clipBound_Y1;
    const int32 clipBottom  = currentScreen->clipBound_Y2;
    const int32 layerWidth  = layer->xsize;
    const int32 widthShift  = layer->widthShift;
    const uint16 *layoutBase = layer->layout;
    int32 lineTileCount     = (pitch >> 4) - 1;
    uint8 *lineBuffer       = &gfxLineBuffer[clipTop];
    ScanlineInfo *scanline  = &scanlines[clipTop];
    uint16 *frameBuffer = &currentScreen->frameBuffer[pitch * clipTop];

    for (int32 cy = clipTop; cy < clipBottom; ++cy) {
        int32 x               = scanline->position.x;
        int32 y               = scanline->position.y;
        int32 tileX           = FROM_FIXED(x);
        const uint16 *activePalette = fullPalette[*lineBuffer++];

        if (tileX >= TILE_SIZE * layerWidth)
            x = TO_FIXED(tileX - TILE_SIZE * layerWidth);
        else if (tileX < 0)
            x = TO_FIXED(tileX + TILE_SIZE * layerWidth);

        int32 tileRemain = TILE_SIZE - (FROM_FIXED(x) & 0xF);
        int32 sheetX     = FROM_FIXED(x) & 0xF;
        int32 sheetY     = TILE_SIZE * (FROM_FIXED(y) & 0xF);
#if RETRO_PLATFORM == RETRO_PSP
        uint16 rowBit    = 1 << (sheetY >> 4); // this scanline's row within every tile
#endif
        int32 lineRemain = pitch;

        int32 tx       = x >> 20;
        const uint16 *layout = &layoutBase[tx + ((y >> 20) << widthShift)];
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

            if (++tx == layerWidth) {
                tx = 0;
                layout -= layerWidth;
            }

            if (*layout < 0xFFFF) {
                uint8 *pixels = &tilesetPixels[TILE_DATASIZE * (*layout & 0xFFF) + sheetY];

#if RETRO_PLATFORM == RETRO_PSP
                uint16 slot = *layout & 0xFFF;
                if (pspTileRowOpaque[slot] & rowBit) { // no transparent pixel in this row: no per-pixel test
#if PSP_HSCROLL_OPT
                    if (pspTileRowSolid[slot] & rowBit)
                        PSPFillSolidTileRow(frameBuffer, activePalette[pixels[0]]);
                    else
                        PSPDrawScheduledOpaqueTileRow(frameBuffer, pixels, activePalette);
#else
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
#endif
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

            if (++tx == layerWidth) {
                tx = 0;
                layout -= layerWidth;
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
