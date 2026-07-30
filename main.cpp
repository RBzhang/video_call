#include "mainwindow.h"

#include <QApplication>
#include <QAudioDevice>
#include <QDebug>
#include <QMediaDevices>
#include <QMetaObject>
#include <QTimer>

#include <string>

namespace {

void invokeScenarioAction(MainWindow *window, const char *slot)
{
    if (!QMetaObject::invokeMethod(window, slot, Qt::DirectConnection)) {
        qCritical().nospace() << "[video_call] shutdown scenario could not invoke " << slot;
    }
}

bool scheduleShutdownScenario(MainWindow *window, const QStringList &arguments)
{
    constexpr auto scenarioPrefix = "--shutdown-scenario=";
    QString scenario;
    for (const QString &argument : arguments) {
        if (argument.startsWith(QLatin1StringView(scenarioPrefix))) {
            scenario = argument.sliced(static_cast<qsizetype>(std::char_traits<char>::length(scenarioPrefix)));
            break;
        }
    }
    if (scenario.isEmpty()) {
        return false;
    }

    int closeAfterMs = 300;
    if (scenario == QStringLiteral("udp")) {
        QTimer::singleShot(0, window, [window] { invokeScenarioAction(window, "onApplyNetworkSettingsClicked"); });
        closeAfterMs = 700;
    } else if (scenario == QStringLiteral("camera")) {
        QTimer::singleShot(0, window, [window] { invokeScenarioAction(window, "onStartCameraClicked"); });
        closeAfterMs = 1800;
    } else if (scenario == QStringLiteral("camera-video")) {
        QTimer::singleShot(0, window, [window] { invokeScenarioAction(window, "onApplyNetworkSettingsClicked"); });
        QTimer::singleShot(100, window, [window] { invokeScenarioAction(window, "onStartCameraClicked"); });
        QTimer::singleShot(1100, window, [window] { invokeScenarioAction(window, "onStartVideoSendClicked"); });
        closeAfterMs = 2800;
    } else if (scenario == QStringLiteral("audio")) {
        QTimer::singleShot(0, window, [window] { invokeScenarioAction(window, "onApplyAudioSettingsClicked"); });
        QTimer::singleShot(700, window, [window] { invokeScenarioAction(window, "onStartAudioClicked"); });
        closeAfterMs = 2000;
    } else if (scenario == QStringLiteral("audio-video")) {
        QTimer::singleShot(0, window, [window] { invokeScenarioAction(window, "onApplyNetworkSettingsClicked"); });
        QTimer::singleShot(100, window, [window] { invokeScenarioAction(window, "onApplyAudioSettingsClicked"); });
        QTimer::singleShot(200, window, [window] { invokeScenarioAction(window, "onStartCameraClicked"); });
        QTimer::singleShot(1200, window, [window] { invokeScenarioAction(window, "onStartAudioClicked"); });
        QTimer::singleShot(1500, window, [window] { invokeScenarioAction(window, "onStartVideoSendClicked"); });
        closeAfterMs = 3200;
    } else if (scenario == QStringLiteral("camera-restart")) {
        QTimer::singleShot(0, window, [window] { invokeScenarioAction(window, "onStartCameraClicked"); });
        QTimer::singleShot(800, window, [window] { invokeScenarioAction(window, "onStopCameraClicked"); });
        QTimer::singleShot(1500, window, [window] { invokeScenarioAction(window, "onStartCameraClicked"); });
        closeAfterMs = 2800;
    } else if (scenario != QStringLiteral("idle")) {
        qCritical().noquote() << QStringLiteral("[video_call] unknown shutdown scenario:") << scenario;
        return false;
    }

    qInfo().noquote() << QStringLiteral("[video_call] running shutdown scenario:") << scenario;
    QTimer::singleShot(closeAfterMs, window, &QWidget::close);
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QCoreApplication::setQuitLockEnabled(false);
    QMediaDevices::defaultAudioInput();
    QMediaDevices::defaultAudioOutput();
    MainWindow w;
    scheduleShutdownScenario(&w, QCoreApplication::arguments());
    w.show();
    return QApplication::exec();
}
