#ifndef AUDIOLOOPPOLICY_H
#define AUDIOLOOPPOLICY_H

#include <QHostAddress>
#include <QtGlobal>

// A matching ACL1 session id alone cannot identify a software loopback: an
// FPGA UDP reflector legitimately returns the sender's packet unchanged.  It
// is a self-loop only when the UDP datagram also originated from the exact
// endpoint to which this AudioWorker is bound.
namespace AudioLoopPolicy {

inline bool isSoftwareSelfLoopback(const QHostAddress &senderAddress,
                                   quint16 senderPort,
                                   const QHostAddress &localAddress,
                                   quint16 localPort)
{
    return localPort != 0 && senderPort == localPort && senderAddress == localAddress;
}

} // namespace AudioLoopPolicy

#endif // AUDIOLOOPPOLICY_H
