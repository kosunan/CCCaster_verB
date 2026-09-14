// ============================================================================
// LogSink.cpp — ログ行のバッファリングとバックグラウンド書き出し（実装）
//
// ロック順序は必ず fileMutex → queueMutex。逆順で取る経路を作らないこと。
//   - WriteLine()       : queueMutex のみ
//   - FlushOnce()       : fileMutex → queueMutex（取り出し後に queueMutex を解放）
//   - ワーカーの待機    : queueMutex のみ（条件変数）
// ============================================================================

#include "core_dll/common/LogSink.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cccaster::core::log {

namespace {

/// キューの上限。ワーカーが詰まっても青天井にメモリを食わないための保険。
/// 60Hz で 1 フレーム数行なら数分ぶんに相当し、通常は到達しない。
constexpr std::size_t kMaxPendingLines = 8192;

/// stdio 側のバッファ。1 バッチ内の細かい write をまとめるためのもので、
/// バッチ末尾で必ず fflush するので欠損窓を広げはしない。
constexpr std::size_t kFileBufferBytes = 64 * 1024;

struct Sink {
    // ── 行キュー ──
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::vector<std::string> pending;
    std::size_t droppedLines = 0;
    std::atomic<bool> writerRunning{false};
    std::atomic<bool> stopRequested{false};

    // ── 出力ファイル ──
    std::mutex fileMutex;
    std::FILE *fp = nullptr;
    bool fileClosed = false;
    std::string path;
    std::vector<char> fileBuffer;
};

/// 意図的にリークする。書き出しスレッドを detach しているため、
/// 静的デストラクタの順序に巻き込まれると解放済みオブジェクトに触りうる。
Sink &S() {
    static Sink *s = new Sink();
    return *s;
}

/// fileMutex を保持した状態で呼ぶこと。
void OpenFileLocked(Sink &s) {
    if (s.fp || s.fileClosed)
        return;

    const char *p = s.path.empty() ? "cccaster_hook_log.txt" : s.path.c_str();
    s.fp = std::fopen(p, "a");
    if (!s.fp)
        return;

    s.fileBuffer.resize(kFileBufferBytes);
    std::setvbuf(s.fp, s.fileBuffer.data(), _IOFBF, s.fileBuffer.size());
}

/// fileMutex を保持した状態で呼ぶこと。
void WriteLinesLocked(Sink &s, const std::vector<std::string> &lines, std::size_t dropped) {
    OpenFileLocked(s);
    if (!s.fp)
        return;

    if (dropped > 0) {
        // 欠損を黙って飲み込むと「出ていない＝起きていない」と誤読される。
        std::fprintf(s.fp, "[LogSink] dropped %lu lines\n", static_cast<unsigned long>(dropped));
    }
    for (const std::string &line : lines) {
        std::fwrite(line.data(), 1, line.size(), s.fp);
        std::fputc('\n', s.fp);
    }
    std::fflush(s.fp);
}

/// 溜まっている行を取り出す。queueMutex は取り出しの間だけ持つ。
/// blocking=false のときは queueMutex も try_lock にする。
/// プロセス強制終了（ExitProcess）経路では、他スレッドが queueMutex を握ったまま
/// 消えている可能性があり、ここで待つと DllMain 内＝ローダーロック保持中に
/// デッドロックしてプロセスが終わらなくなる。fileMutex だけ try_lock にしても
/// この関数で待ってしまっては意味がない。
bool TakePending(Sink &s, std::vector<std::string> &out, std::size_t &dropped, bool blocking = true) {
    std::unique_lock<std::mutex> lk(s.queueMutex, std::defer_lock);
    if (blocking) {
        lk.lock();
    } else if (!lk.try_lock()) {
        return false;
    }
    if (s.pending.empty() && s.droppedLines == 0)
        return false;

    out.swap(s.pending);
    s.pending.clear(); // 容量は保持したまま次のバッチに再利用する
    dropped = s.droppedLines;
    s.droppedLines = 0;
    return true;
}

/// 1 バッチぶん書き出す。
/// blocking=false のときは fileMutex を取れなければ何もせず戻る
/// （キューからは取り出さないので行は失われない）。
void FlushOnce(Sink &s, bool blocking) {
    std::unique_lock<std::mutex> lk(s.fileMutex, std::defer_lock);
    if (blocking) {
        lk.lock();
    } else if (!lk.try_lock()) {
        return;
    }

    std::vector<std::string> lines;
    std::size_t dropped = 0;
    if (!TakePending(s, lines, dropped, blocking))
        return;

    WriteLinesLocked(s, lines, dropped);
}

void WriterLoop() {
    Sink &s = S();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(s.queueMutex);
            s.queueCv.wait(lk, [&s] { return s.stopRequested || !s.pending.empty(); });
            if (s.stopRequested && s.pending.empty())
                return;
        }
        FlushOnce(s, true);
    }
}

