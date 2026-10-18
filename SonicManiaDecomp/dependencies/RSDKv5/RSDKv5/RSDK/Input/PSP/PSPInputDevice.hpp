namespace SKU
{

struct InputDevicePSP : InputDevice {
    void UpdateInput();
    void ProcessInput(int32 controllerID);
    void CloseDevice();

    int32 buttonMasks;
    int32 prevButtonMasks;
    uint8 stateUp;
    uint8 stateDown;
    uint8 stateLeft;
    uint8 stateRight;
    uint8 stateA;
    uint8 stateB;
    uint8 stateC;
    uint8 stateX;
    uint8 stateY;
    uint8 stateZ;
    uint8 stateStart;
    uint8 stateSelect;
    uint8 stateBumper_L;
    uint8 stateBumper_R;
    float bumperDeltaL;
    float bumperDeltaR;
    float hDelta_L;
    float vDelta_L;
};

InputDevicePSP *InitPSPInputDevice(uint32 id);

void InitPSPInputAPI();

} // namespace SKU
