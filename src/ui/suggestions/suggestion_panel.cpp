#include "suggestion_panel.h"

#include "hazard_squiggles.h"
#include "pipeline_timeline.h"

#include "processor/processor_manager.h"
#include "ui/editor_tab/editortab.h"
#include "ui/editor_tab/main_editor.h"
#include "ui/mainwindow/mainwindow.h"
#include "ui/theme/font_manager.h"
#include "ui/theme/theme_manager.h"

#include <QAction>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTextBlock>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace Kites
{
namespace
{
const QColor kYellow(214, 170, 30); // squiggle: the hazard unit stalls here
const QColor kRed(232, 72, 72);     // squiggle: without a hazard unit a NOP is needed here

QToolBar *mainToolbar(QMainWindow *window)
{
    for (QToolBar *toolbar : window->findChildren<QToolBar *>())
    {
        if (toolbar->windowTitle() == QLatin1String("Main Toolbar"))
        {
            return toolbar;
        }
    }
    return nullptr;
}

} // namespace

SuggestionPanel *SuggestionPanel::install(QMainWindow *window, EditorTab *editorTab, ProcessorManager *processorManager)
{
    auto *panel = new SuggestionPanel(window, editorTab, processorManager);
    window->addDockWidget(Qt::BottomDockWidgetArea, panel);
    panel->hide();

    QToolBar *toolbar = mainToolbar(window);
    auto *analyzeAction = new QAction(tr("Analyze"), window);
    analyzeAction->setToolTip(tr("Look for instructions that can be moved to remove pipeline stalls"));
    if (toolbar)
    {
        toolbar->addAction(analyzeAction);
        // The Run button: if there are suggestions we first show a banner (see eventFilter).
        for (QAction *action : toolbar->actions())
        {
            if (action->text() == QLatin1String("Run"))
            {
                panel->m_runAction = action;
                panel->m_runButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(action));
                if (panel->m_runButton)
                {
                    panel->m_runButton->installEventFilter(panel);
                }
            }
        }
    }
    connect(analyzeAction, &QAction::triggered, panel, &SuggestionPanel::analyzeAndShow);
    if (auto *mainWindow = qobject_cast<MainWindow *>(window))
    {
        connect(mainWindow, &MainWindow::processorChangedSignal, panel,
                [panel]()
                {
                    if (panel->m_analyzed && panel->isVisible())
                    {
                        panel->analyze();
                    }
                });
    }
    return panel;
}

