#pragma once
#include "p2p/Protocol.hpp"
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <map>
#include <functional>

namespace cccaster::matching {
using p2p::Json;
const std::string& DirectoryTopic();
inline constexpr size_t PageSize = 20;
inline constexpr int64_t PublicListingLifetimeSeconds = 6 * 60 * 60;

// 登録ごとに生成する署名鍵。公開コードを知る第三者による掲載取消を防ぐ。
class Identity {
    void* key_ = nullptr;
public:
    Identity();
    ~Identity();
    Identity(const Identity&) = delete;
    Identity& operator=(const Identity&) = delete;
    std::string publicKey;
    Json Sign(Json value) const;
};
bool Verified(const Json& value);
std::string IdentityId(const Identity& identity);
bool Registration(const Json& value);
std::string ControlTopic(const p2p::Keys& keys);

struct Person {
    std::string id, code, name, comment, publicKey, result;
    bool allowSpectators = true;
    uint64_t revision = 0;
    int64_t listedAt = 0;
};
class Directory {
    std::map<std::string, Person> people_;
    std::map<std::string, uint64_t> versions_;
    std::vector<std::string> order_;
    int64_t cleanupBefore_ = 0;
public:
    bool incomplete = false;
    bool Apply(const Json& value, int64_t now = p2p::Now());
    bool Expired(int64_t listedAt) const { return listedAt > 0 && listedAt <= cleanupBefore_; }
    std::vector<Person> People() const;
    void Result(const std::string& id, const std::string& result);
};
struct Request {
    std::string id;
    Person peer;
    int64_t expires = 0;
};
struct Snapshot {
    bool registered = false, publicVisible = false, paused = false, incomplete = false;
    std::string code, id, state = "idle", notice, error, service = "connecting";
    std::vector<Person> people;
    std::vector<Request> incoming;
    Request outgoing;
    uint64_t notifications = 0;
};
struct Event {
    std::string type, match, code;
    bool host = false, allowSpectators = true;
};
struct Options {
    std::string server = "https://ntfy.sh", directoryTopic = DirectoryTopic();
    unsigned responseSeconds = 60, connectionSeconds = 45;
};
// 通信と状態変更は一つの専用スレッドで直列化。描画スレッドをHTTPで止めない。
class Client {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit Client(Options options = {});
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Snapshot View() const;
    std::vector<Event> Events();
    void Command(Json command);
};

// 個人コードから、今回の一組だけの観戦用接続コードを解決する。
std::string Watch(const std::string& code, const std::string& server, const Json& registration,
                  const std::function<bool()>& cancelled,
                  const std::function<void(const std::string&)>& report);
} // namespace cccaster::matching
