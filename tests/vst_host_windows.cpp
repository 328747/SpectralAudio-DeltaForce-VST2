#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include "../src/vst2_minimal.h"
#include <array>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
using MainProc = AEffect* (*)(HostCallback);
static void check(bool test,const char* what){if(!test){std::fprintf(stderr,"FAIL: %s\n",what);std::exit(2);}}
int main(int argc,char** argv){
    check(argc==2,"Provide DLL pathname");
    HMODULE lib=LoadLibraryA(argv[1]);
    if(!lib) std::fprintf(stderr,"LoadLibrary Win32 error=%lu\n",GetLastError());
    check(lib!=nullptr,"LoadLibraryA DLL");
    auto entry=reinterpret_cast<MainProc>(GetProcAddress(lib,"VSTPluginMain"));
    check(entry!=nullptr,"VSTPluginMain export");
    check(GetProcAddress(lib,"main")!=nullptr,"legacy main export");
    AEffect* fx=entry(nullptr);
    check(fx!=nullptr,"original Delta Force AI model is compiled in");
    check(fx->magic==kVstMagic && fx->numInputs==2 && fx->numOutputs==2,"VST2 stereo ABI");
    check(fx->initialDelay==1024 && fx->numParams==4,"VST2 latency/parameters");
    fx->dispatcher(fx,effOpen,0,0,nullptr,0.0f);
    fx->dispatcher(fx,effSetSampleRate,0,0,nullptr,48000.0f);
    fx->dispatcher(fx,effMainsChanged,0,1,nullptr,0.0f);
    constexpr int N=12000;
    std::vector<float> inL(N),inR(N),outL(N),outR(N);
    for(int i=0;i<N;++i){
        inL[i]=0.14f*std::sin(i*0.021f)+0.03f*std::sin(i*.074f);
        inR[i]=0.12f*std::cos(i*0.017f)+0.02f*std::cos(i*.093f);
    }
    auto render=[&](){
        int pos=0,round=0;
        constexpr int sizes[]{17,511,32,1025,1,256,801};
        while(pos<N){
            const int n=std::min(sizes[round++%7],N-pos);
            float* input[]{inL.data()+pos,inR.data()+pos};
            float* output[]{outL.data()+pos,outR.data()+pos};
            fx->processReplacing(fx,input,output,n);
            pos+=n;
        }
    };
    fx->setParameter(fx,3,1.0f);
    render();
    float error=0.0f;
    for(int i=2000;i<N;++i){
        error=std::max(error,std::fabs(outL[i]-inL[i-1024]));
        error=std::max(error,std::fabs(outR[i]-inR[i-1024]));
    }
    check(error<1.e-5f,"bypass delay alignment and arbitrary block sizes");
    fx->dispatcher(fx,effMainsChanged,0,0,nullptr,0);
    fx->setParameter(fx,3,0.0f);
    fx->dispatcher(fx,effMainsChanged,0,1,nullptr,0);
    render();
    float maxAmplitude=0.0f;
    for(int i=3000;i<N;++i){
        check(std::isfinite(outL[i]) && std::isfinite(outR[i]),"finite AI output");
        check(std::fabs(outL[i])<=1.0f && std::fabs(outR[i])<=1.0f,"output sample bounds");
        maxAmplitude=std::max(maxAmplitude,std::fabs(outL[i]));
    }
    check(maxAmplitude>0.000001f,"real model produces non-silent sound");
    fx->dispatcher(fx,effClose,0,0,nullptr,0);
    FreeLibrary(lib);
    std::printf("PASS: Windows x64 VST2 real-model load, DSP output, bypass alignment, non-silent audio.\n");
    return 0;
}
#endif
