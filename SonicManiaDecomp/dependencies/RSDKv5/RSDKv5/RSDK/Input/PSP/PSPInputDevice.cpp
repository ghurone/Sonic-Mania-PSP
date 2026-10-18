using namespace RSDK;

#define PSP_ANALOG_DEADZONE (40)

extern int pspAutoTest;

// ---------------------------------------------------------------------------------------------------------------
// Input recording (boot.txt "record=NAME") and replay ("replay=NAME"): the pad, one entry per engine frame, in
// rec_NAME.bin next to the game. Both modes use a fixed random seed (Math.cpp), so a replay plays the same game again:
// the way to re-test real play sessions (special stages, later parts of a zone) unattended.
// Entry: buttons in the low 16 bits, analog x in bits 16-23, y in bits 24-31.
// ---------------------------------------------------------------------------------------------------------------
#define PSP_REC_MAX_FRAMES (60 * 60 * 10) // ten minutes

int pspInputMode = 0; // PSP_INPUT_RECORD / PSP_INPUT_REPLAY
int pspReplayStopAt = 0; // boot.txt "stopat=N": hold the replay's frame N on screen (for screenshots of an exact moment)
static char pspRecPath[0x40];
static uint32 *pspRec;
static uint32 pspRecCount, pspRecFrame, pspRecSaved;

void PSPInputRecordingInit(int mode, const char *name)
{
    pspInputMode = mode;
    sprintf(pspRecPath, "rec_%s.bin", name);

    if (mode == PSP_INPUT_RECORD) {
        pspRec = (uint32 *)malloc(sizeof(uint32) * PSP_REC_MAX_FRAMES);
        if (!pspRec)
            pspInputMode = 0;
        return;
    }

    FILE *file = fopen(pspRecPath, "rb");
    if (file) {
        uint32 header[2] = { 0, 0 };
        if (fread(header, sizeof(uint32), 2, file) == 2 && header[0] == 0x43455250 && header[1] <= PSP_REC_MAX_FRAMES) { // "PREC"
            pspRec = (uint32 *)malloc(sizeof(uint32) * (header[1] ? header[1] : 1));
            if (pspRec)
                pspRecCount = fread(pspRec, sizeof(uint32), header[1], file);
        }
        fclose(file);
    }
    if (!pspRec)
        pspInputMode = 0; // nothing to replay: play normally
}

void PSPSaveInputRecording()
{
    if (pspInputMode != PSP_INPUT_RECORD || pspRecCount == pspRecSaved)
        return;

    FILE *file = fopen(pspRecPath, "wb");
    if (file) {
        uint32 header[2] = { 0x43455250, pspRecCount };
        fwrite(header, sizeof(uint32), 2, file);
        fwrite(pspRec, sizeof(uint32), pspRecCount, file);
        fclose(file);
        pspRecSaved = pspRecCount;
        PrintLog(PRINT_NORMAL, "PSP recording: %u frames saved to %s", pspRecCount, pspRecPath);
    }
}

