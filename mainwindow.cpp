#include "mainwindow.h"
#include "audioworker.h"
#include "cameraworker.h"
#include "remotevideodecoder.h"
#include "videopacketprotocol.h"
#include "videoudptransport.h"

#include "./ui_mainwindow.h"

#include <QBuffer>
#include <QCloseEvent>
#include <QComboBox>
#include <QDataStream>
#include <QDebug>
#include <QFontMetrics>
#include <QIODevice>
#include <QLabel>
#include <QMetaObject>
#include <QNetworkInterface>
#include <QPixmap>
#include <QResizeEvent>
#include <QSet>
#include <QShowEvent>
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
    refreshLocalIpv4Addresses();
    setAudioStatus(QStringLiteral("音频：网络未配置"));
    updateVideoLabelGeometry(ui->localVideoContainer, ui->videoLabel);
    updateVideoLabelGeometry(ui->remoteVideoContainer, ui->remoteVideoLabel);

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

    qRegisterMetaType<AudioStatistics>("AudioStatistics");
    m_audioThread = new QThread;
    m_audioWorker = new AudioWorker;
    m_audioWorker->moveToThread(m_audioThread);

    connect(m_audioThread,
            &QThread::finished,
            m_audioWorker,
            &QObject::deleteLater);
    connect(this,
            &MainWindow::requestConfigureAudioNetwork,
            m_audioWorker,
            &AudioWorker::configureNetwork,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::requestStartAudio,
            m_audioWorker,
            &AudioWorker::startAudio,
            Qt::QueuedConnection);
    connect(this,
            &MainWindow::requestStopAudio,
            m_audioWorker,
            &AudioWorker::stopAudio,
            Qt::QueuedConnection);
    connect(m_audioWorker,
            &AudioWorker::audioNetworkReady,
            this,
            &MainWindow::onAudioNetworkReady,
            Qt::QueuedConnection);
    connect(m_audioWorker,
            &AudioWorker::audioStarted,
            this,
            &MainWindow::onAudioStarted,
            Qt::QueuedConnection);
    connect(m_audioWorker,
            &AudioWorker::audioStopped,
            this,
            &MainWindow::onAudioStopped,
            Qt::QueuedConnection);
    connect(m_audioWorker,
            &AudioWorker::audioError,
            this,
            &MainWindow::onAudioError,
            Qt::QueuedConnection);
    connect(m_audioWorker,
            &AudioWorker::audioStatisticsUpdated,
            this,
            &MainWindow::onAudioStatisticsUpdated,
            Qt::QueuedConnection);
    m_audioThread->start();

    m_cameraThread = new QThread;
    m_cameraWorker = new CameraWorker;
    m_cameraWorker->moveToThread(m_cameraThread);

    connect(m_cameraThread,
            &QThread::finished,
            m_cameraWorker,
            &QObject::deleteLater);

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
    connect(ui->applyAudioSettingsButton,
            &QPushButton::clicked,
            this,
            &MainWindow::onApplyAudioSettingsClicked);
    connect(ui->startAudioButton,
            &QPushButton::clicked,
            this,
            &MainWindow::onStartAudioClicked);
    connect(ui->stopAudioButton,
            &QPushButton::clicked,
            this,
            &MainWindow::onStopAudioClicked);
    connect(ui->refreshLocalAddressButton,
            &QPushButton::clicked,
            this,
            &MainWindow::refreshLocalIpv4Addresses);

    resetCameraUi();
    updateVideoSendUi();
    m_cameraThread->start();
}

MainWindow::~MainWindow()
{
    shutdownAll();
    delete ui;
    ui = nullptr;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    shutdownAll();
    QMainWindow::closeEvent(event);
}

