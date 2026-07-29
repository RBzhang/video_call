#include "mainwindow.h"
#include "cameraworker.h"
#include "remotevideodecoder.h"
#include "videopacketprotocol.h"
#include "videoudptransport.h"

#include "./ui_mainwindow.h"

#include <QBuffer>
#include <QCloseEvent>
#include <QDataStream>
#include <QDebug>
#include <QIODevice>
#include <QMetaObject>
#include <QPixmap>
#include <QResizeEvent>
#include <QThread>
#include <QTimer>

#include <utility>

namespace {

constexpr qsizetype TestFrameSize = 50000;
const QByteArray TestFrameMagic("VCL_TEST_FRAME_V1");

void setTestFrameError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

void clearTestFrameError(QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
}

bool hasJpegMarkers(const QByteArray &frame)
{
    return frame.size() >= 4
        && static_cast<uchar>(frame.at(0)) == 0xff
        && static_cast<uchar>(frame.at(1)) == 0xd8
        && static_cast<uchar>(frame.at(frame.size() - 2)) == 0xff
        && static_cast<uchar>(frame.at(frame.size() - 1)) == 0xd9;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    m_videoUdpTransport = new VideoUdpTransport(this);
    connect(m_videoUdpTransport,
            &VideoUdpTransport::frameReceived,
            this,
            &MainWindow::onUdpFrameReceived);
    connect(m_videoUdpTransport,
            &VideoUdpTransport::frameSent,
            this,
            &MainWindow::onUdpFrameSent);
    connect(m_videoUdpTransport,
            &VideoUdpTransport::networkError,
            this,
            &MainWindow::onUdpNetworkError);
    connect(m_videoUdpTransport,
            &VideoUdpTransport::datagramRejected,
            this,
            &MainWindow::onUdpDatagramRejected);

    m_videoStatsTimer = new QTimer(this);
    m_videoStatsTimer->setInterval(1000);
    connect(m_videoStatsTimer,
            &QTimer::timeout,
            this,
            &MainWindow::updateVideoSendStatistics);

    m_remoteStatsTimer = new QTimer(this);
    m_remoteStatsTimer->setInterval(1000);
    connect(m_remoteStatsTimer,
            &QTimer::timeout,
            this,
            &MainWindow::updateRemoteReceiveStatistics);

    m_remoteDecoderThread = new QThread;
    m_remoteDecoder = new RemoteVideoDecoder;
    m_remoteDecoder->moveToThread(m_remoteDecoderThread);

    connect(m_remoteDecoderThread,
            &QThread::finished,
            m_remoteDecoder,
            &QObject::deleteLater);
    connect(m_remoteDecoder,
            &QObject::destroyed,
            this,
            [this] { m_remoteDecoder = nullptr; });
    connect(this,
            &MainWindow::requestRemoteJpegDecode,
            m_remoteDecoder,
            &RemoteVideoDecoder::decodeJpeg,
            Qt::QueuedConnection);
    connect(m_remoteDecoder,
            &RemoteVideoDecoder::frameDecoded,
            this,
            &MainWindow::onRemoteFrameDecoded,
            Qt::QueuedConnection);
    connect(m_remoteDecoder,
            &RemoteVideoDecoder::frameDecodeFailed,
            this,
            &MainWindow::onRemoteFrameDecodeFailed,
            Qt::QueuedConnection);
    m_remoteDecoderThread->start();

    m_cameraThread = new QThread;
    m_cameraWorker = new CameraWorker;
    m_cameraWorker->moveToThread(m_cameraThread);

    connect(m_cameraThread,
            &QThread::finished,
            m_cameraWorker,
            &QObject::deleteLater);
    connect(m_cameraWorker,
            &QObject::destroyed,
            this,
            [this] { m_cameraWorker = nullptr; });

    connect(this,
            &MainWindow::requestStartCamera,
            m_cameraWorker,
            &CameraWorker::startCamera,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::requestStopCamera,
            m_cameraWorker,
            &CameraWorker::stopCamera,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::requestStartVideoEncoding,
            m_cameraWorker,
            &CameraWorker::startVideoEncoding,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::requestStopVideoEncoding,
            m_cameraWorker,
            &CameraWorker::stopVideoEncoding,
            Qt::QueuedConnection);

    connect(m_cameraWorker,
            &CameraWorker::frameReady,
            this,
            &MainWindow::onFrameReady,
            Qt::QueuedConnection);
    connect(m_cameraWorker,
            &CameraWorker::cameraStarted,
            this,
            &MainWindow::onCameraStarted,
            Qt::QueuedConnection);
    connect(m_cameraWorker,
            &CameraWorker::cameraStopped,
            this,
            &MainWindow::onCameraStopped,
            Qt::QueuedConnection);
    connect(m_cameraWorker,
            &CameraWorker::errorOccurred,
            this,
            &MainWindow::onCameraError,
            Qt::QueuedConnection);
    connect(m_cameraWorker,
            &CameraWorker::diagnosticOccurred,
            this,
            &MainWindow::onCameraDiagnostic,
            Qt::QueuedConnection);
    connect(m_cameraWorker,
            &CameraWorker::jpegFrameReady,
            this,
            &MainWindow::onJpegFrameReady,
            Qt::QueuedConnection);
    connect(m_cameraWorker,
            &CameraWorker::videoEncodingError,
            this,
            &MainWindow::onVideoEncodingError,
            Qt::QueuedConnection);

    connect(ui->startCameraButton,
            &QPushButton::clicked,
            this,
            &MainWindow::onStartCameraClicked);
    connect(ui->stopCameraButton,
            &QPushButton::clicked,
            this,
            &MainWindow::onStopCameraClicked);
    connect(ui->applyNetworkSettingsButton,
            &QPushButton::clicked,
            this,
            &MainWindow::onApplyNetworkSettingsClicked);
    connect(ui->stopNetworkButton,
            &QPushButton::clicked,
            this,
            &MainWindow::onStopNetworkClicked);
    connect(ui->sendTestFrameButton,
            &QPushButton::clicked,
            this,
            &MainWindow::onSendTestFrameClicked);
    connect(ui->startVideoSendButton,
            &QPushButton::clicked,
            this,
            &MainWindow::onStartVideoSendClicked);
    connect(ui->stopVideoSendButton,
            &QPushButton::clicked,
            this,
            &MainWindow::onStopVideoSendClicked);

    resetCameraUi();
    updateVideoSendUi();
    m_cameraThread->start();
}

MainWindow::~MainWindow()
{
    stopVideoSending(QString());
    if (m_videoUdpTransport) {
        m_videoUdpTransport->close();
    }
    resetRemoteReceiveState(QStringLiteral("远端接收：网络已停止"));
    shutdownRemoteDecoderThread();
    shutdownCameraThread();
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    stopVideoSending(QStringLiteral("视频发送：窗口正在关闭。"));
    if (m_videoUdpTransport) {
        m_videoUdpTransport->close();
    }
    resetRemoteReceiveState(QStringLiteral("远端接收：网络已停止"));
    shutdownRemoteDecoderThread();
    shutdownCameraThread();
    QMainWindow::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateVideoDisplay();
    updateRemoteVideoDisplay();
}

void MainWindow::onStartCameraClicked()
{
    if (m_shuttingDown) {
        return;
    }

    if (!m_cameraThread || !m_cameraThread->isRunning() || !m_cameraWorker) {
        m_cameraRunning = false;
        resetCameraUi();
        setLocalVideoStatus(QStringLiteral("状态：摄像头工作线程不可用。"));
        return;
    }

    const int cameraIndex = ui->cameraIndexSpinBox->value();
    m_cameraRunning = false;
    m_cameraOpening = true;
    ui->startCameraButton->setEnabled(false);
    ui->stopCameraButton->setEnabled(false);
    ui->cameraIndexSpinBox->setEnabled(false);
    setLocalVideoStatus(QStringLiteral("状态：正在打开摄像头……"));

    emit requestStartCamera(cameraIndex);
}

void MainWindow::onStopCameraClicked()
{
    if (m_shuttingDown) {
        return;
    }

    ui->startCameraButton->setEnabled(false);
    ui->stopCameraButton->setEnabled(false);
    ui->cameraIndexSpinBox->setEnabled(false);
    setLocalVideoStatus(QStringLiteral("状态：正在停止摄像头……"));

    stopVideoSending(QStringLiteral("视频发送：摄像头正在停止。"));
    emit requestStopCamera();
}

void MainWindow::onApplyNetworkSettingsClicked()
{
    stopVideoSending(QStringLiteral("视频发送：网络设置已重新应用，已停止。"));
    resetRemoteReceiveState(QStringLiteral("远端接收：网络正在重新配置"));

    const QString addressText = ui->peerAddressLineEdit->text().trimmed();
    const int localPort = ui->localVideoPortSpinBox->value();
    const int peerPort = ui->peerVideoPortSpinBox->value();

    QHostAddress candidateAddress;
    const bool isValidIpv4 = candidateAddress.setAddress(addressText)
        && candidateAddress.protocol() == QAbstractSocket::IPv4Protocol;
    if (!isValidIpv4) {
        if (m_videoUdpTransport) {
            m_videoUdpTransport->close();
        }
        m_networkSettingsValid = false;
        ui->applyNetworkSettingsButton->setEnabled(true);
        ui->stopNetworkButton->setEnabled(false);
        ui->sendTestFrameButton->setEnabled(false);
        ui->networkStatusLabel->setText(QStringLiteral("网络：对端 IPv4 地址无效"));
        resetRemoteVideoDisplay(QStringLiteral("远端接收：网络未启动"));
        updateVideoSendUi();
        return;
    }

    if (!m_videoUdpTransport) {
        m_networkSettingsValid = false;
        ui->stopNetworkButton->setEnabled(false);
        ui->sendTestFrameButton->setEnabled(false);
        ui->networkStatusLabel->setText(QStringLiteral("网络：UDP 传输对象不可用。"));
        resetRemoteVideoDisplay(QStringLiteral("远端接收：网络未启动"));
        updateVideoSendUi();
        return;
    }

    m_videoUdpTransport->close();
    m_videoUdpTransport->configurePeer(candidateAddress, static_cast<quint16>(peerPort));
    QString bindError;
    if (!m_videoUdpTransport->bindReceiver(QHostAddress::AnyIPv4,
                                            static_cast<quint16>(localPort),
                                            &bindError)) {
        m_networkSettingsValid = false;
        ui->applyNetworkSettingsButton->setEnabled(true);
        ui->stopNetworkButton->setEnabled(false);
        ui->sendTestFrameButton->setEnabled(false);
        ui->networkStatusLabel->setText(QStringLiteral("网络：%1").arg(bindError));
        resetRemoteVideoDisplay(QStringLiteral("远端接收：网络未启动"));
        updateVideoSendUi();
        return;
    }

    m_peerAddress = candidateAddress;
    m_localVideoPort = m_videoUdpTransport->localPort();
    m_peerVideoPort = static_cast<quint16>(peerPort);
    m_networkSettingsValid = true;
    ui->applyNetworkSettingsButton->setEnabled(true);
    ui->stopNetworkButton->setEnabled(true);
    ui->sendTestFrameButton->setEnabled(true);

    ui->networkStatusLabel->setText(
        QStringLiteral("网络：已绑定 0.0.0.0:%1 → %2:%3")
            .arg(m_localVideoPort)
            .arg(m_peerAddress.toString())
            .arg(m_peerVideoPort));
    resetRemoteVideoDisplay(QStringLiteral("尚未收到远端视频"));
    ui->remoteVideoStatusLabel->setText(QStringLiteral("远端接收：等待对端 JPEG……"));
    if (m_remoteStatsTimer) {
        m_remoteStatsElapsedTimer.restart();
        m_remoteStatsTimer->start();
    }
    updateVideoSendUi();
}

void MainWindow::onStopNetworkClicked()
{
    stopVideoSending(QStringLiteral("视频发送：网络已停止。"));
    if (m_videoUdpTransport) {
        m_videoUdpTransport->close();
    }
    m_networkSettingsValid = false;
    resetRemoteReceiveState(QStringLiteral("远端接收：网络已停止"));
    ui->applyNetworkSettingsButton->setEnabled(true);
    ui->stopNetworkButton->setEnabled(false);
    ui->sendTestFrameButton->setEnabled(false);
    ui->networkStatusLabel->setText(QStringLiteral("网络：已停止"));
    updateVideoSendUi();
}

void MainWindow::onSendTestFrameClicked()
{
    if (!m_networkSettingsValid || !m_videoUdpTransport || !m_videoUdpTransport->isBound()) {
        ui->networkStatusLabel->setText(QStringLiteral("网络：尚未完成 UDP 绑定。"));
        return;
    }

    ++m_testFrameSequence;
    if (m_testFrameSequence == 0) {
        m_testFrameSequence = 1;
    }

    const QByteArray frame = createDeterministicTestFrame(m_testFrameSequence);
    if (frame.size() != TestFrameSize) {
        ui->networkStatusLabel->setText(QStringLiteral("网络：创建测试帧失败。"));
        return;
    }

    m_lastSentTestFrameSequence = m_testFrameSequence;
    QString errorMessage;
    if (!m_videoUdpTransport->sendEncodedFrame(frame, &errorMessage)) {
        ui->networkStatusLabel->setText(
            QStringLiteral("网络：发送测试帧失败：%1").arg(errorMessage));
    }
}

void MainWindow::onStartVideoSendClicked()
{
    if (m_shuttingDown || m_videoSending) {
        return;
    }
    if (!m_cameraRunning) {
        ui->videoSendStatusLabel->setText(QStringLiteral("视频发送：请先成功启动摄像头。"));
        return;
    }
    if (!m_networkSettingsValid || !m_videoUdpTransport || !m_videoUdpTransport->isBound()
        || m_peerAddress.protocol() != QAbstractSocket::IPv4Protocol || m_peerVideoPort == 0) {
        ui->videoSendStatusLabel->setText(QStringLiteral("视频发送：请先完成有效 UDP 绑定和对端配置。"));
        return;
    }

    const int targetFps = ui->videoSendFpsSpinBox->value();
    const int jpegQuality = ui->jpegQualitySpinBox->value();
    if (targetFps < 1 || targetFps > 30 || jpegQuality < 1 || jpegQuality > 100) {
        ui->videoSendStatusLabel->setText(QStringLiteral("视频发送：FPS 或 JPEG quality 参数无效。"));
        return;
    }

    m_videoSending = true;
    m_activeVideoFps = targetFps;
    m_activeJpegQuality = jpegQuality;
    m_videoFramesSentTotal = 0;
    m_videoBytesSentTotal = 0;
    m_videoFragmentsSentTotal = 0;
    m_videoFramesSentInterval = 0;
    m_videoBytesSentInterval = 0;
    m_videoFragmentsSentInterval = 0;
    m_videoEncodingDurationUsInterval = 0;
    m_lastJpegSize = 0;
    m_lastFragmentCount = 0;
    m_lastVideoWidth = 0;
    m_lastVideoHeight = 0;

    emit requestStartVideoEncoding(targetFps, jpegQuality);
    if (m_videoStatsTimer) {
        m_videoStatsElapsedTimer.restart();
        m_videoStatsTimer->start();
    }
    ui->videoSendStatusLabel->setText(
        QStringLiteral("视频发送：正在以 %1 FPS、JPEG quality %2 编码并发送")
            .arg(targetFps)
            .arg(jpegQuality));
    updateVideoSendUi();
}

void MainWindow::onStopVideoSendClicked()
{
    stopVideoSending(QStringLiteral("视频发送：已停止。"));
}

void MainWindow::onUdpFrameReceived(const QByteArray &encodedFrame,
                                    quint32 sessionId,
                                    quint32 frameId,
                                    quint32 timestampMs,
                                    const QHostAddress &senderAddress,
                                    quint16 senderPort)
{
    if (!m_networkSettingsValid || !m_videoUdpTransport || !m_videoUdpTransport->isBound()) {
        return;
    }
    if (senderAddress != m_peerAddress || senderPort != m_peerVideoPort) {
        ++m_remoteForeignFramesInterval;
        return;
    }

    if (encodedFrame.startsWith(TestFrameMagic)) {
        quint32 sequence = 0;
        QString errorMessage;
        if (validateDeterministicTestFrame(encodedFrame, &sequence, &errorMessage)) {
            ui->networkStatusLabel->setText(
                QStringLiteral("网络：收到有效测试帧 %1，%2 bytes，来源 %3:%4")
                    .arg(sequence)
                    .arg(encodedFrame.size())
                    .arg(senderAddress.toString())
                    .arg(senderPort));
        } else {
            ui->networkStatusLabel->setText(
                QStringLiteral("网络：确定性测试帧校验失败：%1").arg(errorMessage));
        }
        return;
    }

    if (hasJpegMarkers(encodedFrame)) {
        ++m_remoteJpegFramesReceivedInterval;
        m_remoteJpegBytesReceivedInterval += static_cast<quint64>(encodedFrame.size());

        PendingRemoteJpegFrame frame;
        frame.jpegData = encodedFrame;
        frame.generation = m_remoteReceiveGeneration;
        frame.sessionId = sessionId;
        frame.frameId = frameId;
        frame.timestampMs = timestampMs;
        frame.senderAddress = senderAddress.toString();
        frame.senderPort = senderPort;
        scheduleRemoteJpegDecode(std::move(frame));
        return;
    }

    ++m_remoteUnsupportedFramesInterval;
    reportRemoteDecodeFailure(QStringLiteral("远端接收：收到不支持的完整帧。"));
}

void MainWindow::onUdpFrameSent(quint32 frameId,
                                qsizetype frameSize,
                                qsizetype fragmentCount)
{
    if (m_videoSending) {
        return;
    }

    Q_UNUSED(frameId);
    const quint32 sequence = m_lastSentTestFrameSequence;
    ui->networkStatusLabel->setText(
        QStringLiteral("网络：测试帧 %1 已发送，%2 bytes，%3 个分片")
            .arg(sequence)
            .arg(frameSize)
            .arg(fragmentCount));
}

void MainWindow::onUdpNetworkError(const QString &message)
{
    if (m_videoSending) {
        stopVideoSending(QStringLiteral("视频发送：UDP 本地错误：%1").arg(message));
    }
    ui->networkStatusLabel->setText(QStringLiteral("网络错误：%1").arg(message));
}

void MainWindow::onUdpDatagramRejected(const QString &message)
{
    ui->networkStatusLabel->setText(QStringLiteral("网络：已拒绝数据报：%1").arg(message));
}

void MainWindow::onCameraStarted(const QString &description)
{
    if (m_shuttingDown) {
        return;
    }

    m_cameraRunning = true;
    m_cameraOpening = false;
    ui->startCameraButton->setEnabled(false);
    ui->stopCameraButton->setEnabled(true);
    ui->cameraIndexSpinBox->setEnabled(false);
    setLocalVideoStatus(QStringLiteral("状态：%1").arg(description));
    updateVideoSendUi();
}

void MainWindow::onCameraStopped()
{
    stopVideoSending(QStringLiteral("视频发送：摄像头已停止。"));
    m_cameraRunning = false;
    m_cameraOpening = false;
    m_lastFrame = QImage();

    if (m_shuttingDown) {
        return;
    }

    resetCameraUi();
    setLocalVideoStatus(QStringLiteral("状态：已停止"));
}

void MainWindow::onCameraError(const QString &message)
{
    stopVideoSending(QStringLiteral("视频发送：摄像头错误，已停止。"));
    if (m_shuttingDown) {
        return;
    }

    m_cameraRunning = false;
    m_cameraOpening = false;
    m_lastFrame = QImage();
    resetCameraUi();
    setLocalVideoStatus(QStringLiteral("状态：%1").arg(message));
}

void MainWindow::onCameraDiagnostic(const QString &message)
{
    if (m_shuttingDown || !m_cameraOpening) {
        return;
    }

    setLocalVideoStatus(QStringLiteral("状态：%1").arg(message));
}

void MainWindow::onFrameReady(const QImage &image)
{
    if (m_shuttingDown || !m_cameraRunning || image.isNull()) {
        return;
    }

    m_lastFrame = image;
    updateVideoDisplay();
}

void MainWindow::onJpegFrameReady(const QByteArray &jpegData,
                                  int width,
                                  int height,
                                  int jpegQuality,
                                  qint64 encodingDurationUs)
{
    if (!m_videoSending) {
        return;
    }
    if (jpegData.isEmpty()) {
        stopVideoSending(QStringLiteral("视频发送：收到空 JPEG 数据，已停止。"));
        return;
    }
    if (!m_networkSettingsValid || !m_videoUdpTransport || !m_videoUdpTransport->isBound()) {
        stopVideoSending(QStringLiteral("视频发送：UDP 未绑定，已停止。"));
        return;
    }
    if (jpegData.size() > VideoPacketProtocol::MaximumFrameSize) {
        stopVideoSending(QStringLiteral("视频发送：JPEG 超过 VCL1 单帧 4 MiB 上限，已停止。"));
        return;
    }

    QString sendError;
    if (!m_videoUdpTransport->sendEncodedFrame(jpegData, &sendError)) {
        stopVideoSending(QStringLiteral("视频发送：UDP 发送失败：%1").arg(sendError));
        return;
    }

    const qsizetype fragmentCount =
        (jpegData.size() + VideoPacketProtocol::MaximumPayloadSize - 1)
        / VideoPacketProtocol::MaximumPayloadSize;
    ++m_videoFramesSentTotal;
    m_videoBytesSentTotal += static_cast<quint64>(jpegData.size());
    m_videoFragmentsSentTotal += static_cast<quint64>(fragmentCount);
    ++m_videoFramesSentInterval;
    m_videoBytesSentInterval += static_cast<quint64>(jpegData.size());
    m_videoFragmentsSentInterval += static_cast<quint64>(fragmentCount);
    if (encodingDurationUs > 0) {
        m_videoEncodingDurationUsInterval += static_cast<quint64>(encodingDurationUs);
    }
    m_lastJpegSize = jpegData.size();
    m_lastFragmentCount = fragmentCount;
    m_lastVideoWidth = width;
    m_lastVideoHeight = height;
    m_activeJpegQuality = jpegQuality;
}

void MainWindow::onVideoEncodingError(const QString &message)
{
    if (m_videoSending) {
        stopVideoSending(QStringLiteral("视频发送：JPEG 编码错误：%1").arg(message));
        return;
    }
    ui->videoSendStatusLabel->setText(QStringLiteral("视频发送：JPEG 编码错误：%1").arg(message));
}

void MainWindow::updateVideoSendStatistics()
{
    if (!m_videoSending) {
        return;
    }

    const qint64 elapsedMs = m_videoStatsElapsedTimer.isValid()
        ? m_videoStatsElapsedTimer.restart()
        : 0;
    if (elapsedMs <= 0) {
        m_videoStatsElapsedTimer.restart();
        return;
    }

    if (m_videoFramesSentInterval == 0) {
        ui->videoSendStatusLabel->setText(
            QStringLiteral("视频发送：目标 %1 FPS，实际 0.0 FPS，等待 JPEG 帧……")
                .arg(m_activeVideoFps));
        m_videoBytesSentInterval = 0;
        m_videoFragmentsSentInterval = 0;
        m_videoEncodingDurationUsInterval = 0;
        return;
    }

    const double frames = static_cast<double>(m_videoFramesSentInterval);
    const double actualFps = frames * 1000.0 / static_cast<double>(elapsedMs);
    const double averageJpegKilobytes = static_cast<double>(m_videoBytesSentInterval) / frames / 1024.0;
    const double payloadMegabitsPerSecond =
        static_cast<double>(m_videoBytesSentInterval) * 8.0
        / (static_cast<double>(elapsedMs) * 1000.0);
    const double averageFragments = static_cast<double>(m_videoFragmentsSentInterval) / frames;
    const double averageEncodingMilliseconds =
        static_cast<double>(m_videoEncodingDurationUsInterval) / frames / 1000.0;
    ui->videoSendStatusLabel->setText(
        QStringLiteral("视频发送：目标 %1 FPS，实际 %2 FPS，%3×%4，JPEG %5 KB/帧，%6 Mbit/s，%7 分片/帧，编码 %8 ms/帧")
            .arg(m_activeVideoFps)
            .arg(actualFps, 0, 'f', 1)
            .arg(m_lastVideoWidth)
            .arg(m_lastVideoHeight)
            .arg(averageJpegKilobytes, 0, 'f', 1)
            .arg(payloadMegabitsPerSecond, 0, 'f', 2)
            .arg(averageFragments, 0, 'f', 1)
            .arg(averageEncodingMilliseconds, 0, 'f', 1));

    m_videoFramesSentInterval = 0;
    m_videoBytesSentInterval = 0;
    m_videoFragmentsSentInterval = 0;
    m_videoEncodingDurationUsInterval = 0;
}

void MainWindow::updateVideoDisplay()
{
    if (m_lastFrame.isNull() || ui->videoLabel->width() <= 0 || ui->videoLabel->height() <= 0) {
        return;
    }

    const QImage scaledImage = m_lastFrame.scaled(ui->videoLabel->size(),
                                                   Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation);
    ui->videoLabel->setPixmap(QPixmap::fromImage(scaledImage));
}

void MainWindow::updateRemoteVideoDisplay()
{
    if (m_remoteFrame.isNull() || ui->remoteVideoLabel->width() <= 0
        || ui->remoteVideoLabel->height() <= 0) {
        return;
    }

    const QImage scaledImage = m_remoteFrame.scaled(ui->remoteVideoLabel->size(),
                                                     Qt::KeepAspectRatio,
                                                     Qt::SmoothTransformation);
    ui->remoteVideoLabel->setPixmap(QPixmap::fromImage(scaledImage));
}

void MainWindow::resetRemoteVideoDisplay(const QString &message)
{
    m_remoteFrame = QImage();
    ui->remoteVideoLabel->clear();
    ui->remoteVideoLabel->setToolTip(QString());
    ui->remoteVideoLabel->setText(message);
}

void MainWindow::setLocalVideoStatus(const QString &message)
{
    ui->statusLabel->setText(message);
    ui->localVideoStatusLabel->setText(message);
}

void MainWindow::scheduleRemoteJpegDecode(PendingRemoteJpegFrame frame)
{
    if (m_shuttingDown || frame.generation != m_remoteReceiveGeneration) {
        return;
    }

    if (!m_remoteDecodeBusy) {
        startRemoteJpegDecode(frame);
        return;
    }

    if (m_pendingRemoteJpeg.has_value()) {
        ++m_remoteSupersededFramesInterval;
    }
    m_pendingRemoteJpeg = std::move(frame);
}

void MainWindow::startRemoteJpegDecode(const PendingRemoteJpegFrame &frame)
{
    if (m_shuttingDown || frame.generation != m_remoteReceiveGeneration) {
        return;
    }
    if (!m_remoteDecoder || !m_remoteDecoderThread || !m_remoteDecoderThread->isRunning()) {
        ++m_remoteDecodeFailuresInterval;
        reportRemoteDecodeFailure(QStringLiteral("远端接收：JPEG 解码线程不可用。"));
        return;
    }

    m_remoteDecodeBusy = true;
    emit requestRemoteJpegDecode(frame.jpegData,
                                 frame.generation,
                                 frame.sessionId,
                                 frame.frameId,
                                 frame.timestampMs,
                                 frame.senderAddress,
                                 frame.senderPort);
}

void MainWindow::completeRemoteJpegDecode()
{
    m_remoteDecodeBusy = false;
    if (!m_pendingRemoteJpeg.has_value()) {
        return;
    }

    PendingRemoteJpegFrame nextFrame = std::move(*m_pendingRemoteJpeg);
    m_pendingRemoteJpeg.reset();
    if (m_shuttingDown || !m_networkSettingsValid
        || nextFrame.generation != m_remoteReceiveGeneration) {
        return;
    }

    startRemoteJpegDecode(nextFrame);
}

void MainWindow::onRemoteFrameDecoded(const QImage &image,
                                      qsizetype jpegSize,
                                      qint64 decodingDurationUs,
                                      quint64 generation,
                                      quint32 sessionId,
                                      quint32 frameId,
                                      quint32 timestampMs,
                                      const QString &senderAddress,
                                      quint16 senderPort)
{
    Q_UNUSED(sessionId);
    Q_UNUSED(frameId);
    Q_UNUSED(timestampMs);

    const bool currentGeneration = !m_shuttingDown
        && generation == m_remoteReceiveGeneration
        && m_networkSettingsValid;
    if (currentGeneration && !image.isNull()) {
        m_remoteFrame = image;
        ++m_remoteFramesDecodedInterval;
        if (decodingDurationUs > 0) {
            m_remoteDecodeDurationUsInterval += static_cast<quint64>(decodingDurationUs);
        }
        m_lastRemoteSenderAddress = senderAddress;
        m_lastRemoteSenderPort = senderPort;
        m_lastRemoteFrameWidth = image.width();
        m_lastRemoteFrameHeight = image.height();
        ui->remoteVideoLabel->setToolTip(
            QStringLiteral("远端来源：%1:%2，分辨率：%3×%4")
                .arg(senderAddress)
                .arg(senderPort)
                .arg(image.width())
                .arg(image.height()));
        updateRemoteVideoDisplay();
    }

    Q_UNUSED(jpegSize);
    completeRemoteJpegDecode();
}

void MainWindow::onRemoteFrameDecodeFailed(const QString &message,
                                           qsizetype jpegSize,
                                           qint64 decodingDurationUs,
                                           quint64 generation,
                                           quint32 sessionId,
                                           quint32 frameId,
                                           const QString &senderAddress,
                                           quint16 senderPort)
{
    Q_UNUSED(jpegSize);
    Q_UNUSED(decodingDurationUs);
    Q_UNUSED(sessionId);
    Q_UNUSED(frameId);
    Q_UNUSED(senderAddress);
    Q_UNUSED(senderPort);

    if (!m_shuttingDown && generation == m_remoteReceiveGeneration && m_networkSettingsValid) {
        ++m_remoteDecodeFailuresInterval;
        reportRemoteDecodeFailure(QStringLiteral("远端接收：JPEG 解码失败：%1").arg(message));
    }

    completeRemoteJpegDecode();
}

void MainWindow::updateRemoteReceiveStatistics()
{
    if (!m_networkSettingsValid || !m_videoUdpTransport || !m_videoUdpTransport->isBound()) {
        return;
    }

    const qint64 elapsedMs = m_remoteStatsElapsedTimer.isValid()
        ? m_remoteStatsElapsedTimer.restart()
        : 0;
    if (elapsedMs <= 0) {
        m_remoteStatsElapsedTimer.restart();
        return;
    }

    if (m_remoteJpegFramesReceivedInterval == 0) {
        ui->remoteVideoStatusLabel->setText(QStringLiteral("远端接收：等待对端 JPEG……"));
        m_remoteFramesDecodedInterval = 0;
        m_remoteDecodeDurationUsInterval = 0;
        m_remoteDecodeFailuresInterval = 0;
        m_remoteSupersededFramesInterval = 0;
        m_remoteForeignFramesInterval = 0;
        m_remoteUnsupportedFramesInterval = 0;
        return;
    }

    const double receivedFrames = static_cast<double>(m_remoteJpegFramesReceivedInterval);
    const double decodedFrames = static_cast<double>(m_remoteFramesDecodedInterval);
    const double receivedFps = receivedFrames * 1000.0 / static_cast<double>(elapsedMs);
    const double decodedFps = decodedFrames * 1000.0 / static_cast<double>(elapsedMs);
    const double averageJpegKilobytes =
        static_cast<double>(m_remoteJpegBytesReceivedInterval) / receivedFrames / 1024.0;
    const double payloadMegabitsPerSecond =
        static_cast<double>(m_remoteJpegBytesReceivedInterval) * 8.0
        / (static_cast<double>(elapsedMs) * 1000.0);
    const double averageDecodingMilliseconds = decodedFrames > 0.0
        ? static_cast<double>(m_remoteDecodeDurationUsInterval) / decodedFrames / 1000.0
        : 0.0;
    const QString sender = m_lastRemoteSenderAddress.isEmpty()
        ? m_peerAddress.toString()
        : m_lastRemoteSenderAddress;
    const quint16 senderPort = m_lastRemoteSenderPort == 0
        ? m_peerVideoPort
        : m_lastRemoteSenderPort;
    ui->remoteVideoStatusLabel->setText(
        QStringLiteral("远端接收：%1:%2，%3×%4，接收 %5 FPS，显示 %6 FPS，JPEG %7 KB/帧，%8 Mbit/s，解码 %9 ms/帧，积压覆盖 %10，失败 %11，外源 %12，不支持 %13")
            .arg(sender)
            .arg(senderPort)
            .arg(m_lastRemoteFrameWidth)
            .arg(m_lastRemoteFrameHeight)
            .arg(receivedFps, 0, 'f', 1)
            .arg(decodedFps, 0, 'f', 1)
            .arg(averageJpegKilobytes, 0, 'f', 1)
            .arg(payloadMegabitsPerSecond, 0, 'f', 2)
            .arg(averageDecodingMilliseconds, 0, 'f', 1)
            .arg(m_remoteSupersededFramesInterval)
            .arg(m_remoteDecodeFailuresInterval)
            .arg(m_remoteForeignFramesInterval)
            .arg(m_remoteUnsupportedFramesInterval));

    m_remoteJpegFramesReceivedInterval = 0;
    m_remoteJpegBytesReceivedInterval = 0;
    m_remoteFramesDecodedInterval = 0;
    m_remoteDecodeDurationUsInterval = 0;
    m_remoteDecodeFailuresInterval = 0;
    m_remoteSupersededFramesInterval = 0;
    m_remoteForeignFramesInterval = 0;
    m_remoteUnsupportedFramesInterval = 0;
}

void MainWindow::resetRemoteReceiveState(const QString &message)
{
    advanceRemoteReceiveGeneration();
    m_pendingRemoteJpeg.reset();
    clearRemoteReceiveStatistics();
    if (!m_remoteDecoder || !m_remoteDecoderThread || !m_remoteDecoderThread->isRunning()) {
        m_remoteDecodeBusy = false;
    }
    if (m_remoteStatsTimer) {
        m_remoteStatsTimer->stop();
    }
    m_remoteStatsElapsedTimer.invalidate();
    m_remoteDecodeErrorTimer.invalidate();
    m_lastRemoteDecodeError.clear();
    resetRemoteVideoDisplay(QStringLiteral("尚未收到远端视频"));
    ui->remoteVideoStatusLabel->setText(message);
}

void MainWindow::clearRemoteReceiveStatistics()
{
    m_remoteJpegFramesReceivedInterval = 0;
    m_remoteJpegBytesReceivedInterval = 0;
    m_remoteFramesDecodedInterval = 0;
    m_remoteDecodeDurationUsInterval = 0;
    m_remoteDecodeFailuresInterval = 0;
    m_remoteSupersededFramesInterval = 0;
    m_remoteForeignFramesInterval = 0;
    m_remoteUnsupportedFramesInterval = 0;
    m_lastRemoteSenderAddress.clear();
    m_lastRemoteSenderPort = 0;
    m_lastRemoteFrameWidth = 0;
    m_lastRemoteFrameHeight = 0;
}

void MainWindow::advanceRemoteReceiveGeneration()
{
    ++m_remoteReceiveGeneration;
    if (m_remoteReceiveGeneration == 0) {
        m_remoteReceiveGeneration = 1;
    }
}

void MainWindow::reportRemoteDecodeFailure(const QString &message)
{
    constexpr qint64 errorReportIntervalMs = 1000;
    if (m_lastRemoteDecodeError == message && m_remoteDecodeErrorTimer.isValid()
        && m_remoteDecodeErrorTimer.elapsed() < errorReportIntervalMs) {
        return;
    }

    m_lastRemoteDecodeError = message;
    m_remoteDecodeErrorTimer.restart();
    qWarning().noquote() << QStringLiteral("[MainWindow]") << message;
    if (!m_shuttingDown) {
        ui->remoteVideoStatusLabel->setText(message);
    }
}

void MainWindow::resetCameraUi()
{
    if (m_shuttingDown) {
        return;
    }

    ui->startCameraButton->setEnabled(true);
    ui->stopCameraButton->setEnabled(false);
    ui->cameraIndexSpinBox->setEnabled(true);
    ui->videoLabel->clear();
    ui->videoLabel->setText(QStringLiteral("摄像头未启动"));
    setLocalVideoStatus(QStringLiteral("状态：未启动"));
    updateVideoSendUi();
}

void MainWindow::stopVideoSending(const QString &reason)
{
    const bool wasSending = m_videoSending;
    if (wasSending) {
        emit requestStopVideoEncoding();
    }

    m_videoSending = false;
    if (m_videoStatsTimer) {
        m_videoStatsTimer->stop();
    }
    m_videoFramesSentInterval = 0;
    m_videoBytesSentInterval = 0;
    m_videoFragmentsSentInterval = 0;
    m_videoEncodingDurationUsInterval = 0;
    m_videoStatsElapsedTimer.invalidate();

    if (wasSending && !reason.isEmpty()) {
        ui->videoSendStatusLabel->setText(reason);
    }
    updateVideoSendUi();
}

void MainWindow::updateVideoSendUi()
{
    const bool udpReady = m_networkSettingsValid
        && m_videoUdpTransport
        && m_videoUdpTransport->isBound()
        && m_peerAddress.protocol() == QAbstractSocket::IPv4Protocol
        && m_peerVideoPort != 0;
    const bool controlsEnabled = !m_shuttingDown && !m_videoSending;

    ui->startVideoSendButton->setEnabled(
        controlsEnabled && m_cameraRunning && udpReady);
    ui->stopVideoSendButton->setEnabled(!m_shuttingDown && m_videoSending);
    ui->videoSendFpsSpinBox->setEnabled(controlsEnabled);
    ui->jpegQualitySpinBox->setEnabled(controlsEnabled);
    ui->sendTestFrameButton->setEnabled(controlsEnabled && udpReady);
}

QByteArray MainWindow::createDeterministicTestFrame(quint32 sequence) const
{
    QByteArray header;
    QBuffer buffer(&header);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {};
    }

    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::BigEndian);
    if (stream.writeRawData(TestFrameMagic.constData(), TestFrameMagic.size())
            != TestFrameMagic.size()) {
        return {};
    }
    stream << sequence;
    if (stream.status() != QDataStream::Ok || header.size() >= TestFrameSize) {
        return {};
    }

    QByteArray frame = header;
    frame.resize(TestFrameSize);
    for (qsizetype index = header.size(); index < frame.size(); ++index) {
        frame[index] = static_cast<char>(
            (static_cast<quint64>(index) * 31
             + static_cast<quint64>(sequence) * 17)
            & 0xffULL);
    }
    return frame;
}

