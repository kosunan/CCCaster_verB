// 版が既知のEXEをCREATE_SUSPENDEDし、実ロード先を読むだけの検証。
// ゲームの命令実行・DLL注入・ゲームメモリ書込みは行わない。
#include "launcher/RemoteGameImage.hpp"
#include <fstream>
#include <iostream>
#include <vector>
int main(int argc, char **argv) {
    using namespace cccaster::game_build;
    if (argc != 2) { std::cerr << "usage: probe_suspended_image MBAA.exe\n"; return 2; }
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (!file || size <= 0 || size > 64 * 1024 * 1024) return 2;
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(data.data()), data.size())) return 2;
    const auto edition = IdentifyFile(data);
    if (edition == Edition::Unknown) return 2;
    STARTUPINFOA si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::string path = argv[1], command = "\"" + path + "\"";
    const auto slash = path.find_last_of("/\\");
    const std::string directory = slash == std::string::npos ? "." : path.substr(0, slash);
    if (!CreateProcessA(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, directory.c_str(), &si, &pi)) return 3;
    LoadedImage image;
    PeIdentity identity;
    const bool ok = ReadSuspendedImage(pi.hProcess, pi.hThread, image, identity) &&
                    IdentifyHeaders(identity) == edition;
    std::cout << "edition=" << Name(edition) << " pid=" << pi.dwProcessId << " base=" << std::hex
              << image.base << " entry=" << image.Resolve(identity.entryRva, 2)
              << " mode=" << (edition == Edition::Steam20170105 ? image.Resolve(0x1b391c, 4) : image.Resolve(0x14eee8, 4))
              << std::dec << " verified=" << ok << "\n";
    const bool terminated = TerminateProcess(pi.hProcess, 0) && WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return ok && terminated ? 0 : 1;
}