void MainWindow::shutdownAll()
{
    if (m_shuttingDown) {
        return;
    }

    m_shuttingDown = true;
    m_cameraRunning = false;
    m_cameraOpening = false;
    m_videoSending = false;
    m_audioRunning = false;
    m_audioNetworkSettingsValid = false;
    m_networkSettingsValid = false;
    m_pendingRemoteJpeg.reset();
    m_remoteDecodeBusy = false;
    advanceRemoteReceiveGeneration();

    if (m_videoStatsTimer) {
        m_videoStatsTimer->stop();
    }
    if (m_remoteStatsTimer) {
        m_remoteStatsTimer->stop();
    }

    if (m_videoUdpTransport) {
        disconnect(m_videoUdpTransport, nullptr, this, nullptr);
        m_videoUdpTransport->close();
    }
    if (m_cameraWorker) {
        disconnect(this, nullptr, m_cameraWorker.data(), nullptr);
        disconnect(m_cameraWorker.data(), nullptr, this, nullptr);
    }
    if (m_audioWorker) {
        disconnect(this, nullptr, m_audioWorker.data(), nullptr);
        disconnect(m_audioWorker.data(), nullptr, this, nullptr);
    }
    if (m_remoteDecoder) {
        disconnect(this, nullptr, m_remoteDecoder.data(), nullptr);
        disconnect(m_remoteDecoder.data(), nullptr, this, nullptr);
    }

    qInfo().noquote() << QStringLiteral("[MainWindow] shutdownAll: stopping worker threads.");
    shutdownAudioThread();
    shutdownRemoteDecoderThread();
    shutdownCameraThread();

    m_shutdownCompleted = !m_audioThread && !m_remoteDecoderThread && !m_cameraThread
        && m_audioWorker.isNull() && m_remoteDecoder.isNull() && m_cameraWorker.isNull();
    qInfo().noquote() << QStringLiteral("[MainWindow] shutdownAll complete; workers destroyed:")
                      << m_shutdownCompleted;
    emit shutdownCompleted(m_shutdownCompleted);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateVideoLabelGeometry(ui->localVideoContainer, ui->videoLabel);
    updateVideoLabelGeometry(ui->remoteVideoContainer, ui->remoteVideoLabel);
    updateVideoDisplay();
    updateRemoteVideoDisplay();
    refreshAudioStatusText();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    updateVideoLabelGeometry(ui->localVideoContainer, ui->videoLabel);
    updateVideoLabelGeometry(ui->remoteVideoContainer, ui->remoteVideoLabel);
}

