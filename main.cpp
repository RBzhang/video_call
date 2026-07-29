#include "mainwindow.h"

#include <QApplication>
#include <QAudioDevice>
#include <QMediaDevices>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QCoreApplication::setQuitLockEnabled(false);
    QMediaDevices::defaultAudioInput();
    QMediaDevices::defaultAudioOutput();
    MainWindow w;
    w.show();
    return QApplication::exec();
}
