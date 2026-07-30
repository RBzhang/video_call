#ifndef AUDIOWORKER_H
#define AUDIOWORKER_H

#include <QElapsedTimer>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudio>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include "audiojitterbuffer.h"

class AudioUdpTransport;
class QAudioSink;
class QAudioSource;
class QHostAddress;
class QIODevice;
class QTimer;

struct AudioStatistics
{
    double sentPacketsPerSecond = 0.0;
    double receivedPacketsPerSecond = 0.0;
    double sentPayloadMegabitsPerSecond = 0.0;
    double receivedPayloadMegabitsPerSecond = 0.0;
    int jitterBufferedPackets = 0;
    int jitterBufferedMilliseconds = 0;
    quint64 concealedPackets = 0;
    quint64 duplicatePackets = 0;
    quint64 latePackets = 0;
    quint64 foreignPackets = 0;
    quint64 invalidPackets = 0;
    quint64 captureOverruns = 0;
    quint64 playbackOverruns = 0;
    QString inputDeviceDescription;
    QString outputDeviceDescription;
    qsizetype sourceBufferSize = 0;
    qsizetype sinkBufferSize = 0;
};

Q_DECLARE_METATYPE(AudioStatistics)

class AudioWorker : public QObject
{
    Q_OBJECT

public:
    explicit AudioWorker(QObject *parent = nullptr);
    ~AudioWorker() override;

    static int destructionCount();
    static void resetDestructionCount();

public slots:
    void configureNetwork(const QString &localAddress,
                          const QString &peerAddress,
                          quint16 localPort,
                          quint16 peerPort);
    void startAudio();
    void stopAudio();
    void shutdown();

signals:
    void audioNetworkReady(const QString &message);
    void audioStarted(const QString &message);
    void audioStopped();
    void audioError(const QString &message);
    void audioStatisticsUpdated(const AudioStatistics &statistics);

private slots:
    void onInputReadyRead();
    void onAudioPacketReceived(const QByteArray &pcmPayload,
                               quint32 sessionId,
                               quint32 sequence,
                               quint32 timestampSamples,
                               const QHostAddress &senderAddress,
                               quint16 senderPort);
    void onAudioDatagramRejected(const QString &message);
    void onForeignDatagramDropped(const QHostAddress &senderAddress, quint16 senderPort);
    void onSourceStateChanged(QtAudio::State state);
    void onSinkStateChanged(QtAudio::State state);
    void onPlayoutTimer();
    void onStatisticsTimer();

private:
    static quint32 nextNonZero(quint32 value);
    static QString describeFormat(const QAudioFormat &format);
    static QString describeDevice(const QAudioDevice &device);
    QString fixedFormatErrorMessage(const QString &side, const QAudioDevice &device) const;
    void reportError(const QString &message);
    void resetIntervalStatistics();
    void stopAudioInternal(bool emitStoppedSignal);
    void appendPlaybackPacket(const QByteArray &packet);
    void drainPlaybackBuffer();
    void ensureTimers();

    AudioUdpTransport *m_transport = nullptr;
    QAudioSource *m_audioSource = nullptr;
    QAudioSink *m_audioSink = nullptr;
    QIODevice *m_inputDevice = nullptr;
    QIODevice *m_outputDevice = nullptr;
    QTimer *m_playoutTimer = nullptr;
    QTimer *m_statisticsTimer = nullptr;
    AudioJitterBuffer m_jitterBuffer;
    QElapsedTimer m_statisticsElapsedTimer;

    QByteArray m_captureBuffer;
    QByteArray m_playbackBuffer;
    QString m_inputDeviceDescription;
    QString m_outputDeviceDescription;
    qsizetype m_sourceBufferSize = 0;
    qsizetype m_sinkBufferSize = 0;
    quint32 m_sendSessionId = 0;
    quint32 m_nextSequence = 1;
    quint32 m_nextTimestampSamples = 0;
    bool m_networkReady = false;
    bool m_running = false;
    bool m_stopping = false;
    bool m_shutdown = false;

    quint64 m_sentPacketsInterval = 0;
    quint64 m_receivedPacketsInterval = 0;
    quint64 m_sentBytesInterval = 0;
    quint64 m_receivedBytesInterval = 0;
    quint64 m_foreignPackets = 0;
    quint64 m_invalidPackets = 0;
    quint64 m_captureOverruns = 0;
    quint64 m_playbackOverruns = 0;
};

#endif // AUDIOWORKER_H
