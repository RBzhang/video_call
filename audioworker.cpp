#include "audioworker.h"

#include "audiolooppolicy.h"
#include "audiopacketprotocol.h"
#include "audioudptransport.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QAbstractSocket>
#include <QDebug>
#include <QHostAddress>
#include <QIODevice>
#include <QMediaDevices>
#include <QRandomGenerator>
#include <QTimer>
#include <QThread>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr int AudioPacketIntervalMs = 20;
constexpr qsizetype CaptureBufferLimit = AudioPacketProtocol::PcmPayloadSize * 10;
constexpr qsizetype PlaybackBufferLimit = AudioPacketProtocol::PcmPayloadSize * 5;
constexpr qsizetype RequestedSinkBufferSize = AudioPacketProtocol::PcmPayloadSize * 4;
constexpr int DefaultAecStreamDelayMs = WebRtcAudioProcessor::DefaultAecStreamDelayMs;
constexpr int PlaybackVolumeUpdateIntervalMs = 100;
constexpr double Int16FullScale = 32768.0;
constexpr double SilenceDbfs = -96.0;
constexpr int LocalAecTestDurationMs = 10000;
constexpr int LocalAecTestPacketCount = LocalAecTestDurationMs / AudioPacketIntervalMs;
constexpr int LocalAecTestPostRollMs = 500;
constexpr int LocalAecTestPostRollPacketCount = LocalAecTestPostRollMs / AudioPacketIntervalMs;
constexpr double LocalAecTestAmplitude = 0.12 * 32767.0; // About -21 dBFS RMS.
constexpr double LocalAecTestSweepStartHz = 300.0;
constexpr double LocalAecTestSweepEndHz = 3000.0;
constexpr double TwoPi = 6.28318530717958647692;

QAudioFormat fixedAudioFormat()
{
    QAudioFormat format;
    format.setSampleRate(AudioPacketProtocol::SampleRate);
    format.setChannelCount(AudioPacketProtocol::Channels);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

std::atomic_int g_audioWorkerDestructionCount{0};

} // namespace

AudioWorker::AudioWorker(QObject *parent)
    : QObject(parent)
    , m_transport(new AudioUdpTransport(this))
{
    connect(m_transport,
            &AudioUdpTransport::audioPacketReceived,
            this,
            &AudioWorker::onAudioPacketReceived);
    connect(m_transport,
            &AudioUdpTransport::datagramRejected,
            this,
            &AudioWorker::onAudioDatagramRejected);
    connect(m_transport,
            &AudioUdpTransport::foreignDatagramDropped,
            this,
            &AudioWorker::onForeignDatagramDropped);
    connect(m_transport, &AudioUdpTransport::networkError, this, &AudioWorker::reportError);
}

AudioWorker::~AudioWorker()
{
    shutdown();
    const int destructionCount = ++g_audioWorkerDestructionCount;
    qInfo().nospace() << "[AudioWorker] destroyed #" << destructionCount
                      << " this=" << static_cast<const void *>(this)
                      << " currentThreadId=" << QThread::currentThreadId()
                      << " objectThread=" << thread()
                      << " objectThreadRunning=" << (thread() && thread()->isRunning());
}

int AudioWorker::destructionCount()
{
    return g_audioWorkerDestructionCount.load();
}

void AudioWorker::resetDestructionCount()
{
    g_audioWorkerDestructionCount.store(0);
}

void AudioWorker::configureNetwork(const QString &localAddress,
                                   const QString &peerAddress,
                                   quint16 localPort,
                                   quint16 peerPort)
{
    if (m_shutdown) {
        return;
    }
    stopAudioInternal(true);
    m_networkReady = false;
    m_jitterBuffer.clear();
    if (!m_transport) {
        reportError(QStringLiteral("音频 UDP Transport 不可用。"));
        return;
    }

    QHostAddress local;
    if (!local.setAddress(localAddress.trimmed())
        || local.protocol() != QAbstractSocket::IPv4Protocol) {
        m_transport->close();
        reportError(QStringLiteral("音频本地 IPv4 地址无效。"));
        return;
    }

    QHostAddress peer;
    if (!peer.setAddress(peerAddress.trimmed())
        || peer.protocol() != QAbstractSocket::IPv4Protocol) {
        m_transport->close();
        reportError(QStringLiteral("音频对端 IPv4 地址无效。"));
        return;
    }

    m_transport->close();
    m_transport->configurePeer(peer, peerPort);
    QString bindError;
    if (!m_transport->bindReceiver(local, localPort, &bindError)) {
        reportError(QStringLiteral("音频网络配置失败：%1").arg(bindError));
        return;
    }

    m_networkReady = true;
    emit audioNetworkReady(
        QStringLiteral("音频：已绑定 %1:%2 → %3:%4")
            .arg(local.toString())
            .arg(m_transport->localPort())
            .arg(peer.toString())
            .arg(peerPort));
}

