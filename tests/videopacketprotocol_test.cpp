#include "videopacketprotocol.h"

#include <QDebug>

using namespace VideoPacketProtocol;

namespace
{

QByteArray makeDeterministicData(qsizetype size)
{
    QByteArray data;
    data.resize(size);
    for (qsizetype index = 0; index < size; ++index) {
        data[index] = static_cast<char>((index * 37 + 11) & 0xff);
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

bool testSingleFragmentRoundTrip()
{
    const QByteArray source = makeDeterministicData(100);
    QString error;
    const QVector<QByteArray> datagrams =
        fragmentEncodedFrame(source, 11, 22, 33, &error);
    if (!expect(error.isEmpty() && datagrams.size() == 1,
                QStringLiteral("单分片：未生成唯一有效数据报。"))) {
        return false;
    }
    if (!expect(datagrams.constFirst().size() <= MaximumDatagramSize,
                QStringLiteral("单分片：数据报超过上限。"))) {
        return false;
    }

    VideoFragmentPacket packet;
    if (!expect(parseVideoFragment(datagrams.constFirst(), &packet, &error),
                QStringLiteral("单分片：解析失败：%1").arg(error))) {
        return false;
    }
    return expect(packet.header.sessionId == 11
                      && packet.header.frameId == 22
                      && packet.header.timestampMs == 33
                      && packet.header.frameSize == source.size()
                      && packet.header.fragmentIndex == 0
                      && packet.header.fragmentCount == 1
                      && packet.header.payloadSize == source.size()
                      && packet.payload == source,
                  QStringLiteral("单分片：头字段或负载不正确。"));
}

bool testMultipleFragmentRoundTrip()
{
    const QByteArray source = makeDeterministicData(50000);
    QString error;
    const QVector<QByteArray> datagrams =
        fragmentEncodedFrame(source, 101, 202, 303, &error);
    if (!expect(error.isEmpty() && datagrams.size() > 1,
                QStringLiteral("多分片：未生成多个有效数据报。"))) {
        return false;
    }

    QVector<QByteArray> payloads;
    payloads.resize(datagrams.size());
    for (qsizetype index = 0; index < datagrams.size(); ++index) {
        VideoFragmentPacket packet;
        if (!expect(parseVideoFragment(datagrams.at(index), &packet, &error),
                    QStringLiteral("多分片：第 %1 个数据报解析失败：%2")
                        .arg(index)
                        .arg(error))) {
            return false;
        }
        if (!expect(packet.header.fragmentIndex == index
                        && packet.header.fragmentCount == datagrams.size()
                        && packet.header.frameSize == source.size()
                        && packet.header.sessionId == 101
                        && packet.header.frameId == 202
                        && packet.header.timestampMs == 303,
                    QStringLiteral("多分片：第 %1 个头字段不正确。").arg(index))) {
            return false;
        }
        payloads[packet.header.fragmentIndex] = packet.payload;
    }

    QByteArray reconstructed;
    for (const QByteArray &payload : payloads) {
        reconstructed.append(payload);
    }
    return expect(reconstructed == source,
                  QStringLiteral("多分片：按索引重组后的数据不一致。"));
}

bool testMaximumSinglePayload()
{
    const QByteArray source = makeDeterministicData(MaximumPayloadSize);
    QString error;
    const QVector<QByteArray> datagrams =
        fragmentEncodedFrame(source, 1, 2, 3, &error);
    return expect(error.isEmpty()
                      && datagrams.size() == 1
                      && datagrams.constFirst().size() == MaximumDatagramSize,
                  QStringLiteral("最大单包负载：结果不等于一个 1200 字节数据报。"));
}

bool testFrameTooLarge()
{
    const QByteArray source(MaximumFrameSize + 1, '\x5a');
    QString error;
    const QVector<QByteArray> datagrams =
        fragmentEncodedFrame(source, 1, 2, 3, &error);
    return expect(datagrams.isEmpty()
                      && error == QStringLiteral("编码帧大小超过协议上限。"),
                  QStringLiteral("超过最大帧：未返回预期中文错误。"));
}

QByteArray validDatagram()
{
    QString error;
    const QVector<QByteArray> datagrams =
        fragmentEncodedFrame(makeDeterministicData(100), 1, 2, 3, &error);
    if (!error.isEmpty() || datagrams.size() != 1) {
        qCritical().noquote() << QStringLiteral("测试前置数据报创建失败：%1").arg(error);
        return {};
    }
    return datagrams.constFirst();
}

bool testInvalidMagic()
{
    QByteArray datagram = validDatagram();
    datagram[0] = static_cast<char>(datagram.at(0) ^ 0x01);
    VideoFragmentPacket packet;
    return expect(!parseVideoFragment(datagram, &packet),
                  QStringLiteral("错误 magic：解析未拒绝。"));
}

bool testInvalidVersion()
{
    QByteArray datagram = validDatagram();
    datagram[4] = 2;
    VideoFragmentPacket packet;
    return expect(!parseVideoFragment(datagram, &packet),
                  QStringLiteral("错误版本：解析未拒绝。"));
}

bool testTruncatedDatagram()
{
    QByteArray datagram = validDatagram();
    datagram.chop(1);
    VideoFragmentPacket packet;
    return expect(!parseVideoFragment(datagram, &packet),
                  QStringLiteral("截断数据报：解析未拒绝。"));
}

bool testExtraData()
{
    QByteArray datagram = validDatagram();
    datagram.append('\0');
    VideoFragmentPacket packet;
    return expect(!parseVideoFragment(datagram, &packet),
                  QStringLiteral("多余数据：解析未拒绝。"));
}

bool testInvalidFragmentIndex()
{
    VideoFragmentHeader header;
    header.frameSize = 10;
    header.fragmentIndex = 1;
    header.fragmentCount = 1;
    header.payloadSize = 10;
    QString error;
    const QByteArray datagram =
        serializeVideoFragment(header, makeDeterministicData(10), &error);
    return expect(datagram.isEmpty() && !error.isEmpty(),
                  QStringLiteral("非法 fragmentIndex：序列化未拒绝。"));
}

bool testPayloadSizeMismatch()
{
    VideoFragmentHeader header;
    header.frameSize = 10;
    header.fragmentIndex = 0;
    header.fragmentCount = 1;
    header.payloadSize = 9;
    QString error;
    const QByteArray datagram =
        serializeVideoFragment(header, makeDeterministicData(10), &error);
    return expect(datagram.isEmpty() && !error.isEmpty(),
                  QStringLiteral("payloadSize 不一致：序列化未拒绝。"));
}

bool runTest(const QString &name, bool (*testFunction)())
{
    const bool result = testFunction();
    if (result) {
        qInfo().noquote() << QStringLiteral("通过：%1").arg(name);
    }
    return result;
}

} // namespace

int main()
{
    bool allPassed = true;
    allPassed = runTest(QStringLiteral("单分片往返"), testSingleFragmentRoundTrip) && allPassed;
    allPassed = runTest(QStringLiteral("多分片往返"), testMultipleFragmentRoundTrip) && allPassed;
    allPassed = runTest(QStringLiteral("最大单包负载"), testMaximumSinglePayload) && allPassed;
    allPassed = runTest(QStringLiteral("超过最大帧"), testFrameTooLarge) && allPassed;
    allPassed = runTest(QStringLiteral("错误 magic"), testInvalidMagic) && allPassed;
    allPassed = runTest(QStringLiteral("错误版本"), testInvalidVersion) && allPassed;
    allPassed = runTest(QStringLiteral("截断数据报"), testTruncatedDatagram) && allPassed;
    allPassed = runTest(QStringLiteral("多余数据"), testExtraData) && allPassed;
    allPassed = runTest(QStringLiteral("非法 fragmentIndex"), testInvalidFragmentIndex) && allPassed;
    allPassed = runTest(QStringLiteral("payloadSize 不一致"), testPayloadSizeMismatch) && allPassed;

    return allPassed ? 0 : 1;
}
