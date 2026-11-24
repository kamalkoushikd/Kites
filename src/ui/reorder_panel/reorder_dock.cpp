#include "reorder_dock.h"

#include "reorder_diff_view.h"

#include "processor/processor_manager.h"
#include "ui/suggestions/suggestion_panel.h"
#include "ui/editor_tab/editortab.h"
#include "ui/editor_tab/main_editor.h"
#include "ui/mainwindow/mainwindow.h"
#include "ui/theme/font_manager.h"
#include "ui/theme/theme_manager.h"

#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace Kites
{
using reorder::PipelineVariant;

namespace
{
const char *const kDockTitle = "Instruction Reordering";

QString colorHex(const QColor &color)
{
    return color.name(QColor::HexRgb);
}

QColor mix(const QColor &from, const QColor &to, double amount)
{
    return QColor::fromRgbF(static_cast<float>(from.redF() * (1.0 - amount) + to.redF() * amount),
                            static_cast<float>(from.greenF() * (1.0 - amount) + to.greenF() * amount),
                            static_cast<float>(from.blueF() * (1.0 - amount) + to.blueF() * amount));
}
} // namespace

ReorderDock *ReorderDock::install(QMainWindow *window, EditorTab *editorTab, ProcessorManager *processorManager)
{
    auto *dock = new ReorderDock(window, editorTab, processorManager);
    window->addDockWidget(Qt::BottomDockWidgetArea, dock);
    dock->hide();

    QToolBar *toolbar = nullptr;
    for (QToolBar *candidate : window->findChildren<QToolBar *>())
    {
        if (candidate->windowTitle() == QLatin1String("Main Toolbar"))
        {
            toolbar = candidate;
            break;
        }
    }
    auto *action = new QAction(tr("Reorder"), window);
    action->setToolTip(tr("Suggest an instruction order with fewer pipeline stalls, and explain every change"));
    if (toolbar)
    {
        toolbar->addSeparator();
        toolbar->addAction(action);
    }
    connect(action, &QAction::triggered, dock, &ReorderDock::showAndAnalyze);
    dock->m_suggestions->attachRunButton(window); // the Run banner ("Review suggestions" / "Run anyway")

    if (auto *mainWindow = qobject_cast<MainWindow *>(window))
    {
        connect(mainWindow, &MainWindow::processorChangedSignal, dock, &ReorderDock::onProcessorChanged);
    }
    return dock;
}

ReorderDock::ReorderDock(QMainWindow *window, EditorTab *editorTab, ProcessorManager *processorManager)
    : QDockWidget(tr(kDockTitle), window), m_window(window), m_editorTab(editorTab),
      m_processorManager(processorManager)
{
    setObjectName(QStringLiteral("instructionReorderingDock"));
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    setAllowedAreas(Qt::BottomDockWidgetArea | Qt::LeftDockWidgetArea);

    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(8, 6, 8, 8);
    layout->setSpacing(6);

    // ---- first row: what to analyze, and the actions --------------------------------
    auto *controls = new QHBoxLayout();
    controls->setSpacing(8);
    m_variantCombo = new QComboBox(content);
    m_variantCombo->addItem(tr("Hazard unit + forwarding (H_F)"));
    m_variantCombo->addItem(tr("Hazard unit, no forwarding (H_NF)"));
    m_variantCombo->addItem(tr("No hazard unit + forwarding (NH_F)"));
    m_variantCombo->addItem(tr("No hazard unit, no forwarding (NH_NF)"));
    m_variantCombo->setToolTip(tr("The 5-stage pipeline to optimize for. It follows the processor chosen in Kites."));
    m_windowSpin = new QSpinBox(content);
    m_windowSpin->setRange(1, 64);
    m_windowSpin->setValue(16);
    m_windowSpin->setSuffix(tr(" instructions"));
    m_windowSpin->setToolTip(tr("Instructions are only reordered inside windows of this many instructions "
                                "of a basic block (a block ends at a label or a branch)."));
    m_analyzeButton = new QPushButton(tr("Analyze"), content);
    m_analyzeButton->setToolTip(tr("Analyze the code in the editor"));
    m_copyButton = new QPushButton(tr("Copy patch"), content);
    m_copyButton->setToolTip(tr("Copy the suggestion as a unified diff (git apply can use it)"));
    m_revertButton = new QPushButton(tr("Revert"), content);
    m_revertButton->setToolTip(tr("Put back the code as it was before Apply"));
    m_applyButton = new QPushButton(tr("Apply"), content);
    m_applyButton->setToolTip(tr("Replace the code in the editor with the suggestion (one undo step)"));
    controls->addWidget(new QLabel(tr("Pipeline"), content));
    controls->addWidget(m_variantCombo);
    controls->addSpacing(6);
    controls->addWidget(new QLabel(tr("Window"), content));
    controls->addWidget(m_windowSpin);
    controls->addWidget(m_analyzeButton);
    controls->addStretch(1);
    controls->addWidget(m_copyButton);
    controls->addWidget(m_revertButton);
    controls->addWidget(m_applyButton);

    // Where the panel lives: docked at the bottom, docked at the left, or a window of its own.
    m_windowButton = new QToolButton(content);
    m_windowButton->setText(tr("Window"));
    m_windowButton->setToolTip(tr("Dock the panel at the bottom or left, or pop it out as a normal window"));
    m_windowButton->setPopupMode(QToolButton::InstantPopup);
    auto *windowMenu = new QMenu(m_windowButton);
    windowMenu->addAction(tr("Pop out as window"), this, &ReorderDock::popOut);
    windowMenu->addAction(tr("Dock at bottom"), this, [this]() { dockTo(Qt::BottomDockWidgetArea); });
    windowMenu->addAction(tr("Dock at left"), this, [this]() { dockTo(Qt::LeftDockWidgetArea); });
    m_windowButton->setMenu(windowMenu);
    controls->addWidget(m_windowButton);
    layout->addLayout(controls);

    // ---- second row: summary and view options ------------------------------------
    m_statusRow = new QWidget(content);
    auto *status = new QHBoxLayout(m_statusRow);
    status->setContentsMargins(0, 0, 0, 0);
    status->setSpacing(8);
    m_summaryLabel = new QLabel(content);
    m_summaryLabel->setTextFormat(Qt::RichText);
    m_summaryLabel->setToolTip(tr("Stalls / NOPs: the scheduler's count, every instruction once.\n"
                                  "Measured on Kites: both programs run on the selected 5-stage pipeline; a stall "
                                  "inside a loop counts every time the loop runs."));
    m_onlyChangedCheck = new QCheckBox(tr("Only changed blocks"), content);
    m_onlyChangedCheck->setChecked(true);
    m_onlyChangedCheck->setToolTip(tr("Fold blocks that the suggestion does not change"));
    m_sideBySideButton = new QToolButton(content);
    m_sideBySideButton->setText(tr("Side by side"));
    m_sideBySideButton->setCheckable(true);
    m_sideBySideButton->setAutoExclusive(true);
    m_sideBySideButton->setChecked(true);
    m_unifiedButton = new QToolButton(content);
    m_unifiedButton->setText(tr("Unified"));
    m_unifiedButton->setCheckable(true);
    m_unifiedButton->setAutoExclusive(true);
    m_suggestionsButton = new QToolButton(content);
    m_suggestionsButton->setText(tr("Suggestions"));
    m_suggestionsButton->setCheckable(true);
    m_suggestionsButton->setAutoExclusive(true);
    m_suggestionsButton->setToolTip(tr("One move at a time: what moved, why, the effect, and a pipeline timeline. "
                                       "Waiting lines are underlined in the editor."));
    // the Kites theme has no style for checked buttons; show which view is on
    const QString toggleStyle = QStringLiteral("QToolButton:checked { background-color: palette(highlight); "
                                               "color: palette(highlighted-text); }");
    m_sideBySideButton->setStyleSheet(toggleStyle);
    m_unifiedButton->setStyleSheet(toggleStyle);
    m_suggestionsButton->setStyleSheet(toggleStyle);
    status->addWidget(m_summaryLabel, 1);
    status->addWidget(m_onlyChangedCheck);
    layout->addWidget(m_statusRow);
    // the view choice sits in the first row, so it stays in the same place in every view
    controls->insertWidget(controls->indexOf(m_copyButton), m_sideBySideButton);
    controls->insertWidget(controls->indexOf(m_copyButton), m_unifiedButton);
    controls->insertWidget(controls->indexOf(m_copyButton), m_suggestionsButton);
    controls->insertSpacing(controls->indexOf(m_copyButton), 12);

    m_bannerLabel = new QLabel(content);
    m_bannerLabel->setWordWrap(true);
    m_bannerLabel->setTextFormat(Qt::RichText);
    m_bannerLabel->hide();
    layout->addWidget(m_bannerLabel);

    // ---- diff and details ------------------------------------------------------------
    auto *splitter = new QSplitter(Qt::Horizontal, content);
    auto *scroll = new QScrollArea(splitter);
    m_scroll = scroll;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_diffView = new ReorderDiffView(scroll);
    scroll->setWidget(m_diffView);
    m_details = new QTextBrowser(splitter);
    m_details->setOpenLinks(false);
    m_details->setFrameShape(QFrame::NoFrame);
    splitter->addWidget(scroll);
    splitter->addWidget(m_details);
    splitter->setCollapsible(0, false);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({720, 380});

    // ---- the two kinds of view: the diff (above), or the suggestions -----------------
    m_suggestions = new SuggestionPanel(content, editorTab, processorManager);
    m_views = new QStackedWidget(content);
    m_views->addWidget(splitter);      // Side by side / Unified
    m_views->addWidget(m_suggestions); // Suggestions
    layout->addWidget(m_views, 1);
    setWidget(content);

    m_measureTimer = new QTimer(this);
    m_measureTimer->setInterval(0);
    connect(m_measureTimer, &QTimer::timeout, this, &ReorderDock::stepMeasuring);

    m_reanalyzeTimer = new QTimer(this);
    m_reanalyzeTimer->setSingleShot(true);
    m_reanalyzeTimer->setInterval(600);
    connect(m_reanalyzeTimer, &QTimer::timeout, this,
            [this]()
            {
                if (isVisible())
                {
                    analyze();
                }
            });

    connect(m_analyzeButton, &QPushButton::clicked, this, &ReorderDock::analyze);
    connect(m_applyButton, &QPushButton::clicked, this, &ReorderDock::apply);
    connect(m_revertButton, &QPushButton::clicked, this, &ReorderDock::revert);
    connect(m_copyButton, &QPushButton::clicked, this, &ReorderDock::copyPatch);
    connect(m_variantCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int)
            {
                if (isVisible())
                {
                    analyze();
                }
            });
    connect(m_windowSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { m_reanalyzeTimer->start(); });
    connect(m_sideBySideButton, &QToolButton::toggled, this,
            [this](bool checked)
            {
                if (checked)
                {
                    m_diffView->setMode(ReorderDiffView::Mode::SideBySide);
                    scrollToFirstChange();
                }
            });
    connect(m_unifiedButton, &QToolButton::toggled, this,
            [this](bool checked)
            {
                if (checked)
                {
                    m_diffView->setMode(ReorderDiffView::Mode::Unified);
                    scrollToFirstChange();
                }
            });
    connect(m_suggestionsButton, &QToolButton::toggled, this, [this](bool) { updateViewMode(); });
    connect(m_suggestions, &SuggestionPanel::reviewRequested, this, &ReorderDock::showSuggestions);
    connect(m_onlyChangedCheck, &QCheckBox::toggled, m_diffView, &ReorderDiffView::setCollapseUnchanged);
    connect(m_diffView, &ReorderDiffView::detailHovered, this,
            [this](const QString &html)
            {
                if (!m_pinned)
                {
                    showDetails(html);
                }
            });
    connect(m_diffView, &ReorderDiffView::detailClicked, this,
            [this](const QString &html, int line, int sourceLine, bool newSide)
            {
                if (html.isEmpty())
                {
                    m_pinned = false;
                    showDetails(QString());
                }
                else
                {
                    m_pinned = true;
                    showDetails(html);
                }
                jumpToLine(line, sourceLine, newSide);
            });
    connect(&ThemeManager::getInstance(), &ThemeManager::themeChangedSignal, this,
            [this](const QString &) { updateState(); });

    if (QPlainTextEdit *edit = editor())
    {
        connect(edit, &QPlainTextEdit::textChanged, this, &ReorderDock::onEditorTextChanged);
        m_diffView->setFont(edit->font());
    }
    connect(&FontManager::getInstance(), &FontManager::fontChangedSignal, this,
            [this](const QFont &)
            {
                // the editor applies the new font first; copy it once that happened
                QTimer::singleShot(0, this,
                                   [this]()
                                   {
                                       if (QPlainTextEdit *edit = editor())
                                       {
                                           m_diffView->setFont(edit->font());
                                       }
                                   });
            });

    // Popping out (with the float button, by dragging, or from our menu) ends up here.
    connect(this, &QDockWidget::topLevelChanged, this, &ReorderDock::onTopLevelChanged);

    syncVariantWithProcessor();
    showDetails(QString());
    updateState();
    updateViewMode();
}

