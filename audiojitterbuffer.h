#ifndef AUDIOJITTERBUFFER_H
#define AUDIOJITTERBUFFER_H

#include <QByteArray>
#include <QMap>
#include <QtGlobal>

class AudioJitterBuffer
{
public:
    static constexpr int PrebufferPackets = 3;
    static constexpr int MaximumBufferedPackets = 10;
    static constexpr int ConsecutiveMissingRebufferThreshold = 5;

    struct Statistics
    {
        quint64 insertedPackets = 0;
        quint64 duplicatePackets = 0;
        quint64 latePackets = 0;
        quint64 concealedPackets = 0;
        quint64 bufferOverflowPackets = 0;
        quint64 sessionResets = 0;
        int currentBufferedPackets = 0;
    };

    bool insertPacket(quint32 sessionId, quint32 sequence, const QByteArray &pcmPayload);
    QByteArray takeNextPacket();
    void clear();
    void resetStatistics();

    bool isPlaying() const;
    int currentBufferedPackets() const;
    quint32 sessionId() const;
    Statistics statistics() const;

private:
    static bool sequenceLess(quint32 left, quint32 right);
    static quint32 incrementSequence(quint32 sequence);
    static quint32 forwardDistance(quint32 from, quint32 to);
    void resetForSession(quint32 sessionId, quint32 firstSequence, bool countReset);
    void updateCurrentBufferedStatistic();

    quint32 m_sessionId = 0;
    quint32 m_nextSequence = 0;
    bool m_playing = false;
    int m_consecutiveMissingPackets = 0;
    QMap<quint32, QByteArray> m_packets;
    Statistics m_statistics;
};

#endif // AUDIOJITTERBUFFER_H
