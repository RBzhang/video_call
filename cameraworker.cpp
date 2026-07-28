#include "cameraworker.h"

#include <cmath>

#include <QDebug>
#include <QElapsedTimer>

#include <opencv2/imgproc.hpp>

namespace {

QString backendDisplayName(int backend)
{
    switch (backend) {
    case cv::CAP_DSHOW:
        return QStringLiteral("DirectShow (DSHOW)");
    case cv::CAP_MSMF:
        return QStringLiteral("Media Foundation (MSMF)");
    case cv::CAP_ANY:
        return QStringLiteral("自动选择 (ANY)");
    default:
        return QStringLiteral("未知后端");
    }
}

} // namespace

CameraWorker::CameraWorker(QObject *parent)
    : QObject(parent)
{
}

CameraWorker::~CameraWorker()
{
    if (m_captureTimer && m_captureTimer->isActive()) {
        m_captureTimer->stop();
    }

    releaseCamera();
}

void CameraWorker::startCamera(int cameraIndex)
{
    if (cameraIndex < 0) {
        emit errorOccurred(QStringLiteral("摄像头编号无效。"));
        return;
    }

    if (m_captureTimer && m_captureTimer->isActive()) {
        m_captureTimer->stop();
    }
    releaseCamera();
    m_running = false;
    m_cameraIndex = -1;
    m_consecutiveReadFailures = 0;

    if (!m_captureTimer) {
        m_captureTimer = new QTimer(this);
        m_captureTimer->setTimerType(Qt::PreciseTimer);
        connect(m_captureTimer, &QTimer::timeout, this, &CameraWorker::captureFrame);
    }

    QString backendDescription;
    const int backends[] = {
        cv::CAP_DSHOW,
        cv::CAP_MSMF,
        cv::CAP_ANY
    };

    bool opened = false;
    for (const int backend : backends) {
        const QString backendName = backendDisplayName(backend);
        reportDiagnostic(
            QStringLiteral("摄像头 %1：正在尝试使用 %2 打开。").arg(cameraIndex).arg(backendName));

        QElapsedTimer openTimer;
        openTimer.start();
        QString failureReason;
        if (tryOpenCamera(cameraIndex, backend, &backendDescription, &failureReason)) {
            reportDiagnostic(
                QStringLiteral("摄像头 %1：%2 打开成功（耗时 %3 ms，OpenCV 后端：%4）。")
                    .arg(cameraIndex)
                    .arg(backendName)
                    .arg(openTimer.elapsed())
                    .arg(backendDescription));
            opened = true;
            break;
        }

        reportDiagnostic(
            QStringLiteral("摄像头 %1：%2 未能打开（耗时 %3 ms，原因：%4）。")
                .arg(cameraIndex)
                .arg(backendName)
                .arg(openTimer.elapsed())
                .arg(failureReason));
    }

    if (!opened) {
        releaseCamera();
        m_frameWidth = 0;
        m_frameHeight = 0;
        m_reportedFps = 0.0;
        m_backendDescription.clear();
        emit errorOccurred(
            QStringLiteral("无法打开编号为 %1 的摄像头。请检查摄像头编号、系统权限以及设备是否被其他程序占用；已依次尝试 DirectShow、Media Foundation 和自动后端。")
                .arg(cameraIndex));
        return;
    }

    try {
        m_camera.set(cv::CAP_PROP_FRAME_WIDTH, 640);
        m_camera.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
        m_camera.set(cv::CAP_PROP_FPS, 30);
    } catch (const cv::Exception &) {
    }

    const auto toDimension = [](double value) {
        if (!std::isfinite(value) || value <= 0.0) {
            return 0;
        }
        return static_cast<int>(std::lround(value));
    };

    m_cameraIndex = cameraIndex;
    try {
        m_frameWidth = toDimension(m_camera.get(cv::CAP_PROP_FRAME_WIDTH));
        m_frameHeight = toDimension(m_camera.get(cv::CAP_PROP_FRAME_HEIGHT));
        m_reportedFps = m_camera.get(cv::CAP_PROP_FPS);
    } catch (const cv::Exception &) {
        m_frameWidth = 0;
        m_frameHeight = 0;
        m_reportedFps = 0.0;
    }
    m_backendDescription = backendDescription;
    m_running = true;
    m_consecutiveReadFailures = 0;

    m_captureTimer->start(33);
    emit cameraStarted(buildCameraDescription());
}