/*
 * Docked vs. popped out
 *
 * A floating QDockWidget is normally a "tool" window. On X11 Qt also draws its
 * own title bar for it, so it has no minimize/maximize buttons and does not
 * look like the other windows. When the panel floats we therefore:
 *   1. hide Qt's own title bar (an empty title bar widget), and
 *   2. make it an ordinary window (Qt::Window with the usual buttons),
 * so the desktop draws the title bar and the window can be minimized,
 * maximized, resized and closed like any other. Docking undoes both steps.
 */
void ReorderDock::onTopLevelChanged(bool floating)
{
    if (!floating || m_changingWindowMode)
    {
        return;
    }
    m_changingWindowMode = true;
    const QRect geometry = this->geometry();
    setTitleBarWidget(new QWidget(this)); // an empty widget: Qt's own title bar disappears
    setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint |
                   Qt::WindowCloseButtonHint);
    setGeometry(geometry);
    show(); // changing the window flags hides a widget, so show it again
    m_changingWindowMode = false;
}

void ReorderDock::popOut()
{
    if (isFloating())
    {
        return;
    }
    const QSize size(std::max(900, width()), std::max(500, height()));
    setFloating(true); // emits topLevelChanged -> onTopLevelChanged makes it a normal window
    resize(size);
    show();
    raise();
    activateWindow();
}

