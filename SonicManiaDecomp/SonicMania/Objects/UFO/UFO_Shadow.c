// ---------------------------------------------------------------------
// RSDK Project: Sonic Mania
// Object Description: UFO_Shadow Object
// Object Author: Christian Whitehead/Simon Thomley/Hunter Bridges
// Decompiled by: Rubberduckycooly & RMGRich
// ---------------------------------------------------------------------

#include "Game.h"

#if defined(__psp__)
// The frame's shadows a few to a 3D scene instead of one each: they are the same model, colour and blend (so the order
// they overlap in changes nothing), drawn one after another (the temp slots at the end of drawGroup 3). Counted in
// LateUpdate; in Draw the first of a group prepares the scene, every one adds its model, the last draws it. (Not all
// in one: where the GE is what holds the frame back, it then got them all late, at the very end - UFO7.)
static int32 pspShadowCount, pspShadowDrawn;
extern int pspShadowChunk; // boot.txt shadowchunk=N
#endif

ObjectUFO_Shadow *UFO_Shadow;

void UFO_Shadow_Update(void) {}

void UFO_Shadow_LateUpdate(void)
{
    RSDK_THIS(UFO_Shadow);
    Entity *parent = self->parent;

    if (parent->classID) {
        self->position.x = parent->position.x;
        self->position.y = parent->position.y;

        if (RSDK.GetTile(UFO_Setup->playFieldLayer, self->position.x >> 20, self->position.y >> 20) == (uint16)-1 || parent->drawGroup != 4) {
            self->visible = false;
        }
        else {
            self->visible = true;
            int32 x       = self->position.x >> 8;
            int32 z       = self->position.y >> 8;
            Matrix *mat   = &UFO_Camera->matWorld;

            self->zdepth = mat->values[2][3] + (z * mat->values[2][2] >> 8) + (x * mat->values[2][0] >> 8);

            if (self->zdepth >= 0x4000) {
                self->visible =
                    abs((int32)((mat->values[0][3] << 8) + ((z * mat->values[0][2]) & 0xFFFFFF00) + ((x * mat->values[0][0]) & 0xFFFFFF00))
                        / self->zdepth)
                    < 0x100;
            }
        }
#if defined(__psp__)
        if (self->visible && self->zdepth >= 0x4000)
            pspShadowCount++;
#endif
    }
    else {
        destroyEntity(self);
    }
}

void UFO_Shadow_StaticUpdate(void)
{
#if defined(__psp__)
    if (pspShadowDrawn && pspShadowDrawn != pspShadowCount)
        LogHelpers_Print("UFO_Shadow: %d of %d shadows drawn", pspShadowDrawn, pspShadowCount);
    pspShadowCount = pspShadowDrawn = 0;
#endif
}

#if defined(__psp__)
extern int pspSkipShadows; // boot.txt "noshadows" (measuring only)
extern int pspShadowBatchOff; // boot.txt "noshadowbatch" (comparing only)
#endif
void UFO_Shadow_Draw(void)
{
    RSDK_THIS(UFO_Shadow);
#if defined(__psp__)
    if (pspSkipShadows)
        return;
#endif

    if (self->zdepth >= 0x4000) {
        RSDK.MatrixScaleXYZ(&self->matrix, self->shadowScale, 0x100, self->shadowScale);
        RSDK.MatrixTranslateXYZ(&self->matrix, self->position.x, 0, self->position.y, 0);
        RSDK.MatrixMultiply(&self->matrix, &self->matrix, &UFO_Camera->matWorld);

#if defined(__psp__)
        if (pspShadowBatchOff) {
            RSDK.Prepare3DScene(UFO_Shadow->sceneID);
            RSDK.AddModelTo3DScene(UFO_Shadow->modelIndex, UFO_Shadow->sceneID, S3D_SOLIDCOLOR_SCREEN, &self->matrix, 0, 0);
            RSDK.Draw3DScene(UFO_Shadow->sceneID);
            return;
        }
        if (!(pspShadowDrawn % pspShadowChunk))
            RSDK.Prepare3DScene(UFO_Shadow->sceneID);
        RSDK.AddModelTo3DScene(UFO_Shadow->modelIndex, UFO_Shadow->sceneID, S3D_SOLIDCOLOR_SCREEN, &self->matrix, 0, 0);
        if (++pspShadowDrawn >= pspShadowCount || !(pspShadowDrawn % pspShadowChunk))
            RSDK.Draw3DScene(UFO_Shadow->sceneID);
#else
        RSDK.Prepare3DScene(UFO_Shadow->sceneID);
        RSDK.AddModelTo3DScene(UFO_Shadow->modelIndex, UFO_Shadow->sceneID, S3D_SOLIDCOLOR_SCREEN, &self->matrix, 0, 0);
        RSDK.Draw3DScene(UFO_Shadow->sceneID);
#endif
    }
}

