#ifndef VIDEOUDPTRANSPORT_H
#define VIDEOUDPTRANSPORT_H

#include "videoframereassembler.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QtGlobal>

class QTimer;
class QUdpSocket;

class VideoUdpTransport : public QObject
{
    Q_OBJECT

public:
    explicit VideoUdpTransport(QObject *parent = nullptr);
    ~VideoUdpTransport() override;

    bool bindReceiver(const QHostAddress &localAddress,
                      quint16 localPort,
                      QString *errorMessage = nullptr);
    void configurePeer(const QHostAddress &peerAddress, quint16 peerPort);
    void close();

    bool isBound() const;
    QHostAddress localAddress() const;
    quint16 localPort() const;

    QHostAddress peerAddress() const;
    quint16 peerPort() const;

    quint32 sessionId() const;

    bool sendEncodedFrame(const QByteArray &encodedFrame,
                          QString *errorMessage = nullptr);

signals:
    void frameReceived(const QByteArray &encodedFrame,
                       quint32 sessionId,
                       quint32 frameId,
                       quint32 timestampMs,
                       const QHostAddress &senderAddress,
                       quint16 senderPort);
    void frameSent(quint32 frameId, qsizetype frameSize, qsizetype fragmentCount);
    void networkError(const QString &message);
    void datagramRejected(const QString &message);

private slots:
    void readPendingDatagrams();
    void cleanupExpiredFrames();

private:
    void paceDatagram(qsizetype datagramSize);
    void resetDatagramPacer();

    QUdpSocket *m_socket = nullptr;
    QTimer *m_cleanupTimer = nullptr;

    VideoFrameReassembler m_reassembler;

    QHostAddress m_peerAddress;
    quint16 m_peerPort = 0;

    quint32 m_sessionId = 0;
    quint32 m_nextFrameId = 1;
    QElapsedTimer m_monotonicClock;
    QElapsedTimer m_pacingClock;
    qint64 m_nextDatagramDeadlineNs = 0;
};

#endif // VIDEOUDPTRANSPORT_H