void ReorderDock::dockTo(Qt::DockWidgetArea area)
{
    if (!m_window)
    {
        return;
    }
    m_changingWindowMode = true;
    delete titleBarWidget(); // back to Qt's own title bar (with its float and close buttons)
    setTitleBarWidget(nullptr);
    setFloating(false);      // QDockWidget restores the window flags of a docked panel
    m_window->addDockWidget(area, this);
    show();
    // give the docked panel a useful size: half the height at the bottom, 45 % of the width at the left
    if (area == Qt::LeftDockWidgetArea)
    {
        m_window->resizeDocks({this}, {m_window->width() * 45 / 100}, Qt::Horizontal);
    }
    else
    {
        m_window->resizeDocks({this}, {m_window->height() / 2}, Qt::Vertical);
    }
    m_changingWindowMode = false;
}

QPlainTextEdit *ReorderDock::editor() const
{
    return m_editorTab ? m_editorTab->findChild<QPlainTextEdit *>(QStringLiteral("assemblyTextEdit")) : nullptr;
}

PipelineVariant ReorderDock::currentVariant() const
{
    switch (m_variantCombo->currentIndex())
    {
    case 1:
        return PipelineVariant::HazardUnitNoForwarding;
    case 2:
        return PipelineVariant::NoHazardUnitForwarding;
    case 3:
        return PipelineVariant::NoHazardUnitNoForwarding;
    default:
        return PipelineVariant::HazardUnitForwarding;
    }
}

