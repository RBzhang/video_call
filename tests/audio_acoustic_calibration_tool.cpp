#include "audioworker.h"

#include <QCoreApplication>
#include <QTimer>
#include <QDebug>

#include <cstdio>

namespace {

constexpr quint16 LocalPort = 5512;
constexpr quint16 PeerPort = 5513;

void finish(QCoreApplication *application, AudioWorker *worker, int exitCode)
{
    // Calibration completion is emitted from QAudioSource::readyRead().  Do
    // not synchronously destroy the source from inside that callback.
    QTimer::singleShot(0, worker, &AudioWorker::stopAudio);
    QTimer::singleShot(100, application, [application, exitCode]() {
        application->exit(exitCode);
    });
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("audio_acoustic_calibration_tool"));
    const bool verifyAecEffect = application.arguments().contains(QStringLiteral("--verify-aec"));

    AudioWorker worker;
    bool finished = false;
    auto finishOnce = [&](int exitCode) {
        if (finished) {
            return;
        }
        finished = true;
        finish(&application, &worker, exitCode);
    };

    QObject::connect(&worker,
                     &AudioWorker::audioStarted,
                     &application,
                     [&worker, verifyAecEffect](const QString &message) {
        qInfo().noquote() << message;
        std::fprintf(stderr, "AUDIO_STARTED\n");
        QTimer::singleShot(100,
                           &worker,
                           verifyAecEffect ? &AudioWorker::verifyLocalAecEffect
                                           : &AudioWorker::playLocalAecTestTone);
    });
    if (verifyAecEffect) {
        QObject::connect(&worker,
                         &AudioWorker::localAecEffectVerified,
                         &application,
                         [&](int delayMs,
                             double echoReductionDb,
                             double rawEchoRmsDbfs,
                             double processedEchoRmsDbfs,
                             double rawCorrelation,
                             double processedCorrelation) {
                             qInfo().nospace()
                                 << "AEC_VERIFICATION_RESULT delay_ms=" << delayMs
                                 << " echo_reduction_db=" << echoReductionDb
                                 << " raw_echo_rms_dbfs=" << rawEchoRmsDbfs
                                 << " processed_echo_rms_dbfs=" << processedEchoRmsDbfs
                                 << " correlation=" << rawCorrelation << " -> "
                                 << processedCorrelation;
                             std::fprintf(stderr,
                                          "AEC_VERIFICATION_RESULT delay_ms=%d "
                                          "echo_reduction_db=%.1f raw_echo_rms_dbfs=%.1f "
                                          "processed_echo_rms_dbfs=%.1f correlation=%.4f->%.4f\n",
                                          delayMs,
                                          echoReductionDb,
                                          rawEchoRmsDbfs,
                                          processedEchoRmsDbfs,
                                          rawCorrelation,
                                          processedCorrelation);
                             finishOnce(0);
                         });
        QObject::connect(&worker,
                         &AudioWorker::localAecEffectVerificationFailed,
                         &application,
                         [&](const QString &reason) {
                             qCritical().noquote() << "AEC_VERIFICATION_FAILED" << reason;
                             std::fprintf(stderr,
                                          "AEC_VERIFICATION_FAILED %s\n",
                                          qPrintable(reason));
                             finishOnce(2);
                         });
    } else {
        QObject::connect(&worker,
                         &AudioWorker::localAecDelayCalibrated,
                         &application,
                         [&](int delayMs, double correlation, double captureRmsDbfs) {
                             qInfo().nospace() << "CALIBRATION_RESULT delay_ms=" << delayMs
                                               << " correlation=" << correlation
                                               << " capture_rms_dbfs=" << captureRmsDbfs;
                             std::fprintf(stderr,
                                          "CALIBRATION_RESULT delay_ms=%d correlation=%.4f "
                                          "capture_rms_dbfs=%.1f\n",
                                          delayMs,
                                          correlation,
                                          captureRmsDbfs);
                             finishOnce(0);
                         });
        QObject::connect(&worker,
                         &AudioWorker::localAecDelayCalibrationFailed,
                         &application,
                         [&](const QString &reason) {
                             qCritical().noquote() << "CALIBRATION_FAILED" << reason;
                             std::fprintf(stderr,
                                          "CALIBRATION_FAILED %s\n",
                                          qPrintable(reason));
                             finishOnce(2);
                         });
    }
    QObject::connect(&worker, &AudioWorker::audioError, &application, [&](const QString &message) {
        qCritical().noquote() << "AUDIO_ERROR" << message;
        std::fprintf(stderr, "AUDIO_ERROR %s\n", qPrintable(message));
        finishOnce(3);
    });
    QTimer::singleShot(15000, &application, [&, verifyAecEffect]() {
        const char *result = verifyAecEffect ? "AEC_VERIFICATION_FAILED timeout"
                                             : "CALIBRATION_FAILED timeout";
        qCritical().noquote() << result;
        std::fprintf(stderr, "%s\n", result);
        finishOnce(4);
    });

    worker.configureNetwork(QStringLiteral("127.0.0.1"),
                            QStringLiteral("127.0.0.1"),
                            LocalPort,
                            PeerPort);
    worker.startAudio();
    return application.exec();
}
