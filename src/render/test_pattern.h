#pragma once
// The alpha composition source (FR-13 test patterns; live inputs are Beta): colour bars plus every LED wall's
// slice outlined and named, so mapping mistakes show up on the walls.
#include "core/model.h"

#include <QImage>

namespace sv::render {

QImage make_composition(const json &project); // RGBA8888 at composition size (capped at 4096)

} // namespace sv::render
