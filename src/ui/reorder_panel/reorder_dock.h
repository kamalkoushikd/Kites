#pragma once

#include "reorder/reorder_analysis.h"

#include <QDockWidget>
#include <QString>
#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QMainWindow;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QStackedWidget;
class QTextBrowser;
class QTimer;
class QToolButton;

namespace Kites
{
class EditorTab;
class ProcessorManager;
class ReorderDiffView;
class SuggestionPanel;

/**
 * @brief Dock that suggests an instruction order with fewer pipeline stalls.
 *
 * Shows the suggestion as a diff with an explanation for every change, and
 * applies it to the editor as one undoable edit (breakpoints follow their
 * instructions). Three views can be chosen: "Side by side" and "Unified"
 * (the diff) and "Suggestions" (one move at a time with a pipeline timeline,
 * see SuggestionPanel). It only reads the editor text and the processor type, so it
 * does not change how the rest of Kites works.
 */
class ReorderDock : public QDockWidget
{
    Q_OBJECT
public:
    /// Creates the dock (hidden) and adds a "Reorder" action to the main toolbar.
    static ReorderDock *install(QMainWindow *window, EditorTab *editorTab, ProcessorManager *processorManager);

    ReorderDock(QMainWindow *window, EditorTab *editorTab, ProcessorManager *processorManager);

public slots:
    void showAndAnalyze();
    void showSuggestions(); ///< opens the panel on the "Suggestions" view
    void popOut();
    void dockTo(Qt::DockWidgetArea area);
    void scrollToFirstChange();
    void analyze();
    void onProcessorChanged();

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onTopLevelChanged(bool floating);

private:
    QPlainTextEdit *editor() const;
    reorder::PipelineVariant currentVariant() const;
    void syncVariantWithProcessor();
    void onEditorTextChanged();
    void apply();
    void revert();
    void copyPatch();
    void replaceEditorText(const QString &text, const std::vector<int> &lineMap);
    void updateState();
    void showDetails(const QString &html);
    void jumpToLine(int line, int sourceLine, bool newSide);
    QString wrapDetails(const QString &html) const;
    bool suggestionsView() const;
    void updateViewMode();

    QMainWindow *m_window = nullptr;
    EditorTab *m_editorTab = nullptr;
    ProcessorManager *m_processorManager = nullptr;

    QComboBox *m_variantCombo = nullptr;
    QSpinBox *m_windowSpin = nullptr;
    QPushButton *m_analyzeButton = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_revertButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QToolButton *m_sideBySideButton = nullptr;
    QToolButton *m_unifiedButton = nullptr;
    QToolButton *m_suggestionsButton = nullptr;
    QStackedWidget *m_views = nullptr;           ///< page 0: the diff, page 1: the suggestions
    SuggestionPanel *m_suggestions = nullptr;
    QCheckBox *m_onlyChangedCheck = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_bannerLabel = nullptr;
    ReorderDiffView *m_diffView = nullptr;
    QScrollArea *m_scroll = nullptr;
    QTextBrowser *m_details = nullptr;
    QTimer *m_reanalyzeTimer = nullptr;
    QToolButton *m_windowButton = nullptr;
    bool m_changingWindowMode = false; ///< set while we switch between docked and popped out

    std::shared_ptr<const reorder::AnalysisResult> m_result;
    QString m_analyzedText;      ///< editor text the result was computed from
    QString m_textBeforeApply;   ///< for Revert
    QString m_appliedText;       ///< editor text right after Apply
    std::vector<uint64_t> m_breakpointsBeforeApply;
    std::vector<int> m_revertLineMap;
    bool m_ignoreTextChange = false;
    bool m_pinned = false;
    bool m_singleCycleProcessor = false;
};

} // namespace Kites
