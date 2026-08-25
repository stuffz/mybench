#pragma once
// The chrome around the whole-server FK graph: toolbar (refresh, fit, node
// size, hops, find-and-focus) and status line around the GraphCanvas that
// draws it.
#include "views/graphcanvas.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;

class GraphView : public QWidget
{
    Q_OBJECT
public:
    explicit GraphView(const QString &connID, QWidget *parent = nullptr);
    void focusOn(const QString &schema, const QString &table);

private:
    void refresh();
    void runFind();

    void setHops(int h);

    QString m_connID;
    GraphCanvas *m_canvas;
    QLabel *m_counts, *m_error;
    QCheckBox *m_hideIsolated;
    QSlider *m_size;
    // The hops stepper: −/+ buttons around this readout, backed by m_hopsN.
    QLabel *m_hopsValue;
    int m_hopsN = 1;
    QLineEdit *m_find;
    QListWidget *m_findList;
    QPushButton *m_clearFocus;
};
