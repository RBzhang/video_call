#include "videoframerateutils.h"

#include <QtGlobal>
#include <QtMath>

namespace VideoFrameRateUtils
{

int intervalMilliseconds(int targetFps)
{
    if (targetFps < 1 || targetFps > 30) {
        return 0;
    }

    return qMax(1, qRound(1000.0 / static_cast<double>(targetFps)));
}

} // namespace VideoFrameRateUtils