void AudioWorker::startAudio()
{
    if (m_shutdown || m_running) {
        return;
    }
    if (!m_networkReady || !m_transport || !m_transport->isBound()) {
        reportError(QStringLiteral("音频网络尚未配置。"));
        return;
    }

    const QAudioDevice inputDevice = QMediaDevices::defaultAudioInput();
    const QAudioDevice outputDevice = QMediaDevices::defaultAudioOutput();
    if (inputDevice.isNull()) {
        reportError(QStringLiteral("未找到默认音频输入设备。"));
        return;
    }
    if (outputDevice.isNull()) {
        reportError(QStringLiteral("未找到默认音频输出设备。"));
        return;
    }

    const QAudioFormat format = fixedAudioFormat();
    if (!inputDevice.isFormatSupported(format)) {
        reportError(fixedFormatErrorMessage(QStringLiteral("输入"), inputDevice));
        return;
    }
    if (!outputDevice.isFormatSupported(format)) {
        reportError(fixedFormatErrorMessage(QStringLiteral("输出"), outputDevice));
        return;
    }

    // Failure is non-fatal: WebRtcAudioProcessor safely returns original PCM
    // until a local WebRTC APM library is configured and initialized.
    m_audioProcessor.initialize();

    m_inputDeviceDescription = inputDevice.description();
    m_outputDeviceDescription = outputDevice.description();
    m_audioSource = new QAudioSource(inputDevice, format, this);
    m_audioSink = new QAudioSink(outputDevice, format, this);
    m_audioSink->setBufferSize(RequestedSinkBufferSize);

    connect(m_audioSource,
            &QAudioSource::stateChanged,
            this,
            &AudioWorker::onSourceStateChanged);
    connect(m_audioSink,
            &QAudioSink::stateChanged,
            this,
            &AudioWorker::onSinkStateChanged);

    m_outputDevice = m_audioSink->start();
    if (!m_outputDevice) {
        reportError(QStringLiteral("默认音频输出设备启动失败。"));
        stopAudioInternal(false);
        return;
    }
    m_inputDevice = m_audioSource->start();
    if (!m_inputDevice) {
        reportError(QStringLiteral("默认音频输入设备启动失败。"));
        stopAudioInternal(false);
        return;
    }
    connect(m_inputDevice, &QIODevice::readyRead, this, &AudioWorker::onInputReadyRead);

    m_sourceBufferSize = m_audioSource->bufferSize();
    m_sinkBufferSize = m_audioSink->bufferSize();
    m_captureBuffer.clear();
    m_playbackBuffer.clear();
    resetPlaybackVolume();
    m_jitterBuffer.clear();
    m_jitterBuffer.resetStatistics();
    m_sendSessionId = QRandomGenerator::global()->generate();
    m_sendSessionId = nextNonZero(m_sendSessionId);
    m_nextSequence = 1;
    m_nextTimestampSamples = 0;
    m_aecStreamDelayMs = DefaultAecStreamDelayMs;
    m_localAecTestActive = false;
    m_localAecEffectVerificationActive = false;
    m_localAecTestPacketsRemaining = 0;
    m_localAecTestPostRollPacketsRemaining = 0;
    m_localAecTestSamplePosition = 0;
    m_localAecTestElapsedTimer.invalidate();
    m_localAecTestRenderStartElapsedMs = -1;
    m_localAecTestCaptureStartElapsedMs = -1;
    m_localAecTestRenderPcm.clear();
    m_localAecTestCapturePcm.clear();
    m_localAecTestProcessedCapturePcm.clear();
    resetIntervalStatistics();
    ensureTimers();
    m_statisticsElapsedTimer.restart();
    m_playoutTimer->start(AudioPacketIntervalMs);
    m_playbackVolumeTimer->start(PlaybackVolumeUpdateIntervalMs);
    m_statisticsTimer->start(1000);
    m_running = true;

    emit audioStarted(
        QStringLiteral("音频：已启动｜输入：%1｜输出：%2｜16 kHz / 单声道 / Int16｜Source bufferSize=%3｜Sink bufferSize=%4")
            .arg(m_inputDeviceDescription,
                 m_outputDeviceDescription)
            .arg(m_sourceBufferSize)
            .arg(m_sinkBufferSize));
}

