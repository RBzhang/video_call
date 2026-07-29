#include "audiojitterbuffer.h"

#include "audiopacketprotocol.h"

#include <limits>

namespace {

constexpr quint32 ObviousSequenceJump = 1000;

QByteArray silencePacket()
{
    return QByteArray(AudioPacketProtocol::PcmPayloadSize, '\0');
}

} // namespace

bool AudioJitterBuffer::insertPacket(quint32 sessionId,
                                     quint32 sequence,
                                     const QByteArray &pcmPayload)
{
    if (sessionId == 0 || sequence == 0
        || pcmPayload.size() != AudioPacketProtocol::PcmPayloadSize) {
        return false;
    }

    if (m_sessionId == 0 || m_sessionId != sessionId) {
        resetForSession(sessionId, sequence, m_sessionId != 0);
    } else if (m_playing) {
        if (sequenceLess(sequence, m_nextSequence)) {
            ++m_statistics.latePackets;
            return false;
        }
        if (forwardDistance(m_nextSequence, sequence) > ObviousSequenceJump) {
            resetForSession(sessionId, sequence, true);
        }
    } else if (sequenceLess(sequence, m_nextSequence)) {
        m_nextSequence = sequence;
    }

    if (m_packets.contains(sequence)) {
        ++m_statistics.duplicatePackets;
        return false;
    }

    if (m_packets.size() >= MaximumBufferedPackets) {
        quint32 farthestSequence = 0;
        quint32 farthestDistance = 0;
        bool hasFarthest = false;
        for (auto iterator = m_packets.cbegin(); iterator != m_packets.cend(); ++iterator) {
            const quint32 distance = forwardDistance(m_nextSequence, iterator.key());
            if (!hasFarthest || distance > farthestDistance) {
                hasFarthest = true;
                farthestDistance = distance;
                farthestSequence = iterator.key();
            }
        }

        const quint32 newDistance = forwardDistance(m_nextSequence, sequence);
        ++m_statistics.bufferOverflowPackets;
        if (hasFarthest && newDistance < farthestDistance) {
            m_packets.remove(farthestSequence);
        } else {
            updateCurrentBufferedStatistic();
            return false;
        }
    }

    m_packets.insert(sequence, pcmPayload);
    ++m_statistics.insertedPackets;
    updateCurrentBufferedStatistic();
    return true;
}

QByteArray AudioJitterBuffer::takeNextPacket()
{
    if (!m_playing) {
        if (m_packets.size() < PrebufferPackets) {
            updateCurrentBufferedStatistic();
            return {};
        }
        m_playing = true;
        m_consecutiveMissingPackets = 0;
    }

    const auto iterator = m_packets.constFind(m_nextSequence);
    if (iterator != m_packets.cend()) {
        const QByteArray packet = iterator.value();
        m_packets.remove(m_nextSequence);
        m_nextSequence = incrementSequence(m_nextSequence);
        m_consecutiveMissingPackets = 0;
        updateCurrentBufferedStatistic();
        return packet;
    }

    ++m_statistics.concealedPackets;
    ++m_consecutiveMissingPackets;
    m_nextSequence = incrementSequence(m_nextSequence);
    if (m_consecutiveMissingPackets >= ConsecutiveMissingRebufferThreshold) {
        m_playing = false;
        m_consecutiveMissingPackets = 0;
        m_packets.clear();
    }
    updateCurrentBufferedStatistic();
    return silencePacket();
}

void AudioJitterBuffer::clear()
{
    m_sessionId = 0;
    m_nextSequence = 0;
    m_playing = false;
    m_consecutiveMissingPackets = 0;
    m_packets.clear();
    updateCurrentBufferedStatistic();
}

void AudioJitterBuffer::resetStatistics()
{
    m_statistics = {};
    updateCurrentBufferedStatistic();
}

bool AudioJitterBuffer::isPlaying() const
{
    return m_playing;
}

int AudioJitterBuffer::currentBufferedPackets() const
{
    return m_packets.size();
}

quint32 AudioJitterBuffer::sessionId() const
{
    return m_sessionId;
}

AudioJitterBuffer::Statistics AudioJitterBuffer::statistics() const
{
    Statistics result = m_statistics;
    result.currentBufferedPackets = currentBufferedPackets();
    return result;
}

bool AudioJitterBuffer::sequenceLess(quint32 left, quint32 right)
{
    return left != right && static_cast<qint32>(left - right) < 0;
}

quint32 AudioJitterBuffer::incrementSequence(quint32 sequence)
{
    ++sequence;
    return sequence == 0 ? 1 : sequence;
}

quint32 AudioJitterBuffer::forwardDistance(quint32 from, quint32 to)
{
    return to - from;
}

void AudioJitterBuffer::resetForSession(quint32 sessionId,
                                        quint32 firstSequence,
                                        bool countReset)
{
    if (countReset) {
        ++m_statistics.sessionResets;
    }
    m_sessionId = sessionId;
    m_nextSequence = firstSequence;
    m_playing = false;
    m_consecutiveMissingPackets = 0;
    m_packets.clear();
    updateCurrentBufferedStatistic();
}

void AudioJitterBuffer::updateCurrentBufferedStatistic()
{
    m_statistics.currentBufferedPackets = m_packets.size();
}
