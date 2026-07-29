#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QHostAddress>
#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QMainWindow>
#include <QString>

#include <optional>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class CameraWorker;
class AudioWorker;
class QCloseEvent;
class QResizeEvent;
class QThread;
class QTimer;
class RemoteVideoDecoder;
class VideoUdpTransport;
struct AudioStatistics;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    void requestStartCamera(int cameraIndex);
    void requestStopCamera();
    void requestStartVideoEncoding(int targetFps, int jpegQuality);
    void requestStopVideoEncoding();
    void requestConfigureAudioNetwork(const QString &peerAddress,
                                      quint16 localPort,
                                      quint16 peerPort);
    void requestStartAudio();
    void requestStopAudio();
    void requestRemoteJpegDecode(const QByteArray &jpegData,
                                 quint64 generation,
                                 quint32 sessionId,
                                 quint32 frameId,
                                 quint32 timestampMs,
                                 const QString &senderAddress,
                                 quint16 senderPort);

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onStartCameraClicked();
    void onStopCameraClicked();
    void onApplyNetworkSettingsClicked();
    void onStopNetworkClicked();
    void onSendTestFrameClicked();
    void onStartVideoSendClicked();
    void onStopVideoSendClicked();
    void onApplyAudioSettingsClicked();
    void onStartAudioClicked();
    void onStopAudioClicked();
    void onUdpFrameReceived(const QByteArray &encodedFrame,
                            quint32 sessionId,
                            quint32 frameId,
                            quint32 timestampMs,
                            const QHostAddress &senderAddress,
                            quint16 senderPort);
    void onUdpFrameSent(quint32 frameId, qsizetype frameSize, qsizetype fragmentCount);
    void onUdpNetworkError(const QString &message);
    void onUdpDatagramRejected(const QString &message);
    void onCameraStarted(const QString &description);
    void onCameraStopped();
    void onCameraError(const QString &message);
    void onCameraDiagnostic(const QString &message);
    void onFrameReady(const QImage &image);
    void onJpegFrameReady(const QByteArray &jpegData,
                          int width,
                          int height,
                          int jpegQuality,
                          qint64 encodingDurationUs);
    void onVideoEncodingError(const QString &message);
    void updateVideoSendStatistics();
    void onRemoteFrameDecoded(const QImage &image,
                              qsizetype jpegSize,
                              qint64 decodingDurationUs,
                              quint64 generation,
                              quint32 sessionId,
                              quint32 frameId,
                              quint32 timestampMs,
                              const QString &senderAddress,
                              quint16 senderPort);
    void onRemoteFrameDecodeFailed(const QString &message,
                                   qsizetype jpegSize,
                                   qint64 decodingDurationUs,
                                   quint64 generation,
                                   quint32 sessionId,
                                   quint32 frameId,
                                   const QString &senderAddress,
                                   quint16 senderPort);
    void updateRemoteReceiveStatistics();
    void onAudioNetworkReady(const QString &message);
    void onAudioStarted(const QString &message);
    void onAudioStopped();
    void onAudioError(const QString &message);
    void onAudioStatisticsUpdated(const AudioStatistics &statistics);

private:
    struct PendingRemoteJpegFrame
    {
        QByteArray jpegData;
        quint64 generation = 0;
        quint32 sessionId = 0;
        quint32 frameId = 0;
        quint32 timestampMs = 0;
        QString senderAddress;
        quint16 senderPort = 0;
    };

    void updateVideoDisplay();
    void updateRemoteVideoDisplay();
    void resetRemoteVideoDisplay(const QString &message);
    void resetCameraUi();
    void setLocalVideoStatus(const QString &message);
    void shutdownCameraThread();
    void shutdownAudioThread();
    void shutdownRemoteDecoderThread();
    void stopVideoSending(const QString &reason);
    void updateVideoSendUi();
    void scheduleRemoteJpegDecode(PendingRemoteJpegFrame frame);
    void startRemoteJpegDecode(const PendingRemoteJpegFrame &frame);
    void completeRemoteJpegDecode();
    void resetRemoteReceiveState(const QString &message);
    void clearRemoteReceiveStatistics();
    void advanceRemoteReceiveGeneration();
    void reportRemoteDecodeFailure(const QString &message);
    QByteArray createDeterministicTestFrame(quint32 sequence) const;
    bool validateDeterministicTestFrame(const QByteArray &frame,
                                        quint32 *sequence,
                                        QString *errorMessage) const;

    Ui::MainWindow *ui;
    CameraWorker *m_cameraWorker = nullptr;
    QThread *m_cameraThread = nullptr;
    AudioWorker *m_audioWorker = nullptr;
    QThread *m_audioThread = nullptr;
    RemoteVideoDecoder *m_remoteDecoder = nullptr;
    QThread *m_remoteDecoderThread = nullptr;
    VideoUdpTransport *m_videoUdpTransport = nullptr;
    QImage m_lastFrame;
    QImage m_remoteFrame;
    QHostAddress m_peerAddress;
    quint16 m_localVideoPort = 5000;
    quint16 m_peerVideoPort = 5000;
    quint32 m_testFrameSequence = 0;
    quint32 m_lastSentTestFrameSequence = 0;
    bool m_cameraRunning = false;
    bool m_cameraOpening = false;
    bool m_shuttingDown = false;
    bool m_networkSettingsValid = false;
    bool m_audioNetworkSettingsValid = false;
    bool m_audioRunning = false;

    bool m_videoSending = false;
    quint64 m_videoFramesSentTotal = 0;
    quint64 m_videoBytesSentTotal = 0;
    quint64 m_videoFragmentsSentTotal = 0;
    quint64 m_videoFramesSentInterval = 0;
    quint64 m_videoBytesSentInterval = 0;
    quint64 m_videoFragmentsSentInterval = 0;
    quint64 m_videoEncodingDurationUsInterval = 0;
    qsizetype m_lastJpegSize = 0;
    qsizetype m_lastFragmentCount = 0;
    int m_lastVideoWidth = 0;
    int m_lastVideoHeight = 0;
    int m_activeVideoFps = 10;
    int m_activeJpegQuality = 60;
    QTimer *m_videoStatsTimer = nullptr;
    QElapsedTimer m_videoStatsElapsedTimer;

    quint64 m_remoteReceiveGeneration = 1;
    bool m_remoteDecodeBusy = false;
    std::optional<PendingRemoteJpegFrame> m_pendingRemoteJpeg;
    QTimer *m_remoteStatsTimer = nullptr;
    QElapsedTimer m_remoteStatsElapsedTimer;
    QElapsedTimer m_remoteDecodeErrorTimer;
    QString m_lastRemoteDecodeError;
    QString m_lastRemoteSenderAddress;
    quint16 m_lastRemoteSenderPort = 0;
    int m_lastRemoteFrameWidth = 0;
    int m_lastRemoteFrameHeight = 0;
    quint64 m_remoteJpegFramesReceivedInterval = 0;
    quint64 m_remoteJpegBytesReceivedInterval = 0;
    quint64 m_remoteFramesDecodedInterval = 0;
    quint64 m_remoteDecodeDurationUsInterval = 0;
    quint64 m_remoteDecodeFailuresInterval = 0;
    quint64 m_remoteSupersededFramesInterval = 0;
    quint64 m_remoteForeignFramesInterval = 0;
    quint64 m_remoteUnsupportedFramesInterval = 0;
};
#endif // MAINWINDOW_H
