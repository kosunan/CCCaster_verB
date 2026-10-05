#include "p2p/Ntfy.hpp"
#include <windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace cccaster::p2p {
namespace {
using Json = nlohmann::json;
struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h)
            WinHttpCloseHandle(h);
    }
};
std::wstring Wide(const std::string &s) {
    if (s.size() > 2048)
        throw std::invalid_argument("ntfy URL too long");
    for (unsigned char c : s)
        if (c < 32 || c > 126)
            throw std::invalid_argument("ntfy URL must be ASCII");
    return {s.begin(), s.end()};
}
struct Address {
    std::wstring host, path;
    INTERNET_PORT port;
    bool tls;
    Address(const std::string &server) {
        auto url = Wide(server);
        URL_COMPONENTS p{};
        p.dwStructSize = sizeof(p);
        p.dwHostNameLength = p.dwUrlPathLength = p.dwUserNameLength = p.dwPasswordLength =
            p.dwExtraInfoLength = DWORD(-1);
        if (!WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &p))
            throw std::invalid_argument("Invalid ntfy URL");
        host.assign(p.lpszHostName, p.dwHostNameLength);
        path.assign(p.lpszUrlPath, p.dwUrlPathLength);
        port = p.nPort;
        tls = p.nScheme == INTERNET_SCHEME_HTTPS;
        if (host.empty() || p.dwUserNameLength || p.dwPasswordLength || p.dwExtraInfoLength ||
            (!tls && !(p.nScheme == INTERNET_SCHEME_HTTP &&
                       (host == L"127.0.0.1" || host == L"localhost" || host == L"::1"))))
            throw std::invalid_argument("ntfy requires HTTPS (HTTP is allowed only on loopback)");
        while (!path.empty() && path.back() == L'/')
            path.pop_back();
    }
};
bool TopicValid(const std::string &s) {
    return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
               c == '-';
    });
}
struct Async {
    std::mutex mutex;
    std::condition_variable cv;
    DWORD event = 0, bytes = 0;
    bool closed = false;
    static void CALLBACK Callback(HINTERNET, DWORD_PTR context, DWORD status, void *data, DWORD length) {
        if (!context)
            return;
        auto &self = *reinterpret_cast<Async *>(context);
        std::lock_guard lock(self.mutex);
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
            self.closed = true;
        else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
                 status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
                 status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE ||
                 status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR ||
                 status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE) {
            self.event = status;
            self.bytes =
                status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE ? *static_cast<DWORD *>(data) : length;
        }
        self.cv.notify_all();
    }
    void Reset() {
        std::lock_guard lock(mutex);
        event = 0;
        bytes = 0;
    }
    bool Wait(DWORD expected, std::atomic<bool> *stop) {
        std::unique_lock lock(mutex);
        while (!event && !closed && !(stop && *stop))
            cv.wait_for(lock, std::chrono::milliseconds(100));
        return !(stop && *stop) && event == expected;
    }
    void Closed() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return closed; });
    }
};
HttpResult Request(const std::string &server, const std::string &topic, const std::string &suffix,
                   const std::string *body, std::atomic<void *> *active = nullptr,
                   std::atomic<bool> *stop = nullptr,
                   const std::function<bool(const std::string &)> &line = {}) {
    HttpResult result;
    if (!TopicValid(topic) || (body && body->size() > 4096))
        return result;
    Address address(server);
    const bool asynchronous = bool(line);
    Async async;
    Handle session{WinHttpOpen(L"CCCaster_B/P2P-v1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, asynchronous ? WINHTTP_FLAG_ASYNC : 0)};
    if (!session.h)
        return result;
    WinHttpSetTimeouts(session.h, 5000, 5000, 5000, line ? 120000 : 8000);
    if (address.tls) {
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    Handle conn{WinHttpConnect(session.h, address.host.c_str(), address.port, 0)};
    if (!conn.h)
        return result;
    auto path = address.path + L"/" + Wide(topic + suffix);
    HINTERNET req =
        WinHttpOpenRequest(conn.h, body ? L"POST" : L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, address.tls ? WINHTTP_FLAG_SECURE : 0);
    if (!req)
        return result;
    // ストリーム停止時には同じrequestだけを閉じ、二重Closeを避ける。
    if (asynchronous) {
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&async);
        if (!WinHttpSetOption(req, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
            WinHttpSetStatusCallback(req, Async::Callback,
                                     WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
                                     0) == WINHTTP_INVALID_STATUS_CALLBACK) {
            WinHttpCloseHandle(req);
            return result;
        }
    }
    struct RequestHandle {
        HINTERNET h;
        std::atomic<void *> *active;
        Async *async;
        ~RequestHandle() {
            if (active)
                active->store(nullptr);
            WinHttpCloseHandle(h);
            if (async)
                async->Closed();
        }
    } guard{req, active, asynchronous ? &async : nullptr};
    if (active)
        active->store(req);
    if (stop && *stop)
        return result;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    const wchar_t *headers = body ? L"Content-Type: text/plain; charset=utf-8\r\n" : nullptr;
    auto sent = WinHttpSendRequest(req, headers, body ? DWORD(-1) : 0, body ? (void *)body->data() : nullptr,
                                   body ? DWORD(body->size()) : 0, body ? DWORD(body->size()) : 0,
                                   asynchronous ? reinterpret_cast<DWORD_PTR>(&async) : 0);
    if (!sent && (!asynchronous || GetLastError() != ERROR_IO_PENDING))
        return result;
    if (asynchronous && !async.Wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, stop))
        return result;
    async.Reset();
    auto received = WinHttpReceiveResponse(req, nullptr);
    if (!received && (!asynchronous || GetLastError() != ERROR_IO_PENDING))
        return result;
    if (asynchronous && !async.Wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, stop))
        return result;
    DWORD n = sizeof(result.status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &result.status, &n, WINHTTP_NO_HEADER_INDEX);
    wchar_t retry[64]{};
    n = sizeof(retry);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"Retry-After", retry, &n, WINHTTP_NO_HEADER_INDEX))
        result.retryAfter = std::min(3600UL, wcstoul(retry, nullptr, 10));
    if (result.status != 200)
        return result;
    std::string buffer;
    char part[2048];
    DWORD read = 0;
    size_t total = 0;
    while (!(stop && *stop)) {
        DWORD available = 0;
        async.Reset();
        auto queried = WinHttpQueryDataAvailable(req, asynchronous ? nullptr : &available);
        if (!queried && (!asynchronous || GetLastError() != ERROR_IO_PENDING))
            break;
        if (asynchronous) {
            if (!async.Wait(WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, stop))
                break;
            available = async.bytes;
        }
        if (!available)
            break;
        async.Reset();
        auto ok = WinHttpReadData(req, part, std::min(DWORD(sizeof(part)), available),
                                  asynchronous ? nullptr : &read);
        if (!ok && (!asynchronous || GetLastError() != ERROR_IO_PENDING))
            break;
        if (asynchronous) {
            if (!async.Wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, stop))
                break;
            read = async.bytes;
        }
        if (!read)
            break;
        buffer.append(part, read);
        total += read;
        if (buffer.size() > 16384 || (!line && total > 65536))
            break;
        size_t end;
        while ((end = buffer.find('\n')) != std::string::npos) {
            auto item = buffer.substr(0, end);
            buffer.erase(0, end + 1);
            if (line) {
                if (!line(item))
                    return result;
            } else {
                auto j = Json::parse(item, nullptr, false);
                if (j.is_object() && j.value("event", std::string{}) == "message" && j.contains("message") &&
                    j["message"].is_string())
                    result.messages.push_back(j["message"].get<std::string>());
            }
        }
    }
    return result;
}
} // namespace
Ntfy::Ntfy(std::string server) : server_(std::move(server)) { Address validate(server_); }
HttpResult Ntfy::Poll(const std::string &topic) {
    return Request(server_, topic, "/json?poll=1&since=latest", nullptr);
}
HttpResult Ntfy::Post(const std::string &topic, const std::string &body) {
    return Request(server_, topic, "", &body);
}
Subscription::Subscription(const Ntfy &client, std::string topic,
                           std::function<void(const std::string &)> message,
                           std::function<void(const std::string &)> report, std::string initialSince) {
    thread_ =
        std::thread([this, server = client.Server(), topic = std::move(topic), message = std::move(message),
                     report = std::move(report), initialSince = std::move(initialSince)] {
            unsigned backoff = 1;
            std::string since = initialSince;
            while (!stop_) {
                bool eventSeen = false;
                ready_ = false;
                try {
                    auto result =
                        Request(server, topic, "/json?since=" + since, nullptr, &request_, &stop_,
                                [&](const std::string &line) {
                                    auto j = Json::parse(line, nullptr, false);
                                    if (!j.is_object())
                                        return true;
                                    const auto event = j.value("event", std::string{});
                                    if (event == "open" || event == "keepalive" || event == "message") {
                                        ready_ = true;
                                        eventSeen = true;
                                    }
                                    if (event == "message") {
                                        auto id = j.value("id", std::string{});
                                        if (!id.empty() && id.size() < 64 &&
                                            std::all_of(id.begin(), id.end(),
                                                        [](unsigned char c) { return std::isalnum(c); }))
                                            since = id;
                                        if (j.contains("message") && j["message"].is_string()) {
                                            auto text = j["message"].get<std::string>();
                                            if (text.size() <= 4096)
                                                message(text);
                                        }
                                    }
                                    return !stop_;
                                });
                    if (stop_)
                        break;
                    if (result.status == 429) {
                        report("rate_limited");
                        backoff = std::max(backoff, result.retryAfter);
                    } else
                        report("reconnecting");
                } catch (const std::exception &) {
                    if (!stop_)
                        report("reconnecting");
                }
                ready_ = false;
                if (eventSeen)
                    backoff = std::max(1U, backoff);
                for (unsigned i = 0; i < backoff * 10 && !stop_; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                backoff = std::min(60U, backoff * 2);
            }
        });
}
Subscription::~Subscription() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}
} // namespace cccaster::p2p
