#pragma once
#include <charconv>
#include <cstdint>
#include <iterator>
#include <ostream>
#include <string>
#include <string_view>

namespace cccaster::session_diagnostics {
enum class Code { None, Completed, Cancelled, UserExit, PeerExit, Disconnected, StateFailure,
    InitTimeout, UnexpectedExit, IpcFailure, Exception, ConnectionFailure };
inline const char* Name(Code code) {
    constexpr const char* names[]{"none", "completed", "cancelled", "user_exit", "peer_exit", "disconnected",
        "state_failure", "init_timeout", "unexpected_exit", "ipc_failure", "exception", "connection_failure"};
    return unsigned(code) < std::size(names) ? names[unsigned(code)] : "none";
}
// 行頭・終端が揃った診断行だけを読む。名前・パス・途中書込みは診断にしない。
inline std::string_view LastLine(std::string_view log, std::string_view prefix) {
    std::string_view found;
    for (size_t begin=0, end; (end=log.find('\n',begin))!=log.npos; begin=end+1) {
        auto line=log.substr(begin,end-begin);
        while (!line.empty() && (line.front()==' ' || line.front()=='\t')) line.remove_prefix(1);
        if (!line.empty() && line.back()=='\r') line.remove_suffix(1);
        if (line.starts_with(prefix)) found=line.substr(prefix.size());
    }
    return found;
}
inline bool HasLine(std::string_view log, std::string_view prefix) {
    return LastLine(log,prefix).data()!=nullptr;
}
inline std::string_view Field(std::string_view line, std::string_view name) {
    while (!line.empty()) {
        const auto end=line.find(' ');
        const auto token=line.substr(0,end);
        if (token.starts_with(name) && token.size()>name.size() && token[name.size()]=='=')
            return token.substr(name.size()+1);
        if (end==line.npos) break;
        line.remove_prefix(end+1);
    }
    return {};
}
inline uint32_t Number(std::string_view value, uint32_t fallback=0) {
    if (value.empty()) return fallback;
    uint32_t result=0;
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
    return parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size() ? result : fallback;
}
struct Result {
    Code code=Code::None;
    std::string stage, detail;
    uint32_t exitCode=0, reason=0;
};
inline Result Parse(std::string_view log) {
    const auto line=LastLine(log,"[SESSION_RESULT] ");
    Result result;
    const auto code=Field(line,"code"), stage=Field(line,"stage");
    if (stage.empty()) return result;
    for (unsigned i=1;i<=unsigned(Code::ConnectionFailure);++i)
        if (code==Name(Code(i))) result.code=Code(i);
    result.stage=stage;result.exitCode=Number(Field(line,"exit"));result.reason=Number(Field(line,"reason"));
    const auto detail=line.find(" detail=");
    if (detail!=line.npos) result.detail=line.substr(detail+8);
    return result;
}
inline void Write(std::ostream& output, Code code, std::string_view stage,
                  std::string_view detail={}, uint32_t reason=0, uint32_t exitCode=0) {
    std::string safe(detail);
    for (char& c:safe) if (static_cast<unsigned char>(c)<0x20) c=' ';
    output<<"[SESSION_RESULT] code="<<Name(code)<<" stage="<<stage
          <<" exit="<<exitCode<<" reason="<<reason<<" detail="<<safe<<'\n'<<std::flush;
}
}
