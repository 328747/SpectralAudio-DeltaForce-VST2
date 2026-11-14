#include "vst2_minimal.h"
#include "spectral_cpu.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <cstdint>

namespace {
constexpr int kHop = Spectral_HOP;      // 512, exact source DSP contract
constexpr int kDelayFrames = 2 * kHop;  // 1024 = staging + source DSP latency
constexpr int kChannels = 2;
constexpr int kParams = 4;

// Host controls have normalized [0,1] ranges, like all VST2 parameters.
// 0: Strength [0..200%], 1: Wet [0..100%], 2: Output trim [-12..+6 dB], 3: Bypass.
float clean(float value) noexcept {
    return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
}
void copy_text(void* dst, const char* src, std::size_t max) noexcept {
    if (!dst || max == 0) return;
    auto* p = static_cast<char*>(dst);
    std::snprintf(p, max, "%s", src);
}

class Plugin {
public:
    AEffect effect{};
    explicit Plugin() {
        parameters_[0].store(0.5f); // 100% AI
        parameters_[1].store(1.0f); // wet 100%
        parameters_[2].store(2.0f/3.0f); // 0 dB output trim
        parameters_[3].store(0.0f); // not bypassed
        effect.magic = kVstMagic;
        effect.dispatcher = dispatch;
        effect.process = process;
        effect.setParameter = set_parameter;
        effect.getParameter = get_parameter;
        effect.numPrograms = 1;
        effect.numParams = kParams;
        effect.numInputs = kChannels;
        effect.numOutputs = kChannels;
        effect.flags = kFlagCanReplacing;
        effect.initialDelay = kDelayFrames;
        effect.object = this;
        effect.uniqueID = kUniqueID;
        effect.version = 100;
        effect.processReplacing = process;
        if (spectral_model_available(2, 4)) stream_ = spectral_stream_create(2, 4, kChannels, 0);
        reset();
    }
    ~Plugin() { if (stream_) spectral_stream_destroy(stream_); }
    bool valid() const noexcept { return stream_ != nullptr; }

private:
    spectral_stream* stream_ = nullptr;
    std::array<std::atomic<float>,kParams> parameters_{};
    float sampleRate_ = 48000.0f;
    bool active_ = true;
    std::array<float, kHop*kChannels> stagedIn_{};
    std::array<float, kHop*kChannels> readyWet_{};
    std::array<float, kDelayFrames*kChannels> dryDelay_{};
    int stagedCount_ = 0;
    int wetRead_ = kHop; // no processed block at startup
    int dryCursor_ = 0;
    float wetSmoothed_ = 1.0f;
    bool lastWetValid_ = false;
    std::atomic<bool> hostBypass_{false};