void ReorderDock::syncVariantWithProcessor()
{
    if (!m_processorManager)
    {
        return;
    }
    int index = -1;
    switch (m_processorManager->getProcessorType())
    {
    case ProcessorType::RV5Stage_H_F:
        index = 0;
        break;
    case ProcessorType::RV5Stage_H_NF:
        index = 1;
        break;
    case ProcessorType::RV5Stage_NH_F:
        index = 2;
        break;
    case ProcessorType::RV5Stage_NH_NF:
        index = 3;
        break;
    default:
        break;
    }
    m_singleCycleProcessor = index < 0;
    if (index >= 0 && index != m_variantCombo->currentIndex())
    {
        const QSignalBlocker blocker(m_variantCombo);
        m_variantCombo->setCurrentIndex(index);
    }
}

void ReorderDock::showAndAnalyze()
{
    const bool wasHidden = isHidden();
    show();
    if (isFloating() && (windowState() & Qt::WindowMinimized))
    {
        setWindowState(windowState() & ~Qt::WindowMinimized); // bring a minimized window back
    }
    raise();
    if (isFloating())
    {
        activateWindow();
    }
    if (wasHidden && m_window && !isFloating())
    {
        m_window->resizeDocks({this}, {std::max(300, m_window->height() / 2)}, Qt::Vertical);
    }
    analyze();
}

