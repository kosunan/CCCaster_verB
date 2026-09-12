#include "core_dll/hook/ScenePairMerge.hpp"
#include "test_support.hpp"
void HookLog(const char *) {}
int main() {
    using namespace cccaster::game_interface::scene_pair_merge;
    auto *a = reinterpret_cast<IDirect3DDevice9 *>(uintptr_t{1});
    auto *b = reinterpret_cast<IDirect3DDevice9 *>(uintptr_t{2});
    CC_CASE("未照合・未開始のシーンは省略しない");
    Reset();
    CC_CHECK(!DeferEnd(a, 100));
    endCaller = 100;
    beginCaller = 200;
    CC_CHECK(!DeferEnd(a, 100));
    CC_CASE("呼出元とデバイスが同じ連続対だけをまとめる");
    active = a;
    CC_CHECK(!DeferEnd(a, 101));
    CC_CHECK(!DeferEnd(b, 100));
    CC_CHECK(DeferEnd(a, 100));
    CC_CHECK(!DeferEnd(a, 100));
    CC_CHECK(!ConsumeBegin(b, 200));
    CC_CHECK(!ConsumeBegin(a, 201));
    CC_CHECK(pending == a);
    CC_CHECK(ConsumeBegin(a, 200));
    CC_CHECK(active == a && pending == nullptr && merged == 1);
    CC_CHECK(!ConsumeBegin(a, 200));
    CC_CASE("描画直後の対と直前の対を取り違えない");
    endAfterDraw = 300;
    beginAfterDraw = 400;
    CC_CHECK(DeferEnd(a, 300));
    CC_CHECK(!ConsumeBegin(a, 200));
    CC_CHECK(ConsumeBegin(a, 400));
    CC_CHECK(active == a && pending == nullptr && merged == 2);
    CC_CASE("Resetと実End後に開始状態を持ち越さない");
    CC_CHECK(DeferEnd(a, 100));
    Reset();
    CC_CHECK(!ConsumeBegin(a, 200));
    CC_CHECK(!DeferEnd(a, 100));
    return cccaster::test::Summarize("scene_pair_merge");
}