/// この行を落とすと調査が不能になる、という種類の行か。
///
/// **新しく致命的なログを足すときは、必ずここに載る文字列を含めること。**
/// 該当しない行は非同期に流れるため、`TerminateProcess` を伴う経路
/// （`FrameControl::ExitGame`）ではワーカーごと消えて残らない。
///
/// `Disconnected` / `Aborted` を入れてあるのは、この2つが実際に
/// `SceneRunner::Step()` の (I)(H) で終了直前に出力される文字列で、
/// かつ**その直後にプロセスが即死する**ため。ここに載っていないと、
/// 「なぜ落ちたか」の最後の1行が毎回失われる。
bool IsUrgent(const char *msg) {
    return std::strstr(msg, "[FATAL]") != nullptr || std::strstr(msg, "[ASSERT]") != nullptr ||
           std::strstr(msg, "Disconnected") != nullptr || std::strstr(msg, "Aborted") != nullptr;
}

} // namespace

void SetLogPath(const std::string &path) {
    Sink &s = S();
    std::lock_guard<std::mutex> lk(s.fileMutex);
    if (s.fp || s.fileClosed)
        return; // 開いた後の差し替えは受け付けない
    s.path = path;
}

void StartWriter() {
    Sink &s = S();
    {
        std::lock_guard<std::mutex> lk(s.queueMutex);
        if (s.writerRunning || s.stopRequested)
            return;
        s.writerRunning = true;
    }

    try {
        std::thread(&WriterLoop).detach();
    } catch (...) {
        // 起動できなければ同期書き出しに戻す。ログのために落ちる方が損。
        std::lock_guard<std::mutex> lk(s.queueMutex);
        s.writerRunning = false;
    }
}

void WriteLine(const char *msg) {
    if (!msg)
        return;
    Sink &s = S();

    const bool urgent = IsUrgent(msg);
    bool writerRunning = false;

    {
        std::lock_guard<std::mutex> lk(s.queueMutex);
        writerRunning = s.writerRunning;
        if (s.pending.size() >= kMaxPendingLines) {
            ++s.droppedLines;
        } else {
            s.pending.emplace_back(msg);
        }
    }

    if (!writerRunning || urgent) {
        // ワーカー未起動（初期化フェーズ / 起動失敗 / Shutdown 後）と
        // 重要行だけは、この場で書き切る。
        FlushOnce(s, true);
        return;
    }

    s.queueCv.notify_one();
}

void FlushNow() {
    FlushOnce(S(), true);
}

void Shutdown(bool blocking) {
    Sink &s = S();
    {
        // ここも blocking=false では待たない（TakePending と同じ理由）。
        // 終了通知はatomic。ローダーロック下でqueueMutexを待たずに公開できる。
        std::unique_lock<std::mutex> lk(s.queueMutex, std::defer_lock);
        if (blocking)
            lk.lock();
        else
            (void)lk.try_lock();
        s.stopRequested = true;
        s.writerRunning = false; // 以降の WriteLine は同期経路に落ちる
    }
    s.queueCv.notify_all();

    // ワーカーは join しない。DllMain から join するとローダーロックで
    // デッドロックしうるし、プロセス終了時は既に止まっている。
    // 代わりにここで書き切り、fileMutex 越しに安全にファイルを閉じる。
    FlushOnce(s, blocking);

    std::unique_lock<std::mutex> lk(s.fileMutex, std::defer_lock);
    if (blocking) {
        lk.lock();
    } else if (!lk.try_lock()) {
        return; // 誰かが握ったまま消えている。諦める（プロセスは終わる）
    }
    if (s.fp) {
        std::fflush(s.fp);
        std::fclose(s.fp);
        s.fp = nullptr;
    }
    s.fileClosed = true;
}

} // namespace cccaster::core::log
