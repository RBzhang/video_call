#include "audioudptransport.h"

#include "audiopacketprotocol.h"

#include <QAbstractSocket>
#include <QNetworkDatagram>
#include <QUdpSocket>

namespace {

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

void clearError(QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
}

} // namespace

AudioUdpTransport::AudioUdpTransport(QObject *parent)
    : QObject(parent)
    , m_socket(new QUdpSocket(this))
{
    connect(m_socket, &QUdpSocket::readyRead, this, &AudioUdpTransport::readPendingDatagrams);
}

AudioUdpTransport::~AudioUdpTransport()
{
    close();
}

bool AudioUdpTransport::bindReceiver(const QHostAddress &address,
                                     quint16 port,
                                     QString *errorMessage)
{
    if (address.protocol() != QAbstractSocket::IPv4Protocol) {
        setError(errorMessage, QStringLiteral("音频本地绑定必须使用有效 IPv4 地址。"));
        return false;
    }

    close();
    if (!m_socket->bind(address, port)) {
        const QString message = QStringLiteral("绑定音频 UDP 端口失败：%1")
                                    .arg(m_socket->errorString());
        setError(errorMessage, message);
        emit networkError(message);
        return false;
    }

    clearError(errorMessage);
    return true;
}

void AudioUdpTransport::configurePeer(const QHostAddress &address, quint16 port)
{
    if (address.protocol() != QAbstractSocket::IPv4Protocol || port == 0) {
        m_peerAddress = QHostAddress();
        m_peerPort = 0;
        return;
    }

    m_peerAddress = address;
    m_peerPort = port;
}

bool AudioUdpTransport::sendAudioPayload(const QByteArray &pcmPayload,
                                         quint32 sessionId,
                                         quint32 sequence,
                                         quint32 timestampSamples,
                                         QString *errorMessage)
{
    if (!isBound()) {
        setError(errorMessage, QStringLiteral("音频 UDP 接收端口尚未绑定。"));
        return false;
    }
    if (m_peerAddress.protocol() != QAbstractSocket::IPv4Protocol || m_peerPort == 0) {
        setError(errorMessage, QStringLiteral("音频对端 IPv4 地址或端口尚未配置。"));
        return false;
    }

    AudioPacketProtocol::AudioPacketHeader header;
    header.sessionId = sessionId;
    header.sequence = sequence;
    header.timestampSamples = timestampSamples;
    QString serializationError;
    const QByteArray datagram = AudioPacketProtocol::serializeAudioPacket(
        header, pcmPayload, &serializationError);
    if (datagram.isEmpty()) {
        const QString message = QStringLiteral("ACL1 音频数据报序列化失败：%1").arg(serializationError);
        setError(errorMessage, message);
        emit networkError(message);
        return false;
    }

    const qint64 written = m_socket->writeDatagram(datagram, m_peerAddress, m_peerPort);
    if (written != datagram.size()) {
        const QString message = QStringLiteral("发送 ACL1 音频数据报失败：%1")
                                    .arg(m_socket->errorString());
        setError(errorMessage, message);
        emit networkError(message);
        return false;
    }

    emit audioPacketSent(sessionId, sequence, timestampSamples, pcmPayload.size());
    clearError(errorMessage);
    return true;
}

void AudioUdpTransport::close()
{
    if (m_socket) {
        m_socket->close();
    }
}

bool AudioUdpTransport::isBound() const
{
    return m_socket && m_socket->state() == QAbstractSocket::BoundState;
}

QHostAddress AudioUdpTransport::localAddress() const
{
    return isBound() ? m_socket->localAddress() : QHostAddress();
}

quint16 AudioUdpTransport::localPort() const
{
    return isBound() ? m_socket->localPort() : 0;
}

QHostAddress AudioUdpTransport::peerAddress() const
{
    return m_peerAddress;
}

quint16 AudioUdpTransport::peerPort() const
{
    return m_peerPort;
}

void AudioUdpTransport::readPendingDatagrams()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket->receiveDatagram();
        if (datagram.senderAddress() != m_peerAddress || datagram.senderPort() != m_peerPort) {
            emit foreignDatagramDropped(datagram.senderAddress(), datagram.senderPort());
            continue;
        }

        AudioPacketProtocol::AudioPacket packet;
        QString parseError;
        if (!AudioPacketProtocol::parseAudioPacket(datagram.data(), &packet, &parseError)) {
            emit datagramRejected(parseError);
            continue;
        }

        emit audioPacketReceived(packet.pcmPayload,
                                 packet.header.sessionId,
                                 packet.header.sequence,
                                 packet.header.timestampSamples,
                                 datagram.senderAddress(),
                                 datagram.senderPort());
    }
}