void AudioWorker::stopAudio()
{
    if (m_shutdown) {
        return;
    }
    stopAudioInternal(true);
}

void AudioWorker::playLocalAecTestTone()
{
    startLocalAecTest(false);
}

void AudioWorker::verifyLocalAecEffect()
{
    if (!m_running || !m_outputDevice || m_localAecTestActive) {
        return;
    }

    if (!m_audioProcessor.isInitialized()) {
        emit localAecEffectVerificationFailed(
            QStringLiteral("WebRTC APM/AEC3 未初始化，无法验证回声消除效果。"));
        return;
    }

    startLocalAecTest(true);
}

void AudioWorker::startLocalAecTest(bool verifyEffect)
{
    if (!m_running || !m_outputDevice || m_localAecTestActive) {
        return;
    }

    // The test source replaces network playout for its fixed 10 s duration.
    // Self-looped microphone packets are discarded in onAudioPacketReceived()
    // so a failed AEC setup cannot turn this diagnostic signal into feedback.
    m_jitterBuffer.clear();
    m_playbackBuffer.clear();
    m_captureBuffer.clear();
    resetPlaybackVolume();
    m_audioProcessor.reset();
    m_localAecTestActive = true;
    m_localAecEffectVerificationActive = verifyEffect;
    m_localAecTestPacketsRemaining = LocalAecTestPacketCount;
    m_localAecTestPostRollPacketsRemaining = LocalAecTestPostRollPacketCount;
    m_localAecTestSamplePosition = 0;
    m_localAecTestElapsedTimer.start();
    m_localAecTestRenderStartElapsedMs = -1;
    m_localAecTestCaptureStartElapsedMs = -1;
    m_localAecTestRenderPcm.clear();
    m_localAecTestCapturePcm.clear();
    m_localAecTestProcessedCapturePcm.clear();
    emit localAecTestToneStateChanged(true);
    if (verifyEffect) {
        emit localAecEffectVerificationStateChanged(true);
    }
}

void AudioWorker::shutdown()
{
    if (m_shutdown) {
        return;
    }
    m_shutdown = true;
    stopAudioInternal(false);
    if (m_playoutTimer) {
        m_playoutTimer->stop();
    }
    if (m_playbackVolumeTimer) {
        m_playbackVolumeTimer->stop();
    }
    if (m_statisticsTimer) {
        m_statisticsTimer->stop();
    }
    if (m_transport) {
        m_transport->close();
    }
    m_networkReady = false;
    m_jitterBuffer.clear();
}

void AudioWorker::onInputReadyRead()
{
    if (!m_running || !m_inputDevice || !m_transport) {
        return;
    }

    m_captureBuffer.append(m_inputDevice->readAll());
    if (m_captureBuffer.size() > CaptureBufferLimit) {
        const qsizetype excess = m_captureBuffer.size() - CaptureBufferLimit;
        const qsizetype packetsToDrop = (excess + AudioPacketProtocol::PcmPayloadSize - 1)
            / AudioPacketProtocol::PcmPayloadSize;
        const qsizetype bytesToDrop = qMin(
            packetsToDrop * AudioPacketProtocol::PcmPayloadSize,
            static_cast<qsizetype>(m_captureBuffer.size()));
        m_captureBuffer.remove(0, bytesToDrop);
        m_captureOverruns += static_cast<quint64>(packetsToDrop);
    }

    while (m_captureBuffer.size() >= AudioPacketProtocol::PcmPayloadSize) {
        const QByteArray payload = m_captureBuffer.first(AudioPacketProtocol::PcmPayloadSize);
        m_captureBuffer.remove(0, AudioPacketProtocol::PcmPayloadSize);
        if (m_localAecTestActive) {
            // Do not send diagnostic capture back through UDP.  Besides
            // preventing acoustic feedback, this keeps the measurement free
            // from a second render path with an unknown network delay.
            collectLocalAecTestCapture(payload);
            continue;
        }

        const QByteArray processedPayload = m_audioProcessor.processCapture20Ms(
            payload, m_aecStreamDelayMs);
        QString sendError;
        if (m_transport->sendAudioPayload(processedPayload,
                                          m_sendSessionId,
                                          m_nextSequence,
                                          m_nextTimestampSamples,
                                          &sendError)) {
            ++m_sentPacketsInterval;
            m_sentBytesInterval += static_cast<quint64>(processedPayload.size());
        } else {
            reportError(QStringLiteral("发送 PCM 音频包失败：%1").arg(sendError));
        }
        m_nextSequence = nextNonZero(m_nextSequence + 1);
        m_nextTimestampSamples += AudioPacketProtocol::SamplesPerChannel;
    }
}

