#include "audioworker.h"

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

namespace {

constexpr int AudioPacketIntervalMs = 20;
constexpr qsizetype CaptureBufferLimit = AudioPacketProtocol::PcmPayloadSize * 10;
constexpr qsizetype PlaybackBufferLimit = AudioPacketProtocol::PcmPayloadSize * 5;
constexpr qsizetype RequestedSinkBufferSize = AudioPacketProtocol::PcmPayloadSize * 4;

QAudioFormat fixedAudioFormat()
{
    QAudioFormat format;
    format.setSampleRate(AudioPacketProtocol::SampleRate);
    format.setChannelCount(AudioPacketProtocol::Channels);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

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
    stopAudioInternal(false);
    if (m_transport) {
        m_transport->close();
    }
    qInfo().noquote() << QStringLiteral("[AudioWorker] 析构。");
}

void AudioWorker::configureNetwork(const QString &peerAddress, quint16 localPort, quint16 peerPort)
{
    stopAudioInternal(true);
    m_networkReady = false;
    m_jitterBuffer.clear();
    if (!m_transport) {
        reportError(QStringLiteral("音频 UDP Transport 不可用。"));
        return;
    }

    QHostAddress address;
    if (!address.setAddress(peerAddress.trimmed())
        || address.protocol() != QAbstractSocket::IPv4Protocol) {
        m_transport->close();
        reportError(QStringLiteral("音频对端 IPv4 地址无效。"));
        return;
    }

    m_transport->close();
    m_transport->configurePeer(address, peerPort);
    QString bindError;
    if (!m_transport->bindReceiver(QHostAddress::AnyIPv4, localPort, &bindError)) {
        reportError(QStringLiteral("音频网络配置失败：%1").arg(bindError));
        return;
    }

    m_networkReady = true;
    emit audioNetworkReady(
        QStringLiteral("音频：已绑定 0.0.0.0:%1 → %2:%3")
            .arg(m_transport->localPort())
            .arg(address.toString())
            .arg(peerPort));
}

void AudioWorker::startAudio()
{
    if (m_running) {
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
    m_jitterBuffer.clear();
    m_jitterBuffer.resetStatistics();
    m_sendSessionId = QRandomGenerator::global()->generate();
    m_sendSessionId = nextNonZero(m_sendSessionId);
    m_nextSequence = 1;
    m_nextTimestampSamples = 0;
    resetIntervalStatistics();
    ensureTimers();
    m_statisticsElapsedTimer.restart();
    m_playoutTimer->start(AudioPacketIntervalMs);
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
    stopAudioInternal(true);
}

void AudioWorker::shutdown()
{
    stopAudioInternal(true);
    if (m_playoutTimer) {
        delete m_playoutTimer;
        m_playoutTimer = nullptr;
    }
    if (m_statisticsTimer) {
        delete m_statisticsTimer;
        m_statisticsTimer = nullptr;
    }
    if (m_transport) {
        m_transport->close();
        delete m_transport;
        m_transport = nullptr;
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
        QString sendError;
        if (m_transport->sendAudioPayload(payload,
                                          m_sendSessionId,
                                          m_nextSequence,
                                          m_nextTimestampSamples,
                                          &sendError)) {
            ++m_sentPacketsInterval;
            m_sentBytesInterval += static_cast<quint64>(payload.size());
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
    Q_UNUSED(senderAddress);
    Q_UNUSED(senderPort);

    if (!m_running) {
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

    const QByteArray packet = m_jitterBuffer.takeNextPacket();
    if (!packet.isEmpty()) {
        appendPlaybackPacket(packet);
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
    statistics.inputDeviceDescription = m_inputDeviceDescription;
    statistics.outputDeviceDescription = m_outputDeviceDescription;
    statistics.sourceBufferSize = m_sourceBufferSize;
    statistics.sinkBufferSize = m_sinkBufferSize;
    emit audioStatisticsUpdated(statistics);
    resetIntervalStatistics();
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
    m_jitterBuffer.clear();
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
        m_playbackBuffer.remove(0, written);
    }
}

void AudioWorker::ensureTimers()
{
    if (!m_playoutTimer) {
        m_playoutTimer = new QTimer(this);
        m_playoutTimer->setTimerType(Qt::PreciseTimer);
        connect(m_playoutTimer, &QTimer::timeout, this, &AudioWorker::onPlayoutTimer);
    }
    if (!m_statisticsTimer) {
        m_statisticsTimer = new QTimer(this);
        m_statisticsTimer->setTimerType(Qt::PreciseTimer);
        connect(m_statisticsTimer, &QTimer::timeout, this, &AudioWorker::onStatisticsTimer);
    }
}
