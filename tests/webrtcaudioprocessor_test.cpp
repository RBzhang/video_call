#include "webrtcaudioprocessor.h"

#include <QDebug>

#include <array>

namespace {

bool expect(bool condition, const QString &message)
{
    if (!condition) {
        qCritical().noquote() << QStringLiteral("[webrtcaudioprocessor_test]") << message;
    }
    return condition;
}

QByteArray orderedFrame()
{
    QByteArray frame(WebRtcAudioProcessor::NetworkFrameBytes, '\0');
    for (int index = 0; index < frame.size(); ++index) {
        frame[index] = static_cast<char>(index % 251);
    }
    return frame;
}

bool testSplitAndJoinPreservesOrder()
{
    const QByteArray input = orderedFrame();
    std::array<QByteArray, 2> frames;
    bool success = true;
    success &= expect(WebRtcAudioProcessor::split20MsFrame(input, &frames),
                      QStringLiteral("640-byte PCM 未能拆分。"));
    success &= expect(frames[0].size() == WebRtcAudioProcessor::ApmFrameBytes,
                      QStringLiteral("第一个 10 ms 子帧大小错误。"));
    success &= expect(frames[1].size() == WebRtcAudioProcessor::ApmFrameBytes,
                      QStringLiteral("第二个 10 ms 子帧大小错误。"));
    success &= expect(frames[0] == input.first(WebRtcAudioProcessor::ApmFrameBytes),
                      QStringLiteral("第一个 10 ms 子帧顺序错误。"));
    success &= expect(frames[1] == input.mid(WebRtcAudioProcessor::ApmFrameBytes,
                                              WebRtcAudioProcessor::ApmFrameBytes),
                      QStringLiteral("第二个 10 ms 子帧顺序错误。"));
    success &= expect(WebRtcAudioProcessor::join10MsFrames(frames) == input,
                      QStringLiteral("两个 10 ms 子帧重组后不是原始 640-byte PCM。"));
    return success;
}

bool testInvalidFrameBypassesSafely()
{
    WebRtcAudioProcessor processor;
    const QByteArray shortFrame(WebRtcAudioProcessor::NetworkFrameBytes - 1, '\x7f');
    std::array<QByteArray, 2> frames;
    bool success = true;
    success &= expect(!WebRtcAudioProcessor::split20MsFrame(shortFrame, &frames),
                      QStringLiteral("错误长度 PCM 不应拆分成功。"));
    success &= expect(processor.processRender20Ms(shortFrame) == shortFrame,
                      QStringLiteral("错误长度 render PCM 未安全旁路。"));
    success &= expect(processor.processCapture20Ms(shortFrame, 80) == shortFrame,
                      QStringLiteral("错误长度 capture PCM 未安全旁路。"));
    success &= expect(processor.statistics().bypassedFrames == 2,
                      QStringLiteral("错误长度旁路统计不正确。"));
    return success;
}

bool testLifecycleAndUninitializedBypass()
{
    WebRtcAudioProcessor processor;
    const QByteArray input = orderedFrame();
    bool success = true;
    success &= expect(processor.processCapture20Ms(input, 80) == input,
                      QStringLiteral("未初始化 APM 的 capture PCM 未旁路。"));
    const bool initialized = processor.initialize();
#if defined(VIDEO_CALL_HAVE_WEBRTC_APM)
    success &= expect(initialized,
                      QStringLiteral("已链接 WebRTC APM 时初始化不应失败。"));
    success &= expect(processor.isInitialized(),
                      QStringLiteral("已链接 WebRTC APM 时状态应为 initialized。"));

    const QByteArray renderOutput = processor.processRender20Ms(input);
    const QByteArray captureOutput = processor.processCapture20Ms(
        input, WebRtcAudioProcessor::DefaultAecStreamDelayMs);
    success &= expect(renderOutput.size() == input.size(),
                      QStringLiteral("APM render 处理后的 PCM 长度错误。"));
    success &= expect(captureOutput.size() == input.size(),
                      QStringLiteral("APM capture 处理后的 PCM 长度错误。"));
    success &= expect(processor.statistics().renderProcessFailures == 0
                          && processor.statistics().captureProcessFailures == 0,
                      QStringLiteral("APM 处理期间出现错误。"));
#else
    Q_UNUSED(initialized);
#endif
    processor.reset();
    processor.shutdown();
    processor.initialize();
    processor.shutdown();
    success &= expect(!processor.isInitialized(),
                      QStringLiteral("shutdown 后 APM 不应保持 initialized。"));
    return success;
}

} // namespace

int main()
{
    const bool success = testSplitAndJoinPreservesOrder()
        && testInvalidFrameBypassesSafely()
        && testLifecycleAndUninitializedBypass();
    return success ? 0 : 1;
}
