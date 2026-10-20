// ---------------------------------------------------------------------
// RSDK Project: Sonic Mania
// Object Description: MagSpikeBall Object
// Object Author: Christian Whitehead/Simon Thomley/Hunter Bridges
// Decompiled by: Rubberduckycooly & RMGRich
// ---------------------------------------------------------------------

#include "Game.h"
#include "PSPFBZSlots.h"

ObjectMagSpikeBall *MagSpikeBall;

void MagSpikeBall_Update(void)
{
    RSDK_THIS(MagSpikeBall);

    self->position.y += self->velocity.y;
    if (self->direction) {
        self->velocity.y -= 0x3800;
        if (RSDK.ObjectTileCollision(self, Zone->collisionLayers, CMODE_ROOF, 0, 0, -0xC0000, true))
            self->velocity.y = 0;

        self->direction = FLIP_NONE;
    }
    else {
        self->velocity.y += 0x3800;
        if (self->velocity.y <= 0 && RSDK.ObjectTileCollision(self, Zone->collisionLayers, CMODE_ROOF, 0, 0, -0xC0000, true))
            self->velocity.y = 0;

        bool32 collided = RSDK.ObjectTileCollision(self, Zone->collisionLayers, CMODE_FLOOR, 0, 0, 0xC0000, true);

#if defined(__psp__)
        PSPFBZSlots *platforms = PSPFBZ_Slots(PSP_FBZ_MAGPLATFORM); // (see PSPFBZSlots.h)
        for (int32 i = 0; i < platforms->count; ++i) {
            EntityMagPlatform *platform = RSDK_GET_ENTITY(platforms->slots[i], MagPlatform);
            if (platform->classID != MagPlatform->classID)
                continue;
#else
        foreach_all(MagPlatform, platform)
        {
#endif
            platform->position.x = platform->drawPos.x - platform->collisionOffset.x;
            platform->position.y = platform->drawPos.y - platform->collisionOffset.y;
            Hitbox *hitbox       = RSDK.GetHitbox(&platform->animator, 0);
            collided |= RSDK.CheckObjectCollisionPlatform(platform, hitbox, self, &MagSpikeBall->hitboxSpikeBall, true);

            platform->position.x = platform->centerPos.x;
            platform->position.y = platform->centerPos.y;
        }

        if (collided) {
            self->velocity.y = 0;
            self->active     = ACTIVE_BOUNDS;
        }
    }

    foreach_active(Player, player)
    {
        if (Player_CheckCollisionTouch(player, self, &MagSpikeBall->hitboxSpikeBall)) {
#if MANIA_USE_PLUS
            if (player->onGround && (self->velocity.y > 0 || !Player_CheckMightyUnspin(player, 0x400, true, &player->uncurlTimer)))
#endif
                Player_Hurt(player, self);
        }
    }
}

void MagSpikeBall_LateUpdate(void) {}

void MagSpikeBall_StaticUpdate(void) {}

void MagSpikeBall_Draw(void)
{
    RSDK_THIS(MagSpikeBall);

    RSDK.DrawSprite(&self->animator, NULL, false);
}

void MagSpikeBall_Create(void *data)
{
    RSDK_THIS(MagSpikeBall);

    self->active        = ACTIVE_BOUNDS;
    self->visible       = true;
    self->updateRange.x = 0x400000;
    self->updateRange.y = 0x400000;
    self->drawGroup     = Zone->objectDrawGroup[0];

    RSDK.SetSpriteAnimation(MagSpikeBall->aniFrames, 0, &self->animator, true, 0);
}

void MagSpikeBall_StageLoad(void)
{
    if (RSDK.CheckSceneFolder("FBZ"))
        MagSpikeBall->aniFrames = RSDK.LoadSpriteAnimation("FBZ/MagSpikeBall.bin", SCOPE_STAGE);

    MagSpikeBall->hitboxSpikeBall.left   = -12;
    MagSpikeBall->hitboxSpikeBall.top    = -12;
    MagSpikeBall->hitboxSpikeBall.right  = 12;
    MagSpikeBall->hitboxSpikeBall.bottom = 12;
}

#if GAME_INCLUDE_EDITOR
void MagSpikeBall_EditorDraw(void) { MagSpikeBall_Draw(); }

void MagSpikeBall_EditorLoad(void) { MagSpikeBall->aniFrames = RSDK.LoadSpriteAnimation("FBZ/MagSpikeBall.bin", SCOPE_STAGE); }
#endif

void MagSpikeBall_Serialize(void) {}
