#include "p2p/MatchingCleanup.hpp"
#include "p2p/Ntfy.hpp"
#include <windows.h>
#include <wincrypt.h>
#include "shared_contracts/NativePath.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

namespace cccaster::matching {
namespace {
using Json = p2p::Json;
constexpr DWORD Limit = 16384;
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
    void Reset() { if(value)CloseHandle(value);value=nullptr; }
};
bool Read(HANDLE pipe, void* data, DWORD size) {
    for (DWORD total=0; total<size;) {
        DWORD read=0;
        if (!ReadFile(pipe, static_cast<char*>(data)+total, size-total, &read, nullptr) || !read) return false;
        total+=read;
    }
    return true;
}
bool Write(HANDLE pipe, const void* data, DWORD size) {
    DWORD written=0;
    return WriteFile(pipe, data, size, &written, nullptr) && written==size;
}
bool Valid(const Json& payload) {
    if (!payload.is_object() || !payload.contains("server") || !payload["server"].is_string() ||
        !payload.contains("records") || !payload["records"].is_array() || payload["records"].size()>2) return false;
    p2p::Ntfy validate(payload["server"]);
    for (const auto& item : payload["records"]) {
        if (!item.is_object() || !item.contains("topic") || !item["topic"].is_string() ||
            !item.contains("body") || !item["body"].is_string()) return false;
        const auto topic=item["topic"].get<std::string>(), body=item["body"].get<std::string>();
        if (topic.size()!=32 || topic.find_first_not_of("0123456789abcdef")!=std::string::npos ||
            body.size()>4096 || p2p::Unbase64(body).size()<28) return false;
    }
    return payload.value("not_before", int64_t(0))>=0;
}
HANDLE Lease(const std::filesystem::path& journal) {
    auto path=journal;path+=L".lock";
    const auto handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_HIDDEN|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
    return handle==INVALID_HANDLE_VALUE?nullptr:handle;
}
bool Save(const std::filesystem::path& path, const Json& payload) {
    if (path.empty()) return true;
    if (payload["records"].empty()) return DeleteFileW(path.c_str()) || GetLastError()==ERROR_FILE_NOT_FOUND;
    const auto text=payload.dump();
    if (text.size()>Limit) return false;
    DATA_BLOB input{DWORD(text.size()),reinterpret_cast<BYTE*>(const_cast<char*>(text.data()))}, encrypted{};
    if (!CryptProtectData(&input,L"CCCaster matching cancellation",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted)) return false;
    auto temporary=path;temporary+=L".tmp";
    Handle file{CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_HIDDEN|FILE_FLAG_WRITE_THROUGH,nullptr)};
    bool ok=false;
    if (file.value==INVALID_HANDLE_VALUE) file.value=nullptr;
    else ok=Write(file.value,encrypted.pbData,encrypted.cbData) && FlushFileBuffers(file.value);
    LocalFree(encrypted.pbData);file.Reset();
    if (ok) ok=MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}
