#pragma once
// The application's own QProxyStyle: the two places where Qt's defaults do not
// match how the app is meant to feel.
#include <QProxyStyle>

class AppStyle : public QProxyStyle
{
public:
    // Qt Style Sheets have no cursor property, so a clickable control cannot be
    // given a pointing hand from the theme sheet. polish() is called for every
    // widget as it is styled, including the buttons QDialogButtonBox builds on
    // our behalf, which a button factory would never reach.
    void polish(QWidget *widget) override;
    void unpolish(QWidget *widget) override;

private:
    static bool isClickable(const QWidget *widget);

public:
    int styleHint(
        StyleHint hint, const QStyleOption *option, const QWidget *widget,
        QStyleHintReturn *returnData
    ) const override;
};