SuggestionPanel::SuggestionPanel(QMainWindow *window, EditorTab *editorTab, ProcessorManager *processorManager)
    : QDockWidget(tr("Suggestions"), window), m_window(window), m_editorTab(editorTab),
      m_processorManager(processorManager)
{
    setObjectName(QStringLiteral("suggestionPanel"));
    setAllowedAreas(Qt::BottomDockWidgetArea | Qt::LeftDockWidgetArea);

    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 6, 10, 8);
    layout->setSpacing(6);

    // header: which suggestion, and navigation
    auto *header = new QHBoxLayout();
    m_titleLabel = new QLabel(content);
    QFont titleFont = m_titleLabel->font();
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    m_modeLabel = new QLabel(content);
    m_previousButton = new QPushButton(tr("Previous"), content);
    m_nextButton = new QPushButton(tr("Next"), content);
    header->addWidget(m_titleLabel);
    header->addSpacing(12);
    header->addWidget(m_modeLabel);
    header->addStretch(1);
    header->addWidget(m_previousButton);
    header->addWidget(m_nextButton);
    layout->addLayout(header);

    // Moved / Why / Effect, then the timeline
    m_details = new QWidget(content);
    auto *details = new QVBoxLayout(m_details);
    details->setContentsMargins(0, 0, 0, 0);
    details->setSpacing(4);
    m_movedLabel = new QLabel(m_details);
    m_movedLabel->setTextFormat(Qt::RichText);
    m_codeLabel = new QLabel(m_details); // styled as a code block in showSuggestion()
    m_codeLabel->setFont(FontManager::getInstance().currentFont());
    m_codeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_whyLabel = new QLabel(m_details);
    m_whyLabel->setTextFormat(Qt::RichText);
    m_whyLabel->setWordWrap(true);
    m_effectLabel = new QLabel(m_details);
    m_effectLabel->setTextFormat(Qt::RichText);
    details->addWidget(m_movedLabel);
    details->addWidget(m_codeLabel, 0, Qt::AlignLeft);
    details->addWidget(m_whyLabel);
    details->addWidget(m_effectLabel);
    m_timeline = new PipelineTimeline(m_details);
    auto *scroll = new QScrollArea(m_details);
    scroll->setWidget(m_timeline);
    scroll->setWidgetResizable(false);
    scroll->setFrameShape(QFrame::NoFrame);
    details->addWidget(scroll, 1);
    layout->addWidget(m_details, 1);

    // footer: Apply / Dismiss and a note
    auto *footer = new QHBoxLayout();
    m_applyButton = new QPushButton(tr("Apply"), content);
    m_applyButton->setToolTip(tr("Rewrite this block in the editor (one undo step)"));
    m_dismissButton = new QPushButton(tr("Dismiss"), content);
    m_dismissButton->setToolTip(tr("Hide this suggestion until the code changes"));
    m_noteLabel = new QLabel(content);
    m_noteLabel->setWordWrap(true);
    footer->addWidget(m_applyButton);
    footer->addWidget(m_dismissButton);
    footer->addSpacing(8);
    footer->addWidget(m_noteLabel, 1);
    layout->addLayout(footer);
    setWidget(content);

    m_reanalyzeTimer = new QTimer(this);
    m_reanalyzeTimer->setSingleShot(true);
    m_reanalyzeTimer->setInterval(700);
    connect(m_reanalyzeTimer, &QTimer::timeout, this, &SuggestionPanel::analyze);

    connect(m_previousButton, &QPushButton::clicked, this, [this]() { showSuggestion(m_current - 1); });
    connect(m_nextButton, &QPushButton::clicked, this, [this]() { showSuggestion(m_current + 1); });
    connect(m_applyButton, &QPushButton::clicked, this, &SuggestionPanel::apply);
    connect(m_dismissButton, &QPushButton::clicked, this, &SuggestionPanel::dismiss);

    if (QPlainTextEdit *edit = editor())
    {
        m_squiggles = new HazardSquiggles(edit);
        connect(edit, &QPlainTextEdit::textChanged, this, &SuggestionPanel::onTextChanged);
    }

    // The banner shown when Run is pressed while there are suggestions.
    m_runBanner = new QFrame(m_editorTab);
    m_runBanner->setObjectName(QStringLiteral("runBanner"));
    auto *bannerLayout = new QHBoxLayout(m_runBanner);
    bannerLayout->setContentsMargins(10, 4, 6, 4);
    m_runBannerLabel = new QLabel(m_runBanner);
    m_runBannerLabel->setWordWrap(true);
    auto *reviewButton = new QPushButton(tr("Review suggestions"), m_runBanner);
    auto *runAnywayButton = new QPushButton(tr("Run anyway"), m_runBanner);
    bannerLayout->addWidget(m_runBannerLabel, 1);
    bannerLayout->addWidget(reviewButton);
    bannerLayout->addWidget(runAnywayButton);
    m_runBanner->hide();
    connect(reviewButton, &QPushButton::clicked, this,
            [this]()
            {
                hideRunBanner();
                analyzeAndShow();
            });
    connect(runAnywayButton, &QPushButton::clicked, this,
            [this]()
            {
                hideRunBanner();
                if (m_runAction)
                {
                    m_runAction->trigger(); // the normal Run, as if the banner had not been there
                }
            });
    if (m_editorTab)
    {
        m_editorTab->installEventFilter(this); // keep the banner at the top when the tab resizes
    }

    showSuggestion(0);
}

QPlainTextEdit *SuggestionPanel::editor() const
{
    return m_editorTab ? m_editorTab->findChild<QPlainTextEdit *>(QStringLiteral("assemblyTextEdit")) : nullptr;
}

bool SuggestionPanel::pipelineProcessor() const
{
    return m_processorManager && m_processorManager->getProcessorType() != ProcessorType::RVSS;
}

scheduler::Options SuggestionPanel::currentOptions() const
{
    scheduler::Options options;
    switch (m_processorManager ? m_processorManager->getProcessorType() : ProcessorType::RVSS)
    {
    case ProcessorType::RV5Stage_H_NF:
        options.forwarding = false;
        break;
    case ProcessorType::RV5Stage_NH_F:
        options.hazardUnit = false;
        break;
    case ProcessorType::RV5Stage_NH_NF:
        options.forwarding = false;
        options.hazardUnit = false;
        break;
    default: // H_F, and the single-cycle processor (shown for the hazard-unit + forwarding pipeline)
        break;
    }
    return options;
}

