#if PSP_TILE_BENCH
#include "RSDK/Scene/PSP/PSPTitleReference.inl"
struct PSPTitleBenchState {
    uint32 calls, pairs;
    uint64 referenceUS, candidateUS;
};
static PSPTitleBenchState pspTitleBench[2][LAYER_COUNT];
static uint32 pspTitleOptimizedMask[2];

static void PSPResetTitleBench()
{
    memset(pspTitleBench, 0, sizeof(pspTitleBench));
    memset(pspTitleOptimizedMask, 0, sizeof(pspTitleOptimizedMask));
}

static void PSPReportTitleModes()
{
    PrintLog(PRINT_NORMAL, "PSP title modes v9: basic_rows_mask=%u roto_groups_mask=%u",
             pspTitleOptimizedMask[0], pspTitleOptimizedMask[1]);
}

static void PSPReportRotoShape(TileLayer *layer, int32 id)
{
    uint32 groups = 0, sameTile = 0, horizontal = 0;
    int32 minDX = 0x7FFFFFFF, maxDX = (-0x7FFFFFFF - 1);
    int32 minDY = 0x7FFFFFFF, maxDY = (-0x7FFFFFFF - 1);
    int32 width = currentScreen->clipBound_X2 - currentScreen->clipBound_X1;
    for (int32 y = currentScreen->clipBound_Y1; y < currentScreen->clipBound_Y2; ++y) {
        const ScanlineInfo &line = scanlines[y];
        int32 dx = line.deform.x, dy = line.deform.y;
        if (dx < minDX) minDX = dx;
        if (dx > maxDX) maxDX = dx;
        if (dy < minDY) minDY = dy;
        if (dy > maxDY) maxDY = dy;
        horizontal += dy == 0;
        bool bounded = dx >= -0x100000 && dx <= 0x100000 && dy >= -0x100000 && dy <= 0x100000;
        uint32 px = (uint32)line.position.x, py = (uint32)line.position.y;
        for (int32 x = 0; x + 4 <= width; x += 4) {
            ++groups;
            if (bounded && !(((px ^ (px + (uint32)dx * 3)) | (py ^ (py + (uint32)dy * 3))) & 0xFFF00000U))
                ++sameTile;
            px += (uint32)dx * 4; py += (uint32)dy * 4;
        }
    }
    PrintLog(PRINT_NORMAL, "PSP roto shape v9: scene=%s/%s layer=%d groups=%u same_tile=%u horizontal_rows=%u dx_min=%d dx_max=%d dy_min=%d dy_max=%d",
             currentSceneFolder, currentSceneID, id, groups, sameTile, horizontal, minDX, maxDX, minDY, maxDY);
}
#else
static void PSPResetTitleBench() {}
static void PSPReportTitleModes() {}
#endif

static void PSPDrawComparedTitle(TileLayer *layer, int32 kind)
{
    if (!layer->xsize || !layer->ysize)
        return;
    typedef void (*DrawFunction)(TileLayer *);
    DrawFunction candidate = kind == 0 ? PSPDrawLayerBasicRows : PSPDrawLayerRotozoomRuns;
#if PSP_TILE_BENCH
    int32 id = (int32)(layer - tileLayers);
    if (id < 0 || id >= LAYER_COUNT) {
        candidate(layer);
        return;
    }
    DrawFunction reference = kind == 0 ? PSPDrawLayerBasicReference : PSPDrawLayerRotozoomReference;
    ((pspTitleOptimizedMask[kind] & (1U << id)) ? candidate : reference)(layer);
    if (currentScreen->clipBound_X1 >= currentScreen->clipBound_X2 || currentScreen->clipBound_Y1 >= currentScreen->clipBound_Y2)
        return;
    PSPTitleBenchState &state = pspTitleBench[kind][id];
    if (state.pairs >= 4 || ++state.calls < 60 || (state.calls & 31) != (uint32)(id * 3))
        return;
    if (!state.pairs && kind == 1)
        PSPReportRotoShape(layer, id);
    for (int32 pass = 0; pass < 2; ++pass) {
        bool old = ((state.pairs + pass) & 1) == 0;
        DrawFunction draw = old ? reference : candidate;
        draw(layer); // Warm the function outside its timed draw.
        uint64 start = PSPProfNow();
        draw(layer);
        uint64 elapsed = PSPProfNow() - start;
        if (old) state.referenceUS += elapsed;
        else state.candidateUS += elapsed;
    }
    if (++state.pairs == 4) {
        bool faster = state.referenceUS && state.candidateUS * 100 <= state.referenceUS * 95;
        if (faster) pspTitleOptimizedMask[kind] |= 1U << id;
        PrintLog(PRINT_NORMAL, "PSP titlebench v9: scene=%s/%s layer=%d type=%s pairs=4 baseline_us=%u candidate_us=%u selected=%s",
                 currentSceneFolder, currentSceneID, id, kind == 0 ? "Basic" : "Rotozoom",
                 (uint32)(state.referenceUS / 4), (uint32)(state.candidateUS / 4), faster ? "candidate" : "reference");
    }
#else
    candidate(layer);
#endif
}