void MainWindow::updateVideoLabelGeometry(QWidget *container, QLabel *label)
{
    if (!container || !label) {
        return;
    }

    const QRect availableRect = container->contentsRect();
    const int width = qMin(availableRect.width(), availableRect.height() * 4 / 3);
    const int height = width * 3 / 4;
    const int x = availableRect.x() + (availableRect.width() - width) / 2;
    const int y = availableRect.y() + (availableRect.height() - height) / 2;
    label->setGeometry(x, y, width, height);
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

    QHostAddress localAddress;
    QString localAddressError;
    if (!selectedLocalIpv4Address(&localAddress, &localAddressError)) {
        if (m_videoUdpTransport) {
            m_videoUdpTransport->close();
        }
        m_networkSettingsValid = false;
        ui->applyNetworkSettingsButton->setEnabled(true);
        ui->stopNetworkButton->setEnabled(false);
        ui->sendTestFrameButton->setEnabled(false);
        ui->networkStatusLabel->setText(QStringLiteral("网络：%1").arg(localAddressError));
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
    if (!m_videoUdpTransport->bindReceiver(localAddress,
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
        QStringLiteral("网络：已绑定 %1:%2 → %3:%4")
            .arg(localAddress.toString())
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

void MainWindow::onApplyAudioSettingsClicked()
{
    if (m_shuttingDown) {
        return;
    }

    const QString peerAddress = ui->peerAddressLineEdit->text().trimmed();
    QHostAddress parsedAddress;
    const bool validIpv4 = parsedAddress.setAddress(peerAddress)
        && parsedAddress.protocol() == QAbstractSocket::IPv4Protocol;
    if (!validIpv4) {
        m_audioNetworkSettingsValid = false;
        m_audioRunning = false;
        ui->startAudioButton->setEnabled(false);
        ui->stopAudioButton->setEnabled(false);
        setAudioStatus(QStringLiteral("音频：对端 IPv4 地址无效"));
        return;
    }

    QHostAddress localAddress;
    QString localAddressError;
    if (!selectedLocalIpv4Address(&localAddress, &localAddressError)) {
        m_audioNetworkSettingsValid = false;
        m_audioRunning = false;
        ui->startAudioButton->setEnabled(false);
        ui->stopAudioButton->setEnabled(false);
        setAudioStatus(QStringLiteral("音频：%1").arg(localAddressError));
        return;
    }
    if (!m_audioWorker || !m_audioThread || !m_audioThread->isRunning()) {
        m_audioNetworkSettingsValid = false;
        m_audioRunning = false;
        ui->startAudioButton->setEnabled(false);
        ui->stopAudioButton->setEnabled(false);
        setAudioStatus(QStringLiteral("音频：工作线程不可用。"));
        return;
    }

    m_audioNetworkSettingsValid = false;
    m_audioRunning = false;
    ui->startAudioButton->setEnabled(false);
    ui->stopAudioButton->setEnabled(false);
    setAudioStatus(QStringLiteral("音频：正在配置网络……"));
    emit requestConfigureAudioNetwork(localAddress.toString(),
                                      peerAddress,
                                      static_cast<quint16>(ui->localAudioPortSpinBox->value()),
                                      static_cast<quint16>(ui->peerAudioPortSpinBox->value()));
}

void MainWindow::refreshLocalIpv4Addresses()
{
    const QString previousAddress = ui->localAddressComboBox->currentData().toString();
    ui->localAddressComboBox->clear();
    ui->localAddressComboBox->addItem(QStringLiteral("127.0.0.1（回环，仅用于同机测试）"),
                                      QHostAddress(QHostAddress::LocalHost).toString());

    QSet<QString> addedAddresses;
    addedAddresses.insert(QHostAddress(QHostAddress::LocalHost).toString());
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &interface : interfaces) {
        const QNetworkInterface::InterfaceFlags flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }

        const QString interfaceName = interface.humanReadableName().trimmed().isEmpty()
            ? interface.name()
            : interface.humanReadableName();
        const QList<QNetworkAddressEntry> entries = interface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol
                || address.isNull()
                || address == QHostAddress::LocalHost) {
                continue;
            }

            const QString addressText = address.toString();
            if (addedAddresses.contains(addressText)) {
                continue;
            }
            addedAddresses.insert(addressText);
            ui->localAddressComboBox->addItem(
                QStringLiteral("%1（%2）").arg(addressText, interfaceName),
                addressText);
        }
    }

    const int previousIndex = ui->localAddressComboBox->findData(previousAddress);
    if (previousIndex >= 0) {
        ui->localAddressComboBox->setCurrentIndex(previousIndex);
    }
}

