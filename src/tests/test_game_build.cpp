#include "shared_contracts/GameBuild.hpp"
#include "shared_contracts/GameImageAddress.hpp"
#include "test_support.hpp"
#ifdef _WIN32
#include "launcher/GameFileHash.hpp"
#endif
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
#ifdef _WIN32
    CC_CASE("全体SHA-256の標準ベクトルを確認する");
    std::string digest;
    CC_CHECK(FileSha256({}, digest));
    CC_CHECK(digest == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const uint8_t abc[] = {'a', 'b', 'c'};
    CC_CHECK(FileSha256(abc, digest));
    CC_CHECK(digest == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
#endif
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
    CC_CHECK(SupportsRuntime(Edition::Carnival140Community));
    CC_CHECK(!SupportsRuntime(Edition::Steam20170105)); CC_CHECK(!SupportsRuntime(Edition::Unknown));
    CC_CASE("コミュニティ版のヘッダーと追加セクションを確認する");
    auto community = old;
    Put16(community, 0x106, 2);
    Put32(community, 0x118 + 56, 0x3b5000);
    const size_t extra = 0x118 + 224 + 40;
    const char extraName[] = ".new00";
    for (size_t i = 0; i < sizeof(extraName); ++i) community[extra+i] = extraName[i];
    Put32(community, extra+8, 4096); Put32(community, extra+12, 0x3b4000);
    Put32(community, extra+16, 4096); Put32(community, extra+20, 0x156000);
    CC_CHECK(ReadHeaders(community, p));
    CC_CHECK(IdentifyHeaders(p) == Edition::Carnival140Community);
    Put32(community, extra+12, 0x3b3000);
    CC_CHECK(ReadHeaders(community, p));
    CC_CHECK(IdentifyHeaders(p) == Edition::Unknown);
    Put32(community, extra+12, 0x3b5000);
    CC_CHECK(!ReadHeaders(community, p));
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
    CC_CASE("起動拒否の理由を分離してもヘッダー一致だけでは許可しない");
    CC_CHECK(InspectFile({}).issue == FileIssue::InvalidPe);
    CC_CHECK(InspectFile(old).issue == FileIssue::TruncatedText);
    auto diagnostic = InspectFile(bad);
    CC_CHECK(diagnostic.issue == FileIssue::CodeMismatch);
    CC_CHECK(diagnostic.headerEdition == Edition::Carnival140);
    CC_CHECK(diagnostic.edition == Edition::Unknown);
    CC_CHECK(!SupportsRuntime(diagnostic.edition));
    CC_CHECK(diagnostic.codeHash != diagnostic.expectedCodeHash);
    Put32(bad, 0x118 + 28, 0x320000);
    CC_CHECK(InspectFile(bad).issue == FileIssue::ImageBaseMismatch);
    CC_CHECK(IdentifyFile(bad) == Edition::Unknown);
    Put32(bad, 0x118 + 28, 0x400000);
    Put32(bad, 0x108, 0);
    CC_CHECK(InspectFile(bad).issue == FileIssue::UnknownBuild);
    CC_CHECK(IdentifyFile(bad) == Edition::Unknown);
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
        diagnostic = InspectFile(bytes);
        CC_CHECK(diagnostic.issue == FileIssue::None);
        CC_CHECK(diagnostic.edition == edition);
        CC_CHECK_EQ(diagnostic.codeHash, diagnostic.expectedCodeHash);
        CC_CHECK(ReadHeaders(bytes, p));
#ifdef _WIN32
        CC_CHECK(FileSha256(bytes, digest));
        CC_CHECK(digest == ExpectedFileSha256(edition));
        // .text以外の変更も全体SHA-256で拒否できる。元EXEは変更しない。
        auto nonCode = bytes;
        nonCode[2] ^= 1;
        CC_CHECK(IdentifyFile(nonCode) == edition);
        CC_CHECK(FileSha256(nonCode, digest));
        CC_CHECK(digest != ExpectedFileSha256(edition));
#endif
        if (edition == Edition::Carnival140Community) {
            auto extension = bytes;
            extension[p.extraOffset + 16] ^= 1;
            CC_CHECK(InspectFile(extension).issue == FileIssue::ExtraSectionMismatch);
            CC_CHECK(IdentifyFile(extension) == Edition::Unknown);
            extension.resize(p.extraOffset + p.extraSize - 1);
            CC_CHECK(InspectFile(extension).issue == FileIssue::ExtraSectionMismatch);
        }
        bytes[p.textOffset + p.textSize / 2] ^= 1;
        CC_CHECK(IdentifyFile(bytes) == Edition::Unknown);
        CC_CHECK(InspectFile(bytes).issue == FileIssue::CodeMismatch);
        bytes.resize(p.textOffset + p.textSize - 1);
        CC_CHECK(IdentifyFile(bytes) == Edition::Unknown);
        CC_CHECK(InspectFile(bytes).issue == FileIssue::TruncatedText);
    }
    return cccaster::test::Summarize("game_build");
}