void SuggestionPanel::analyzeAndShow()
{
    const bool wasHidden = isHidden();
    show();
    raise();
    if (wasHidden && !isFloating() && m_window)
    {
        m_window->resizeDocks({this}, {m_window->height() * 9 / 20}, Qt::Vertical); // room for the timeline
    }
    analyze();
}

void SuggestionPanel::analyze()
{
    m_reanalyzeTimer->stop();
    QPlainTextEdit *edit = editor();
    if (!edit)
    {
        return;
    }
    m_analyzedText = edit->toPlainText();
    m_result = scheduler::analyze(m_analyzedText.toStdString(), currentOptions());
    m_analyzed = true;
    showSuggestion(std::min(m_current, std::max(0, static_cast<int>(visibleSuggestions().size()) - 1)));
    updateSquiggles();
}

std::vector<int> SuggestionPanel::visibleSuggestions() const
{
    std::vector<int> visible;
    for (size_t i = 0; i < m_result.suggestions.size(); ++i)
    {
        if (!m_dismissed.contains(suggestionKey(m_result.suggestions[i])))
        {
            visible.push_back(static_cast<int>(i));
        }
    }
    return visible;
}

QString SuggestionPanel::suggestionKey(const scheduler::Suggestion &suggestion) const
{
    return QString::fromStdString(suggestion.why);
}

void SuggestionPanel::showSuggestion(int index)
{
    const scheduler::Options options = m_result.options;
    m_modeLabel->setText(m_analyzed ? tr("Pipeline: hazard unit %1, forwarding %2%3")
                                          .arg(options.hazardUnit ? tr("on") : tr("off"),
                                               options.forwarding ? tr("on") : tr("off"),
                                               pipelineProcessor() ? QString() : tr(" (the selected processor is single-cycle)"))
                                    : QString());
    const std::vector<int> visible = visibleSuggestions();
    const bool any = !visible.empty();
    m_details->setVisible(any);
    m_applyButton->setEnabled(any);
    m_dismissButton->setEnabled(any);
    m_previousButton->setEnabled(any && index > 0);
    m_nextButton->setEnabled(any && index + 1 < static_cast<int>(visible.size()));
    if (!any)
    {
        m_current = 0;
        m_timeline->clear();
        if (!m_analyzed)
        {
            m_titleLabel->setText(tr("Press Analyze to look for instructions that can be moved."));
        }
        else if (!m_result.ok)
        {
            m_titleLabel->setText(QString::fromStdString(m_result.error));
        }
        else
        {
            m_titleLabel->setText(tr("No suggestions: no stall in this code can be removed by reordering."));
        }
        m_noteLabel->clear();
        return;
    }

    m_current = std::clamp(index, 0, static_cast<int>(visible.size()) - 1);
    const scheduler::Suggestion &s = m_result.suggestions[visible[m_current]];
    const scheduler::BlockChange &block = m_result.blocks[s.block];
    m_titleLabel->setText(tr("Suggestion %1 of %2").arg(m_current + 1).arg(visible.size()));
    m_movedLabel->setText(tr("<b>Moved</b> (line %1)").arg(s.movedLine));
    const ThemeManager &theme = ThemeManager::getInstance();
    m_codeLabel->setStyleSheet(QStringLiteral("QLabel { background-color: %1; color: %2; border: 1px solid %3; "
                                              "border-radius: 3px; padding: 4px 10px; }")
                                   .arg(theme.getEditorBackgroundColor().name(), theme.getEditorForegroundColor().name(),
                                        theme.getEditorMutedForegroundColor().name()));
    m_codeLabel->setText(QString::fromStdString(s.movedText));
    m_whyLabel->setText(tr("<b>Why:</b> %1").arg(QString::fromStdString(s.why).toHtmlEscaped()));
    m_effectLabel->setText(tr("<b>Effect:</b> %1").arg(QString::fromStdString(s.effect)));
    m_timeline->setBlock(block.before, block.after, options.hazardUnit, s.movedLine);

    int inBlock = 0;
    for (int i : visible)
    {
        inBlock += m_result.suggestions[i].block == s.block ? 1 : 0;
    }
    const int firstInstructionLine = block.before.empty() ? block.firstLine : block.before.front().line;
    QString note = tr("Apply rewrites lines %1–%2 (this block) as one undo step").arg(firstInstructionLine).arg(block.lastLine);
    if (inBlock > 1)
    {
        note += inBlock == 2 ? tr(", together with the other suggestion in this block")
                             : tr(", together with the other %1 suggestions in this block").arg(inBlock - 1);
    }
    note += tr(". NOPs are written only where an instruction still waits, so the block also runs without a hazard unit.");
    m_noteLabel->setText(note);

    // show the waiting line in the editor
    if (QPlainTextEdit *edit = editor())
    {
        const QTextBlock line = edit->document()->findBlockByNumber(s.consumerLine - 1);
        if (line.isValid())
        {
            edit->setTextCursor(QTextCursor(line));
            edit->ensureCursorVisible();
        }
    }
}