void AudioWorker::onAudioPacketReceived(const QByteArray &pcmPayload,
                                        quint32 sessionId,
                                        quint32 sequence,
                                        quint32 timestampSamples,
                                        const QHostAddress &senderAddress,
                                        quint16 senderPort)
{
    Q_UNUSED(timestampSamples);

    if (!m_running) {
        return;
    }
    const bool isSoftwareSelfLoopback = m_transport
        && AudioLoopPolicy::isSoftwareSelfLoopback(senderAddress,
                                                    senderPort,
                                                    m_transport->localAddress(),
                                                    m_transport->localPort());
    if (sessionId == m_sendSessionId && m_sendSessionId != 0 && isSoftwareSelfLoopback) {
        // Reject only a packet returned by this very UDP endpoint.  An FPGA
        // reflector preserves the ACL1 session id, but returns the packet
        // from its own configured peer endpoint and must be rendered.
        ++m_localLoopbackPackets;
        return;
    }
    if (m_localAecTestActive) {
        return;
    }
    ++m_receivedPacketsInterval;
    m_receivedBytesInterval += static_cast<quint64>(pcmPayload.size());
    m_jitterBuffer.insertPacket(sessionId, sequence, pcmPayload);
}

void AudioWorker::onAudioDatagramRejected(const QString &message)
{
    ++m_invalidPackets;
    qWarning().noquote() << QStringLiteral("[AudioWorker] 拒绝 ACL1 数据报：") << message;
}

void AudioWorker::onForeignDatagramDropped(const QHostAddress &senderAddress, quint16 senderPort)
{
    ++m_foreignPackets;
    qWarning().noquote()
        << QStringLiteral("[AudioWorker] 丢弃非配置音频来源：%1:%2")
               .arg(senderAddress.toString())
               .arg(senderPort);
}

void AudioWorker::onSourceStateChanged(QtAudio::State state)
{
    if (!m_audioSource || m_stopping || state != QtAudio::StoppedState
        || m_audioSource->error() == QtAudio::NoError) {
        return;
    }
    const int error = static_cast<int>(m_audioSource->error());
    stopAudioInternal(true);
    reportError(QStringLiteral("音频输入停止，错误枚举值：%1。").arg(error));
}

void AudioWorker::onSinkStateChanged(QtAudio::State state)
{
    if (!m_audioSink || m_stopping || state != QtAudio::StoppedState
        || m_audioSink->error() == QtAudio::NoError) {
        return;
    }
    const int error = static_cast<int>(m_audioSink->error());
    stopAudioInternal(true);
    reportError(QStringLiteral("音频输出停止，错误枚举值：%1。").arg(error));
}

void AudioWorker::onPlayoutTimer()
{
    if (!m_running || !m_outputDevice) {
        return;
    }

    QByteArray packet;
    if (m_localAecTestActive && m_localAecTestPacketsRemaining > 0) {
        packet = nextLocalAecTestPacket();
        --m_localAecTestPacketsRemaining;
        if (m_localAecTestRenderPcm.isEmpty() && m_localAecTestElapsedTimer.isValid()) {
            m_localAecTestRenderStartElapsedMs = m_localAecTestElapsedTimer.elapsed();
        }
        m_localAecTestRenderPcm.append(packet);
    } else if (!m_localAecTestActive) {
        packet = m_jitterBuffer.takeNextPacket();
    }
    if (!packet.isEmpty()) {
        // This is intentionally after jitter-buffer scheduling.  Concealment
        // packets are valid 640-byte silence frames and become AEC references.
        appendPlaybackPacket(m_audioProcessor.processRender20Ms(packet));
    }
    drainPlaybackBuffer();
}

