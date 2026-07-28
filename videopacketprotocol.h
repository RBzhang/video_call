#ifndef VIDEOPACKETPROTOCOL_H
#define VIDEOPACKETPROTOCOL_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace VideoPacketProtocol
{

constexpr quint32 Magic = 0x56434C31;
constexpr quint8 Version = 1;
constexpr quint8 VideoFragmentPacketType = 1;

constexpr qsizetype HeaderSize = 32;
constexpr qsizetype MaximumDatagramSize = 1200;
constexpr qsizetype MaximumPayloadSize = MaximumDatagramSize - HeaderSize;
constexpr qsizetype MaximumFrameSize = 4 * 1024 * 1024;
constexpr quint16 MaximumFragmentCount = 4096;

static_assert(HeaderSize == 32);
static_assert(MaximumPayloadSize > 0);

struct VideoFragmentHeader
{
    quint32 sessionId = 0;
    quint32 frameId = 0;
    quint32 timestampMs = 0;
    quint32 frameSize = 0;

    quint16 fragmentIndex = 0;
    quint16 fragmentCount = 0;
    quint16 payloadSize = 0;
    quint16 flags = 0;
};

struct VideoFragmentPacket
{
    VideoFragmentHeader header;
    QByteArray payload;
};

QByteArray serializeVideoFragment(const VideoFragmentHeader &header,
                                  const QByteArray &payload,
                                  QString *errorMessage = nullptr);

bool parseVideoFragment(const QByteArray &datagram,
                        VideoFragmentPacket *packet,
                        QString *errorMessage = nullptr);

QVector<QByteArray> fragmentEncodedFrame(const QByteArray &encodedFrame,
                                         quint32 sessionId,
                                         quint32 frameId,
                                         quint32 timestampMs,
                                         QString *errorMessage = nullptr);

} // namespace VideoPacketProtocol

#endif // VIDEOPACKETPROTOCOL_H
