#ifndef VIDEODISPLAYLABEL_H
#define VIDEODISPLAYLABEL_H

#include <QLabel>


class VideoDisplayLabel : public QLabel
{
    Q_OBJECT

public:
    explicit VideoDisplayLabel(QWidget *parent = nullptr);

    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
};

class VideoDisplayContainer : public QWidget
{
    Q_OBJECT

public:
    explicit VideoDisplayContainer(QWidget *parent = nullptr);
};

#endif // VIDEODISPLAYLABEL_H