void AudioWorker::onStatisticsTimer()
{
    if (!m_running) {
        return;
    }

    const qint64 elapsedMs = m_statisticsElapsedTimer.isValid()
        ? m_statisticsElapsedTimer.restart()
        : 0;
    if (elapsedMs <= 0) {
        return;
    }

    const AudioJitterBuffer::Statistics jitterStatistics = m_jitterBuffer.statistics();
    AudioStatistics statistics;
    statistics.sentPacketsPerSecond = static_cast<double>(m_sentPacketsInterval) * 1000.0
        / static_cast<double>(elapsedMs);
    statistics.receivedPacketsPerSecond = static_cast<double>(m_receivedPacketsInterval) * 1000.0
        / static_cast<double>(elapsedMs);
    statistics.sentPayloadMegabitsPerSecond = static_cast<double>(m_sentBytesInterval) * 8.0
        / (static_cast<double>(elapsedMs) * 1000.0);
    statistics.receivedPayloadMegabitsPerSecond = static_cast<double>(m_receivedBytesInterval) * 8.0
        / (static_cast<double>(elapsedMs) * 1000.0);
    statistics.jitterBufferedPackets = jitterStatistics.currentBufferedPackets;
    statistics.jitterBufferedMilliseconds = jitterStatistics.currentBufferedPackets * AudioPacketIntervalMs;
    statistics.concealedPackets = jitterStatistics.concealedPackets;
    statistics.duplicatePackets = jitterStatistics.duplicatePackets;
    statistics.latePackets = jitterStatistics.latePackets;
    statistics.foreignPackets = m_foreignPackets;
    statistics.invalidPackets = m_invalidPackets;
    statistics.captureOverruns = m_captureOverruns;
    statistics.playbackOverruns = m_playbackOverruns;
    const WebRtcAudioProcessor::Statistics processorStatistics = m_audioProcessor.statistics();
    statistics.aecBackendAvailable = processorStatistics.backendAvailable;
    statistics.aecInitialized = processorStatistics.initialized;
    statistics.aecEnabled = processorStatistics.aecEnabled;
    statistics.aecStreamDelayMs = m_aecStreamDelayMs;
    statistics.aecRenderProcessFailures = processorStatistics.renderProcessFailures;
    statistics.aecCaptureProcessFailures = processorStatistics.captureProcessFailures;
    statistics.aecBypassedFrames = processorStatistics.bypassedFrames;
    statistics.localLoopbackPackets = m_localLoopbackPackets;
    statistics.inputDeviceDescription = m_inputDeviceDescription;
    statistics.outputDeviceDescription = m_outputDeviceDescription;
    statistics.sourceBufferSize = m_sourceBufferSize;
    statistics.sinkBufferSize = m_sinkBufferSize;
    emit audioStatisticsUpdated(statistics);
    resetIntervalStatistics();
}

void AudioWorker::onPlaybackVolumeTimer()
{
    if (!m_running) {
        return;
    }

    double normalizedRms = 0.0;
    if (m_playbackVolumeSampleCount > 0) {
        normalizedRms = std::sqrt(m_playbackVolumeSumSquares
                                  / static_cast<double>(m_playbackVolumeSampleCount))
            / Int16FullScale;
    }
    normalizedRms = std::clamp(normalizedRms, 0.0, 1.0);
    const int rmsPercent = qRound(normalizedRms * 100.0);
    const double rmsDbfs = normalizedRms > 0.0
        ? std::max(SilenceDbfs, 20.0 * std::log10(normalizedRms))
        : SilenceDbfs;
    resetPlaybackVolume();
    emit playbackVolumeUpdated(rmsPercent, rmsDbfs);
}

quint32 AudioWorker::nextNonZero(quint32 value)
{
    return value == 0 ? 1 : value;
}

QString AudioWorker::describeFormat(const QAudioFormat &format)
{
    return QStringLiteral("sampleRate=%1，channelCount=%2，sampleFormat=%3")
        .arg(format.sampleRate())
        .arg(format.channelCount())
        .arg(static_cast<int>(format.sampleFormat()));
}

QString AudioWorker::describeDevice(const QAudioDevice &device)
{
    return QStringLiteral("%1（preferredFormat：%2）")
        .arg(device.description(), describeFormat(device.preferredFormat()));
}

QString AudioWorker::fixedFormatErrorMessage(const QString &side, const QAudioDevice &device) const
{
    return QStringLiteral("默认音频%1设备不支持固定 16 kHz / 单声道 / Int16：%2")
        .arg(side, describeDevice(device));
}

void AudioWorker::reportError(const QString &message)
{
    qWarning().noquote() << QStringLiteral("[AudioWorker]") << message;
    emit audioError(message);
}

void AudioWorker::resetIntervalStatistics()
{
    m_sentPacketsInterval = 0;
    m_receivedPacketsInterval = 0;
    m_sentBytesInterval = 0;
    m_receivedBytesInterval = 0;
}

