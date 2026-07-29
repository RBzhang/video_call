#ifndef JPEGFRAMEENCODER_H
#define JPEGFRAMEENCODER_H

#include <QByteArray>
#include <QString>

#include <opencv2/core.hpp>

namespace JpegFrameEncoder
{

QByteArray encodeBgrFrame(const cv::Mat &bgrFrame,
                          int jpegQuality,
                          QString *errorMessage = nullptr);

} // namespace JpegFrameEncoder

#endif // JPEGFRAMEENCODER_H