bool MainWindow::selectedLocalIpv4Address(QHostAddress *address, QString *errorMessage) const
{
    const QString addressText = ui->localAddressComboBox->currentData().toString().trimmed();
    QHostAddress parsedAddress;
    if (addressText.isEmpty()
        || !parsedAddress.setAddress(addressText)
        || parsedAddress.protocol() != QAbstractSocket::IPv4Protocol) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("请选择有效的本机 IPv4 地址；网卡状态变化后请刷新 IP 列表。");
        }
        return false;
    }

    if (address) {
        *address = parsedAddress;
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

void MainWindow::onStartAudioClicked()
{
    if (m_shuttingDown || m_audioRunning) {
        return;
    }
    if (!m_audioNetworkSettingsValid || !m_audioWorker || !m_audioThread
        || !m_audioThread->isRunning()) {
        setAudioStatus(QStringLiteral("音频：请先完成有效的音频网络配置。"));
        return;
    }

    ui->startAudioButton->setEnabled(false);
    ui->stopAudioButton->setEnabled(false);
    setAudioStatus(QStringLiteral("音频：正在启动默认输入和输出设备……"));
    emit requestStartAudio();
}

void MainWindow::onStopAudioClicked()
{
    if (m_shuttingDown || !m_audioWorker || !m_audioThread || !m_audioThread->isRunning()) {
        return;
    }

    ui->startAudioButton->setEnabled(false);
    ui->stopAudioButton->setEnabled(false);
    setAudioStatus(QStringLiteral("音频：正在停止……"));
    emit requestStopAudio();
}

void MainWindow::onAudioNetworkReady(const QString &message)
{
    if (m_shuttingDown) {
        return;
    }

    m_audioNetworkSettingsValid = true;
    m_audioRunning = false;
    ui->startAudioButton->setEnabled(true);
    ui->stopAudioButton->setEnabled(false);
    setAudioStatus(message, QString(), message);
}

void MainWindow::onAudioStarted(const QString &message)
{
    if (m_shuttingDown) {
        return;
    }

    m_audioRunning = true;
    ui->startAudioButton->setEnabled(false);
    ui->stopAudioButton->setEnabled(true);
    setAudioStatus(message, QString(), message);
}

void MainWindow::onAudioStopped()
{
    m_audioRunning = false;
    if (m_shuttingDown) {
        return;
    }

    ui->startAudioButton->setEnabled(m_audioNetworkSettingsValid);
    ui->stopAudioButton->setEnabled(false);
    setAudioStatus(QStringLiteral("音频：已停止（网络保持绑定）"));
}

void MainWindow::onAudioError(const QString &message)
{
    m_audioRunning = false;
    if (m_shuttingDown) {
        return;
    }

    ui->startAudioButton->setEnabled(m_audioNetworkSettingsValid);
    ui->stopAudioButton->setEnabled(false);
    setAudioStatus(QStringLiteral("音频：%1").arg(message), QString(), message);
}

void MainWindow::onAudioStatisticsUpdated(const AudioStatistics &statistics)
{
    if (m_shuttingDown || !m_audioRunning) {
        return;
    }

    const QString primaryText =
        QStringLiteral("音频：发送 %1 包/s｜接收 %2 包/s｜发送 %3 Mbit/s｜接收 %4 Mbit/s｜抖动 %5 包/%6 ms")
            .arg(statistics.sentPacketsPerSecond, 0, 'f', 1)
            .arg(statistics.receivedPacketsPerSecond, 0, 'f', 1)
            .arg(statistics.sentPayloadMegabitsPerSecond, 0, 'f', 3)
            .arg(statistics.receivedPayloadMegabitsPerSecond, 0, 'f', 3)
            .arg(statistics.jitterBufferedPackets)
            .arg(statistics.jitterBufferedMilliseconds);
    const QString secondaryText =
        QStringLiteral("补偿 %1｜重复 %2｜迟到 %3｜外源 %4｜无效 %5｜采集溢出 %6｜播放溢出 %7")
            .arg(statistics.concealedPackets)
            .arg(statistics.duplicatePackets)
            .arg(statistics.latePackets)
            .arg(statistics.foreignPackets)
            .arg(statistics.invalidPackets)
            .arg(statistics.captureOverruns)
            .arg(statistics.playbackOverruns);
    const QString details =
        QStringLiteral("输入设备：%1\n输出设备：%2\n输入缓冲：%3 bytes\n输出缓冲：%4 bytes\n当前 PCM 格式：16 kHz / mono / Int16")
            .arg(statistics.inputDeviceDescription)
            .arg(statistics.outputDeviceDescription)
            .arg(statistics.sourceBufferSize)
            .arg(statistics.sinkBufferSize);
    setAudioStatus(primaryText, secondaryText, details);
}

void MainWindow::onUdpFrameReceived(const QByteArray &encodedFrame,
                                    quint32 sessionId,
                                    quint32 frameId,
                                    quint32 timestampMs,
                                    const QHostAddress &senderAddress,
                                    quint16 senderPort)
{
    if (m_shuttingDown || !m_networkSettingsValid || !m_videoUdpTransport
        || !m_videoUdpTransport->isBound()) {
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
    if (m_shuttingDown || m_videoSending) {
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
    if (m_shuttingDown) {
        return;
    }
    if (m_videoSending) {
        stopVideoSending(QStringLiteral("视频发送：UDP 本地错误：%1").arg(message));
    }
    ui->networkStatusLabel->setText(QStringLiteral("网络错误：%1").arg(message));
}

void MainWindow::onUdpDatagramRejected(const QString &message)
{
    if (m_shuttingDown) {
        return;
    }
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
    if (m_shuttingDown) {
        return;
    }
    stopVideoSending(QStringLiteral("视频发送：摄像头已停止。"));
    m_cameraRunning = false;
    m_cameraOpening = false;
    m_lastFrame = QImage();

    resetCameraUi();
    setLocalVideoStatus(QStringLiteral("状态：已停止"));
}

void MainWindow::onCameraError(const QString &message)
{
    if (m_shuttingDown) {
        return;
    }

    stopVideoSending(QStringLiteral("视频发送：摄像头错误，已停止。"));
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
    if (m_shuttingDown || !m_videoSending) {
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
    if (m_shuttingDown) {
        return;
    }
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
    const QSize targetSize = ui->videoLabel->contentsRect().size();
    if (m_lastFrame.isNull() || targetSize.width() <= 0 || targetSize.height() <= 0) {
        return;
    }

    const QImage scaledImage = m_lastFrame.scaled(targetSize,
                                                   Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation);
    ui->videoLabel->setText(QString());
    ui->videoLabel->setPixmap(QPixmap::fromImage(scaledImage));
}

void MainWindow::updateRemoteVideoDisplay()
{
    const QSize targetSize = ui->remoteVideoLabel->contentsRect().size();
    if (m_remoteFrame.isNull() || targetSize.width() <= 0 || targetSize.height() <= 0) {
        return;
    }

    const QImage scaledImage = m_remoteFrame.scaled(targetSize,
                                                     Qt::KeepAspectRatio,
                                                     Qt::SmoothTransformation);
    ui->remoteVideoLabel->setText(QString());
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

void MainWindow::setAudioStatus(const QString &primaryText,
                                const QString &secondaryText,
                                const QString &details)
{
    m_audioPrimaryStatusText = primaryText.simplified();
    m_audioSecondaryStatusText = secondaryText.simplified();
    m_audioStatusDetails = details.isEmpty()
        ? m_audioPrimaryStatusText
              + (m_audioSecondaryStatusText.isEmpty()
                     ? QString()
                     : QLatin1Char('\n') + m_audioSecondaryStatusText)
        : details;
    refreshAudioStatusText();
}

void MainWindow::refreshAudioStatusText()
{
    if (!ui || !ui->audioStatusLabel || !ui->audioSecondaryStatusLabel) {
        return;
    }

    ui->audioStatusLabel->setText(elidedTextForLabel(ui->audioStatusLabel,
                                                      m_audioPrimaryStatusText));
    ui->audioSecondaryStatusLabel->setText(elidedTextForLabel(ui->audioSecondaryStatusLabel,
                                                               m_audioSecondaryStatusText));
    ui->audioStatusPanel->setToolTip(m_audioStatusDetails);
    ui->audioStatusLabel->setToolTip(m_audioStatusDetails);
    ui->audioSecondaryStatusLabel->setToolTip(m_audioStatusDetails);
}

QString MainWindow::elidedTextForLabel(const QLabel *label, const QString &text) const
{
    if (!label || text.isEmpty()) {
        return text;
    }

    const int availableWidth = label->contentsRect().width();
    return availableWidth > 0
        ? QFontMetrics(label->font()).elidedText(text, Qt::ElideRight, availableWidth)
        : text;
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
    QString remoteStatus = QStringLiteral("%1:%2｜%3×%4｜接收 %5 FPS｜显示 %6 FPS\nJPEG %7 KB｜%8 Mbit/s｜解码 %9 ms｜覆盖 %10｜失败 %11")
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
                               .arg(m_remoteDecodeFailuresInterval);
    if (m_remoteForeignFramesInterval != 0 || m_remoteUnsupportedFramesInterval != 0) {
        remoteStatus += QStringLiteral("｜外源 %1｜不支持 %2")
                            .arg(m_remoteForeignFramesInterval)
                            .arg(m_remoteUnsupportedFramesInterval);
    }
    ui->remoteVideoStatusLabel->setText(remoteStatus);

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

void MainWindow::shutdownAudioThread()
{
    QThread *audioThread = m_audioThread;
    if (!audioThread) {
        return;
    }

    const bool calledFromAudioThread = QThread::currentThread() == audioThread;
    if (calledFromAudioThread) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] 禁止在音频线程中等待自身退出。");
        return;
    }

    if (audioThread->isRunning() && m_audioWorker) {
        qInfo().nospace() << "[MainWindow] shutdown AudioWorker this="
                          << static_cast<const void *>(m_audioWorker.data())
                          << " thread=" << audioThread;
        const bool shutdownInvoked = QMetaObject::invokeMethod(
            m_audioWorker.data(),
            &AudioWorker::shutdown,
            Qt::BlockingQueuedConnection);
        if (!shutdownInvoked) {
            qCritical().noquote() << QStringLiteral("[MainWindow] AudioWorker::shutdown 调用未投递。");
        }
    }

    audioThread->quit();
    const bool waitSucceeded = audioThread->wait();
    if (!waitSucceeded) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] 音频线程未结束，拒绝删除 QThread。");
        return;
    }
    if (m_audioWorker) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] AudioWorker 在线程结束后未销毁，拒绝删除其 QThread。");
        return;
    }

    m_audioThread = nullptr;
    delete audioThread;
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
    if (calledFromDecoderThread) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] 禁止在远端解码线程中等待自身退出。");
        return;
    }

    if (decoderThread->isRunning() && m_remoteDecoder) {
        qInfo().nospace() << "[MainWindow] shutdown RemoteVideoDecoder this="
                          << static_cast<const void *>(m_remoteDecoder.data())
                          << " thread=" << decoderThread;
        const bool shutdownInvoked = QMetaObject::invokeMethod(
            m_remoteDecoder.data(),
            &RemoteVideoDecoder::shutdown,
            Qt::BlockingQueuedConnection);
        if (!shutdownInvoked) {
            qCritical().noquote()
                << QStringLiteral("[MainWindow] RemoteVideoDecoder::shutdown 调用未投递。");
        }
    }

    decoderThread->quit();
    const bool waitSucceeded = decoderThread->wait();
    if (!waitSucceeded) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] 远端 JPEG 解码线程未结束，拒绝删除 QThread。");
        return;
    }
    if (m_remoteDecoder) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] RemoteVideoDecoder 在线程结束后未销毁，拒绝删除其 QThread。");
        return;
    }

    m_remoteDecodeBusy = false;
    m_remoteDecoderThread = nullptr;
    delete decoderThread;
}

