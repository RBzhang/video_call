#include "videoframerateutils.h"

#include <QCoreApplication>
#include <QDebug>

namespace {

bool checkInterval(int targetFps, int expectedIntervalMs)
{
    const int actualIntervalMs = VideoFrameRateUtils::intervalMilliseconds(targetFps);
    if (actualIntervalMs == expectedIntervalMs) {
        return true;
    }

    qCritical().noquote()
        << QStringLiteral("[videoframerateutils_test] FPS %1：期望 %2 ms，实际 %3 ms。")
               .arg(targetFps)
               .arg(expectedIntervalMs)
               .arg(actualIntervalMs);
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);

    const bool success = checkInterval(1, 1000)
        && checkInterval(5, 200)
        && checkInterval(10, 100)
        && checkInterval(15, 67)
        && checkInterval(20, 50)
        && checkInterval(25, 40)
        && checkInterval(30, 33)
        && checkInterval(0, 0)
        && checkInterval(31, 0);
    return success ? 0 : 1;
}