void AudioWorker::stopAudioInternal(bool emitStoppedSignal)
{
    if (m_stopping) {
        return;
    }
    const bool wasActive = m_running || m_audioSource || m_audioSink;
    m_stopping = true;
    if (m_statisticsTimer) {
        m_statisticsTimer->stop();
    }
    if (m_playoutTimer) {
        m_playoutTimer->stop();
    }
    if (m_playbackVolumeTimer) {
        m_playbackVolumeTimer->stop();
    }
    if (m_inputDevice) {
        disconnect(m_inputDevice, &QIODevice::readyRead, this, &AudioWorker::onInputReadyRead);
    }
    m_inputDevice = nullptr;
    m_outputDevice = nullptr;
    if (m_audioSource) {
        m_audioSource->stop();
        delete m_audioSource;
        m_audioSource = nullptr;
    }
    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
    }
    m_captureBuffer.clear();
    m_playbackBuffer.clear();
    resetPlaybackVolume();
    m_jitterBuffer.clear();
    const bool wasLocalAecTestActive = m_localAecTestActive;
    const bool wasLocalAecEffectVerificationActive = m_localAecEffectVerificationActive;
    m_localAecTestActive = false;
    m_localAecEffectVerificationActive = false;
    m_localAecTestPacketsRemaining = 0;
    m_localAecTestPostRollPacketsRemaining = 0;
    m_localAecTestSamplePosition = 0;
    m_localAecTestElapsedTimer.invalidate();
    m_localAecTestRenderStartElapsedMs = -1;
    m_localAecTestCaptureStartElapsedMs = -1;
    m_localAecTestRenderPcm.clear();
    m_localAecTestCapturePcm.clear();
    m_localAecTestProcessedCapturePcm.clear();
    if (wasLocalAecTestActive) {
        emit localAecTestToneStateChanged(false);
    }
    if (wasLocalAecEffectVerificationActive) {
        emit localAecEffectVerificationStateChanged(false);
    }
    m_audioProcessor.shutdown();
    m_sendSessionId = 0;
    m_nextSequence = 1;
    m_nextTimestampSamples = 0;
    m_running = false;
    m_stopping = false;
    if (wasActive && emitStoppedSignal) {
        emit audioStopped();
    }
}

void AudioWorker::appendPlaybackPacket(const QByteArray &packet)
{
    if (packet.size() != AudioPacketProtocol::PcmPayloadSize) {
        return;
    }
    while (m_playbackBuffer.size() + packet.size() > PlaybackBufferLimit) {
        if (m_playbackBuffer.size() >= AudioPacketProtocol::PcmPayloadSize) {
            m_playbackBuffer.remove(0, AudioPacketProtocol::PcmPayloadSize);
        } else {
            m_playbackBuffer.clear();
        }
        ++m_playbackOverruns;
    }
    m_playbackBuffer.append(packet);
}

void AudioWorker::drainPlaybackBuffer()
{
    if (!m_outputDevice || m_playbackBuffer.isEmpty()) {
        return;
    }
    const qint64 written = m_outputDevice->write(m_playbackBuffer);
    if (written < 0) {
        stopAudioInternal(true);
        reportError(QStringLiteral("写入音频输出设备失败。"));
        return;
    }
    if (written > 0) {
        accumulatePlaybackVolume(m_playbackBuffer, static_cast<qsizetype>(written));
        m_playbackBuffer.remove(0, written);
    }
}

void AudioWorker::accumulatePlaybackVolume(const QByteArray &pcm, qsizetype byteCount)
{
    const qsizetype availableBytes = qMin(byteCount, pcm.size());
    const qsizetype sampleBytes = availableBytes - (availableBytes % sizeof(qint16));
    for (qsizetype offset = 0; offset < sampleBytes; offset += sizeof(qint16)) {
        qint16 sample = 0;
        // QByteArray is byte-aligned; copy before interpreting samples so a
        // partial QAudioSink write cannot cause an unaligned int16 access.
        std::memcpy(&sample, pcm.constData() + offset, sizeof(sample));
        const double sampleValue = static_cast<double>(sample);
        m_playbackVolumeSumSquares += sampleValue * sampleValue;
        ++m_playbackVolumeSampleCount;
    }
}

void AudioWorker::resetPlaybackVolume()
{
    m_playbackVolumeSampleCount = 0;
    m_playbackVolumeSumSquares = 0.0;
}

