#pragma once
// The CPU-drawn composition (FR-13): colour bars or a mapping grid with every wall and strip slice outlined and named,
// so mapping mistakes show up on the walls, or a still image placed by source.fit (fit|fill|stretch). Live sources
// (video, capture) are drawn over it by the render thread once frames arrive; until then it reads "No signal".
#include "core/model.h"

#include <QImage>
#include <map>

namespace sv::render {

QImage make_composition(const json &project, const QImage &still = {}); // RGBA8888 at composition size (capped at 4096)

// Per-output live feeds (one NDI stream per Resolume Screen): every wall or strip on a fed output gets its outRect of
// that output's frame painted onto its slice of the composition. Output uuid -> latest frame.
QImage compose_feeds(const json &project, QImage composition, const std::map<std::string, QImage> &feeds,
                     const std::map<std::string, double> &opacity = {}); // per output, 1 if missing

} // namespace sv::render
