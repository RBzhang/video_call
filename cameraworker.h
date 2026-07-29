#ifndef CAMERAWORKER_H
#define CAMERAWORKER_H

#include <QImage>
#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <opencv2/videoio.hpp>

class CameraWorker : public QObject
{
    Q_OBJECT

public:
    explicit CameraWorker(QObject *parent = nullptr);
    ~CameraWorker() override;

signals:
    void frameReady(const QImage &image);
    void cameraStarted(const QString &description);
    void cameraStopped();
    void errorOccurred(const QString &message);
    void diagnosticOccurred(const QString &message);
    void jpegFrameReady(const QByteArray &jpegData,
                        int width,
                        int height,
                        int jpegQuality);
    void videoEncodingError(const QString &message);

public slots:
    void startCamera(int cameraIndex);
    void stopCamera();
    void startVideoEncoding(int targetFps, int jpegQuality);
    void stopVideoEncoding();

private slots:
    void captureFrame();

private:
    bool tryOpenCamera(int cameraIndex,
                       int backend,
                       QString *backendDescription,
                       QString *failureReason);
    void reportDiagnostic(const QString &message);
    void reportVideoEncodingError(const QString &message);
    void releaseCamera();
    QString buildCameraDescription() const;

    cv::VideoCapture m_camera;
    QTimer *m_captureTimer = nullptr;
    bool m_running = false;
    int m_cameraIndex = -1;
    int m_consecutiveReadFailures = 0;
    int m_frameWidth = 0;
    int m_frameHeight = 0;
    double m_reportedFps = 0.0;
    QString m_backendDescription;

    bool m_videoEncodingEnabled = false;
    int m_videoTargetFps = 10;
    int m_jpegQuality = 60;
    int m_videoFrameIntervalMs = 100;
    QElapsedTimer m_videoEncodeTimer;
    QElapsedTimer m_videoEncodingErrorTimer;
    QString m_lastVideoEncodingError;
};

#endif // CAMERAWORKER_H
