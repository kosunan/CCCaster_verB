#pragma once
#include "core_dll/spectator/Stream.hpp"
#include <thread>
#include <string>

namespace cccaster::spectator {
enum class Status : uint32_t { Off, Connecting, Waiting, Receiving, Disconnected, Overflow, Failed };
class Transport {
    Queue<2048> queue_;
    std::atomic<bool> running_{false};
    std::atomic<Status> status_{Status::Off};
    std::atomic<uint32_t> viewers_{0}, latest_{0};
    std::atomic<uint16_t> port_{0};
    std::thread worker_;
    void Serve(uint16_t port);
    void Receive(std::string ip, uint16_t port);
public:
    // DLLのプロセス終了ではloader lock下にjoinしない。通常Stopはharnessで使用。
    static Transport &Get() { static auto *instance = new Transport; return *instance; }
    ~Transport() { Stop(); }
    void StartHost(uint16_t port);
    void StartViewer(const std::string &ip, uint16_t port);
    void Stop();
    bool Publish(const Record &r) {
        if (!running_.load(std::memory_order_relaxed)) return false;
        if (queue_.Push(r)) return true;
        status_.store(Status::Overflow, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        return false; // 対戦は継続、欠落した観戦ストリームだけを終了。
    }
    bool PublishInput(uint32_t frame, uint32_t p1, uint32_t p2) {
        if (!running_.load(std::memory_order_relaxed)) return false;
        if (queue_.PushInput(frame, p1, p2)) return true;
        status_.store(Status::Overflow, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        return false;
    }
    bool Take(Record &r) { return queue_.Pop(r); }
    uint32_t Buffered() const { return queue_.Count(); }
    uint32_t Latest() const { return latest_.load(std::memory_order_relaxed); }
    uint32_t Viewers() const { return viewers_.load(std::memory_order_relaxed); }
    uint16_t Port() const { return port_.load(std::memory_order_acquire); }
    Status State() const { return status_.load(std::memory_order_acquire); }
    bool Active() const { return running_.load(std::memory_order_relaxed); }
};
}
