#include "mainwindow.h"
#include "cameraworker.h"

#include "./ui_mainwindow.h"

#include <QMetaObject>
#include <QPixmap>
#include <QThread>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    m_cameraThread = new QThread(this);
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

    connect(ui->startCameraButton,
            &QPushButton::clicked,
            this,
            &MainWindow::onStartCameraClicked);
    connect(ui->stopCameraButton,
            &QPushButton::clicked,
            this,
            &MainWindow::onStopCameraClicked);

    resetCameraUi();
    m_cameraThread->start();
}

MainWindow::~MainWindow()
{
    shutdownCameraThread();
    delete ui;
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

void MainWindow::onCameraStarted(const QString &description)
{
    if (m_shuttingDown) {
        return;
    }

    m_cameraRunning = true;
    ui->startCameraButton->setEnabled(false);
    ui->stopCameraButton->setEnabled(true);
    ui->cameraIndexSpinBox->setEnabled(false);
    ui->statusLabel->setText(QStringLiteral("状态：%1").arg(description));
}

void MainWindow::onCameraStopped()
{
    m_cameraRunning = false;
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
    m_lastFrame = QImage();
    resetCameraUi();
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

void MainWindow::shutdownCameraThread()
{
    if (m_shuttingDown) {
        return;
    }

    m_shuttingDown = true;

    if (!m_cameraThread || !m_cameraThread->isRunning()) {
        return;
    }

    if (m_cameraWorker) {
        if (QThread::currentThread() == m_cameraThread) {
            m_cameraWorker->stopCamera();
        } else {
            QMetaObject::invokeMethod(
                m_cameraWorker,
                [worker = m_cameraWorker] { worker->stopCamera(); },
                Qt::BlockingQueuedConnection);
        }
    }

    m_cameraThread->quit();
    m_cameraThread->wait();
}
