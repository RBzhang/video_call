#include "videoframereassembler.h"

#include "videopacketprotocol.h"

#include <QAbstractSocket>

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

} // namespace

bool VideoFrameReassembler::FrameKey::operator==(const FrameKey &other) const
{
    return senderIpv4 == other.senderIpv4
        && senderPort == other.senderPort
        && sessionId == other.sessionId
        && frameId == other.frameId;
}

VideoFrameReassembler::AddResult VideoFrameReassembler::addDatagram(
    const QByteArray &datagram,
    const QHostAddress &senderAddress,
    quint16 senderPort,
    qint64 currentTimeMs,
    CompletedFrame *completedFrame,
    QString *errorMessage)
{
    VideoPacketProtocol::VideoFragmentPacket packet;
    QString parseError;
    if (!VideoPacketProtocol::parseVideoFragment(datagram, &packet, &parseError)) {
        setError(errorMessage, parseError);
        return AddResult::Rejected;
    }
    if (senderAddress.protocol() != QAbstractSocket::IPv4Protocol) {
        setError(errorMessage, QStringLiteral("仅支持 IPv4 发送方地址。"));
        return AddResult::Rejected;
    }

    removeExpiredFrames(currentTimeMs);

    const FrameKey key {
        senderAddress.toIPv4Address(),
        senderPort,
        packet.header.sessionId,
        packet.header.frameId
    };

    auto iterator = m_pendingFrames.find(key);
    if (iterator != m_pendingFrames.end()) {
        PendingFrame &pending = iterator.value();
        if (pending.frameSize != packet.header.frameSize
            || pending.fragmentCount != packet.header.fragmentCount
            || pending.timestampMs != packet.header.timestampMs) {
            removePendingFrame(iterator);
            setError(errorMessage,
                     QStringLiteral("同一帧的分片元数据不一致，已丢弃该帧。"));
            return AddResult::Rejected;
        }

        const qsizetype fragmentIndex = packet.header.fragmentIndex;
        if (pending.receivedFragments.testBit(fragmentIndex)) {
            if (pending.fragments.at(fragmentIndex) == packet.payload) {
                pending.lastUpdatedAtMs = currentTimeMs;
                clearError(errorMessage);
                return AddResult::Duplicate;
            }

            removePendingFrame(iterator);
            setError(errorMessage,
                     QStringLiteral("收到内容冲突的重复分片，已丢弃该帧。"));
            return AddResult::Rejected;
        }

        const qsizetype frameSize = pending.frameSize;
        if (pending.receivedBytes > frameSize - packet.payload.size()) {
            removePendingFrame(iterator);
            setError(errorMessage,
                     QStringLiteral("分片累计长度超过声明帧大小，已丢弃该帧。"));
            return AddResult::Rejected;
        }

        if (!makeRoomForPayload(packet.payload.size(), &key)) {
            setError(errorMessage, QStringLiteral("接收缓存空间不足，已拒绝该分片。"));
            return AddResult::Rejected;
        }
        iterator = m_pendingFrames.find(key);
        if (iterator == m_pendingFrames.end()) {
            setError(errorMessage, QStringLiteral("接收缓存已清理该帧。"));
            return AddResult::Rejected;
        }
    } else {
        while (m_pendingFrames.size() >= MaximumPendingFrameCount) {
            if (!removeOldestPendingFrame()) {
                setError(errorMessage, QStringLiteral("接收缓存帧数已达到上限。"));
                return AddResult::Rejected;
            }
        }
        if (!makeRoomForPayload(packet.payload.size())) {
            setError(errorMessage, QStringLiteral("接收缓存空间不足，已拒绝该分片。"));
            return AddResult::Rejected;
        }

        PendingFrame pending;
        pending.frameSize = packet.header.frameSize;
        pending.fragmentCount = packet.header.fragmentCount;
        pending.timestampMs = packet.header.timestampMs;
        pending.fragments.resize(packet.header.fragmentCount);
        pending.receivedFragments = QBitArray(packet.header.fragmentCount, false);
        pending.createdAtMs = currentTimeMs;
        pending.lastUpdatedAtMs = currentTimeMs;
        iterator = m_pendingFrames.insert(key, pending);
    }

    PendingFrame &pending = iterator.value();
    const qsizetype fragmentIndex = packet.header.fragmentIndex;
    pending.fragments[fragmentIndex] = packet.payload;
    pending.receivedFragments.setBit(fragmentIndex, true);
    ++pending.receivedCount;
    pending.receivedBytes += packet.payload.size();
    pending.lastUpdatedAtMs = currentTimeMs;
    m_pendingPayloadBytes += packet.payload.size();

    if (pending.receivedCount != pending.fragmentCount) {
        clearError(errorMessage);
        return AddResult::Accepted;
    }

    QByteArray encodedFrame;
    encodedFrame.reserve(pending.frameSize);
    for (qsizetype index = 0; index < pending.fragmentCount; ++index) {
        if (!pending.receivedFragments.testBit(index)
            || pending.fragments.at(index).isEmpty()) {
            removePendingFrame(iterator);
            setError(errorMessage, QStringLiteral("完整帧缺少分片，已丢弃该帧。"));
            return AddResult::Rejected;
        }
        const QByteArray &fragment = pending.fragments.at(index);
        if (encodedFrame.size() > pending.frameSize - fragment.size()) {
            removePendingFrame(iterator);
            setError(errorMessage, QStringLiteral("重组帧长度超过声明大小，已丢弃该帧。"));
            return AddResult::Rejected;
        }
        encodedFrame.append(fragment);
    }

    if (pending.receivedBytes != pending.frameSize
        || encodedFrame.size() != pending.frameSize) {
        removePendingFrame(iterator);
        setError(errorMessage, QStringLiteral("重组帧大小与声明大小不一致，已丢弃该帧。"));
        return AddResult::Rejected;
    }

    CompletedFrame result;
    result.encodedFrame = encodedFrame;
    result.senderAddress = senderAddress;
    result.senderPort = senderPort;
    result.sessionId = packet.header.sessionId;
    result.frameId = packet.header.frameId;
    result.timestampMs = packet.header.timestampMs;

    removePendingFrame(iterator);
    if (completedFrame) {
        *completedFrame = result;
    }
    clearError(errorMessage);
    return AddResult::Completed;
}

