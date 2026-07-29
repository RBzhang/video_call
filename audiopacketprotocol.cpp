#include "audiopacketprotocol.h"

#include <QBuffer>
#include <QDataStream>

#include <utility>

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

bool validateHeader(const AudioPacketProtocol::AudioPacketHeader &header,
                    QString *errorMessage)
{
    using namespace AudioPacketProtocol;

    if (header.magic != Magic) {
        setError(errorMessage, QStringLiteral("ACL1 magic 无效。"));
        return false;
    }
    if (header.version != Version) {
        setError(errorMessage, QStringLiteral("ACL1 version 无效。"));
        return false;
    }
    if (header.packetType != PcmPacketType) {
        setError(errorMessage, QStringLiteral("ACL1 packetType 无效。"));
        return false;
    }
    if (header.headerSize != HeaderSize) {
        setError(errorMessage, QStringLiteral("ACL1 headerSize 必须为 32。"));
        return false;
    }
    if (header.sessionId == 0) {
        setError(errorMessage, QStringLiteral("ACL1 sessionId 不能为 0。"));
        return false;
    }
    if (header.sequence == 0) {
        setError(errorMessage, QStringLiteral("ACL1 sequence 不能为 0。"));
        return false;
    }
    if (header.sampleRate != SampleRate) {
        setError(errorMessage, QStringLiteral("ACL1 sampleRate 必须为 16000。"));
        return false;
    }
    if (header.channels != Channels) {
        setError(errorMessage, QStringLiteral("ACL1 channels 必须为 1。"));
        return false;
    }
    if (header.sampleFormat != Int16SampleFormat) {
        setError(errorMessage, QStringLiteral("ACL1 sampleFormat 必须为 Int16。"));
        return false;
    }
    if (header.samplesPerChannel != SamplesPerChannel) {
        setError(errorMessage, QStringLiteral("ACL1 samplesPerChannel 必须为 320。"));
        return false;
    }
    if (header.payloadSize != PcmPayloadSize) {
        setError(errorMessage, QStringLiteral("ACL1 payloadSize 必须为 640。"));
        return false;
    }

    return true;
}

} // namespace

namespace AudioPacketProtocol
{

bool validatePcmPayload(const QByteArray &pcmPayload, QString *errorMessage)
{
    if (pcmPayload.size() != PcmPayloadSize) {
        setError(errorMessage,
                 QStringLiteral("PCM payload 必须严格为 640 bytes，实际为 %1 bytes。")
                     .arg(pcmPayload.size()));
        return false;
    }

    clearError(errorMessage);
    return true;
}

QByteArray serializeAudioPacket(const AudioPacketHeader &header,
                                const QByteArray &pcmPayload,
                                QString *errorMessage)
{
    if (!validateHeader(header, errorMessage) || !validatePcmPayload(pcmPayload, errorMessage)) {
        return {};
    }

    QByteArray datagram;
    datagram.reserve(DatagramSize);
    QBuffer buffer(&datagram);
    if (!buffer.open(QIODevice::WriteOnly)) {
        setError(errorMessage, QStringLiteral("无法创建 ACL1 写入缓冲区。"));
        return {};
    }

    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << header.magic
           << header.version
           << header.packetType
           << header.headerSize
           << header.sessionId
           << header.sequence
           << header.timestampSamples
           << header.sampleRate
           << header.channels
           << header.sampleFormat
           << header.samplesPerChannel
           << header.payloadSize;
    if (stream.status() != QDataStream::Ok || datagram.size() != HeaderSize) {
        setError(errorMessage, QStringLiteral("ACL1 头序列化失败。"));
        return {};
    }

    datagram.append(pcmPayload);
    if (datagram.size() != DatagramSize) {
        setError(errorMessage, QStringLiteral("ACL1 数据报长度错误。"));
        return {};
    }

    clearError(errorMessage);
    return datagram;
}

bool parseAudioPacket(const QByteArray &datagram,
                      AudioPacket *packet,
                      QString *errorMessage)
{
    if (!packet) {
        setError(errorMessage, QStringLiteral("ACL1 输出 packet 指针为空。"));
        return false;
    }
    if (datagram.size() != DatagramSize) {
        setError(errorMessage,
                 QStringLiteral("ACL1 数据报必须严格为 672 bytes，实际为 %1 bytes。")
                     .arg(datagram.size()));
        return false;
    }

    QBuffer buffer;
    buffer.setData(datagram);
    if (!buffer.open(QIODevice::ReadOnly)) {
        setError(errorMessage, QStringLiteral("无法创建 ACL1 读取缓冲区。"));
        return false;
    }

    AudioPacket parsed;
    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::BigEndian);
    stream >> parsed.header.magic
           >> parsed.header.version
           >> parsed.header.packetType
           >> parsed.header.headerSize
           >> parsed.header.sessionId
           >> parsed.header.sequence
           >> parsed.header.timestampSamples
           >> parsed.header.sampleRate
           >> parsed.header.channels
           >> parsed.header.sampleFormat
           >> parsed.header.samplesPerChannel
           >> parsed.header.payloadSize;
    if (stream.status() != QDataStream::Ok || buffer.pos() != HeaderSize) {
        setError(errorMessage, QStringLiteral("ACL1 头解析失败或长度错误。"));
        return false;
    }
    if (!validateHeader(parsed.header, errorMessage)) {
        return false;
    }

    parsed.pcmPayload = datagram.sliced(HeaderSize);
    if (!validatePcmPayload(parsed.pcmPayload, errorMessage)) {
        return false;
    }

    *packet = std::move(parsed);
    clearError(errorMessage);
    return true;
}

} // namespace AudioPacketProtocol
