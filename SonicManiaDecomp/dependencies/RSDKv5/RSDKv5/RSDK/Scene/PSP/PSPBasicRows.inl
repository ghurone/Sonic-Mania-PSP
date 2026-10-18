// Preserve v8 block traversal and edges; classify only complete 16-pixel rows.
static void PSPDrawLayerBasicRows(TileLayer *layer)
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
                    const uint32 slot = *layout & 0xFFF;
                    const uint16 emptyRows = pspTileRowEmpty[slot];
                    const uint16 opaqueRows = pspTileRowOpaque[slot];
                    const uint16 solidRows = pspTileRowSolid[slot];
                    uint16 rowBit = 1U << (((uint32)(pixels - tilesetPixels) >> 4) & 15);
                    
                    for (int32 y = 0; y < tileRemainY; ++y) {
                        if (opaqueRows & rowBit) {
                            if (solidRows & rowBit)
                                PSPFillSolidTileRow(frameBuffer, activePalette[pixels[0]]);
                            else
                                PSPDrawScheduledOpaqueTileRow(frameBuffer, pixels, activePalette);
                        }
                        else if (!(emptyRows & rowBit))
                            PSPDrawMixedTileRow(frameBuffer, pixels, activePalette);
                        rowBit <<= 1;

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
                    const uint32 slot = *layout & 0xFFF;
                    const uint16 emptyRows = pspTileRowEmpty[slot];
                    const uint16 opaqueRows = pspTileRowOpaque[slot];
                    const uint16 solidRows = pspTileRowSolid[slot];
                    uint16 rowBit = 1U << (((uint32)(pixels - tilesetPixels) >> 4) & 15);
                    

                    for (int32 y = 0; y < TILE_SIZE; ++y) {
                        if (opaqueRows & rowBit) {
                            if (solidRows & rowBit)
                                PSPFillSolidTileRow(frameBuffer, activePalette[pixels[0]]);
                            else
                                PSPDrawScheduledOpaqueTileRow(frameBuffer, pixels, activePalette);
                        }
                        else if (!(emptyRows & rowBit))
                            PSPDrawMixedTileRow(frameBuffer, pixels, activePalette);
                        rowBit <<= 1;

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
                    const uint32 slot = *layout & 0xFFF;
                    const uint16 emptyRows = pspTileRowEmpty[slot];
                    const uint16 opaqueRows = pspTileRowOpaque[slot];
                    const uint16 solidRows = pspTileRowSolid[slot];
                    uint16 rowBit = 1U << (((uint32)(pixels - tilesetPixels) >> 4) & 15);
                    
                    for (int32 y = 0; y < sheetY; ++y) {
                        if (opaqueRows & rowBit) {
                            if (solidRows & rowBit)
                                PSPFillSolidTileRow(frameBuffer, activePalette[pixels[0]]);
                            else
                                PSPDrawScheduledOpaqueTileRow(frameBuffer, pixels, activePalette);
                        }
                        else if (!(emptyRows & rowBit))
                            PSPDrawMixedTileRow(frameBuffer, pixels, activePalette);
                        rowBit <<= 1;

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
