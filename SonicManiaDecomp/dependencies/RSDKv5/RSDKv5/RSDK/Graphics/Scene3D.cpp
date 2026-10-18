#include "RSDK/Core/RetroEngine.hpp"

using namespace RSDK;

#if RETRO_REV0U
#include "Legacy/Scene3DLegacy.cpp"
#endif

Model RSDK::modelList[MODEL_COUNT];
Scene3D RSDK::scene3DList[SCENE3D_COUNT];

ScanEdge RSDK::scanEdgeBuffer[SCREEN_YSIZE * 2];

enum ModelFlags {
    MODEL_NOFLAGS     = 0,
    MODEL_USENORMALS  = 1 << 0,
    MODEL_USETEXTURES = 1 << 1,
    MODEL_USECOLOURS  = 1 << 2,
};

void RSDK::ProcessScanEdge(int32 x1, int32 y1, int32 x2, int32 y2)
{
    int32 ix1 = FROM_FIXED(x1);
    int32 iy1 = FROM_FIXED(y1);
    int32 ix2 = FROM_FIXED(x2);
    int32 iy2 = FROM_FIXED(y2);

    int32 top = FROM_FIXED(y1);
    if (iy1 != iy2) {
        if (iy1 > iy2) {
            top = FROM_FIXED(y2);
            ix1 = FROM_FIXED(x2);
            ix2 = FROM_FIXED(x1);
            iy1 = FROM_FIXED(y2);
            iy2 = FROM_FIXED(y1);
        }

        int32 bottom = iy2 + 1;
        if (top < currentScreen->clipBound_Y2 && bottom >= currentScreen->clipBound_Y1) {
            if (bottom > currentScreen->clipBound_Y2)
                bottom = currentScreen->clipBound_Y2;
            int32 scanPos = TO_FIXED(ix1);
            int32 delta   = TO_FIXED(ix2 - ix1) / (iy2 - iy1);
            if (top < 0) {
                scanPos -= top * delta;
                top = 0;
            }

            ScanEdge *edge = &scanEdgeBuffer[top];
            for (int32 i = top; i < bottom; ++i) {
                int32 scanX = scanPos >> 16;
                if (scanX < edge->start)
                    edge->start = scanX;
                if (scanX > edge->end)
                    edge->end = scanX;
                scanPos += delta;
                ++edge;
            }
        }
    }
}

void RSDK::ProcessScanEdgeClr(uint32 c1, uint32 c2, int32 x1, int32 y1, int32 x2, int32 y2)
{
    int32 iy1 = FROM_FIXED(y1);
    int32 iy2 = FROM_FIXED(y2);
    int32 ix1 = FROM_FIXED(x1);
    int32 ix2 = FROM_FIXED(x2);

    int32 top     = FROM_FIXED(y1);
    uint32 color1 = c1;
    uint32 color2 = c2;
    if (iy1 != iy2) {
        if (iy1 > iy2) {
            top    = FROM_FIXED(y2);
            ix1    = FROM_FIXED(x2);
            ix2    = FROM_FIXED(x1);
            iy1    = FROM_FIXED(y2);
            iy2    = FROM_FIXED(y1);
            color1 = c2;
            color2 = c1;
        }

        int32 bottom = iy2 + 1;
        if (top < currentScreen->clipBound_Y2 && bottom >= currentScreen->clipBound_Y1) {
            if (bottom > currentScreen->clipBound_Y2)
                bottom = currentScreen->clipBound_Y2;

            int32 size   = iy2 - iy1;
            int32 scanX  = TO_FIXED(ix1);
            int32 deltaX = TO_FIXED(ix2 - ix1) / size;

            int32 c1R   = (color1 & 0xFF0000);
            int32 c2R   = (color2 & 0xFF0000);
            int32 scanR = c1R;

            int32 deltaR = 0;
            if (c1R != c2R)
                deltaR = (c2R - c1R) / size;

            int32 c1G   = (color1 & 0x00FF00) << 8;
            int32 c2G   = (color2 & 0x00FF00) << 8;
            int32 scanG = c1G;

            int32 deltaG = 0;
            if (c1G != c2G)
                deltaG = (c2G - c1G) / size;

            int32 c1B   = (color1 & 0x0000FF) << 16;
            int32 c2B   = (color2 & 0x0000FF) << 16;
            int32 scanB = c1B;

            int32 deltaB = 0;
            if (c1B != c2B)
                deltaB = (c2B - c1B) / size;

            if (top < 0) {
                scanX -= top * deltaX;

                scanR -= top * deltaR;
                scanG -= top * deltaG;
                scanB -= top * deltaB;

                top = 0;
            }

            ScanEdge *edge = &scanEdgeBuffer[top];
            for (int32 i = top; i < bottom; ++i) {
                if (FROM_FIXED(scanX) < edge->start) {
                    edge->start = FROM_FIXED(scanX);

                    edge->startR = scanR;
                    edge->startG = scanG;
                    edge->startB = scanB;
                }

                if (FROM_FIXED(scanX) > edge->end) {
                    edge->end = FROM_FIXED(scanX);

                    edge->endR = scanR;
                    edge->endG = scanG;
                    edge->endB = scanB;
                }

                scanX += deltaX;

                scanR += deltaR;
                scanG += deltaG;
                scanB += deltaB;

                ++edge;
            }
        }
    }
}

void RSDK::SetIdentityMatrix(Matrix *matrix)
{
    matrix->values[0][0] = 0x100;
    matrix->values[1][0] = 0;
    matrix->values[2][0] = 0;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[2][1] = 0;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[1][2] = 0;
    matrix->values[2][2] = 0x100;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixMultiply(Matrix *dest, Matrix *matrixA, Matrix *matrixB)
{
    int32 result[4][4];
    memset(result, 0, 4 * 4 * sizeof(int32));

    for (int32 i = 0; i < 0x10; ++i) {
        uint32 rowA        = i / 4;
        uint32 rowB        = i % 4;
        result[rowB][rowA] = (matrixA->values[3][rowA] * matrixB->values[rowB][3] >> 8) + (matrixA->values[2][rowA] * matrixB->values[rowB][2] >> 8)
                             + (matrixA->values[1][rowA] * matrixB->values[rowB][1] >> 8)
                             + (matrixA->values[0][rowA] * matrixB->values[rowB][0] >> 8);
    }

    for (int32 i = 0; i < 0x10; ++i) {
        uint32 rowA              = i / 4;
        uint32 rowB              = i % 4;
        dest->values[rowB][rowA] = result[rowB][rowA];
    }
}
void RSDK::MatrixTranslateXYZ(Matrix *matrix, int32 x, int32 y, int32 z, bool32 setIdentity)
{
    if (setIdentity) {
        matrix->values[0][0] = 0x100;
        matrix->values[1][0] = 0;
        matrix->values[2][0] = 0;
        matrix->values[0][1] = 0;
        matrix->values[1][1] = 0x100;
        matrix->values[2][1] = 0;
        matrix->values[0][2] = 0;
        matrix->values[1][2] = 0;
        matrix->values[2][2] = 0x100;
        matrix->values[3][0] = 0;
        matrix->values[3][1] = 0;
        matrix->values[3][2] = 0;
        matrix->values[3][3] = 0x100;
    }

    matrix->values[0][3] = x >> 8;
    matrix->values[1][3] = y >> 8;
    matrix->values[2][3] = z >> 8;
}
void RSDK::MatrixScaleXYZ(Matrix *matrix, int32 scaleX, int32 scaleY, int32 scaleZ)
{
    matrix->values[0][0] = scaleX;
    matrix->values[1][0] = 0;
    matrix->values[2][0] = 0;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = 0;
    matrix->values[1][1] = scaleY;
    matrix->values[2][1] = 0;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[1][2] = 0;
    matrix->values[2][2] = scaleZ;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixRotateX(Matrix *matrix, int16 rotationX)
{
    int32 sine   = sin1024LookupTable[rotationX & 0x3FF] >> 2;
    int32 cosine = cos1024LookupTable[rotationX & 0x3FF] >> 2;

    matrix->values[0][0] = 0x100;
    matrix->values[1][0] = 0;
    matrix->values[2][0] = 0;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = 0;
    matrix->values[1][1] = cosine;
    matrix->values[2][1] = sine;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[1][2] = -sine;
    matrix->values[2][2] = cosine;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixRotateY(Matrix *matrix, int16 rotationY)
{
    int32 sine           = sin1024LookupTable[rotationY & 0x3FF] >> 2;
    int32 cosine         = cos1024LookupTable[rotationY & 0x3FF] >> 2;
    matrix->values[0][0] = cosine;
    matrix->values[1][0] = 0;
    matrix->values[2][0] = sine;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[2][1] = 0;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = -sine;
    matrix->values[1][2] = 0;
    matrix->values[2][2] = cosine;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixRotateZ(Matrix *matrix, int16 rotationZ)
{
    int32 sine           = sin1024LookupTable[rotationZ & 0x3FF] >> 2;
    int32 cosine         = cos1024LookupTable[rotationZ & 0x3FF] >> 2;
    matrix->values[0][0] = cosine;
    matrix->values[1][0] = -sine;
    matrix->values[2][0] = 0;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = sine;
    matrix->values[1][1] = cosine;
    matrix->values[2][1] = 0;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[1][2] = 0;
    matrix->values[2][2] = 0x100;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixRotateXYZ(Matrix *matrix, int16 rotationX, int16 rotationY, int16 rotationZ)
{
    int32 sinX = sin1024LookupTable[rotationX & 0x3FF] >> 2;
    int32 cosX = cos1024LookupTable[rotationX & 0x3FF] >> 2;
    int32 sinY = sin1024LookupTable[rotationY & 0x3FF] >> 2;
    int32 cosY = cos1024LookupTable[rotationY & 0x3FF] >> 2;
    int32 sinZ = sin1024LookupTable[rotationZ & 0x3FF] >> 2;
    int32 cosZ = cos1024LookupTable[rotationZ & 0x3FF] >> 2;

    matrix->values[0][0] = (cosZ * cosY >> 8) + (sinZ * (sinY * sinX >> 8) >> 8);
    matrix->values[0][1] = -(sinZ * cosX) >> 8;
    matrix->values[0][2] = (sinZ * (cosY * sinX >> 8) >> 8) - (cosZ * sinY >> 8);
    matrix->values[0][3] = 0;
    matrix->values[1][0] = (sinZ * cosY >> 8) - (cosZ * (sinY * sinX >> 8) >> 8);
    matrix->values[1][1] = cosZ * cosX >> 8;
    matrix->values[1][2] = (-(sinZ * sinY) >> 8) - (cosZ * (cosY * sinX >> 8) >> 8);
    matrix->values[1][3] = 0;
    matrix->values[2][0] = sinY * cosX >> 8;
    matrix->values[2][1] = sinX;
    matrix->values[2][2] = cosY * cosX >> 8;
    matrix->values[2][3] = 0;
    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixInverse(Matrix *dest, Matrix *matrix)
{
    double inv[16], det;
    double m[16];
    for (int32 y = 0; y < 4; ++y) {
        for (int32 x = 0; x < 4; ++x) {
            m[(y << 2) + x] = matrix->values[y][x] / 256.0;
        }
    }

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];

    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];

    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];

    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];

    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];

    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];

    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];

    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];

    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];

    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];

    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];

    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];

    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (det == 0)
        return;

    det = 1.0 / det;

    for (int32 i = 0; i < 0x10; ++i) inv[i] = (int32)((inv[i] * det) * 256);
    for (int32 i = 0; i < 0x10; ++i) dest->values[i / 4][i % 4] = (int32)inv[i];
}

