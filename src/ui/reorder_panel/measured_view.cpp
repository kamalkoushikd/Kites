#include "measured_view.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <map>

namespace Kites
{

namespace
{

constexpr unsigned kMaxCycles = 2000000; ///< a run that takes longer is stopped (endless loop)
constexpr unsigned kCyclesPerTick = 20000;

QStringList splitLines(const std::string &text)
{
    return QString::fromStdString(text).split(QLatin1Char('\n'));
}

/// A table cell that sorts by number.
QTableWidgetItem *numberItem(long long value)
{
    auto *item = new QTableWidgetItem();
    item->setData(Qt::DisplayRole, value);
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

QTableWidgetItem *textItem(const QString &text)
{
    return new QTableWidgetItem(text);
}

QString change(long long before, long long after)
{
    const long long difference = after - before;
    if (difference == 0)
    {
        return QStringLiteral("0");
    }
    QString text = (difference > 0 ? QStringLiteral("+") : QStringLiteral("−")) + QString::number(std::llabs(difference));
    if (before > 0)
    {
        text += QStringLiteral(" (%1%)").arg(100.0 * static_cast<double>(difference) / static_cast<double>(before), 0, 'f', 1);
    }
    return text;
}

} // namespace

MeasuredView::MeasuredView(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *top = new QHBoxLayout();
    m_runButton = new QPushButton(tr("Run both"), this);
    m_runButton->setToolTip(tr("Run the original and the reordered program on the chosen pipeline, cycle by cycle, "
                               "and count the stalls they really have (loops count every time they run)."));
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    top->addWidget(m_runButton);
    top->addWidget(m_status, 1);
    layout->addLayout(top);

    m_summary = new QTableWidget(0, 4, this);
    m_summary->setHorizontalHeaderLabels({tr(""), tr("Original"), tr("Reordered"), tr("Change")});
    m_summary->verticalHeader()->hide();
    m_summary->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_summary->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_summary->setSelectionMode(QAbstractItemView::NoSelection);

    m_lines = new QTableWidget(0, 8, this);
    m_lines->setHorizontalHeaderLabels({tr("Line"), tr("Instruction"), tr("Ran"), tr("Stalls before"),
                                        tr("Stalls after"), tr("Caused before"), tr("Caused after"),
                                        tr("NOPs run before / after")});
    m_lines->verticalHeader()->hide();
    m_lines->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_lines->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_lines->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_lines->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_lines->setToolTip(tr("Stalls: cycles the line waited in ID. Caused: stall cycles of other lines that waited for "
                           "this line's result. Lines are those of the original program; the reordered program's "
                           "lines are mapped back to them."));

    layout->addWidget(m_summary);
    layout->addWidget(m_lines, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(0);
    connect(m_timer, &QTimer::timeout, this, &MeasuredView::stepRun);
    connect(m_runButton, &QPushButton::clicked, this, &MeasuredView::run);
    setAnalysis(nullptr);
}

MeasuredView::~MeasuredView() = default;

void MeasuredView::setAnalysis(std::shared_ptr<const reorder::AnalysisResult> result)
{
    m_result = std::move(result);
    m_timer->stop();
    m_running.reset();
    m_summary->setRowCount(0);
    m_lines->setRowCount(0);
    const bool ready = m_result && m_result->ok;
    m_runButton->setEnabled(ready);
    m_status->setText(ready ? tr("Press Run both to measure the stalls on %1.")
                                  .arg(QString::fromStdString(reorder::variantDescription(m_result->options.variant)))
                            : tr("Analyze a program first."));
}

void MeasuredView::run()
{
    if (!m_result || !m_result->ok)
    {
        return;
    }
    m_before = reorder::RunProfile{};
    m_after = reorder::RunProfile{};
    m_summary->setRowCount(0);
    m_lines->setRowCount(0);
    m_stage = 0;
    m_running = std::make_unique<reorder::ProfiledRun>(m_result->originalText, m_result->options.variant, kMaxCycles);
    m_runButton->setEnabled(false);
    m_status->setText(tr("Running the original program…"));
    m_timer->start();
}

void MeasuredView::stepRun()
{
    if (!m_running)
    {
        m_timer->stop();
        return;
    }
    if (m_running->step(kCyclesPerTick))
    {
        return; // more next tick
    }
    if (m_stage == 0)
    {
        m_before = m_running->profile();
        m_stage = 1;
        m_running = std::make_unique<reorder::ProfiledRun>(m_result->newText, m_result->options.variant, kMaxCycles);
        m_status->setText(tr("Running the reordered program…"));
        return;
    }
    m_after = m_running->profile();
    m_running.reset();
    m_timer->stop();
    m_runButton->setEnabled(true);
    showResults();
}

void MeasuredView::showResults()
{
    QStringList problems;
    for (const auto &[name, profile] : {std::pair<QString, const reorder::RunProfile &>{tr("original"), m_before},
                                        std::pair<QString, const reorder::RunProfile &>{tr("reordered"), m_after}})
    {
        if (!profile.error.empty())
        {
            problems << tr("The %1 program: %2").arg(name, QString::fromStdString(profile.error));
        }
        else if (!profile.finished)
        {
            problems << tr("The %1 program did not finish within %2 cycles (an endless loop or waiting for input?).")
                            .arg(name)
                            .arg(kMaxCycles);
        }
    }
    const bool hazardUnit = reorder::hasHazardUnit(m_result->options.variant);
    QString status = tr("Measured on %1.").arg(QString::fromStdString(reorder::variantDescription(m_result->options.variant)));
    if (!m_before.endReason.empty())
    {
        status += tr(" Runs ended: %1 / %2.")
                      .arg(QString::fromStdString(m_before.endReason), QString::fromStdString(m_after.endReason));
    }
    if (m_before.ok && m_after.ok)
    {
        status += m_before.registers == m_after.registers ? tr(" Both end with the same registers.")
                                                          : tr(" <b>The registers differ at the end.</b>");
        if (!hazardUnit && m_result->missingNops > 0 && m_before.registers != m_after.registers)
        {
            status += tr(" Your code misses %n NOP(s), so on this pipeline the original reads old values; the "
                         "reordered program has the NOPs it needs.",
                         nullptr, m_result->missingNops);
        }
        if (!hazardUnit)
        {
            status += tr(" Without a hazard unit the program's NOPs take the place of stalls, so NOPs run are counted.");
        }
    }
    if (!problems.isEmpty())
    {
        status += QStringLiteral("<br>") + problems.join(QStringLiteral("<br>"));
    }
    m_status->setText(status);
    fillSummary();
    fillLines();
}

void MeasuredView::fillSummary()
{
    const bool hazardUnit = reorder::hasHazardUnit(m_result->options.variant);
    struct Row
    {
        QString name;
        long long before;
        long long after;
    };
    std::vector<Row> rows = {
        {tr("Cycles"), m_before.cycles, m_after.cycles},
        {tr("Instructions run"), m_before.instructions, m_after.instructions},
        {hazardUnit ? tr("Stall cycles (measured)") : tr("NOPs run (measured)"),
         hazardUnit ? m_before.stalls : m_before.nops, hazardUnit ? m_after.stalls : m_after.nops},
        {hazardUnit ? tr("Stalls (static count)") : tr("NOPs needed (static count)"), m_result->stallsBefore,
         m_result->stallsAfter},
    };
    if (m_before.unexplained > 0 || m_after.unexplained > 0)
    {
        rows.push_back({tr("Stalls without a real dependence (Kites' hazard unit)"), m_before.unexplained,
                        m_after.unexplained});
    }
    m_summary->setRowCount(static_cast<int>(rows.size()) + 1);
    int r = 0;
    for (const Row &row : rows)
    {
        m_summary->setItem(r, 0, textItem(row.name));
        m_summary->setItem(r, 1, numberItem(row.before));
        m_summary->setItem(r, 2, numberItem(row.after));
        m_summary->setItem(r, 3, textItem(change(row.before, row.after)));
        ++r;
    }
    auto cpi = [](const reorder::RunProfile &p) {
        return p.instructions > 0 ? QString::number(static_cast<double>(p.cycles) / p.instructions, 'f', 3)
                                  : QStringLiteral("-");
    };
    m_summary->setItem(r, 0, textItem(tr("CPI")));
    m_summary->setItem(r, 1, textItem(cpi(m_before)));
    m_summary->setItem(r, 2, textItem(cpi(m_after)));
    m_summary->setItem(r, 3, textItem(QString()));

    // show every row: the table is short, the per-line table below gets the rest of the space
    m_summary->resizeRowsToContents();
    int height = m_summary->horizontalHeader()->height() + 2 * m_summary->frameWidth();
    for (int row = 0; row < m_summary->rowCount(); ++row)
    {
        height += m_summary->rowHeight(row);
    }
    m_summary->setFixedHeight(height);
}

void MeasuredView::fillLines()
{
    // the reordered program's lines, mapped back to the original lines
    const std::vector<int> &lineMap = m_result->lineMap;
    std::map<int, int> originalOfNew;
    for (size_t old = 0; old < lineMap.size(); ++old)
    {
        if (lineMap[old] > 0)
        {
            originalOfNew[lineMap[old]] = static_cast<int>(old) + 1;
        }
    }
    struct Row
    {
        QString text;
        reorder::LineRun before;
        reorder::LineRun after;
        bool isNop = false;
    };
    const QStringList oldLines = splitLines(m_result->originalText);
    const QStringList newLines = splitLines(m_result->newText);
    std::map<std::pair<int, int>, Row> rows; // key: (original line, or 0 for added lines; new line if added)
    auto textOf = [](const QStringList &lines, int line) {
        return line >= 1 && line <= lines.size() ? lines[line - 1].trimmed() : QString();
    };
    auto isNopText = [](const QString &text) { return text.section(QLatin1Char('#'), 0, 0).trimmed() == QStringLiteral("nop"); };
    for (const auto &[line, run] : m_before.lines)
    {
        Row &row = rows[{line, 0}];
        row.text = textOf(oldLines, line);
        row.before = run;
        row.isNop = isNopText(row.text);
    }
    for (const auto &[line, run] : m_after.lines)
    {
        const auto it = originalOfNew.find(line);
        const std::pair<int, int> key = it != originalOfNew.end() ? std::make_pair(it->second, 0) : std::make_pair(0, line);
        Row &row = rows[key];
        if (row.text.isEmpty())
        {
            row.text = it != originalOfNew.end() ? textOf(oldLines, it->second) : textOf(newLines, line) + tr("  (added)");
        }
        row.after = run;
        row.isNop = row.isNop || isNopText(textOf(newLines, line));
    }

    m_lines->setSortingEnabled(false);
    m_lines->setRowCount(0);
    for (const auto &[key, row] : rows)
    {
        const bool interesting = row.before.stalls || row.after.stalls || row.before.caused || row.after.caused ||
                                 (row.isNop && (row.before.executed || row.after.executed));
        if (!interesting)
        {
            continue;
        }
        const int r = m_lines->rowCount();
        m_lines->insertRow(r);
        m_lines->setItem(r, 0, key.first > 0 ? numberItem(key.first) : textItem(tr("new %1").arg(key.second)));
        QTableWidgetItem *text = textItem(row.text);
        if (row.before.unexplained > 0 || row.after.unexplained > 0)
        {
            text->setText(row.text + tr("   ⚠ %1 / %2 stall cycles without a real dependence")
                                         .arg(row.before.unexplained)
                                         .arg(row.after.unexplained));
            text->setToolTip(tr("No instruction ahead of it writes a register it reads. Kites' hazard unit reads "
                                "bits 24..20 of every instruction as rs2, also where they are part of an immediate, "
                                "so it sometimes stalls without a reason (see docs/KNOWN_UI_ISSUES.md)."));
        }
        m_lines->setItem(r, 1, text);
        m_lines->setItem(r, 2, numberItem(std::max(row.before.executed, row.after.executed)));
        m_lines->setItem(r, 3, numberItem(row.before.stalls));
        m_lines->setItem(r, 4, numberItem(row.after.stalls));
        m_lines->setItem(r, 5, numberItem(row.before.caused));
        m_lines->setItem(r, 6, numberItem(row.after.caused));
        m_lines->setItem(r, 7, textItem(row.isNop ? QStringLiteral("%1 / %2").arg(row.before.executed).arg(row.after.executed)
                                                  : QString()));
    }
    m_lines->setSortingEnabled(true);
    m_lines->sortByColumn(3, Qt::DescendingOrder);
}

} // namespace Kites
