#include "../src/vst2_minimal.h"
#include <dlfcn.h>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
using MainProc = AEffect* (*)(HostCallback);
void must(bool v,const char* reason){if(!v){std::fprintf(stderr,"FAIL %s\n",reason);std::exit(2);}}
int main(int argc,char** argv){
 must(argc==2,"DLL/SO pathname");
 void* lib=dlopen(argv[1],RTLD_NOW);
 if(!lib){std::fprintf(stderr,"dlopen: %s\n",dlerror());return 3;}
 auto entry=reinterpret_cast<MainProc>(dlsym(lib,"VSTPluginMain"));
 must(entry!=nullptr,"VSTPluginMain export");
 AEffect* fx=entry(nullptr);
 must(fx!=nullptr,"created effect");
 must(fx->magic==kVstMagic,"VST magic");
 must(fx->numInputs==2 && fx->numOutputs==2,"stereo io");
 must(fx->numParams==5 && fx->initialDelay==1024,"parameters and latency");
 must(fx->processReplacing!=nullptr,"process callback");
 char name[128]{};
 must(fx->dispatcher(fx,effGetEffectName,0,0,name,0)==1,"effect name query");
 must(fx->dispatcher(fx,effGetVstVersion,0,0,nullptr,0)==2400,"VST version");
 fx->dispatcher(fx,effOpen,0,0,nullptr,0);
 fx->dispatcher(fx,effSetSampleRate,0,0,nullptr,48000.0f);
 fx->dispatcher(fx,effMainsChanged,0,1,nullptr,0);
 // Exact-bypass test over arbitrary host buffer sizes (not divisible by 512).
 fx->setParameter(fx,3,1.0f);
 constexpr int N=25000;
 std::vector<float> inL(N),inR(N),outL(N),outR(N);
 for(int i=0;i<N;++i){inL[i]=0.4f*std::sin(i*.013f);inR[i]=-0.35f*std::cos(i*.017f);}
 int offs=0;const int sizes[]={17,33,255,128,701,1,1024,13,2048};int p=0;
 while(offs<N){int step=std::min(sizes[p++%9],N-offs);float* in[]={inL.data()+offs,inR.data()+offs};float* out[]={outL.data()+offs,outR.data()+offs};fx->processReplacing(fx,in,out,step);offs+=step;}
 float err=0;
 for(int i=1500;i<N;++i){err=std::max(err,std::fabs(outL[i]-inL[i-1024]));err=std::max(err,std::fabs(outR[i]-inR[i-1024]));}
 must(err<1.e-5f,"bypass produces 1024-frame delayed dry signal");
 // With mock core (1-hop delay), the active processing chain should remain
 // aligned to the dry path after its fade-in, no matter the buffer boundaries.
 fx->dispatcher(fx,effMainsChanged,0,0,nullptr,0);
 fx->setParameter(fx,3,0.0f);
 fx->dispatcher(fx,effMainsChanged,0,1,nullptr,0);
 offs=0;p=0;std::fill(outL.begin(),outL.end(),0);std::fill(outR.begin(),outR.end(),0);
 while(offs<N){int step=std::min(sizes[p++%9],N-offs);float* in[]={inL.data()+offs,inR.data()+offs};float* out[]={outL.data()+offs,outR.data()+offs};fx->processReplacing(fx,in,out,step);offs+=step;}
 err=0;
 for(int i=5000;i<N;++i){err=std::max(err,std::fabs(outL[i]-inL[i-1024]));err=std::max(err,std::fabs(outR[i]-inR[i-1024]));}
 must(err<1.e-5f,"wet process remains aligned at 1024 frames");
 // Unsupported sample rate should fail safely into dry output.
 fx->dispatcher(fx,effSetSampleRate,0,0,nullptr,44100.0f);
 offs=0;p=0;
 while(offs<N){int step=std::min(sizes[p++%9],N-offs);float* in[]={inL.data()+offs,inR.data()+offs};float* out[]={outL.data()+offs,outR.data()+offs};fx->processReplacing(fx,in,out,step);offs+=step;}
 err=0;
 for(int i=5000;i<N;++i) err=std::max(err,std::fabs(outL[i]-inL[i-1024]));
 must(err<1.e-5f,"unsupported rate dry bypass");
 fx->dispatcher(fx,effClose,0,0,nullptr,0);
 dlclose(lib);
 std::printf("PASS: VST2 ABI, stereo, parameters, arbitrary buffers, 1024-frame latency, wet/dry alignment, sample-rate fallback.\n");
}
