#include "pipeline_timeline.h"

#include "ui/theme/font_manager.h"
#include "ui/theme/theme_manager.h"

#include <QPaintEvent>
#include <QPainter>
#include <algorithm>

namespace Kites
{
namespace
{
const char *const kStages[5] = {"IF", "ID", "EX", "MEM", "WB"};
const QColor kStall(230, 180, 40);  // hazard unit on: a stall
const QColor kNop(232, 72, 72);     // hazard unit off: a NOP is needed
const QColor kMoved(46, 160, 67);

QColor mixed(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(static_cast<float>(a.redF() * (1 - t) + b.redF() * t),
                            static_cast<float>(a.greenF() * (1 - t) + b.greenF() * t),
                            static_cast<float>(a.blueF() * (1 - t) + b.blueF() * t));
}
} // namespace

PipelineTimeline::PipelineTimeline(QWidget *parent) : QWidget(parent)
{
    setFont(FontManager::getInstance().currentFont());
    connect(&ThemeManager::getInstance(), &ThemeManager::editorThemeChangedSignal, this,
            [this](const QString &) { update(); });
}

void PipelineTimeline::setBlock(const std::vector<scheduler::TimelineRow> &before,
                                const std::vector<scheduler::TimelineRow> &after, bool hazardUnit, int highlightLine)
{
    m_before = before;
    m_after = after;
    m_hazardUnit = hazardUnit;
    m_highlightLine = highlightLine;
    const QFontMetrics metrics(font());
    m_rowHeight = metrics.height() + 6;
    m_cellWidth = std::max({metrics.horizontalAdvance(QStringLiteral("MEM")), metrics.horizontalAdvance(tr("stall")),
                            metrics.horizontalAdvance(tr("nop"))}) + 10;
    int widest = 0;
    for (const auto *rows : {&m_before, &m_after})
    {
        for (const scheduler::TimelineRow &row : *rows)
        {
            widest = std::max(widest, metrics.horizontalAdvance(QStringLiteral("%1  %2").arg(row.line).arg(
                                          QString::fromStdString(row.text))));
        }
    }
    m_labelWidth = std::min(widest + 16, 340);
    setMinimumSize(sizeHint());
    updateGeometry();
    update();
}

void PipelineTimeline::clear()
{
    setBlock({}, {}, m_hazardUnit, -1);
}

int PipelineTimeline::cycles() const
{
    int last = 0;
    for (const auto *rows : {&m_before, &m_after})
    {
        for (const scheduler::TimelineRow &row : *rows)
        {
            // IF, the waiting cycles, then ID EX MEM WB; a pseudo-instruction takes extra slots
            last = std::max(last, row.fetchCycle + row.stalls + 5 + row.instructions - 1);
        }
    }
    return last;
}

QSize PipelineTimeline::sizeHint() const
{
    const int rows = static_cast<int>(m_before.size() + m_after.size()) + 4; // two titles, two cycle rulers
    return QSize(m_labelWidth + cycles() * m_cellWidth + 8, rows * m_rowHeight + 16);
}

void PipelineTimeline::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    const ThemeManager &theme = ThemeManager::getInstance();
    painter.fillRect(event->rect(), theme.getEditorBackgroundColor());
    if (m_before.empty())
    {
        return;
    }
    painter.setFont(font());
    int y = 4;
    y = paintTimeline(painter, y, tr("Before"), m_before, false);
    y += 8;
    paintTimeline(painter, y, tr("After"), m_after, true);
}

int PipelineTimeline::paintTimeline(QPainter &painter, int y, const QString &title,
                                    const std::vector<scheduler::TimelineRow> &rows, bool after)
{
    const ThemeManager &theme = ThemeManager::getInstance();
    const QColor background = theme.getEditorBackgroundColor();
    const QColor foreground = theme.getEditorForegroundColor();
    const QColor muted = theme.getEditorMutedForegroundColor();
    const QColor waitColor = m_hazardUnit ? kStall : kNop;
    const QFontMetrics metrics(font());

    // title and cycle numbers
    QFont bold = font();
    bold.setBold(true);
    painter.setFont(bold);
    painter.setPen(foreground);
    painter.drawText(QRect(4, y, m_labelWidth, m_rowHeight), Qt::AlignVCenter | Qt::AlignLeft, title);
    painter.setFont(font());
    painter.setPen(muted);
    const int total = cycles();
    for (int c = 0; c < total; ++c)
    {
        painter.drawText(QRect(m_labelWidth + c * m_cellWidth, y, m_cellWidth, m_rowHeight), Qt::AlignCenter,
                         QString::number(c + 1));
    }
    y += m_rowHeight;

    for (const scheduler::TimelineRow &row : rows)
    {
        const QRect rowRect(0, y, width(), m_rowHeight);
        const bool highlight = after && row.line == m_highlightLine;
        if (highlight)
        {
            painter.fillRect(rowRect, mixed(background, kMoved, 0.22));
        }
        else if (after && row.moved)
        {
            painter.fillRect(rowRect, mixed(background, kMoved, 0.08));
        }

        // label: line number and instruction
        QString label = QStringLiteral("%1  %2").arg(row.line).arg(QString::fromStdString(row.text));
        if (row.instructions > 1)
        {
            label += tr("  (%1 instr.)").arg(row.instructions);
        }
        painter.setPen(foreground);
        painter.drawText(QRect(6, y, m_labelWidth - 10, m_rowHeight), Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(label, Qt::ElideRight, m_labelWidth - 10));

        // stages: IF, waiting cycles, ID EX MEM WB
        int cycle = row.fetchCycle;
        auto cell = [&](const QString &text, const QColor &fill) {
            const QRect box(m_labelWidth + cycle * m_cellWidth + 1, y + 2, m_cellWidth - 2, m_rowHeight - 4);
            painter.fillRect(box, fill);
            painter.setPen(foreground);
            painter.drawText(box, Qt::AlignCenter, text);
            ++cycle;
        };
        const QColor stageFill = mixed(background, foreground, 0.12);
        cell(QString::fromLatin1(kStages[0]), stageFill);
        for (int s = 0; s < row.stalls; ++s)
        {
            cell(m_hazardUnit ? tr("stall") : tr("nop"), mixed(background, waitColor, 0.55));
        }
        for (int s = 1; s < 5; ++s)
        {
            cell(QString::fromLatin1(kStages[s]), stageFill);
        }

        if (highlight || (after && row.fillsStall))
        {
            painter.setPen(kMoved.darker(background.lightness() < 128 ? 70 : 130));
            painter.drawText(QRect(m_labelWidth + cycle * m_cellWidth + 6, y, 120, m_rowHeight),
                             Qt::AlignVCenter | Qt::AlignLeft, tr("← fills stall"));
        }
        y += m_rowHeight;
    }
    return y;
}

} // namespace Kites
