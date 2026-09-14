#include "core_dll/mbaa_mem/SoundPrewarm.hpp"
#include "test_support.hpp"
struct Buffer {
    DWORD status=0, position=64;
    LONG volume=-1666;
    bool playFail=false, stopFail=false, audible=false;
    unsigned plays=0, sets=0;
    HRESULT GetStatus(DWORD *p) { *p=status; return S_OK; }
    HRESULT GetVolume(LONG *p) { *p=volume; return S_OK; }
    HRESULT GetCurrentPosition(DWORD *p,DWORD *) { *p=position; return S_OK; }
    HRESULT SetVolume(LONG v) { volume=v; ++sets; return S_OK; }
    HRESULT SetCurrentPosition(DWORD p) { position=p; return S_OK; }
    HRESULT Play(DWORD,DWORD,DWORD) { ++plays; audible=volume!=DSBVOLUME_MIN; status=DSBSTATUS_PLAYING; position=128; return playFail?E_FAIL:S_OK; }
    HRESULT Stop() { if(stopFail)return E_FAIL; status=0; return S_OK; }
};
int main() {
    using namespace cccaster::game_interface::sound_prewarm;
    CC_CASE("最小音量で準備し停止位置音量を復元する");
    Buffer b;
    CC_CHECK(Prepare(b)==Result::Prepared);
    CC_CHECK(b.plays==1 && !b.audible && b.status==0 && b.position==64 && b.volume==-1666);
    CC_CASE("再生中と消失バッファは変更しない");
    Buffer playing; playing.status=DSBSTATUS_PLAYING;
    CC_CHECK(Prepare(playing)==Result::Skipped && playing.plays==0 && playing.sets==0);
    Buffer lost; lost.status=DSBSTATUS_BUFFERLOST;
    CC_CHECK(Prepare(lost)==Result::Skipped && lost.sets==0);
    CC_CASE("Play失敗でも状態を復元する");
    Buffer fail; fail.playFail=true;
    CC_CHECK(Prepare(fail)==Result::Skipped && fail.status==0 && fail.position==64 && fail.volume==-1666);
    CC_CASE("停止失敗を隠さず音量を戻して再生させない");
    Buffer stop; stop.stopFail=true;
    CC_CHECK(Prepare(stop)==Result::FailedRestoration && stop.volume==DSBVOLUME_MIN);
    return cccaster::test::Summarize("sound_prewarm");
}
