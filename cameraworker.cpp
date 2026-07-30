#include "cameraworker.h"

#include "jpegframeencoder.h"
#include "videoframerateutils.h"

#include <cmath>

#include <QDebug>
#include <QElapsedTimer>
#include <QThread>

#include <opencv2/imgproc.hpp>

#include <atomic>

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

std::atomic_int g_cameraWorkerDestructionCount{0};

} // namespace

CameraWorker::CameraWorker(QObject *parent)
    : QObject(parent)
{
}

CameraWorker::~CameraWorker()
{
    shutdown();
    const int destructionCount = ++g_cameraWorkerDestructionCount;
    qInfo().nospace() << "[CameraWorker] destroyed #" << destructionCount
                      << " this=" << static_cast<const void *>(this)
                      << " currentThreadId=" << QThread::currentThreadId()
                      << " objectThread=" << thread()
                      << " objectThreadRunning=" << (thread() && thread()->isRunning());
}

int CameraWorker::destructionCount()
{
    return g_cameraWorkerDestructionCount.load();
}

void CameraWorker::resetDestructionCount()
{
    g_cameraWorkerDestructionCount.store(0);
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

    stopVideoEncoding();

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

void CameraWorker::startVideoEncoding(int targetFps, int jpegQuality)
{
    if (targetFps < 1 || targetFps > 30) {
        emit videoEncodingError(QStringLiteral("视频发送帧率必须在 1～30 FPS 范围内。"));
        return;
    }
    if (jpegQuality < 1 || jpegQuality > 100) {
        emit videoEncodingError(QStringLiteral("JPEG quality 必须在 1～100 范围内。"));
        return;
    }

    stopVideoEncoding();

    if (!m_videoEncodeScheduleTimer) {
        m_videoEncodeScheduleTimer = new QTimer(this);
        m_videoEncodeScheduleTimer->setTimerType(Qt::PreciseTimer);
        connect(m_videoEncodeScheduleTimer,
                &QTimer::timeout,
                this,
                &CameraWorker::encodeLatestVideoFrame);
    }

    m_videoTargetFps = targetFps;
    m_jpegQuality = jpegQuality;
    m_videoFrameIntervalMs = VideoFrameRateUtils::intervalMilliseconds(m_videoTargetFps);
    m_videoEncodingEnabled = true;
    m_encodeFirstFrameImmediately = true;
    m_videoEncodingErrorTimer.invalidate();
    m_lastVideoEncodingError.clear();
    m_videoEncodeScheduleTimer->start(m_videoFrameIntervalMs);
}

void CameraWorker::stopVideoEncoding()
{
    m_videoEncodingEnabled = false;
    m_encodeFirstFrameImmediately = false;
    if (m_videoEncodeScheduleTimer) {
        m_videoEncodeScheduleTimer->stop();
    }
    m_latestBgrFrame.release();
    m_latestFrameSerial = 0;
    m_lastEncodedFrameSerial = 0;
    m_videoEncodingErrorTimer.invalidate();
    m_lastVideoEncodingError.clear();
}

void CameraWorker::shutdown()
{
    stopVideoEncoding();
    if (m_captureTimer) {
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

    if (!m_videoEncodingEnabled) {
        return;
    }

    bgrFrame.copyTo(m_latestBgrFrame);
    ++m_latestFrameSerial;
    if (m_latestFrameSerial == 0) {
        ++m_latestFrameSerial;
    }

    if (m_encodeFirstFrameImmediately) {
        m_encodeFirstFrameImmediately = false;
        encodeLatestVideoFrame();
    }
}

void CameraWorker::encodeLatestVideoFrame()
{
    if (!m_videoEncodingEnabled || !m_running || !m_camera.isOpened()
        || m_latestBgrFrame.empty() || m_latestFrameSerial == 0
        || m_latestFrameSerial == m_lastEncodedFrameSerial) {
        return;
    }

    const quint64 frameSerial = m_latestFrameSerial;
    QString encodingError;
    QElapsedTimer encodingTimer;
    encodingTimer.start();
    const QByteArray jpegData = JpegFrameEncoder::encodeBgrFrame(
        m_latestBgrFrame, m_jpegQuality, &encodingError);
    const qint64 encodingDurationUs = encodingTimer.nsecsElapsed() / 1000;

    if (jpegData.isEmpty()) {
        reportVideoEncodingError(encodingError);
        return;
    }

    m_lastEncodedFrameSerial = frameSerial;
    m_lastVideoEncodingError.clear();
    m_videoEncodingErrorTimer.invalidate();
    emit jpegFrameReady(jpegData,
                        m_latestBgrFrame.cols,
                        m_latestBgrFrame.rows,
                        m_jpegQuality,
                        encodingDurationUs);
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

void CameraWorker::reportVideoEncodingError(const QString &message)
{
    const QString nonEmptyMessage = message.isEmpty()
        ? QStringLiteral("JPEG 编码失败。")
        : message;
    constexpr qint64 errorReportIntervalMs = 1000;
    if (m_lastVideoEncodingError == nonEmptyMessage
        && m_videoEncodingErrorTimer.isValid()
        && m_videoEncodingErrorTimer.elapsed() < errorReportIntervalMs) {
        return;
    }

    m_lastVideoEncodingError = nonEmptyMessage;
    m_videoEncodingErrorTimer.restart();
    emit videoEncodingError(nonEmptyMessage);
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