void SuggestionPanel::updateSquiggles()
{
    if (!m_squiggles)
    {
        return;
    }
    QMap<int, QString> tooltips;
    for (int i : visibleSuggestions())
    {
        const scheduler::Suggestion &s = m_result.suggestions[i];
        QString text = tr("Moved: %1\nWhy: %2\nEffect: %3")
                           .arg(QString::fromStdString(s.movedText), QString::fromStdString(s.why),
                                QString::fromStdString(s.effect));
        if (tooltips.contains(s.consumerLine))
        {
            text = tooltips.value(s.consumerLine) + QStringLiteral("\n\n") + text;
        }
        tooltips.insert(s.consumerLine, text);
    }
    m_squiggles->setLines(tooltips, m_result.options.hazardUnit ? kYellow : kRed);
}

void SuggestionPanel::apply()
{
    QPlainTextEdit *edit = editor();
    const std::vector<int> visible = visibleSuggestions();
    if (!edit || visible.empty())
    {
        return;
    }
    if (edit->isReadOnly())
    {
        QMessageBox::information(this, windowTitle(), tr("Stop the running program before changing the code."));
        return;
    }
    if (edit->toPlainText() != m_analyzedText)
    {
        analyze(); // the code changed since the analysis
        return;
    }
    const scheduler::BlockChange block = m_result.blocks[m_result.suggestions[visible[m_current]].block];

    // breakpoints: lines after the block shift; inside it they follow their line's text
    std::vector<uint64_t> breakpoints;
    auto *kitesEditor = dynamic_cast<Editor *>(edit);
    if (kitesEditor)
    {
        const int delta = static_cast<int>(block.newLines.size()) - (block.lastLine - block.firstLine + 1);
        std::vector<bool> used(block.newLines.size(), false);
        for (uint64_t line : kitesEditor->getBreakpoints())
        {
            const int l = static_cast<int>(line);
            if (l < block.firstLine)
            {
                breakpoints.push_back(line);
            }
            else if (l > block.lastLine)
            {
                breakpoints.push_back(static_cast<uint64_t>(l + delta));
            }
            else
            {
                const QString old = edit->document()->findBlockByNumber(l - 1).text().trimmed();
                for (size_t k = 0; k < block.newLines.size(); ++k)
                {
                    if (!used[k] && QString::fromStdString(block.newLines[k]).trimmed() == old)
                    {
                        used[k] = true;
                        breakpoints.push_back(static_cast<uint64_t>(block.firstLine + static_cast<int>(k)));
                        break;
                    }
                }
            }
        }
    }

    // replace lines firstLine..lastLine as one undo step
    QTextDocument *document = edit->document();
    const QTextBlock first = document->findBlockByNumber(block.firstLine - 1);
    const QTextBlock last = document->findBlockByNumber(block.lastLine - 1);
    if (!first.isValid() || !last.isValid())
    {
        return;
    }
    QStringList lines;
    for (const std::string &line : block.newLines)
    {
        lines << QString::fromStdString(line);
    }
    m_applying = true;
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    cursor.setPosition(first.position());
    cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    cursor.insertText(lines.join(QLatin1Char('\n')));
    cursor.endEditBlock();
    m_applying = false;
    if (kitesEditor)
    {
        std::sort(breakpoints.begin(), breakpoints.end());
        kitesEditor->setBreakpoints(breakpoints);
    }
    analyze();
}