void MainWindow::shutdownCameraThread()
{
    QThread *cameraThread = m_cameraThread;
    if (!cameraThread) {
        return;
    }

    const bool calledFromCameraThread = QThread::currentThread() == cameraThread;
    if (calledFromCameraThread) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] 禁止在摄像头线程中等待自身退出。");
        return;
    }

    if (cameraThread->isRunning() && m_cameraWorker) {
        qInfo().nospace() << "[MainWindow] shutdown CameraWorker this="
                          << static_cast<const void *>(m_cameraWorker.data())
                          << " thread=" << cameraThread;
        const bool shutdownInvoked = QMetaObject::invokeMethod(
            m_cameraWorker.data(),
            &CameraWorker::shutdown,
            Qt::BlockingQueuedConnection);
        if (!shutdownInvoked) {
            qCritical().noquote() << QStringLiteral("[MainWindow] CameraWorker::shutdown 调用未投递。");
        }
    }

    cameraThread->quit();
    const bool waitSucceeded = cameraThread->wait();
    if (!waitSucceeded) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] 摄像头线程未结束，拒绝删除 QThread。");
        return;
    }
    if (m_cameraWorker) {
        qCritical().noquote()
            << QStringLiteral("[MainWindow] CameraWorker 在线程结束后未销毁，拒绝删除其 QThread。");
        return;
    }

    m_cameraThread = nullptr;
    delete cameraThread;
}
