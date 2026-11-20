#include "pipeline_timeline.h"

#include "ui/theme/font_manager.h"
#include "ui/theme/theme_manager.h"

#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <algorithm>

namespace Kites
{
namespace
{
const char *const kStages[5] = {"IF", "ID", "EX", "MEM", "WB"};
const QColor kStall(230, 180, 40);  // hazard unit on: a stall
const QColor kNop(232, 72, 72);     // hazard unit off: a NOP is needed
const QColor kMoved(46, 160, 67);
const int kGap = 24; // between the Before and After columns

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

void PipelineTimeline::setBlock(const std::vector<suggestions::TimelineRow> &before,
                                const std::vector<suggestions::TimelineRow> &after, bool hazardUnit, int highlightLine)
{
    m_before = before;
    m_after = after;
    m_hazardUnit = hazardUnit;
    m_highlightLine = highlightLine;
    const QFontMetrics metrics(font());
    m_rowHeight = metrics.height() + 6;
    m_cellWidth = std::max({metrics.horizontalAdvance(QStringLiteral("MEM")), metrics.horizontalAdvance(tr("stall")),
                            metrics.horizontalAdvance(tr("nop"))}) + 6;
    m_noteWidth = metrics.horizontalAdvance(tr("← fills stall")) + 12;
    int widest = 0;
    for (const auto *rows : {&m_before, &m_after})
    {
        for (const suggestions::TimelineRow &row : *rows)
        {
            widest = std::max(widest, metrics.horizontalAdvance(QStringLiteral("%1  %2").arg(row.line).arg(
                                          QString::fromStdString(row.text))));
        }
    }
    m_labelWidth = std::min(widest + 16, 340);
    setMinimumWidth(columnWidth(true)); // narrower than one timeline: the scroll area scrolls
    updateMinimumHeight();
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
        for (const suggestions::TimelineRow &row : *rows)
        {
            // IF, the waiting cycles, then ID EX MEM WB; a pseudo-instruction takes extra slots
            last = std::max(last, row.fetchCycle + row.stalls + 5 + row.instructions - 1);
        }
    }
    return last;
}

int PipelineTimeline::columnWidth(bool after) const
{
    return m_labelWidth + cycles() * m_cellWidth + (after ? m_noteWidth : 8); // only After has "fills stall"
}

int PipelineTimeline::sideBySideHeight() const
{
    const int rows = static_cast<int>(std::max(m_before.size(), m_after.size())) + 1; // + title row
    return rows * m_rowHeight + 8;
}

int PipelineTimeline::stackedHeight() const
{
    const int rows = static_cast<int>(m_before.size() + m_after.size()) + 2; // + two title rows
    return rows * m_rowHeight + 16;
}

bool PipelineTimeline::sideBySide() const
{
    return width() >= columnWidth(false) + kGap + columnWidth(true);
}

void PipelineTimeline::updateMinimumHeight()
{
    setMinimumHeight(m_before.empty() ? 0 : (sideBySide() ? sideBySideHeight() : stackedHeight()));
}

QSize PipelineTimeline::sizeHint() const
{
    return QSize(columnWidth(false) + kGap + columnWidth(true), sideBySideHeight());
}

QSize PipelineTimeline::minimumSizeHint() const
{
    return QSize(columnWidth(true), sideBySideHeight());
}

void PipelineTimeline::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateMinimumHeight(); // side by side needs fewer rows than one below the other
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
    if (sideBySide())
    {
        // Before on the left, After from the middle (or right after Before, if that is further right)
        const int afterX = std::max(columnWidth(false) + kGap, width() / 2);
        paintTimeline(painter, 0, 4, afterX - kGap, tr("Before"), m_before, false);
        paintTimeline(painter, afterX, 4, width() - afterX, tr("After"), m_after, true);
    }
    else
    {
        const int y = paintTimeline(painter, 0, 4, width(), tr("Before"), m_before, false);
        paintTimeline(painter, 0, y + 8, width(), tr("After"), m_after, true);
    }
}

int PipelineTimeline::paintTimeline(QPainter &painter, int x, int y, int width, const QString &title,
                                    const std::vector<suggestions::TimelineRow> &rows, bool after)
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
    painter.drawText(QRect(x + 4, y, m_labelWidth, m_rowHeight), Qt::AlignVCenter | Qt::AlignLeft, title);
    painter.setFont(font());
    painter.setPen(muted);
    const int total = cycles();
    for (int c = 0; c < total; ++c)
    {
        painter.drawText(QRect(x + m_labelWidth + c * m_cellWidth, y, m_cellWidth, m_rowHeight), Qt::AlignCenter,
                         QString::number(c + 1));
    }
    y += m_rowHeight;

    for (const suggestions::TimelineRow &row : rows)
    {
        const QRect rowRect(x, y, width, m_rowHeight);
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
        painter.drawText(QRect(x + 6, y, m_labelWidth - 10, m_rowHeight), Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(label, Qt::ElideRight, m_labelWidth - 10));

        // stages: IF, waiting cycles, ID EX MEM WB
        int cycle = row.fetchCycle;
        auto cell = [&](const QString &text, const QColor &fill) {
            const QRect box(x + m_labelWidth + cycle * m_cellWidth + 1, y + 2, m_cellWidth - 2, m_rowHeight - 4);
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
            painter.drawText(QRect(x + m_labelWidth + cycle * m_cellWidth + 6, y, m_noteWidth - 6, m_rowHeight),
                             Qt::AlignVCenter | Qt::AlignLeft, tr("← fills stall"));
        }
        y += m_rowHeight;
    }
    return y;
}

} // namespace Kites