void SuggestionPanel::dismiss()
{
    const std::vector<int> visible = visibleSuggestions();
    if (visible.empty())
    {
        return;
    }
    m_dismissed.insert(suggestionKey(m_result.suggestions[visible[m_current]]));
    showSuggestion(m_current);
    updateSquiggles();
}

void SuggestionPanel::onTextChanged()
{
    if (m_applying)
    {
        return;
    }
    m_dismissed.clear(); // "until the code changes"
    if (m_squiggles)
    {
        m_squiggles->clearLines(); // the old lines no longer match the text
    }
    hideRunBanner();
    if (m_analyzed && isVisible())
    {
        m_reanalyzeTimer->start();
    }
    else
    {
        m_analyzed = false;
    }
}

// ---- Run banner --------------------------------------------------------------------

void SuggestionPanel::guardRun()
{
    analyze();
    int saved = 0;
    const std::vector<int> visible = visibleSuggestions();
    if (visible.empty())
    {
        if (m_runAction)
        {
            m_runAction->trigger(); // all suggestions were dismissed: just run
        }
        return;
    }
    for (int i : visible)
    {
        saved += m_result.suggestions[i].saved;
    }
    showRunBanner(static_cast<int>(visible.size()), saved);
}

void SuggestionPanel::showRunBanner(int suggestions, int saved)
{
    const bool hazardUnit = m_result.options.hazardUnit;
    const QString what = suggestions == 1 ? tr("1 suggestion") : tr("%1 suggestions").arg(suggestions);
    const QString unit = hazardUnit ? (saved == 1 ? tr("stall cycle") : tr("stall cycles"))
                                    : (saved == 1 ? tr("NOP") : tr("NOPs"));
    m_runBannerLabel->setText(tr("%1 could save %2 %3 in this code.").arg(what).arg(saved).arg(unit));
    const QColor background = ThemeManager::getInstance().getEditorBackgroundColor();
    const QColor accent = hazardUnit ? kYellow : kRed;
    const QColor fill = QColor::fromRgbF(static_cast<float>(background.redF() * 0.8 + accent.redF() * 0.2),
                                         static_cast<float>(background.greenF() * 0.8 + accent.greenF() * 0.2),
                                         static_cast<float>(background.blueF() * 0.8 + accent.blueF() * 0.2));
    // the Kites stylesheet gives every widget a background; the label must show the banner's colour
    m_runBanner->setStyleSheet(QStringLiteral("QFrame#runBanner { background-color: %1; border: 1px solid %2; "
                                              "border-radius: 4px; } QFrame#runBanner QLabel { background: transparent; }")
                                   .arg(fill.name(), accent.name()));
    placeRunBanner();
    m_runBanner->show();
    m_runBanner->raise();
}

void SuggestionPanel::hideRunBanner()
{
    if (m_runBanner)
    {
        m_runBanner->hide();
    }
}

void SuggestionPanel::placeRunBanner()
{
    if (!m_runBanner || !m_editorTab)
    {
        return;
    }
    // at the bottom of the editor tab, where it covers the least code
    const int height = std::max(40, m_runBanner->sizeHint().height());
    m_runBanner->setGeometry(8, m_editorTab->height() - height - 8, std::max(200, m_editorTab->width() - 16), height);
}

bool SuggestionPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_editorTab && event->type() == QEvent::Resize)
    {
        placeRunBanner();
    }
    // Run pressed: if the code has suggestions, show the banner instead of running.
    if (watched == m_runButton && event->type() == QEvent::MouseButtonRelease && m_runAction &&
        m_runAction->text() == QLatin1String("Run") && m_runAction->isEnabled() && pipelineProcessor())
    {
        auto *mouse = static_cast<QMouseEvent *>(event);
        QPlainTextEdit *edit = editor();
        if (mouse->button() == Qt::LeftButton && m_runButton->rect().contains(mouse->position().toPoint()) && edit &&
            !edit->isReadOnly())
        {
            const scheduler::Result result = scheduler::analyze(edit->toPlainText().toStdString(), currentOptions());
            if (!result.suggestions.empty())
            {
                m_runButton->setDown(false); // we take the click: release the button
                guardRun();
                return true;
            }
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

} // namespace Kites
