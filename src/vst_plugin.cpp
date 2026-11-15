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
#include <cwchar>
#include <complex>
#include <limits>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif
#endif

namespace {
constexpr int kHop = Spectral_HOP;      // 512, exact source DSP contract
constexpr int kDelayFrames = 2 * kHop;  // 1024 = staging + source DSP latency
constexpr int kChannels = 2;
constexpr int kParams = 5;  // original 4 controls + independent mute
constexpr int kVisualFFT = 1024;
constexpr int kVisualBands = 28;
constexpr int kWaterfallCols = 64;
#ifdef _WIN32
// VST2 effect editor opcodes, matching VST 2.4 ABI.
constexpr VstInt32 kFlagHasEditor = 1 << 0;
constexpr VstInt32 kEffEditGetRect = 13;
constexpr VstInt32 kEffEditOpen = 14;
constexpr VstInt32 kEffEditClose = 15;
constexpr VstInt32 kEffEditIdle = 19;
struct EditorRect {
    std::int16_t top, left, bottom, right;
};
static_assert(sizeof(EditorRect) == 8, "Invalid VST2 EditorRect ABI");
constexpr int kUiWidth = 900;
constexpr int kUiHeight = 712;
#endif

// Host controls have normalized [0,1] ranges, like all VST2 parameters.
// 0: Strength [0..200%], 1: Wet [0..100%], 2: Trim [-12..+6 dB], 3: Bypass, 4: Mute.
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
        parameters_[4].store(0.0f); // not muted
        effect.magic = kVstMagic;
        effect.dispatcher = dispatch;
        effect.process = process;
        effect.setParameter = set_parameter;
        effect.getParameter = get_parameter;
        effect.numPrograms = 1;
        effect.numParams = kParams;
        effect.numInputs = kChannels;
        effect.numOutputs = kChannels;
        effect.flags = kFlagCanReplacing
#ifdef _WIN32
            | kFlagHasEditor
#endif
            ;
        effect.initialDelay = kDelayFrames;
        effect.object = this;
        effect.uniqueID = kUniqueID;
        effect.version = 200;
        effect.processReplacing = process;
        if (spectral_model_available(2, 4)) stream_ = spectral_stream_create(2, 4, kChannels, 0);
        reset();
    }
    ~Plugin() {
#ifdef _WIN32
        closeEditor();
#endif
        if (stream_) spectral_stream_destroy(stream_);
    }
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
    // Real-time tap: aligned dry samples vs the actual VST output.
    // Atomic sample slots avoid races; no allocations, FFTs or GDI on the audio thread.
    std::array<std::atomic<float>, kVisualFFT> visualDry_{};
    std::array<std::atomic<float>, kVisualFFT> visualOut_{};
    std::atomic<std::uint64_t> visualPublished_{0};
    std::atomic<std::uint64_t> visualClips_{0};
    std::atomic<bool> monitorEnabled_{false};
    std::uint64_t visualWritePos_ = 0;
