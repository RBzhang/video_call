#include "audiolooppolicy.h"

#include <QCoreApplication>
#include <QDebug>

namespace {

bool expect(bool condition, const QString &message)
{
    if (!condition) {
        qCritical().noquote() << QStringLiteral("[audio_loop_policy_test]") << message;
    }
    return condition;
}

bool testSoftwareLoopbackPolicy()
{
    const QHostAddress localHost = QHostAddress::LocalHost;
    const QHostAddress localEthernet(QStringLiteral("192.168.10.50"));
    const QHostAddress fpga(QStringLiteral("192.168.10.10"));

    return expect(AudioLoopPolicy::isSoftwareSelfLoopback(localHost,
                                                           5002,
                                                           localHost,
                                                           5002),
                  QStringLiteral("同一本地 UDP 端点未识别为软件回环。"))
        && expect(!AudioLoopPolicy::isSoftwareSelfLoopback(localHost,
                                                            5003,
                                                            localHost,
                                                            5002),
                   QStringLiteral("不同本地端口被错误识别为软件回环。"))
        && expect(!AudioLoopPolicy::isSoftwareSelfLoopback(fpga,
                                                            5002,
                                                            localEthernet,
                                                            5002),
                   QStringLiteral("FPGA 回送端点被错误识别为软件回环。"))
        && expect(!AudioLoopPolicy::isSoftwareSelfLoopback(localEthernet,
                                                            5002,
                                                            localEthernet,
                                                            0),
                   QStringLiteral("未绑定端口被错误识别为软件回环。"));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    return testSoftwareLoopbackPolicy() ? 0 : 1;
}
