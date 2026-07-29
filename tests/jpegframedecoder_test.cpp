#include "jpegframedecoder.h"
#include "videopacketprotocol.h"

#include <QCoreApplication>
#include <QDebug>

#include <opencv2/imgcodecs.hpp>

#include <vector>

namespace {

bool check(bool condition, const QString &message)
{
    if (!condition) {
        qCritical().noquote() << QStringLiteral("[jpegframedecoder_test]") << message;
    }
    return condition;
}

cv::Mat createColorImage()
{
    cv::Mat image(480, 640, CV_8UC3);
    for (int y = 0; y < image.rows; ++y) {
        for (int x = 0; x < image.cols; ++x) {
            image.at<cv::Vec3b>(y, x) = cv::Vec3b(
                static_cast<uchar>((x * 3 + y) & 0xff),
                static_cast<uchar>((y * 5 + x) & 0xff),
                static_cast<uchar>((x + y * 7) & 0xff));
        }
    }
    return image;
}

QByteArray encodeJpeg(const cv::Mat &image)
{
    std::vector<uchar> encoded;
    if (!cv::imencode(".jpg", image, encoded, {cv::IMWRITE_JPEG_QUALITY, 60})) {
        return {};
    }
    return QByteArray(reinterpret_cast<const char *>(encoded.data()),
                      static_cast<qsizetype>(encoded.size()));
}

QImage decodeColorImageAfterInputsAreDestroyed()
{
    const cv::Mat image = createColorImage();
    const QByteArray jpegData = encodeJpeg(image);
    QString errorMessage;
    return JpegFrameDecoder::decodeJpegFrame(jpegData, &errorMessage);
}

bool testColorJpeg()
{
    QString errorMessage;
    const QByteArray jpegData = encodeJpeg(createColorImage());
    const QImage image = JpegFrameDecoder::decodeJpegFrame(jpegData, &errorMessage);

    bool success = true;
    success &= check(!image.isNull(), QStringLiteral("640×480 彩色 JPEG 解码失败：%1").arg(errorMessage));
    success &= check(image.width() == 640, QStringLiteral("彩色 JPEG 输出宽度不是 640。"));
    success &= check(image.height() == 480, QStringLiteral("彩色 JPEG 输出高度不是 480。"));
    success &= check(image.format() == QImage::Format_RGB888,
                     QStringLiteral("彩色 JPEG 输出不是 Format_RGB888。"));

    const QImage survivingImage = decodeColorImageAfterInputsAreDestroyed();
    success &= check(!survivingImage.isNull(), QStringLiteral("输入销毁后 QImage 无效。"));
    success &= check(survivingImage.constBits() != nullptr,
                     QStringLiteral("深拷贝 QImage 没有有效像素数据。"));
    success &= check(survivingImage.pixelColor(20, 20).isValid(),
                     QStringLiteral("深拷贝 QImage 像素不可访问。"));
    return success;
}

bool testGrayJpeg()
{
    cv::Mat image(480, 640, CV_8UC1);
    for (int y = 0; y < image.rows; ++y) {
        for (int x = 0; x < image.cols; ++x) {
            image.at<uchar>(y, x) = static_cast<uchar>((x + y) & 0xff);
        }
    }

    QString errorMessage;
    const QImage decoded = JpegFrameDecoder::decodeJpegFrame(encodeJpeg(image), &errorMessage);
    return check(!decoded.isNull(), QStringLiteral("灰度 JPEG 解码失败：%1").arg(errorMessage))
        && check(decoded.format() == QImage::Format_Grayscale8,
                 QStringLiteral("灰度 JPEG 输出不是 Format_Grayscale8。"));
}

bool testInvalidInputs()
{
    QString errorMessage;
    bool success = true;
    success &= check(JpegFrameDecoder::decodeJpegFrame({}, &errorMessage).isNull(),
                     QStringLiteral("空 JPEG 未被拒绝。"));
    success &= check(JpegFrameDecoder::decodeJpegFrame(QByteArray("not a jpeg"), &errorMessage).isNull(),
                     QStringLiteral("随机字节未被拒绝。"));
    success &= check(JpegFrameDecoder::decodeJpegFrame(QByteArray("\xff\xd8", 2), &errorMessage).isNull(),
                     QStringLiteral("只有 SOI 的截断 JPEG 未被拒绝。"));

    QByteArray missingEoi = encodeJpeg(createColorImage());
    if (!missingEoi.isEmpty()) {
        missingEoi.chop(2);
    }
    success &= check(JpegFrameDecoder::decodeJpegFrame(missingEoi, &errorMessage).isNull(),
                     QStringLiteral("缺少 EOI 的 JPEG 未被拒绝。"));

    QByteArray tooLarge(VideoPacketProtocol::MaximumFrameSize + 1, '\0');
    tooLarge[0] = static_cast<char>(0xff);
    tooLarge[1] = static_cast<char>(0xd8);
    tooLarge[tooLarge.size() - 2] = static_cast<char>(0xff);
    tooLarge[tooLarge.size() - 1] = static_cast<char>(0xd9);
    success &= check(JpegFrameDecoder::decodeJpegFrame(tooLarge, &errorMessage).isNull(),
                     QStringLiteral("超过 4 MiB 的 JPEG 未被拒绝。"));
    return success;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);

    const bool success = testColorJpeg()
        && testGrayJpeg()
        && testInvalidInputs();
    return success ? 0 : 1;
}