void ReorderDock::showSuggestions()
{
    syncVariantWithProcessor(); // the banner counted for the selected processor
    m_suggestionsButton->setChecked(true);
    showAndAnalyze();
}

bool ReorderDock::suggestionsView() const
{
    return m_suggestionsButton->isChecked();
}

// The diff's own controls are hidden in the Suggestions view (it has its own Apply).
void ReorderDock::updateViewMode()
{
    const bool diff = !suggestionsView();
    m_views->setCurrentIndex(diff ? 0 : 1);
    m_statusRow->setVisible(diff); // summary and "Only changed blocks" belong to the diff
    m_copyButton->setVisible(diff);
    m_revertButton->setVisible(diff);
    m_applyButton->setVisible(diff);
    updateState();
}

void ReorderDock::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);
    QPlainTextEdit *edit = editor();
    if (edit && (!m_result || edit->toPlainText() != m_analyzedText))
    {
        m_reanalyzeTimer->start(0);
    }
}

void ReorderDock::onProcessorChanged()
{
    syncVariantWithProcessor();
    if (isVisible())
    {
        analyze();
    }
    else
    {
        updateState();
    }
}

void ReorderDock::analyze()
{
    m_reanalyzeTimer->stop();
    QPlainTextEdit *edit = editor();
    if (!edit)
    {
        return;
    }
    reorder::AnalysisOptions options;
    options.variant = currentVariant();
    options.windowSize = m_windowSpin->value();
    m_analyzedText = edit->toPlainText();
    m_result = std::make_shared<const reorder::AnalysisResult>(
        reorder::analyzeProgram(m_analyzedText.toStdString(), options));
    m_pinned = false;
    m_diffView->setPlaceholderText(m_result->ok ? tr("There is nothing to show for this program.")
                                                : QString::fromStdString(m_result->error));
    m_diffView->setResult(m_result);
    startMeasuring();
    scrollToFirstChange();
    showDetails(QString());
    updateState();

    // the Suggestions view analyzes the same code for the same pipeline and window
    suggestions::Options suggestionOptions;
    suggestionOptions.forwarding = reorder::hasForwarding(options.variant);
    suggestionOptions.hazardUnit = reorder::hasHazardUnit(options.variant);
    suggestionOptions.window = options.windowSize;
    m_suggestions->setOptions(suggestionOptions);
    m_suggestions->analyze();
}

void ReorderDock::scrollToFirstChange()
{
    // after the scroll area has resized the view
    QTimer::singleShot(0, this,
                       [this]()
                       {
                           const int y = m_diffView->firstChangeY();
                           m_scroll->verticalScrollBar()->setValue(y > 0 ? y : 0);
                       });
}

void ReorderDock::onEditorTextChanged()
{
    if (m_ignoreTextChange)
    {
        return;
    }
    QPlainTextEdit *edit = editor();
    // Undo (Ctrl+Z) of an Apply: the breakpoints go back with the text.
    if (edit && !m_appliedText.isEmpty() && edit->toPlainText() == m_textBeforeApply)
    {
        if (auto *kitesEditor = dynamic_cast<Editor *>(edit))
        {
            kitesEditor->setBreakpoints(m_breakpointsBeforeApply);
        }
        m_appliedText.clear();
    }
    updateState();
    if (isVisible())
    {
        m_reanalyzeTimer->start();
    }
}

