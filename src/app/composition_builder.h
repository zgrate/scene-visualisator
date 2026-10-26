#pragma once
// The composition the walls show: Session's CPU image, the main live source's latest frame placed over it, then the
// per-output feeds and the take's recorded feeds. The viewport's render thread and the DDP pixel output each own one,
// because MediaSource::latest(serial) keeps its serial per consumer. Setters run while the UI thread is not editing.
#include "core/model.h"
#include "media/media_source.h"

#include <QImage>
#include <map>
#include <memory>
#include <string>
#include <vector>

class CompositionBuilder {
  public:
    using Sources = std::vector<std::pair<std::string, const sv::media::MediaSource *>>;

    // Each returns true when the composition went back to the CPU image, which the caller then shows again.
    bool set_project(std::shared_ptr<const sv::json> project, QImage cpu);
    bool set_feeds(const Sources &live);
    bool set_recorded(const Sources &recorded);
    void set_opacity(std::map<std::string, double> opacity);
    // Polls every source: the new composition, or a null image when nothing changed since the last call.
    QImage update(const sv::media::MediaSource &media);
    const QImage &cpu() const { return cpu_; }

  private:
    struct Feed {
        const sv::media::MediaSource *source = nullptr;
        quint64 serial = 0;
        QImage frame; // kept, so a quiet feed stays on its walls while another one updates
    };
    bool set(std::map<std::string, Feed> &feeds, const Sources &sources);
    void reset();

    std::shared_ptr<const sv::json> project_;
    QImage cpu_;  // Session::composition() as of the last set_project
    QImage base_; // the composition under the feeds: the main source's frame or the CPU one
    quint64 media_serial_ = 0;
    std::map<std::string, Feed> feeds_;    // output uuid -> its live feed
    std::map<std::string, Feed> recorded_; // output uuid -> the take's video, drawn over the live feed
    std::map<std::string, double> opacity_;
    bool opacity_changed_ = false;
};
