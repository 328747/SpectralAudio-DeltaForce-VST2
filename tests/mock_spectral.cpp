#include "spectral_cpu.h"
#include <array>
#include <new>
#include <algorithm>
struct spectral_stream { std::array<float,1024> prev{}; float strength=1.0f; };
extern "C" int spectral_model_available(int p,int l){return p==2 && l==4;}
extern "C" spectral_stream* spectral_stream_create(int p,int l,int c,int u){return spectral_model_available(p,l)&&c==2&&!u?new(std::nothrow)spectral_stream():nullptr;}
extern "C" void spectral_stream_destroy(spectral_stream* s){delete s;}
extern "C" int spectral_stream_configure(spectral_stream* s,int p,int l){return s&&spectral_model_available(p,l)?0:-1;}
extern "C" int spectral_stream_set_strength(spectral_stream* s,float f){if(!s)return -1;s->strength=f;return 0;}
extern "C" int spectral_stream_reset(spectral_stream* s){if(!s)return -1;s->prev.fill(0);return 0;}
extern "C" int spectral_stream_process(spectral_stream* s,const float* in,float* out){
  if(!s||!in||!out)return -1;
  for(int i=0;i<1024;++i){out[i]=s->prev[i];s->prev[i]=in[i];}
  return 0;
}
