#ifndef JPEGFRAMEDECODER_H
#define JPEGFRAMEDECODER_H

#include <QByteArray>
#include <QImage>
#include <QString>

namespace JpegFrameDecoder
{

QImage decodeJpegFrame(const QByteArray &jpegData,
                       QString *errorMessage = nullptr);

} // namespace JpegFrameDecoder

#endif // JPEGFRAMEDECODER_H
