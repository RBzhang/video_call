#ifndef ACOUSTICDELAYESTIMATOR_H
#define ACOUSTICDELAYESTIMATOR_H

#include <QByteArray>
#include <QtGlobal>

// Estimates the delay between PCM handed to the render side and the acoustic
// copy captured by the microphone.  It is intentionally independent from
// WebRTC so the calibration path can be tested on machines without an APM
// library.
class AcousticDelayEstimator
{
public:
    static constexpr int SampleRate = 16000;
    static constexpr int MaximumDelayMs = 500;

    struct Result
    {
        bool valid = false;
        int delayMs = 0;
        int waveformAlignmentMs = 0;
        double normalizedCorrelation = 0.0;
        double captureRmsDbfs = -96.0;
    };

    // Metrics for the component in the capture stream that is linearly
    // correlated with the known render signal at one specified delay.  This
    // lets the local diagnostic compare microphone PCM before and after APM
    // without ever routing the microphone back to the speaker.
    struct EchoMetrics
    {
        bool valid = false;
        double normalizedCorrelation = 0.0;
        double captureRmsDbfs = -96.0;
        double renderCorrelatedRmsDbfs = -96.0;
    };

    // `renderPcm` must begin when the diagnostic signal was submitted to the
    // audio output path. `capturePcm` must begin immediately afterwards and
    // include at least `maximumDelayMs` of post-roll audio.
    static Result estimate(const QByteArray &renderPcm,
                           const QByteArray &capturePcm,
                           qint64 captureStartOffsetMs = 0,
                           int maximumDelayMs = MaximumDelayMs);

    static EchoMetrics measureAtDelay(const QByteArray &renderPcm,
                                      const QByteArray &capturePcm,
                                      qint64 captureStartOffsetMs,
                                      int delayMs);
};

#endif // ACOUSTICDELAYESTIMATOR_H
