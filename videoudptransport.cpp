#include "videoudptransport.h"

#include "videopacketprotocol.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>

namespace
{

// FPGA 回环端没有可用的 TEMAC RX 反压通路，且其包描述符队列仅能容纳有限
// 的短时突发。视频一帧的所有分片必须按线速整形，而不能在一个事件循环中突发。
constexpr double FpgaLoopbackWireRateMbitPerSecond = 80.0;
constexpr qint64 EthernetPreambleAndSfdBytes = 8;
constexpr qint64 EthernetHeaderBytes = 14;
constexpr qint64 Ipv4HeaderBytes = 20;
constexpr qint64 UdpHeaderBytes = 8;
constexpr qint64 EthernetFcsBytes = 4;
constexpr qint64 EthernetIfgBytes = 12;

qint64 datagramWireTimeNs(qsizetype udpPayloadSize)
{
    const qint64 wireBytes = EthernetPreambleAndSfdBytes
        + EthernetHeaderBytes
        + Ipv4HeaderBytes
        + UdpHeaderBytes
        + udpPayloadSize
        + EthernetFcsBytes
        + EthernetIfgBytes;
    return static_cast<qint64>(
        (static_cast<double>(wireBytes) * 8'000.0) / FpgaLoopbackWireRateMbitPerSecond);
}

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

VideoUdpTransport::VideoUdpTransport(QObject *parent)
    : QObject(parent)
    , m_socket(new QUdpSocket(this))
    , m_cleanupTimer(new QTimer(this))
    , m_sessionId(QRandomGenerator::global()->generate())
{
    if (m_sessionId == 0) {
        m_sessionId = 1;
    }

    connect(m_socket, &QUdpSocket::readyRead, this, &VideoUdpTransport::readPendingDatagrams);
    connect(m_cleanupTimer,
            &QTimer::timeout,
            this,
            &VideoUdpTransport::cleanupExpiredFrames);
    m_cleanupTimer->setInterval(100);
    m_monotonicClock.start();
    m_pacingClock.start();
}

VideoUdpTransport::~VideoUdpTransport()
{
    close();
}

bool VideoUdpTransport::bindReceiver(const QHostAddress &localAddress,
                                     quint16 localPort,
                                     QString *errorMessage)
{
    if (localAddress.protocol() != QAbstractSocket::IPv4Protocol) {
        setError(errorMessage, QStringLiteral("本地绑定地址必须是 IPv4 地址。"));
        return false;
    }

    close();
    if (!m_socket->bind(localAddress, localPort)) {
        const QString message = QStringLiteral("绑定 UDP 接收端口失败：%1")
                                    .arg(m_socket->errorString());
        setError(errorMessage, message);
        emit networkError(message);
        return false;
    }

    m_cleanupTimer->start();
    resetDatagramPacer();
    clearError(errorMessage);
    return true;
}

void VideoUdpTransport::configurePeer(const QHostAddress &peerAddress, quint16 peerPort)
{
    if (peerAddress.protocol() != QAbstractSocket::IPv4Protocol || peerPort == 0) {
        m_peerAddress = QHostAddress();
        m_peerPort = 0;
        return;
    }

    m_peerAddress = peerAddress;
    m_peerPort = peerPort;
    resetDatagramPacer();
}

void VideoUdpTransport::close()
{
    if (m_cleanupTimer) {
        m_cleanupTimer->stop();
    }
    if (m_socket) {
        m_socket->close();
    }

    // 保留对端配置，以便调用方重新绑定后继续使用同一对端。
    m_reassembler.clear();
    resetDatagramPacer();
}

bool VideoUdpTransport::isBound() const
{
    return m_socket && m_socket->state() == QAbstractSocket::BoundState;
}

QHostAddress VideoUdpTransport::localAddress() const
{
    return isBound() ? m_socket->localAddress() : QHostAddress();
}

quint16 VideoUdpTransport::localPort() const
{
    return isBound() ? m_socket->localPort() : 0;
}

QHostAddress VideoUdpTransport::peerAddress() const
{
    return m_peerAddress;
}

quint16 VideoUdpTransport::peerPort() const
{
    return m_peerPort;
}

quint32 VideoUdpTransport::sessionId() const
{
    return m_sessionId;
}

bool VideoUdpTransport::sendEncodedFrame(const QByteArray &encodedFrame,
                                         QString *errorMessage)
{
    if (!isBound()) {
        setError(errorMessage, QStringLiteral("UDP 接收端口尚未绑定。"));
        return false;
    }
    if (m_peerAddress.protocol() != QAbstractSocket::IPv4Protocol || m_peerPort == 0) {
        setError(errorMessage, QStringLiteral("对端 IPv4 地址或端口尚未配置。"));
        return false;
    }
    if (encodedFrame.isEmpty()) {
        setError(errorMessage, QStringLiteral("待发送编码帧不能为空。"));
        return false;
    }

    const quint32 frameId = m_nextFrameId;
    const quint32 timestampMs = static_cast<quint32>(
        static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) & 0xffffffffULL);
    QString fragmentationError;
    const QVector<QByteArray> datagrams = VideoPacketProtocol::fragmentEncodedFrame(
        encodedFrame,
        m_sessionId,
        frameId,
        timestampMs,
        &fragmentationError);
    if (datagrams.isEmpty()) {
        const QString message = QStringLiteral("视频帧分片失败：%1").arg(fragmentationError);
        setError(errorMessage, message);
        emit networkError(message);
        return false;
    }

    for (qsizetype index = 0; index < datagrams.size(); ++index) {
        const QByteArray &datagram = datagrams.at(index);
        paceDatagram(datagram.size());
        const qint64 written = m_socket->writeDatagram(datagram, m_peerAddress, m_peerPort);
        if (written != datagram.size()) {
            const QString message = QStringLiteral("发送帧 %1 的分片 %2 失败：%3")
                                        .arg(frameId)
                                        .arg(index)
                                        .arg(m_socket->errorString());
            setError(errorMessage, message);
            emit networkError(message);
            return false;
        }
    }

    emit frameSent(frameId, encodedFrame.size(), datagrams.size());
    ++m_nextFrameId;
    if (m_nextFrameId == 0) {
        m_nextFrameId = 1;
    }
    clearError(errorMessage);
    return true;
}

void VideoUdpTransport::paceDatagram(qsizetype datagramSize)
{
    const qint64 intervalNs = datagramWireTimeNs(datagramSize);
    if (!m_pacingClock.isValid()) {
        m_pacingClock.start();
    }

    qint64 nowNs = m_pacingClock.nsecsElapsed();
    if (m_nextDatagramDeadlineNs < nowNs) {
        // 不追赶落后的发送时隙，避免编码或 UI 调度延迟后形成微突发。
        m_nextDatagramDeadlineNs = nowNs;
    }

    while ((nowNs = m_pacingClock.nsecsElapsed()) < m_nextDatagramDeadlineNs) {
        const qint64 remainingNs = m_nextDatagramDeadlineNs - nowNs;
        if (remainingNs > 2'000'000) {
            QThread::usleep(static_cast<unsigned long>((remainingNs - 1'000'000) / 1'000));
        }
    }

    m_nextDatagramDeadlineNs = m_pacingClock.nsecsElapsed() + intervalNs;
}

void VideoUdpTransport::resetDatagramPacer()
{
    if (!m_pacingClock.isValid()) {
        m_pacingClock.start();
    }
    m_nextDatagramDeadlineNs = m_pacingClock.nsecsElapsed();
}

void VideoUdpTransport::readPendingDatagrams()
{
    while (m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket->receiveDatagram();
        VideoFrameReassembler::CompletedFrame completedFrame;
        QString errorMessage;
        const VideoFrameReassembler::AddResult result = m_reassembler.addDatagram(
            datagram.data(),
            datagram.senderAddress(),
            datagram.senderPort(),
            m_monotonicClock.elapsed(),
            &completedFrame,
            &errorMessage);

        if (result == VideoFrameReassembler::AddResult::Rejected) {
            emit datagramRejected(errorMessage.isEmpty()
                                      ? QStringLiteral("UDP 数据报被协议层拒绝。")
                                      : errorMessage);
        } else if (result == VideoFrameReassembler::AddResult::Completed) {
            emit frameReceived(completedFrame.encodedFrame,
                               completedFrame.sessionId,
                               completedFrame.frameId,
                               completedFrame.timestampMs,
                               completedFrame.senderAddress,
                               completedFrame.senderPort);
        }
    }
}

void VideoUdpTransport::cleanupExpiredFrames()
{
    m_reassembler.removeExpiredFrames(m_monotonicClock.elapsed());
}
