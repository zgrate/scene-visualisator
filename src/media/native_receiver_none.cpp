#include "media/native_receiver.h"

namespace sv::media {

std::unique_ptr<NativeReceiver> NativeReceiver::create(const QString &, const QString &, std::function<void(QImage)>) {
    return nullptr;
}

QStringList NativeReceiver::senders(const QString &) { return {}; }

} // namespace sv::media
