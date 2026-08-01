#include "webrtcaudioprocessor.h"

#include <QDebug>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#if defined(VIDEO_CALL_HAVE_WEBRTC_APM)
#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"
#endif

struct WebRtcAudioProcessor::Impl
{
#if defined(VIDEO_CALL_HAVE_WEBRTC_APM)
    webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig streamConfig{SampleRate, Channels};
#endif
};

WebRtcAudioProcessor::WebRtcAudioProcessor() = default;

WebRtcAudioProcessor::~WebRtcAudioProcessor() = default;

bool WebRtcAudioProcessor::initialize()
{
    shutdown();
    m_statistics = {};
    m_statistics.streamDelayMs = DefaultAecStreamDelayMs;

#if defined(VIDEO_CALL_HAVE_WEBRTC_APM)
    m_statistics.backendAvailable = true;
    m_impl = std::make_unique<Impl>();

    webrtc::AudioProcessing::Config config;
    config.echo_canceller.enabled = true; // Built-in desktop echo canceller is AEC3.
    config.high_pass_filter.enabled = true;
    config.noise_suppression.enabled = true;
    config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
    config.gain_controller1.enabled = false;
    config.gain_controller1.analog_gain_controller.enabled = false;
    config.gain_controller2.enabled = false;
    config.gain_controller2.adaptive_digital.enabled = false;

    m_impl->apm = webrtc::BuiltinAudioProcessingBuilder(config).Build(webrtc::CreateEnvironment());
    if (!m_impl->apm) {
        qWarning() << "[WebRtcAudioProcessor] WebRTC APM creation failed; PCM will bypass AEC.";
        m_impl.reset();
        return false;
    }

    const int initializeResult = m_impl->apm->Initialize();
    if (initializeResult != webrtc::AudioProcessing::kNoError) {
        qWarning().nospace()
            << "[WebRtcAudioProcessor] WebRTC APM initialization failed ("
            << initializeResult << "); PCM will bypass AEC.";
        m_impl.reset();
        return false;
    }

    m_statistics.initialized = true;
    m_statistics.aecEnabled = true;
    qInfo() << "[WebRtcAudioProcessor] WebRTC APM/AEC3 initialized (16 kHz mono, delay"
            << DefaultAecStreamDelayMs << "ms).";
    return true;
#else
    qWarning() << "[WebRtcAudioProcessor] WebRTC APM is not configured; PCM will bypass AEC."
               << "Set VIDEO_CALL_WEBRTC_APM_ROOT or VIDEO_CALL_WEBRTC_APM_TARGET.";
    return false;
#endif
}

void WebRtcAudioProcessor::shutdown()
{
    m_impl.reset();
    m_statistics.initialized = false;
    m_statistics.aecEnabled = false;
}

void WebRtcAudioProcessor::reset()
{
#if defined(VIDEO_CALL_HAVE_WEBRTC_APM)
    if (!m_impl || !m_impl->apm) {
        return;
    }

    const int resetResult = m_impl->apm->Initialize();
    if (resetResult != webrtc::AudioProcessing::kNoError) {
        qWarning().nospace()
            << "[WebRtcAudioProcessor] WebRTC APM reset failed ("
            << resetResult << "); PCM will bypass AEC.";
        m_impl.reset();
        m_statistics.initialized = false;
        m_statistics.aecEnabled = false;
    }
#endif
}

bool WebRtcAudioProcessor::isInitialized() const
{
    return m_statistics.initialized;
}

WebRtcAudioProcessor::Statistics WebRtcAudioProcessor::statistics() const
{
    return m_statistics;
}

QByteArray WebRtcAudioProcessor::processRender20Ms(const QByteArray &pcm)
{
    if (!hasValidNetworkFrame(pcm)) {
        countBypassedFrame();
        return pcm;
    }

#if defined(VIDEO_CALL_HAVE_WEBRTC_APM)
    if (!m_statistics.initialized || !m_impl || !m_impl->apm) {
        countBypassedFrame();
        return pcm;
    }

    std::array<QByteArray, 2> frames;
    if (!split20MsFrame(pcm, &frames)) {
        countBypassedFrame();
        return pcm;
    }

    for (const QByteArray &frame : frames) {
        // QByteArray only guarantees byte alignment.  Copy to a fixed int16_t
        // array before passing the frame to WebRTC to avoid unaligned access.
        std::array<int16_t, ApmFrameSamples> inputSamples{};
        std::array<int16_t, ApmFrameSamples> outputSamples{};
        std::memcpy(inputSamples.data(), frame.constData(), ApmFrameBytes);
        const int result = m_impl->apm->ProcessReverseStream(inputSamples.data(),
                                                               m_impl->streamConfig,
                                                               m_impl->streamConfig,
                                                               outputSamples.data());
        if (result != webrtc::AudioProcessing::kNoError) {
            ++m_statistics.renderProcessFailures;
            countBypassedFrame();
            if (m_statistics.renderProcessFailures == 1) {
                qWarning().nospace()
                    << "[WebRtcAudioProcessor] ProcessReverseStream failed ("
                    << result << "); keeping the original render PCM.";
            }
            return pcm;
        }
    }
#else
    countBypassedFrame();
#endif

    // APM's reverse stream is a reference/analysis input.  Do not let a render
    // preprocessor alter the actual sink data, otherwise the reference could
    // diverge from what reaches the speaker.
    return pcm;
}

