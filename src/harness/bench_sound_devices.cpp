// 他の出力先を変更せず、明示したDirectSoundデバイスで処理時間だけを比較。
#include <windows.h>
#include <dsound.h>
#include <objbase.h>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <cstring>
#include "core_dll/mbaa_mem/SoundPrewarm.hpp"
struct Device { GUID guid; std::wstring name; };
static BOOL CALLBACK Enumerate(LPGUID guid,LPCWSTR name,LPCWSTR,LPVOID context) {
    if(guid) static_cast<std::vector<Device> *>(context)->push_back({*guid,name});
    return TRUE;
}
static std::string Utf8(const std::wstring &s) {
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);
    std::string result(n,' ');
    WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),result.data(),n,nullptr,nullptr);
    return result;
}
static int64_t Now() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
struct Row { unsigned trial,rate,channels,milliseconds,prepared; int64_t prepareTicks; unsigned prepareResult;
    int64_t ticks[6]; HRESULT results[6]; };
static HRESULT Create(IDirectSound8 *device, unsigned rate,unsigned channels,unsigned milliseconds, IDirectSoundBuffer **buffer) {
    WAVEFORMATEX format{}; format.wFormatTag=WAVE_FORMAT_PCM; format.nChannels=WORD(channels);
    format.nSamplesPerSec=rate; format.wBitsPerSample=16; format.nBlockAlign=WORD(channels*2);
    format.nAvgBytesPerSec=rate*format.nBlockAlign;
    DSBUFFERDESC desc{}; desc.dwSize=sizeof(desc);
    desc.dwFlags=DSBCAPS_CTRLVOLUME|DSBCAPS_CTRLPAN|DSBCAPS_CTRLFREQUENCY|DSBCAPS_GLOBALFOCUS|DSBCAPS_GETCURRENTPOSITION2;
    desc.dwBufferBytes=rate*milliseconds/1000*format.nBlockAlign; desc.lpwfxFormat=&format;
    auto hr=device->CreateSoundBuffer(&desc,buffer,nullptr); if(FAILED(hr))return hr;
    void *a=nullptr,*b=nullptr; DWORD na=0,nb=0;
    hr=(*buffer)->Lock(0,desc.dwBufferBytes,&a,&na,&b,&nb,0);
    if(FAILED(hr))return hr;
    std::memset(a,0,na); if(b)std::memset(b,0,nb); // 無音データ。波形や音質の試験ではない。
    hr=(*buffer)->Unlock(a,na,b,nb); if(FAILED(hr))return hr;
    return (*buffer)->SetVolume(-1666);
}
template<class F> static void Timed(Row &r,unsigned index,F f) {
    auto start=Now();r.results[index]=f();r.ticks[index]=Now()-start;
}
int main(int argc,char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<Device> devices;
    if(FAILED(DirectSoundEnumerateW(Enumerate,&devices)))return 2;
    for(unsigned i=0;i<devices.size();++i) {
        wchar_t guid[40]{}; StringFromGUID2(devices[i].guid,guid,40);
        std::cout<<i<<"\t"<<Utf8(devices[i].name)<<"\t"<<Utf8(guid)<<"\n";
    }
    if(argc==1)return 0;
    if(argc!=3 && argc!=4)return 2;
    const unsigned firstPrepared=argc==4 ? unsigned(std::stoul(argv[3]))%2 : 0;
    unsigned selected=std::stoul(argv[1]); if(selected>=devices.size())return 2;
    if(std::filesystem::exists(argv[2])) { std::cerr<<"Output already exists\n";return 2; }
    IDirectSound8 *device=nullptr;
    auto hr=DirectSoundCreate8(&devices[selected].guid,&device,nullptr);
    if(FAILED(hr)){std::cerr<<"Create device HRESULT="<<std::hex<<uint32_t(hr)<<"\n";return 3;}
    hr=device->SetCooperativeLevel(GetDesktopWindow(),DSSCL_NORMAL);
    if(FAILED(hr)){device->Release();return 3;}
    LARGE_INTEGER freq;QueryPerformanceFrequency(&freq);
    std::vector<Row> rows;rows.reserve(384);
    struct Format {unsigned rate,channels,ms;};
    const Format formats[]={{22050,1,250},{44100,1,1000},{48000,2,250}};
    for(auto f:formats)for(unsigned trial=0;trial<64;++trial) {
        for(unsigned order=0;order<2;++order) {
            Row r{};r.trial=trial;r.rate=f.rate;r.channels=f.channels;r.milliseconds=f.ms;r.prepared=(trial+order+firstPrepared)%2;
            IDirectSoundBuffer *buffer=nullptr;
            hr=Create(device,f.rate,f.channels,f.ms,&buffer);
            if(FAILED(hr)){if(buffer)buffer->Release();device->Release();std::cerr<<"Create buffer HRESULT="<<std::hex<<uint32_t(hr)<<"\n";return 4;}
            if(r.prepared) {
                auto start=Now();
                r.prepareResult=unsigned(cccaster::game_interface::sound_prewarm::Prepare(*buffer));
                r.prepareTicks=Now()-start;
            }
            DWORD status=0;
            Timed(r,0,[&]{return buffer->GetStatus(&status);});
            Timed(r,1,[&]{return buffer->Stop();});
            Timed(r,2,[&]{return buffer->SetCurrentPosition(0);});
            Timed(r,3,[&]{return buffer->SetVolume(-1666);});
            Timed(r,4,[&]{return buffer->SetPan(0);});
            Timed(r,5,[&]{return buffer->Play(0,0,0);});
            buffer->Stop(); buffer->Release(); rows.push_back(r);
        }
        Sleep(2);
    }
    device->Release();
    std::ofstream out(argv[2]);
    out<<"device,trial,rate,channels,ms,prepared,prepare_us,prepare_result,GetStatus_us,Stop_us,SetCurrentPosition_us,SetVolume_us,SetPan_us,Play_us,failures\n";
    for(auto &r:rows) {
        out<<selected<<','<<r.trial<<','<<r.rate<<','<<r.channels<<','<<r.milliseconds<<','<<r.prepared<<','
            <<double(r.prepareTicks)*1e6/freq.QuadPart<<','<<r.prepareResult;
        unsigned failures=0;
        for(unsigned i=0;i<6;++i){out<<','<<double(r.ticks[i])*1e6/freq.QuadPart;failures+=FAILED(r.results[i]);}
        out<<','<<failures<<'\n';
    }
    return out.good()?0:5;
}
