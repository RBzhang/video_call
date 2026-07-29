#include "remotevideodecoder.h"

#include "jpegframedecoder.h"

#include <QElapsedTimer>

#include <exception>

RemoteVideoDecoder::RemoteVideoDecoder(QObject *parent)
    : QObject(parent)
{
}

RemoteVideoDecoder::~RemoteVideoDecoder() = default;

void RemoteVideoDecoder::decodeJpeg(const QByteArray &jpegData,
                                    quint64 generation,
                                    quint32 sessionId,
                                    quint32 frameId,
                                    quint32 timestampMs,
                                    const QString &senderAddress,
                                    quint16 senderPort)
{
    QElapsedTimer decodingTimer;
    decodingTimer.start();

    QString errorMessage;
    QImage image;
    try {
        image = JpegFrameDecoder::decodeJpegFrame(jpegData, &errorMessage);
    } catch (const std::exception &exception) {
        errorMessage = QStringLiteral("JPEG 解码 Worker 标准异常：%1")
                           .arg(QString::fromLocal8Bit(exception.what()));
    } catch (...) {
        errorMessage = QStringLiteral("JPEG 解码 Worker 发生未知异常。");
    }

    const qint64 decodingDurationUs = decodingTimer.nsecsElapsed() / 1000;
    if (image.isNull()) {
        if (errorMessage.isEmpty()) {
            errorMessage = QStringLiteral("JPEG 解码 Worker 未返回图像。");
        }
        emit frameDecodeFailed(errorMessage,
                               jpegData.size(),
                               decodingDurationUs,
                               generation,
                               sessionId,
                               frameId,
                               senderAddress,
                               senderPort);
        return;
    }

    emit frameDecoded(image,
                      jpegData.size(),
                      decodingDurationUs,
                      generation,
                      sessionId,
                      frameId,
                      timestampMs,
                      senderAddress,
                      senderPort);
}
