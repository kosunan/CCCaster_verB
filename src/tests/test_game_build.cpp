#include "shared_contracts/GameBuild.hpp"
#include "shared_contracts/GameImageAddress.hpp"
#include "test_support.hpp"
#include <fstream>
#include <iostream>
#include <vector>
using namespace cccaster::game_build;

static void Put16(std::vector<uint8_t> &b, size_t p, uint16_t v) {
    b[p] = v; b[p + 1] = v >> 8;
}
static void Put32(std::vector<uint8_t> &b, size_t p, uint32_t v) {
    Put16(b, p, v); Put16(b, p + 2, v >> 16);
}
static std::vector<uint8_t> Headers(bool steam) {
    std::vector<uint8_t> b(4096);
    Put16(b, 0, 0x5a4d); Put32(b, 0x3c, 0x100);
    Put32(b, 0x100, 0x4550); Put16(b, 0x104, 0x14c); Put16(b, 0x106, 1);
    Put32(b, 0x108, steam ? 0x586e1804 : 0x4fe44444); Put16(b, 0x114, 224);
    const size_t opt = 0x118, sec = opt + 224;
    Put16(b, opt, 0x10b); Put32(b, opt + 16, steam ? 0x130982 : 0xe3d7e);
    Put32(b, opt + 28, 0x400000); Put32(b, opt + 56, steam ? 0xf3e000 : 0x3b4000);
    const char name[] = ".text";
    for (size_t i = 0; i < sizeof(name); ++i) b[sec + i] = name[i];
    Put32(b, sec + 8, steam ? 0x1695eb : 0x11918f); Put32(b, sec + 12, 0x1000);
    Put32(b, sec + 16, steam ? 0x169600 : 0x11a000); Put32(b, sec + 20, 4096);
    return b;
}
int main(int argc, char **argv) {
    PeIdentity p;
    CC_CASE("起動ごとの基準位置からRVAを解決し範囲外を拒否する");
    const LoadedImage a{0x400000, 0xf3e000}, b{0x320000, 0xf3e000};
    CC_CHECK_EQ(a.Resolve(0x1b391c, 4), uintptr_t(0x5b391c));
    CC_CHECK_EQ(b.Resolve(0x1b391c, 4), uintptr_t(0x4d391c));
    CC_CHECK_EQ(b.Resolve(0xf3dfff, 2), uintptr_t(0));
    CC_CHECK_EQ(b.Resolve(0xffffffff, 1), uintptr_t(0));
    CC_CHECK_EQ(b.Resolve(0, 0), uintptr_t(0));
    const LoadedImage overflow{(std::numeric_limits<uintptr_t>::max)() - 1, 16};
    CC_CHECK_EQ(overflow.Resolve(1, 2), uintptr_t(0));
    CC_CASE("版識別と対応可否を分離する");
    auto old = Headers(false), steam = Headers(true);
    CC_CHECK(ReadHeaders(old, p)); CC_CHECK(IdentifyHeaders(p) == Edition::Carnival140);
    CC_CHECK(ReadHeaders(steam, p)); CC_CHECK(IdentifyHeaders(p) == Edition::Steam20170105);
    p.imageBase = 0x320000;
    CC_CHECK(IdentifyHeaders(p) == Edition::Steam20170105);
    CC_CHECK(SupportsRuntime(Edition::Carnival140));
    CC_CHECK(!SupportsRuntime(Edition::Steam20170105)); CC_CHECK(!SupportsRuntime(Edition::Unknown));
    CC_CASE("切れたヘッダー・不正オフセット・PE32+・巨大セクションを拒否する");
    for (size_t n = 0; n < 0x220; ++n)
        CC_CHECK(!ReadHeaders(std::span(old).first(n), p));
    auto bad = old; Put32(bad, 0x3c, 0xfffffff0); CC_CHECK(!ReadHeaders(bad, p));
    bad = old; Put16(bad, 0x118, 0x20b); CC_CHECK(!ReadHeaders(bad, p));
    bad = old; Put16(bad, 0x106, 0xffff); CC_CHECK(!ReadHeaders(bad, p));
    bad = old; Put32(bad, 0x1f8 + 12, 0xfffffff0); CC_CHECK(!ReadHeaders(bad, p));
    CC_CASE("メタデータが同じでもコード欠落・改変を受け入れない");
    CC_CHECK(IdentifyFile(old) == Edition::Unknown);
    bad = old; bad.resize(4096 + 0x11a000); CC_CHECK(IdentifyFile(bad) == Edition::Unknown);
    // 実ファイルは利用可能なときだけ引数で与える。ゲームを起動しない。
    for (int i = 1; i < argc; ++i) {
        std::ifstream file(argv[i], std::ios::binary | std::ios::ate);
        CC_CHECK(bool(file));
        if (!file) continue;
        std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
        file.seekg(0); file.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
        const auto edition = IdentifyFile(bytes);
        std::cout << argv[i] << ": " << Name(edition) << "\n";
        CC_CHECK(edition != Edition::Unknown);
        CC_CHECK(ReadHeaders(bytes, p));
        bytes[p.textOffset + p.textSize / 2] ^= 1;
        CC_CHECK(IdentifyFile(bytes) == Edition::Unknown);
        bytes.resize(p.textOffset + p.textSize - 1);
        CC_CHECK(IdentifyFile(bytes) == Edition::Unknown);
    }
    return cccaster::test::Summarize("game_build");
}
