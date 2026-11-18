#pragma once

#include "scheduler/scheduler.h"

#include <QWidget>
#include <QPointer>
#include <QSet>
#include <QString>
#include <vector>

class QAction;
class QLabel;
class QMainWindow;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class QToolButton;

namespace Kites
{
class EditorTab;
class HazardSquiggles;
class PipelineTimeline;
class ProcessorManager;

/**
 * @brief "Style C" suggestions: squiggles in the editor, and a view with one
 *        suggestion at a time (Moved / Why / Effect + pipeline timeline).
 *
 * It is the "Suggestions" view of the Reorder panel (ReorderDock), next to
 * "Side by side" and "Unified".
 *
 * Flow:
 *   Analyze (Reorder panel) -> scheduler::analyze() on the editor text
 *                           -> waiting lines get a squiggle (hover = the three lines)
 *                           -> the view shows suggestion 1 of N with its block's timeline
 *   Apply                   -> rewrites only that block, as one undo step, then analyzes again
 *   Dismiss                 -> hides that suggestion until the code changes
 *   Run (toolbar)           -> if there are suggestions, a banner offers "Review suggestions" or "Run anyway"
 */
class SuggestionPanel : public QWidget
{
    Q_OBJECT
public:
    SuggestionPanel(QWidget *parent, EditorTab *editorTab, ProcessorManager *processorManager);

    /// Watches the toolbar's Run button, so pressing Run can show the banner first.
    void attachRunButton(QMainWindow *window);
    /// Pipeline and window size to analyze for (the Reorder panel's "Pipeline" and "Window").
    void setOptions(const scheduler::Options &options);

signals:
    /// "Review suggestions" was clicked in the Run banner.
    void reviewRequested();

public slots:
    void analyze();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    QPlainTextEdit *editor() const;
    scheduler::Options processorOptions() const;
    bool pipelineProcessor() const;
    std::vector<int> visibleSuggestions() const;
    QString suggestionKey(const scheduler::Suggestion &suggestion) const;
    void showSuggestion(int index);
    void updateSquiggles();
    void apply();
    void dismiss();
    void onTextChanged();
    void guardRun();
    void showRunBanner(int suggestions, int saved);
    void hideRunBanner();
    void placeRunBanner();

    EditorTab *m_editorTab = nullptr;
    ProcessorManager *m_processorManager = nullptr;

    QLabel *m_titleLabel = nullptr;
    QLabel *m_modeLabel = nullptr;
    QLabel *m_movedLabel = nullptr;
    QLabel *m_codeLabel = nullptr; ///< the moved instruction, in its own code block
    QLabel *m_whyLabel = nullptr;
    QLabel *m_effectLabel = nullptr;
    QLabel *m_noteLabel = nullptr;
    QPushButton *m_previousButton = nullptr;
    QPushButton *m_nextButton = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_dismissButton = nullptr;
    PipelineTimeline *m_timeline = nullptr;
    QWidget *m_details = nullptr;
    HazardSquiggles *m_squiggles = nullptr;
    QTimer *m_reanalyzeTimer = nullptr;

    // Run banner
    QPointer<QAction> m_runAction;
    QPointer<QToolButton> m_runButton;
    QWidget *m_runBanner = nullptr;
    QLabel *m_runBannerLabel = nullptr;
    bool m_runAnyway = false; ///< the next Run click goes through without the banner

    scheduler::Options m_options; ///< set by the Reorder panel
    scheduler::Result m_result;
    bool m_analyzed = false;
    QString m_analyzedText;
    QSet<QString> m_dismissed;
    int m_current = 0; ///< position in visibleSuggestions()
    bool m_applying = false;
};

} // namespace Kites