void ReorderDock::apply()
{
    QPlainTextEdit *edit = editor();
    if (!edit || !m_result || !m_result->ok || !m_result->changed)
    {
        return;
    }
    if (edit->isReadOnly())
    {
        QMessageBox::information(this, tr(kDockTitle), tr("Stop the running program before changing the code."));
        return;
    }
    if (edit->toPlainText() != m_analyzedText)
    {
        analyze();
        return;
    }

    m_textBeforeApply = m_analyzedText;
    m_breakpointsBeforeApply.clear();
    if (auto *kitesEditor = dynamic_cast<Editor *>(edit))
    {
        m_breakpointsBeforeApply = kitesEditor->getBreakpoints();
    }
    const std::vector<int> &lineMap = m_result->lineMap;
    int newLineCount = 0;
    for (int line : lineMap)
    {
        newLineCount = std::max(newLineCount, line);
    }
    m_revertLineMap.assign(static_cast<size_t>(newLineCount), -1);
    for (size_t old = 0; old < lineMap.size(); ++old)
    {
        if (lineMap[old] > 0)
        {
            m_revertLineMap[static_cast<size_t>(lineMap[old] - 1)] = static_cast<int>(old) + 1;
        }
    }

    replaceEditorText(QString::fromStdString(m_result->newText), lineMap);
    m_appliedText = edit->toPlainText();
    m_pinned = false;
    showDetails(QString());
    updateState();
}

void ReorderDock::revert()
{
    QPlainTextEdit *edit = editor();
    if (!edit || m_appliedText.isEmpty())
    {
        return;
    }
    if (edit->isReadOnly())
    {
        QMessageBox::information(this, tr(kDockTitle), tr("Stop the running program before changing the code."));
        return;
    }
    if (edit->toPlainText() != m_appliedText)
    {
        QMessageBox::information(this, tr(kDockTitle),
                                 tr("The code was edited after Apply. Use Undo (Ctrl+Z) in the editor instead."));
        return;
    }
    replaceEditorText(m_textBeforeApply, m_revertLineMap);
    if (auto *kitesEditor = dynamic_cast<Editor *>(edit))
    {
        kitesEditor->setBreakpoints(m_breakpointsBeforeApply);
    }
    m_appliedText.clear();
    updateState();
}

void ReorderDock::copyPatch()
{
    if (!m_result || !m_result->ok || !m_result->changed)
    {
        return;
    }
    QGuiApplication::clipboard()->setText(
        QString::fromStdString(reorder::makeUnifiedPatch(*m_result, "program.s")));
    if (m_window && m_window->statusBar())
    {
        m_window->statusBar()->showMessage(tr("Reordering patch copied to the clipboard"), 4000);
    }
}

void ReorderDock::replaceEditorText(const QString &text, const std::vector<int> &lineMap)
{
    QPlainTextEdit *edit = editor();
    if (!edit)
    {
        return;
    }
    const int cursorLine = edit->textCursor().blockNumber() + 1;
    const int scrollValue = edit->verticalScrollBar()->value();
    auto *kitesEditor = dynamic_cast<Editor *>(edit);
    std::vector<uint64_t> breakpoints;
    if (kitesEditor)
    {
        breakpoints = kitesEditor->getBreakpoints();
    }

    m_ignoreTextChange = true;
    QTextCursor cursor(edit->document());
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.insertText(text);
    cursor.endEditBlock();
    m_ignoreTextChange = false;

    // Breakpoints follow their instructions.
    if (kitesEditor)
    {
        std::vector<uint64_t> moved;
        for (uint64_t breakpoint : breakpoints)
        {
            if (breakpoint >= 1 && breakpoint <= lineMap.size() && lineMap[breakpoint - 1] > 0)
            {
                moved.push_back(static_cast<uint64_t>(lineMap[breakpoint - 1]));
            }
        }
        std::sort(moved.begin(), moved.end());
        moved.erase(std::unique(moved.begin(), moved.end()), moved.end());
        kitesEditor->setBreakpoints(moved);
    }

    int targetLine = cursorLine;
    if (cursorLine >= 1 && cursorLine <= static_cast<int>(lineMap.size()) && lineMap[cursorLine - 1] > 0)
    {
        targetLine = lineMap[cursorLine - 1];
    }
    const QTextBlock block = edit->document()->findBlockByNumber(std::max(0, targetLine - 1));
    if (block.isValid())
    {
        edit->setTextCursor(QTextCursor(block));
    }
    edit->verticalScrollBar()->setValue(scrollValue);
}