void UFO_Shadow_Create(void *data)
{
    RSDK_THIS(UFO_Shadow);

    if (!SceneInfo->inEditor) {
        self->inkEffect     = INK_BLEND;
        self->visible       = true;
        self->drawFX        = FX_SCALE;
        self->drawGroup     = 3;
        self->active        = ACTIVE_RBOUNDS;
        self->updateRange.x = 0x300;
        self->updateRange.y = 0x300;
    }
}

void UFO_Shadow_StageLoad(void)
{
    UFO_Shadow->modelIndex = RSDK.LoadMesh("Special/Shadow.bin", SCOPE_STAGE);
#if defined(__psp__)
    UFO_Shadow->active = ACTIVE_ALWAYS; // (its StaticUpdate starts each frame's count)
#endif
    UFO_Shadow->sceneID    = RSDK.Create3DScene("View:Special", 4096, SCOPE_STAGE);

    int32 slot = TEMPENTITY_START;
    foreach_all(UFO_Player, player)
    {
        EntityUFO_Shadow *shadow = RSDK_GET_ENTITY(slot--, UFO_Shadow);
        RSDK.ResetEntity(shadow, UFO_Shadow->classID, NULL);
        shadow->position.x  = player->position.x;
        shadow->position.y  = player->position.y;
        shadow->parent      = (Entity *)player;
        shadow->shadowScale = 0x140;
    }

    foreach_all(UFO_Circuit, ufo)
    {
        if (ufo->startNode) {
            EntityUFO_Shadow *shadow = RSDK_GET_ENTITY(slot--, UFO_Shadow);
            RSDK.ResetEntity(shadow, UFO_Shadow->classID, NULL);
            shadow->position.x  = ufo->position.x;
            shadow->position.y  = ufo->position.y;
            shadow->parent      = (Entity *)ufo;
            shadow->shadowScale = 0x400;
        }
    }

    foreach_all(UFO_Ring, ring)
    {
        EntityUFO_Shadow *shadow = RSDK_GET_ENTITY(slot--, UFO_Shadow);
        RSDK.ResetEntity(shadow, UFO_Shadow->classID, NULL);
        shadow->position.x  = ring->position.x;
        shadow->position.y  = ring->position.y;
        shadow->parent      = (Entity *)ring;
        shadow->shadowScale = 0xC0;
    }

    foreach_all(UFO_Sphere, sphere)
    {
        EntityUFO_Shadow *shadow = RSDK_GET_ENTITY(slot--, UFO_Shadow);
        RSDK.ResetEntity(shadow, UFO_Shadow->classID, NULL);
        shadow->position.x  = sphere->position.x;
        shadow->position.y  = sphere->position.y;
        shadow->parent      = (Entity *)sphere;
        shadow->shadowScale = 0x100;
    }

    LogHelpers_Print("%d shadow entities spawned", TEMPENTITY_START - slot);
}

#if GAME_INCLUDE_EDITOR
void UFO_Shadow_EditorDraw(void) {}

void UFO_Shadow_EditorLoad(void) {}
#endif

void UFO_Shadow_Serialize(void) {}
