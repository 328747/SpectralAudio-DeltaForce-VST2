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
    check(fx->initialDelay==1024 && fx->numParams==5,"VST2 latency/parameters");
    fx->dispatcher(fx,effOpen,0,0,nullptr,0.0f);
    // Explicitly exercise the VST2 editor API; the previous build passed
    // DSP smoke tests but crashed when Equalizer APO requested its GUI.
    check((fx->flags & (1 << 0)) != 0, "VST2 editor capability flag");
    struct EditorRect { short top,left,bottom,right; };
    EditorRect* rect=nullptr;
    check(fx->dispatcher(fx,13,0,0,&rect,0.0f)!=0 && rect!=nullptr,
          "effEditGetRect gives a valid rectangle");
    check(rect->right > rect->left && rect->bottom > rect->top,
          "VST2 editor dimensions are valid");
    HWND hostWindow=CreateWindowExW(0,L"STATIC",L"VST host smoke",
                    WS_OVERLAPPEDWINDOW,0,0,500,400,nullptr,nullptr,
                    GetModuleHandleW(nullptr),nullptr);
    check(hostWindow!=nullptr,"Create test host window");
    check(fx->dispatcher(fx,14,0,0,hostWindow,0.0f)!=0,
          "effEditOpen works with a host window");
    HWND editor=FindWindowExW(hostWindow,nullptr,L"SpectralDeltaForceVST2MonitorV2",nullptr);
    check(editor!=nullptr,"Editor child window created");
    check(SetDlgItemTextW(editor,100,L"75.0")!=0,"Update AI strength field");
    SendMessageW(editor,WM_COMMAND,MAKEWPARAM(110,BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(editor,110)));
    check(std::fabs(fx->getParameter(fx,0)-0.375f)<0.001f,
          "Apply button updates normalized AI strength");
    check(SetDlgItemTextW(editor,101,L"-6.0")!=0,"Update output gain field");
    SendMessageW(editor,WM_COMMAND,MAKEWPARAM(110,BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(editor,110)));
    check(std::fabs(fx->getParameter(fx,2)-1.0f/3.0f)<0.001f,
          "Apply button updates output trim");
    SendMessageW(editor,WM_TIMER,27,0);
    fx->dispatcher(fx,19,0,0,nullptr,0.0f);
    check(fx->dispatcher(fx,15,0,0,nullptr,0.0f)!=0,
          "effEditClose closes the editor");
    DestroyWindow(hostWindow);
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
    fx->setParameter(fx,4,0.0f);
    fx->setParameter(fx,2,2.0f/3.0f);
    fx->setParameter(fx,3,1.0f);
    render();
    float error=0.0f;
    for(int i=2000;i<N;++i){
        error=std::max(error,std::fabs(outL[i]-inL[i-1024]));
        error=std::max(error,std::fabs(outR[i]-inR[i-1024]));
    }
    check(error<1.e-5f,"bypass delay alignment and arbitrary block sizes");
    fx->setParameter(fx,4,1.0f);
    render();
    float mutedPeak=0.0f;
    for(int i=4000;i<N;++i) mutedPeak=std::max(mutedPeak,std::fabs(outL[i]));
    check(mutedPeak<1.e-5f,"new mute control silences output");
    fx->setParameter(fx,4,0.0f);
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
    // Check GDI paint + timer with actual audio data (not just opening an empty panel).
    HWND host2=CreateWindowExW(0,L"STATIC",L"Spectrum host",WS_OVERLAPPEDWINDOW,
        0,0,960,820,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(host2!=nullptr,"second test host window");
    check(fx->dispatcher(fx,14,0,0,host2,0.0f)!=0,"reopen spectrum editor");
    HWND spectrumEditor=FindWindowExW(host2,nullptr,L"SpectralDeltaForceVST2MonitorV2",nullptr);
    check(spectrumEditor!=nullptr,"spectrum editor created");
    render();  // ensures visualization tap sees samples while the panel is open
    SendMessageW(spectrumEditor,WM_TIMER,27,0);
    InvalidateRect(spectrumEditor,nullptr,FALSE);
    UpdateWindow(spectrumEditor);
    fx->dispatcher(fx,15,0,0,nullptr,0.0f);
    DestroyWindow(host2);
    fx->dispatcher(fx,effClose,0,0,nullptr,0);
    FreeLibrary(lib);
    std::printf("PASS: Windows x64 VST2 real model + GUI open/close + DSP + bypass.\n");
    return 0;
}
#endif
