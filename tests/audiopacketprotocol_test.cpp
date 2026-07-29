#include "audiopacketprotocol.h"

#include <QDebug>

using namespace AudioPacketProtocol;

namespace {

bool expect(bool condition, const QString &message)
{
    if (!condition) {
        qCritical().noquote() << QStringLiteral("[audio_packet_protocol_test]") << message;
    }
    return condition;
}

QByteArray makePcm()
{
    QByteArray pcm(PcmPayloadSize, '\0');
    for (qsizetype index = 0; index < pcm.size(); ++index) {
        pcm[index] = static_cast<char>((index * 17 + 9) & 0xff);
    }
    return pcm;
}

QByteArray validDatagram()
{
    AudioPacketHeader header;
    header.sessionId = 1234;
    header.sequence = 42;
    header.timestampSamples = 960;
    QString error;
    const QByteArray datagram = serializeAudioPacket(header, makePcm(), &error);
    if (datagram.isEmpty()) {
        qCritical().noquote() << QStringLiteral("创建有效 ACL1 数据报失败：%1").arg(error);
    }
    return datagram;
}

bool testRoundTripAndBigEndianHeader()
{
    const QByteArray datagram = validDatagram();
    AudioPacket packet;
    QString error;
    bool success = true;
    success &= expect(datagram.size() == DatagramSize,
                      QStringLiteral("ACL1 数据报长度不是 672。"));
    success &= expect(static_cast<uchar>(datagram.at(0)) == 0x41
                          && static_cast<uchar>(datagram.at(1)) == 0x43
                          && static_cast<uchar>(datagram.at(2)) == 0x4c
                          && static_cast<uchar>(datagram.at(3)) == 0x31,
                      QStringLiteral("ACL1 magic 未按 Big Endian 写入。"));
    success &= expect(static_cast<uchar>(datagram.at(8)) == 0
                          && static_cast<uchar>(datagram.at(11)) == 0xd2,
                      QStringLiteral("sessionId 未按 Big Endian 写入。"));
    success &= expect(parseAudioPacket(datagram, &packet, &error),
                      QStringLiteral("合法 ACL1 数据报解析失败：%1").arg(error));
    success &= expect(packet.header.sessionId == 1234
                          && packet.header.sequence == 42
                          && packet.header.timestampSamples == 960
                          && packet.header.sampleRate == SampleRate
                          && packet.header.channels == Channels
                          && packet.header.sampleFormat == Int16SampleFormat
                          && packet.header.samplesPerChannel == SamplesPerChannel
                          && packet.header.payloadSize == PcmPayloadSize
                          && packet.pcmPayload == makePcm(),
                      QStringLiteral("ACL1 往返后的字段或 PCM 内容不一致。"));
    return success;
}

bool parseMustFail(QByteArray datagram, const QString &name)
{
    AudioPacket packet;
    QString error;
    return expect(!parseAudioPacket(datagram, &packet, &error) && !error.isEmpty(),
                  QStringLiteral("%1：解析未拒绝。").arg(name));
}

bool testInvalidDatagrams()
{
    bool success = true;
    QByteArray datagram = validDatagram();
    datagram[0] ^= 0x01;
    success &= parseMustFail(datagram, QStringLiteral("错误 magic"));

    datagram = validDatagram();
    datagram[4] = 2;
    success &= parseMustFail(datagram, QStringLiteral("错误 version"));
    datagram = validDatagram();
    datagram[5] = 2;
    success &= parseMustFail(datagram, QStringLiteral("错误 packetType"));
    datagram = validDatagram();
    datagram[6] = 0;
    datagram[7] = 0;
    success &= parseMustFail(datagram, QStringLiteral("错误 headerSize"));
    datagram = validDatagram();
    datagram.replace(8, 4, QByteArray(4, '\0'));
    success &= parseMustFail(datagram, QStringLiteral("sessionId 为 0"));
    datagram = validDatagram();
    datagram.replace(12, 4, QByteArray(4, '\0'));
    success &= parseMustFail(datagram, QStringLiteral("sequence 为 0"));
    datagram = validDatagram();
    datagram[23] = 1;
    success &= parseMustFail(datagram, QStringLiteral("错误采样率"));
    datagram = validDatagram();
    datagram[25] = 2;
    success &= parseMustFail(datagram, QStringLiteral("错误声道数"));
    datagram = validDatagram();
    datagram[27] = 2;
    success &= parseMustFail(datagram, QStringLiteral("错误 sampleFormat"));
    datagram = validDatagram();
    datagram[29] = 1;
    success &= parseMustFail(datagram, QStringLiteral("错误 samplesPerChannel"));
    datagram = validDatagram();
    datagram[30] = 0;
    datagram[31] = 0;
    success &= parseMustFail(datagram, QStringLiteral("错误 payloadSize"));
    datagram = validDatagram();
    datagram.chop(1);
    success &= parseMustFail(datagram, QStringLiteral("截断数据报"));
    datagram = validDatagram();
    datagram.append('\0');
    success &= parseMustFail(datagram, QStringLiteral("尾部多余字节"));
    return success;
}

bool testPayloadValidation()
{
    AudioPacketHeader header;
    header.sessionId = 1;
    header.sequence = 1;
    QString error;
    bool success = true;
    success &= expect(!validatePcmPayload({}, &error) && !error.isEmpty(),
                      QStringLiteral("空 PCM payload 未拒绝。"));
    success &= expect(serializeAudioPacket(header, QByteArray(639, '\0'), &error).isEmpty(),
                      QStringLiteral("639-byte PCM payload 未拒绝。"));
    success &= expect(serializeAudioPacket(header, QByteArray(641, '\0'), &error).isEmpty(),
                      QStringLiteral("641-byte PCM payload 未拒绝。"));
    return success;
}

} // namespace

int main()
{
    const bool success = testRoundTripAndBigEndianHeader()
        && testInvalidDatagrams()
        && testPayloadValidation();
    return success ? 0 : 1;
}