void RSDK::SKU::InputDevicePSP::UpdateInput()
{
    this->prevButtonMasks = this->buttonMasks;

    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);

    if (pspInputMode == PSP_INPUT_REPLAY) {
        if (pspReplayStopAt && pspRecFrame == (uint32)pspReplayStopAt) {
            // the frame before this one is on screen (or about to be): keep it there, without starving PSPLink
            PrintLog(PRINT_NORMAL, "PSP replay: holding at frame %u", pspRecFrame);
            PSPFlushLog();
            for (;;) sceKernelDelayThread(100000);
        }
        if (pspRecFrame < pspRecCount) {
            uint32 entry = pspRec[pspRecFrame];
            pad.Buttons  = entry & 0xFFFF;
            pad.Lx       = (entry >> 16) & 0xFF;
            pad.Ly       = entry >> 24;
        }
        else {
            if (pspRecFrame == pspRecCount)
                PrintLog(PRINT_NORMAL, "PSP replay: %s finished after %u frames", pspRecPath, pspRecCount);
            pad.Buttons = 0;
            pad.Lx = pad.Ly = 128;
        }
        ++pspRecFrame;
    }

    // scripted pad for unattended runs (boot.txt containing "autotest" or "autoplay")
    static uint32 autoFrame = 0;
    if (::pspAutoTest == 1) {
        ++autoFrame;
        pad.Buttons = 0;
        if (autoFrame > 30 && (autoFrame % 90) < 6)
            pad.Buttons |= PSP_CTRL_START; // skips the title screen / presses through menus
        if (autoFrame > 200) {
            pad.Buttons |= PSP_CTRL_RIGHT;
            if ((autoFrame % 70) < 25)
                pad.Buttons |= PSP_CTRL_CROSS;
        }
    }
    else if (::pspAutoTest == 2) {
        // mostly right with jumps, a second of left every 4s (steering in the special stages),
        // and a pause from 20s to 24s to exercise the pause menu
        ++autoFrame;
        pad.Buttons = 0;
        if (autoFrame > 120) {
            pad.Buttons |= (autoFrame % 240) < 60 ? PSP_CTRL_LEFT : PSP_CTRL_RIGHT;
            if ((autoFrame % 100) < 20)
                pad.Buttons |= PSP_CTRL_CROSS;
        }
        if (autoFrame >= 1200 && autoFrame < 1446) // hands off while paused: only START to pause and resume
            pad.Buttons = (autoFrame < 1206 || autoFrame >= 1440) ? PSP_CTRL_START : 0;
    }

    // recorded after the scripted pads, so a scripted run can be replayed too
    if (pspInputMode == PSP_INPUT_RECORD) {
        if (pspRecFrame < PSP_REC_MAX_FRAMES) {
            pspRec[pspRecFrame] = (pad.Buttons & 0xFFFF) | ((uint32)pad.Lx << 16) | ((uint32)pad.Ly << 24);
            pspRecCount         = pspRecFrame + 1;
        }
        ++pspRecFrame;
        static uint32 prevButtons = 0;
        bool selectPressed = (pad.Buttons & PSP_CTRL_SELECT) && !(prevButtons & PSP_CTRL_SELECT);
        prevButtons        = pad.Buttons;
        if (selectPressed || !(pspRecFrame % (60 * 20)))
            PSPSaveInputRecording(); // SELECT saves right away; otherwise every 20 seconds, in case the session never exits cleanly
    }

    // a record and its replay should log the same positions
    if (pspInputMode && pspRecFrame && !(pspRecFrame % 60)) {
        uint32 sum = 0;
        for (int32 e = 0; e < ENTITY_COUNT; ++e)
            sum = sum * 31 + (uint32)objectEntityList[e].position.x + (uint32)objectEntityList[e].position.y * 7 + objectEntityList[e].classID;
        PrintLog(PRINT_NORMAL, "PSP %s frame %u: player=(%d,%d) rand=%08X entities=%08X", pspInputMode == PSP_INPUT_RECORD ? "record" : "replay",
                 pspRecFrame, objectEntityList[0].position.x >> 16, objectEntityList[0].position.y >> 16, randSeed, sum);
    }

    float prevHDeltaL = hDelta_L;
    float prevVDeltaL = vDelta_L;

    int32 masks = 0;
    if (pad.Buttons & PSP_CTRL_UP)
        masks |= KEYMASK_UP;
    if (pad.Buttons & PSP_CTRL_DOWN)
        masks |= KEYMASK_DOWN;
    if (pad.Buttons & PSP_CTRL_LEFT)
        masks |= KEYMASK_LEFT;
    if (pad.Buttons & PSP_CTRL_RIGHT)
        masks |= KEYMASK_RIGHT;
    if (pad.Buttons & PSP_CTRL_CROSS)
        masks |= KEYMASK_A;
    if (pad.Buttons & PSP_CTRL_CIRCLE)
        masks |= KEYMASK_B;
    if (pad.Buttons & PSP_CTRL_SQUARE)
        masks |= KEYMASK_X;
    if (pad.Buttons & PSP_CTRL_TRIANGLE)
        masks |= KEYMASK_Y;
    if (pad.Buttons & PSP_CTRL_START)
        masks |= KEYMASK_START;
    if (pad.Buttons & PSP_CTRL_SELECT)
        masks |= KEYMASK_SELECT;
    if (pad.Buttons & PSP_CTRL_LTRIGGER)
        masks |= KEYMASK_BUMPERL;
    if (pad.Buttons & PSP_CTRL_RTRIGGER)
        masks |= KEYMASK_BUMPERR;

    // analog nub, also mirrored to the d-pad so menus/gameplay work with either
    int32 ax = (int32)pad.Lx - 128;
    int32 ay = (int32)pad.Ly - 128;

    hDelta_L = 0.0f;
    vDelta_L = 0.0f;
    if (ax > PSP_ANALOG_DEADZONE || ax < -PSP_ANALOG_DEADZONE)
        hDelta_L = ax / 128.0f;
    if (ay > PSP_ANALOG_DEADZONE || ay < -PSP_ANALOG_DEADZONE)
        vDelta_L = -ay / 128.0f; // engine: up is positive

    if (hDelta_L < -0.5f)
        masks |= KEYMASK_LEFT;
    else if (hDelta_L > 0.5f)
        masks |= KEYMASK_RIGHT;
    if (vDelta_L > 0.5f)
        masks |= KEYMASK_UP;
    else if (vDelta_L < -0.5f)
        masks |= KEYMASK_DOWN;

    this->buttonMasks = masks;

    bumperDeltaL = (masks & KEYMASK_BUMPERL) ? 1.0f : 0.0f;
    bumperDeltaR = (masks & KEYMASK_BUMPERR) ? 1.0f : 0.0f;

    int32 changedButtons = ~this->prevButtonMasks & (this->prevButtonMasks ^ this->buttonMasks);

    if (changedButtons || hDelta_L != prevHDeltaL || vDelta_L != prevVDeltaL) {
        this->inactiveTimer[0] = 0;
        this->anyPress         = true;
    }
    else {
        ++this->inactiveTimer[0];
        this->anyPress = false;
    }

    if ((changedButtons & KEYMASK_A) || (changedButtons & KEYMASK_START))
        this->inactiveTimer[1] = 0;
    else
        ++this->inactiveTimer[1];

    this->stateUp       = (masks & KEYMASK_UP) != 0;
    this->stateDown     = (masks & KEYMASK_DOWN) != 0;
    this->stateLeft     = (masks & KEYMASK_LEFT) != 0;
    this->stateRight    = (masks & KEYMASK_RIGHT) != 0;
    this->stateA        = (masks & KEYMASK_A) != 0;
    this->stateB        = (masks & KEYMASK_B) != 0;
    this->stateC        = (masks & KEYMASK_C) != 0;
    this->stateX        = (masks & KEYMASK_X) != 0;
    this->stateY        = (masks & KEYMASK_Y) != 0;
    this->stateZ        = (masks & KEYMASK_Z) != 0;
    this->stateStart    = (masks & KEYMASK_START) != 0;
    this->stateSelect   = (masks & KEYMASK_SELECT) != 0;
    this->stateBumper_L = (masks & KEYMASK_BUMPERL) != 0;
    this->stateBumper_R = (masks & KEYMASK_BUMPERR) != 0;

    ProcessInput(CONT_ANY);
}

