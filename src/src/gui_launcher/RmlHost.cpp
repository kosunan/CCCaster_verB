#include "RmlHost.hpp"
#include "RmlView.hpp"
#include "EmblemPresets.hpp"
#include "RmlUi_Platform_Win32.h"
#include "RmlUi_Renderer_DX11.h"
#include "p2p/Crypto.hpp"
#include <fstream>
#include <map>
#include <span>
#include <vector>

namespace cccaster::gui {
namespace {
template<class T> struct Com {
    T* p=nullptr;
    ~Com(){Reset();}
    void Reset(){if(p){p->Release();p=nullptr;}}
    T** Put(){Reset();return &p;}
    T* operator->()const{return p;}
};
void Check(HRESULT hr,const char* operation) {if(FAILED(hr))throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(uint32_t(hr)));}
std::span<const uint8_t> Resource(int id) {
    auto module=GetModuleHandleW(nullptr);auto resource=FindResourceW(module,MAKEINTRESOURCEW(id),RT_RCDATA);
    if(!resource)throw std::runtime_error("Missing embedded GUI resource");
    auto memory=LoadResource(module,resource);
    return {static_cast<const uint8_t*>(LockResource(memory)),SizeofResource(module,resource)};
}
class Files final:public Rml::FileInterface {
    struct File {std::span<const uint8_t> data;size_t position=0;};
public:
    Rml::FileHandle Open(const Rml::String& path)override {
        const int id=path=="launcher.rml"?201:path=="launcher.rcss"?202:0;
        return id?reinterpret_cast<Rml::FileHandle>(new File{Resource(id)}):0;
    }
    void Close(Rml::FileHandle handle)override {delete reinterpret_cast<File*>(handle);}
    size_t Read(void* destination,size_t size,Rml::FileHandle handle)override {
        auto& file=*reinterpret_cast<File*>(handle);size=std::min(size,file.data.size()-file.position);
        std::memcpy(destination,file.data.data()+file.position,size);file.position+=size;return size;
    }
    bool Seek(Rml::FileHandle handle,long offset,int origin)override {
        auto& file=*reinterpret_cast<File*>(handle);
        if(origin!=SEEK_SET&&origin!=SEEK_CUR&&origin!=SEEK_END)return false;
        auto next=int64_t(origin==SEEK_SET?0:origin==SEEK_CUR?file.position:file.data.size())+offset;
        if(next<0||uint64_t(next)>file.data.size())return false;
        file.position=size_t(next);return true;
    }
    size_t Tell(Rml::FileHandle handle)override{return reinterpret_cast<File*>(handle)->position;}
};
class System final:public SystemInterface_Win32 {
    std::ofstream log_;
public:
    System() {
        wchar_t temp[32768]{};GetTempPathW(32768,temp);
        log_.open(std::filesystem::path(temp)/(L"CCCaster_B_RmlGui_"+std::to_wstring(GetCurrentProcessId())+L".log"),std::ios::app);
    }
    bool LogMessage(Rml::Log::Type type,const Rml::String& message)override {
        log_<<GetTickCount64()<<" ["<<int(type)<<"] "<<message<<"\n";log_.flush();return true;
    }
    void JoinPath(Rml::String& result,const Rml::String&,const Rml::String& path)override{result=path;}
};
class Renderer final:public RenderInterface_DX11 {
public:
    std::vector<uint8_t> emblem;
    explicit Renderer(ID3D11Device* device):RenderInterface_DX11(device){}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& size,const Rml::String& source)override {
        if(source.rfind("emblem:",0)==0 || source.rfind("preset:",0)==0) {
            if(source=="emblem:0") {
                size={1,1};const uint8_t transparent[4]={};
                return GenerateTexture({transparent,4},size);
            }
            emblem::Image preset;
            const uint8_t* pixels = nullptr;
            if (source.rfind("preset:",0)==0) {
                if (!emblem::ImportPreset(source.substr(7), preset)) return 0;
                pixels = preset.pixels.data();
            } else {
                if(emblem.size()!=emblem::Bytes)return 0;
                pixels = emblem.data();
            }
            // ピクセル境界を保つ整数倍のプレビュー（96×48）。
            constexpr unsigned zoom = 4, width = emblem::Width * zoom, height = emblem::Height * zoom;
            size={width,height};std::vector<uint8_t> rgba(width * height * 4);
            for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
                const auto src=((y/zoom)*emblem::Width+x/zoom)*4, dst=(y*width+x)*4;
                for(unsigned c=0;c<3;++c)rgba[dst+c]=uint8_t(unsigned(pixels[src+2-c])*pixels[src+3]/255);
                rgba[dst+3]=pixels[src+3];
            }
            return GenerateTexture({rgba.data(),rgba.size()},size);
        }
        return 0;
    }
};
}
struct RmlHost::Impl {
    HWND window;
    std::function<void(Json)> receive;
    std::filesystem::path testDirectory;
    System system;Files files;TextInputMethodEditor_Win32 ime;
    Com<ID3D11Device> device;Com<ID3D11DeviceContext> deviceContext;
    Com<IDXGISwapChain> swap;Com<ID3D11RenderTargetView> target;
    std::unique_ptr<Renderer> renderer;
    Rml::Context* context=nullptr;std::unique_ptr<RmlView> view;
    Json lastState,fixture,captured=Json::array();
    bool initialized=false,visible=true,software=false,dirty=true,resizing=false;
    double nextUpdate=0;std::string error;
    Impl(HWND w,std::function<void(Json)> r,const std::filesystem::path& test):window(w),receive(std::move(r)),testDirectory(test){system.SetWindow(w);}
    ~Impl(){Stop();}
    void Log(const std::string& message){system.LogMessage(Rml::Log::LT_INFO,message);}
    void Stop() {
        view.reset();
        if(context){Rml::RemoveContext("launcher");context=nullptr;}
        if(initialized){Rml::Shutdown();initialized=false;}
        Rml::SetTextInputHandler(nullptr);Rml::SetRenderInterface(nullptr);Rml::SetFileInterface(nullptr);Rml::SetSystemInterface(nullptr);
        renderer.reset();target.Reset();swap.Reset();deviceContext.Reset();device.Reset();
    }
    void Device(bool cpu) {
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=2;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.OutputWindow=window;desc.SampleDesc.Count=1;
        desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0};
        Check(D3D11CreateDeviceAndSwapChain(nullptr,cpu?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,1,D3D11_SDK_VERSION,&desc,swap.Put(),device.Put(),nullptr,deviceContext.Put()),"D3D11CreateDeviceAndSwapChain");
        Com<ID3D11Device1> checkedDevice;
        Check(device->QueryInterface(__uuidof(ID3D11Device1),reinterpret_cast<void**>(checkedDevice.Put())),"ID3D11Device1");
        software=cpu;
    }
    void Resize() {
        if(!renderer||!context||IsIconic(window)||resizing)return;
        RECT rect{};GetClientRect(window,&rect);if(rect.right<=0||rect.bottom<=0)return;
        resizing=true;
        try {
            deviceContext->OMSetRenderTargets(0,nullptr,nullptr);target.Reset();
            Check(swap->ResizeBuffers(0,UINT(rect.right),UINT(rect.bottom),DXGI_FORMAT_UNKNOWN,0),"ResizeBuffers");
            Com<ID3D11Texture2D> buffer;Check(swap->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(buffer.Put())),"GetBuffer");
            Check(device->CreateRenderTargetView(buffer.p,nullptr,target.Put()),"CreateRenderTargetView");
            renderer->SetViewport(rect.right,rect.bottom);
            context->SetDimensions({rect.right,rect.bottom});
            using Dpi=UINT(WINAPI*)(HWND);auto getDpi=reinterpret_cast<Dpi>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow")));
            context->SetDensityIndependentPixelRatio(getDpi?float(getDpi(window))/96.f:1.f);dirty=true;
        }catch(...){resizing=false;throw;}
        resizing=false;
    }
    void Start(bool cpu) {
        const Json saved=view?view->Save():Json{};
        Stop();error.clear();
        try {
            try{Device(cpu);}catch(const std::exception& e){if(cpu)throw;Log(std::string("GPU initialization failed; retry with WARP: ")+e.what());Stop();Device(true);}
            renderer=std::make_unique<Renderer>(device.p);
            Rml::SetSystemInterface(&system);Rml::SetFileInterface(&files);Rml::SetRenderInterface(renderer.get());Rml::SetTextInputHandler(&ime);
            if(!Rml::Initialise())throw std::runtime_error("RmlUi initialization failed");
            initialized=true;
            const auto font=Resource(204);
            for(auto weight:{Rml::Style::FontWeight::Normal,Rml::Style::FontWeight::Bold})
                if(!Rml::LoadFontFace({font.data(),font.size()},"Launcher",Rml::Style::FontStyle::Normal,weight,true))throw std::runtime_error("Bundled font could not be loaded");
            context=Rml::CreateContext("launcher",{1,1});if(!context)throw std::runtime_error("RmlUi context creation failed");
            view=std::make_unique<RmlView>(*context,[this](Json command){if(fixture.empty())receive(std::move(command));else captured.push_back(std::move(command));dirty=true;});
            Resize();if(!lastState.empty())Send(lastState);view->Restore(saved);
            Log(software?"RmlUi 6.3 initialized: CPU WARP, FreeType 2.13.3":"RmlUi 6.3 initialized: GPU Direct3D 11, FreeType 2.13.3");
            dirty=true;
        }catch(const std::exception& e){Stop();error=e.what();Log(error);PostMessageW(window,DisplayErrorMessage,0,0);}
    }
    void Fail(const std::string& reason) {
        Log(reason);
        if(!software){PostMessageW(window,ReloadDisplayMessage,1,0);return;}
        error=reason;PostMessageW(window,DisplayErrorMessage,0,0);Stop();
    }
    void Send(Json state) {
        lastState=state;if(!view)return;
        if(!fixture.empty())state=fixture;
        state["display"]={{"software",software},{"throttled",!visible}};
        const auto pixels=state["profile"].value("pixels",std::string{});
        renderer->emblem=pixels.empty()?std::vector<uint8_t>{}:p2p::Unbase64(pixels);
        view->State(state);dirty=true;
    }
    // 明示した開発試験だけが使うファイル式UI操作口。通常起動では読まない。
    void Test() {
        if(testDirectory.empty()||!view)return;
        const auto outputPath=testDirectory/L"response.tmp";
        const auto responsePath=testDirectory/L"response.json";
        // 読取り側が旧応答を開いている間、Windowsは置換を拒否することがある。
        // 次の描画周期で送達を再試行し、既に実行した操作そのものは繰り返さない。
        std::error_code ec;
        if(std::filesystem::exists(outputPath,ec) &&
            !MoveFileExW(outputPath.c_str(),responsePath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return;
        auto requestPath=testDirectory/L"request.json";
        if(!std::filesystem::exists(requestPath,ec))return;
        Json response;
        try {
            if(std::filesystem::file_size(requestPath)>128*1024)throw std::runtime_error("test request too large");
            std::ifstream input(requestPath);Json request;input>>request;input.close();std::filesystem::remove(requestPath);
            const auto action=request.value("action","");response["sequence"]=request.value("sequence",int64_t{0});
            if(action=="fixture") {fixture=request.value("state",Json{});captured=Json::array();Send(lastState);context->Update();response["result"]=view->Inspect();}
            else if(action=="dpi") {context->SetDensityIndependentPixelRatio(request.at("value").get<float>());context->Update();response["result"]=view->Inspect();}
            else if(action=="renderer") {Start(request.value("software",true));response["result"]={{"software",software}};}
            else response["result"]=view->Test(request);
            response["commands"]=captured;response["ok"]=true;dirty=true;
        }catch(const std::exception& e){response["ok"]=false;response["error"]=e.what();std::filesystem::remove(requestPath,ec);}
        {std::ofstream output(outputPath);output<<response.dump();}
        MoveFileExW(outputPath.c_str(),responsePath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    }
    void Frame() {
        Test();if(!context||!visible||IsIconic(window)||!target.p)return;
        const auto now=system.GetElapsedTime();if(!dirty&&now<nextUpdate)return;
        try {
            context->Update();renderer->BeginFrame();context->Render();renderer->EndFrame(target.p);
            const auto result=swap->Present(0,0);
            if(FAILED(result)){Fail("Present failed: "+std::to_string(uint32_t(result)));return;}
            nextUpdate=now+std::clamp(context->GetNextUpdateDelay(),0.016,1.0);dirty=false;
        }catch(const std::exception& e){Fail(e.what());}
    }
};
RmlHost::RmlHost(HWND window,std::function<void(Json)> receive,const std::filesystem::path& directory):impl_(std::make_unique<Impl>(window,std::move(receive),directory)){}
RmlHost::~RmlHost()=default;
void RmlHost::Start(bool software){impl_->Start(software);}
void RmlHost::Resize(){try{impl_->Resize();}catch(const std::exception& e){impl_->Fail(e.what());}}
void RmlHost::Visibility(bool visible){if(impl_->visible!=visible){impl_->visible=visible;impl_->dirty=true;impl_->Log(visible?"Drawing resumed":"Drawing paused; native matching remains active");}}
void RmlHost::ShowError(){if(!impl_->error.empty())MessageBoxW(impl_->window,(L"画面を初期化できませんでした。F8でCPU描画をお試しください。\n"+RmlWin32::ConvertToUTF16(impl_->error)).c_str(),L"CCCaster / RmlUi",MB_OK|MB_ICONERROR);}
void RmlHost::Send(const Json& value){impl_->Send(value);}
void RmlHost::Frame(){impl_->Frame();}
bool RmlHost::Message(UINT message,WPARAM w,LPARAM l){
    if(!impl_->context)return false;
    if(message==WM_PAINT){impl_->dirty=true;return false;}
    if((message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST)||(message>=WM_KEYFIRST&&message<=WM_KEYLAST)||message==WM_IME_COMPOSITION)impl_->dirty=true;
    return !RmlWin32::WindowProcedure(impl_->context,impl_->ime,impl_->window,message,w,l);
}
bool RmlHost::Ready()const{return impl_->view!=nullptr;}
bool RmlHost::Software()const{return impl_->software;}
}