void AudioWorker::collectLocalAecTestCapture(const QByteArray &packet)
{
    if (packet.size() != AudioPacketProtocol::PcmPayloadSize) {
        return;
    }

    // Do not use capture that predates the first known render frame.  The
    // capture tail is kept for 500 ms after the final render frame so every
    // candidate 0–500 ms delay has matching microphone samples.
    if (!m_localAecTestRenderPcm.isEmpty()) {
        if (m_localAecTestCapturePcm.isEmpty() && m_localAecTestElapsedTimer.isValid()) {
            m_localAecTestCaptureStartElapsedMs = m_localAecTestElapsedTimer.elapsed();
        }
        m_localAecTestCapturePcm.append(packet);
        if (m_localAecEffectVerificationActive) {
            // This invokes ProcessStream() with the same microphone PCM that
            // a call would send, but the resulting PCM is recorded only for
            // comparison and never sent or played locally.
            m_localAecTestProcessedCapturePcm.append(
                m_audioProcessor.processCapture20Ms(packet, m_aecStreamDelayMs));
        }
    }
    if (m_localAecTestPacketsRemaining == 0 && m_localAecTestPostRollPacketsRemaining > 0) {
        --m_localAecTestPostRollPacketsRemaining;
        if (m_localAecTestPostRollPacketsRemaining == 0) {
            finishLocalAecTest();
        }
    }
}

void AudioWorker::finishLocalAecTest()
{
    if (!m_localAecTestActive) {
        return;
    }

    const qint64 captureStartOffsetMs = m_localAecTestCaptureStartElapsedMs >= 0
        && m_localAecTestRenderStartElapsedMs >= 0
        ? m_localAecTestCaptureStartElapsedMs - m_localAecTestRenderStartElapsedMs
        : 0;
    const AcousticDelayEstimator::Result result = AcousticDelayEstimator::estimate(
        m_localAecTestRenderPcm, m_localAecTestCapturePcm, captureStartOffsetMs);
    const bool wasLocalAecEffectVerificationActive = m_localAecEffectVerificationActive;
    const QByteArray renderPcm = m_localAecTestRenderPcm;
    const QByteArray rawCapturePcm = m_localAecTestCapturePcm;
    const QByteArray processedCapturePcm = m_localAecTestProcessedCapturePcm;
    m_localAecTestActive = false;
    m_localAecEffectVerificationActive = false;
    m_localAecTestPacketsRemaining = 0;
    m_localAecTestPostRollPacketsRemaining = 0;
    m_localAecTestSamplePosition = 0;
    m_localAecTestElapsedTimer.invalidate();
    m_localAecTestRenderStartElapsedMs = -1;
    m_localAecTestCaptureStartElapsedMs = -1;
    m_localAecTestRenderPcm.clear();
    m_localAecTestCapturePcm.clear();
    m_localAecTestProcessedCapturePcm.clear();

    // APM collected only reverse frames during the diagnostic.  Start its
    // normal capture/render state fresh before returning to a call.
    m_audioProcessor.reset();
    emit localAecTestToneStateChanged(false);
    if (wasLocalAecEffectVerificationActive) {
        emit localAecEffectVerificationStateChanged(false);
        if (!result.valid) {
            const QString reason = QStringLiteral(
                "未检测到可信的扬声器回声，无法验证 AEC（麦克风 %1 dBFS，相关性 %2）。")
                                       .arg(result.captureRmsDbfs, 0, 'f', 1)
                                       .arg(result.normalizedCorrelation, 0, 'f', 2);
            qWarning().noquote() << QStringLiteral("[AudioWorker] AEC effect verification failed:")
                                 << reason;
            emit localAecEffectVerificationFailed(reason);
            return;
        }

        const AcousticDelayEstimator::EchoMetrics rawMetrics =
            AcousticDelayEstimator::measureAtDelay(renderPcm,
                                                   rawCapturePcm,
                                                   captureStartOffsetMs,
                                                   result.delayMs);
        const AcousticDelayEstimator::EchoMetrics processedMetrics =
            AcousticDelayEstimator::measureAtDelay(renderPcm,
                                                   processedCapturePcm,
                                                   captureStartOffsetMs,
                                                   result.delayMs);
        if (!rawMetrics.valid || !processedMetrics.valid) {
            const QString reason = QStringLiteral("AEC 前后 PCM 长度不足，无法完成本机效果对比。");
            qWarning().noquote() << QStringLiteral("[AudioWorker] AEC effect verification failed:")
                                 << reason;
            emit localAecEffectVerificationFailed(reason);
            return;
        }

        const double echoReductionDb = rawMetrics.renderCorrelatedRmsDbfs
            - processedMetrics.renderCorrelatedRmsDbfs;
        qInfo().nospace() << "[AudioWorker] AEC effect verification: delay="
                          << result.delayMs << " ms, correlated echo "
                          << rawMetrics.renderCorrelatedRmsDbfs << " -> "
                          << processedMetrics.renderCorrelatedRmsDbfs << " dBFS, reduction="
                          << echoReductionDb << " dB, correlation "
                          << rawMetrics.normalizedCorrelation << " -> "
                          << processedMetrics.normalizedCorrelation << ".";
        emit localAecEffectVerified(result.delayMs,
                                    echoReductionDb,
                                    rawMetrics.renderCorrelatedRmsDbfs,
                                    processedMetrics.renderCorrelatedRmsDbfs,
                                    rawMetrics.normalizedCorrelation,
                                    processedMetrics.normalizedCorrelation);
        return;
    }
    if (result.valid) {
        m_aecStreamDelayMs = result.delayMs;
        qInfo().nospace() << "[AudioWorker] Acoustic delay calibration: "
                          << result.delayMs << " ms, correlation="
                          << result.normalizedCorrelation << ", capture="
                          << result.captureRmsDbfs << " dBFS, callbackOffset="
                          << captureStartOffsetMs << " ms, waveformAlignment="
                          << result.waveformAlignmentMs << " ms.";
        emit localAecDelayCalibrated(result.delayMs,
                                      result.normalizedCorrelation,
                                      result.captureRmsDbfs);
        return;
    }

    const QString reason = QStringLiteral(
        "未检测到足够强的扬声器测试音（麦克风 %1 dBFS，相关性 %2，最佳候选 %3 ms）；延时保持 %4 ms。")
                               .arg(result.captureRmsDbfs, 0, 'f', 1)
                               .arg(result.normalizedCorrelation, 0, 'f', 2)
                               .arg(result.delayMs)
                               .arg(m_aecStreamDelayMs);
    qWarning().noquote() << QStringLiteral("[AudioWorker] AEC delay calibration failed:") << reason;
    emit localAecDelayCalibrationFailed(reason);
}