void RSDK::SKU::InputDevicePSP::ProcessInput(int32 controllerID)
{
    controller[controllerID].keyUp.press |= this->stateUp;
    controller[controllerID].keyDown.press |= this->stateDown;
    controller[controllerID].keyLeft.press |= this->stateLeft;
    controller[controllerID].keyRight.press |= this->stateRight;
    controller[controllerID].keyA.press |= this->stateA;
    controller[controllerID].keyB.press |= this->stateB;
    controller[controllerID].keyC.press |= this->stateC;
    controller[controllerID].keyX.press |= this->stateX;
    controller[controllerID].keyY.press |= this->stateY;
    controller[controllerID].keyZ.press |= this->stateZ;
    controller[controllerID].keyStart.press |= this->stateStart;
    controller[controllerID].keySelect.press |= this->stateSelect;

#if RETRO_REV02
    stickL[controllerID].keyStick.press |= false;
    stickL[controllerID].hDelta = this->hDelta_L;
    stickL[controllerID].vDelta = this->vDelta_L;
    stickL[controllerID].keyUp.press |= this->vDelta_L > INPUT_DEADZONE;
    stickL[controllerID].keyDown.press |= this->vDelta_L < -INPUT_DEADZONE;
    stickL[controllerID].keyLeft.press |= this->hDelta_L < -INPUT_DEADZONE;
    stickL[controllerID].keyRight.press |= this->hDelta_L > INPUT_DEADZONE;

    triggerL[controllerID].keyBumper.press |= this->stateBumper_L;
    triggerL[controllerID].bumperDelta = this->bumperDeltaL;

    triggerR[controllerID].keyBumper.press |= this->stateBumper_R;
    triggerR[controllerID].bumperDelta = this->bumperDeltaR;
#else
    stickL[controllerID].hDeltaL = this->hDelta_L;
    stickL[controllerID].vDeltaL = this->vDelta_L;
    stickL[controllerID].keyUp.press |= this->vDelta_L > INPUT_DEADZONE;
    stickL[controllerID].keyDown.press |= this->vDelta_L < -INPUT_DEADZONE;
    stickL[controllerID].keyLeft.press |= this->hDelta_L < -INPUT_DEADZONE;
    stickL[controllerID].keyRight.press |= this->hDelta_L > INPUT_DEADZONE;

    controller[controllerID].keyBumperL.press |= this->stateBumper_L;
    controller[controllerID].keyBumperR.press |= this->stateBumper_R;
#endif
}