QByteArray WebRtcAudioProcessor::processCapture20Ms(const QByteArray &pcm, int streamDelayMs)
{
    if (!hasValidNetworkFrame(pcm)) {
        countBypassedFrame();
        return pcm;
    }

#if defined(VIDEO_CALL_HAVE_WEBRTC_APM)
    if (!m_statistics.initialized || !m_impl || !m_impl->apm) {
        countBypassedFrame();
        return pcm;
    }

    std::array<QByteArray, 2> frames;
    if (!split20MsFrame(pcm, &frames)) {
        countBypassedFrame();
        return pcm;
    }

    const int boundedDelayMs = std::clamp(streamDelayMs,
                                          MinimumAecStreamDelayMs,
                                          MaximumAecStreamDelayMs);
    m_statistics.streamDelayMs = boundedDelayMs;
    std::array<QByteArray, 2> processedFrames;
    for (size_t index = 0; index < frames.size(); ++index) {
        // WebRTC requires the delay setting before every ProcessStream call.
        const int delayResult = m_impl->apm->set_stream_delay_ms(boundedDelayMs);
        if (delayResult != webrtc::AudioProcessing::kNoError) {
            ++m_statistics.captureProcessFailures;
            countBypassedFrame();
            if (m_statistics.captureProcessFailures == 1) {
                qWarning().nospace()
                    << "[WebRtcAudioProcessor] set_stream_delay_ms failed ("
                    << delayResult << "); keeping the original capture PCM.";
            }
            return pcm;
        }

        std::array<int16_t, ApmFrameSamples> inputSamples{};
        std::array<int16_t, ApmFrameSamples> outputSamples{};
        std::memcpy(inputSamples.data(), frames[index].constData(), ApmFrameBytes);
        const int result = m_impl->apm->ProcessStream(inputSamples.data(),
                                                        m_impl->streamConfig,
                                                        m_impl->streamConfig,
                                                        outputSamples.data());
        if (result != webrtc::AudioProcessing::kNoError) {
            ++m_statistics.captureProcessFailures;
            countBypassedFrame();
            if (m_statistics.captureProcessFailures == 1) {
                qWarning().nospace()
                    << "[WebRtcAudioProcessor] ProcessStream failed ("
                    << result << "); keeping the original capture PCM.";
            }
            return pcm;
        }

        processedFrames[index] = QByteArray(ApmFrameBytes, '\0');
        std::memcpy(processedFrames[index].data(), outputSamples.data(), ApmFrameBytes);
    }
    return join10MsFrames(processedFrames);
#else
    Q_UNUSED(streamDelayMs);
    countBypassedFrame();
    return pcm;
#endif
}

bool WebRtcAudioProcessor::split20MsFrame(const QByteArray &pcm,
                                           std::array<QByteArray, 2> *apmFrames)
{
    if (!apmFrames || pcm.size() != NetworkFrameBytes) {
        return false;
    }

    (*apmFrames)[0] = pcm.first(ApmFrameBytes);
    (*apmFrames)[1] = pcm.mid(ApmFrameBytes, ApmFrameBytes);
    return true;
}

QByteArray WebRtcAudioProcessor::join10MsFrames(const std::array<QByteArray, 2> &apmFrames)
{
    if (apmFrames[0].size() != ApmFrameBytes || apmFrames[1].size() != ApmFrameBytes) {
        return {};
    }

    QByteArray joined;
    joined.reserve(NetworkFrameBytes);
    joined.append(apmFrames[0]);
    joined.append(apmFrames[1]);
    return joined;
}

bool WebRtcAudioProcessor::hasValidNetworkFrame(const QByteArray &pcm) const
{
    return pcm.size() == NetworkFrameBytes;
}

void WebRtcAudioProcessor::countBypassedFrame()
{
    ++m_statistics.bypassedFrames;
}
