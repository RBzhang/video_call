#ifndef VIDEOFRAMEREASSEMBLER_H
#define VIDEOFRAMEREASSEMBLER_H

#include <QBitArray>
#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QString>
#include <QVector>
#include <QtGlobal>

class VideoFrameReassembler
{
public:
    static constexpr qsizetype MaximumPendingFrameCount = 16;
    static constexpr qsizetype MaximumPendingPayloadBytes = 16 * 1024 * 1024;
    static constexpr qint64 FrameTimeoutMs = 500;

    struct CompletedFrame
    {
        QByteArray encodedFrame;

        QHostAddress senderAddress;
        quint16 senderPort = 0;

        quint32 sessionId = 0;
        quint32 frameId = 0;
        quint32 timestampMs = 0;
    };

    enum class AddResult
    {
        Rejected,
        Accepted,
        Duplicate,
        Completed
    };

    AddResult addDatagram(const QByteArray &datagram,
                          const QHostAddress &senderAddress,
                          quint16 senderPort,
                          qint64 currentTimeMs,
                          CompletedFrame *completedFrame,
                          QString *errorMessage = nullptr);

    int removeExpiredFrames(qint64 currentTimeMs);
    void clear();

    qsizetype pendingFrameCount() const;
    qsizetype pendingPayloadBytes() const;

private:
    struct FrameKey
    {
        quint32 senderIpv4 = 0;
        quint16 senderPort = 0;
        quint32 sessionId = 0;
        quint32 frameId = 0;

        bool operator==(const FrameKey &other) const;

        friend size_t qHash(const FrameKey &key, size_t seed = 0) noexcept
        {
            size_t hash = seed;
            hash ^= static_cast<size_t>(key.senderIpv4) * static_cast<size_t>(0x9e3779b1U);
            hash ^= static_cast<size_t>(key.senderPort) * static_cast<size_t>(0x85ebca6bU);
            hash ^= static_cast<size_t>(key.sessionId) * static_cast<size_t>(0xc2b2ae35U);
            hash ^= static_cast<size_t>(key.frameId) * static_cast<size_t>(0x27d4eb2fU);
            return hash;
        }
    };

    struct PendingFrame
    {
        quint32 frameSize = 0;
        quint16 fragmentCount = 0;
        quint32 timestampMs = 0;

        QVector<QByteArray> fragments;
        QBitArray receivedFragments;

        quint16 receivedCount = 0;
        qsizetype receivedBytes = 0;

        qint64 createdAtMs = 0;
        qint64 lastUpdatedAtMs = 0;
    };

    using PendingFrames = QHash<FrameKey, PendingFrame>;

    static bool frameKeyLess(const FrameKey &left, const FrameKey &right);
    void removePendingFrame(PendingFrames::iterator iterator);
    bool removeOldestPendingFrame(const FrameKey *protectedKey = nullptr);
    bool makeRoomForPayload(qsizetype requiredBytes, const FrameKey *protectedKey = nullptr);

    PendingFrames m_pendingFrames;
    qsizetype m_pendingPayloadBytes = 0;
};

#endif // VIDEOFRAMEREASSEMBLER_H
