#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
namespace cccaster::p2p {
struct HttpResult {
    unsigned status = 0, retryAfter = 0;
    std::vector<std::string> messages;
};
class Ntfy {
  public:
    explicit Ntfy(std::string server);
    HttpResult Poll(const std::string &topic);
    HttpResult Post(const std::string &topic, const std::string &body);
    const std::string &Server() const { return server_; }

  private:
    std::string server_;
};
class Subscription {
  public:
    Subscription(const Ntfy &client, std::string topic, std::function<void(const std::string &)> message,
                 std::function<void(const std::string &)> report, std::string since = "60s");
    ~Subscription();
    bool Ready() const { return ready_; }

  private:
    std::atomic<bool> stop_{false}, ready_{false};
    std::atomic<void *> request_{nullptr};
    std::thread thread_;
};
} // namespace cccaster::p2p
