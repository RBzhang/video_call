#include "audioudptransport.h"

#include "audiopacketprotocol.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QTimer>
#include <QUdpSocket>

#include <functional>

namespace {

struct ReceivedPacket
{
    QByteArray payload;
    quint32 sessionId = 0;
    quint32 sequence = 0;
    quint32 timestampSamples = 0;
};

bool expect(bool condition, const QString &message)
{
    if (!condition) {
        qCritical().noquote() << QStringLiteral("[audio_udp_transport_test]") << message;
    }
    return condition;
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 3000)
{
    if (predicate()) {
        return true;
    }
    QEventLoop loop;
    QTimer timeout;
    QTimer poll;
    timeout.setSingleShot(true);
    timeout.setInterval(timeoutMs);
    poll.setInterval(10);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (predicate()) {
            loop.quit();
        }
    });
    timeout.start();
    poll.start();
    loop.exec();
    return predicate();
}

QByteArray pcmFor(quint32 sequence)
{
    QByteArray pcm(AudioPacketProtocol::PcmPayloadSize, '\0');
    pcm[0] = static_cast<char>(sequence & 0xff);
    pcm[1] = static_cast<char>((sequence >> 8) & 0xff);
    return pcm;
}

bool testLoopbackAndValidation()
{
    AudioUdpTransport endpointA;
    AudioUdpTransport endpointB;
    QString error;
    bool success = true;
    success &= expect(endpointA.bindReceiver(QHostAddress::LocalHost, 0, &error),
                      QStringLiteral("A 绑定失败：%1").arg(error));
    success &= expect(endpointB.bindReceiver(QHostAddress::LocalHost, 0, &error),
                      QStringLiteral("B 绑定失败：%1").arg(error));
    if (!success) {
        return false;
    }
    endpointA.configurePeer(QHostAddress::LocalHost, endpointB.localPort());
    endpointB.configurePeer(QHostAddress::LocalHost, endpointA.localPort());

    QVector<ReceivedPacket> receivedByA;
    QVector<ReceivedPacket> receivedByB;
    int foreignDrops = 0;
    int rejected = 0;
    QObject::connect(&endpointA, &AudioUdpTransport::audioPacketReceived,
                     [&receivedByA](const QByteArray &payload, quint32 sessionId, quint32 sequence,
                                    quint32 timestampSamples, const QHostAddress &, quint16) {
                         receivedByA.append({payload, sessionId, sequence, timestampSamples});
                     });
    QObject::connect(&endpointB, &AudioUdpTransport::audioPacketReceived,
                     [&receivedByB](const QByteArray &payload, quint32 sessionId, quint32 sequence,
                                    quint32 timestampSamples, const QHostAddress &, quint16) {
                         receivedByB.append({payload, sessionId, sequence, timestampSamples});
                     });
    QObject::connect(&endpointB, &AudioUdpTransport::foreignDatagramDropped,
                     [&foreignDrops](const QHostAddress &, quint16) { ++foreignDrops; });
    QObject::connect(&endpointB, &AudioUdpTransport::datagramRejected,
                     [&rejected](const QString &) { ++rejected; });

    success &= expect(endpointA.sendAudioPayload(pcmFor(1), 101, 1, 0, &error),
                      QStringLiteral("A→B 单包发送失败：%1").arg(error));
    success &= expect(waitUntil([&] { return receivedByB.size() == 1; }),
                      QStringLiteral("A→B 单包接收超时。"));
    success &= expect(receivedByB.constFirst().payload == pcmFor(1)
                          && receivedByB.constFirst().sessionId == 101
                          && receivedByB.constFirst().sequence == 1
                          && receivedByB.constFirst().timestampSamples == 0,
                      QStringLiteral("A→B 单包字段或 PCM 不一致。"));

    success &= expect(endpointB.sendAudioPayload(pcmFor(9), 202, 9, 2560, &error),
                      QStringLiteral("B→A 单包发送失败：%1").arg(error));
    success &= expect(waitUntil([&] { return receivedByA.size() == 1; }),
                      QStringLiteral("B→A 单包接收超时。"));

    receivedByB.clear();
    for (quint32 sequence = 1; sequence <= 50; ++sequence) {
        success &= expect(endpointA.sendAudioPayload(pcmFor(sequence), 303, sequence,
                                                      (sequence - 1) * 320, &error),
                          QStringLiteral("连续音频包 %1 发送失败：%2").arg(sequence).arg(error));
    }
    success &= expect(waitUntil([&] { return receivedByB.size() == 50; }),
                      QStringLiteral("50 个连续音频包接收超时。"));
    for (quint32 index = 0; index < 50 && success; ++index) {
        const ReceivedPacket &packet = receivedByB.at(index);
        success &= expect(packet.payload == pcmFor(index + 1)
                              && packet.sequence == index + 1
                              && packet.timestampSamples == index * 320,
                          QStringLiteral("连续包 %1 的 sequence、timestamp 或 PCM 错误。").arg(index + 1));
    }

    QUdpSocket foreignSocket;
    success &= expect(foreignSocket.bind(QHostAddress::LocalHost, 0),
                      QStringLiteral("foreign Socket 绑定失败。"));
    const QByteArray valid = AudioPacketProtocol::serializeAudioPacket(
        [] { AudioPacketProtocol::AudioPacketHeader header; header.sessionId = 404; header.sequence = 1; return header; }(),
        pcmFor(1));
    foreignSocket.writeDatagram(valid, QHostAddress::LocalHost, endpointB.localPort());
    success &= expect(waitUntil([&] { return foreignDrops == 1; }),
                      QStringLiteral("非配置来源未被丢弃。"));

    QUdpSocket rawPeer;
    success &= expect(rawPeer.bind(QHostAddress::LocalHost, 0),
                      QStringLiteral("raw peer Socket 绑定失败。"));
    endpointB.configurePeer(QHostAddress::LocalHost, rawPeer.localPort());
    rawPeer.writeDatagram(QByteArray("invalid"), QHostAddress::LocalHost, endpointB.localPort());
    success &= expect(waitUntil([&] { return rejected == 1; }),
                      QStringLiteral("错误 ACL1 数据报未被拒绝。"));

    endpointA.close();
    endpointB.close();
    success &= expect(!endpointA.isBound() && !endpointB.isBound(),
                      QStringLiteral("close() 后仍绑定。"));
    success &= expect(endpointA.bindReceiver(QHostAddress::LocalHost, 0, &error),
                      QStringLiteral("close 后 A 重新绑定失败：%1").arg(error));
    endpointA.close();
    return success;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    return testLoopbackAndValidation() ? 0 : 1;
}
