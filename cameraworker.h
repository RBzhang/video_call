#ifndef CAMERAWORKER_H
#define CAMERAWORKER_H

#include <QImage>
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

public slots:
    void startCamera(int cameraIndex);
    void stopCamera();

private slots:
    void captureFrame();

private:
    bool tryOpenCamera(int cameraIndex, int backend, QString *backendDescription);
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
};

#endif // CAMERAWORKER_H