void RSDK::SKU::InputDevicePSP::CloseDevice()
{
    this->active     = false;
    this->isAssigned = false;
}

RSDK::SKU::InputDevicePSP *RSDK::SKU::InitPSPInputDevice(uint32 id)
{
    if (inputDeviceCount >= INPUTDEVICE_COUNT)
        return NULL;

    if (inputDeviceList[inputDeviceCount] && inputDeviceList[inputDeviceCount]->active)
        return NULL;

    if (inputDeviceList[inputDeviceCount])
        delete inputDeviceList[inputDeviceCount];

    inputDeviceList[inputDeviceCount] = new InputDevicePSP();

    InputDevicePSP *device = (InputDevicePSP *)inputDeviceList[inputDeviceCount];

    device->active      = true;
    device->disabled    = false;
    device->isAssigned  = false;
    device->unused      = 0;
    // PlayStation prompts (cross/circle/square/triangle sit where Xbox A/B/X/Y are, see UpdateInput)
    device->gamepadType = (DEVICE_API_NONE << 16) | (DEVICE_TYPE_CONTROLLER << 8) | (DEVICE_PS4 << 0);
    device->id          = id;
    device->buttonMasks = 0;
    device->prevButtonMasks = 0;
    device->stateUp = device->stateDown = device->stateLeft = device->stateRight = 0;
    device->stateA = device->stateB = device->stateC = device->stateX = 0;
    device->stateY = device->stateZ = device->stateStart = device->stateSelect = 0;
    device->stateBumper_L = device->stateBumper_R = 0;
    device->bumperDeltaL = device->bumperDeltaR = 0.0f;
    device->hDelta_L = device->vDelta_L = 0.0f;
    device->anyPress = false;
    device->inactiveTimer[0] = device->inactiveTimer[1] = 0;

    for (int32 i = 0; i < PLAYER_COUNT; ++i) {
        if (inputSlots[i] == id) {
            inputSlotDevices[i] = device;
            device->isAssigned  = true;
        }
    }

    inputDeviceCount++;
    return device;
}

void RSDK::SKU::InitPSPInputAPI()
{
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    // the handheld's built-in pad is always present
    InitPSPInputDevice(0x01);
}