int VideoFrameReassembler::removeExpiredFrames(qint64 currentTimeMs)
{
    int removedCount = 0;
    auto iterator = m_pendingFrames.begin();
    while (iterator != m_pendingFrames.end()) {
        const qint64 ageMs = currentTimeMs - iterator.value().lastUpdatedAtMs;
        if (ageMs > FrameTimeoutMs) {
            const auto expired = iterator++;
            removePendingFrame(expired);
            ++removedCount;
        } else {
            ++iterator;
        }
    }
    return removedCount;
}

void VideoFrameReassembler::clear()
{
    m_pendingFrames.clear();
    m_pendingPayloadBytes = 0;
}

qsizetype VideoFrameReassembler::pendingFrameCount() const
{
    return m_pendingFrames.size();
}

qsizetype VideoFrameReassembler::pendingPayloadBytes() const
{
    return m_pendingPayloadBytes;
}

bool VideoFrameReassembler::frameKeyLess(const FrameKey &left, const FrameKey &right)
{
    if (left.senderIpv4 != right.senderIpv4) {
        return left.senderIpv4 < right.senderIpv4;
    }
    if (left.senderPort != right.senderPort) {
        return left.senderPort < right.senderPort;
    }
    if (left.sessionId != right.sessionId) {
        return left.sessionId < right.sessionId;
    }
    return left.frameId < right.frameId;
}

void VideoFrameReassembler::removePendingFrame(PendingFrames::iterator iterator)
{
    const qsizetype receivedBytes = iterator.value().receivedBytes;
    if (receivedBytes > m_pendingPayloadBytes) {
        m_pendingPayloadBytes = 0;
    } else {
        m_pendingPayloadBytes -= receivedBytes;
    }
    m_pendingFrames.erase(iterator);
}

bool VideoFrameReassembler::removeOldestPendingFrame(const FrameKey *protectedKey)
{
    auto oldest = m_pendingFrames.end();
    for (auto iterator = m_pendingFrames.begin(); iterator != m_pendingFrames.end(); ++iterator) {
        if (protectedKey && iterator.key() == *protectedKey) {
            continue;
        }
        if (oldest == m_pendingFrames.end()
            || iterator.value().lastUpdatedAtMs < oldest.value().lastUpdatedAtMs
            || (iterator.value().lastUpdatedAtMs == oldest.value().lastUpdatedAtMs
                && (iterator.value().createdAtMs < oldest.value().createdAtMs
                    || (iterator.value().createdAtMs == oldest.value().createdAtMs
                        && frameKeyLess(iterator.key(), oldest.key()))))) {
            oldest = iterator;
        }
    }

    if (oldest == m_pendingFrames.end()) {
        return false;
    }
    removePendingFrame(oldest);
    return true;
}

bool VideoFrameReassembler::makeRoomForPayload(qsizetype requiredBytes,
                                                const FrameKey *protectedKey)
{
    if (requiredBytes < 0 || requiredBytes > MaximumPendingPayloadBytes) {
        return false;
    }

    while (m_pendingPayloadBytes > MaximumPendingPayloadBytes - requiredBytes) {
        if (!removeOldestPendingFrame(protectedKey)) {
            return false;
        }
    }
    return true;
}
