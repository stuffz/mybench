#pragma once
// Preferences. Applied immediately and saved with the workspace, same as the
// web dialog; the theme lists come from theme.h so adding a palette adds a
// row here for free. The MCP section talks to the backend directly — those
// settings live in SQLite, not in the prefs blob.
#include <QDialog>
#include <QJsonObject>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QSlider;
class QSpinBox;

class PrefsDialog : public QDialog
{
    Q_OBJECT
public:
    PrefsDialog(const QJsonObject &prefs, QWidget *parent = nullptr);

signals:
    void prefsChanged(const QJsonObject &prefs);

private:
    void emitPrefs();
    // Updates the "13px" readouts only. Applying a font size re-lays out this
    // dialog too, which moves the slider out from under the pointer mid-drag,
    // so the apply is deferred while a handle is being dragged.
    void updateSizeLabels();
    // Reflects a backend mcp.Status into the switch, port, endpoint and error
    // rows (the latter three exist only while the listener is enabled).
    void applyMcpStatus(const QJsonObject &status);
    void configureMcp(bool enabled, int port);

    QJsonObject m_prefs;
    class QTimer *m_applyTimer;
    QFormLayout *m_form = nullptr;
    QComboBox *m_appTheme, *m_editorTheme, *m_copySep;
    QSlider *m_uiFontSize, *m_editorFontSize, *m_tabSize;
    QLabel *m_uiFontSizeLbl, *m_editorFontSizeLbl, *m_tabSizeLbl;
    QCheckBox *m_hideDefaultDBs, *m_mcp;
    QSpinBox *m_historyKeep, *m_defaultRows;
    int m_sentHistoryKeep = -1; // last value pushed to the backend

    QSpinBox *m_mcpPort = nullptr;
    QLabel *m_mcpUrl = nullptr, *m_mcpError = nullptr;
    QString m_mcpToken;
    int m_mcpPortRow = -1, m_mcpEndpointRow = -1, m_mcpErrorRow = -1;
    int m_mcpAppliedPort = -1; // last port the backend confirmed
};
