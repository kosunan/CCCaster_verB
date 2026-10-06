#include "AppContext.hpp"
#include "LauncherModel.hpp"
#include "RmlHost.hpp"
#include "ProductVersion.hpp"
#include "shared_contracts/ResourceIds.h"
#include "shared_contracts/NativePath.hpp"
#include "p2p/MatchingCleanup.hpp"
#include <shellapi.h>
#include <deque>

namespace {
using namespace cccaster::gui;
struct Application {
    LauncherModel model;
    RmlHost display;
    std::deque<Json> pending;
    bool includeLog=false, handling=false;
    bool testMode=false;
    UINT interval=0;
    explicit Application(HWND window,const std::filesystem::path& testDirectory) : display(window,[this,window](Json message){
        if(pending.size()<128){pending.push_back(std::move(message));PostMessageW(window,UiCommandMessage,0,0);}
    },testDirectory),testMode(!testDirectory.empty()) {}
    void Tick() {
        if(handling)return;
        handling=true;
        try {
            model.Poll();
            while(!pending.empty()) {
                auto command=std::move(pending.front());pending.pop_front();
                if(command.value("type",std::string{})=="view") {
                    if(command.contains("log") && command["log"].is_boolean()) includeLog=command["log"].get<bool>();
                } else model.Command(command);
            }
            const bool game=model.GameRunning();
            const bool visible=!IsIconic(guiWindow) && (!game || GetForegroundWindow()==guiWindow);
            display.Visibility(visible);
            const UINT desired=model.TrainingStandbyRunning()?100:game?1000:(visible?250:500);
            if(interval!=desired){interval=desired;SetTimer(guiWindow,1,interval,nullptr);}
            if(display.Ready() && (visible || testMode)) {
                auto state=model.State(includeLog);state["display"]={{"software",display.Software()},{"throttled",game}};
                display.Send(state);
            }
        } catch(const std::exception&) { /* 不正なUIメッセージでイベントループを終了しない。 */ }
        handling=false;
    }
};
LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    auto app=reinterpret_cast<Application*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(app && app->model.ControllerKey(message,w,l)){app->Tick();return 0;}
    if(app && app->display.Message(message,w,l))return 0;
    switch(message) {
    case WM_SIZE: if(app){app->display.Resize();app->Tick();}return 0;
    case WM_ACTIVATE: if(app)app->Tick();break;
    case WM_TIMER: if(app){if(w==2){app->model.PollController();app->display.Frame();}else app->Tick();}return 0;
    case UiCommandMessage: if(app)app->Tick();return 0;
    case DisplayErrorMessage: if(app)app->display.ShowError();return 0;
    case WM_COMMAND:
        if(LOWORD(w)==1001 || LOWORD(w)==1002){PostMessageW(window,ReloadDisplayMessage,LOWORD(w)==1002,0);return 0;}break;
    case ReloadDisplayMessage:
        if(app){app->display.Start(w!=0);CheckMenuRadioItem(GetMenu(window),1001,1002,w?1002:1001,MF_BYCOMMAND);}return 0;
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize={620,520};return 0;
    case WM_DPICHANGED: {
        const auto bounds=reinterpret_cast<RECT*>(l);
        SetWindowPos(window,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;
    }
    case WM_DESTROY: KillTimer(window,1);KillTimer(window,2);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(window,message,w,l);
}
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE,LPSTR,int) {
    using cccaster::main_app::ConfigManager;
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);exePath=path;
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (argv && (argc==4 || argc==5) && wcscmp(argv[1],L"--matching-cleanup")==0) {
        int result=1;
        try { result=cccaster::matching::RunCleanupWorker(std::stoull(argv[2]),std::stoull(argv[3]),argc==5?std::stoull(argv[4]):0); }
        catch (const std::exception&) {}
        LocalFree(argv);return result;
    }
    const auto config=cccaster::ConfigPath(exePath.parent_path());
    if(std::filesystem::exists(config))ConfigManager::Load(cccaster::PathUtf8(config));
    if(argv && argc>1 && wcscmp(argv[1],L"--worker")==0)return RunWorker(argc,argv);
    bool software=ConfigManager::GetInt("GUI","SoftwareRendering",0)!=0;
    std::filesystem::path testDirectory;
    for(int i=1;argv && i<argc;++i) {
        if(wcscmp(argv[i],L"--software-rendering")==0)software=true;
        if(wcscmp(argv[i],L"--ui-test-dir")==0 && i+1<argc) {
            testDirectory=std::filesystem::absolute(argv[++i]);
            if(!std::filesystem::is_directory(testDirectory))return 2;
        }
    }
    if(argv)LocalFree(argv);
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(com))return 1;
    WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.lpfnWndProc=WindowProc;wc.hInstance=instance;
    wc.hIcon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(CCCASTER_APP_ICON),IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),LR_DEFAULTCOLOR));
    wc.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(CCCASTER_APP_ICON),IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_DEFAULTCOLOR));
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName=L"CCCasterRmlGui";RegisterClassExW(&wc);
    const auto menu=CreateMenu(),render=CreatePopupMenu();
    AppendMenuW(render,MF_STRING,1001,L"自動描画で再読み込み");AppendMenuW(render,MF_STRING,1002,L"CPU描画で再読み込み（F8）");
    AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(render),L"表示 / Display");
    guiWindow=CreateWindowExW(0,wc.lpszClassName,CCCASTER_PRODUCT_TITLE_W,WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,1060,780,nullptr,menu,instance,nullptr);
    if(!guiWindow){CoUninitialize();return 1;}
    int exitCode=0;
    try {
        Application app(guiWindow,testDirectory);
        SetWindowLongPtrW(guiWindow,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&app));
        ShowWindow(guiWindow,SW_SHOWDEFAULT);UpdateWindow(guiWindow);
        app.display.Start(software);CheckMenuRadioItem(menu,1001,1002,software?1002:1001,MF_BYCOMMAND);
        SetTimer(guiWindow,1,250,nullptr);
        SetTimer(guiWindow,2,33,nullptr);
        ACCEL entry{FVIRTKEY,VK_F8,1002};
        const auto shortcuts=CreateAcceleratorTableW(&entry,1);
        MSG message{};int result;
        while((result=GetMessageW(&message,nullptr,0,0))>0) {
            if(!TranslateAcceleratorW(guiWindow,shortcuts,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
        }
        DestroyAcceleratorTable(shortcuts);
        if(result<0)exitCode=1;
        SetWindowLongPtrW(guiWindow,GWLP_USERDATA,0);
    } catch(const std::exception&) {
        MessageBoxW(guiWindow,L"GUIの初期化に失敗しました。配置先とユーザーフォルダーの書込み権限を確認してください。",L"CCCaster",MB_OK|MB_ICONERROR);
        exitCode=1;
    }
    if(IsWindow(guiWindow))DestroyWindow(guiWindow);
    guiWindow=nullptr;UnregisterClassW(wc.lpszClassName,instance);
    if(wc.hIcon)DestroyIcon(wc.hIcon);
    if(wc.hIconSm)DestroyIcon(wc.hIconSm);
    CoUninitialize();return exitCode;
}
