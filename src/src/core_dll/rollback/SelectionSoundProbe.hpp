#pragma once
#include <dsound.h>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

namespace cccaster::diagnostics::selection_sound {
// 明示した診断ディレクトリへゲームのBGMバッファだけを採取する。再生位置は変更しない。
inline void Sample() {
    static const char* directory=std::getenv("CCCASTER_SELECTION_SOUND_CAPTURE");
    if(!directory) return;
    static std::ofstream pcm;
    static uintptr_t previous=0;
    static DWORD position=0,lastTick=0;
    static unsigned segment=0;
    const DWORD tick=GetTickCount();
    if(tick-lastTick<50) return;
    lastTick=tick;
    const auto object=*reinterpret_cast<const uintptr_t*>(0x76DFF8);
    if(*reinterpret_cast<const uint32_t*>(0x54EEE8)!=20 || !object ||
       !*reinterpret_cast<const uint32_t*>(0x76E838)) {
        previous=0;
        if(pcm.is_open()) pcm.close();
        return;
    }
    auto list=*reinterpret_cast<IDirectSoundBuffer***>(object+4);
    if(!list || !list[0]) return;
    DWORD cursor=0,write=0;
    DSBCAPS caps{}; caps.dwSize=sizeof(caps);
    WAVEFORMATEX format{};
    if(FAILED(list[0]->GetCurrentPosition(&cursor,&write)) || FAILED(list[0]->GetCaps(&caps)) ||
       FAILED(list[0]->GetFormat(&format,sizeof(format),nullptr)) || !caps.dwBufferBytes) return;
    if(previous!=object) {
        if(pcm.is_open()) pcm.close();
        previous=object;position=cursor;
        const auto stem=std::string(directory)+"/bgm-"+std::to_string(++segment);
        pcm.open(stem+".pcm",std::ios::binary);
        std::ofstream meta(stem+".txt");
        meta<<format.wFormatTag<<' '<<format.nChannels<<' '<<format.nSamplesPerSec<<' '
            <<format.wBitsPerSample<<' '<<caps.dwBufferBytes<<' '<<cursor<<'\n';
        return;
    }
    const DWORD size=(cursor+caps.dwBufferBytes-position)%caps.dwBufferBytes;
    if(!size || !pcm.is_open()) return;
    void *first=nullptr,*second=nullptr;
    DWORD firstSize=0,secondSize=0;
    if(SUCCEEDED(list[0]->Lock(position,size,&first,&firstSize,&second,&secondSize,0))) {
        pcm.write(static_cast<const char*>(first),firstSize);
        if(secondSize) pcm.write(static_cast<const char*>(second),secondSize);
        list[0]->Unlock(first,firstSize,second,secondSize);
        pcm.flush();
    }
    position=cursor;
}
}
