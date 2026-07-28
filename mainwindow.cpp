#include "mainwindow.h"
#include "cameraworker.h"
#include "videoudptransport.h"

#include "./ui_mainwindow.h"

#include <QBuffer>
#include <QCloseEvent>
#include <QDataStream>
#include <QDebug>
#include <QIODevice>
#include <QMetaObject>
#include <QPixmap>
#include <QThread>

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

    resetCameraUi();
    m_cameraThread->start();
}

MainWindow::~MainWindow()
{
    if (m_videoUdpTransport) {
        m_videoUdpTransport->close();
    }
    shutdownCameraThread();
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_videoUdpTransport) {
        m_videoUdpTransport->close();
    }
    shutdownCameraThread();
    QMainWindow::closeEvent(event);
}

void MainWindow::onStartCameraClicked()
{
    if (m_shuttingDown) {
        return;
    }

    if (!m_cameraThread || !m_cameraThread->isRunning() || !m_cameraWorker) {
        m_cameraRunning = false;
        resetCameraUi();
        ui->statusLabel->setText(QStringLiteral("状态：摄像头工作线程不可用。"));
        return;
    }

    const int cameraIndex = ui->cameraIndexSpinBox->value();
    m_cameraRunning = false;
    m_cameraOpening = true;
    ui->startCameraButton->setEnabled(false);
    ui->stopCameraButton->setEnabled(false);
    ui->cameraIndexSpinBox->setEnabled(false);
    ui->statusLabel->setText(QStringLiteral("状态：正在打开摄像头……"));

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
    ui->statusLabel->setText(QStringLiteral("状态：正在停止摄像头……"));

    emit requestStopCamera();
}

void MainWindow::onApplyNetworkSettingsClicked()
{
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
        return;
    }

    if (!m_videoUdpTransport) {
        m_networkSettingsValid = false;
        ui->stopNetworkButton->setEnabled(false);
        ui->sendTestFrameButton->setEnabled(false);
        ui->networkStatusLabel->setText(QStringLiteral("网络：UDP 传输对象不可用。"));
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
}

void MainWindow::onStopNetworkClicked()
{
    if (m_videoUdpTransport) {
        m_videoUdpTransport->close();
    }
    m_networkSettingsValid = false;
    ui->applyNetworkSettingsButton->setEnabled(true);
    ui->stopNetworkButton->setEnabled(false);
    ui->sendTestFrameButton->setEnabled(false);
    ui->networkStatusLabel->setText(QStringLiteral("网络：已停止"));
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

void MainWindow::onUdpFrameReceived(const QByteArray &encodedFrame,
                                    quint32 sessionId,
                                    quint32 frameId,
                                    quint32 timestampMs,
                                    const QHostAddress &senderAddress,
                                    quint16 senderPort)
{
    Q_UNUSED(sessionId);
    Q_UNUSED(frameId);
    Q_UNUSED(timestampMs);

    quint32 sequence = 0;
    QString errorMessage;
    if (validateDeterministicTestFrame(encodedFrame, &sequence, &errorMessage)) {
        ui->networkStatusLabel->setText(
            QStringLiteral("网络：收到有效测试帧 %1，%2 bytes，来源 %3:%4")
                .arg(sequence)
                .arg(encodedFrame.size())
                .arg(senderAddress.toString())
                .arg(senderPort));
        return;
    }

    ui->networkStatusLabel->setText(
        QStringLiteral("网络：收到完整编码帧，但测试数据校验失败：%1").arg(errorMessage));
}

void MainWindow::onUdpFrameSent(quint32 frameId,
                                qsizetype frameSize,
                                qsizetype fragmentCount)
{
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
    ui->statusLabel->setText(QStringLiteral("状态：%1").arg(description));
}

void MainWindow::onCameraStopped()
{
    m_cameraRunning = false;
    m_cameraOpening = false;
    m_lastFrame = QImage();

    if (m_shuttingDown) {
        return;
    }

    resetCameraUi();
    ui->statusLabel->setText(QStringLiteral("状态：已停止"));
}

void MainWindow::onCameraError(const QString &message)
{
    if (m_shuttingDown) {
        return;
    }

    m_cameraRunning = false;
    m_cameraOpening = false;
    m_lastFrame = QImage();
    resetCameraUi();
    ui->statusLabel->setText(QStringLiteral("状态：%1").arg(message));
}

void MainWindow::onCameraDiagnostic(const QString &message)
{
    if (m_shuttingDown || !m_cameraOpening) {
        return;
    }

    ui->statusLabel->setText(QStringLiteral("状态：%1").arg(message));
}

void MainWindow::onFrameReady(const QImage &image)
{
    if (m_shuttingDown || !m_cameraRunning || image.isNull()) {
        return;
    }

    m_lastFrame = image;
    updateVideoDisplay();
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
    ui->statusLabel->setText(QStringLiteral("状态：未启动"));
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
