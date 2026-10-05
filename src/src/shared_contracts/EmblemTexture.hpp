#pragma once
#include <d3d9.h>
#include "shared_contracts/PlayerEmblem.hpp"
namespace cccaster::emblem {
class Texture {
    IDirect3DTexture9* texture_ = nullptr;
    uint32_t id_ = 0;
public:
    Texture() = default;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    ~Texture() { Release(); }
    void Release() { if (texture_) texture_->Release(); texture_ = nullptr; id_ = 0; }
    IDirect3DTexture9* Get() const { return texture_; }
    bool Update(IDirect3DDevice9* device, const Image* image) {
        const auto id = image ? image->id : 0;
        if (id == id_ && (!id || texture_)) return true;
        Release();
        if (!id) return true;
        if (!device || FAILED(device->CreateTexture(Width, Height, 1, 0, D3DFMT_A8R8G8B8,
                D3DPOOL_MANAGED, &texture_, nullptr))) return false;
        D3DLOCKED_RECT lock{};
        if (FAILED(texture_->LockRect(0, &lock, nullptr, 0))) { Release(); return false; }
        for (unsigned y = 0; y < Height; ++y)
            std::memcpy(static_cast<uint8_t*>(lock.pBits) + y * lock.Pitch,
                        image->pixels.data() + y * Width * 4, Width * 4);
        texture_->UnlockRect(0); id_ = id; return true;
    }
};
}
