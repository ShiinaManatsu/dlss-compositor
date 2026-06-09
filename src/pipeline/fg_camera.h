#pragma once

#include "../core/types.h"
#include "../dlss/dlss_fg.h"

namespace dlss {

void fillFgCameraInputs(FgInputs& fgIn,
                        const CameraData* current,
                        const CameraData* previous);

} // namespace dlss
