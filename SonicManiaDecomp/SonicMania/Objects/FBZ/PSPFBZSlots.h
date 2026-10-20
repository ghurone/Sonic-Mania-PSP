#ifndef PSP_FBZ_SLOTS_H
#define PSP_FBZ_SLOTS_H

#if defined(__psp__)
// foreach_all walks all ENTITY_COUNT (2368) slots, a cache miss each on the PSP: ~0.35ms a call. Every active
// ElectroMagnet ran three a frame (the stage's magnets all switch on together, and stay updated off-screen while on),
// and every MagSpikeBall they pull runs one a frame until it lands - for good when it never does, which slowed Flying
// Battery down until the stage was reloaded. Instead, the slots of those classes are gathered with one walk a frame,
// in slot order like foreach_all, and each use checks the slot still holds that class. (An entity created later in the
// same frame is seen from the next frame on.)
enum { PSP_FBZ_BLASTER, PSP_FBZ_MAGSPIKEBALL, PSP_FBZ_MAGPLATFORM, PSP_FBZ_CLASSES };

typedef struct {
    int32 count;
    uint16 slots[ENTITY_COUNT];
} PSPFBZSlots;

extern int32 pspFrameIndex; // (PSPRenderDevice.cpp: frames shown so far)

static PSPFBZSlots pspFBZSlots[PSP_FBZ_CLASSES];
static int32 pspFBZSlotsFrame = -1, pspFBZSlotsTimer = -1;

static PSPFBZSlots *PSPFBZ_Slots(int32 which)
{
    if (pspFBZSlotsFrame != pspFrameIndex || pspFBZSlotsTimer != Zone->timer) {
        const uint16 classes[PSP_FBZ_CLASSES] = { Blaster->classID, MagSpikeBall->classID, MagPlatform->classID };
        for (int32 c = 0; c < PSP_FBZ_CLASSES; ++c) pspFBZSlots[c].count = 0;

        for (int32 slot = 0; slot < ENTITY_COUNT; ++slot) {
            uint16 classID = RSDK_GET_ENTITY_GEN(slot)->classID;
            for (int32 c = 0; c < PSP_FBZ_CLASSES; ++c) {
                if (classID == classes[c])
                    pspFBZSlots[c].slots[pspFBZSlots[c].count++] = (uint16)slot;
            }
        }
        pspFBZSlotsFrame = pspFrameIndex;
        pspFBZSlotsTimer = Zone->timer;
    }
    return &pspFBZSlots[which];
}
#endif

#endif
