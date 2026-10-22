// Spout 2 receiver without the Spout SDK: the sender list and per-sender info are plain shared memory, the texture is
// a legacy D3D11 shared handle guarded by a named mutex. Layouts follow Spout2's SpoutSenderNames / SharedTextureInfo.
#include "media/native_receiver.h"

#include <d3d11.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace sv::media {
namespace {

constexpr int name_bytes = 256; // SpoutMaxSenderNameLen

struct SharedTextureInfo {
    std::uint32_t share_handle;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t format; // DXGI_FORMAT, or a D3D9 format (0 / 21) from old DX9 senders, both BGRA
    std::uint32_t usage;
    wchar_t description[128];
    std::uint32_t partner_id;
};
static_assert(sizeof(SharedTextureInfo) == 280);

template <class T> struct Com {
    T *p = nullptr;
    ~Com() {
        if (p)
            p->Release();
    }
    T *operator->() const { return p; }
};

// Copies a whole named shared-memory block of `size` bytes; false when it does not exist.
bool read_shared(const std::string &name, void *out, size_t size) {
    HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
    if (!map)
        return false;
    const void *view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, size);
    if (view)
        memcpy(out, view, size);
    if (view)
        UnmapViewOfFile(view);
    CloseHandle(map);
    return view != nullptr;
}

class SpoutReceiver : public NativeReceiver {
  public:
    SpoutReceiver(std::string sender, std::function<void(QImage)> on_frame)
        : thread_([this, sender = std::move(sender), on_frame = std::move(on_frame)] { run(sender, on_frame); }) {}
    ~SpoutReceiver() override {
        stop_ = true;
        thread_.join();
    }

  private:
    void run(const std::string &sender, const std::function<void(QImage)> &on_frame) {
        Com<ID3D11Device> device;
        Com<ID3D11DeviceContext> context;
        for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
            if (SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device.p,
                                            nullptr, &context.p)))
                break;
        if (!device.p)
            return;
        Com<ID3D11Texture2D> staging, shared;
        D3D11_TEXTURE2D_DESC staged{};
        std::uint32_t opened = 0; // share handle `shared` was opened from
        while (!stop_) {
            // shortcut: polls at ~60 Hz instead of Spout's frame-count semaphore, upgrade if senders above 60 fps
            // matter
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            SharedTextureInfo info{};
            if (!read_shared(sender, &info, sizeof info) || !info.share_handle || !info.width || !info.height)
                continue; // sender gone; keep polling so it reconnects when the name comes back
            const bool rgba = info.format == DXGI_FORMAT_R8G8B8A8_UNORM;
            if (!rgba && info.format != DXGI_FORMAT_B8G8R8A8_UNORM && info.format != 0 && info.format != 21)
                continue;                      // 10-bit and float senders are not read
            if (opened != info.share_handle) { // a new handle on sender restart or resize
                if (shared.p)
                    shared.p->Release();
                shared.p = nullptr;
                opened = 0;
                if (FAILED(device->OpenSharedResource(reinterpret_cast<HANDLE>(std::uintptr_t(info.share_handle)),
                                                      __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&shared.p))))
                    continue;
                opened = info.share_handle;
            }
            D3D11_TEXTURE2D_DESC desc;
            shared->GetDesc(&desc);
            if (!staging.p || staged.Width != desc.Width || staged.Height != desc.Height ||
                staged.Format != desc.Format) {
                if (staging.p)
                    staging.p->Release();
                staging.p = nullptr;
                staged = desc;
                staged.Usage = D3D11_USAGE_STAGING;
                staged.BindFlags = 0;
                staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                staged.MiscFlags = 0;
                staged.MipLevels = staged.ArraySize = 1;
                staged.SampleDesc = {1, 0};
                if (FAILED(device->CreateTexture2D(&staged, nullptr, &staging.p)))
                    continue;
            }
            HANDLE mutex = OpenMutexA(SYNCHRONIZE, FALSE, (sender + "_SpoutAccessMutex").c_str());
            if (mutex && WaitForSingleObject(mutex, 67) == WAIT_TIMEOUT) {
                CloseHandle(mutex);
                continue;
            }
            context->CopyResource(staging.p, shared.p);
            context->Flush();
            if (mutex) {
                ReleaseMutex(mutex);
                CloseHandle(mutex);
            }
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (FAILED(context->Map(staging.p, 0, D3D11_MAP_READ, 0, &mapped)))
                continue;
            QImage image = QImage(static_cast<const uchar *>(mapped.pData), int(desc.Width), int(desc.Height),
                                  int(mapped.RowPitch), rgba ? QImage::Format_RGBA8888 : QImage::Format_ARGB32)
                               .copy();
            context->Unmap(staging.p, 0);
            on_frame(std::move(image));
        }
    }

    std::atomic_bool stop_{false};
    std::thread thread_;
};

} // namespace

std::unique_ptr<NativeReceiver> NativeReceiver::create(const QString &kind, const QString &sender,
                                                       std::function<void(QImage)> on_frame) {
    if (kind != "spout")
        return nullptr;
    return std::make_unique<SpoutReceiver>(sender.toStdString(), std::move(on_frame));
}

QStringList NativeReceiver::senders(const QString &kind) {
    QStringList out;
    if (kind != "spout")
        return out;
    std::string names(64 * name_bytes, '\0'); // Spout's default maximum of 64 senders
    if (!read_shared("SpoutSenderNames", names.data(), names.size()))
        return out;
    for (size_t at = 0; at < names.size() && names[at]; at += name_bytes)
        out << QString::fromLocal8Bit(names.c_str() + at);
    return out;
}

} // namespace sv::media
