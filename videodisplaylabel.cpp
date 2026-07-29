#include "videodisplaylabel.h"

#include <QSizePolicy>
#include <QtMath>

VideoDisplayLabel::VideoDisplayLabel(QWidget *parent)
    : QLabel(parent)
{
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    setAlignment(Qt::AlignCenter);
    setScaledContents(false);
}

bool VideoDisplayLabel::hasHeightForWidth() const
{
    return true;
}

int VideoDisplayLabel::heightForWidth(int width) const
{
    return width > 0 ? qRound(static_cast<double>(width) * 3.0 / 4.0) : 0;
}

QSize VideoDisplayLabel::sizeHint() const
{
    return QSize(640, 480);
}

QSize VideoDisplayLabel::minimumSizeHint() const
{
    return QSize(320, 240);
}

VideoDisplayContainer::VideoDisplayContainer(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}
