#include "jpegframeencoder.h"

#include "videopacketprotocol.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <exception>
#include <vector>

namespace {

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

namespace JpegFrameEncoder
{

QByteArray encodeBgrFrame(const cv::Mat &bgrFrame,
                          int jpegQuality,
                          QString *errorMessage)
{
    if (bgrFrame.empty() || bgrFrame.cols <= 0 || bgrFrame.rows <= 0) {
        setError(errorMessage, QStringLiteral("JPEG 编码输入图像为空或尺寸无效。"));
        return {};
    }
    if (bgrFrame.channels() != 1 && bgrFrame.channels() != 3 && bgrFrame.channels() != 4) {
        setError(errorMessage, QStringLiteral("JPEG 编码仅支持灰度、BGR 或 BGRA 图像。"));
        return {};
    }
    if (jpegQuality < 1 || jpegQuality > 100) {
        setError(errorMessage, QStringLiteral("JPEG quality 必须在 1～100 范围内。"));
        return {};
    }

    try {
        cv::Mat jpegInput;
        if (bgrFrame.channels() == 4) {
            cv::cvtColor(bgrFrame, jpegInput, cv::COLOR_BGRA2BGR);
        } else {
            jpegInput = bgrFrame;
        }

        std::vector<uchar> encoded;
        const std::vector<int> parameters {
            cv::IMWRITE_JPEG_QUALITY,
            jpegQuality
        };
        if (!cv::imencode(".jpg", jpegInput, encoded, parameters) || encoded.empty()) {
            setError(errorMessage, QStringLiteral("OpenCV JPEG 编码失败或输出为空。"));
            return {};
        }
        if (encoded.size() > static_cast<size_t>(VideoPacketProtocol::MaximumFrameSize)) {
            setError(errorMessage, QStringLiteral("JPEG 编码结果超过 VCL1 单帧 4 MiB 上限。"));
            return {};
        }

        const QByteArray jpegData(reinterpret_cast<const char *>(encoded.data()),
                                  static_cast<qsizetype>(encoded.size()));
        if (jpegData.isEmpty()) {
            setError(errorMessage, QStringLiteral("JPEG 编码结果复制失败。"));
            return {};
        }

        clearError(errorMessage);
        return jpegData;
    } catch (const cv::Exception &exception) {
        setError(errorMessage,
                 QStringLiteral("OpenCV JPEG 编码异常：%1")
                     .arg(QString::fromLocal8Bit(exception.what())));
    } catch (const std::exception &exception) {
        setError(errorMessage,
                 QStringLiteral("JPEG 编码标准异常：%1")
                     .arg(QString::fromLocal8Bit(exception.what())));
    }
    return {};
}

} // namespace JpegFrameEncoder