    void reset() noexcept {
        stagedIn_.fill(0.0f);
        readyWet_.fill(0.0f);
        dryDelay_.fill(0.0f);
        stagedCount_ = 0;
        wetRead_ = kHop;
        dryCursor_ = 0;
        lastWetValid_ = false;
        wetSmoothed_ = 0.0f; // fade in initial DSP after buffer is full
        if (stream_) spectral_stream_reset(stream_);
    }
    void render(float** inputs, float** outputs, VstInt32 count) noexcept {
        if (!outputs || !outputs[0] || !outputs[1] || count <= 0) return;
        const float strength = parameters_[0].load(std::memory_order_relaxed) * 2.0f;
        const float wetTarget = parameters_[1].load(std::memory_order_relaxed);
        const float normalizedTrim = parameters_[2].load(std::memory_order_relaxed);
        const bool bypass = hostBypass_.load(std::memory_order_relaxed) ||
                            parameters_[3].load(std::memory_order_relaxed) >= 0.5f ||
                            std::fabs(sampleRate_ - 48000.0f) > 0.5f || !active_;
        const float gainDb = -12.0f + 18.0f * normalizedTrim;
        const float outputGain = bypass ? 1.0f : std::pow(10.0f, gainDb / 20.0f);
        // Independent wet/bypass transition: prevents clicks when changing the mode.
        const float targetBlend = bypass ? 0.0f : wetTarget;
        if (stream_ && !bypass) spectral_stream_set_strength(stream_,strength);

        for (VstInt32 i = 0; i < count; ++i) {
            const float left = clean(inputs && inputs[0] ? inputs[0][i] : 0.0f);
            const float right = clean(inputs && inputs[1] ? inputs[1][i] : 0.0f);
            const int dryIndex = dryCursor_ * 2;
            const float dryLeft = dryDelay_[dryIndex];
            const float dryRight = dryDelay_[dryIndex+1];
            dryDelay_[dryIndex] = left;
            dryDelay_[dryIndex+1] = right;
            if (++dryCursor_ == kDelayFrames) dryCursor_ = 0;

            const float wetLeft = wetRead_ < kHop && lastWetValid_ ? readyWet_[wetRead_*2] : dryLeft;
            const float wetRight = wetRead_ < kHop && lastWetValid_ ? readyWet_[wetRead_*2+1] : dryRight;
            if (wetRead_ < kHop) ++wetRead_;

            wetSmoothed_ += 0.004f * (targetBlend - wetSmoothed_);
            const float blend = std::clamp(wetSmoothed_, 0.0f, 1.0f);
            outputs[0][i] = clean((dryLeft+(wetLeft-dryLeft)*blend)*outputGain);
            outputs[1][i] = clean((dryRight+(wetRight-dryRight)*blend)*outputGain);

            stagedIn_[stagedCount_*2] = left;
            stagedIn_[stagedCount_*2+1] = right;
            if (++stagedCount_ == kHop) {
                // Always advance the DSP, even while bypassed, so reactivation
                // does not reuse stale neural-network history.
                lastWetValid_ = (stream_ && std::fabs(sampleRate_-48000.0f)<0.5f &&
                    spectral_stream_process(stream_,stagedIn_.data(),readyWet_.data()) == 0);
                wetRead_ = 0;
                stagedCount_ = 0;
            }
        }
    }
    static Plugin* instance(AEffect* fx) noexcept {
        return fx ? static_cast<Plugin*>(fx->object) : nullptr;
    }
    static VstIntPtr dispatch(AEffect* fx, VstInt32 opcode, VstInt32 index,
                              VstIntPtr value, void* ptr, float opt) noexcept {
        Plugin* self = instance(fx);
        if (!self) return 0;
        switch (opcode) {
            case effOpen: return 1;
            case effClose: delete self; return 1;
            case effSetProgram: return index == 0 ? 1 : 0;
            case effGetProgram: return 0;
            case effSetProgramName: return 1;
            case effGetProgramName: copy_text(ptr,"Delta Force",24); return 1;
            case effGetParamLabel:
                if (index == 0 || index == 1) copy_text(ptr,"%",8);
                else if(index == 2) copy_text(ptr,"dB",8);
                else copy_text(ptr,"",8);
                return 1;
            case effGetParamDisplay: {
                char text[24];
                const float p = (index>=0 && index<kParams) ? self->parameters_[index].load() : 0.0f;
                if (index == 0) std::snprintf(text,sizeof(text),"%.1f",p*200.0f);
                else if (index == 1) std::snprintf(text,sizeof(text),"%.1f",p*100.0f);
                else if (index == 2) std::snprintf(text,sizeof(text),"%+.1f",p*18.0f-12.0f);
                else std::snprintf(text,sizeof(text),"%s",p>=0.5f?"On":"Off");
                copy_text(ptr,text,16); return 1;
            }
            case effGetParamName: {
                constexpr const char* names[kParams]={"AI Strength","Wet Mix","Output Trim","Bypass"};
                copy_text(ptr,(index>=0 && index<kParams)?names[index]:"",16); return 1;
            }
            case effSetSampleRate:
                if (opt > 0) {self->sampleRate_=opt;self->reset();} return 1;
            case effSetBlockSize: return 1;
            case effMainsChanged:
                self->active_ = value != 0;
                if (self->active_) self->reset();
                return 1;
            case effGetPlugCategory: return 1; // effect
            case effSetBypass: self->hostBypass_.store(value!=0); return 1;
            case effGetEffectName: copy_text(ptr,"SpectralAudio DeltaForce",32); return 1;
            case effGetVendorString: copy_text(ptr,"Community VST Wrapper",64); return 1;
            case effGetProductString: copy_text(ptr,"SA DeltaForce VST2",64); return 1;
            case effGetVendorVersion: return 100;
            case effCanDo:
                if (ptr && std::strcmp(static_cast<const char*>(ptr),"bypass")==0) return 1;
                return 0;
            case effGetTailSize: return kDelayFrames;
            case effGetVstVersion: return 2400;
            case effStartProcess: case effStopProcess: return 1;
            default: return 0;
        }
    }
    static void process(AEffect* fx, float** inputs, float** outputs, VstInt32 count) noexcept {
        Plugin* self = instance(fx);
        if (self) self->render(inputs,outputs,count);
    }
    static void set_parameter(AEffect* fx, VstInt32 index, float value) noexcept {
        Plugin* self = instance(fx);
        if (self && index>=0 && index<kParams && std::isfinite(value))
            self->parameters_[index].store(std::clamp(value,0.0f,1.0f),std::memory_order_relaxed);
    }
    static float get_parameter(AEffect* fx,VstInt32 index) noexcept {
        Plugin* self=instance(fx);
        return (self && index>=0 && index<kParams) ? self->parameters_[index].load(std::memory_order_relaxed) : 0.0f;
    }
};
}

#ifdef _WIN32
#define EXPORT_PLUGIN extern "C" __declspec(dllexport)
#else
#define EXPORT_PLUGIN extern "C" __attribute__((visibility("default")))
#endif
EXPORT_PLUGIN AEffect* VSTPluginMain(HostCallback /*host*/) noexcept {
    auto* plugin = new(std::nothrow) Plugin();
    if (!plugin) return nullptr;
    if (!plugin->valid()) { delete plugin; return nullptr; }
    return &plugin->effect;
}
