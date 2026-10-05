#include "p2p/Matching.hpp"
#include "p2p/Ntfy.hpp"
#include "p2p/MatchingCleanup.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>

namespace cccaster::matching {
using namespace std::chrono_literals;
namespace {
using p2p::Now;
std::string Token() { return p2p::Hex(p2p::Random(8)); }
bool TokenValid(const std::string& value) { return value.size() == 16 && p2p::Unhex(value).size() == 8; }
Person ReadPerson(const Json& value) {
    return {value.at("id"), value.at("code"), value.at("name"), value.at("comment"), value.at("key"), {},
            value.at("spectators"), value.at("revision")};
}
} // namespace
struct Client::Impl {
    Options options;
    p2p::Ntfy ntfy;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::pair<std::string, std::string>> inbox;
    std::deque<Json> commands;
    std::vector<Event> events;
    Snapshot visible, state;
    std::atomic<bool> stop{false};
    std::thread thread;
    Directory directory;
    std::unique_ptr<Identity> identity;
    std::unique_ptr<p2p::Keys> keys;
    std::unique_ptr<p2p::Subscription> listing, control;
    std::unique_ptr<CleanupGuard> cleanup;
    std::map<std::string, std::shared_ptr<p2p::Keys>> peerKeys;
    Json registration;
    uint64_t revision = 0;
    int64_t listedAt = 0;
    bool allowSpectators = true, away = false, listingMayExist = false;
    std::string match, transportCode;
    Request selected;
    bool host = false, matchSpectators = true, launched = false;
    int64_t deadline = 0, nextPost = 0;
    std::set<std::string> seen;
    std::deque<std::string> seenOrder;
    struct Watcher { std::string id; int64_t since; };
    std::map<std::string, Watcher> watchers;

