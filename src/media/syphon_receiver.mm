// Syphon client on SyphonClientBase: the server's IOSurface is locked and copied to a BGRA image on Syphon's frame
// thread, with no GL or Metal context. Compiled with SV_WITH_SYPHON (macOS only).
#include "media/native_receiver.h"

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>

#import "SyphonClientBase.h"
#import "SyphonServerDirectory.h"

#include <atomic>
#include <thread>

// From SyphonSubclassing.h, which needs the framework's <Syphon/...> include layout.
@interface SyphonClientBase (SVSubclassing)
- (IOSurfaceRef)newSurface;
@end

namespace sv::media {
namespace {

NSString *display_name(NSDictionary *server) {
    NSString *name = server[SyphonServerDescriptionNameKey];
    NSString *app = server[SyphonServerDescriptionAppNameKey];
    if (name.length && app.length)
        return [NSString stringWithFormat:@"%@ - %@", app, name];
    return name.length ? name : (app ?: @"");
}

class SyphonReceiver : public NativeReceiver {
  public:
    SyphonReceiver(NSString *sender, std::function<void(QImage)> on_frame)
        : sender_(sender), on_frame_(std::move(on_frame)), thread_([this] { run(); }) {}
    ~SyphonReceiver() override {
        stop_ = true;
        thread_.join();
    }

  private:
    void run() {
        SyphonClientBase *client = nil;
        while (!stop_) {
            @autoreleasepool {
                if (!client.isValid) { // connect, or reconnect when the server name comes back
                    [client stop];
                    client = nil;
                    for (NSDictionary *server in [SyphonServerDirectory sharedDirectory].servers)
                        if ([display_name(server) isEqualToString:sender_]) {
                            client = [[SyphonClientBase alloc] initWithServerDescription:server
                                                                                 options:nil
                                                                         newFrameHandler:^(SyphonClientBase *c) {
                                                                           deliver(c);
                                                                         }];
                            break;
                        }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        [client stop];
    }

    void deliver(SyphonClientBase *client) {
        IOSurfaceRef surface = [client newSurface];
        if (!surface)
            return;
        if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) == kIOReturnSuccess) {
            // Syphon surfaces are always BGRA8.
            on_frame_(QImage(static_cast<const uchar *>(IOSurfaceGetBaseAddress(surface)),
                             int(IOSurfaceGetWidth(surface)), int(IOSurfaceGetHeight(surface)),
                             qsizetype(IOSurfaceGetBytesPerRow(surface)), QImage::Format_ARGB32)
                          .copy());
            IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
        }
        CFRelease(surface);
    }

    NSString *sender_;
    std::function<void(QImage)> on_frame_;
    std::atomic_bool stop_{false};
    std::thread thread_;
};

} // namespace

std::unique_ptr<NativeReceiver> NativeReceiver::create(const QString &kind, const QString &sender,
                                                       std::function<void(QImage)> on_frame) {
    if (kind != "syphon")
        return nullptr;
    return std::make_unique<SyphonReceiver>(sender.toNSString(), std::move(on_frame));
}

QStringList NativeReceiver::senders(const QString &kind) {
    QStringList out;
    if (kind == "syphon")
        for (NSDictionary *server in [SyphonServerDirectory sharedDirectory].servers)
            out << QString::fromNSString(display_name(server));
    return out;
}

} // namespace sv::media
