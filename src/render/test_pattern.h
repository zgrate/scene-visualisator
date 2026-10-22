#pragma once
// The CPU-drawn composition (FR-13): colour bars or a mapping grid with every wall and strip slice outlined and named,
// so mapping mistakes show up on the walls, or a still image placed by source.fit (fit|fill|stretch). Live sources
// (video, capture) are drawn over it by the render thread once frames arrive; until then it reads "No signal".
#include "core/model.h"

#include <QImage>

namespace sv::render {

QImage make_composition(const json &project, const QImage &still = {}); // RGBA8888 at composition size (capped at 4096)

} // namespace sv::render