Json Load(const std::filesystem::path& path) {
    const auto size=std::filesystem::file_size(path);
    if (!size || size>65536) throw std::runtime_error("invalid cancellation file");
    std::string bytes(size,'\0');std::ifstream file(path,std::ios::binary);
    if (!file.read(bytes.data(),bytes.size())) throw std::runtime_error("cannot read cancellation file");
    DATA_BLOB input{DWORD(bytes.size()),reinterpret_cast<BYTE*>(bytes.data())}, plain{};
    if (!CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain))
        throw std::runtime_error("cannot open cancellation file");
    const auto text=std::string(reinterpret_cast<char*>(plain.pbData),plain.cbData);LocalFree(plain.pbData);
    auto payload=Json::parse(text);
    if (!Valid(payload)) throw std::runtime_error("invalid cancellation payload");
    return payload;
}
std::filesystem::path Journal(const Json& payload) {
    return Utf8Path(payload.value("journal",std::string{}));
}
}
struct CleanupGuard::Impl {
    Handle writer, acknowledgment, process, lease;
    std::filesystem::path directory, journal;
    Json pending;
    bool healthy=true;
    bool Start() {
        if (process.value) return WaitForSingleObject(process.value,0)==WAIT_TIMEOUT;
        writer.Reset();acknowledgment.Reset();
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        Handle reader, reply;
        if (!CreatePipe(&reader.value,&writer.value,&security,Limit+4) ||
            !CreatePipe(&acknowledgment.value,&reply.value,&security,64)) return false;
        if (!SetHandleInformation(writer.value,HANDLE_FLAG_INHERIT,0) ||
            !SetHandleInformation(acknowledgment.value,HANDLE_FLAG_INHERIT,0)) return false;
        SIZE_T size=0;
        InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<unsigned char> storage(size);
        auto attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes,1,0,&size)) return false;
        Handle inheritedLease;
        if (lease.value && !DuplicateHandle(GetCurrentProcess(),lease.value,GetCurrentProcess(),&inheritedLease.value,0,TRUE,DUPLICATE_SAME_ACCESS)) {
            DeleteProcThreadAttributeList(attributes);return false;
        }
        HANDLE inherited[]{reader.value,reply.value,inheritedLease.value};
        bool updated=UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                               inherited,(inheritedLease.value?3:2)*sizeof(HANDLE),nullptr,nullptr)!=FALSE;
        wchar_t path[32768]{};
        const auto length=GetModuleFileNameW(nullptr,path,32768);
        std::wstring command=L"\""+std::wstring(path)+L"\" --matching-cleanup "+
            std::to_wstring(reinterpret_cast<uintptr_t>(reader.value))+L" "+
            std::to_wstring(reinterpret_cast<uintptr_t>(reply.value))+L" "+
            std::to_wstring(reinterpret_cast<uintptr_t>(inheritedLease.value));
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);
        startup.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;
        startup.lpAttributeList=attributes;
        PROCESS_INFORMATION child{};
        bool created=updated && length && length<32768 && CreateProcessW(path,command.data(),nullptr,nullptr,TRUE,
            EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW,nullptr,nullptr,&startup.StartupInfo,&child);
        DeleteProcThreadAttributeList(attributes);
        if (!created) return false;
        CloseHandle(child.hThread);process.value=child.hProcess;
        return true;
    }
    bool Update(Json payload) {
        if (!Valid(payload)) return false;
        if (!directory.empty()) {
            if (journal.empty()) {
                std::error_code error;std::filesystem::create_directories(directory,error);
                if (error) return false;
                journal=std::filesystem::absolute(directory)/(p2p::Hex(p2p::Random(16))+".pending");
                lease.value=Lease(journal);
                if (!lease.value) { journal.clear();return false; }
            }
            payload["journal"]=PathUtf8(journal);
            // 公開投稿より先に、取消だけをWindowsユーザーの鍵で保護して保存する。
            if (!Save(journal,payload)) return false;
        }
        if (!healthy) return false;
        auto fail=[this] { healthy=false;return false; };
        if (!Start()) return fail();
        const auto text=payload.dump();
        if (text.size()>Limit) return fail();
        DWORD length=DWORD(text.size());
        if (!Write(writer.value,&length,sizeof(length)) || !Write(writer.value,text.data(),length)) return fail();
        const auto deadline=GetTickCount64()+5000;
        while (GetTickCount64()<deadline) {
            DWORD available=0;
            if (!PeekNamedPipe(acknowledgment.value,nullptr,0,nullptr,&available,nullptr)) return fail();
            if (available) {
                char ok=0;
                if (!Read(acknowledgment.value,&ok,1) || ok!=1) return fail();
                pending=std::move(payload);return true;
            }
            if (WaitForSingleObject(process.value,0)!=WAIT_TIMEOUT) return fail();
            Sleep(10);
        }
        return fail();
    }
};
CleanupGuard::CleanupGuard(const std::filesystem::path& directory):impl_(std::make_unique<Impl>()){impl_->directory=directory;}
CleanupGuard::~CleanupGuard()=default;
bool CleanupGuard::Arm(Json payload) { return impl_->Update(std::move(payload)); }
void CleanupGuard::Defer(int64_t until) {
    if (impl_->pending.is_object()) { auto payload=impl_->pending;payload["not_before"]=until;impl_->Update(std::move(payload)); }
}
void CleanupGuard::Disarm() {
    if (impl_->pending.is_object()) { auto payload=impl_->pending;payload["records"]=Json::array();impl_->Update(std::move(payload)); }
}
int RunCleanupWorker(uintptr_t input, uintptr_t acknowledgment, uintptr_t lease) {
    Handle reader{reinterpret_cast<HANDLE>(input)}, reply{reinterpret_cast<HANDLE>(acknowledgment)}, heldLease{reinterpret_cast<HANDLE>(lease)};
    Json pending;
    try {
        for (;;) {
            DWORD size=0;
            if (!Read(reader.value,&size,sizeof(size)) || !size || size>Limit) break;
            std::string text(size,'\0');
            if (!Read(reader.value,text.data(),size)) break;
            auto payload=Json::parse(text,nullptr,false);
            if (!Valid(payload)) break;
            pending=std::move(payload);
            const char ok=1;
            if (!Write(reply.value,&ok,1)) break;
        }
        if (!pending.is_object() || pending["records"].empty()) return 0;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
        auto delay=std::max(int64_t(0),pending.value("not_before",int64_t(0))-p2p::Now());
        p2p::Ntfy client(pending["server"]);
        for (unsigned attempt=0;attempt<3;++attempt) {
            if (std::chrono::steady_clock::now()+std::chrono::seconds(delay)>deadline) return 1;
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            delay=attempt?4:1;
            auto& records=pending["records"];
            for (size_t i=0;i<records.size();) {
                const auto result=client.Post(records[i]["topic"],records[i]["body"]);
                if (result.status==200) {
                    records.erase(records.begin()+i);
                    if (!Save(Journal(pending),pending)) return 1;
                }
                else if (result.status==429) {
                    delay=std::max(60U,result.retryAfter);pending["not_before"]=p2p::Now()+delay;
                    Save(Journal(pending),pending);break;
                }
                else ++i;
            }
            if (records.empty()) return 0;
        }
    } catch (const std::exception&) { return 1; }
    return 1;
}
CleanupRecovery RecoverCleanup(const std::filesystem::path& directory, const std::string& server,
    const std::function<bool(const std::string&, const std::string&)>& post, int64_t& nextPost) {
    CleanupRecovery result;
    if (directory.empty()) return result;
    std::error_code error;
    if (!std::filesystem::exists(directory,error)) { if(error)++result.pending;return result; }
    struct Job { Json payload; std::unique_ptr<Handle> lease; };
    std::vector<Job> jobs;
    try { for (const auto& entry:std::filesystem::directory_iterator(directory,error)) {
        const auto path=entry.path();const auto name=path.stem().string();
        if (path.extension()!=L".pending" || name.size()!=32 || name.find_first_not_of("0123456789abcdef")!=std::string::npos) continue;
        auto lease=std::make_unique<Handle>();lease->value=Lease(path);
        if (!lease->value) {
            const auto reason=GetLastError();
            // 同じGUIや補助プロセスが動作中の募集には触らない。
            if (reason!=ERROR_SHARING_VIOLATION && reason!=ERROR_LOCK_VIOLATION) ++result.pending;
            continue;
        }
        try {
            auto payload=Load(path);
            if (payload["server"]!=server) continue;
            payload["journal"]=PathUtf8(path);
            nextPost=std::max(nextPost,payload.value("not_before",int64_t(0)));
            jobs.push_back({std::move(payload),std::move(lease)});
        } catch (const std::exception&) { ++result.pending; }
    } } catch (const std::exception&) { ++result.pending; }
    if (error) ++result.pending;
    // 全保存データの429待機期限を確認してから、同じサーバーへの再送を始める。
    unsigned attempted=0;
    for (auto& job:jobs) {
        try {
            auto& payload=job.payload;auto& records=payload["records"];
            const auto path=Journal(payload);
            if (attempted++>=8 || p2p::Now()<nextPost) { ++result.pending;continue; }
            bool saved=records.empty()?Save(path,payload):true;
            while (!records.empty()) {
                if (!post(records[0]["topic"],records[0]["body"])) {
                    payload["not_before"]=nextPost;
                    Save(path,payload);break;
                }
                records.erase(records.begin());
                if (!(saved=Save(path,payload))) break;
            }
            if (records.empty() && saved) ++result.completed;
            else ++result.pending;
        } catch (const std::exception&) { ++result.pending; }
    }
    return result;
}
}
