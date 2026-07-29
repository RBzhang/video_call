#ifndef AUDIOPACKETPROTOCOL_H
#define AUDIOPACKETPROTOCOL_H

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace AudioPacketProtocol
{

constexpr quint32 Magic = 0x41434c31U; // ACL1
constexpr quint8 Version = 1;
constexpr quint8 PcmPacketType = 1;
constexpr quint16 HeaderSize = 32;
constexpr quint32 SampleRate = 16000;
constexpr quint16 Channels = 1;
constexpr quint16 Int16SampleFormat = 1;
constexpr quint16 SamplesPerChannel = 320;
constexpr qsizetype PcmPayloadSize = 640;
constexpr qsizetype DatagramSize = HeaderSize + PcmPayloadSize;

struct AudioPacketHeader
{
    quint32 magic = Magic;
    quint8 version = Version;
    quint8 packetType = PcmPacketType;
    quint16 headerSize = HeaderSize;
    quint32 sessionId = 0;
    quint32 sequence = 0;
    quint32 timestampSamples = 0;
    quint32 sampleRate = SampleRate;
    quint16 channels = Channels;
    quint16 sampleFormat = Int16SampleFormat;
    quint16 samplesPerChannel = SamplesPerChannel;
    quint16 payloadSize = PcmPayloadSize;
};

struct AudioPacket
{
    AudioPacketHeader header;
    QByteArray pcmPayload;
};

bool validatePcmPayload(const QByteArray &pcmPayload, QString *errorMessage = nullptr);
QByteArray serializeAudioPacket(const AudioPacketHeader &header,
                                const QByteArray &pcmPayload,
                                QString *errorMessage = nullptr);
bool parseAudioPacket(const QByteArray &datagram,
                      AudioPacket *packet,
                      QString *errorMessage = nullptr);

} // namespace AudioPacketProtocol

#endif // AUDIOPACKETPROTOCOL_H
