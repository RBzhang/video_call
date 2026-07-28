#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QHostAddress>
#include <QByteArray>
#include <QImage>
#include <QMainWindow>
#include <QString>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class CameraWorker;
class QCloseEvent;
class QThread;
class VideoUdpTransport;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    void requestStartCamera(int cameraIndex);
    void requestStopCamera();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onStartCameraClicked();
    void onStopCameraClicked();
    void onApplyNetworkSettingsClicked();
    void onStopNetworkClicked();
    void onSendTestFrameClicked();
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

private:
    void updateVideoDisplay();
    void resetCameraUi();
    void shutdownCameraThread();
    QByteArray createDeterministicTestFrame(quint32 sequence) const;
    bool validateDeterministicTestFrame(const QByteArray &frame,
                                        quint32 *sequence,
                                        QString *errorMessage) const;

    Ui::MainWindow *ui;
    CameraWorker *m_cameraWorker = nullptr;
    QThread *m_cameraThread = nullptr;
    VideoUdpTransport *m_videoUdpTransport = nullptr;
    QImage m_lastFrame;
    QHostAddress m_peerAddress;
    quint16 m_localVideoPort = 5000;
    quint16 m_peerVideoPort = 5000;
    quint32 m_testFrameSequence = 0;
    quint32 m_lastSentTestFrameSequence = 0;
    bool m_cameraRunning = false;
    bool m_cameraOpening = false;
    bool m_shuttingDown = false;
    bool m_networkSettingsValid = false;
};
#endif // MAINWINDOW_H