#if RETRO_PLATFORM == RETRO_PSP
uint32 RSDK::pspFacesDrawn  = 0;
uint32 RSDK::pspFacesCulled = 0;
unsigned long long RSDK::pspS3DTime[3];
uint32 RSDK::pspS3DPixels;
unsigned long long RSDK::pspS3DDetail[3];
uint32 RSDK::pspS3DCalls, RSDK::pspS3DVerts, RSDK::pspS3DModels, RSDK::pspS3DReused;
static uint32 pspLitModelCount[MODEL_COUNT];
static uint8 pspLitModelFlags[MODEL_COUNT];
// the models lit most this period: id, vertices, times per 100 frames, flags (1 interpolated, 2 own colours, 4 ortho, 8 normals, 16 per face)
void RSDK::PSPReportLitModels(uint32 frames)
{
    for (int32 n = 0; n < 6; ++n) {
        int32 best = -1;
        for (int32 m = 0; m < MODEL_COUNT; ++m)
            if (pspLitModelCount[m] && (best < 0 || pspLitModelCount[m] * modelList[m].vertCount > pspLitModelCount[best] * modelList[best].vertCount))
                best = m;
        if (best < 0)
            break;
        PrintLog(PRINT_NORMAL, "PSP lit model %d: verts=%u indices=%u per100=%u flags=%u", best, modelList[best].vertCount, modelList[best].indexCount,
                 pspLitModelCount[best] * 100 / frames, pspLitModelFlags[best]);
        pspLitModelCount[best] = 0;
    }
    memset(pspLitModelCount, 0, sizeof(pspLitModelCount));
    memset(pspLitModelFlags, 0, sizeof(pspLitModelFlags));
}

