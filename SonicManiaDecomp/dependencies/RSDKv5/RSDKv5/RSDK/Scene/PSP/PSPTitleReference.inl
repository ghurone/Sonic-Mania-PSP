// Frozen v8 drawing routines for comparisons on identical framebuffers.
static void PSPDrawLayerBasicReference(TileLayer *layer)
{
    if (!layer->xsize || !layer->ysize)
        return;

    if (currentScreen->clipBound_X1 >= currentScreen->clipBound_X2 || currentScreen->clipBound_Y1 >= currentScreen->clipBound_Y2)
        return;

    uint16 *activePalette = fullPalette[0];
    if (currentScreen->clipBound_X1 < currentScreen->clipBound_X2 && currentScreen->clipBound_Y1 < currentScreen->clipBound_Y2) {
        int32 lineSize = (currentScreen->clipBound_X2 - currentScreen->clipBound_X1) >> 4;

        ScanlineInfo *scanline = &scanlines[currentScreen->clipBound_Y1];

        int32 tx          = (currentScreen->clipBound_X1 + FROM_FIXED(scanline->position.x)) >> 4;
        int32 ty          = FROM_FIXED(scanline->position.y) >> 4;
        int32 sheetY      = FROM_FIXED(scanline->position.y) & 0xF;
        int32 sheetX      = (currentScreen->clipBound_X1 + FROM_FIXED(scanline->position.x)) & 0xF;
        int32 tileRemainX = TILE_SIZE - sheetX;
        int32 tileRemainY = TILE_SIZE - sheetY;

        uint16 *frameBuffer = &currentScreen->frameBuffer[currentScreen->clipBound_X1 + currentScreen->clipBound_Y1 * currentScreen->pitch];
        uint16 *layout      = &layer->layout[tx + (ty << layer->widthShift)];

        // Remaining pixels on top
        {
            if (*layout == 0xFFFF) {
                frameBuffer += TILE_SIZE - sheetX;
            }
            else {
                uint8 *pixels = &tilesetPixels[TILE_DATASIZE * (*layout & 0xFFF) + TILE_SIZE * sheetY + sheetX];

                for (int32 y = 0; y < tileRemainY; ++y) {
                    for (int32 x = 0; x < tileRemainX; ++x) {
                        if (*pixels)
                            *frameBuffer = activePalette[*pixels];
                        ++pixels;
                        ++frameBuffer;
                    }

                    pixels += sheetX;
                    frameBuffer += currentScreen->pitch - tileRemainX;
                }

                frameBuffer += tileRemainX - currentScreen->pitch * tileRemainY;
            }

            ++layout;
            if (++tx == layer->xsize) {
                tx = 0;
                layout -= layer->xsize;
            }

            for (int32 x = 0; x < lineSize; ++x) {
                if (*layout == 0xFFFF) {
                    frameBuffer += TILE_SIZE;
                }
                else {
                    uint8 *pixels = &tilesetPixels[TILE_DATASIZE * (*layout & 0xFFF) + TILE_SIZE * sheetY];
                    for (int32 y = 0; y < tileRemainY; ++y) {
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

                        frameBuffer += currentScreen->pitch;
                        pixels += TILE_SIZE;
                    }

                    frameBuffer += TILE_SIZE - currentScreen->pitch * tileRemainY;
                }

                ++layout;
                if (++tx == layer->xsize) {
                    tx = 0;
                    layout -= layer->xsize;
                }
            }

            if (*layout == 0xFFFF) {
                frameBuffer += currentScreen->pitch * tileRemainY;
            }
            else {
                uint8 *pixels = &tilesetPixels[TILE_DATASIZE * (*layout & 0xFFF) + TILE_SIZE * sheetY];

                for (int32 y = 0; y < tileRemainY; ++y) {
                    for (int32 x = 0; x < sheetX; ++x) {
                        if (*pixels)
                            *frameBuffer = activePalette[*pixels];
                        ++pixels;
                        ++frameBuffer;
                    }

                    pixels += tileRemainX;
                    frameBuffer += currentScreen->pitch - sheetX;
                }
            }
        }

        // We've drawn a single line of pixels, increase our variables
        frameBuffer += sheetX + -TILE_SIZE * lineSize - TILE_SIZE;
        scanline += tileRemainY;
        if (++ty == layer->ysize)
            ty = 0;

        // Draw the bulk of the tiles
        int32 lineTileCount = ((currentScreen->clipBound_Y2 - currentScreen->clipBound_Y1) >> 4) - 1;
        for (int32 l = 0; l < lineTileCount; ++l) {
            sheetX      = (currentScreen->clipBound_X1 + FROM_FIXED(scanline->position.x)) & 0xF;
            tx          = (currentScreen->clipBound_X1 + FROM_FIXED(scanline->position.x)) >> 4;
            tileRemainX = TILE_SIZE - sheetX;
            layout      = &layer->layout[tx + (ty << layer->widthShift)];

            // Draw any stray pixels on the left
            if (*layout == 0xFFFF) {
                frameBuffer += tileRemainX;
            }
            else {
                uint8 *pixels = &tilesetPixels[TILE_DATASIZE * (*layout & 0xFFF) + sheetX];

                for (int32 y = 0; y < TILE_SIZE; ++y) {
                    for (int32 x = 0; x < tileRemainX; ++x) {
                        if (*pixels)
                            *frameBuffer = activePalette[*pixels];
                        ++pixels;
                        ++frameBuffer;
                    }

                    pixels += sheetX;
                    frameBuffer += currentScreen->pitch - tileRemainX;
                }

                frameBuffer += tileRemainX - TILE_SIZE * currentScreen->pitch;
            }
            ++layout;
            if (++tx == layer->xsize) {
                tx = 0;
                layout -= layer->xsize;
            }

            // Draw the bulk of the tiles on this line
            for (int32 x = 0; x < lineSize; ++x) {
                if (*layout == 0xFFFF) {
                    frameBuffer += TILE_SIZE;
                }
                else {
                    uint8 *pixels = &tilesetPixels[TILE_DATASIZE * (*layout & 0xFFF)];

                    for (int32 y = 0; y < TILE_SIZE; ++y) {
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

                        pixels += TILE_SIZE;
                        frameBuffer += currentScreen->pitch;
                    }

                    frameBuffer -= TILE_SIZE * currentScreen->pitch;
                    frameBuffer += TILE_SIZE;
                }

                ++layout;
                if (++tx == layer->xsize) {
                    tx = 0;
                    layout -= layer->xsize;
                }
            }

            // Draw any stray pixels on the right
            if (*layout == 0xFFFF) {
                frameBuffer += TILE_SIZE * currentScreen->pitch;
            }
            else {
                uint8 *pixels = &tilesetPixels[TILE_DATASIZE * (*layout & 0xFFF)];

                for (int32 y = 0; y < TILE_SIZE; ++y) {
                    for (int32 x = 0; x < sheetX; ++x) {
                        if (*pixels)
                            *frameBuffer = activePalette[*pixels];
                        ++pixels;
                        ++frameBuffer;
                    }

                    pixels += tileRemainX;
                    frameBuffer += currentScreen->pitch - sheetX;
                }
            }
            ++layout;
            if (++tx == layer->xsize) {
                tx = 0;
                layout -= layer->xsize;
            }

            // We've drawn a single line, increase our variables
            scanline += TILE_SIZE;
            frameBuffer += sheetX + -TILE_SIZE * lineSize - TILE_SIZE;
            if (++ty == layer->ysize)
                ty = 0;
        }

        // Remaining pixels on bottom
        {
            tx          = (currentScreen->clipBound_X1 + FROM_FIXED(scanline->position.x)) >> 4;
            sheetX      = (currentScreen->clipBound_X1 + FROM_FIXED(scanline->position.x)) & 0xF;
            tileRemainX = TILE_SIZE - sheetX;
            layout      = &layer->layout[tx + (ty << layer->widthShift)];

            if (*layout != 0xFFFF) {
                frameBuffer += tileRemainX;
            }
            else {
                uint8 *pixels = &tilesetPixels[TILE_DATASIZE * (*layout & 0xFFF) + sheetX];

                for (int32 y = 0; y < sheetY; ++y) {
                    for (int32 x = 0; x < tileRemainX; ++x) {
                        if (*pixels)
                            *frameBuffer = activePalette[*pixels];
                        ++pixels;
                        ++frameBuffer;
                    }

                    pixels += sheetX;
                    frameBuffer += currentScreen->pitch - tileRemainX;
                }

                frameBuffer += tileRemainX - currentScreen->pitch * sheetY;
            }
            ++layout;
            if (++tx == layer->xsize) {
                tx = 0;
                layout -= layer->xsize;
            }

            for (int32 x = 0; x < lineSize; ++x) {
                if (*layout == 0xFFFF) {
                    frameBuffer += TILE_SIZE;
                }
                else {
                    uint8 *pixels = &tilesetPixels[TILE_DATASIZE * (*layout & 0xFFF)];
                    for (int32 y = 0; y < sheetY; ++y) {
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

                        pixels += TILE_SIZE;
                        frameBuffer += currentScreen->pitch;
                    }

                    frameBuffer += TILE_SIZE - currentScreen->pitch * sheetY;
                }

                ++layout;
                if (++tx == layer->xsize) {
                    tx = 0;
                    layout -= layer->xsize;
                }
            }

            if (*layout != 0xFFFF) {
                uint8 *pixels = &tilesetPixels[256 * (*layout & 0xFFF)];

                for (int32 y = 0; y < sheetY; ++y) {
                    for (int32 x = 0; x < sheetX; ++x) {
                        if (*pixels)
                            *frameBuffer = activePalette[*pixels];
                        ++pixels;
                        ++frameBuffer;
                    }

                    pixels += tileRemainX;
                }

                frameBuffer += currentScreen->pitch - sheetX;
            }
        }
    }
}
static void PSPDrawLayerRotozoomReference(TileLayer *layer)
{
    if (!layer->xsize || !layer->ysize)
        return;

    uint16 *layout         = layer->layout;
    uint8 *lineBuffer      = &gfxLineBuffer[currentScreen->clipBound_Y1];
    ScanlineInfo *scanline = &scanlines[currentScreen->clipBound_Y1];
    uint16 *frameBuffer    = &currentScreen->frameBuffer[currentScreen->clipBound_X1 + currentScreen->clipBound_Y1 * currentScreen->pitch];

    int32 width    = (TILE_SIZE << layer->widthShift) - 1;
    int32 height   = (TILE_SIZE << layer->heightShift) - 1;
    int32 lineSize = currentScreen->clipBound_X2 - currentScreen->clipBound_X1;

#if RETRO_PLATFORM == RETRO_PSP
    // Same pixels as the loop below. That loop reloads scanline->deform and layer->widthShift for every pixel
    // (the framebuffer stores may alias them under -fno-strict-aliasing) and multiplies to index the tileset;
    // on the PSP this layer type (the title screen's clouds/island) costs ~10ms a frame. Keep it all in registers:
    // TILE_SIZE * (y + TILE_SIZE * tile) + x == (tile << 8) + ((posY >> 12) & 0xF0) + ((posX >> 16) & 0xF).
    int32 maskX    = width >> 4;
    int32 maskY    = height >> 4;
    int32 shiftW   = layer->widthShift;
    int32 fbOffset = currentScreen->pitch - lineSize;
    for (int32 cy = currentScreen->clipBound_Y1; cy < currentScreen->clipBound_Y2; ++cy) {
        int32 posX            = scanline->position.x;
        int32 posY            = scanline->position.y;
        int32 deformX         = scanline->deform.x;
        int32 deformY         = scanline->deform.y;
        uint16 *activePalette = fullPalette[*lineBuffer++];

        for (int32 cx = 0; cx < lineSize; ++cx) {
            int32 tile = layout[(maskX & (posX >> 20)) + ((maskY & (posY >> 20)) << shiftW)] & 0xFFF;
            uint8 idx  = tilesetPixels[(tile << 8) + ((posY >> 12) & 0xF0) + ((posX >> 16) & 0xF)];

            if (idx)
                *frameBuffer = activePalette[idx];

            posX += deformX;
            posY += deformY;
            ++frameBuffer;
        }

        frameBuffer += fbOffset;
        ++scanline;
    }
    return;
#endif

    for (int32 cy = currentScreen->clipBound_Y1; cy < currentScreen->clipBound_Y2; ++cy) {
        int32 posX = scanline->position.x;
        int32 posY = scanline->position.y;

        uint16 *activePalette = fullPalette[*lineBuffer];
        ++lineBuffer;
        int32 fbOffset = currentScreen->pitch - lineSize;

        for (int32 cx = 0; cx < lineSize; ++cx) {
            int32 tx = posX >> 20;
            int32 ty = posY >> 20;
            int32 x  = FROM_FIXED(posX) & 0xF;
            int32 y  = FROM_FIXED(posY) & 0xF;

            uint16 tile = layout[((width >> 4) & tx) + (((height >> 4) & ty) << layer->widthShift)] & 0xFFF;
            uint8 idx   = tilesetPixels[TILE_SIZE * (y + TILE_SIZE * tile) + x];

            if (idx)
                *frameBuffer = activePalette[idx];

            posX += scanline->deform.x;
            posY += scanline->deform.y;
            ++frameBuffer;
        }

        frameBuffer += fbOffset;
        ++scanline;
    }
}
