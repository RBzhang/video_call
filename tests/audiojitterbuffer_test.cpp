#include "audiojitterbuffer.h"

#include "audiopacketprotocol.h"

#include <QDebug>

#include <limits>

namespace {

bool expect(bool condition, const QString &message)
{
    if (!condition) {
        qCritical().noquote() << QStringLiteral("[audio_jitter_buffer_test]") << message;
    }
    return condition;
}

QByteArray pcmFor(quint32 sequence)
{
    QByteArray pcm(AudioPacketProtocol::PcmPayloadSize, '\0');
    pcm[0] = static_cast<char>(sequence & 0xff);
    pcm[1] = static_cast<char>((sequence >> 8) & 0xff);
    return pcm;
}

bool isSilence(const QByteArray &packet)
{
    if (packet.size() != AudioPacketProtocol::PcmPayloadSize) {
        return false;
    }
    for (const char byte : packet) {
        if (byte != '\0') {
            return false;
        }
    }
    return true;
}

void insertThree(AudioJitterBuffer *buffer, quint32 sessionId, quint32 firstSequence)
{
    buffer->insertPacket(sessionId, firstSequence, pcmFor(firstSequence));
    buffer->insertPacket(sessionId, firstSequence + 1, pcmFor(firstSequence + 1));
    buffer->insertPacket(sessionId, firstSequence + 2, pcmFor(firstSequence + 2));
}

bool testOrderPrebufferAndDuplicate()
{
    AudioJitterBuffer buffer;
    bool success = true;
    success &= expect(buffer.insertPacket(7, 2, pcmFor(2)), QStringLiteral("插入 sequence 2 失败。"));
    success &= expect(buffer.insertPacket(7, 1, pcmFor(1)), QStringLiteral("乱序插入 sequence 1 失败。"));
    success &= expect(buffer.insertPacket(7, 3, pcmFor(3)), QStringLiteral("插入 sequence 3 失败。"));
    success &= expect(buffer.isPlaying() == false, QStringLiteral("播放不应在取包前开始。"));
    success &= expect(buffer.takeNextPacket() == pcmFor(1), QStringLiteral("乱序后未先输出 sequence 1。"));
    success &= expect(buffer.isPlaying(), QStringLiteral("达到三包预缓冲后未开始播放。"));
    success &= expect(buffer.takeNextPacket() == pcmFor(2), QStringLiteral("未输出 sequence 2。"));
    success &= expect(buffer.takeNextPacket() == pcmFor(3), QStringLiteral("未输出 sequence 3。"));
    success &= expect(!buffer.insertPacket(7, 2, pcmFor(2)), QStringLiteral("迟到包未丢弃。"));
    success &= expect(buffer.statistics().latePackets == 1, QStringLiteral("迟到统计不正确。"));

    AudioJitterBuffer duplicates;
    success &= expect(duplicates.insertPacket(8, 1, pcmFor(1)), QStringLiteral("重复测试首包插入失败。"));
    success &= expect(!duplicates.insertPacket(8, 1, pcmFor(1)), QStringLiteral("重复包未丢弃。"));
    success &= expect(duplicates.statistics().duplicatePackets == 1,
                      QStringLiteral("重复包统计不正确。"));
    return success;
}

bool testConcealmentAndRebuffer()
{
    AudioJitterBuffer buffer;
    buffer.insertPacket(9, 1, pcmFor(1));
    buffer.insertPacket(9, 3, pcmFor(3));
    buffer.insertPacket(9, 4, pcmFor(4));
    bool success = true;
    success &= expect(buffer.takeNextPacket() == pcmFor(1), QStringLiteral("缺包测试未输出首包。"));
    success &= expect(isSilence(buffer.takeNextPacket()), QStringLiteral("缺失包未输出 640-byte 静音。"));
    success &= expect(buffer.takeNextPacket() == pcmFor(3), QStringLiteral("静音后未恢复 sequence 3。"));
    success &= expect(buffer.statistics().concealedPackets == 1, QStringLiteral("concealed 统计不正确。"));

    AudioJitterBuffer rebuffer;
    rebuffer.insertPacket(10, 1, pcmFor(1));
    rebuffer.insertPacket(10, 10, pcmFor(10));
    rebuffer.insertPacket(10, 11, pcmFor(11));
    success &= expect(rebuffer.takeNextPacket() == pcmFor(1), QStringLiteral("重缓冲测试首包错误。"));
    for (int index = 0; index < AudioJitterBuffer::ConsecutiveMissingRebufferThreshold; ++index) {
        success &= expect(isSilence(rebuffer.takeNextPacket()),
                          QStringLiteral("连续缺失未输出静音。"));
    }
    success &= expect(!rebuffer.isPlaying(), QStringLiteral("连续五包缺失后未重新预缓冲。"));
    success &= expect(rebuffer.currentBufferedPackets() == 0,
                      QStringLiteral("重缓冲后旧包未清空。"));
    return success;
}

bool testSessionCapacityWrapAndClear()
{
    AudioJitterBuffer buffer;
    insertThree(&buffer, 11, 1);
    buffer.insertPacket(12, 100, pcmFor(100));
    bool success = true;
    success &= expect(buffer.sessionId() == 12 && buffer.currentBufferedPackets() == 1
                          && buffer.statistics().sessionResets == 1,
                      QStringLiteral("新 session 未正确重置。"));

    AudioJitterBuffer capacity;
    for (quint32 sequence = 1; sequence <= 11; ++sequence) {
        capacity.insertPacket(13, sequence, pcmFor(sequence));
    }
    success &= expect(capacity.currentBufferedPackets() == AudioJitterBuffer::MaximumBufferedPackets,
                      QStringLiteral("最大十包缓存限制未生效。"));
    success &= expect(capacity.statistics().bufferOverflowPackets >= 1,
                      QStringLiteral("缓存溢出统计未增加。"));

    AudioJitterBuffer wrap;
    const quint32 beforeWrap = std::numeric_limits<quint32>::max() - 1;
    wrap.insertPacket(14, beforeWrap, pcmFor(beforeWrap));
    wrap.insertPacket(14, std::numeric_limits<quint32>::max(),
                      pcmFor(std::numeric_limits<quint32>::max()));
    wrap.insertPacket(14, 1, pcmFor(1));
    success &= expect(wrap.takeNextPacket() == pcmFor(beforeWrap),
                      QStringLiteral("回绕前 sequence 输出错误。"));
    success &= expect(wrap.takeNextPacket() == pcmFor(std::numeric_limits<quint32>::max()),
                      QStringLiteral("最大 sequence 输出错误。"));
    success &= expect(wrap.takeNextPacket() == pcmFor(1),
                      QStringLiteral("sequence 回绕后输出错误。"));

    wrap.clear();
    success &= expect(!wrap.isPlaying() && wrap.currentBufferedPackets() == 0 && wrap.sessionId() == 0,
                      QStringLiteral("clear() 后状态未重置。"));
    return success;
}

} // namespace

int main()
{
    const bool success = testOrderPrebufferAndDuplicate()
        && testConcealmentAndRebuffer()
        && testSessionCapacityWrapAndClear();
    return success ? 0 : 1;
}
