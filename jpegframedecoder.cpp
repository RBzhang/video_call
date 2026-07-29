#include "jpegframedecoder.h"

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

bool hasJpegMarkers(const QByteArray &jpegData)
{
    return jpegData.size() >= 4
        && static_cast<uchar>(jpegData.at(0)) == 0xff
        && static_cast<uchar>(jpegData.at(1)) == 0xd8
        && static_cast<uchar>(jpegData.at(jpegData.size() - 2)) == 0xff
        && static_cast<uchar>(jpegData.at(jpegData.size() - 1)) == 0xd9;
}

} // namespace

namespace JpegFrameDecoder
{

QImage decodeJpegFrame(const QByteArray &jpegData, QString *errorMessage)
{
    if (jpegData.isEmpty()) {
        setError(errorMessage, QStringLiteral("JPEG 解码输入为空。"));
        return {};
    }
    if (jpegData.size() > VideoPacketProtocol::MaximumFrameSize) {
        setError(errorMessage, QStringLiteral("JPEG 解码输入超过 VCL1 单帧 4 MiB 上限。"));
        return {};
    }
    if (!hasJpegMarkers(jpegData)) {
        setError(errorMessage, QStringLiteral("JPEG 缺少有效的 SOI 或 EOI 标记。"));
        return {};
    }

    try {
        const std::vector<uchar> encoded(jpegData.cbegin(), jpegData.cend());
        const cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
        if (decoded.empty() || decoded.cols <= 0 || decoded.rows <= 0) {
            setError(errorMessage, QStringLiteral("OpenCV JPEG 解码失败或图像尺寸无效。"));
            return {};
        }
        if (decoded.depth() != CV_8U) {
            setError(errorMessage, QStringLiteral("JPEG 解码结果不是 8 位图像。"));
            return {};
        }

        cv::Mat converted;
        const cv::Mat *imageFrame = &decoded;
        QImage::Format imageFormat = QImage::Format_Invalid;
        switch (decoded.channels()) {
        case 1:
            imageFormat = QImage::Format_Grayscale8;
            break;
        case 3:
            cv::cvtColor(decoded, converted, cv::COLOR_BGR2RGB);
            imageFrame = &converted;
            imageFormat = QImage::Format_RGB888;
            break;
        case 4:
            cv::cvtColor(decoded, converted, cv::COLOR_BGRA2RGBA);
            imageFrame = &converted;
            imageFormat = QImage::Format_RGBA8888;
            break;
        default:
            setError(errorMessage,
                     QStringLiteral("JPEG 解码得到不支持的 %1 通道图像。")
                         .arg(decoded.channels()));
            return {};
        }

        const QImage image(imageFrame->data,
                           imageFrame->cols,
                           imageFrame->rows,
                           static_cast<qsizetype>(imageFrame->step),
                           imageFormat);
        if (image.isNull()) {
            setError(errorMessage, QStringLiteral("无法从 JPEG 解码结果创建 QImage。"));
            return {};
        }

        const QImage copiedImage = image.copy();
        if (copiedImage.isNull()) {
            setError(errorMessage, QStringLiteral("JPEG 解码结果深拷贝失败。"));
            return {};
        }

        clearError(errorMessage);
        return copiedImage;
    } catch (const cv::Exception &exception) {
        setError(errorMessage,
                 QStringLiteral("OpenCV JPEG 解码异常：%1")
                     .arg(QString::fromLocal8Bit(exception.what())));
    } catch (const std::exception &exception) {
        setError(errorMessage,
                 QStringLiteral("JPEG 解码标准异常：%1")
                     .arg(QString::fromLocal8Bit(exception.what())));
    }
    return {};
}

} // namespace JpegFrameDecoder