static ModelVertex *pspWeldVertices;
static int PSPCompareVertexPos(const void *a, const void *b)
{
    const ModelVertex *va = &pspWeldVertices[*(const uint16 *)a];
    const ModelVertex *vb = &pspWeldVertices[*(const uint16 *)b];
    if (va->x != vb->x)
        return va->x < vb->x ? -1 : 1;
    if (va->y != vb->y)
        return va->y < vb->y ? -1 : 1;
    if (va->z != vb->z)
        return va->z < vb->z ? -1 : 1;
    return 0;
}
static int PSPCompareEdge(const void *a, const void *b)
{
    uint32 x = *(const uint32 *)a, y = *(const uint32 *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// The engine draws every face, back to front. For a closed mesh whose faces are all wound the same way, the faces turned
// away from the camera are always covered by the ones facing it, so they can be skipped without changing the picture.
// Returns the sign of the volume (which winding faces out) when every edge is used exactly once in each direction,
// else 0 (open, double sided or inconsistent meshes always draw every face).
static int8 PSPAnalyzeMesh(Model *mdl)
{
    int32 k = mdl->faceVertCount;
    if (k < 3 || k > 4 || !mdl->vertCount || mdl->indexCount < 4 * k || mdl->indexCount % k)
        return 0;
    for (int32 i = 0; i < mdl->indexCount; ++i) {
        if (mdl->indices[i] >= mdl->vertCount)
            return 0;
    }

    uint16 *order = (uint16 *)malloc(sizeof(uint16) * mdl->vertCount);
    uint16 *weld  = (uint16 *)malloc(sizeof(uint16) * mdl->vertCount);
    uint32 *edges = (uint32 *)malloc(sizeof(uint32) * mdl->indexCount);
    int8 result   = 0;

    if (order && weld && edges) {
        // one position is often split into several vertices (per-face normals/colours): weld by frame 0 position
        for (int32 i = 0; i < mdl->vertCount; ++i) order[i] = i;
        pspWeldVertices = mdl->vertices;
        qsort(order, mdl->vertCount, sizeof(uint16), PSPCompareVertexPos);
        for (int32 i = 0; i < mdl->vertCount; ++i)
            weld[order[i]] = (i && !PSPCompareVertexPos(&order[i - 1], &order[i])) ? weld[order[i - 1]] : order[i];

        bool32 closed = true;
        int32 edgeCount = 0;
        for (int32 f = 0; f < mdl->indexCount && closed; f += k) {
            for (int32 c = 0; c < k; ++c) {
                uint32 a = weld[mdl->indices[f + c]];
                uint32 b = weld[mdl->indices[f + (c + 1) % k]];
                if (a == b) {
                    closed = false;
                    break;
                }
                edges[edgeCount++] = (a << 16) | b;
            }
        }

        if (closed) {
            qsort(edges, edgeCount, sizeof(uint32), PSPCompareEdge);
            for (int32 i = 0; i < edgeCount && closed; ++i) {
                uint32 reverse = (edges[i] << 16) | (edges[i] >> 16);
                if ((i && edges[i] == edges[i - 1]) || !bsearch(&reverse, edges, edgeCount, sizeof(uint32), PSPCompareEdge))
                    closed = false;
            }
        }

        if (closed) {
            double volume = 0.0;
            for (int32 f = 0; f < mdl->indexCount; f += k) {
                ModelVertex *v0 = &mdl->vertices[mdl->indices[f]];
                for (int32 t = 1; t + 1 < k; ++t) {
                    ModelVertex *v1 = &mdl->vertices[mdl->indices[f + t]];
                    ModelVertex *v2 = &mdl->vertices[mdl->indices[f + t + 1]];
                    volume += (double)v0->x * ((double)v1->y * v2->z - (double)v1->z * v2->y)
                              - (double)v0->y * ((double)v1->x * v2->z - (double)v1->z * v2->x)
                              + (double)v0->z * ((double)v1->x * v2->y - (double)v1->y * v2->x);
                }
            }
            result = volume > 0.0 ? 1 : (volume < 0.0 ? -1 : 0);
        }
    }

    free(order);
    free(weld);
    free(edges);
    return result;
}

// Marks the vertices of the faces just added: the sign tells which screen winding is a back face for them.
static void PSPSetFaceCull(Scene3D *scn, int32 firstVertex, int32 vertexCount, Model *mdl, Matrix *m)
{
    if (!scn->pspFaceCull)
        return;

    int8 cull = 0;
    if (mdl->pspCull) {
        // world * view: a mirroring transform swaps which winding faces out
        float det = (float)m->values[0][0] * ((float)m->values[1][1] * m->values[2][2] - (float)m->values[1][2] * m->values[2][1])
                    - (float)m->values[0][1] * ((float)m->values[1][0] * m->values[2][2] - (float)m->values[1][2] * m->values[2][0])
                    + (float)m->values[0][2] * ((float)m->values[1][0] * m->values[2][1] - (float)m->values[1][1] * m->values[2][0]);
        cull = det > 0.0f ? mdl->pspCull : (det < 0.0f ? -mdl->pspCull : 0);
    }
    memset(&scn->pspFaceCull[firstVertex], cull, vertexCount);
}

// View space faces project through x' = x/z, y' = -y/z (z > 0): a face shows its outer side when volumeSign * screenArea > 0.
static inline bool32 PSPIsBackFace(const int8 *cull, int32 firstVertex, const Vector2 *pos, int32 count)
{
    if (!cull || !cull[firstVertex])
        return false;

    int64 area = 0;
    for (int32 i = 0; i < count; ++i) {
        int32 j = i + 1 == count ? 0 : i + 1;
        area += (int64)(pos[i].x >> 8) * (pos[j].y >> 8) - (int64)(pos[j].x >> 8) * (pos[i].y >> 8);
    }
    return cull[firstVertex] > 0 ? area < 0 : area > 0;
}
#endif

#if RETRO_PLATFORM == RETRO_PSP
// Models handed to the GE whole (see PSPRenderDevice.cpp): AddModelToScene/AddMeshFrameToScene only record what to
// draw, and Draw3DScene lights each unique vertex once and queues the model, instead of transforming every face
// vertex, sorting and rasterizing faces on the CPU. Only the draw modes whose look the GE reproduces are recorded:
// per-vertex (Gouraud) lighting, and flat colour when the whole model shares one colour.
struct PSPModelRecord {
    uint16 sceneID;
    uint16 modelID;
    uint8 drawMode;
    uint8 hasNormals;
    int32 frameOffset, nextFrameOffset, interpolate; // like AddMeshFrameToScene; 0/0/0 for AddModelToScene
    uint32 color;
    Matrix world;
    Matrix normals;
};

#define PSP_MODEL_RECORDS (160) // (UFO_Shadow puts up to ~90 shadows in one scene)
static PSPModelRecord pspModelRecords[PSP_MODEL_RECORDS];
static int32 pspModelRecordCount;
static bool32 pspBypassModelRecords; // set while re-adding a refused model the software way
int pspGeModels = 1;

void RSDK::PSPDropModelRecords(uint16 sceneID)
{
    int32 kept = 0;
    for (int32 r = 0; r < pspModelRecordCount; ++r) {
        if (pspModelRecords[r].sceneID != sceneID)
            pspModelRecords[kept++] = pspModelRecords[r];
    }
    pspModelRecordCount = kept;
}

// Whether the GE can draw this model, and which normals/colours AddModelToScene would have given its vertices.
static bool32 PSPRecordModel(uint16 modelFrames, uint16 sceneIndex, Animator *animator, uint8 drawMode, Matrix *matWorld, Matrix *matNormals,
                             color color)
{
    if (!pspGeModels || pspBypassModelRecords || modelFrames >= MODEL_COUNT || sceneIndex >= SCENE3D_COUNT || !matWorld
        || pspModelRecordCount == PSP_MODEL_RECORDS)
        return false;

    Model *mdl = &modelList[modelFrames];
    if (!mdl->pspGeReady)
        return false;

    // the only flags combination where the vertices take the model's own colours
    bool32 modelColors = mdl->flags == (MODEL_USENORMALS | MODEL_USECOLOURS);
    if (drawMode == S3D_SOLIDCOLOR_SCREEN) {
        if (modelColors)
            return false; // flat faces of the first vertex's colour: only uniform models look the same
    }
    else if (drawMode == S3D_SOLIDCOLOR_SHADED_BLENDED) {
        // fine either way: coloured models get three vertices per face (see PSPDrawModelRecord)
    }
    else if (drawMode != S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN) {
        return false;
    }

    Scene3D *scn = &scene3DList[sceneIndex];
    if (scn->vertLimit - scn->vertexCount < mdl->indexCount)
        return false; // the software path wouldn't draw it either; let it decide

    PSPModelRecord *record = &pspModelRecords[pspModelRecordCount++];
    record->sceneID        = sceneIndex;
    record->modelID        = modelFrames;
    record->drawMode       = drawMode;
    record->color          = color;
    record->world          = *matWorld;
    record->hasNormals     = (mdl->flags & MODEL_USENORMALS) && (mdl->flags == MODEL_USENORMALS || modelColors) && matNormals;
    if (record->hasNormals)
        record->normals = *matNormals;

    record->frameOffset     = 0;
    record->nextFrameOffset = 0;
    record->interpolate     = 0;
    if (animator) {
        int32 nextFrame = animator->frameID + 1;
        if (nextFrame >= animator->frameCount)
            nextFrame = animator->loopIndex;
        record->frameOffset     = animator->frameID * mdl->vertCount;
        record->nextFrameOffset = nextFrame * mdl->vertCount;
        record->interpolate     = animator->timer;
    }

    scn->drawMode = drawMode;
    return true;
}

// 0, 1, 2, ... for models drawn with a vertex per face corner
static const uint16 *PSPIdentityIndices(int32 count)
{
    static uint16 *indices = NULL;
    static int32 size      = 0;
    if (count > size) {
        uint16 *grown = (uint16 *)realloc(indices, count * sizeof(uint16));
        if (!grown)
            return NULL;
        indices = grown;
        for (int32 i = size; i < count; ++i) indices[i] = (uint16)i;
        size = count;
    }
    return indices;
}

// Lights the record's vertices exactly like Draw3DScene does per face vertex and queues the model on the GE.
// false: the GE can't draw it (ink, memory), the caller re-adds it the software way.
// View space depth range of a record's bounds.
static void PSPModelRecordDepth(PSPModelRecord *record, float *zMin, float *zMax)
{
    Model *mdl      = &modelList[record->modelID];
    const Matrix *m = &record->world;
    for (int32 c = 0; c < 8; ++c) {
        float vx = (c & 1) ? mdl->pspBoundsMax[0] : mdl->pspBoundsMin[0];
        float vy = (c & 2) ? mdl->pspBoundsMax[1] : mdl->pspBoundsMin[1];
        float vz = (c & 4) ? mdl->pspBoundsMax[2] : mdl->pspBoundsMin[2];
        float z  = (m->values[2][0] * vx + m->values[2][1] * vy + m->values[2][2] * vz) / 256.0f + m->values[2][3];
        *zMin    = MIN(*zMin, z);
        *zMax    = MAX(*zMax, z);
    }
}

// The common case of the lighting below (own colours, normals, shaded, a vertex per model vertex), with nothing left to
// decide per vertex: the same arithmetic, in fewer instructions and registers.
struct PSPLightParams {
    int32 interpolate, n0, n1, n2;
    int32 difX, difY, difZ, difIX, difIY, difIZ, specIX, specIY, specIZ;
    uint32 alphaBits;
};
template <bool interp>
static void PSPLightColoredVertices(PSPGe3DModelVertex *__restrict__ out, const ModelVertex *__restrict__ a, const ModelVertex *__restrict__ b,
                                    const Color *__restrict__ colors, int32 count, const PSPLightParams p)
{
    const int32 t = p.interpolate;
    for (int32 v = 0; v < count; ++v, ++a, ++b, ++out) {
        int32 x = a->x, y = a->y, z = a->z;
        int32 nx = a->nx, ny = a->ny, nz = a->nz;
        if (interp) {
            x += (t * (b->x - x)) >> 8;
            y += (t * (b->y - y)) >> 8;
            z += (t * (b->z - z)) >> 8;
            nx += (t * (b->nx - nx)) >> 8;
            ny += (t * (b->ny - ny)) >> 8;
            nz += (t * (b->nz - nz)) >> 8;
        }
        const uint32 c         = colors[v].color;
        const int32 normal     = (ny * p.n1 >> 8) + (nz * p.n2 >> 8) + (nx * p.n0 >> 8);
        const int32 normalVal  = ((normal >> 2) * (abs(normal) >> 2)) >> 6;
        const int32 diffuse    = normal >> 10;
        int32 r  = CLAMP(normalVal >> p.specIX, 0x00, 0xFF) + ((int32)((c >> 16) & 0xFF) * (diffuse + p.difX) >> p.difIX);
        int32 g  = CLAMP(normalVal >> p.specIY, 0x00, 0xFF) + ((int32)((c >> 8) & 0xFF) * (diffuse + p.difY) >> p.difIY);
        int32 bl = CLAMP(normalVal >> p.specIZ, 0x00, 0xFF) + ((int32)(c & 0xFF) * (diffuse + p.difZ) >> p.difIZ);
        r  = CLAMP(r, 0x00, 0xFF);
        g  = CLAMP(g, 0x00, 0xFF);
        bl = CLAMP(bl, 0x00, 0xFF);
        out->x     = (float)x;
        out->y     = (float)y;
        out->z     = (float)z;
        out->color = p.alphaBits | ((uint32)bl << 16) | ((uint32)g << 8) | (uint32)r;
    }
}

static bool32 PSPDrawModelRecord(PSPModelRecord *record, Scene3D *scn, Entity *entity, uint32 sceneSerial, float zNear, float zFar)
{
    int32 alpha = entity->alpha;
    int32 ink   = PSPGe3DInk(entity->inkEffect, &alpha);
    if (ink == -1)
        return false;
    if (ink == -2)
        return true;

    Model *mdl     = &modelList[record->modelID];
    const Matrix *m = &record->world;

    // On-screen rectangle from the model's bounds. Models reaching the camera's near limit, or projecting far outside
    // the screen, go the software way instead (which drops faces too close, then queues them as screen-space faces):
    // vertices near or behind the camera make the GE misbehave on hardware, up to locking the whole system.
    int32 clipX1 = currentScreen->clipBound_X1, clipX2 = currentScreen->clipBound_X2;
    int32 clipY1 = currentScreen->clipBound_Y1, clipY2 = currentScreen->clipBound_Y2;
    int32 x1 = clipX1, y1 = clipY1, x2 = clipX2, y2 = clipY2;
    // SHADED_BLENDED (no _SCREEN): no perspective, x / 256 and y / 256 minus the screen position (Draw3DScene)
    const bool32 ortho = record->drawMode == S3D_SOLIDCOLOR_SHADED_BLENDED;
    float orthoZMin = 1e30f, orthoZMax = -1e30f;
    if (ortho) {
        float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
        for (int32 c = 0; c < 8; ++c) {
            float vx = (c & 1) ? mdl->pspBoundsMax[0] : mdl->pspBoundsMin[0];
            float vy = (c & 2) ? mdl->pspBoundsMax[1] : mdl->pspBoundsMin[1];
            float vz = (c & 4) ? mdl->pspBoundsMax[2] : mdl->pspBoundsMin[2];
            float sx = ((m->values[0][0] * vx + m->values[0][1] * vy + m->values[0][2] * vz) / 256.0f + m->values[0][3]) / 256.0f
                       - currentScreen->position.x;
            float sy = ((m->values[1][0] * vx + m->values[1][1] * vy + m->values[1][2] * vz) / 256.0f + m->values[1][3]) / 256.0f
                       - currentScreen->position.y;
            float z  = (m->values[2][0] * vx + m->values[2][1] * vy + m->values[2][2] * vz) / 256.0f + m->values[2][3];
            // keep the GE well inside its 4096-pixel drawing space (w is 1 here, so nothing nears the camera)
            if (sx < currentScreen->center.x - 1500.0f || sx > currentScreen->center.x + 1500.0f || sy < currentScreen->center.y - 1500.0f
                || sy > currentScreen->center.y + 1500.0f)
                return false;
            minX = MIN(minX, sx), maxX = MAX(maxX, sx);
            minY = MIN(minY, sy), maxY = MAX(maxY, sy);
            orthoZMin = MIN(orthoZMin, z), orthoZMax = MAX(orthoZMax, z);
        }
        if (maxX < clipX1 || minX >= clipX2 || maxY < clipY1 || minY >= clipY2)
            return true; // entirely off screen
        x1 = MAX(clipX1, (int32)minX - 2);
        y1 = MAX(clipY1, (int32)minY - 2);
        x2 = MIN(clipX2, (int32)maxX + 3);
        y2 = MIN(clipY2, (int32)maxY + 3);
        orthoZMin -= 16.0f, orthoZMax += 16.0f;
    }
    else {
        float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
        bool32 nearCut = false;
        for (int32 c = 0; c < 8 && !nearCut; ++c) {
            float vx = (c & 1) ? mdl->pspBoundsMax[0] : mdl->pspBoundsMin[0];
            float vy = (c & 2) ? mdl->pspBoundsMax[1] : mdl->pspBoundsMin[1];
            float vz = (c & 4) ? mdl->pspBoundsMax[2] : mdl->pspBoundsMin[2];
            float x  = (m->values[0][0] * vx + m->values[0][1] * vy + m->values[0][2] * vz) / 256.0f + m->values[0][3];
            float y  = (m->values[1][0] * vx + m->values[1][1] * vy + m->values[1][2] * vz) / 256.0f + m->values[1][3];
            float z  = (m->values[2][0] * vx + m->values[2][1] * vy + m->values[2][2] * vz) / 256.0f + m->values[2][3];
            if (z < 1024.0f) {
                nearCut = true;
                break;
            }
            float dx = x * (float)(1 << scn->projectionX) / z;
            float dy = y * (float)(1 << scn->projectionY) / z;
            if (dx < -768.0f || dx > 768.0f || dy < -768.0f || dy > 768.0f) {
                nearCut = true; // well inside the GE clip volume (PSP_GE3D_NDC) or not at all
                break;
            }
            float sx = currentScreen->center.x + dx;
            float sy = currentScreen->center.y - dy;
            minX     = MIN(minX, sx);
            maxX     = MAX(maxX, sx);
            minY     = MIN(minY, sy);
            maxY     = MAX(maxY, sy);
        }
        if (nearCut)
            return false;
        if (maxX < clipX1 || minX >= clipX2 || maxY < clipY1 || minY >= clipY2)
            return true; // entirely off screen
        x1 = MAX(clipX1, (int32)minX - 2);
        y1 = MAX(clipY1, (int32)minY - 2);
        x2 = MIN(clipX2, (int32)maxX + 3);
        y2 = MIN(clipY2, (int32)maxY + 3);
    }
    if (x2 <= x1 || y2 <= y1)
        return true;

    // the lighting inputs: an instance lit exactly like the last one queued can draw the same vertices
    bool32 modelColors = mdl->flags == (MODEL_USENORMALS | MODEL_USECOLOURS);
    bool32 shaded      = record->drawMode == S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN || ortho;
    bool32 useNormals  = shaded && record->hasNormals;
    const Matrix *n    = &record->normals;
    int32 key[18]      = { record->frameOffset,
                           record->nextFrameOffset,
                           record->interpolate,
                           (int32)record->color,
                           alpha,
                           record->drawMode | (modelColors << 8) | (useNormals << 9) | (ink << 16),
                           useNormals ? n->values[1][0] : 0,
                           useNormals ? n->values[1][1] : 0,
                           useNormals ? n->values[1][2] : 0 };
    if (shaded) {
        key[9] = scn->diffuseX, key[10] = scn->diffuseY, key[11] = scn->diffuseZ;
        key[12] = scn->diffuseIntensityX, key[13] = scn->diffuseIntensityY, key[14] = scn->diffuseIntensityZ;
        key[15] = scn->specularIntensityX, key[16] = scn->specularIntensityY, key[17] = scn->specularIntensityZ;
    }

    unsigned long long lightStart = PSPProfFine();
    if (mdl->pspLitBatch == PSPGe3DBatch() && !memcmp(key, mdl->pspLitKey, sizeof(key))
        && PSPReuse3DModel(mdl->pspLitBatch, mdl->pspLitOffset, ink, sceneSerial)) {
        pspS3DReused++;
    }
    else {
        // SHADED_BLENDED lights all three corners of a face with the face's first vertex colour (Draw3DScene's
        // drawVert->color): with per-vertex colours each face needs its own three vertices
        const bool32 perFace = ortho && modelColors;
        if (perFace && !PSPIdentityIndices(mdl->indexCount))
            return false;
        PSPGe3DModelVertex *out = PSPBegin3DModel(perFace ? mdl->indexCount : mdl->vertCount, ink, sceneSerial);
        if (!out)
            return false;
        mdl->pspLitBatch  = PSPGe3DBatch();
        mdl->pspLitOffset = PSPGe3DOffset(out);
        memcpy(mdl->pspLitKey, key, sizeof(key));
        out = PSPGe3DUncached(out);
        pspS3DVerts += mdl->vertCount;
        pspS3DModels++;
        pspLitModelCount[record->modelID]++;
        pspLitModelFlags[record->modelID] |= (record->interpolate ? 1 : 0) | (modelColors ? 2 : 0) | (ortho ? 4 : 0) | (useNormals ? 8 : 0) | (perFace ? 16 : 0);

        // locals: the compiler can't keep anything read through pointers in registers across the stores to out
        const int32 interpolate = record->interpolate;
        const ModelVertex *a    = &mdl->vertices[record->frameOffset];
        const ModelVertex *b    = &mdl->vertices[record->nextFrameOffset];
        const Color *colors     = modelColors ? mdl->colors : NULL;
        const uint32 flat       = record->color;
        const uint32 alphaBits  = (uint32)alpha << 24;
        const int32 n0 = key[6], n1 = key[7], n2 = key[8];
        const int32 difX = scn->diffuseX, difY = scn->diffuseY, difZ = scn->diffuseZ;
        const int32 difIX = scn->diffuseIntensityX, difIY = scn->diffuseIntensityY, difIZ = scn->diffuseIntensityZ;
        const int32 specIX = scn->specularIntensityX, specIY = scn->specularIntensityY, specIZ = scn->specularIntensityZ;
        const int32 vertCount = perFace ? mdl->indexCount : mdl->vertCount;
        const ModelVertex *frameA = a, *frameB = b;
        const uint16 *corners     = mdl->indices;

        if (!perFace && colors && shaded && useNormals) {
            const PSPLightParams params = { interpolate, n0, n1, n2, difX, difY, difZ, difIX, difIY, difIZ, specIX, specIY, specIZ, alphaBits };
            if (interpolate)
                PSPLightColoredVertices<true>(out, a, b, colors, vertCount, params);
            else
                PSPLightColoredVertices<false>(out, a, b, colors, vertCount, params);
        }
        else
        for (int32 v = 0; v < vertCount; ++v, ++a, ++b, ++out) {
            uint32 faceColor = 0;
            if (perFace) {
                a         = &frameA[corners[v]];
                b         = &frameB[corners[v]];
                faceColor = colors[corners[v - v % 3]].color;
            }
            int32 x = a->x, y = a->y, z = a->z;
            if (interpolate) {
                x += (interpolate * (b->x - x)) >> 8;
                y += (interpolate * (b->y - y)) >> 8;
                z += (interpolate * (b->z - z)) >> 8;
            }
            out->x = (float)x;
            out->y = (float)y;
            out->z = (float)z;

            uint32 c = perFace ? faceColor : (colors ? colors[v].color : flat);
            int32 r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, bl = c & 0xFF;
            if (shaded) {
                int32 normal = 0;
                if (useNormals) {
                    int32 nx = a->nx, ny = a->ny, nz = a->nz;
                    if (interpolate) {
                        nx += (interpolate * (b->nx - nx)) >> 8;
                        ny += (interpolate * (b->ny - ny)) >> 8;
                        nz += (interpolate * (b->nz - nz)) >> 8;
                    }
                    normal = (ny * n1 >> 8) + (nz * n2 >> 8) + (nx * n0 >> 8);
                }
                int32 normalVal = (normal >> 2) * (abs(normal) >> 2);
                int32 diffuse   = normal >> 10;

                int32 specular = CLAMP(normalVal >> 6 >> specIX, 0x00, 0xFF);
                r              = CLAMP(specular + (r * (diffuse + difX) >> difIX), 0x00, 0xFF);
                specular       = CLAMP(normalVal >> 6 >> specIY, 0x00, 0xFF);
                g              = CLAMP(specular + (g * (diffuse + difY) >> difIY), 0x00, 0xFF);
                specular       = CLAMP(normalVal >> 6 >> specIZ, 0x00, 0xFF);
                bl             = CLAMP(specular + (bl * (diffuse + difZ) >> difIZ), 0x00, 0xFF);
            }
            out->color = alphaBits | ((uint32)bl << 16) | ((uint32)g << 8) | (uint32)r;
        }
    }

    unsigned long long lightEnd = PSPProfFine();
    pspS3DDetail[1] += lightEnd - lightStart;
    // closed models: the GE can skip faces turned away (which way that is flips with a mirroring transform)
    int32 cull = 0;
    if (mdl->pspCull && ink == INK_NONE && !ortho) { // (the software path culls nothing in the non-SCREEN modes)
        float det = (float)m->values[0][0] * ((float)m->values[1][1] * m->values[2][2] - (float)m->values[1][2] * m->values[2][1])
                    - (float)m->values[0][1] * ((float)m->values[1][0] * m->values[2][2] - (float)m->values[1][2] * m->values[2][0])
                    + (float)m->values[0][2] * ((float)m->values[1][0] * m->values[2][1] - (float)m->values[1][1] * m->values[2][0]);
        cull = det > 0.0f ? mdl->pspCull : (det < 0.0f ? -mdl->pspCull : 0);
    }
    if (ortho)
        PSPEnd3DModelOrtho(ortho && modelColors ? PSPIdentityIndices(mdl->indexCount) : mdl->indices, mdl->indexCount, m, x1, y1, x2, y2,
                           orthoZMin, orthoZMax);
    else
        PSPEnd3DModel(mdl->indices, mdl->indexCount, m, scn->projectionX, scn->projectionY, x1, y1, x2, y2, cull, zNear, zFar);
    pspS3DDetail[2] += PSPProfFine() - lightEnd;
    pspFacesDrawn += mdl->indexCount / 3;
    return true;
}

// Called at the top of Draw3DScene: queues this scene's recorded models, re-adding refused ones the software way.
static void PSPDrawModelRecords(uint16 sceneID, Scene3D *scn, Entity *entity)
{
    // the scene's models share one depth span (the software renderer sorted all their faces together)
    static uint32 sceneSerial = 0;
    struct RecordsTimer {
        unsigned long long start;
        ~RecordsTimer() { pspS3DDetail[0] += PSPProfFine() - start; }
    } recordsTimer = { PSPProfFine() };
    pspS3DCalls++;
    float zMin = 1e30f, zMax = -1e30f;
    for (int32 r = 0; r < pspModelRecordCount; ++r) {
        if (pspModelRecords[r].sceneID == sceneID)
            PSPModelRecordDepth(&pspModelRecords[r], &zMin, &zMax);
    }
    ++sceneSerial;
    float zNear = MAX(zMin * 0.98f, 256.0f), zFar = zMax * 1.02f + 16.0f;

    for (int32 r = 0; r < pspModelRecordCount; ++r) {
        PSPModelRecord *record = &pspModelRecords[r];
        if (record->sceneID != sceneID || PSPDrawModelRecord(record, scn, entity, sceneSerial, zNear, zFar))
            continue;

        Model *mdl            = &modelList[record->modelID];
        pspBypassModelRecords = true;
        if (record->frameOffset || record->nextFrameOffset || record->interpolate) {
            Animator animator;
            memset(&animator, 0, sizeof(animator));
            animator.frameID    = record->frameOffset / mdl->vertCount;
            animator.frameCount = 0x7FFF;
            animator.loopIndex  = record->nextFrameOffset / mdl->vertCount;
            animator.timer      = record->interpolate;
            if (animator.frameID + 1 != animator.loopIndex)
                animator.frameCount = animator.frameID + 1; // next frame is the loop point
            AddMeshFrameToScene(record->modelID, sceneID, &animator, record->drawMode, &record->world, record->hasNormals ? &record->normals : NULL,
                                record->color);
        }
        else {
            AddModelToScene(record->modelID, sceneID, record->drawMode, &record->world, record->hasNormals ? &record->normals : NULL, record->color);
        }
        pspBypassModelRecords = false;
    }
    PSPDropModelRecords(sceneID);
}
#endif

uint16 RSDK::LoadMesh(const char *filename, uint8 scope)
{
#if RETRO_PLATFORM == RETRO_PSP
    PSPLoadTimer loadTimer(PSPLOAD_MESH);
#endif
    if (!scope || scope > SCOPE_STAGE)
        return -1;

    char fullFilePath[0x100];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/Meshes/%s", filename);

    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5(fullFilePath, hash);

    for (int32 i = 0; i < MODEL_COUNT; ++i) {
        if (HASH_MATCH_MD5(hash, modelList[i].hash)) {
            return i;
        }
    }

    uint16 id = -1;
    for (id = 0; id < MODEL_COUNT; ++id) {
        if (modelList[id].scope == SCOPE_NONE)
            break;
    }

    if (id >= MODEL_COUNT)
        return -1;

    Model *model = &modelList[id];
    FileInfo info;
    InitFileInfo(&info);
    if (LoadFile(&info, fullFilePath, FMODE_RB)) {
        uint32 sig = ReadInt32(&info, false);

        if (sig != RSDK_SIGNATURE_MDL) {
            CloseFile(&info);
            return -1;
        }

        model->scope = scope;
        HASH_COPY_MD5(model->hash, hash);

        model->flags         = ReadInt8(&info);
        model->faceVertCount = ReadInt8(&info);

        model->vertCount  = ReadInt16(&info);
        model->frameCount = ReadInt16(&info);

        AllocateStorage((void **)&model->vertices, sizeof(ModelVertex) * model->vertCount * model->frameCount, DATASET_STG, true);
        if (model->flags & MODEL_USETEXTURES)
            AllocateStorage((void **)&model->texCoords, sizeof(TexCoord) * model->vertCount, DATASET_STG, true);
        if (model->flags & MODEL_USECOLOURS)
            AllocateStorage((void **)&model->colors, sizeof(Color) * model->vertCount, DATASET_STG, true);

        if (model->flags & MODEL_USETEXTURES) {
            for (int32 v = 0; v < model->vertCount; ++v) {
                model->texCoords[v].x = ReadSingle(&info);
                model->texCoords[v].y = ReadSingle(&info);
            }
        }

        if (model->flags & MODEL_USECOLOURS) {
            for (int32 v = 0; v < model->vertCount; ++v) {
                model->colors[v].color = ReadInt32(&info, false);
            }
        }

        model->indexCount = ReadInt16(&info);
        AllocateStorage((void **)&model->indices, sizeof(uint16) * model->indexCount, DATASET_STG, true);
#if RETRO_PLATFORM == RETRO_PSP
        // Same values as the per-value reads below (the file is little endian like the PSP), read a block at a time:
        // one call per float made meshes the slowest part of loading the special stages.
        if (model->indices)
            ReadBytes(&info, model->indices, sizeof(uint16) * model->indexCount);

        int32 floatsPerVertex = (model->flags & MODEL_USENORMALS) ? 6 : 3;
        float *frameData      = (float *)malloc(sizeof(float) * floatsPerVertex * model->vertCount);
        for (int32 f = 0; frameData && f < model->frameCount; ++f) {
            ReadBytes(&info, frameData, sizeof(float) * floatsPerVertex * model->vertCount);

            const float *value = frameData;
            for (int32 v = 0; v < model->vertCount; ++v) {
                ModelVertex *vertex = &model->vertices[(f * model->vertCount) + v];
                vertex->x           = (int32)(value[0] * 0x100);
                vertex->y           = (int32)(value[1] * 0x100);
                vertex->z           = (int32)(value[2] * 0x100);

                vertex->nx = 0;
                vertex->ny = 0;
                vertex->nz = 0;
                if (model->flags & MODEL_USENORMALS) {
                    vertex->nx = (int32)(value[3] * 0x10000);
                    vertex->ny = (int32)(value[4] * 0x10000);
                    vertex->nz = (int32)(value[5] * 0x10000);
                }
                value += floatsPerVertex;
            }
        }

        int32 firstFrame = frameData ? model->frameCount : 0; // no scratch memory: the original path does it all
        free(frameData);
        if (!model->indices)
            firstFrame = model->frameCount; // nothing sensible to read into, as before
#else
        for (int32 i = 0; i < model->indexCount; ++i) model->indices[i] = ReadInt16(&info);
        int32 firstFrame = 0;
#endif

        for (int32 f = firstFrame; f < model->frameCount; ++f) {
            for (int32 v = 0; v < model->vertCount; ++v) {
                model->vertices[(f * model->vertCount) + v].x = (int32)(ReadSingle(&info) * 0x100);
                model->vertices[(f * model->vertCount) + v].y = (int32)(ReadSingle(&info) * 0x100);
                model->vertices[(f * model->vertCount) + v].z = (int32)(ReadSingle(&info) * 0x100);

                model->vertices[(f * model->vertCount) + v].nx = 0;
                model->vertices[(f * model->vertCount) + v].ny = 0;
                model->vertices[(f * model->vertCount) + v].nz = 0;
                if (model->flags & MODEL_USENORMALS) {
                    model->vertices[(f * model->vertCount) + v].nx = (int32)(ReadSingle(&info) * 0x10000);
                    model->vertices[(f * model->vertCount) + v].ny = (int32)(ReadSingle(&info) * 0x10000);
                    model->vertices[(f * model->vertCount) + v].nz = (int32)(ReadSingle(&info) * 0x10000);
                }
            }
        }

        CloseFile(&info);
#if RETRO_PLATFORM == RETRO_PSP
        unsigned long long analyzeStart = PSPProfNow();
        model->pspCull                  = PSPAnalyzeMesh(model);

        model->pspLitBatch = 0; // nothing queued for it yet
        model->pspGeReady = model->faceVertCount == 3 && model->indexCount >= 3 && !(model->indexCount % 3) && model->indices && model->vertices;
        for (int32 i = 0; model->pspGeReady && i < model->indexCount; ++i) {
            if (model->indices[i] >= model->vertCount)
                model->pspGeReady = false;
        }
        for (int32 a = 0; a < 3; ++a) {
            model->pspBoundsMin[a] = 1e30f;
            model->pspBoundsMax[a] = -1e30f;
        }
        for (int32 v = 0; model->vertices && v < model->vertCount * model->frameCount; ++v) {
            const int32 pos[3] = { model->vertices[v].x, model->vertices[v].y, model->vertices[v].z };
            for (int32 a = 0; a < 3; ++a) {
                model->pspBoundsMin[a] = MIN(model->pspBoundsMin[a], (float)pos[a]);
                model->pspBoundsMax[a] = MAX(model->pspBoundsMax[a], (float)pos[a]);
            }
        }
        PrintLog(PRINT_NORMAL, "PSP mesh %s: faces=%d verts=%d frames=%d cull=%d (%uus)", filename,
                 model->faceVertCount ? model->indexCount / model->faceVertCount : 0, model->vertCount, model->frameCount, model->pspCull,
                 (uint32)(PSPProfNow() - analyzeStart));
#endif
        return id;
    }
    return -1;
}
uint16 RSDK::Create3DScene(const char *name, uint16 vertexLimit, uint8 scope)
{
    if (!scope || scope > SCOPE_STAGE)
        return -1;

    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5(name, hash);

    for (int32 i = 0; i < SCENE3D_COUNT; ++i) {
        if (HASH_MATCH_MD5(hash, scene3DList[i].hash)) {
            return i;
        }
    }

    uint16 id = -1;
    for (id = 0; id < SCENE3D_COUNT; ++id) {
        if (scene3DList[id].scope == SCOPE_NONE)
            break;
    }

    if (id >= SCENE3D_COUNT)
        return -1;

    Scene3D *scene = &scene3DList[id];

    if (vertexLimit > SCENE3D_VERT_COUNT || !vertexLimit)
        vertexLimit = SCENE3D_VERT_COUNT;

    scene->scope = scope;
    HASH_COPY_MD5(scene->hash, hash);
    scene->vertLimit = vertexLimit;
    scene->faceCount = 6;

    scene->projectionX = 8;
    scene->projectionY = 8;
    AllocateStorage((void **)&scene->vertices, sizeof(Scene3DVertex) * vertexLimit, DATASET_STG, true);
    AllocateStorage((void **)&scene->normals, sizeof(Scene3DVertex) * vertexLimit, DATASET_STG, true);
    AllocateStorage((void **)&scene->faceVertCounts, sizeof(uint8) * vertexLimit, DATASET_STG, true);
    AllocateStorage((void **)&scene->faceBuffer, sizeof(Scene3DFace) * vertexLimit, DATASET_STG, true);
#if RETRO_PLATFORM == RETRO_PSP
    AllocateStorage((void **)&scene->pspFaceCull, sizeof(int8) * vertexLimit, DATASET_STG, true); // NULL: never cull
#endif

    return id;
}
void RSDK::AddModelToScene(uint16 modelFrames, uint16 sceneIndex, uint8 drawMode, Matrix *matWorld, Matrix *matNormals, color color)
{
#if RETRO_PLATFORM == RETRO_PSP
    struct AddTimer {
        unsigned long long start = PSPProfFine();
        ~AddTimer() { pspS3DTime[0] += PSPProfFine() - start; }
    } addTimer;
    if (PSPRecordModel(modelFrames, sceneIndex, NULL, drawMode, matWorld, matNormals, color))
        return; // drawn whole by the GE in Draw3DScene
#endif
    if (modelFrames < MODEL_COUNT && sceneIndex < SCENE3D_COUNT) {
        if (matWorld) {
            Model *mdl            = &modelList[modelFrames];
            Scene3D *scn          = &scene3DList[sceneIndex];
            uint16 *indices       = mdl->indices;
            int32 vertID          = scn->vertexCount;
            uint8 *faceVertCounts = &scn->faceVertCounts[scn->faceCount];
            int32 indCnt          = mdl->indexCount;
            if (scn->vertLimit - vertID >= indCnt) {
#if RETRO_PLATFORM == RETRO_PSP
                PSPSetFaceCull(scn, vertID, indCnt, mdl, matWorld);
#endif
                scn->vertexCount += mdl->indexCount;
                scn->drawMode = drawMode;
                scn->faceCount += indCnt / mdl->faceVertCount;

                int32 i = 0;
                int32 f = 0;
                switch (mdl->flags) {
                    default:
                    case MODEL_NOFLAGS:
                    case MODEL_USECOLOURS:
                        for (; i < mdl->indexCount;) {
                            faceVertCounts[f++] = mdl->faceVertCount;

                            for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                ModelVertex *modelVert = &mdl->vertices[indices[i++]];
                                Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                vertex->x = matWorld->values[0][3] + (modelVert->z * matWorld->values[0][2] >> 8)
                                            + (matWorld->values[0][0] * modelVert->x >> 8) + (matWorld->values[0][1] * modelVert->y >> 8);
                                vertex->y = matWorld->values[1][3] + (modelVert->y * matWorld->values[1][1] >> 8)
                                            + (modelVert->z * matWorld->values[1][2] >> 8) + (matWorld->values[1][0] * modelVert->x >> 8);
                                vertex->z = matWorld->values[2][3] + ((modelVert->x * matWorld->values[2][0]) >> 8)
                                            + ((matWorld->values[2][2] * modelVert->z >> 8) + (matWorld->values[2][1] * modelVert->y >> 8));

                                vertex->color = color;
                            }
                        }
                        break;

                    case MODEL_USENORMALS:
                        if (matNormals) {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *modelVert = &mdl->vertices[indices[i++]];
                                    Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                    vertex->x = matWorld->values[0][3] + (modelVert->z * matWorld->values[0][2] >> 8)
                                                + (modelVert->x * matWorld->values[0][0] >> 8) + (modelVert->y * matWorld->values[0][1] >> 8);
                                    vertex->y = matWorld->values[1][3] + (modelVert->y * matWorld->values[1][1] >> 8)
                                                + (matWorld->values[1][0] * modelVert->x >> 8) + (modelVert->z * matWorld->values[1][2] >> 8);
                                    vertex->z = matWorld->values[2][3] + (modelVert->x * matWorld->values[2][0] >> 8)
                                                + (matWorld->values[2][2] * modelVert->z >> 8) + (matWorld->values[2][1] * modelVert->y >> 8);

                                    vertex->nx = (modelVert->nz * matNormals->values[0][2] >> 8) + (modelVert->nx * matNormals->values[0][0] >> 8)
                                                 + (matNormals->values[0][1] * modelVert->ny >> 8);
                                    vertex->ny = (modelVert->ny * matNormals->values[1][1] >> 8) + (modelVert->nz * matNormals->values[1][2] >> 8)
                                                 + (modelVert->nx * matNormals->values[1][0] >> 8);
                                    vertex->nz =
                                        ((modelVert->ny * matNormals->values[2][1]) >> 8)
                                        + ((matNormals->values[2][0] * modelVert->nx >> 8) + (modelVert->nz * matNormals->values[2][2] >> 8));

                                    vertex->color = color;
                                }
                            }
                        }
                        else {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *modelVert = &mdl->vertices[indices[i++]];
                                    Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                    vertex->x = matWorld->values[0][3] + (modelVert->z * matWorld->values[0][2] >> 8)
                                                + (matWorld->values[0][0] * modelVert->x >> 8) + (matWorld->values[0][1] * modelVert->y >> 8);
                                    vertex->y = matWorld->values[1][3] + (modelVert->y * matWorld->values[1][1] >> 8)
                                                + (modelVert->z * matWorld->values[1][2] >> 8) + (matWorld->values[1][0] * modelVert->x >> 8);
                                    vertex->z = matWorld->values[2][3] + ((matWorld->values[2][2] * modelVert->z) >> 8)
                                                + ((matWorld->values[2][0] * modelVert->x >> 8) + (matWorld->values[2][1] * modelVert->y >> 8));

                                    vertex->color = color;
                                }
                            }
                        }
                        break;

                    case MODEL_USENORMALS | MODEL_USECOLOURS:
                        if (matNormals) {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *modelVert = &mdl->vertices[indices[i]];
                                    Color *modelColor      = &mdl->colors[indices[i++]];
                                    Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                    vertex->x = matWorld->values[0][3] + (matWorld->values[0][2] * modelVert->z >> 8)
                                                + (modelVert->y * matWorld->values[0][1] >> 8) + (matWorld->values[0][0] * modelVert->x >> 8);
                                    vertex->y = matWorld->values[1][3] + (matWorld->values[1][2] * modelVert->z >> 8)
                                                + (modelVert->y * matWorld->values[1][1] >> 8) + (matWorld->values[1][0] * modelVert->x >> 8);
                                    vertex->z = matWorld->values[2][3] + (modelVert->x * matWorld->values[2][0] >> 8)
                                                + (modelVert->y * matWorld->values[2][1] >> 8) + (matWorld->values[2][2] * modelVert->z >> 8);

                                    vertex->nx = (matNormals->values[0][0] * modelVert->nx >> 8) + (modelVert->ny * matNormals->values[0][1] >> 8)
                                                 + (matNormals->values[0][2] * modelVert->nz >> 8);
                                    vertex->ny = (matNormals->values[1][0] * modelVert->nx >> 8) + (modelVert->ny * matNormals->values[1][1] >> 8)
                                                 + (matNormals->values[1][2] * modelVert->nz >> 8);
                                    vertex->nz =
                                        ((matNormals->values[2][2] * modelVert->nz) >> 8)
                                        + ((modelVert->ny * matNormals->values[2][1] >> 8) + (matNormals->values[2][0] * modelVert->nx >> 8));

                                    vertex->color = modelColor->color;
                                }
                            }
                        }
                        else {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *modelVert = &mdl->vertices[indices[i]];
                                    Color *modelColor      = &mdl->colors[indices[i++]];
                                    Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                    vertex->x = matWorld->values[0][3] + (matWorld->values[0][0] * modelVert->x >> 8)
                                                + (modelVert->y * matWorld->values[0][1] >> 8) + (modelVert->z * matWorld->values[0][2] >> 8);
                                    vertex->y = matWorld->values[1][3] + (modelVert->z * matWorld->values[1][2] >> 8)
                                                + (matWorld->values[1][0] * modelVert->x >> 8) + (modelVert->y * matWorld->values[1][1] >> 8);
                                    vertex->z = matWorld->values[2][3] + (matWorld->values[2][2] * modelVert->z >> 8)
                                                + (modelVert->y * matWorld->values[2][1] >> 8) + (modelVert->x * matWorld->values[2][0] >> 8);

                                    vertex->color = modelColor->color;
                                }
                            }
                        }
                        break;
                }
            }
        }
    }
}
void RSDK::AddMeshFrameToScene(uint16 modelFrames, uint16 sceneIndex, Animator *animator, uint8 drawMode, Matrix *matWorld, Matrix *matNormals,
                               color color)
{
#if RETRO_PLATFORM == RETRO_PSP
    struct AddTimer {
        unsigned long long start = PSPProfFine();
        ~AddTimer() { pspS3DTime[0] += PSPProfFine() - start; }
    } addTimer;
    if (animator && PSPRecordModel(modelFrames, sceneIndex, animator, drawMode, matWorld, matNormals, color))
        return; // drawn whole by the GE in Draw3DScene
#endif
    if (modelFrames < MODEL_COUNT && sceneIndex < SCENE3D_COUNT) {
        if (matWorld && animator) {
            Model *mdl            = &modelList[modelFrames];
            Scene3D *scn          = &scene3DList[sceneIndex];
            uint16 *indices       = mdl->indices;
            int32 vertID          = scn->vertexCount;
            uint8 *faceVertCounts = &scn->faceVertCounts[scn->faceCount];
            int32 indCnt          = mdl->indexCount;
            if (scn->vertLimit - vertID >= indCnt) {
#if RETRO_PLATFORM == RETRO_PSP
                PSPSetFaceCull(scn, vertID, indCnt, mdl, matWorld);
#endif
                scn->vertexCount += mdl->indexCount;
                scn->drawMode = drawMode;
                scn->faceCount += indCnt / mdl->faceVertCount;

                int32 nextFrame = animator->frameID + 1;
                if (nextFrame >= animator->frameCount)
                    nextFrame = animator->loopIndex;
                int32 frameOffset     = animator->frameID * mdl->vertCount;
                int32 nextFrameOffset = nextFrame * mdl->vertCount;

                int32 i           = 0;
                int32 f           = 0;
                int32 interpolate = animator->timer;
                switch (mdl->flags) {
                    default:
                    case MODEL_NOFLAGS:
                    case MODEL_USECOLOURS:
                        for (; i < mdl->indexCount;) {
                            faceVertCounts[f++] = mdl->faceVertCount;

                            for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                i++;
                                Scene3DVertex *vertex = &scn->vertices[vertID++];
                                vertex->x             = matWorld->values[0][3] + (z * matWorld->values[0][2] >> 8) + (matWorld->values[0][0] * x >> 8)
                                            + (matWorld->values[0][1] * y >> 8);
                                vertex->y = matWorld->values[1][3] + (y * matWorld->values[1][1] >> 8) + (z * matWorld->values[1][2] >> 8)
                                            + (matWorld->values[1][0] * x >> 8);
                                vertex->z = matWorld->values[2][3] + ((x * matWorld->values[2][0]) >> 8)
                                            + ((matWorld->values[2][2] * z >> 8) + (matWorld->values[2][1] * y >> 8));
                                vertex->color = color;
                            }
                        }
                        break;

                    case MODEL_USENORMALS:
                        if (matNormals) {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                    ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                    int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                    int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                    int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                    int32 nx                   = frameVert->nx + ((interpolate * (nextFrameVert->nx - frameVert->nx)) >> 8);
                                    int32 ny                   = frameVert->ny + ((interpolate * (nextFrameVert->ny - frameVert->ny)) >> 8);
                                    int32 nz                   = frameVert->nz + ((interpolate * (nextFrameVert->nz - frameVert->nz)) >> 8);
                                    i++;

                                    Scene3DVertex *vertex = &scn->vertices[vertID++];
                                    vertex->x = matWorld->values[0][3] + (z * matWorld->values[0][2] >> 8) + (x * matWorld->values[0][0] >> 8)
                                                + (y * matWorld->values[0][1] >> 8);
                                    vertex->y = matWorld->values[1][3] + (y * matWorld->values[1][1] >> 8) + (matWorld->values[1][0] * x >> 8)
                                                + (z * matWorld->values[1][2] >> 8);
                                    vertex->z = matWorld->values[2][3] + (x * matWorld->values[2][0] >> 8) + (matWorld->values[2][2] * z >> 8)
                                                + (matWorld->values[2][1] * y >> 8);
                                    vertex->nx = (nz * matNormals->values[0][2] >> 8) + (nx * matNormals->values[0][0] >> 8)
                                                 + (matNormals->values[0][1] * ny >> 8);
                                    vertex->ny = (ny * matNormals->values[1][1] >> 8) + (nz * matNormals->values[1][2] >> 8)
                                                 + (nx * matNormals->values[1][0] >> 8);
                                    vertex->nz = ((ny * matNormals->values[2][1]) >> 8)
                                                 + ((matNormals->values[2][0] * nx >> 8) + (nz * matNormals->values[2][2] >> 8));
                                    vertex->color = color;
                                }
                            }
                        }
                        else {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                    ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                    int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                    int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                    int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                    i++;
                                    Scene3DVertex *vertex = &scn->vertices[vertID++];
                                    vertex->x = matWorld->values[0][3] + (z * matWorld->values[0][2] >> 8) + (matWorld->values[0][0] * x >> 8)
                                                + (matWorld->values[0][1] * y >> 8);
                                    vertex->y = matWorld->values[1][3] + (y * matWorld->values[1][1] >> 8) + (z * matWorld->values[1][2] >> 8)
                                                + (matWorld->values[1][0] * x >> 8);
                                    vertex->z = matWorld->values[2][3] + ((matWorld->values[2][2] * z) >> 8)
                                                + ((matWorld->values[2][0] * x >> 8) + (matWorld->values[2][1] * y >> 8));
                                    vertex->color = color;
                                }
                            }
                        }
                        break;

                    case MODEL_USENORMALS | MODEL_USECOLOURS:
                        if (matNormals) {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                    ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                    int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                    int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                    int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                    int32 nx                   = frameVert->nx + ((interpolate * (nextFrameVert->nx - frameVert->nx)) >> 8);
                                    int32 ny                   = frameVert->ny + ((interpolate * (nextFrameVert->ny - frameVert->ny)) >> 8);
                                    int32 nz                   = frameVert->nz + ((interpolate * (nextFrameVert->nz - frameVert->nz)) >> 8);

                                    Color *modelColor     = &mdl->colors[indices[i++]];
                                    Scene3DVertex *vertex = &scn->vertices[vertID++];
                                    vertex->x = matWorld->values[0][3] + (matWorld->values[0][2] * z >> 8) + (y * matWorld->values[0][1] >> 8)
                                                + (matWorld->values[0][0] * x >> 8);
                                    vertex->y = matWorld->values[1][3] + (matWorld->values[1][2] * z >> 8) + (y * matWorld->values[1][1] >> 8)
                                                + (matWorld->values[1][0] * x >> 8);
                                    vertex->z = matWorld->values[2][3] + (x * matWorld->values[2][0] >> 8) + (y * matWorld->values[2][1] >> 8)
                                                + (matWorld->values[2][2] * z >> 8);
                                    vertex->nx = (matNormals->values[0][0] * nx >> 8) + (ny * matNormals->values[0][1] >> 8)
                                                 + (matNormals->values[0][2] * nz >> 8);
                                    vertex->ny = (matNormals->values[1][0] * nx >> 8) + (ny * matNormals->values[1][1] >> 8)
                                                 + (matNormals->values[1][2] * nz >> 8);
                                    vertex->nz = ((matNormals->values[2][2] * nz) >> 8)
                                                 + ((ny * matNormals->values[2][1] >> 8) + (matNormals->values[2][0] * nx >> 8));
                                    vertex->color = modelColor->color;
                                }
                            }
                        }
                        else {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                    ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                    int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                    int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                    int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                    Color *modelColor          = &mdl->colors[indices[i++]];
                                    Scene3DVertex *vertex      = &scn->vertices[vertID++];
                                    vertex->x = matWorld->values[0][3] + (matWorld->values[0][0] * x >> 8) + (y * matWorld->values[0][1] >> 8)
                                                + (z * matWorld->values[0][2] >> 8);
                                    vertex->y = matWorld->values[1][3] + (z * matWorld->values[1][2] >> 8) + (matWorld->values[1][0] * x >> 8)
                                                + (y * matWorld->values[1][1] >> 8);
                                    vertex->z = matWorld->values[2][3] + (matWorld->values[2][2] * z >> 8) + (y * matWorld->values[2][1] >> 8)
                                                + (x * matWorld->values[2][0] >> 8);
                                    vertex->color = modelColor->color;
                                }
                            }
                        }
                        break;
                }
            }
        }
    }
}

