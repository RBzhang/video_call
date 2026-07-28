#include "videopacketprotocol.h"

#include <QBuffer>
#include <QDataStream>
#include <QIODevice>

#include <limits>

namespace VideoPacketProtocol
{
namespace
{

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

bool validateHeaderAndPayload(const VideoFragmentHeader &header,
                              qsizetype payloadLength,
                              QString *errorMessage)
{
    if (header.frameSize == 0) {
        setError(errorMessage, QStringLiteral("帧大小必须大于零。"));
        return false;
    }
    if (static_cast<quint64>(header.frameSize)
        > static_cast<quint64>(MaximumFrameSize)) {
        setError(errorMessage, QStringLiteral("帧大小超过协议上限。"));
        return false;
    }
    if (header.fragmentCount == 0 || header.fragmentCount > MaximumFragmentCount) {
        setError(errorMessage, QStringLiteral("分片数量无效。"));
        return false;
    }
    if (header.fragmentIndex >= header.fragmentCount) {
        setError(errorMessage, QStringLiteral("分片索引无效。"));
        return false;
    }
    if (header.payloadSize == 0
        || static_cast<qsizetype>(header.payloadSize) > MaximumPayloadSize) {
        setError(errorMessage, QStringLiteral("分片负载大小无效。"));
        return false;
    }
    if (payloadLength != static_cast<qsizetype>(header.payloadSize)) {
        setError(errorMessage, QStringLiteral("分片负载长度与头字段不一致。"));
        return false;
    }
    if (header.frameSize < header.payloadSize) {
        setError(errorMessage, QStringLiteral("帧大小小于分片负载大小。"));
        return false;
    }

    return true;
}

} // namespace

QByteArray serializeVideoFragment(const VideoFragmentHeader &header,
                                  const QByteArray &payload,
                                  QString *errorMessage)
{
    if (!validateHeaderAndPayload(header, payload.size(), errorMessage)) {
        return {};
    }

    const qsizetype expectedSize = HeaderSize + static_cast<qsizetype>(header.payloadSize);
    if (expectedSize > MaximumDatagramSize) {
        setError(errorMessage, QStringLiteral("数据报大小超过协议上限。"));
        return {};
    }

    QByteArray datagram;
    datagram.reserve(expectedSize);
    QBuffer buffer(&datagram);
    if (!buffer.open(QIODevice::WriteOnly)) {
        setError(errorMessage, QStringLiteral("无法创建协议数据报。"));
        return {};
    }

    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << Magic
           << Version
           << VideoFragmentPacketType
           << static_cast<quint16>(HeaderSize)
           << header.sessionId
           << header.frameId
           << header.timestampMs
           << header.frameSize
           << header.fragmentIndex
           << header.fragmentCount
           << header.payloadSize
           << header.flags;

    if (stream.status() != QDataStream::Ok
        || buffer.write(payload) != payload.size()
        || datagram.size() != expectedSize) {
        setError(errorMessage, QStringLiteral("协议数据报序列化失败。"));
        return {};
    }

    clearError(errorMessage);
    return datagram;
}

bool parseVideoFragment(const QByteArray &datagram,
                        VideoFragmentPacket *packet,
                        QString *errorMessage)
{
    if (!packet) {
        setError(errorMessage, QStringLiteral("输出数据包指针不能为空。"));
        return false;
    }
    if (datagram.size() < HeaderSize) {
        setError(errorMessage, QStringLiteral("数据报长度小于协议头。"));
        return false;
    }
    if (datagram.size() > MaximumDatagramSize) {
        setError(errorMessage, QStringLiteral("数据报大小超过协议上限。"));
        return false;
    }

    QBuffer buffer;
    buffer.setData(datagram);
    if (!buffer.open(QIODevice::ReadOnly)) {
        setError(errorMessage, QStringLiteral("无法读取协议数据报。"));
        return false;
    }

    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::BigEndian);

    quint32 magic = 0;
    quint8 version = 0;
    quint8 packetType = 0;
    quint16 headerSize = 0;
    VideoFragmentHeader header;
    stream >> magic
           >> version
           >> packetType
           >> headerSize
           >> header.sessionId
           >> header.frameId
           >> header.timestampMs
           >> header.frameSize
           >> header.fragmentIndex
           >> header.fragmentCount
           >> header.payloadSize
           >> header.flags;

