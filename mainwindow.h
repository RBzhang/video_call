#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QImage>
#include <QMainWindow>
#include <QString>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class CameraWorker;
class QThread;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    void requestStartCamera(int cameraIndex);
    void requestStopCamera();

private slots:
    void onStartCameraClicked();
    void onStopCameraClicked();
    void onCameraStarted(const QString &description);
    void onCameraStopped();
    void onCameraError(const QString &message);
    void onCameraDiagnostic(const QString &message);
    void onFrameReady(const QImage &image);

private:
    void updateVideoDisplay();
    void resetCameraUi();
    void shutdownCameraThread();

    Ui::MainWindow *ui;
    CameraWorker *m_cameraWorker = nullptr;
    QThread *m_cameraThread = nullptr;
    QImage m_lastFrame;
    bool m_cameraRunning = false;
    bool m_cameraOpening = false;
    bool m_shuttingDown = false;
};
#endif // MAINWINDOW_H