bool MainWindow::validateDeterministicTestFrame(const QByteArray &frame,
                                                quint32 *sequence,
                                                QString *errorMessage) const
{
    constexpr qsizetype sequenceSize = sizeof(quint32);
    const qsizetype headerSize = TestFrameMagic.size() + sequenceSize;
    if (frame.size() != TestFrameSize) {
        setTestFrameError(errorMessage, QStringLiteral("测试帧长度不是 50000 bytes。"));
        return false;
    }
    if (frame.size() < headerSize || !frame.startsWith(TestFrameMagic)) {
        setTestFrameError(errorMessage, QStringLiteral("测试帧标识无效。"));
        return false;
    }

    QBuffer buffer;
    buffer.setData(frame.mid(TestFrameMagic.size(), sequenceSize));
    if (!buffer.open(QIODevice::ReadOnly)) {
        setTestFrameError(errorMessage, QStringLiteral("无法读取测试帧序号。"));
        return false;
    }

    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 parsedSequence = 0;
    stream >> parsedSequence;
    if (stream.status() != QDataStream::Ok) {
        setTestFrameError(errorMessage, QStringLiteral("测试帧序号格式无效。"));
        return false;
    }

    for (qsizetype index = headerSize; index < frame.size(); ++index) {
        const char expected = static_cast<char>(
            (static_cast<quint64>(index) * 31
             + static_cast<quint64>(parsedSequence) * 17)
            & 0xffULL);
        if (frame.at(index) != expected) {
            setTestFrameError(errorMessage, QStringLiteral("测试帧内容模式不匹配。"));
            return false;
        }
    }

    if (sequence) {
        *sequence = parsedSequence;
    }
    clearTestFrameError(errorMessage);
    return true;
}

