#pragma once
#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>
#include <algorithm>

// 元画像をEXEへ同梱。縦横比を保つ描画用に原寸でデコードする。
struct HeaderImage {
    IDirect3DTexture9* texture = nullptr;
    UINT width = 0, height = 0;

    void Release() {
        if (texture) { texture->Release(); texture = nullptr; }
    }

    bool Load(IDirect3DDevice9* device) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        bool loaded = false;
        do {
            const auto module = GetModuleHandleW(nullptr);
            const auto resource = FindResourceW(module, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10)); // RCDATA
            if (!resource) break;
            const auto memory = LoadResource(module, resource);
            auto* bytes = static_cast<BYTE*>(LockResource(memory));
            if (!bytes) break;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                    IID_IWICImagingFactory, reinterpret_cast<void**>(&factory)))) break;
            if (FAILED(factory->CreateStream(&stream))) break;
            if (FAILED(stream->InitializeFromMemory(bytes, SizeofResource(module, resource)))) break;
            if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) break;
            if (FAILED(decoder->GetFrame(0, &frame))) break;
            UINT sourceHeight = 0;
            if (FAILED(frame->GetSize(&width, &sourceHeight)) || !width || !sourceHeight) break;
            height = sourceHeight;
            if (FAILED(factory->CreateFormatConverter(&converter))) break;
            if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) break;
            // MANAGEDにしてリサイズ時のD3D9 Resetでも画像を維持する。
            if (FAILED(device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8,
                    D3DPOOL_MANAGED, &texture, nullptr))) break;
            D3DLOCKED_RECT lock{};
            if (FAILED(texture->LockRect(0, &lock, nullptr, 0))) break;
            WICRect crop{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
            loaded = SUCCEEDED(converter->CopyPixels(&crop, lock.Pitch,
                lock.Pitch * height, static_cast<BYTE*>(lock.pBits)));
            texture->UnlockRect(0);
        } while (false);
        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();
        if (SUCCEEDED(com)) CoUninitialize();
        if (!loaded) Release();
        return loaded;
    }
};
