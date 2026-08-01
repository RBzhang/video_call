#ifndef WEBRTCAUDIOPROCESSOR_H
#define WEBRTCAUDIOPROCESSOR_H

#include <QByteArray>
#include <QtGlobal>

#include <array>
#include <memory>

// This class is deliberately not a QObject.  AudioWorker owns one instance and
// calls it only from its audio thread, so no extra processing thread or queued
// audio copies are introduced.
class WebRtcAudioProcessor
{
public:
    static constexpr int SampleRate = 16000;
    static constexpr int Channels = 1;
    static constexpr int NetworkFrameSamples = 320;
    static constexpr int ApmFrameSamples = 160;
    static constexpr int NetworkFrameBytes = 640;
    static constexpr int ApmFrameBytes = 320;
    // Measured on the current Realtek speaker/microphone path: 200, 203, 204
    // and 209 ms across independent calibration passes.  The rounded median
    // is a stable starting point; the runtime calibration can refine it.
    static constexpr int DefaultAecStreamDelayMs = 204;
    static constexpr int MinimumAecStreamDelayMs = 0;
    static constexpr int MaximumAecStreamDelayMs = 500;

    struct Statistics
    {
        bool backendAvailable = false;
        bool initialized = false;
        bool aecEnabled = false;
        int streamDelayMs = DefaultAecStreamDelayMs;
        quint64 renderProcessFailures = 0;
        quint64 captureProcessFailures = 0;
        quint64 bypassedFrames = 0;
    };

    WebRtcAudioProcessor();
    ~WebRtcAudioProcessor();

    WebRtcAudioProcessor(const WebRtcAudioProcessor &) = delete;
    WebRtcAudioProcessor &operator=(const WebRtcAudioProcessor &) = delete;

    bool initialize();
    void shutdown();
    void reset();

    bool isInitialized() const;
    Statistics statistics() const;

    // ProcessReverseStream is called for each 10 ms chunk.  Its output is not
    // used for playback: the bytes passed to APM remain the exact bytes passed
    // to QAudioSink, which keeps the AEC render reference truthful.
    QByteArray processRender20Ms(const QByteArray &pcm);
    QByteArray processCapture20Ms(const QByteArray &pcm, int streamDelayMs);

    // Kept public and independent of WebRTC so frame handling can be tested on
    // machines that do not have a local APM build installed.
    static bool split20MsFrame(const QByteArray &pcm,
                               std::array<QByteArray, 2> *apmFrames);
    static QByteArray join10MsFrames(const std::array<QByteArray, 2> &apmFrames);

private:
    struct Impl;

    bool hasValidNetworkFrame(const QByteArray &pcm) const;
    void countBypassedFrame();

    std::unique_ptr<Impl> m_impl;
    Statistics m_statistics;
};

#endif // WEBRTCAUDIOPROCESSOR_H