    explicit Impl(Options value) : options(std::move(value)), ntfy(options.server) {
        thread = std::thread([this] { Run(); });
    }
    ~Impl() { stop = true; wake.notify_all(); if (thread.joinable()) thread.join(); }
    void Queue(const std::string& type, const std::string& text) {
        std::lock_guard lock(mutex);
        if (inbox.size() < 4096) inbox.emplace_back(type, text);
        else if (inbox.back().first != "overflow") inbox.back() = {"overflow", ""};
        wake.notify_all();
    }
    void Publish() {
        if (state.publicVisible && directory.Expired(listedAt)) {
            state.publicVisible = false;
            state.notice = "listing_expired";
        }
        state.people = directory.People();
        state.incomplete = directory.incomplete;
        std::lock_guard lock(mutex);
        visible = state;
    }
    void Emit(Event value) { std::lock_guard lock(mutex); events.push_back(std::move(value)); }
    bool Post(const std::string& topic, const std::string& body) {
        if (Now() < nextPost) { state.error = "rate_limited"; return false; }
        const auto result = ntfy.Post(topic, body);
        if (result.status == 429) {
            nextPost = Now() + std::max(60U, result.retryAfter);
            if (cleanup) cleanup->Defer(nextPost);
            state.error = "rate_limited";
        } else if (result.status != 200) state.error = "service_unavailable";
        return result.status == 200;
    }
    std::shared_ptr<p2p::Keys> PeerKeys(const std::string& code) {
        if (auto found = peerKeys.find(code); found != peerKeys.end()) return found->second;
        auto result = std::make_shared<p2p::Keys>(code);
        if (peerKeys.size() >= 32) peerKeys.erase(peerKeys.begin());
        peerKeys[code] = result;
        return result;
    }
    bool Send(const Person& peer, const std::string& type, const std::string& request, Json extra = Json::object()) {
        if (!identity) return false;
        extra.update({{"v", 1}, {"kind", "matching-control"}, {"type", type}, {"message", Token()},
                      {"request", request}, {"target", peer.id}, {"from", registration}, {"ts", Now()}});
        auto target = PeerKeys(peer.code);
        const auto topic = ControlTopic(*target);
        return Post(topic, target->Seal(topic, identity->Sign(std::move(extra)).dump()));
    }
    void Notice(const Person& peer, const std::string& result) {
        directory.Result(peer.id, result); state.notice = result;
    }
    void RejectOthers() {
        for (const auto& request : state.incoming) if (request.id != selected.id)
            Send(request.peer, "reply", request.id, {{"result", "busy"}});
        state.incoming.clear();
        if (!state.outgoing.id.empty() && state.outgoing.id != selected.id)
            Send(state.outgoing.peer, "cancel", state.outgoing.id);
        state.outgoing = {};
    }
    void SuspendRequests() {
        for (const auto& request : state.incoming) Send(request.peer, "reply", request.id, {{"result", "paused"}});
        state.incoming.clear();
        if (!state.outgoing.id.empty()) Send(state.outgoing.peer, "cancel", state.outgoing.id);
        state.outgoing = {};
    }
    void ResetMatch(const std::string& reason, bool notify = true, bool finished = false) {
        if (notify && !selected.id.empty()) Send(selected.peer, "end", selected.id, {{"match", match}});
        Notice(selected.peer, reason);
        if (launched && !finished) {
            if (state.state != "playing") Emit({"abort", match});
            state.state = "ending"; deadline = 0; return;
        }
        selected = {}; match.clear(); transportCode.clear(); launched = false; deadline = 0;
        state.state = state.registered ? "waiting" : "idle";
    }
    Json Record(const std::string& action) {
        auto result = registration;
        result["action"] = action; result["revision"] = ++revision;
        const auto now = Now();
        if (action == "register") result["listed_at"] = now;
        if (action == "register" || action == "remove")
            result["cleanup_before"] = now - PublicListingLifetimeSeconds;
        return identity->Sign(std::move(result));
    }
    bool ArmCleanup(bool listed) {
        if (!cleanup) cleanup=std::make_unique<CleanupGuard>(options.cleanupDirectory);
        Json records=Json::array();
        if (listed) records.push_back({{"topic",options.directoryTopic},
            {"body",SealDirectory(options.directoryTopic,Record("remove"))}});
        records.push_back({{"topic",keys->status},{"body",keys->Seal(keys->status,Record("closed").dump())}});
        if (cleanup->Arm({{"server",options.server},{"records",records},{"not_before",nextPost}})) return true;
        state.error="cleanup_unavailable";return false;
    }
    void RecoverOrphans() {
        const auto recovered=RecoverCleanup(options.cleanupDirectory,options.server,[this](const auto& topic,const auto& body) {
            if (stop || !Post(topic,body)) return false;
            if (topic==options.directoryTopic) {
                Json record;
                if (OpenDirectory(topic,body,record)) directory.Apply(record);
            }
            return true;
        },nextPost);
        state.cleanupPending=recovered.pending;
        if (recovered.pending) state.error=Now()<nextPost?"rate_limited":"cleanup_pending";
        else if (recovered.completed) { state.error.clear();state.notice="cleanup_finished"; }
    }
    bool Visibility(bool value) {
        if (state.publicVisible == value && (value || !listingMayExist)) return true;
        auto record = Record(value ? "register" : "remove");
        // 投稿が届いた直後に親が落ちても、より新しいrevisionの取消を残す。
        if (value) {
            if (!ArmCleanup(true)) return false;
            listingMayExist=true;
        }
        if (!Post(options.directoryTopic, SealDirectory(options.directoryTopic, record))) return false;
        state.publicVisible = value; directory.Apply(record);
        if (value) {
            listedAt = record["listed_at"];
            if (state.notice == "listing_expired") state.notice.clear();
        } else {
            listingMayExist=false;
            ArmCleanup(false);
        }
        return true;
    }
    void Start(const Json& command) {
        if (state.registered || !match.empty()) return;
        state.error.clear(); state.notice.clear(); state.state = "registering"; Publish();
        const auto name = command.value("name", std::string("PLAYER"));
        const auto comment = command.value("comment", std::string{});
        if (name.empty() || name.size() > 31 || comment.size() > 160) { state.error = "invalid_profile"; state.state="idle"; return; }
        identity = std::make_unique<Identity>();
        state.id = IdentityId(*identity);
        state.code.clear();
        for (unsigned attempt = 0; attempt < 8 && !stop; ++attempt) {
            state.code = p2p::NewCode(); keys = std::make_unique<p2p::Keys>(state.code);
            const auto prior = ntfy.Poll(keys->status);
            if (prior.status != 200) { state.error = "service_unavailable"; state.state="idle"; return; }
            bool used = false;
            for (const auto& body : prior.messages) {
                std::string plain;
                if (!keys->Open(keys->status, body, plain)) continue;
                const auto record = Json::parse(plain, nullptr, false);
                used |= record.is_object() && record.value("action", std::string{}) != "closed" &&
                    record.value("state", std::string{}) != "closed";
            }
            if (!used) break;
            if (attempt == 7) { state.error="code_collision"; state.state="idle"; return; }
        }
        if (stop) return;
        allowSpectators = command.value("spectators", true);
        revision = 1;
        registration = identity->Sign({{"v", 1}, {"kind", "matching"}, {"id", state.id}, {"code", state.code},
            {"name", name}, {"comment", comment}, {"spectators", allowSpectators},
            {"action", "register"}, {"revision", revision}, {"created", Now()}});
        if (!Registration(registration)) { state.error="invalid_profile"; state.state="idle"; return; }
        cleanup.reset();listingMayExist=false;
        control = std::make_unique<p2p::Subscription>(ntfy, ControlTopic(*keys),
            [this](const auto& text) { Queue("control", text); },
            [this](const auto& text) { Queue("service", text); }, "60s");
        const auto ready = std::chrono::steady_clock::now() + 8s;
        while (!stop && !control->Ready() && std::chrono::steady_clock::now() < ready) std::this_thread::sleep_for(20ms);
        if (stop || !control->Ready() || !ArmCleanup(false) || !Post(keys->status, keys->Seal(keys->status, registration.dump()))) {
            control.reset(); state.state="idle"; if(state.error.empty())state.error="service_unavailable"; return;
        }
        // 公開サービスのキャッシュ反映を待ってから利用者にコードを渡す。
        const auto settled = std::chrono::steady_clock::now() + 2s;
        while (!stop && std::chrono::steady_clock::now() < settled) std::this_thread::sleep_for(20ms);
        state.registered = true; state.publicVisible = false; state.paused = false; state.state = "waiting";
        seen.clear(); seenOrder.clear(); state.incoming.clear(); state.outgoing = {};
        if (command.value("public", false)) Visibility(true);
    }
    void WatchReply(const std::string& id, const std::string& status) {
        auto value = identity->Sign({{"v", 1}, {"kind", "matching-watch-reply"}, {"request", id},
            {"id", state.id}, {"state", status}, {"match", match}, {"code", transportCode}, {"ts", Now()}});
        auto topic = keys->Answer("watch-" + id);
        Post(topic, keys->Seal(topic, value.dump()));
    }
    void NotifyWatchers(const std::string& status) {
        for (const auto& [id, unused] : watchers) WatchReply(id, status);
        watchers.clear();
    }
    void StopRegistration() {
        if (!state.registered) return;
        state.paused = true;
        if (!Visibility(false)) return;
        auto closed = Record("closed");
        if (!Post(keys->status, keys->Seal(keys->status, closed.dump()))) return;
        if (cleanup) { cleanup->Disarm();cleanup.reset(); }
        for (const auto& request : state.incoming) Send(request.peer, "reply", request.id, {{"result", "closed"}});
        state.incoming.clear();
        if (!state.outgoing.id.empty()) Send(state.outgoing.peer, "cancel", state.outgoing.id);
        state.outgoing = {};
        NotifyWatchers("closed");
        state.registered = false;
        if (!match.empty() && state.state != "playing") ResetMatch("cancelled");
        if (match.empty()) state.state = "idle";
        // 対戦中の登録取消はゲームを終了しない。進行中の相手への制御だけ残す。
    }
    void Invite(const std::string& rawCode) {
        auto code = p2p::NormalizeCode(rawCode);
        if (!state.registered || state.paused || away || !match.empty() || !state.outgoing.id.empty()) return;
        if (code.empty() || code == state.code) { state.error="invalid_code"; return; }
        auto target = PeerKeys(code);
        const auto lookup = ntfy.Poll(target->status);
        Json record;
        for (const auto& body : lookup.messages) {
            std::string plain;
            if (target->Open(target->status, body, plain)) {
                auto value = Json::parse(plain, nullptr, false);
                if (Registration(value) && value["code"] == code) record = std::move(value);
            }
        }
        if (record.is_null()) { state.error="unavailable"; return; }
        const auto peer = ReadPerson(record);
        if (record["action"] == "closed") { Notice(peer, "closed"); return; }
        const auto incoming = std::find_if(state.incoming.begin(), state.incoming.end(),
            [&](const auto& request) { return request.peer.id == peer.id && request.expires > Now(); });
        if (incoming != state.incoming.end()) { Accept(incoming->id); return; }
        Request request{Token(), peer, Now() + options.responseSeconds};
        if (Send(peer, "invite", request.id, {{"expires", request.expires}, {"spectators", allowSpectators}})) {
            state.outgoing = request; Notice(peer, "pending");
        }
    }
    void Accept(const std::string& id) {
        if (!state.registered || state.paused || away || !match.empty()) return;
        auto it = std::find_if(state.incoming.begin(), state.incoming.end(), [&](const auto& r) { return r.id == id && r.expires > Now(); });
        if (it == state.incoming.end()) return;
        selected = *it; match = Token(); host = true; matchSpectators = allowSpectators && selected.peer.allowSpectators;
        if (!Send(selected.peer, "offer", selected.id, {{"match", match}, {"spectators", matchSpectators}})) {
            selected = {}; match.clear(); return;
        }
        state.state = "confirming"; deadline = Now() + options.connectionSeconds; RejectOthers();
    }
    void Handle(Json command) {
        const auto type = command.at("type").get<std::string>();
        state.error.clear();
        if (type == "start") Start(command);
        else if (type == "stop") StopRegistration();
        else if (type == "cleanup" && match.empty() && state.outgoing.id.empty()) RecoverOrphans();
        else if (type == "visibility" && state.registered) Visibility(command.at("public"));
        else if (type == "pause") { state.paused = command.at("paused"); if (state.paused) SuspendRequests(); }
        else if (type == "activity") { away = command.at("busy"); if (away) SuspendRequests(); }
        else if (type == "spectators" && match.empty()) {
            allowSpectators = command.at("allowed");
            if (!allowSpectators) NotifyWatchers("disabled");
        }
        else if (type == "invite") Invite(command.at("code"));
        else if (type == "accept") Accept(command.at("request"));
        else if (type == "reject") {
            auto id = command.at("request").get<std::string>();
            for (const auto& r : state.incoming) if (r.id == id) Send(r.peer, "reply", id, {{"result", "declined"}});
            std::erase_if(state.incoming, [&](const auto& r) { return r.id == id; });
        } else if (type == "cancel") {
            if (!state.outgoing.id.empty()) Send(state.outgoing.peer, "cancel", state.outgoing.id);
            state.outgoing = {}; state.notice = "cancelled";
        } else if (type == "cancel_match" && !match.empty() && state.state != "playing") {
            ResetMatch("cancelled");
        } else if (type == "host_ready" && command.at("match") == match && host && state.state == "connecting") {
            auto code = p2p::NormalizeCode(command.at("code"));
            if (transportCode.empty() && !code.empty()) {
                transportCode = code;
                if (!Send(selected.peer, "connect", selected.id, {{"match", match}, {"code", code}, {"spectators", matchSpectators}}))
                    ResetMatch("connection_failed");
            }
        } else if (type == "playing" && command.at("match") == match && !match.empty()) {
            state.state = "playing"; deadline = 0;
            NotifyWatchers(matchSpectators && allowSpectators ? "ready" : "disabled");
        } else if (type == "finished" && command.at("match") == match && !match.empty()) ResetMatch("finished", true, true);
    }
    void Receive(const std::string& body) {
        if (!keys || !identity) return;
        std::string plain;
        if (!keys->Open(ControlTopic(*keys), body, plain)) return;
        const auto value = Json::parse(plain, nullptr, false);
        if (!value.is_object() || value.at("v") != 1 || value.at("ts").get<int64_t>() < Now() - 120 ||
            value.at("ts").get<int64_t>() > Now() + 60 || value.at("target") != state.id) return;
        const auto id = value.at("request").get<std::string>();
        if (!TokenValid(id)) return;
        if (value.at("kind") == "matching-watch") {
            if (!state.registered) { WatchReply(id, "closed"); return; }
            if (!allowSpectators || (!match.empty() && !matchSpectators)) { WatchReply(id, "disabled"); return; }
            if (state.state == "playing" && !transportCode.empty()) { WatchReply(id, "ready"); return; }
            if (watchers.contains(id) || watchers.size() >= 64) return;
            watchers[id] = {id, Now()}; WatchReply(id, "waiting"); return;
        }
        if (value.at("kind") != "matching-control" || !Verified(value) ||
            !Registration(value.at("from")) || value.at("key") != value.at("from").at("key")) return;
        auto peer = ReadPerson(value["from"]);
        if (peer.id == state.id) return;
        const auto messageId = value.at("message").get<std::string>();
        if (!TokenValid(messageId) || seen.contains(messageId)) return;
        seen.insert(messageId); seenOrder.push_back(messageId);
        if (seenOrder.size() > 1024) { seen.erase(seenOrder.front()); seenOrder.pop_front(); }
        const auto type = value.at("type").get<std::string>();
        const bool outgoing = id == state.outgoing.id && peer.id == state.outgoing.peer.id;
        const bool active = id == selected.id && peer.id == selected.peer.id && value.value("match", std::string{}) == match && !match.empty();
        if (type == "invite") {
            const auto expires = value.at("expires").get<int64_t>();
            if (expires <= Now() || expires > Now() + 90) return;
            const std::string refusal = !state.registered ? "closed" : !match.empty() ? "busy" : (state.paused || away) ? "paused" : "";
            if (!refusal.empty()) { Send(peer, "reply", id, {{"result", refusal}}); return; }
            if (state.incoming.size() >= 8) { Send(peer, "reply", id, {{"result", "busy"}}); return; }
            if (std::any_of(state.incoming.begin(), state.incoming.end(), [&](const auto& r) { return r.id == id || r.peer.id == peer.id; })) return;
            peer.allowSpectators = value.at("spectators").get<bool>();
            // 双方からの申し込みは一組へまとめ、IDの小さい側を承諾側にする。
            if (!state.outgoing.id.empty() && state.outgoing.peer.id == peer.id) {
                if (state.id < peer.id) {
                    Send(peer, "cancel", state.outgoing.id); state.outgoing = {};
                    state.incoming.push_back({id, peer, expires}); Accept(id);
                }
                return;
            }
            state.incoming.push_back({id, peer, expires}); ++state.notifications;
            Send(peer, "reply", id, {{"result", "pending"}});
        } else if (type == "reply" && outgoing) {
            const auto result = value.at("result").get<std::string>();
            if (result != "pending" && result != "busy" && result != "paused" && result != "declined" && result != "closed") return;
            Notice(peer, result);
            if (result != "pending") state.outgoing = {};
        } else if (type == "cancel") {
            std::erase_if(state.incoming, [&](const auto& r) { return r.id == id && r.peer.id == peer.id; });
            if (id == selected.id && peer.id == selected.peer.id && state.state != "playing") ResetMatch("cancelled", false);
        } else if (type == "offer" && outgoing && match.empty() && state.registered && !state.paused && !away && state.outgoing.expires > Now()) {
            const auto proposed = value.at("match").get<std::string>();
            if (!TokenValid(proposed)) return;
            selected = state.outgoing; match = proposed; host = false;
            matchSpectators = allowSpectators && value.at("spectators").get<bool>();
            state.state = "confirming"; deadline = Now() + options.connectionSeconds; RejectOthers();
            if (!Send(peer, "confirm", id, {{"match", match}, {"spectators", matchSpectators}})) ResetMatch("connection_failed", false);
        } else if (type == "confirm" && active && host && state.state == "confirming") {
            matchSpectators = matchSpectators && value.at("spectators").get<bool>();
            state.state = "connecting"; launched = true;
            Emit({"launch", match, {}, true, matchSpectators});
        } else if (type == "connect" && active && !host && state.state == "confirming") {
            auto code = p2p::NormalizeCode(value.at("code"));
            if (code.empty()) return;
            transportCode = code; state.state = "connecting"; launched = true;
            Emit({"launch", match, code, false, matchSpectators});
        } else if (type == "end" && active) ResetMatch("finished", false);
    }
    void Tick() {
        const auto now = Now();
        std::erase_if(state.incoming, [&](const auto& r) { return r.expires <= now; });
        if (!state.outgoing.id.empty() && state.outgoing.expires <= now) {
            Notice(state.outgoing.peer, "no_response"); Send(state.outgoing.peer, "cancel", state.outgoing.id); state.outgoing = {};
        }
        if (deadline && now >= deadline) ResetMatch("connection_failed");
        std::erase_if(watchers, [&](const auto& item) { return item.second.since < now - 86400; });
    }
    void Run() {
        try {
            listing = std::make_unique<p2p::Subscription>(ntfy, options.directoryTopic,
                [this](const auto& text) { Queue("directory", text); },
                [this](const auto& text) { Queue("service", text); }, "all");
            RecoverOrphans();
            while (!stop) {
                std::deque<Json> work;
                std::deque<std::pair<std::string, std::string>> messages;
                {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, 100ms, [&] { return stop || !inbox.empty() || !commands.empty(); });
                    work.swap(commands); messages.swap(inbox);
                }
                // 到着済みの取消を先に反映し、古い画面の承諾を無効にする。
                for (const auto& [type, body] : messages) try {
                    if (type == "directory") {
                        Json record;
                        if (OpenDirectory(options.directoryTopic, body, record)) directory.Apply(record);
                    }
                    else if (type == "control") Receive(body);
                    else if (type == "overflow") directory.incomplete = true;
                    else state.service = body;
                } catch (const std::exception&) { /* 不正な外部メッセージを無視 */ }
                for (auto& command : work) try { Handle(std::move(command)); }
                    catch (const std::exception&) {
                        state.error = "operation_failed";
                        if (state.state == "registering" && !state.registered) {
                            control.reset(); state.state = "idle";
                        }
                    }
                Tick();
                if (listing->Ready() && (!control || control->Ready())) state.service = "online";
                Publish();
            }
        } catch (const std::exception&) { state.error="service_unavailable"; Publish(); }
        try { StopRegistration(); } catch (const std::exception&) { /* 未送信分は別プロセスから試みる。 */ }
        control.reset(); listing.reset();
    }
};
Client::Client(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Client::~Client() = default;
Snapshot Client::View() const { std::lock_guard lock(impl_->mutex); return impl_->visible; }
std::vector<Event> Client::Events() { std::lock_guard lock(impl_->mutex); std::vector<Event> events; events.swap(impl_->events); return events; }
void Client::Command(Json command) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->commands.size() < 32) impl_->commands.push_back(std::move(command));
    impl_->wake.notify_all();
}

