#include "media/ndi.h"

#include <QDir>
#include <QLibrary>
#include <QtGlobal>

namespace sv::media {

const NDIlib_v6 *ndi_api() {
    static const NDIlib_v6 *api = []() -> const NDIlib_v6 * {
        static QLibrary lib;
        const QString dir = qEnvironmentVariable(NDILIB_REDIST_FOLDER);
        for (const QString &name : {dir.isEmpty() ? QString() : QDir(dir).filePath(NDILIB_LIBRARY_NAME),
                                    QStringLiteral(NDILIB_LIBRARY_NAME)}) {
            if (name.isEmpty())
                continue;
            lib.setFileName(name);
            if (lib.load())
                break;
        }
        auto load = reinterpret_cast<const NDIlib_v6 *(*)()>(lib.resolve("NDIlib_v6_load"));
        const NDIlib_v6 *v6 = load ? load() : nullptr;
        if (!v6 || !v6->initialize()) {
            if (lib.isLoaded())
                qWarning("NDI runtime found but not usable (CPU unsupported or wrong version)");
            return nullptr;
        }
        return v6;
    }();
    return api;
}

QStringList ndi_sources(int wait_ms) {
    QStringList out;
    const NDIlib_v6 *ndi = ndi_api();
    if (!ndi)
        return out;
    NDIlib_find_create_t settings;
    NDIlib_find_instance_t find = ndi->find_create_v2(&settings);
    if (!find)
        return out;
    ndi->find_wait_for_sources(find, uint32_t(wait_ms));
    uint32_t count = 0;
    const NDIlib_source_t *sources = ndi->find_get_current_sources(find, &count);
    for (uint32_t i = 0; i < count; ++i)
        out << QString::fromUtf8(sources[i].p_ndi_name);
    ndi->find_destroy(find);
    return out;
}

NdiReceiver::NdiReceiver(const QString &source, std::function<void(QImage)> on_frame) {
    const NDIlib_v6 *ndi = ndi_api();
    if (!ndi)
        return;
    thread_ = std::thread([this, ndi, name = source.toStdString(), on_frame = std::move(on_frame)] {
        NDIlib_recv_create_v3_t settings;
        settings.source_to_connect_to.p_ndi_name = name.c_str();
        settings.color_format = NDIlib_recv_color_format_BGRX_BGRA;
        settings.bandwidth = NDIlib_recv_bandwidth_highest;
        settings.allow_video_fields = false;
        settings.p_ndi_recv_name = "Scene Visualiser";
        NDIlib_recv_instance_t recv = ndi->recv_create_v3(&settings);
        if (!recv)
            return;
        while (!stop_) {
            NDIlib_video_frame_v2_t video;
            // Audio and metadata pointers are null, so the runtime drops them.
            if (ndi->recv_capture_v3(recv, &video, nullptr, nullptr, 100) != NDIlib_frame_type_video)
                continue;
            const auto format =
                video.FourCC == NDIlib_FourCC_video_type_BGRX ? QImage::Format_RGB32 : QImage::Format_ARGB32;
            QImage image(video.p_data, video.xres, video.yres, video.line_stride_in_bytes, format);
            on_frame(image.copy()); // detach from the runtime's buffer before freeing it
            ndi->recv_free_video_v2(recv, &video);
        }
        ndi->recv_destroy(recv);
    });
}

NdiReceiver::~NdiReceiver() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

} // namespace sv::media
