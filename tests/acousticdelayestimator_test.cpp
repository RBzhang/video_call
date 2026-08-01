#include "acousticdelayestimator.h"

#include <QDebug>

#include <cmath>
#include <cstring>

namespace {

bool expect(bool condition, const QString &message)
{
    if (!condition) {
        qCritical().noquote() << QStringLiteral("[acousticdelayestimator_test]") << message;
    }
    return condition;
}

QByteArray makeKnownSignal(int samples)
{
    QByteArray pcm(samples * static_cast<int>(sizeof(qint16)), '\0');
    for (int index = 0; index < samples; ++index) {
        const double time = static_cast<double>(index) / AcousticDelayEstimator::SampleRate;
        const double value = 0.35 * 32767.0
            * (0.55 * std::sin(2.0 * 3.14159265358979323846 * 347.0 * time)
               + 0.30 * std::sin(2.0 * 3.14159265358979323846 * 911.0 * time)
               + 0.15 * std::sin(2.0 * 3.14159265358979323846 * 1843.0 * time));
        const qint16 sample = static_cast<qint16>(std::lround(value));
        std::memcpy(pcm.data() + index * static_cast<int>(sizeof(sample)),
                    &sample,
                    sizeof(sample));
    }
    return pcm;
}

bool testKnownDelay()
{
    constexpr int expectedDelayMs = 137;
    const QByteArray render = makeKnownSignal(AcousticDelayEstimator::SampleRate * 6);
    QByteArray capture(expectedDelayMs * AcousticDelayEstimator::SampleRate / 1000
                           * static_cast<int>(sizeof(qint16)),
                       '\0');
    capture.append(render);
    capture.append(AcousticDelayEstimator::MaximumDelayMs
                       * AcousticDelayEstimator::SampleRate / 1000
                       * static_cast<int>(sizeof(qint16)),
                   '\0');

    const AcousticDelayEstimator::Result result = AcousticDelayEstimator::estimate(render, capture);
    return expect(result.valid, QStringLiteral("有效的回声副本未通过相关性校验。"))
        && expect(result.delayMs == expectedDelayMs,
                  QStringLiteral("估算延时错误：期望 %1 ms，实际 %2 ms。")
                      .arg(expectedDelayMs)
                      .arg(result.delayMs))
        && expect(result.normalizedCorrelation > 0.99,
                  QStringLiteral("已知副本的相关性过低：%1。")
                      .arg(result.normalizedCorrelation, 0, 'f', 3));
}

bool testCallbackOffsetIsCompensated()
{
    constexpr int expectedDelayMs = 173;
    constexpr int captureStartOffsetMs = 90;
    constexpr int waveformAlignmentMs = expectedDelayMs - captureStartOffsetMs;
    const QByteArray render = makeKnownSignal(AcousticDelayEstimator::SampleRate * 6);
    QByteArray capture(waveformAlignmentMs * AcousticDelayEstimator::SampleRate / 1000
                           * static_cast<int>(sizeof(qint16)),
                       '\0');
    capture.append(render);
    capture.append(AcousticDelayEstimator::MaximumDelayMs
                       * AcousticDelayEstimator::SampleRate / 1000
                       * static_cast<int>(sizeof(qint16)),
                   '\0');

    const AcousticDelayEstimator::Result result = AcousticDelayEstimator::estimate(
        render, capture, captureStartOffsetMs);
    return expect(result.valid, QStringLiteral("带回调偏移的有效回声副本未通过相关性校验。"))
        && expect(result.delayMs == expectedDelayMs,
                  QStringLiteral("补偿回调偏移后延时错误：期望 %1 ms，实际 %2 ms。")
                      .arg(expectedDelayMs)
                      .arg(result.delayMs));
}

bool testEchoMetricsAtKnownDelay()
{
    constexpr int expectedDelayMs = 121;
    const QByteArray render = makeKnownSignal(AcousticDelayEstimator::SampleRate * 6);
    QByteArray capture(expectedDelayMs * AcousticDelayEstimator::SampleRate / 1000
                           * static_cast<int>(sizeof(qint16)),
                       '\0');
    capture.append(render);
    capture.append(AcousticDelayEstimator::MaximumDelayMs
                       * AcousticDelayEstimator::SampleRate / 1000
                       * static_cast<int>(sizeof(qint16)),
                   '\0');

    const AcousticDelayEstimator::EchoMetrics metrics =
        AcousticDelayEstimator::measureAtDelay(render, capture, 0, expectedDelayMs);
    return expect(metrics.valid, QStringLiteral("已知回声的指标不可用。"))
        && expect(metrics.normalizedCorrelation > 0.99,
                  QStringLiteral("已知回声的相关性过低：%1。")
                      .arg(metrics.normalizedCorrelation, 0, 'f', 3))
        && expect(metrics.renderCorrelatedRmsDbfs > -20.0,
                  QStringLiteral("已知回声的相关分量过低：%1 dBFS。")
                      .arg(metrics.renderCorrelatedRmsDbfs, 0, 'f', 1));
}

bool testSilenceIsRejected()
{
    const QByteArray silence(AcousticDelayEstimator::SampleRate * 7
                                 * static_cast<int>(sizeof(qint16)),
                             '\0');
    const AcousticDelayEstimator::Result result = AcousticDelayEstimator::estimate(silence, silence);
    return expect(!result.valid, QStringLiteral("静音不应产生有效的声学延时。"));
}

} // namespace

int main()
{
    return testKnownDelay() && testCallbackOffsetIsCompensated() && testEchoMetricsAtKnownDelay()
        && testSilenceIsRejected() ? 0 : 1;
}
