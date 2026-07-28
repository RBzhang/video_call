#include "videoframereassembler.h"
#include "videopacketprotocol.h"

#include <QDebug>
#include <QHostAddress>

#include <algorithm>

using namespace VideoPacketProtocol;

namespace
{

QByteArray makeDeterministicData(qsizetype size, quint32 seed)
{
    QByteArray data;
    data.resize(size);
    for (qsizetype index = 0; index < size; ++index) {
        data[index] = static_cast<char>(
            (static_cast<quint64>(index) * 31 + static_cast<quint64>(seed) * 17) & 0xffULL);
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

QVector<QByteArray> makeDatagrams(const QByteArray &frame,
                                  quint32 sessionId,
                                  quint32 frameId,
                                  quint32 timestampMs)
{
    QString error;
    const QVector<QByteArray> datagrams =
        fragmentEncodedFrame(frame, sessionId, frameId, timestampMs, &error);
    if (datagrams.isEmpty()) {
        qCritical().noquote() << QStringLiteral("创建测试分片失败：%1").arg(error);
    }
    return datagrams;
}

bool addInOrder(VideoFrameReassembler &reassembler,
                const QVector<QByteArray> &datagrams,
                const QVector<qsizetype> &order,
                const QHostAddress &senderAddress,
                quint16 senderPort,
                VideoFrameReassembler::CompletedFrame *completedFrame,
                VideoFrameReassembler::AddResult *lastResult = nullptr)
{
    VideoFrameReassembler::CompletedFrame completed;
    VideoFrameReassembler::AddResult result = VideoFrameReassembler::AddResult::Rejected;
    QString error;
    for (qsizetype position = 0; position < order.size(); ++position) {
        result = reassembler.addDatagram(datagrams.at(order.at(position)),
                                         senderAddress,
                                         senderPort,
                                         position,
                                         &completed,
                                         &error);
        if (result == VideoFrameReassembler::AddResult::Rejected) {
            qCritical().noquote()
                << QStringLiteral("重组时第 %1 个分片被拒绝：%2").arg(position).arg(error);
            return false;
        }
    }
    if (completedFrame) {
        *completedFrame = completed;
    }
    if (lastResult) {
        *lastResult = result;
    }
    return true;
}

QVector<qsizetype> normalOrder(qsizetype size)
{
    QVector<qsizetype> order;
    order.reserve(size);
    for (qsizetype index = 0; index < size; ++index) {
        order.append(index);
    }
    return order;
}

bool testSingleFragment()
{
    const QByteArray source = makeDeterministicData(100, 1);
    const QVector<QByteArray> datagrams = makeDatagrams(source, 10, 20, 30);
    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame completed;
    VideoFrameReassembler::AddResult result;
    if (!addInOrder(reassembler,
                    datagrams,
                    normalOrder(datagrams.size()),
                    QHostAddress::LocalHost,
                    4000,
                    &completed,
                    &result)) {
        return false;
    }
    return expect(result == VideoFrameReassembler::AddResult::Completed
                      && completed.encodedFrame == source
                      && completed.sessionId == 10
                      && completed.frameId == 20
                      && completed.timestampMs == 30
                      && completed.senderAddress == QHostAddress::LocalHost
                      && completed.senderPort == 4000
                      && reassembler.pendingFrameCount() == 0,
                  QStringLiteral("单分片重组结果不正确。"));
}

bool testMultipleNormalOrder()
{
    const QByteArray source = makeDeterministicData(50000, 2);
    const QVector<QByteArray> datagrams = makeDatagrams(source, 11, 21, 31);
    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame completed;
    VideoFrameReassembler::AddResult result;
    if (!addInOrder(reassembler,
                    datagrams,
                    normalOrder(datagrams.size()),
                    QHostAddress::LocalHost,
                    4001,
                    &completed,
                    &result)) {
        return false;
    }
    return expect(result == VideoFrameReassembler::AddResult::Completed
                      && completed.encodedFrame == source,
                  QStringLiteral("多分片顺序重组结果不正确。"));
}

bool testMultipleReverseOrder()
{
    const QByteArray source = makeDeterministicData(50000, 3);
    const QVector<QByteArray> datagrams = makeDatagrams(source, 12, 22, 32);
    QVector<qsizetype> order = normalOrder(datagrams.size());
    std::reverse(order.begin(), order.end());
    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame completed;
    VideoFrameReassembler::AddResult result;
    if (!addInOrder(reassembler,
                    datagrams,
                    order,
                    QHostAddress::LocalHost,
                    4002,
                    &completed,
                    &result)) {
        return false;
    }
    return expect(result == VideoFrameReassembler::AddResult::Completed
                      && completed.encodedFrame == source,
                  QStringLiteral("多分片逆序重组结果不正确。"));
}

bool testMultipleArbitraryOrder()
{
    const QByteArray source = makeDeterministicData(50000, 4);
    const QVector<QByteArray> datagrams = makeDatagrams(source, 13, 23, 33);
    QVector<qsizetype> order = normalOrder(datagrams.size());
    std::sort(order.begin(), order.end(), [](qsizetype left, qsizetype right) {
        return (left * 17) % 43 < (right * 17) % 43;
    });
    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame completed;
    VideoFrameReassembler::AddResult result;
    if (!addInOrder(reassembler,
                    datagrams,
                    order,
                    QHostAddress::LocalHost,
                    4003,
                    &completed,
                    &result)) {
        return false;
    }
    return expect(result == VideoFrameReassembler::AddResult::Completed
                      && completed.encodedFrame == source,
                  QStringLiteral("多分片乱序重组结果不正确。"));
}

bool testIdenticalDuplicate()
{
    const QByteArray source = makeDeterministicData(50000, 5);
    const QVector<QByteArray> datagrams = makeDatagrams(source, 14, 24, 34);
    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame completed;
    QString error;
    const auto firstResult = reassembler.addDatagram(datagrams.constFirst(),
                                                      QHostAddress::LocalHost,
                                                      4004,
                                                      0,
                                                      &completed,
                                                      &error);
    const auto duplicateResult = reassembler.addDatagram(datagrams.constFirst(),
                                                          QHostAddress::LocalHost,
                                                          4004,
                                                          1,
                                                          &completed,
                                                          &error);
    if (!expect(firstResult == VideoFrameReassembler::AddResult::Accepted
                    && duplicateResult == VideoFrameReassembler::AddResult::Duplicate,
                QStringLiteral("完全相同的重复分片未被正确识别。"))) {
        return false;
    }

    QVector<qsizetype> remaining;
    for (qsizetype index = 1; index < datagrams.size(); ++index) {
        remaining.append(index);
    }
    VideoFrameReassembler::AddResult result;
    if (!addInOrder(reassembler,
                    datagrams,
                    remaining,
                    QHostAddress::LocalHost,
                    4004,
                    &completed,
                    &result)) {
        return false;
    }
    return expect(result == VideoFrameReassembler::AddResult::Completed
                      && completed.encodedFrame == source,
                  QStringLiteral("重复分片后的完整帧不正确。"));
}

bool testConflictingDuplicate()
{
    const QVector<QByteArray> datagrams = makeDatagrams(makeDeterministicData(50000, 6), 15, 25, 35);
    VideoFragmentPacket packet;
    QString error;
    if (!parseVideoFragment(datagrams.constFirst(), &packet, &error)) {
        return expect(false, QStringLiteral("无法准备冲突重复分片：%1").arg(error));
    }
    packet.payload[0] = static_cast<char>(packet.payload.at(0) ^ 0x7f);
    const QByteArray conflicting = serializeVideoFragment(packet.header, packet.payload, &error);
    if (!expect(!conflicting.isEmpty(), QStringLiteral("无法构造冲突重复分片：%1").arg(error))) {
        return false;
    }

    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame completed;
    const auto firstResult = reassembler.addDatagram(datagrams.constFirst(),
                                                      QHostAddress::LocalHost,
                                                      4005,
                                                      0,
                                                      &completed,
                                                      &error);
    const auto conflictResult = reassembler.addDatagram(conflicting,
                                                         QHostAddress::LocalHost,
                                                         4005,
                                                         1,
                                                         &completed,
                                                         &error);
    return expect(firstResult == VideoFrameReassembler::AddResult::Accepted
                      && conflictResult == VideoFrameReassembler::AddResult::Rejected
                      && reassembler.pendingFrameCount() == 0,
                  QStringLiteral("内容冲突的重复分片未丢弃整帧。"));
}

bool testDifferentSessionIds()
{
    const QByteArray firstSource = makeDeterministicData(50000, 7);
    const QByteArray secondSource = makeDeterministicData(50000, 8);
    const QVector<QByteArray> firstDatagrams = makeDatagrams(firstSource, 100, 50, 40);
    const QVector<QByteArray> secondDatagrams = makeDatagrams(secondSource, 101, 50, 40);
    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame firstCompleted;
    VideoFrameReassembler::CompletedFrame secondCompleted;
    QString error;
    reassembler.addDatagram(firstDatagrams.constFirst(), QHostAddress::LocalHost, 4006, 0,
                            &firstCompleted, &error);
    reassembler.addDatagram(secondDatagrams.constFirst(), QHostAddress::LocalHost, 4006, 1,
                            &secondCompleted, &error);
    QVector<qsizetype> remaining = normalOrder(firstDatagrams.size());
    remaining.removeFirst();
    VideoFrameReassembler::AddResult result;
    if (!addInOrder(reassembler, firstDatagrams, remaining, QHostAddress::LocalHost, 4006,
                    &firstCompleted, &result)
        || !expect(firstCompleted.encodedFrame == firstSource,
                   QStringLiteral("不同会话的第一帧重组错误。"))
        || !addInOrder(reassembler, secondDatagrams, remaining, QHostAddress::LocalHost, 4006,
                       &secondCompleted, &result)) {
        return false;
    }
    return expect(secondCompleted.encodedFrame == secondSource,
                  QStringLiteral("不同 sessionId 的帧发生混合。"));
}

bool testDifferentSenderAddresses()
{
    const QByteArray firstSource = makeDeterministicData(50000, 9);
    const QByteArray secondSource = makeDeterministicData(50000, 10);
    const QVector<QByteArray> firstDatagrams = makeDatagrams(firstSource, 102, 51, 41);
    const QVector<QByteArray> secondDatagrams = makeDatagrams(secondSource, 102, 51, 41);
    const QHostAddress firstAddress(QStringLiteral("127.0.0.1"));
    const QHostAddress secondAddress(QStringLiteral("127.0.0.2"));
    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame firstCompleted;
    VideoFrameReassembler::CompletedFrame secondCompleted;
    QString error;
    reassembler.addDatagram(firstDatagrams.constFirst(), firstAddress, 4007, 0, &firstCompleted, &error);
    reassembler.addDatagram(secondDatagrams.constFirst(), secondAddress, 4007, 1, &secondCompleted, &error);
    QVector<qsizetype> remaining = normalOrder(firstDatagrams.size());
    remaining.removeFirst();
    VideoFrameReassembler::AddResult result;
    if (!addInOrder(reassembler, firstDatagrams, remaining, firstAddress, 4007,
                    &firstCompleted, &result)
        || !addInOrder(reassembler, secondDatagrams, remaining, secondAddress, 4007,
                       &secondCompleted, &result)) {
        return false;
    }
    return expect(firstCompleted.encodedFrame == firstSource
                      && secondCompleted.encodedFrame == secondSource,
                  QStringLiteral("不同发送地址的帧发生混合。"));
}

bool testDifferentSenderPorts()
{
    const QByteArray firstSource = makeDeterministicData(50000, 11);
    const QByteArray secondSource = makeDeterministicData(50000, 12);
    const QVector<QByteArray> firstDatagrams = makeDatagrams(firstSource, 103, 52, 42);
    const QVector<QByteArray> secondDatagrams = makeDatagrams(secondSource, 103, 52, 42);
    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame firstCompleted;
    VideoFrameReassembler::CompletedFrame secondCompleted;
    QString error;
    reassembler.addDatagram(firstDatagrams.constFirst(), QHostAddress::LocalHost, 4008, 0,
                            &firstCompleted, &error);
    reassembler.addDatagram(secondDatagrams.constFirst(), QHostAddress::LocalHost, 4009, 1,
                            &secondCompleted, &error);
    QVector<qsizetype> remaining = normalOrder(firstDatagrams.size());
    remaining.removeFirst();
    VideoFrameReassembler::AddResult result;
    if (!addInOrder(reassembler, firstDatagrams, remaining, QHostAddress::LocalHost, 4008,
                    &firstCompleted, &result)
        || !addInOrder(reassembler, secondDatagrams, remaining, QHostAddress::LocalHost, 4009,
                       &secondCompleted, &result)) {
        return false;
    }
    return expect(firstCompleted.encodedFrame == firstSource
                      && secondCompleted.encodedFrame == secondSource,
                  QStringLiteral("不同发送端口的帧发生混合。"));
}

bool testMetadataMismatch()
{
    const QVector<QByteArray> datagrams = makeDatagrams(makeDeterministicData(50000, 13), 104, 53, 43);
    VideoFragmentPacket packet;
    QString error;
    if (!parseVideoFragment(datagrams.constFirst(), &packet, &error)) {
        return expect(false, QStringLiteral("无法准备元数据冲突分片：%1").arg(error));
    }
    ++packet.header.timestampMs;
    const QByteArray inconsistent = serializeVideoFragment(packet.header, packet.payload, &error);
    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame completed;
    const auto firstResult = reassembler.addDatagram(datagrams.constFirst(), QHostAddress::LocalHost,
                                                      4010, 0, &completed, &error);
    const auto mismatchResult = reassembler.addDatagram(inconsistent, QHostAddress::LocalHost,
                                                         4010, 1, &completed, &error);
    return expect(!inconsistent.isEmpty()
                      && firstResult == VideoFrameReassembler::AddResult::Accepted
                      && mismatchResult == VideoFrameReassembler::AddResult::Rejected
                      && reassembler.pendingFrameCount() == 0,
                  QStringLiteral("元数据不一致未丢弃未完成帧。"));
}

bool testTimeoutCleanup()
{
    const QVector<QByteArray> datagrams = makeDatagrams(makeDeterministicData(50000, 14), 105, 54, 44);
    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame completed;
    QString error;
    const auto result = reassembler.addDatagram(datagrams.constFirst(), QHostAddress::LocalHost,
                                                4011, 0, &completed, &error);
    const int removed = reassembler.removeExpiredFrames(VideoFrameReassembler::FrameTimeoutMs + 1);
    return expect(result == VideoFrameReassembler::AddResult::Accepted
                      && removed == 1
                      && reassembler.pendingFrameCount() == 0
                      && reassembler.pendingPayloadBytes() == 0,
                  QStringLiteral("500 ms 超时清理不正确。"));
}

bool testClear()
{
    const QVector<QByteArray> datagrams = makeDatagrams(makeDeterministicData(50000, 15), 106, 55, 45);
    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame completed;
    QString error;
    reassembler.addDatagram(datagrams.constFirst(), QHostAddress::LocalHost,
                            4012, 0, &completed, &error);
    reassembler.clear();
    return expect(reassembler.pendingFrameCount() == 0 && reassembler.pendingPayloadBytes() == 0,
                  QStringLiteral("clear() 未清空重组缓存。"));
}

bool testPendingFrameCountLimit()
{
    VideoFrameReassembler reassembler;
    const QByteArray source = makeDeterministicData(MaximumPayloadSize + 1, 16);
    for (quint32 frameId = 1; frameId <= 17; ++frameId) {
        const QVector<QByteArray> datagrams = makeDatagrams(source, 107, frameId, 46);
        VideoFrameReassembler::CompletedFrame completed;
        QString error;
        const auto result = reassembler.addDatagram(datagrams.constFirst(), QHostAddress::LocalHost,
                                                    4013, frameId, &completed, &error);
        if (!expect(result == VideoFrameReassembler::AddResult::Accepted,
                    QStringLiteral("缓存帧数测试中的分片被拒绝：%1").arg(error))) {
            return false;
        }
    }
    return expect(reassembler.pendingFrameCount() == VideoFrameReassembler::MaximumPendingFrameCount
                      && reassembler.pendingPayloadBytes()
                          <= VideoFrameReassembler::MaximumPendingPayloadBytes,
                  QStringLiteral("未完成帧数量上限未生效。"));
}

QByteArray makeLargePendingDatagram(quint32 frameId, quint16 fragmentIndex)
{
    VideoFragmentHeader header;
    header.sessionId = 108;
    header.frameId = frameId;
    header.timestampMs = 47;
    header.frameSize = MaximumFrameSize;
    header.fragmentIndex = fragmentIndex;
    header.fragmentCount = MaximumFragmentCount;
    header.payloadSize = MaximumPayloadSize;
    return serializeVideoFragment(header, QByteArray(MaximumPayloadSize, '\x5a'));
}

bool testPendingPayloadByteLimit()
{
    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame completed;
    QString error;
    constexpr quint16 partialFragmentCount = 3591;
    for (quint32 frameId = 1; frameId <= 4; ++frameId) {
        for (quint16 fragmentIndex = 0; fragmentIndex < partialFragmentCount; ++fragmentIndex) {
            const QByteArray datagram = makeLargePendingDatagram(frameId, fragmentIndex);
            const auto result = reassembler.addDatagram(datagram,
                                                        QHostAddress::LocalHost,
                                                        4014,
                                                        fragmentIndex,
                                                        &completed,
                                                        &error);
            if (!expect(result == VideoFrameReassembler::AddResult::Accepted,
                        QStringLiteral("缓存字节测试分片被拒绝：%1").arg(error))) {
                return false;
            }
        }
    }

    VideoFragmentHeader nextHeader;
    nextHeader.sessionId = 108;
    nextHeader.frameId = 5;
    nextHeader.timestampMs = 47;
    nextHeader.frameSize = MaximumFrameSize;
    nextHeader.fragmentIndex = 0;
    nextHeader.fragmentCount = MaximumFragmentCount;
    nextHeader.payloadSize = 100;
    const QByteArray nextDatagram = serializeVideoFragment(nextHeader, QByteArray(100, '\x33'));
    const auto result = reassembler.addDatagram(nextDatagram,
                                                QHostAddress::LocalHost,
                                                4014,
                                                1000,
                                                &completed,
                                                &error);
    return expect(result == VideoFrameReassembler::AddResult::Accepted
                      && reassembler.pendingPayloadBytes()
                          <= VideoFrameReassembler::MaximumPendingPayloadBytes
                      && reassembler.pendingFrameCount() == 4,
                  QStringLiteral("缓存字节上限未通过淘汰最旧帧保持限制。"));
}

bool testCorruptDatagram()
{
    VideoFrameReassembler reassembler;
    VideoFrameReassembler::CompletedFrame completed;
    QString error;
    const auto result = reassembler.addDatagram(QByteArray("invalid"), QHostAddress::LocalHost,
                                                4015, 0, &completed, &error);
    return expect(result == VideoFrameReassembler::AddResult::Rejected
                      && !error.isEmpty()
                      && reassembler.pendingFrameCount() == 0,
                  QStringLiteral("损坏协议数据报未被拒绝。"));
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
    allPassed = runTest(QStringLiteral("单分片重组"), testSingleFragment) && allPassed;
    allPassed = runTest(QStringLiteral("多分片顺序重组"), testMultipleNormalOrder) && allPassed;
    allPassed = runTest(QStringLiteral("多分片逆序重组"), testMultipleReverseOrder) && allPassed;
    allPassed = runTest(QStringLiteral("多分片任意乱序重组"), testMultipleArbitraryOrder) && allPassed;
    allPassed = runTest(QStringLiteral("完全相同的重复分片"), testIdenticalDuplicate) && allPassed;
    allPassed = runTest(QStringLiteral("内容冲突的重复分片"), testConflictingDuplicate) && allPassed;
    allPassed = runTest(QStringLiteral("不同 sessionId"), testDifferentSessionIds) && allPassed;
    allPassed = runTest(QStringLiteral("不同 senderAddress"), testDifferentSenderAddresses) && allPassed;
    allPassed = runTest(QStringLiteral("不同 senderPort"), testDifferentSenderPorts) && allPassed;
    allPassed = runTest(QStringLiteral("元数据不一致"), testMetadataMismatch) && allPassed;
    allPassed = runTest(QStringLiteral("500 ms 超时清理"), testTimeoutCleanup) && allPassed;
    allPassed = runTest(QStringLiteral("clear 清空状态"), testClear) && allPassed;
    allPassed = runTest(QStringLiteral("缓存帧数上限"), testPendingFrameCountLimit) && allPassed;
    allPassed = runTest(QStringLiteral("缓存字节上限"), testPendingPayloadByteLimit) && allPassed;
    allPassed = runTest(QStringLiteral("损坏协议数据报"), testCorruptDatagram) && allPassed;
    return allPassed ? 0 : 1;
}