#if RETRO_PLATFORM == RETRO_PSP
// Stable sort by descending depth, the exact order of the original insertion sort. Bottom-up merge sort over runs of 8,
// using the unused tail of the face buffer (faces of 2+ vertices never fill more than half of it) as scratch.
static void PSPSortFaces(Scene3DFace *faces, int32 count, int32 capacity)
{
    const int32 run = count * 2 <= capacity ? 8 : count; // no room for scratch: one insertion sort, as originally

    for (int32 lo = 0; lo < count; lo += run) {
        int32 hi = MIN(lo + run, count);
        for (int32 i = lo + 1; i < hi; ++i) {
            Scene3DFace face = faces[i];
            int32 j          = i - 1;
            while (j >= lo && faces[j].depth < face.depth) {
                faces[j + 1] = faces[j];
                --j;
            }
            faces[j + 1] = face;
        }
    }

    if (run >= count)
        return;

    Scene3DFace *src = faces;
    Scene3DFace *dst = faces + count;
    for (int32 width = run; width < count; width *= 2) {
        for (int32 lo = 0; lo < count; lo += width * 2) {
            int32 mid = MIN(lo + width, count);
            int32 hi  = MIN(lo + width * 2, count);
            int32 l = lo, r = mid, o = lo;
            while (l < mid && r < hi) dst[o++] = src[l].depth >= src[r].depth ? src[l++] : src[r++]; // ties: left first
            while (l < mid) dst[o++] = src[l++];
            while (r < hi) dst[o++] = src[r++];
        }

        Scene3DFace *swap = src;
        src               = dst;
        dst               = swap;
    }

    if (src != faces)
        memcpy(faces, src, sizeof(Scene3DFace) * count);
}
#endif

