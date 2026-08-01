#ifndef REMOTEVIDEODECODER_H
#define REMOTEVIDEODECODER_H

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>
#include <QtGlobal>

class RemoteVideoDecoder : public QObject
{
    Q_OBJECT

public:
    explicit RemoteVideoDecoder(QObject *parent = nullptr);
    ~RemoteVideoDecoder() override;

    static int destructionCount();
    static void resetDestructionCount();

signals:
    void frameDecoded(const QImage &image,
                      qsizetype jpegSize,
                      qint64 decodingDurationUs,
                      quint64 generation,
                      quint32 sessionId,
                      quint32 frameId,
                      quint32 timestampMs,
                      const QString &senderAddress,
                      quint16 senderPort);
    void frameDecodeFailed(const QString &message,
                           qsizetype jpegSize,
                           qint64 decodingDurationUs,
                           quint64 generation,
                           quint32 sessionId,
                           quint32 frameId,
                           const QString &senderAddress,
                           quint16 senderPort);

public slots:
    void shutdown();
    void decodeJpeg(const QByteArray &jpegData,
                    quint64 generation,
                    quint32 sessionId,
                    quint32 frameId,
                    quint32 timestampMs,
                    const QString &senderAddress,
                    quint16 senderPort);

private:
    bool m_shutdown = false;
};

#endif // REMOTEVIDEODECODER_H
