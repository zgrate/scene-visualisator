#pragma once
// FR-13 GPU-shared inputs: Spout on Windows, Syphon on macOS. One implementation file per platform is compiled
// (spout_receiver.cpp, syphon_receiver.mm, native_receiver_none.cpp); kinds the platform lacks return null / empty.
#include <QImage>
#include <QStringList>

#include <functional>
#include <memory>

namespace sv::media {

class NativeReceiver {
  public:
    virtual ~NativeReceiver() = default;

    // Starts receiving `sender` on the receiver's own thread; frames arrive as BGRA images on that thread.
    static std::unique_ptr<NativeReceiver> create(const QString &kind, const QString &sender,
                                                  std::function<void(QImage)> on_frame);
    static QStringList senders(const QString &kind);
};

} // namespace sv::media
