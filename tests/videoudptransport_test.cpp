#include "videoudptransport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QHostAddress>
#include <QTimer>

#include <algorithm>
#include <functional>

namespace
{

struct ReceivedFrame
{
    QByteArray data;
    quint32 sessionId = 0;
    quint32 frameId = 0;
    quint32 timestampMs = 0;
    QHostAddress senderAddress;
    quint16 senderPort = 0;
};

struct SentFrame
{
    quint32 frameId = 0;
    qsizetype frameSize = 0;
    qsizetype fragmentCount = 0;
};

QByteArray makeDeterministicData(qsizetype size, quint32 seed)
{
    QByteArray data;
    data.resize(size);
    for (qsizetype index = 0; index < size; ++index) {
        data[index] = static_cast<char>(
            (static_cast<quint64>(index) * 29 + static_cast<quint64>(seed) * 13) & 0xffULL);
    }
    return data;
}

bool expect(bool condition, const QString &message)
{
    if (!condition) {
        qCritical().noquote() << message;
        return false;
    }
    return true;
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 3000)
{
    if (predicate()) {
        return true;
    }

    QEventLoop eventLoop;
    QTimer timeoutTimer;
    QTimer pollTimer;
    bool completed = false;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(timeoutMs);
    pollTimer.setInterval(10);

    QObject::connect(&timeoutTimer, &QTimer::timeout, &eventLoop, &QEventLoop::quit);
    QObject::connect(&pollTimer, &QTimer::timeout, &eventLoop, [&] {
        if (predicate()) {
            completed = true;
            eventLoop.quit();
        }
    });

    timeoutTimer.start();
    pollTimer.start();
    eventLoop.exec();
    return completed || predicate();
}

bool testUdpLoopback()
{
    VideoUdpTransport unboundTransport;
    QString error;
    if (!expect(!unboundTransport.sendEncodedFrame(makeDeterministicData(100, 1), &error)
                    && !error.isEmpty(),
                QStringLiteral("未绑定 Socket 发送未失败。"))) {
        return false;
    }

    VideoUdpTransport noPeerTransport;
    if (!expect(noPeerTransport.bindReceiver(QHostAddress::LocalHost, 0, &error),
                QStringLiteral("无对端测试绑定失败：%1").arg(error))
        || !expect(!noPeerTransport.sendEncodedFrame(makeDeterministicData(100, 2), &error)
                       && !error.isEmpty(),
                   QStringLiteral("未配置对端发送未失败。"))) {
        return false;
    }
    noPeerTransport.close();

    VideoUdpTransport endpointA;
    VideoUdpTransport endpointB;
    if (!expect(endpointA.bindReceiver(QHostAddress::LocalHost, 0, &error),
                QStringLiteral("端点 A 绑定失败：%1").arg(error))
        || !expect(endpointB.bindReceiver(QHostAddress::LocalHost, 0, &error),
                   QStringLiteral("端点 B 绑定失败：%1").arg(error))) {
        return false;
    }

    endpointA.configurePeer(QHostAddress::LocalHost, endpointB.localPort());
    endpointB.configurePeer(QHostAddress::LocalHost, endpointA.localPort());

    QVector<ReceivedFrame> receivedByA;
    QVector<ReceivedFrame> receivedByB;
    QVector<SentFrame> sentByA;
    QVector<SentFrame> sentByB;
    QObject::connect(&endpointA,
                     &VideoUdpTransport::frameReceived,
                     [&receivedByA](const QByteArray &data,
                                    quint32 sessionId,
                                    quint32 frameId,
                                    quint32 timestampMs,
                                    const QHostAddress &senderAddress,
                                    quint16 senderPort) {
                         receivedByA.append({data, sessionId, frameId, timestampMs,
                                             senderAddress, senderPort});
                     });
    QObject::connect(&endpointB,
                     &VideoUdpTransport::frameReceived,
                     [&receivedByB](const QByteArray &data,
                                    quint32 sessionId,
                                    quint32 frameId,
                                    quint32 timestampMs,
                                    const QHostAddress &senderAddress,
                                    quint16 senderPort) {
                         receivedByB.append({data, sessionId, frameId, timestampMs,
                                             senderAddress, senderPort});
                     });
    QObject::connect(&endpointA,
                     &VideoUdpTransport::frameSent,
                     [&sentByA](quint32 frameId, qsizetype frameSize, qsizetype fragmentCount) {
                         sentByA.append({frameId, frameSize, fragmentCount});
                     });
    QObject::connect(&endpointB,
                     &VideoUdpTransport::frameSent,
                     [&sentByB](quint32 frameId, qsizetype frameSize, qsizetype fragmentCount) {
                         sentByB.append({frameId, frameSize, fragmentCount});
                     });

    const QByteArray smallFrame = makeDeterministicData(100, 3);
    if (!expect(endpointA.sendEncodedFrame(smallFrame, &error),
                QStringLiteral("A 到 B 的 100-byte 发送失败：%1").arg(error))
        || !expect(waitUntil([&receivedByB] { return receivedByB.size() == 1; }),
                   QStringLiteral("A 到 B 的 100-byte 接收超时。"))) {
        return false;
    }
    if (!expect(receivedByB.constFirst().data == smallFrame
                    && receivedByB.constFirst().sessionId == endpointA.sessionId()
                    && receivedByB.constFirst().frameId == 1
                    && sentByA.constFirst().fragmentCount == 1,
                QStringLiteral("A 到 B 的 100-byte 回环结果不正确。"))) {
        return false;
    }

    receivedByB.clear();
    sentByA.clear();
    const QByteArray largeFrameA = makeDeterministicData(50000, 4);
    if (!expect(endpointA.sendEncodedFrame(largeFrameA, &error),
                QStringLiteral("A 到 B 的 50000-byte 发送失败：%1").arg(error))
        || !expect(waitUntil([&receivedByB] { return receivedByB.size() == 1; }),
                   QStringLiteral("A 到 B 的 50000-byte 接收超时。"))) {
        return false;
    }
    if (!expect(receivedByB.constFirst().data == largeFrameA
                    && receivedByB.constFirst().sessionId == endpointA.sessionId()
                    && receivedByB.constFirst().frameId == 2
                    && sentByA.constFirst().frameId == 2
                    && sentByA.constFirst().fragmentCount > 1,
                QStringLiteral("A 到 B 的 50000-byte 多分片回环结果不正确。"))) {
        return false;
    }

    receivedByA.clear();
    sentByB.clear();
    const QByteArray largeFrameB = makeDeterministicData(50000, 5);
    if (!expect(endpointB.sendEncodedFrame(largeFrameB, &error),
                QStringLiteral("B 到 A 的 50000-byte 发送失败：%1").arg(error))
        || !expect(waitUntil([&receivedByA] { return receivedByA.size() == 1; }),
                   QStringLiteral("B 到 A 的 50000-byte 接收超时。"))) {
        return false;
    }
    if (!expect(receivedByA.constFirst().data == largeFrameB
                    && receivedByA.constFirst().sessionId == endpointB.sessionId()
                    && receivedByA.constFirst().frameId == 1
                    && sentByB.constFirst().frameId == 1
                    && sentByB.constFirst().fragmentCount > 1,
                QStringLiteral("B 到 A 的 50000-byte 多分片回环结果不正确。"))) {
        return false;
    }

    receivedByB.clear();
    sentByA.clear();
    for (quint32 index = 0; index < 5; ++index) {
        const QByteArray frame = makeDeterministicData(50000, 100 + index);
        if (!expect(endpointA.sendEncodedFrame(frame, &error),
                    QStringLiteral("连续发送第 %1 帧失败：%2").arg(index).arg(error))) {
            return false;
        }
        if (!expect(waitUntil([&receivedByB, index] { return receivedByB.size() == index + 1; }),
                    QStringLiteral("连续第 %1 帧接收超时。").arg(index))) {
            return false;
        }
    }
    if (!expect(sentByA.size() == 5, QStringLiteral("连续发送未产生 5 个发送信号。"))) {
        return false;
    }
    for (quint32 index = 0; index < 5; ++index) {
        const quint32 expectedFrameId = 3 + index;
        const auto receivedIterator = std::find_if(
            receivedByB.cbegin(),
            receivedByB.cend(),
            [expectedFrameId](const ReceivedFrame &received) {
                return received.frameId == expectedFrameId;
            });
        if (!expect(receivedIterator != receivedByB.cend()
                        && receivedIterator->data == makeDeterministicData(50000, 100 + index)
                        && receivedIterator->sessionId == endpointA.sessionId()
                        && sentByA.at(index).frameId == expectedFrameId
                        && sentByA.at(index).fragmentCount > 1,
                    QStringLiteral("连续第 %1 帧内容、会话或帧编号不正确。").arg(index))) {
            return false;
        }
    }

    endpointA.close();
    endpointB.close();
    return expect(!endpointA.isBound() && !endpointB.isBound(),
                  QStringLiteral("close() 后 Socket 仍处于绑定状态。"));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    return testUdpLoopback() ? 0 : 1;
}