void MainWindow::shutdownRemoteDecoderThread()
{
    m_pendingRemoteJpeg.reset();

    QThread *decoderThread = m_remoteDecoderThread;
    if (!decoderThread) {
        m_remoteDecodeBusy = false;
        return;
    }

    const bool calledFromDecoderThread = QThread::currentThread() == decoderThread;
    Q_ASSERT(!calledFromDecoderThread);
    if (calledFromDecoderThread) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] 禁止在远端解码线程中等待自身退出。");
        return;
    }

    if (m_remoteDecoder) {
        disconnect(this,
                   &MainWindow::requestRemoteJpegDecode,
                   m_remoteDecoder,
                   &RemoteVideoDecoder::decodeJpeg);
        disconnect(m_remoteDecoder,
                   &RemoteVideoDecoder::frameDecoded,
                   this,
                   &MainWindow::onRemoteFrameDecoded);
        disconnect(m_remoteDecoder,
                   &RemoteVideoDecoder::frameDecodeFailed,
                   this,
                   &MainWindow::onRemoteFrameDecodeFailed);
    }

    qInfo().noquote() << QStringLiteral("[MainWindow] 开始 quit 远端 JPEG 解码线程。");
    decoderThread->quit();
    const bool waitSucceeded = decoderThread->wait();
    qInfo().noquote()
        << QStringLiteral("[MainWindow] 远端 JPEG 解码线程 wait 返回：") << waitSucceeded;

    Q_ASSERT(waitSucceeded);
    if (!waitSucceeded) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] 远端 JPEG 解码线程未结束，拒绝删除 QThread。");
        return;
    }

    m_remoteDecodeBusy = false;
    m_remoteDecoderThread = nullptr;
    m_remoteDecoder = nullptr;
    qInfo().noquote() << QStringLiteral("[MainWindow] 删除远端 JPEG 解码 QThread。");
    delete decoderThread;
}

