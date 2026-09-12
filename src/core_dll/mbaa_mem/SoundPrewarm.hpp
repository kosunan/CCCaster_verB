#pragma once
#include <windows.h>
#include <dsound.h>

namespace cccaster::game_interface::sound_prewarm {
enum class Result { Skipped, Prepared, FailedRestoration };
// 停止済みの効果音だけを無音で準備し、元の位置と音量へ戻す。
// ゲームの効果音フラグや再生履歴を経由しない。
template<class Buffer>
Result Prepare(Buffer &buffer) {
    DWORD status = 0, position = 0;
    LONG volume = 0;
    if (FAILED(buffer.GetStatus(&status)) || (status & (DSBSTATUS_PLAYING | DSBSTATUS_BUFFERLOST)) ||
        FAILED(buffer.GetVolume(&volume)) || FAILED(buffer.GetCurrentPosition(&position, nullptr)))
        return Result::Skipped;
    if (FAILED(buffer.SetVolume(DSBVOLUME_MIN))) {
        return SUCCEEDED(buffer.SetVolume(volume)) ? Result::Skipped : Result::FailedRestoration;
    }
    const auto played = buffer.Play(0, 0, 0);
    // 失敗時にも停止を試みる。停止できない状態で音量を戻さない。
    if (FAILED(buffer.Stop())) return Result::FailedRestoration;
    const auto restoredPosition = buffer.SetCurrentPosition(position);
    const auto restoredVolume = buffer.SetVolume(volume);
    DWORD actualPosition = 0, actualStatus = 0;
    LONG actualVolume = 0;
    if (FAILED(restoredPosition) || FAILED(restoredVolume) ||
        FAILED(buffer.GetStatus(&actualStatus)) || (actualStatus & DSBSTATUS_PLAYING) ||
        FAILED(buffer.GetVolume(&actualVolume)) || actualVolume != volume ||
        FAILED(buffer.GetCurrentPosition(&actualPosition, nullptr)) || actualPosition != position)
        return Result::FailedRestoration;
    return SUCCEEDED(played) ? Result::Prepared : Result::Skipped;
}
}