    if (stream.status() != QDataStream::Ok) {
        setError(errorMessage, QStringLiteral("协议头读取失败。"));
        return false;
    }
    if (magic != Magic) {
        setError(errorMessage, QStringLiteral("协议魔数无效。"));
        return false;
    }
    if (version != Version) {
        setError(errorMessage, QStringLiteral("协议版本不受支持。"));
        return false;
    }
    if (packetType != VideoFragmentPacketType) {
        setError(errorMessage, QStringLiteral("数据包类型无效。"));
        return false;
    }
    if (headerSize != HeaderSize) {
        setError(errorMessage, QStringLiteral("协议头大小无效。"));
        return false;
    }
    if (!validateHeaderAndPayload(header,
                                  static_cast<qsizetype>(header.payloadSize),
                                  errorMessage)) {
        return false;
    }

    const qsizetype expectedSize = HeaderSize + static_cast<qsizetype>(header.payloadSize);
    if (datagram.size() != expectedSize) {
        setError(errorMessage, QStringLiteral("数据报长度与分片负载大小不一致。"));
        return false;
    }

    VideoFragmentPacket parsedPacket;
    parsedPacket.header = header;
    parsedPacket.payload = datagram.mid(HeaderSize, header.payloadSize);
    if (parsedPacket.payload.size() != static_cast<qsizetype>(header.payloadSize)) {
        setError(errorMessage, QStringLiteral("分片负载读取失败。"));
        return false;
    }

    *packet = parsedPacket;
    clearError(errorMessage);
    return true;
}

QVector<QByteArray> fragmentEncodedFrame(const QByteArray &encodedFrame,
                                         quint32 sessionId,
                                         quint32 frameId,
                                         quint32 timestampMs,
                                         QString *errorMessage)
{
    const qsizetype frameSize = encodedFrame.size();
    if (frameSize <= 0) {
        setError(errorMessage, QStringLiteral("编码帧不能为空。"));
        return {};
    }
    if (frameSize > MaximumFrameSize) {
        setError(errorMessage, QStringLiteral("编码帧大小超过协议上限。"));
        return {};
    }
    if (static_cast<quint64>(frameSize)
        > static_cast<quint64>(std::numeric_limits<quint32>::max())) {
        setError(errorMessage, QStringLiteral("编码帧大小无法表示。"));
        return {};
    }

    const qsizetype fragmentCountSize =
        (frameSize + MaximumPayloadSize - 1) / MaximumPayloadSize;
    if (fragmentCountSize <= 0
        || fragmentCountSize > MaximumFragmentCount
        || static_cast<quint64>(fragmentCountSize)
            > static_cast<quint64>(std::numeric_limits<quint16>::max())) {
        setError(errorMessage, QStringLiteral("编码帧的分片数量无效。"));
        return {};
    }

    const quint16 fragmentCount = static_cast<quint16>(fragmentCountSize);
    const quint32 frameSizeValue = static_cast<quint32>(frameSize);
    QVector<QByteArray> datagrams;
    datagrams.reserve(fragmentCountSize);

    qsizetype offset = 0;
    for (qsizetype index = 0; index < fragmentCountSize; ++index) {
        const qsizetype payloadLength = qMin(MaximumPayloadSize, frameSize - offset);
        if (payloadLength <= 0
            || static_cast<quint64>(payloadLength)
                > static_cast<quint64>(std::numeric_limits<quint16>::max())) {
            setError(errorMessage, QStringLiteral("分片负载大小无法表示。"));
            return {};
        }

        VideoFragmentHeader header;
        header.sessionId = sessionId;
        header.frameId = frameId;
        header.timestampMs = timestampMs;
        header.frameSize = frameSizeValue;
        header.fragmentIndex = static_cast<quint16>(index);
        header.fragmentCount = fragmentCount;
        header.payloadSize = static_cast<quint16>(payloadLength);
        header.flags = 0;

        const QByteArray payload = encodedFrame.mid(offset, payloadLength);
        QString serializationError;
        const QByteArray datagram = serializeVideoFragment(header, payload, &serializationError);
        if (datagram.isEmpty()) {
            setError(errorMessage, serializationError);
            return {};
        }

        datagrams.append(datagram);
        offset += payloadLength;
    }

    if (offset != frameSize) {
        setError(errorMessage, QStringLiteral("编码帧分片长度不一致。"));
        return {};
    }

    clearError(errorMessage);
    return datagrams;
}

} // namespace VideoPacketProtocol