QByteArray AudioWorker::nextLocalAecTestPacket()
{
    QByteArray packet(AudioPacketProtocol::PcmPayloadSize, '\0');
    const double sweepRangeHz = LocalAecTestSweepEndHz - LocalAecTestSweepStartHz;
    for (int sampleIndex = 0; sampleIndex < AudioPacketProtocol::SamplesPerChannel; ++sampleIndex) {
        const quint64 position = m_localAecTestSamplePosition++;
        const double sweepTimeSeconds = static_cast<double>(position % AudioPacketProtocol::SampleRate)
            / static_cast<double>(AudioPacketProtocol::SampleRate);
        const double phase = TwoPi * (LocalAecTestSweepStartHz * sweepTimeSeconds
                                      + 0.5 * sweepRangeHz * sweepTimeSeconds * sweepTimeSeconds);
        const int sampleValue = static_cast<int>(std::lround(LocalAecTestAmplitude * std::sin(phase)));
        const qint16 sample = static_cast<qint16>(std::clamp(sampleValue, -32768, 32767));
        std::memcpy(packet.data() + sampleIndex * static_cast<int>(sizeof(sample)),
                    &sample,
                    sizeof(sample));
    }
    return packet;
}

void AudioWorker::ensureTimers()
{
    if (!m_playoutTimer) {
        m_playoutTimer = new QTimer(this);
        m_playoutTimer->setTimerType(Qt::PreciseTimer);
        connect(m_playoutTimer, &QTimer::timeout, this, &AudioWorker::onPlayoutTimer);
    }
    if (!m_playbackVolumeTimer) {
        m_playbackVolumeTimer = new QTimer(this);
        m_playbackVolumeTimer->setTimerType(Qt::PreciseTimer);
        connect(m_playbackVolumeTimer,
                &QTimer::timeout,
                this,
                &AudioWorker::onPlaybackVolumeTimer);
    }
    if (!m_statisticsTimer) {
        m_statisticsTimer = new QTimer(this);
        m_statisticsTimer->setTimerType(Qt::PreciseTimer);
        connect(m_statisticsTimer, &QTimer::timeout, this, &AudioWorker::onStatisticsTimer);
    }
}