void ReorderDock::jumpToLine(int line, int sourceLine, bool newSide)
{
    QPlainTextEdit *edit = editor();
    if (!edit || line <= 0 || !m_result)
    {
        return;
    }
    const QString text = edit->toPlainText();
    int target = -1;
    if (text == m_analyzedText)
    {
        target = newSide ? sourceLine : line; // the editor still shows the original code
    }
    else if (!m_appliedText.isEmpty() && text == m_appliedText)
    {
        if (newSide)
        {
            target = line;
        }
        else if (line <= static_cast<int>(m_result->lineMap.size()))
        {
            target = m_result->lineMap[static_cast<size_t>(line - 1)];
        }
    }
    if (target <= 0)
    {
        return;
    }
    const QTextBlock block = edit->document()->findBlockByNumber(target - 1);
    if (block.isValid())
    {
        edit->setTextCursor(QTextCursor(block));
        edit->centerCursor();
    }
}

QString ReorderDock::wrapDetails(const QString &html) const
{
    return QStringLiteral("<div style=\"margin: 2px 4px;\">") + html + QStringLiteral("</div>");
}

void ReorderDock::showDetails(const QString &html)
{
    if (!html.isEmpty())
    {
        m_details->setHtml(wrapDetails(html));
        return;
    }
    if (!m_result)
    {
        m_details->setHtml(wrapDetails(
            tr("<p><b>Instruction reordering</b></p><p>Press <b>Analyze</b> to look for an order of your "
               "instructions with fewer pipeline stalls. Every change is explained: hover over a row of the diff "
               "to see why that line moved.</p>")));
        return;
    }
    if (!m_result->ok)
    {
        m_details->setHtml(wrapDetails(QStringLiteral("<p><b>%1</b></p><p>%2</p>")
                                           .arg(tr("The analysis could not run"),
                                                QString::fromStdString(m_result->error).toHtmlEscaped())));
        return;
    }
    m_details->setHtml(wrapDetails(QString::fromStdString(m_result->overviewHtml)));
}

namespace
{
constexpr unsigned kMeasureCycles = 2000000; ///< a run that takes longer is not counted
constexpr unsigned kCyclesPerTick = 20000;   ///< keeps the editor responsive while it runs
} // namespace

// Runs both programs on the selected pipeline. With a hazard unit the original is run as
// written and the stall cycles are counted; without one the NOPs are counted, and the
// original gets its NOPs first (the order kept, window of 1), as a program without them
// is not correct on that pipeline.
void ReorderDock::startMeasuring()
{
    m_measureTimer->stop();
    m_runBefore.reset();
    m_runAfter.reset();
    m_measuredSummary.clear();
    if (!m_result || !m_result->ok)
    {
        return;
    }
    const reorder::PipelineVariant variant = m_result->options.variant;
    std::string before = m_result->originalText;
    if (!reorder::hasHazardUnit(variant))
    {
        reorder::AnalysisOptions nopsOnly = m_result->options;
        nopsOnly.windowSize = 1;
        before = reorder::analyzeProgram(before, nopsOnly).newText;
    }
    m_runBefore = std::make_unique<reorder::ProfiledRun>(before, variant, kMeasureCycles);
    m_runAfter = std::make_unique<reorder::ProfiledRun>(m_result->newText, variant, kMeasureCycles);
    m_measuredSummary = tr(" · measuring on Kites…");
    m_measureTimer->start();
}

void ReorderDock::stepMeasuring()
{
    const bool beforeRuns = m_runBefore && m_runBefore->step(kCyclesPerTick);
    const bool afterRuns = m_runAfter && m_runAfter->step(kCyclesPerTick);
    if (beforeRuns || afterRuns)
    {
        return;
    }
    m_measureTimer->stop();
    if (!m_runBefore || !m_runAfter || !m_result)
    {
        return;
    }
    const reorder::RunProfile &before = m_runBefore->profile();
    const reorder::RunProfile &after = m_runAfter->profile();
    const bool hazardUnit = reorder::hasHazardUnit(m_result->options.variant);
    if (!before.ok || !after.ok)
    {
        m_measuredSummary = tr(" · measured on Kites: the run stopped with an error");
    }
    else if (!before.finished || !after.finished)
    {
        m_measuredSummary = tr(" · measured on Kites: does not end within %1 cycles").arg(kMeasureCycles);
    }
    else
    {
        const unsigned b = hazardUnit ? before.stalls : before.nops;
        const unsigned a = hazardUnit ? after.stalls : after.nops;
        m_measuredSummary = (hazardUnit ? tr(" · measured on Kites: <b>%1 → %2</b> stall cycles")
                                        : tr(" · measured on Kites: <b>%1 → %2</b> NOPs run"))
                                .arg(b)
                                .arg(a);
    }
    m_runBefore.reset();
    m_runAfter.reset();
    updateState();
}