#ifdef _WIN32
    EditorRect editorRect_{0, 0, static_cast<std::int16_t>(kUiHeight), static_cast<std::int16_t>(kUiWidth)};
    HWND editorWindow_ = nullptr;
    std::array<HWND, 3> edits_{};
    HWND bypassCheck_ = nullptr;
    HWND muteCheck_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT titleFont_ = nullptr;
    HBRUSH backBrush_ = nullptr;
    HBRUSH inputBrush_ = nullptr;
    std::array<HBRUSH, 11> heatBrush_{};
    std::array<float, kVisualBands> uiDryDb_{};
    std::array<float, kVisualBands> uiOutDb_{};
    std::array<float, kVisualBands> uiGainDb_{};
    std::array<std::array<float,kVisualBands>, kWaterfallCols> heatHistory_{};
    int heatWrite_ = 0;
    std::uint64_t lastVisualFrame_ = 0;
    int idleTicks_ = 0;
    float inputRmsDb_ = -100.0f;
    float outputRmsDb_ = -100.0f;
    float inputPeakDb_ = -100.0f;
    float outputPeakDb_ = -100.0f;
    float overallGainDb_ = 0.0f;
    bool reliableGain_ = false;
    bool graphValid_ = false;
    static constexpr UINT_PTR kMonitorTimer = 27;
    static constexpr COLORREF kBackground = RGB(15,20,26);
    static constexpr COLORREF kPanel = RGB(24,32,40);
    static constexpr COLORREF kForeground = RGB(232,239,241);
    static constexpr COLORREF kMuted = RGB(155,171,183);
    static constexpr COLORREF kOriginal = RGB(106,173,255);
    static constexpr COLORREF kProcessed = RGB(93,224,180);

    static LRESULT CALLBACK editorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
        Plugin* self = reinterpret_cast<Plugin*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<Plugin*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }
        if (msg == WM_ERASEBKGND) return 1;
        if ((msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN) && self) {
            auto dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, kForeground);
            SetBkColor(dc, msg == WM_CTLCOLOREDIT ? RGB(32,45,55) : kBackground);
            return reinterpret_cast<LRESULT>(msg == WM_CTLCOLOREDIT ? self->inputBrush_ : self->backBrush_);
        }
        if (msg == WM_COMMAND && self && LOWORD(wParam) == 110 && HIWORD(wParam) == BN_CLICKED) {
            self->applyEditor(); return 0;
        }
        if (msg == WM_TIMER && self && wParam == kMonitorTimer) {
            self->updateTelemetry();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (msg == WM_PAINT && self) {
            PAINTSTRUCT ps{}; HDC dc=BeginPaint(hwnd,&ps);
            self->paintEditor(dc);
            EndPaint(hwnd,&ps); return 0;
        }
        if (msg == WM_NCDESTROY && self) {
            self->monitorEnabled_.store(false,std::memory_order_release);
            KillTimer(hwnd,kMonitorTimer);
            self->editorWindow_=nullptr;
            self->edits_.fill(nullptr);
            self->bypassCheck_=nullptr;
            self->muteCheck_=nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    static HINSTANCE pluginModule() noexcept {
        static int addressInDll=0; HMODULE module=nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&addressInDll),&module);
        return reinterpret_cast<HINSTANCE>(module);
    }
    bool registerEditorClass() noexcept {
        WNDCLASSW wc{};
        wc.lpfnWndProc=editorWndProc;
        wc.hInstance=pluginModule();
        wc.lpszClassName=L"SpectralDeltaForceVST2MonitorV2";
        wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
        wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        return RegisterClassW(&wc)!=0 || GetLastError()==ERROR_CLASS_ALREADY_EXISTS;
    }
    HWND makeControl(HWND parent,const wchar_t* type,const wchar_t* text,
                     DWORD style,int x,int y,int w,int h,int id=0) noexcept {
        HWND hwnd=CreateWindowExW(type[0]==L'E' ? WS_EX_CLIENTEDGE : 0,
             type,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,parent,
             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),pluginModule(),nullptr);
        if(hwnd) SendMessageW(hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(uiFont_),TRUE);
        return hwnd;
    }
    static void setNumber(HWND edit,float value) noexcept {
        if(!edit)return;
        wchar_t tmp[32]{};
        std::swprintf(tmp,32,L"%.1f",static_cast<double>(value));
        SetWindowTextW(edit,tmp);
    }
    static float readNumber(HWND edit,float fallback,float lo,float hi) noexcept {
        if(!edit)return fallback;
        wchar_t tmp[64]{}; GetWindowTextW(edit,tmp,64);
        wchar_t* end=nullptr;
        const float v=std::wcstof(tmp,&end);
        if(end==tmp||!std::isfinite(v))return fallback;
        return std::clamp(v,lo,hi);
    }
    void updateEditor() noexcept {
        if(!editorWindow_)return;
        setNumber(edits_[0],parameters_[0].load()*200.0f);
        setNumber(edits_[1],-12.0f+18.0f*parameters_[2].load());
        setNumber(edits_[2],parameters_[1].load()*100.0f);
        if(bypassCheck_)SendMessageW(bypassCheck_,BM_SETCHECK,
            parameters_[3].load()>=.5f?BST_CHECKED:BST_UNCHECKED,0);
        if(muteCheck_)SendMessageW(muteCheck_,BM_SETCHECK,
            parameters_[4].load()>=.5f?BST_CHECKED:BST_UNCHECKED,0);
    }
    void applyEditor() noexcept {
        const float str=readNumber(edits_[0],parameters_[0].load()*200.0f,0.0f,200.0f);
        const float trim=readNumber(edits_[1],-12.0f+18.0f*parameters_[2].load(),-12.0f,6.0f);
        const float wet=readNumber(edits_[2],parameters_[1].load()*100.0f,0.0f,100.0f);
        parameters_[0].store(str/200.0f);
        parameters_[2].store((trim+12.0f)/18.0f);
        parameters_[1].store(wet/100.0f);
        parameters_[3].store(bypassCheck_ && SendMessageW(bypassCheck_,BM_GETCHECK,0,0)==BST_CHECKED?1.0f:0.0f);
        parameters_[4].store(muteCheck_ && SendMessageW(muteCheck_,BM_GETCHECK,0,0)==BST_CHECKED?1.0f:0.0f);
        updateEditor();
    }
    static void fftToBands(const std::array<float,kVisualFFT>& samples,
                           std::array<float,kVisualBands>& out) noexcept {
        std::array<std::complex<float>,kVisualFFT> a{};
        constexpr float pi=3.14159265358979323846f;
        for(int i=0;i<kVisualFFT;++i) {
            const float w=.5f-.5f*std::cos(2.0f*pi*i/(kVisualFFT-1));
            a[i]={samples[i]*w,0.0f};
        }
        for(int i=1,j=0;i<kVisualFFT;++i) {
            int bit=kVisualFFT>>1;
            for(;j&bit;bit>>=1) j^=bit;
            j^=bit;
            if(i<j) std::swap(a[i],a[j]);
        }
        for(int len=2;len<=kVisualFFT;len<<=1) {
            const float angle=-2.0f*pi/len;
            const std::complex<float> root(std::cos(angle),std::sin(angle));
            for(int i=0;i<kVisualFFT;i+=len) {
                std::complex<float> w(1.0f,0.0f);
                for(int j=0;j<len/2;++j) {
                    const auto u=a[i+j],v=a[i+j+len/2]*w;
                    a[i+j]=u+v; a[i+j+len/2]=u-v; w*=root;
                }
            }
        }
        std::array<double,kVisualBands> powSum{};
        std::array<int,kVisualBands> count{};
        const double span=std::log(16000.0/80.0);
        for(int k=1;k<kVisualFFT/2;++k) {
            const double f=48000.0*k/kVisualFFT;
            if(f<80.0 || f>16000.0)continue;
            const int band=std::clamp(static_cast<int>(std::log(f/80.0)/span*kVisualBands),0,kVisualBands-1);
            powSum[band]+=static_cast<double>(std::norm(a[k]));
            count[band]++;
        }
        for(int b=0;b<kVisualBands;++b) {
            // Approximate dBFS spectral amplitude; 1024-point Hann peak ~256.
            const double p=count[b]?powSum[b]/count[b]:0.0;
            out[b]=static_cast<float>(std::clamp(10.0*std::log10(p/(256.0*256.0)+1.e-12),-100.0,20.0));
        }
    }
    static float dB(float v) noexcept { return 20.0f*std::log10(std::max(v,1.e-7f)); }
    void updateTelemetry() noexcept {
        if(!editorWindow_)return;
        const std::uint64_t published=visualPublished_.load(std::memory_order_acquire);
        if(published<kVisualFFT || published==lastVisualFrame_) {
            if(idleTicks_<1000) ++idleTicks_;
            return;
        }
        idleTicks_=0;
        lastVisualFrame_=published;
        std::array<float,kVisualFFT> dry{},out{};
        double drySum=0,outSum=0;
        float dryPeak=0,outPeak=0;
        for(int i=0;i<kVisualFFT;++i) {
            const auto index=static_cast<std::size_t>((published-kVisualFFT+i)&(kVisualFFT-1));
            dry[i]=visualDry_[index].load(std::memory_order_relaxed);
            out[i]=visualOut_[index].load(std::memory_order_relaxed);
            drySum+=static_cast<double>(dry[i])*dry[i];
            outSum+=static_cast<double>(out[i])*out[i];
            dryPeak=std::max(dryPeak,std::abs(dry[i]));
            outPeak=std::max(outPeak,std::abs(out[i]));
        }
        inputRmsDb_=dB(static_cast<float>(std::sqrt(drySum/kVisualFFT)));
        outputRmsDb_=dB(static_cast<float>(std::sqrt(outSum/kVisualFFT)));
        inputPeakDb_=dB(dryPeak);
        outputPeakDb_=dB(outPeak);
        reliableGain_=inputRmsDb_>-60.0f;
        overallGainDb_=reliableGain_?std::clamp(outputRmsDb_-inputRmsDb_,-60.0f,60.0f):0.0f;
        fftToBands(dry,uiDryDb_);
        fftToBands(out,uiOutDb_);
        for(int b=0;b<kVisualBands;++b) {
            // Exclude near-silent input bands from the gain visualization.
            uiGainDb_[b]=(reliableGain_ && uiDryDb_[b]>-65.0f)
                  ?std::clamp(uiOutDb_[b]-uiDryDb_[b],-24.0f,24.0f):0.0f;
        }
        heatHistory_[heatWrite_]=uiGainDb_;
        heatWrite_=(heatWrite_+1)%kWaterfallCols;
        graphValid_=true;
    }
    static void label(HDC dc,const wchar_t* s,int x,int y,int w,int h,COLORREF color,HFONT font=nullptr) noexcept {
        RECT r{x,y,x+w,y+h};
        SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);
        if(font)SelectObject(dc,font);
        DrawTextW(dc,s,-1,&r,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
    }
    static void line(HDC dc,int x0,int y0,int x1,int y1,COLORREF color,int width=1) noexcept {
        HPEN pen=CreatePen(PS_SOLID,width,color);
        auto old=SelectObject(dc,pen);
        MoveToEx(dc,x0,y0,nullptr);LineTo(dc,x1,y1);
        SelectObject(dc,old);DeleteObject(pen);
    }
    void paintEditor(HDC target) noexcept {
        RECT client{};GetClientRect(editorWindow_,&client);
        const int width=client.right,height=client.bottom;
        HDC mem=CreateCompatibleDC(target);
        HBITMAP bmp=CreateCompatibleBitmap(target,std::max(width,1),std::max(height,1));
        auto oldBmp=SelectObject(mem,bmp);
        FillRect(mem,&client,backBrush_);
        label(mem,L"Spectral AUDIO   |   DELTA FORCE",25,14,690,38,kProcessed,titleFont_);
        label(mem,L"GRU128  本地实时推理 · VST2 / Equalizer APO",28,52,750,20,kMuted,uiFont_);
        RECT controls{21,86,879,228};
        HBRUSH panelBrush=CreateSolidBrush(kPanel);
        FillRect(mem,&controls,panelBrush);
        DeleteObject(panelBrush);
        label(mem,L"增强强度 (%)",38,94,180,20,kForeground,uiFont_);
        label(mem,L"输出增益 (dB)",322,94,180,20,kForeground,uiFont_);
        label(mem,L"干湿混合 (%)",605,94,180,20,kForeground,uiFont_);
        label(mem,L"恢复原音会旁路 AI；静音会使输出为 0。音频设备由 Equalizer APO 决定。",38,197,820,22,kMuted,uiFont_);
        label(mem,L"实时频谱   (蓝：原音   绿：实际输出；均取左声道)",25,237,850,29,kForeground,uiFont_);
        RECT plot{42,276,856,451};
        HBRUSH plotBg=CreateSolidBrush(RGB(19,26,34));
        FillRect(mem,&plot,plotBg);DeleteObject(plotBg);
        for(int i=0;i<=5;++i) {
            const int y=276+i*35;
            line(mem,42,y,856,y,RGB(47,58,67));
            wchar_t db[20];std::swprintf(db,20,L"%d",0-i*20);
            label(mem,db,5,y-9,36,18,kMuted,uiFont_);
        }
        static const wchar_t* freqs[]={L"80",L"250",L"1k",L"4k",L"16k Hz"};
        for(int i=0;i<5;++i)label(mem,freqs[i],38+i*202,452,82,18,kMuted,uiFont_);
        if(graphValid_) {
            for(int b=1;b<kVisualBands;++b) {
                const int x0=42+(b-1)*814/(kVisualBands-1);
                const int x1=42+b*814/(kVisualBands-1);
                auto yOf=[](float db) {return 276+static_cast<int>(std::clamp(-db,0.0f,100.0f)*175.0f/100.0f);};
                line(mem,x0,yOf(uiDryDb_[b-1]),x1,yOf(uiDryDb_[b]),kOriginal,2);
                line(mem,x0,yOf(uiOutDb_[b-1]),x1,yOf(uiOutDb_[b]),kProcessed,2);
            }
        } else label(mem,L"等待有效音频输入…",290,340,450,32,kMuted,uiFont_);
        label(mem,L"VST 实际处理差异热力图  (含 AI / Wet Mix / Trim；绿增强、蓝衰减)",25,480,850,28,kForeground,uiFont_);
        RECT heat{42,514,856,629};
        HBRUSH heatBg=CreateSolidBrush(RGB(18,25,32));
        FillRect(mem,&heat,heatBg);DeleteObject(heatBg);
        for(int col=0;col<kWaterfallCols;++col) {
            const int hist=(heatWrite_+col)%kWaterfallCols;
            for(int band=0;band<kVisualBands;++band) {
                const float gain=heatHistory_[hist][band];
                const int level=std::clamp(static_cast<int>(std::round(gain/5.0f)), -5,5);
                RECT cell{42+col*814/kWaterfallCols, 514+(kVisualBands-1-band)*115/kVisualBands,
                      42+(col+1)*814/kWaterfallCols+1,514+(kVisualBands-band)*115/kVisualBands+1};
                FillRect(mem,&cell,heatBrush_[level+5]);
            }
        }
        label(mem,L"过去约 3 秒                                   最新 →",42,629,805,17,kMuted,uiFont_);
        wchar_t status[420]{};
        if(graphValid_) {
            if(reliableGain_)
                std::swprintf(status,420,L"输入 RMS %.1f dBFS    输出 RMS %.1f dBFS    净变化 %+.1f dB    输出 Peak %.1f dBFS",
                      inputRmsDb_,outputRmsDb_,overallGainDb_,outputPeakDb_);
            else std::swprintf(status,420,L"输入信号太弱，暂停增益判断    输入 RMS %.1f dBFS",inputRmsDb_);
        } else std::swprintf(status,420,L"未检测到连续音频输入");
        label(mem,status,25,648,845,22,kForeground,uiFont_);
        wchar_t second[260]{};
        std::swprintf(second,260,L"运行帧: %llu    超出 ±1 的内部输出样本: %llu    状态: %ls",
            static_cast<unsigned long long>(visualPublished_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(visualClips_.load(std::memory_order_relaxed)),
            idleTicks_>20?L"无新音频":L"正在接收音频");
        label(mem,second,25,679,845,20,kMuted,uiFont_);
        BitBlt(target,0,0,width,height,mem,0,0,SRCCOPY);
        SelectObject(mem,oldBmp);DeleteObject(bmp);DeleteDC(mem);
    }
    bool openEditor(void* parentPtr) noexcept {
        if(editorWindow_)return true;
        if(!parentPtr || !registerEditorClass())return false;
        backBrush_=CreateSolidBrush(kBackground);
        inputBrush_=CreateSolidBrush(RGB(32,45,55));
        uiFont_=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                           DEFAULT_PITCH|FF_DONTCARE,L"Microsoft YaHei UI");
        titleFont_=CreateFontW(-25,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                           DEFAULT_PITCH|FF_DONTCARE,L"Microsoft YaHei UI");
        for(int i=0;i<11;++i) {
            const int intensity=std::abs(i-5);
            COLORREF color=RGB(30,41,51);
            if(i>5)color=RGB(25,60+intensity*27,58+intensity*12);
            if(i<5)color=RGB(25+intensity*5,52+intensity*12,65+intensity*29);
            heatBrush_[i]=CreateSolidBrush(color);
        }
        editorWindow_=CreateWindowExW(0,L"SpectralDeltaForceVST2MonitorV2",
            L"Spectral AUDIO",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,
            0,0,kUiWidth,kUiHeight,reinterpret_cast<HWND>(parentPtr),nullptr,pluginModule(),this);
        if(!editorWindow_){releaseEditorResources();return false;}
        edits_[0]=makeControl(editorWindow_,L"EDIT",L"",ES_LEFT|ES_AUTOHSCROLL|WS_TABSTOP,38,125,180,29,100);
        edits_[1]=makeControl(editorWindow_,L"EDIT",L"",ES_LEFT|ES_AUTOHSCROLL|WS_TABSTOP,322,125,180,29,101);
        edits_[2]=makeControl(editorWindow_,L"EDIT",L"",ES_LEFT|ES_AUTOHSCROLL|WS_TABSTOP,605,125,180,29,102);
        bypassCheck_=makeControl(editorWindow_,L"BUTTON",L"恢复原音 / Bypass",BS_AUTOCHECKBOX|WS_TABSTOP,38,162,215,29,105);
        muteCheck_=makeControl(editorWindow_,L"BUTTON",L"静音 / Mute",BS_AUTOCHECKBOX|WS_TABSTOP,300,162,180,29,106);
        makeControl(editorWindow_,L"BUTTON",L"应用设置",BS_PUSHBUTTON|WS_TABSTOP,720,161,127,32,110);
        monitorEnabled_.store(true,std::memory_order_release);
        SetTimer(editorWindow_,kMonitorTimer,50,nullptr);
        updateEditor();
        InvalidateRect(editorWindow_,nullptr,FALSE);
        return true;
    }
    void releaseEditorResources() noexcept {
        if(uiFont_) {DeleteObject(uiFont_);uiFont_=nullptr;}
        if(titleFont_) {DeleteObject(titleFont_);titleFont_=nullptr;}
        if(backBrush_) {DeleteObject(backBrush_);backBrush_=nullptr;}
        if(inputBrush_) {DeleteObject(inputBrush_);inputBrush_=nullptr;}
        for(auto& b:heatBrush_) {if(b)DeleteObject(b);b=nullptr;}
    }
    void closeEditor() noexcept {
        monitorEnabled_.store(false,std::memory_order_release);
        if(editorWindow_)DestroyWindow(editorWindow_);
        releaseEditorResources();
    }