void MainWindow::shutdownCameraThread()
{
    if (m_shuttingDown) {
        return;
    }

    m_shuttingDown = true;
    m_cameraOpening = false;

    QThread *cameraThread = m_cameraThread;
    if (!cameraThread) {
        return;
    }

    const bool calledFromCameraThread = QThread::currentThread() == cameraThread;
    Q_ASSERT(!calledFromCameraThread);
    if (calledFromCameraThread) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] 禁止在摄像头线程中等待自身退出。");
        return;
    }

    if (!m_cameraWorker) {
        qCritical().noquote() << QStringLiteral("[MainWindow] CameraWorker 不可用，无法执行安全退出。");
        return;
    }

    qInfo().noquote() << QStringLiteral("[MainWindow] 开始停止 CameraWorker。");
    const bool stopInvoked = QMetaObject::invokeMethod(
        m_cameraWorker,
        &CameraWorker::stopCamera,
        Qt::BlockingQueuedConnection);
    if (!stopInvoked) {
        qCritical().noquote() << QStringLiteral("[MainWindow] stopCamera 调用未投递。");
        return;
    }
    qInfo().noquote() << QStringLiteral("[MainWindow] stopCamera 完成。");

    qInfo().noquote() << QStringLiteral("[MainWindow] 开始 quit 摄像头线程。");
    const bool quitInvoked = QMetaObject::invokeMethod(
        m_cameraWorker,
        [] { QThread::currentThread()->quit(); },
        Qt::BlockingQueuedConnection);
    if (!quitInvoked) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] QThread::quit() 调用未投递。");
        return;
    }

    const bool waitSucceeded = cameraThread->wait();
    qInfo().noquote()
        << QStringLiteral("[MainWindow] 摄像头线程 wait 返回：") << waitSucceeded;

    Q_ASSERT(waitSucceeded);
    if (!waitSucceeded) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] 摄像头线程未结束，拒绝删除 QThread。");
        return;
    }

    m_cameraThread = nullptr;
    m_cameraWorker = nullptr;

    qInfo().noquote() << QStringLiteral("[MainWindow] 删除 QThread。");
    delete cameraThread;
}
