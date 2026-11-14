#pragma once
#define Spectral_HOP 512
#ifdef __cplusplus
extern "C" {
#endif
struct spectral_stream;
int spectral_model_available(int,int);
spectral_stream* spectral_stream_create(int,int,int,int);
void spectral_stream_destroy(spectral_stream*);
int spectral_stream_configure(spectral_stream*,int,int);
int spectral_stream_set_strength(spectral_stream*,float);
int spectral_stream_reset(spectral_stream*);
int spectral_stream_process(spectral_stream*,const float*,float*);
#ifdef __cplusplus
}
#endif