#endif

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
        const bool muted = parameters_[4].load(std::memory_order_relaxed) >= 0.5f;
        const float outputGain = muted ? 0.0f : (bypass ? 1.0f : std::pow(10.0f, gainDb / 20.0f));
        const bool monitoring = monitorEnabled_.load(std::memory_order_relaxed);
        std::uint64_t localClips=0;
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
            const float rawLeft=(dryLeft+(wetLeft-dryLeft)*blend)*outputGain;
            const float rawRight=(dryRight+(wetRight-dryRight)*blend)*outputGain;
            outputs[0][i]=clean(rawLeft);
            outputs[1][i]=clean(rawRight);
            if(monitoring) {
                localClips+=(std::abs(rawLeft)>1.0f || std::abs(rawRight)>1.0f) ? 1 : 0;
                const std::size_t ix=static_cast<std::size_t>(visualWritePos_++&(kVisualFFT-1));
                visualDry_[ix].store(dryLeft,std::memory_order_relaxed);
                visualOut_[ix].store(outputs[0][i],std::memory_order_relaxed);
            }

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
        if(monitoring) {
            visualClips_.fetch_add(localClips,std::memory_order_relaxed);
            visualPublished_.store(visualWritePos_,std::memory_order_release);
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
#ifdef _WIN32
            case kEffEditGetRect:
                if (!ptr) return 0;
                *static_cast<EditorRect**>(ptr) = &self->editorRect_;
                return 1;
            case kEffEditOpen: return self->openEditor(ptr) ? 1 : 0;
            case kEffEditClose: self->closeEditor(); return 1;
            case kEffEditIdle: return 1;
#endif
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
                copy_text(ptr,text,9); return 1;
            }
            case effGetParamName: {
                constexpr const char* names[kParams]={"Strength","WetMix","Trim","Bypass","Mute"};
                copy_text(ptr,(index>=0 && index<kParams)?names[index]:"",9); return 1;
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
            case effGetEffectName: copy_text(ptr,"SpectralAudio DeltaForce V2",32); return 1;
            case effGetVendorString: copy_text(ptr,"Community VST Wrapper",64); return 1;
            case effGetProductString: copy_text(ptr,"SA DeltaForce VST2",64); return 1;
            case effGetVendorVersion: return 200;
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
