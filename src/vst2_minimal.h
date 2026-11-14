#pragma once
// Small VST2 binary ABI declaration, independently written for interoperability.
// This is not a copy of a Steinberg SDK header.
#include <cstdint>
#include <cstddef>

struct AEffect;
using VstInt32 = std::int32_t;
using VstIntPtr = std::intptr_t;
using HostCallback = VstIntPtr (*)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
using Dispatcher = VstIntPtr (*)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
using Process = void (*)(AEffect*, float**, float**, VstInt32);
using SetParameter = void (*)(AEffect*, VstInt32, float);
using GetParameter = float (*)(AEffect*, VstInt32);
using DoubleProcess = void (*)(AEffect*, double**, double**, VstInt32);

struct AEffect {
    VstInt32 magic;
    Dispatcher dispatcher;
    Process process;
    SetParameter setParameter;
    GetParameter getParameter;
    VstInt32 numPrograms;
    VstInt32 numParams;
    VstInt32 numInputs;
    VstInt32 numOutputs;
    VstInt32 flags;
    VstIntPtr resvd1;
    VstIntPtr resvd2;
    VstInt32 initialDelay;
    VstInt32 realQualities;
    VstInt32 offQualities;
    float ioRatio;
    void* object;
    void* user;
    VstInt32 uniqueID;
    VstInt32 version;
    Process processReplacing;
    DoubleProcess processDoubleReplacing;
    char future[56];
};
static_assert(offsetof(AEffect, processReplacing) == 120, "Incorrect VST2 x64 ABI layout");
static_assert(sizeof(void*) != 8 || sizeof(AEffect) == 192, "Incorrect VST2 x64 ABI size");

constexpr VstInt32 kVstMagic = 0x56737450; // 'VstP'
constexpr VstInt32 kFlagCanReplacing = 1 << 4;
constexpr VstInt32 kUniqueID = 0x53414446; // 'SADF'

// VST2 effect dispatch opcodes that the host may call.
enum : VstInt32 {
    effOpen=0, effClose=1, effSetProgram=2, effGetProgram=3,
    effSetProgramName=4, effGetProgramName=5, effGetParamLabel=6,
    effGetParamDisplay=7, effGetParamName=8, effSetSampleRate=10,
    effSetBlockSize=11, effMainsChanged=12, effGetPlugCategory=35,
    effSetBypass=44, effGetEffectName=45, effGetVendorString=47,
    effGetProductString=48, effGetVendorVersion=49, effCanDo=51,
    effGetTailSize=52, effGetVstVersion=58,
    effStartProcess=71, effStopProcess=72
};