void CameraWorker::stopCamera()
{
    reportDiagnostic(QStringLiteral("正在停止摄像头。"));

    if (m_captureTimer && m_captureTimer->isActive()) {
        m_captureTimer->stop();
    }

    releaseCamera();
    m_running = false;
    m_cameraIndex = -1;
    m_consecutiveReadFailures = 0;
    m_frameWidth = 0;
    m_frameHeight = 0;
    m_reportedFps = 0.0;
    m_backendDescription.clear();

    emit cameraStopped();
}

void CameraWorker::captureFrame()
{
    if (!m_running || !m_camera.isOpened()) {
        return;
    }

    cv::Mat bgrFrame;
    bool success = false;
    try {
        success = m_camera.read(bgrFrame);
    } catch (const cv::Exception &) {
        success = false;
    }

    if (!success || bgrFrame.empty()) {
        ++m_consecutiveReadFailures;
        constexpr int maxConsecutiveReadFailures = 10;
        if (m_consecutiveReadFailures >= maxConsecutiveReadFailures) {
            if (m_captureTimer && m_captureTimer->isActive()) {
                m_captureTimer->stop();
            }
            releaseCamera();
            m_running = false;
            m_cameraIndex = -1;
            m_consecutiveReadFailures = 0;
            m_frameWidth = 0;
            m_frameHeight = 0;
            m_reportedFps = 0.0;
            m_backendDescription.clear();
            emit errorOccurred(QStringLiteral("摄像头连续读取失败，采集已停止。"));
            emit cameraStopped();
        }
        return;
    }

    m_consecutiveReadFailures = 0;

    cv::Mat convertedFrame;
    const cv::Mat *imageFrame = &bgrFrame;
    QImage::Format imageFormat = QImage::Format_Invalid;

    try {
        switch (bgrFrame.channels()) {
        case 3:
            cv::cvtColor(bgrFrame, convertedFrame, cv::COLOR_BGR2RGB);
            imageFrame = &convertedFrame;
            imageFormat = QImage::Format_RGB888;
            break;
        case 4:
            cv::cvtColor(bgrFrame, convertedFrame, cv::COLOR_BGRA2RGBA);
            imageFrame = &convertedFrame;
            imageFormat = QImage::Format_RGBA8888;
            break;
        case 1:
            imageFormat = QImage::Format_Grayscale8;
            break;
        default:
            return;
        }
    } catch (const cv::Exception &) {
        return;
    }

    const QImage image(imageFrame->data,
                       imageFrame->cols,
                       imageFrame->rows,
                       static_cast<qsizetype>(imageFrame->step),
                       imageFormat);
    if (image.isNull()) {
        return;
    }

    emit frameReady(image.copy());
}

bool CameraWorker::tryOpenCamera(int cameraIndex,
                                 int backend,
                                 QString *backendDescription,
                                 QString *failureReason)
{
    releaseCamera();
    bool opened = false;
    try {
        opened = m_camera.open(cameraIndex, backend);
    } catch (const cv::Exception &exception) {
        opened = false;
        if (failureReason) {
            *failureReason = QString::fromLocal8Bit(exception.what());
        }
    }

    if (!opened) {
        releaseCamera();
        if (failureReason && failureReason->isEmpty()) {
            *failureReason = QStringLiteral("OpenCV open() 返回 false");
        }
        return false;
    }

    QString description;
    try {
        description = QString::fromStdString(m_camera.getBackendName());
    } catch (const cv::Exception &) {
    }

    if (description.trimmed().isEmpty()) {
        description = QStringLiteral("未知后端");
    }

    if (backendDescription) {
        *backendDescription = description;
    }
    return true;
}

void CameraWorker::reportDiagnostic(const QString &message)
{
    qInfo().noquote() << QStringLiteral("[CameraWorker]") << message;
    emit diagnosticOccurred(message);
}

void CameraWorker::releaseCamera()
{
    try {
        if (m_camera.isOpened()) {
            m_camera.release();
        }
    } catch (const cv::Exception &) {
    }
}

QString CameraWorker::buildCameraDescription() const
{
    const QString resolution = m_frameWidth > 0 && m_frameHeight > 0
        ? QStringLiteral("%1×%2").arg(m_frameWidth).arg(m_frameHeight)
        : QStringLiteral("未知");
    const QString frameRate = std::isfinite(m_reportedFps) && m_reportedFps > 0.0
        ? QStringLiteral("%1 FPS").arg(m_reportedFps, 0, 'f', 2)
        : QStringLiteral("未知");
    const QString backend = m_backendDescription.trimmed().isEmpty()
        ? QStringLiteral("未知后端")
        : m_backendDescription;

    return QStringLiteral("摄像头 %1 已启动｜后端：%2｜分辨率：%3｜设备帧率：%4")
        .arg(m_cameraIndex)
        .arg(backend)
        .arg(resolution)
        .arg(frameRate);
}
