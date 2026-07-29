#include "jpegframeencoder.h"
#include "videopacketprotocol.h"

#include <QCoreApplication>
#include <QDebug>

#include <opencv2/imgcodecs.hpp>

#include <vector>

namespace {

bool check(bool condition, const QString &message)
{
    if (!condition) {
        qCritical().noquote() << QStringLiteral("[jpegframeencoder_test]") << message;
    }
    return condition;
}

cv::Mat createBgrTestImage()
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

bool testBgrEncoding()
{
    QString errorMessage;
    const cv::Mat image = createBgrTestImage();
    const QByteArray jpegData = JpegFrameEncoder::encodeBgrFrame(image, 60, &errorMessage);

    bool success = true;
    success &= check(!jpegData.isEmpty(), QStringLiteral("quality=60 BGR 编码失败：%1").arg(errorMessage));
    success &= check(jpegData.size() < VideoPacketProtocol::MaximumFrameSize,
                     QStringLiteral("JPEG 输出超过 4 MiB。"));
    success &= check(jpegData.size() >= 4
                         && static_cast<uchar>(jpegData.at(0)) == 0xff
                         && static_cast<uchar>(jpegData.at(1)) == 0xd8,
                     QStringLiteral("JPEG 缺少 SOI 标记。"));
    success &= check(jpegData.size() >= 4
                         && static_cast<uchar>(jpegData.at(jpegData.size() - 2)) == 0xff
                         && static_cast<uchar>(jpegData.at(jpegData.size() - 1)) == 0xd9,
                     QStringLiteral("JPEG 缺少 EOI 标记。"));

    const std::vector<uchar> encoded(jpegData.begin(), jpegData.end());
    const cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_COLOR);
    success &= check(!decoded.empty(), QStringLiteral("JPEG 解码失败。"));
    success &= check(decoded.cols == 640 && decoded.rows == 480,
                     QStringLiteral("JPEG 解码尺寸不是 640×480。"));
    return success;
}

bool testGrayEncoding()
{
    cv::Mat image(480, 640, CV_8UC1);
    for (int y = 0; y < image.rows; ++y) {
        for (int x = 0; x < image.cols; ++x) {
            image.at<uchar>(y, x) = static_cast<uchar>((x + y) & 0xff);
        }
    }

    QString errorMessage;
    const QByteArray jpegData = JpegFrameEncoder::encodeBgrFrame(image, 60, &errorMessage);
    return check(!jpegData.isEmpty(), QStringLiteral("灰度 JPEG 编码失败：%1").arg(errorMessage));
}

bool testInvalidInput()
{
    QString errorMessage;
    bool success = true;
    success &= check(JpegFrameEncoder::encodeBgrFrame(cv::Mat(), 60, &errorMessage).isEmpty(),
                     QStringLiteral("空图像未被拒绝。"));
    success &= check(JpegFrameEncoder::encodeBgrFrame(createBgrTestImage(), 0, &errorMessage).isEmpty(),
                     QStringLiteral("quality=0 未被拒绝。"));
    success &= check(JpegFrameEncoder::encodeBgrFrame(createBgrTestImage(), 101, &errorMessage).isEmpty(),
                     QStringLiteral("quality=101 未被拒绝。"));
    return success;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);

    const bool success = testBgrEncoding()
        && testGrayEncoding()
        && testInvalidInput();
    return success ? 0 : 1;
}