std::string Watch(const std::string& code, const std::string& server, const Json& registration,
                  const std::function<bool()>& cancelled, const std::function<void(const std::string&)>& report) {
    if (!Registration(registration)) return {};
    auto say = [&](const std::string& value) { if (report) report("[WATCH_STATUS] " + value); };
    if (registration["action"] == "closed") { say("closed"); return {}; }
    p2p::Keys keys(code); p2p::Ntfy ntfy(server);
    const auto request = Token(), replyTopic = keys.Answer("watch-" + request);
    std::mutex mutex; std::deque<std::string> inbox;
    p2p::Subscription subscription(ntfy, replyTopic, [&](const auto& text) { std::lock_guard lock(mutex); if(inbox.size()<32)inbox.push_back(text); },
        [&](const auto& text) { if(report)report("[WATCH_STATUS] " + text); }, "60s");
    auto limit = std::chrono::steady_clock::now() + 8s;
    while (!cancelled() && !subscription.Ready() && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(20ms);
    if (cancelled()) return {};
    Json value{{"v", 1}, {"kind", "matching-watch"}, {"request", request}, {"target", registration["id"]}, {"ts", Now()}};
    const auto topic = ControlTopic(keys);
    if (ntfy.Post(topic, keys.Seal(topic, value.dump())).status != 200) { say("unavailable"); return {}; }
    auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!cancelled()) {
        std::deque<std::string> messages;
        { std::lock_guard lock(mutex); messages.swap(inbox); }
        for (const auto& body : messages) try {
            std::string plain;
            if (!keys.Open(replyTopic, body, plain)) continue;
            const auto answer = Json::parse(plain, nullptr, false);
            if (!Verified(answer) || answer.at("key") != registration.at("key") || answer.at("request") != request ||
                answer.at("kind") != "matching-watch-reply" || answer.at("id") != registration.at("id")) continue;
            const auto state = answer.at("state").get<std::string>();
            if (state == "closed" || state == "disabled") { say(state); return {}; }
            if (state == "ready") return p2p::NormalizeCode(answer.at("code"));
            if (state == "waiting") { say("standby"); deadline = std::chrono::steady_clock::time_point::max(); }
        } catch (const std::exception&) {}
        if (std::chrono::steady_clock::now() >= deadline) { say("unavailable"); return {}; }
        std::this_thread::sleep_for(20ms);
    }
    say("cancelled"); return {};
}
} // namespace cccaster::matching
