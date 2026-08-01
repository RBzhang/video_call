#include "acousticdelayestimator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr int BytesPerSample = static_cast<int>(sizeof(qint16));
constexpr int AnalysisStartMs = 1000;
constexpr int AnalysisDurationMs = 4000;
constexpr int AnalysisStrideSamples = 4;
constexpr double SilenceDbfs = -96.0;
constexpr double MinimumCorrelation = 0.12;
constexpr double MinimumCaptureRmsDbfs = -60.0;

std::vector<qint16> toSamples(const QByteArray &pcm)
{
    if (pcm.size() % BytesPerSample != 0) {
        return {};
    }

    std::vector<qint16> samples(static_cast<size_t>(pcm.size() / BytesPerSample));
    for (size_t index = 0; index < samples.size(); ++index) {
        std::memcpy(&samples[index],
                    pcm.constData() + static_cast<qsizetype>(index * BytesPerSample),
                    BytesPerSample);
    }
    return samples;
}

struct AnalysisWindow
{
    bool valid = false;
    size_t renderStart = 0;
    size_t captureStart = 0;
    size_t sampleCount = 0;
};

AnalysisWindow makeAnalysisWindow(const std::vector<qint16> &render,
                                  const std::vector<qint16> &capture,
                                  qint64 captureStartOffsetMs,
                                  int delayMs)
{
    const size_t analysisStartSamples = static_cast<size_t>(
        AnalysisStartMs * AcousticDelayEstimator::SampleRate / 1000);
    const size_t analysisSamples = static_cast<size_t>(
        AnalysisDurationMs * AcousticDelayEstimator::SampleRate / 1000);
    if (render.size() < analysisStartSamples + analysisSamples
        || capture.size() < analysisStartSamples + analysisSamples) {
        return {};
    }

    const qint64 alignmentSamples = static_cast<qint64>(delayMs - captureStartOffsetMs)
        * AcousticDelayEstimator::SampleRate / 1000;
    const size_t renderStart = analysisStartSamples
        + (alignmentSamples < 0 ? static_cast<size_t>(-alignmentSamples) : 0U);
    const size_t captureStart = analysisStartSamples
        + (alignmentSamples > 0 ? static_cast<size_t>(alignmentSamples) : 0U);
    if (renderStart + analysisSamples > render.size()
        || captureStart + analysisSamples > capture.size()) {
        return {};
    }
    return {true, renderStart, captureStart, analysisSamples};
}

double toDbfs(double rms)
{
    const double normalizedRms = rms / 32768.0;
    return normalizedRms > 0.0 ? std::max(SilenceDbfs, 20.0 * std::log10(normalizedRms))
                               : SilenceDbfs;
}

AcousticDelayEstimator::EchoMetrics measureWindow(const std::vector<qint16> &render,
                                                   const std::vector<qint16> &capture,
                                                   const AnalysisWindow &window)
{
    AcousticDelayEstimator::EchoMetrics result;
    if (!window.valid) {
        return result;
    }

    double dotProduct = 0.0;
    double renderEnergy = 0.0;
    double captureEnergy = 0.0;
    size_t sampleCount = 0;
    for (size_t offset = 0; offset < window.sampleCount; offset += AnalysisStrideSamples) {
        const double renderSample = render[window.renderStart + offset];
        const double captureSample = capture[window.captureStart + offset];
        dotProduct += renderSample * captureSample;
        renderEnergy += renderSample * renderSample;
        captureEnergy += captureSample * captureSample;
        ++sampleCount;
    }
    if (sampleCount == 0 || renderEnergy <= 0.0 || captureEnergy <= 0.0) {
        return result;
    }
    result.valid = true;
    result.normalizedCorrelation = dotProduct / std::sqrt(renderEnergy * captureEnergy);
    result.captureRmsDbfs = toDbfs(std::sqrt(captureEnergy / static_cast<double>(sampleCount)));
    const double renderCorrelatedRms = std::abs(dotProduct)
        / std::sqrt(renderEnergy * static_cast<double>(sampleCount));
    result.renderCorrelatedRmsDbfs = toDbfs(renderCorrelatedRms);
    return result;
}

double rmsDbfs(const std::vector<qint16> &samples)
{
    if (samples.empty()) {
        return SilenceDbfs;
    }

    double sumSquares = 0.0;
    for (const qint16 sample : samples) {
        const double value = sample;
        sumSquares += value * value;
    }
    return toDbfs(std::sqrt(sumSquares / static_cast<double>(samples.size())));
}

} // namespace

AcousticDelayEstimator::Result AcousticDelayEstimator::estimate(const QByteArray &renderPcm,
                                                                 const QByteArray &capturePcm,
                                                                 qint64 captureStartOffsetMs,
                                                                 int maximumDelayMs)
{
    Result result;
    const std::vector<qint16> render = toSamples(renderPcm);
    const std::vector<qint16> capture = toSamples(capturePcm);
    result.captureRmsDbfs = rmsDbfs(capture);

    const int boundedMaximumDelayMs = std::clamp(maximumDelayMs, 0, MaximumDelayMs);
    double bestCorrelation = -std::numeric_limits<double>::infinity();
    int bestDelayMs = 0;
    for (int delayMs = 0; delayMs <= boundedMaximumDelayMs; ++delayMs) {
        // The QAudioSource and QAudioSink callbacks begin at different wall
        // times and each side has its own driver buffer.  Account for that
        // measured callback offset before comparing waveform sample indices.
        const AnalysisWindow window = makeAnalysisWindow(render,
                                                         capture,
                                                         captureStartOffsetMs,
                                                         delayMs);
        const EchoMetrics metrics = measureWindow(render, capture, window);
        if (!metrics.valid) {
            continue;
        }
        if (metrics.normalizedCorrelation > bestCorrelation) {
            bestCorrelation = metrics.normalizedCorrelation;
            bestDelayMs = delayMs;
        }
    }

    result.delayMs = bestDelayMs;
    result.waveformAlignmentMs = bestDelayMs - static_cast<int>(captureStartOffsetMs);
    result.normalizedCorrelation = std::isfinite(bestCorrelation) ? bestCorrelation : 0.0;
    // Over the 4 s / 4x downsampled analysis window, a 0.12 matched-signal
    // correlation is well above a silent or uncorrelated microphone input,
    // while still accommodating Realtek driver processing and independent
    // capture/render clocks observed on the default device.
    result.valid = result.normalizedCorrelation >= MinimumCorrelation
        && result.captureRmsDbfs >= MinimumCaptureRmsDbfs;
    return result;
}

AcousticDelayEstimator::EchoMetrics AcousticDelayEstimator::measureAtDelay(
    const QByteArray &renderPcm,
    const QByteArray &capturePcm,
    qint64 captureStartOffsetMs,
    int delayMs)
{
    const std::vector<qint16> render = toSamples(renderPcm);
    const std::vector<qint16> capture = toSamples(capturePcm);
    const int boundedDelayMs = std::clamp(delayMs, 0, MaximumDelayMs);
    return measureWindow(render,
                         capture,
                         makeAnalysisWindow(render,
                                            capture,
                                            captureStartOffsetMs,
                                            boundedDelayMs));
}