void RSDK::Draw3DScene(uint16 sceneID)
{
    if (sceneID < SCENE3D_COUNT) {
#if RETRO_PLATFORM == RETRO_PSP
        unsigned long long drawStart = PSPProfFine();
#endif
        Entity *entity = sceneInfo.entity;
        Scene3D *scn   = &scene3DList[sceneID];
#if RETRO_PLATFORM == RETRO_PSP
        // translucent models show their back faces through the front ones
        const int8 *cullFaces = entity->inkEffect == INK_NONE ? scn->pspFaceCull : NULL;
        PSPDrawModelRecords(sceneID, scn, entity);
#endif

        // Setup face buffer.
        // Each face's depth is an average of the depth of its vertices.
        Scene3DVertex *vertices = scn->vertices;
        Scene3DFace *faceBuffer = scn->faceBuffer;
        uint8 *faceVertCounts = scn->faceVertCounts;

        int32 vertIndex = 0;
        for (int32 i = 0; i < scn->faceCount; ++i) {
            switch (*faceVertCounts) {
                default:
                case 1:
                    faceBuffer->depth = vertices[0].z;
                    vertices += *faceVertCounts;
                    break;

                case 2:
                    faceBuffer->depth = vertices[0].z >> 1;
                    faceBuffer->depth += vertices[1].z >> 1;
                    vertices += 2;
                    break;

                case 3:
                    faceBuffer->depth = vertices[0].z >> 1;
                    faceBuffer->depth = (faceBuffer->depth + (vertices[1].z >> 1)) >> 1;
                    faceBuffer->depth += vertices[2].z >> 1;
                    vertices += 3;
                    break;

                case 4:
                    faceBuffer->depth = vertices[0].z >> 2;
                    faceBuffer->depth += vertices[1].z >> 2;
                    faceBuffer->depth += vertices[2].z >> 2;
                    faceBuffer->depth += vertices[3].z >> 2;
                    vertices += 4;
                    break;
            }

            faceBuffer->index = vertIndex;
            vertIndex += *faceVertCounts;

            ++faceBuffer;
            ++faceVertCounts;
        }

        // Sort the face buffer. This is needed so that the faces don't overlap each other incorrectly when they're rendered.
        // This is an insertion sort, taken from here:
        // https://web.archive.org/web/20110108233032/http://rosettacode.org/wiki/Sorting_algorithms/Insertion_sort#C

        Scene3DFace *a = scn->faceBuffer;

#if RETRO_PLATFORM == RETRO_PSP
        // the insertion sort below is O(n^2) on models with hundreds of faces; same (stable, descending) order
        PSPSortFaces(a, scn->faceCount, scn->vertLimit);
        unsigned long long sortEnd = PSPProfFine();
        pspS3DTime[1] += sortEnd - drawStart;
        struct RasterTimer {
            unsigned long long start;
            ~RasterTimer() { pspS3DTime[2] += PSPProfFine() - start; }
        } rasterTimer = { sortEnd };
#else
        int i, j;
        Scene3DFace temp;

        for(i=1; i<scn->faceCount; i++)
        {
            temp = a[i];
            j = i-1;
            while(j>=0 && a[j].depth < temp.depth)
            {
                a[j+1] = a[j];
                j -= 1;
            }
            a[j+1] = temp;
        }
#endif

        // Finally, display the faces.

        uint8 *vertCnt = scn->faceVertCounts;
        Vector2 vertPos[4];
        uint32 vertClrs[4];

        switch (scn->drawMode) {
            default: break;

            case S3D_WIREFRAME:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    for (int32 v = 0; v < *vertCnt - 1; ++v) {
                        DrawLine(drawVert[v + 0].x << 8, drawVert[v + 0].y << 8, drawVert[v + 1].x << 8, drawVert[v + 1].y << 8, drawVert[0].color,
                                 entity->alpha, entity->inkEffect, false);
                    }
                    DrawLine(drawVert[0].x << 8, drawVert[0].y << 8, drawVert[*vertCnt - 1].x << 8, drawVert[*vertCnt - 1].y << 8, drawVert[0].color,
                             entity->alpha, entity->inkEffect, false);
                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    for (int32 v = 0; v < *vertCnt; ++v) {
                        vertPos[v].x = (drawVert[v].x << 8) - (currentScreen->position.x << 16);
                        vertPos[v].y = (drawVert[v].y << 8) - (currentScreen->position.y << 16);
                    }
                    DrawFace(vertPos, *vertCnt, (drawVert->color >> 16) & 0xFF, (drawVert->color >> 8) & 0xFF, (drawVert->color >> 0) & 0xFF,
                             entity->alpha, entity->inkEffect);
                    vertCnt++;
                }
                break;

            // Might have been reserved for textures?
            // not sure about this, just a guess based on tex coords existing in the model format spec
            case S3D_UNUSED_1: break;
            case S3D_UNUSED_2: break;

            case S3D_WIREFRAME_SHADED:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 ny1 = 0;
                    for (int32 v = 0; v < vertCount; ++v) {
                        ny1 += drawVert[v].ny;
                    }

                    int32 normal    = ny1 / vertCount;
                    int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                    int32 specular = normalVal >> 6 >> scn->specularIntensityX;
                    specular       = CLAMP(specular, 0x00, 0xFF);
                    int32 r = specular + ((int32)((drawVert->color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                    specular = normalVal >> 6 >> scn->specularIntensityY;
                    specular = CLAMP(specular, 0x00, 0xFF);
                    int32 g  = specular + ((int32)((drawVert->color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                    specular = normalVal >> 6 >> scn->specularIntensityZ;
                    specular = CLAMP(specular, 0x00, 0xFF);
                    int32 b  = specular + ((int32)((drawVert->color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                    r = CLAMP(r, 0x00, 0xFF);
                    g = CLAMP(g, 0x00, 0xFF);
                    b = CLAMP(b, 0x00, 0xFF);

                    uint32 color = (r << 16) | (g << 8) | (b << 0);

                    for (int32 v = 0; v < vertCount - 1; ++v) {
                        DrawLine(drawVert[v + 0].x << 8, drawVert[v + 0].y << 8, drawVert[v + 1].x << 8, drawVert[v + 1].y << 8, color, entity->alpha,
                                 entity->inkEffect, false);
                    }
                    DrawLine(drawVert[vertCount - 1].x << 8, drawVert[vertCount - 1].y << 8, drawVert[0].x << 8, drawVert[0].y << 8, color,
                             entity->alpha, entity->inkEffect, false);

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SHADED:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 ny = 0;
                    for (int32 v = 0; v < vertCount; ++v) {
                        ny += drawVert[v].ny;
                        vertPos[v].x = (drawVert[v].x << 8) - (currentScreen->position.x << 16);
                        vertPos[v].y = (drawVert[v].y << 8) - (currentScreen->position.y << 16);
                    }

                    int32 normal    = ny / vertCount;
                    int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                    int32 specular = normalVal >> 6 >> scn->specularIntensityX;
                    specular       = CLAMP(specular, 0x00, 0xFF);
                    int32 r = specular + ((int32)((drawVert->color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                    specular = normalVal >> 6 >> scn->specularIntensityY;
                    specular = CLAMP(specular, 0x00, 0xFF);
                    int32 g  = specular + ((int32)((drawVert->color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                    specular = normalVal >> 6 >> scn->specularIntensityZ;
                    specular = CLAMP(specular, 0x00, 0xFF);
                    int32 b  = specular + ((int32)((drawVert->color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                    r = CLAMP(r, 0x00, 0xFF);
                    g = CLAMP(g, 0x00, 0xFF);
                    b = CLAMP(b, 0x00, 0xFF);

                    uint32 color = (r << 16) | (g << 8) | (b << 0);

                    drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    DrawFace(vertPos, *vertCnt, (color >> 16) & 0xFF, (color >> 8) & 0xFF, (color >> 0) & 0xFF, entity->alpha, entity->inkEffect);

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SHADED_BLENDED:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    for (int32 v = 0; v < vertCount; ++v) {
                        vertPos[v].x = (drawVert[v].x << 8) - (currentScreen->position.x << 16);
                        vertPos[v].y = (drawVert[v].y << 8) - (currentScreen->position.y << 16);

                        int32 normal    = drawVert[v].ny;
                        int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                        int32 specular = (normalVal >> 6) >> scn->specularIntensityX;
                        specular       = CLAMP(specular, 0x00, 0xFF);
                        int32 r = specular + ((int32)((drawVert->color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                        specular = (normalVal >> 6) >> scn->specularIntensityY;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 g  = specular + ((int32)((drawVert->color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                        specular = (normalVal >> 6) >> scn->specularIntensityZ;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 b  = specular + ((int32)((drawVert->color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                        r = CLAMP(r, 0x00, 0xFF);
                        g = CLAMP(g, 0x00, 0xFF);
                        b = CLAMP(b, 0x00, 0xFF);

                        vertClrs[v] = (r << 16) | (g << 8) | (b << 0);
                    }

                    DrawBlendedFace(vertPos, vertClrs, *vertCnt, entity->alpha, entity->inkEffect);

                    vertCnt++;
                }
                break;

            case S3D_WIREFRAME_SCREEN:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];

                    int32 v = 0;
                    for (; v < *vertCnt && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = currentScreen->center.x + (drawVert[v].x << scn->projectionX) / vertZ;
                            vertPos[v].y = currentScreen->center.y - (drawVert[v].y << scn->projectionY) / vertZ;
                        }
                    }

                    if (v < 0xFF) {
                        for (int32 v = 0; v < *vertCnt - 1; ++v) {
                            DrawLine(vertPos[v + 0].x, vertPos[v + 0].y, vertPos[v + 1].x, vertPos[v + 1].y, drawVert[0].color, entity->alpha,
                                     entity->inkEffect, true);
                        }
                        DrawLine(vertPos[0].x, vertPos[0].y, vertPos[*vertCnt - 1].x, vertPos[*vertCnt - 1].y, drawVert[0].color, entity->alpha,
                                 entity->inkEffect, true);
                    }

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SCREEN:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 v = 0;
                    for (; v < vertCount && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = (currentScreen->center.x << 16) + ((drawVert[v].x << scn->projectionX) / vertZ << 16);
                            vertPos[v].y = (currentScreen->center.y << 16) - ((drawVert[v].y << scn->projectionY) / vertZ << 16);
                        }
                    }

#if RETRO_PLATFORM == RETRO_PSP
                    if (v < 0xFF) {
                        if (PSPIsBackFace(cullFaces, scn->faceBuffer[f].index, vertPos, vertCount)) {
                            v = 0xFF;
                            ++pspFacesCulled;
                        }
                        else {
                            ++pspFacesDrawn;
                        }
                    }
#endif
                    if (v < 0xFF) {
#if RETRO_PLATFORM == RETRO_PSP
                        if (!PSPQueue3DFace(vertPos, NULL, drawVert[0].color & 0xFFFFFF, *vertCnt, entity->alpha, entity->inkEffect))
#endif
                        DrawFace(vertPos, *vertCnt, (drawVert[0].color >> 16) & 0xFF, (drawVert[0].color >> 8) & 0xFF,
                                 (drawVert[0].color >> 0) & 0xFF, entity->alpha, entity->inkEffect);
                    }
                    vertCnt++;
                }
                break;

            case S3D_WIREFRAME_SHADED_SCREEN:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 v   = 0;
                    int32 ny1 = 0;
                    for (; v < *vertCnt && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = currentScreen->center.x + (drawVert[v].x << scn->projectionX) / vertZ;
                            vertPos[v].y = currentScreen->center.y - (drawVert[v].y << scn->projectionY) / vertZ;
                            ny1 += drawVert[v].ny;
                        }
                    }

                    if (v < 0xFF) {
                        int32 normal    = ny1 / vertCount;
                        int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                        int32 specular = normalVal >> 6 >> scn->specularIntensityX;
                        specular       = CLAMP(specular, 0x00, 0xFF);
                        int32 r = specular + ((int32)((drawVert[0].color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                        specular = normalVal >> 6 >> scn->specularIntensityY;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 g  = specular + ((int32)((drawVert[0].color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                        specular = normalVal >> 6 >> scn->specularIntensityZ;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 b  = specular + ((int32)((drawVert[0].color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                        r = CLAMP(r, 0x00, 0xFF);
                        g = CLAMP(g, 0x00, 0xFF);
                        b = CLAMP(b, 0x00, 0xFF);

                        uint32 color = (r << 16) | (g << 8) | (b << 0);

                        for (int32 v = 0; v < *vertCnt - 1; ++v) {
                            DrawLine(vertPos[v + 0].x, vertPos[v + 0].y, vertPos[v + 1].x, vertPos[v + 1].y, color, entity->alpha, entity->inkEffect,
                                     true);
                        }
                        DrawLine(vertPos[*vertCnt - 1].x, vertPos[*vertCnt - 1].y, vertPos[0].x, vertPos[0].y, color, entity->alpha,
                                 entity->inkEffect, true);
                    }

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SHADED_SCREEN:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 v  = 0;
                    int32 ny = 0;
                    for (; v < vertCount && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = (currentScreen->center.x << 16) + ((drawVert[v].x << scn->projectionX) / vertZ << 16);
                            vertPos[v].y = (currentScreen->center.y << 16) - ((drawVert[v].y << scn->projectionY) / vertZ << 16);
                            ny += drawVert[v].ny;
                        }
                    }

#if RETRO_PLATFORM == RETRO_PSP
                    if (v < 0xFF) {
                        if (PSPIsBackFace(cullFaces, scn->faceBuffer[f].index, vertPos, vertCount)) {
                            v = 0xFF;
                            ++pspFacesCulled;
                        }
                        else {
                            ++pspFacesDrawn;
                        }
                    }
#endif
                    if (v < 0xFF) {
                        int32 normal    = ny / vertCount;
                        int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                        int32 specular = normalVal >> 6 >> scn->specularIntensityX;
                        specular       = CLAMP(specular, 0x00, 0xFF);
                        int32 r = specular + ((int32)((drawVert[0].color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                        specular = normalVal >> 6 >> scn->specularIntensityY;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 g  = specular + ((int32)((drawVert[0].color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                        specular = normalVal >> 6 >> scn->specularIntensityZ;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 b  = specular + ((int32)((drawVert[0].color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                        r = CLAMP(r, 0x00, 0xFF);
                        g = CLAMP(g, 0x00, 0xFF);
                        b = CLAMP(b, 0x00, 0xFF);

                        uint32 color = (r << 16) | (g << 8) | (b << 0);

                        drawVert = &scn->vertices[scn->faceBuffer[f].index];
#if RETRO_PLATFORM == RETRO_PSP
                        if (!PSPQueue3DFace(vertPos, NULL, color, *vertCnt, entity->alpha, entity->inkEffect))
#endif
                        DrawFace(vertPos, *vertCnt, (color >> 16) & 0xFF, (color >> 8) & 0xFF, (color >> 0) & 0xFF, entity->alpha, entity->inkEffect);
                    }

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

#if RETRO_PLATFORM == RETRO_PSP
                    int32 v = 0;
                    // project first, so faces that end up culled skip the lighting
                    for (; v < vertCount && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = (currentScreen->center.x << 16) + ((drawVert[v].x << scn->projectionX) / vertZ << 16);
                            vertPos[v].y = (currentScreen->center.y << 16) - ((drawVert[v].y << scn->projectionY) / vertZ << 16);
                        }
                    }

#if RETRO_PLATFORM == RETRO_PSP
                    if (v < 0xFF) {
                        if (PSPIsBackFace(cullFaces, scn->faceBuffer[f].index, vertPos, vertCount)) {
                            v = 0xFF;
                            ++pspFacesCulled;
                        }
                        else {
                            ++pspFacesDrawn;
                        }
                    }
#endif

                    for (int32 c = 0; v < 0xFF && c < vertCount; ++c) {
                        int32 normal    = drawVert[c].ny;
                        int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                        int32 specular = normalVal >> 6 >> scn->specularIntensityX;
                        specular       = CLAMP(specular, 0x00, 0xFF);
                        int32 r = specular + ((int32)((drawVert[c].color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                        specular = normalVal >> 6 >> scn->specularIntensityY;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 g  = specular + ((int32)((drawVert[c].color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                        specular = normalVal >> 6 >> scn->specularIntensityZ;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 b  = specular + ((int32)((drawVert[c].color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                        r = CLAMP(r, 0x00, 0xFF);
                        g = CLAMP(g, 0x00, 0xFF);
                        b = CLAMP(b, 0x00, 0xFF);

                        vertClrs[c] = (r << 16) | (g << 8) | (b << 0);
                    }
#else
                    int32 v = 0;
                    for (; v < vertCount && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = (currentScreen->center.x << 16) + ((drawVert[v].x << scn->projectionX) / vertZ << 16);
                            vertPos[v].y = (currentScreen->center.y << 16) - ((drawVert[v].y << scn->projectionY) / vertZ << 16);

                            int32 normal    = drawVert[v].ny;
                            int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                            int32 specular = normalVal >> 6 >> scn->specularIntensityX;
                            specular       = CLAMP(specular, 0x00, 0xFF);
                            int32 r =
                                specular + ((int32)((drawVert[v].color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                            specular = normalVal >> 6 >> scn->specularIntensityY;
                            specular = CLAMP(specular, 0x00, 0xFF);
                            int32 g =
                                specular + ((int32)((drawVert[v].color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                            specular = normalVal >> 6 >> scn->specularIntensityZ;
                            specular = CLAMP(specular, 0x00, 0xFF);
                            int32 b =
                                specular + ((int32)((drawVert[v].color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                            r = CLAMP(r, 0x00, 0xFF);
                            g = CLAMP(g, 0x00, 0xFF);
                            b = CLAMP(b, 0x00, 0xFF);

                            vertClrs[v] = (r << 16) | (g << 8) | (b << 0);
                        }
                    }

#endif
                    if (v < 0xFF) {
                        drawVert = &scn->vertices[scn->faceBuffer[f].index];
#if RETRO_PLATFORM == RETRO_PSP
                        if (!PSPQueue3DFace(vertPos, vertClrs, 0, *vertCnt, entity->alpha, entity->inkEffect))
#endif
                        DrawBlendedFace(vertPos, vertClrs, *vertCnt, entity->alpha, entity->inkEffect);
                    }

                    vertCnt++;
                }
                break;
        }
    }
}
