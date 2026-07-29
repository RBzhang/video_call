#ifndef AUDIOUDPTRANSPORT_H
#define AUDIOUDPTRANSPORT_H

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QtGlobal>

class QUdpSocket;

class AudioUdpTransport : public QObject
{
    Q_OBJECT

public:
    explicit AudioUdpTransport(QObject *parent = nullptr);
    ~AudioUdpTransport() override;

    bool bindReceiver(const QHostAddress &address, quint16 port, QString *errorMessage = nullptr);
    void configurePeer(const QHostAddress &address, quint16 port);
    bool sendAudioPayload(const QByteArray &pcmPayload,
                          quint32 sessionId,
                          quint32 sequence,
                          quint32 timestampSamples,
                          QString *errorMessage = nullptr);
    void close();

    bool isBound() const;
    QHostAddress localAddress() const;
    quint16 localPort() const;
    QHostAddress peerAddress() const;
    quint16 peerPort() const;

signals:
    void audioPacketReceived(const QByteArray &pcmPayload,
                             quint32 sessionId,
                             quint32 sequence,
                             quint32 timestampSamples,
                             const QHostAddress &senderAddress,
                             quint16 senderPort);
    void audioPacketSent(quint32 sessionId,
                         quint32 sequence,
                         quint32 timestampSamples,
                         qsizetype payloadSize);
    void datagramRejected(const QString &message);
    void foreignDatagramDropped(const QHostAddress &senderAddress, quint16 senderPort);
    void networkError(const QString &message);

private slots:
    void readPendingDatagrams();

private:
    QUdpSocket *m_socket = nullptr;
    QHostAddress m_peerAddress;
    quint16 m_peerPort = 0;
};

#endif // AUDIOUDPTRANSPORT_H