void ReorderDock::updateState()
{
    QPlainTextEdit *edit = editor();
    const QString text = edit ? edit->toPlainText() : QString();
    const bool ok = m_result && m_result->ok;
    const bool upToDate = m_result && text == m_analyzedText;
    const bool applied = !m_appliedText.isEmpty() && text == m_appliedText;

    m_applyButton->setEnabled(ok && upToDate && m_result->changed);
    m_revertButton->setEnabled(applied);
    m_copyButton->setEnabled(ok && m_result->changed);

    // summary
    QString summary;
    if (ok)
    {
        const reorder::AnalysisResult &r = *m_result;
        const bool noHazardUnit = !reorder::hasHazardUnit(r.options.variant);
        if (!r.changed)
        {
            summary = noHazardUnit ? tr("No better order found · NOPs: <b>%1</b>").arg(r.nopsBefore)
                                   : tr("No better order found · stalls: <b>%1</b>").arg(r.stallsBefore);
        }
        else
        {
            summary = (noHazardUnit ? tr("NOPs <b>%1 → %2</b>").arg(r.nopsBefore).arg(r.nopsAfter)
                                    : tr("Stalls <b>%1 → %2</b>").arg(r.stallsBefore).arg(r.stallsAfter)) +
                      tr(" · cycles %1 → %2").arg(r.cyclesBefore).arg(r.cyclesAfter) +
                      (r.movedCount == 1 ? tr(" · 1 line moved") : tr(" · %1 lines moved").arg(r.movedCount));
        }
        if (noHazardUnit && r.missingNops > 0)
        {
            summary += tr(" · <span style=\"color:%1\">⚠ %2 missing in your code</span>")
                           .arg(colorHex(ThemeManager::getInstance().getEditorErrorColor()),
                                r.missingNops == 1 ? tr("1 NOP") : tr("%1 NOPs").arg(r.missingNops));
        }
    }
    else if (!m_result)
    {
        summary = tr("Not analyzed yet");
    }
    else
    {
        summary = tr("The analysis could not run");
    }
    if (ok)
    {
        summary += m_measuredSummary;
    }
    m_summaryLabel->setText(summary);

    // banner: the most important message first
    QStringList messages;
    if (m_result && !m_result->ok)
    {
        messages << QString::fromStdString(m_result->error).toHtmlEscaped();
    }
    else if (applied)
    {
        messages << tr("<b>Applied.</b> The diff shows what changed. <b>Revert</b> puts your code back "
                       "(Undo in the editor works too).");
    }
    else if (m_result && !upToDate)
    {
        messages << tr("The code changed since this analysis; it is updated in a moment.");
    }
    if (m_singleCycleProcessor && m_result)
    {
        messages << tr("The selected processor is single-cycle and never stalls; the suggestion is for the "
                       "5-stage pipeline chosen above.");
    }
    if (ok && upToDate)
    {
        for (const std::string &note : m_result->notes)
        {
            messages << QString::fromStdString(note).toHtmlEscaped();
        }
    }
    if (messages.isEmpty())
    {
        m_bannerLabel->hide();
    }
    else
    {
        const ThemeManager &theme = ThemeManager::getInstance();
        const QColor background = theme.getEditorBackgroundColor();
        const QColor accent = (m_result && !m_result->ok) ? theme.getEditorErrorColor() : QColor(56, 139, 253);
        m_bannerLabel->setStyleSheet(QStringLiteral("QLabel { background-color: %1; border-left: 3px solid %2; "
                                                    "border-radius: 3px; padding: 4px 8px; }")
                                         .arg(colorHex(mix(background, accent, 0.14)), colorHex(accent)));
        m_bannerLabel->setText(messages.join(QStringLiteral("<br>")));
        m_bannerLabel->show();
    }
    if (suggestionsView())
    {
        m_bannerLabel->hide(); // its messages describe the diff; the other views show their own
    }
}

} // namespace Kites
