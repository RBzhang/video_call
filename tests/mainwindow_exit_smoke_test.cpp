#include "audioworker.h"
#include "cameraworker.h"
#include "mainwindow.h"
#include "remotevideodecoder.h"

#include <QApplication>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QCameraDevice>
#include <QDebug>
#include <QMediaDevices>
#include <QMetaObject>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QTimer>

#include <cstdio>

namespace {

QMutex g_messageMutex;
bool g_destroyedRunningThreadWarning = false;
QtMessageHandler g_previousMessageHandler = nullptr;

struct ExitScenario
{
    const char *name = "unnamed";
    QList<QPair<int, const char *>> actions;
    int closeAfterMs = 0;
};

void exitSmokeMessageHandler(QtMsgType type,
                             const QMessageLogContext &context,
                             const QString &message)
{
    if (type == QtWarningMsg
        && message.contains(QStringLiteral("QThread: Destroyed while thread is still running"))) {
        QMutexLocker locker(&g_messageMutex);
        g_destroyedRunningThreadWarning = true;
    }

    if (g_previousMessageHandler) {
        g_previousMessageHandler(type, context, message);
        return;
    }

    const QByteArray localMessage = message.toLocal8Bit();
    std::fprintf(stderr, "%s\n", localMessage.constData());
}

bool runExitCycle(QApplication *application, const ExitScenario &scenario)
{
    MainWindow window;
    int shutdownSignalCount = 0;
    bool workersDestroyed = false;
    bool actionsInvoked = true;
    QObject::connect(&window,
                     &MainWindow::shutdownCompleted,
                     &window,
                     [&shutdownSignalCount, &workersDestroyed](bool destroyed) {
                         ++shutdownSignalCount;
                         workersDestroyed = destroyed;
                     });

    for (const auto &action : scenario.actions) {
        QTimer::singleShot(action.first, &window, [&window, &actionsInvoked, action] {
            if (!QMetaObject::invokeMethod(&window, action.second, Qt::DirectConnection)) {
                std::fprintf(stderr, "failed to invoke action %s\n", action.second);
                actionsInvoked = false;
            }
        });
    }

    window.show();
    QTimer::singleShot(scenario.closeAfterMs, &window, [&window, application] {
        window.close();
        application->quit();
    });
    application->exec();

    if (!actionsInvoked || shutdownSignalCount != 1 || !workersDestroyed) {
        std::fprintf(stderr,
                     "exit scenario %s failed: actions=%d shutdown signals=%d workers destroyed=%d\n",
                     scenario.name,
                     actionsInvoked,
                     shutdownSignalCount,
                     workersDestroyed);
        return false;
    }

    std::printf("PASS %s\n", scenario.name);
    return true;
}

bool supportsFixedAudioFormat()
{
    QAudioFormat format;
    format.setSampleRate(16000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    const QAudioDevice input = QMediaDevices::defaultAudioInput();
    const QAudioDevice output = QMediaDevices::defaultAudioOutput();
    return !input.isNull() && !output.isNull()
        && input.isFormatSupported(format) && output.isFormatSupported(format);
}

bool runScenario(QApplication *application,
                 const ExitScenario &scenario,
                 int *completedCycles)
{
    if (!runExitCycle(application, scenario)) {
        qCritical().noquote()
            << QStringLiteral("exit scenario failed: %1").arg(QString::fromLatin1(scenario.name));
        return false;
    }
    if (completedCycles) {
        ++*completedCycles;
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);

    CameraWorker::resetDestructionCount();
    AudioWorker::resetDestructionCount();
    RemoteVideoDecoder::resetDestructionCount();
    g_previousMessageHandler = qInstallMessageHandler(exitSmokeMessageHandler);

    const bool hasCamera = !QMediaDevices::videoInputs().isEmpty();
    const bool hasSupportedAudio = supportsFixedAudioFormat();
    int completedCycles = 0;
    bool succeeded = true;

    const ExitScenario idleScenario{"startup-close", {}, 0};
    for (int repetition = 0; repetition < 3 && succeeded; ++repetition) {
        succeeded = runScenario(&application, idleScenario, &completedCycles);
    }

    if (succeeded) {
        succeeded = runScenario(&application,
                                ExitScenario{"udp-configure-close",
                                             {{0, "onApplyNetworkSettingsClicked"}},
                                             150},
                                &completedCycles);
    }

    if (hasCamera) {
        if (succeeded) {
            succeeded = runScenario(&application,
                                    ExitScenario{"camera-start-close",
                                                 {{0, "onStartCameraClicked"}},
                                                 600},
                                    &completedCycles);
        }
        if (succeeded) {
            succeeded = runScenario(&application,
                                    ExitScenario{"camera-video-close",
                                                 {{0, "onApplyNetworkSettingsClicked"},
                                                  {20, "onStartCameraClicked"},
                                                  {400, "onStartVideoSendClicked"}},
                                                 900},
                                    &completedCycles);
        }
        if (succeeded) {
            succeeded = runScenario(&application,
                                    ExitScenario{"camera-restart-close",
                                                 {{0, "onStartCameraClicked"},
                                                  {250, "onStopCameraClicked"},
                                                  {500, "onStartCameraClicked"}},
                                                 900},
                                    &completedCycles);
        }
    } else {
        std::printf("SKIP camera scenarios: no Qt camera device available\n");
    }

    if (hasSupportedAudio) {
        if (succeeded) {
            succeeded = runScenario(&application,
                                    ExitScenario{"audio-start-close",
                                                 {{0, "onApplyAudioSettingsClicked"},
                                                  {250, "onStartAudioClicked"}},
                                                 700},
                                    &completedCycles);
        }
        if (hasCamera && succeeded) {
            succeeded = runScenario(&application,
                                    ExitScenario{"audio-video-close",
                                                 {{0, "onApplyNetworkSettingsClicked"},
                                                  {20, "onApplyAudioSettingsClicked"},
                                                  {50, "onStartCameraClicked"},
                                                  {350, "onStartAudioClicked"},
                                                  {450, "onStartVideoSendClicked"}},
                                                 1000},
                                    &completedCycles);
        }
    } else {
        std::printf("SKIP audio scenarios: no compatible 16 kHz mono Int16 input/output pair\n");
    }

    qInstallMessageHandler(g_previousMessageHandler);

    bool destroyedRunningThreadWarning = false;
    {
        QMutexLocker locker(&g_messageMutex);
        destroyedRunningThreadWarning = g_destroyedRunningThreadWarning;
    }
    if (destroyedRunningThreadWarning) {
        qCritical().noquote()
            << QStringLiteral("exit smoke test observed a QThread destroyed while still running warning.");
        succeeded = false;
    }

    if (CameraWorker::destructionCount() != completedCycles
        || AudioWorker::destructionCount() != completedCycles
        || RemoteVideoDecoder::destructionCount() != completedCycles) {
        qCritical().noquote()
            << QStringLiteral("exit smoke test expected exactly %1 destructor calls per worker; got camera=%2, audio=%3, decoder=%4")
                   .arg(completedCycles)
                   .arg(CameraWorker::destructionCount())
                   .arg(AudioWorker::destructionCount())
                   .arg(RemoteVideoDecoder::destructionCount());
        succeeded = false;
    }

    return succeeded ? 0 : 1;
}
