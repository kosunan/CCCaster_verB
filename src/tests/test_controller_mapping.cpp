#include "core_dll/ui/ControllerMappingValidation.hpp"
#include "core_dll/engine/TrainingState.hpp"
#include "test_support.hpp"
#include <cstring>
#include <fstream>
#include <filesystem>
#ifdef _WIN32
#include "core_dll/ui/Controller_Ui_Logic.hpp"
#include "core_dll/common/DataPaths.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include <imgui.h>
#endif
using namespace cccaster::input;
using namespace cccaster::domain::ui;
using namespace cccaster::domain::session;

static std::vector<cccaster::game_interface::JoyDeviceInfo> devices;
static cccaster::game_interface::JoyDeviceInfo Device(int id, const char *name, const char *guid) {
    cccaster::game_interface::JoyDeviceInfo d{};
    d.id = id; std::strncpy(d.name, name, sizeof(d.name)-1); std::strncpy(d.instanceGuid, guid, sizeof(d.instanceGuid)-1);
    return d;
}
struct Memory : cccaster::game_interface::IGameMemory {
    int value = 0, saves = 0, loads = 0;
    bool saveOK = true, loadOK = true;
    bool IsAvailable() const override { return true; }
    uint32_t GameMode() const override { return 20; }
    uint8_t IntroState() const override { return 0; }
    uint32_t WorldTimer() const override { return 0; }
    uint32_t RealTimer() const override { return 0; }
    uint32_t MenuStateCounter() const override { return 0; }
    void WriteInput(cccaster::game_interface::GameInput, cccaster::game_interface::GameInput) override {}
    bool SupportsSnapshots() const override { return true; }
    size_t SnapshotSize() const override { return sizeof(value); }
    bool SaveSnapshot(std::span<char> out) override { ++saves; std::memcpy(out.data(), &value, sizeof(value)); return saveOK; }
    bool LoadSnapshot(std::span<char> in) override { ++loads; if (loadOK) std::memcpy(&value, in.data(), sizeof(value)); return loadOK; }
};
#ifdef _WIN32
static std::string edge, held;
static int reloads = 0;
namespace cccaster::game_interface {
std::vector<JoyDeviceInfo> DirectInputHook::GetConnectedDevices() { return devices; }
std::string DirectInputHook::GetAnyInputEdge(int id) { return id >= 0 ? edge : ""; }
bool DirectInputHook::IsBindingPressed(int, const std::string &bind) { return held == bind; }
void DirectInputHook::ReloadConfigs() { ++reloads; }
}
static std::string Read(const std::string &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
static void Frame(const std::string &input = "", const std::string &down = "") {
    edge = input; held = down;
    ImGui::GetIO().DeltaTime = 0.25f;
    ImGui::NewFrame(); ControllerUiLogic::Update(); ImGui::Render();
}
static void Bind(int index, const std::string &value) {
    ControllerUiLogic::StartBinding(0, index); Frame(); Frame(value); Frame();
}
static void TestUi() {
    using L = ControllerUiLogic;
    using cccaster::main_app::Config;
    using cccaster::main_app::ConfigManager;
    const auto root = std::filesystem::temp_directory_path() / ("cccaster_mapping_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(root);
    cccaster::core::paths::SetDataRoot(root.string());
    const auto mainPath = (root / "cccaster_v10.ini").string();
    const auto legacyPath = (root / "Pad.ini").string();
    Config main, profile;
    main.SetString("Settings", "P1Device", "Pad");
    main.SetString("Settings", "Unrelated", "keep"); main.Save(mainPath);
    const auto defaults = DefaultBindings(false);
    for (int i=0;i<BindingCount;++i) profile.SetString("Mapping", BindingKeys[i], defaults[i]);
    profile.SetString("Other", "UserValue", "keep"); profile.Save(legacyPath);
    ConfigManager::Load(mainPath);
    devices = {Device(0, "Pad", "AAA")};
    ImGui::CreateContext();
    auto &io = ImGui::GetIO(); io.DisplaySize = ImVec2(640,480); io.IniFilename = nullptr;
    unsigned char *pixels; int w,h; io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    CC_CASE("open-close and button testing do not write or begin mapping");
    const auto before = Read(mainPath), legacyBefore = Read(legacyPath);
    L::BeginUiSession(); CC_CHECK(L::Binds(0)==defaults); Frame("B0");
    CC_CHECK_EQ(L::CapturePlayer(), -1); CC_CHECK(!L::Dirty());
    CC_CHECK(L::EndUiSession()); CC_CHECK(Read(mainPath)==before); CC_CHECK(Read(legacyPath)==legacyBefore);
    CC_CHECK_EQ(reloads,0);
    CC_CASE("invalid duplicate blocks closing and preserves draft and disk");
    L::BeginUiSession(); Bind(5,"B0"); CC_CHECK(!L::EndUiSession());
    CC_CHECK(L::StatusError()); CC_CHECK(L::Dirty()); CC_CHECK(Read(mainPath)==before);
    CC_CHECK(Read(legacyPath)==legacyBefore); CC_CHECK(L::Binds(0)[5]=="B0");
    CC_CASE("individual fix, optional clear and training duplicates persist to GUID file");
    Bind(5,"B1"); Bind(13,"B0"); Bind(14,"B0"); L::ClearBinding(0,10);
    CC_CHECK(L::EndUiSession()); CC_CHECK_EQ(reloads,1);
    const auto uniquePath = (root / "Pad__AAA.ini").string();
    Config check; check.Load(uniquePath);
    CC_CHECK(check.GetString("Mapping","FN1","MISSING").empty());
    CC_CHECK(check.GetString("Mapping","TrainingSave")=="B0");
    CC_CHECK(check.GetString("Mapping","TrainingLoad")=="B0");
    CC_CHECK(check.GetString("Other","UserValue")=="keep");
    CC_CHECK(Read(legacyPath)==legacyBefore); CC_CHECK(ConfigManager::GetString("Settings","P1DeviceGuid")=="AAA");
    CC_CASE("defaults and revert are not automatic writes");
    L::BeginUiSession(); const auto saved = Read(uniquePath);
    L::ResetPlayerToDefaults(0); CC_CHECK(L::Binds(0)[13].empty()); CC_CHECK(Read(uniquePath)==saved);
    L::Revert(); CC_CHECK(L::Binds(0)[13]=="B0"); CC_CHECK(!L::Dirty());
    CC_CASE("map all retains current values, release gate and skip/back work");
    L::StartMappingForPlayer(0); Frame(); Frame("H0_8","H0_8");
    CC_CHECK_EQ(L::CaptureBinding(),1); Frame("H0_8","H0_8"); CC_CHECK_EQ(L::CaptureBinding(),1);
    L::SkipCapture(); CC_CHECK_EQ(L::CaptureBinding(),2); L::PreviousBinding(); CC_CHECK_EQ(L::CaptureBinding(),1);
    L::CancelCapture(); CC_CHECK(L::Binds(0)[1]=="H0_2"); L::Revert();
    CC_CASE("disconnect/reorder never captures from another identical controller");
    Bind(13,"B3"); L::StartBinding(0,14); devices = {Device(0,"Pad","BBB")}; Frame("B5");
    CC_CHECK_EQ(L::DeviceId(0),-1); CC_CHECK_EQ(L::CapturePlayer(),-1); CC_CHECK(L::Binds(0)[14]=="B0");
    CC_CHECK(!L::EndUiSession()); L::Suspend(); L::BeginUiSession(); CC_CHECK(L::Binds(0)[13]=="B3");
    devices = {Device(0,"Pad","BBB"),Device(1,"Pad","AAA")}; CC_CHECK_EQ(L::DeviceId(0),1);
    CC_CHECK(L::EndUiSession());
    CC_CASE("write failures are visible and retain unsaved data");
    L::BeginUiSession(); Bind(13,"B6");
    cccaster::core::paths::SetDataRoot((root / "missing").string());
    CC_CHECK(!L::EndUiSession()); CC_CHECK(L::Dirty()); CC_CHECK(L::StatusError());
    cccaster::core::paths::SetDataRoot(root.string()); L::Revert(); CC_CHECK(L::EndUiSession());
    CC_CASE("keyboard defaults use keyboard inputs");
    L::BeginUiSession(); CC_CHECK(L::SelectDevice(0,-2)); L::ResetPlayerToDefaults(0);
    CC_CHECK(L::Binds(0)==DefaultBindings(true)); CC_CHECK(!L::AnalogDirections(0)); CC_CHECK(L::EndUiSession());
    ImGui::DestroyContext();
    std::printf("Fixtures: %s\n",root.string().c_str());
}
#endif

int main() {
    CC_CASE("required controls and duplicate validation");
    auto binds = DefaultBindings(false); CC_CHECK(ValidateControllerMapping(binds).valid);
    binds[0].clear(); CC_CHECK(!ValidateControllerMapping(binds).valid);
    binds = DefaultBindings(false); binds[1]=binds[0]; CC_CHECK(!ValidateControllerMapping(binds).valid);
    binds.fill("B1"); CC_CHECK(!ValidateControllerMapping(binds).valid);
    binds = DefaultBindings(false); binds[10].clear(); binds[11].clear(); binds[12].clear();
    CC_CHECK(ValidateControllerMapping(binds).valid);
    binds[13]=binds[14]=binds[4]; CC_CHECK(ValidateControllerMapping(binds).valid);
    CC_CASE("identity strict GUID and legacy unique-name migration");
    devices = {Device(0,"Pad","BBB"),Device(1,"Pad","AAA")};
    CC_CHECK_EQ(ResolveDevice({"Pad","AAA"},devices),1);
    CC_CHECK_EQ(ResolveDevice({"Pad",""},devices),-1);
    devices.pop_back(); CC_CHECK_EQ(ResolveDevice({"Pad","AAA"},devices),-1);
    CC_CHECK_EQ(ResolveDevice({"Pad",""},devices),0);
    CC_CASE("training-only commands, hold, overlap, failures and lifecycle");
    Memory mem; TrainingState state;
    cccaster::TrainingFrameSample sample; sample.valid=true; sample.round=1;
    int64_t time=100;
    auto step=[&](int buttons,int mode=1,bool battle=true,bool configuring=false) {
        return state.Step(mode,battle,configuring,buttons,sample,mem,++time);
    };
    for(int mode : {0,2,3,4,255}) { step(0,mode); step(1,mode); step(2,mode); }
    CC_CHECK_EQ(mem.saves,0); CC_CHECK_EQ(mem.loads,0);
    step(0); CC_CHECK(step(2)==TrainingStateEvent::Empty);
    step(0); mem.value=42; CC_CHECK(step(1)==TrainingStateEvent::Saved);
    step(1); CC_CHECK_EQ(mem.saves,1); mem.value=100;
    step(0); CC_CHECK(step(3)==TrainingStateEvent::Loaded); CC_CHECK_EQ(mem.value,42); CC_CHECK_EQ(mem.saves,1);
    step(3); CC_CHECK_EQ(mem.loads,1);
    step(0); mem.saveOK=false; mem.value=15; CC_CHECK(step(1)==TrainingStateEvent::SaveFailed);
    CC_CHECK(state.HasState()); step(0); CC_CHECK(step(2)==TrainingStateEvent::Loaded); CC_CHECK_EQ(mem.value,42);
    step(0,1,true,true); step(1,1,true,true); step(1); CC_CHECK_EQ(mem.saves,2);
    mem.saveOK=true; step(0); step(1); CC_CHECK_EQ(mem.saves,3);
    step(0); mem.loadOK=false; CC_CHECK(step(2)==TrainingStateEvent::LoadFailed); CC_CHECK(!state.HasState());
    mem.loadOK=true; step(0); step(1); sample.round++; step(0); CC_CHECK(!state.HasState());
    step(1); CC_CHECK(state.HasState()); step(0,1,false); CC_CHECK(!state.HasState());
    step(1); CC_CHECK(!state.HasState()); step(0); step(1); CC_CHECK(state.HasState());
    CC_CHECK(state.Notice(time+4000001)==TrainingStateEvent::None);
#ifdef _WIN32
    TestUi();
#endif
    return cccaster::test::Summarize("controller mapping and training state");
}
