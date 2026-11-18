#include "reorder_diff_view.h"

#include "ui/theme/theme_manager.h"

#include <QEvent>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QToolTip>
#include <algorithm>

namespace Kites
{
using reorder::CellChange;
using reorder::CellKind;
using reorder::DiffCell;
using reorder::DiffSegment;
using reorder::UnifiedRow;
using reorder::UnifiedSign;

namespace
{
constexpr int kTopMargin = 4;
constexpr int kStripWidth = 44;
constexpr int kMarkerWidth = 3;

const QColor kGreen(46, 160, 67);
const QColor kAmber(210, 153, 34);
const QColor kRed(248, 81, 73);
const QColor kBlue(56, 139, 253);

/// Line text as shown: no trailing '\r', tabs expanded to a tab stop of 4.
QString displayText(const std::string &text)
{
    QString line = QString::fromStdString(text);
    if (line.endsWith(QLatin1Char('\r')))
    {
        line.chop(1);
    }
    QString expanded;
    int column = 0;
    for (const QChar c : line)
    {
        if (c == QLatin1Char('\t'))
        {
            const int spaces = 4 - (column % 4);
            expanded += QString(spaces, QLatin1Char(' '));
            column += spaces;
        }
        else
        {
            expanded += c;
            ++column;
        }
    }
    return expanded;
}

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(std::clamp(alpha, 0, 255));
    return color;
}

QColor blend(const QColor &from, const QColor &to, double amount)
{
    return QColor::fromRgbF(static_cast<float>(from.redF() * (1.0 - amount) + to.redF() * amount),
                            static_cast<float>(from.greenF() * (1.0 - amount) + to.greenF() * amount),
                            static_cast<float>(from.blueF() * (1.0 - amount) + to.blueF() * amount));
}

bool isDark(const QColor &color)
{
    return color.lightness() < 128;
}

/// A readable text color for an accent on the current background.
QColor accentText(const QColor &accent, const QColor &background)
{
    return isDark(background) ? accent.lighter(130) : accent.darker(135);
}

QColor syntaxColor(SyntaxStyle style, const QColor &fallback)
{
    const QTextCharFormat format = ThemeManager::getInstance().getSyntaxFormat(style);
    if (format.foreground().style() == Qt::NoBrush)
    {
        return fallback;
    }
    return format.foreground().color();
}

bool isGhost(const DiffCell &cell)
{
    return cell.kind == CellKind::Stall || cell.kind == CellKind::MissingNop;
}

QFont badgeFont(const QFont &base)
{
    QFont font = base;
    if (font.pixelSize() > 0)
    {
        font.setPixelSize(std::max(8, font.pixelSize() - 2));
    }
    else if (font.pointSizeF() > 0)
    {
        font.setPointSizeF(std::max(6.0, font.pointSizeF() - 1.5));
    }
    return font;
}

} // namespace

ReorderDiffView::ReorderDiffView(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    m_placeholder = tr("Press Analyze to look for an instruction order with fewer pipeline stalls.");
    connect(&ThemeManager::getInstance(), &ThemeManager::editorThemeChangedSignal, this,
            [this](const QString &) { update(); });
    updateMetrics();
}

void ReorderDiffView::setResult(std::shared_ptr<const reorder::AnalysisResult> result)
{
    m_result = std::move(result);
    rebuild();
}

void ReorderDiffView::setMode(Mode mode)
{
    if (m_mode == mode)
    {
        return;
    }
    m_mode = mode;
    rebuild();
}

ReorderDiffView::Mode ReorderDiffView::mode() const
{
    return m_mode;
}

void ReorderDiffView::setCollapseUnchanged(bool collapse)
{
    if (m_collapse == collapse)
    {
        return;
    }
    m_collapse = collapse;
    rebuild();
}

void ReorderDiffView::setPlaceholderText(const QString &text)
{
    m_placeholder = text;
    update();
}

int ReorderDiffView::firstChangeY() const
{
    if (!m_result)
    {
        return -1;
    }
    for (int row = 0; row < static_cast<int>(m_rows.size()); ++row)
    {
        const Row &entry = m_rows[row];
        if (entry.type == RowType::Header && m_result->segments[entry.segment].changed)
        {
            return kTopMargin + row * m_rowHeight;
        }
    }
    return -1;
}

QSize ReorderDiffView::sizeHint() const
{
    return QSize(900, kTopMargin * 2 + static_cast<int>(m_rows.size()) * m_rowHeight);
}

QSize ReorderDiffView::minimumSizeHint() const
{
    return QSize(m_mode == Mode::SideBySide ? 520 : 380, kTopMargin * 2 + static_cast<int>(m_rows.size()) * m_rowHeight);
}

void ReorderDiffView::updateMetrics()
{
    const QFontMetrics metrics(font());
    m_rowHeight = metrics.height() + 6;
    m_charWidth = std::max(1, metrics.horizontalAdvance(QLatin1Char('0')));
    int maxCycle = 99;
    int maxLine = 99;
    if (m_result)
    {
        for (const DiffSegment &segment : m_result->segments)
        {
            for (const std::vector<DiffCell> *cells : {&segment.left, &segment.right})
            {
                for (const DiffCell &cell : *cells)
                {
                    maxCycle = std::max(maxCycle, cell.cycle);
                    maxLine = std::max({maxLine, cell.line, cell.sourceLine});
                }
            }
        }
    }
    m_cycleWidth = m_charWidth * static_cast<int>(QString::number(maxCycle).size()) + 10;
    m_lineWidth = m_charWidth * static_cast<int>(QString::number(maxLine).size()) + 14;
}

void ReorderDiffView::rebuild()
{
    m_rows.clear();
    m_leftRowOf.clear();
    m_rightRowOf.clear();
    m_hoverRow = -1;
    m_hoverInstruction = -1;
    m_related.clear();

    if (m_result && m_result->ok)
    {
        const bool collapseAllowed = m_collapse && m_result->changed;
        const std::vector<DiffSegment> &segments = m_result->segments;
        for (int s = 0; s < static_cast<int>(segments.size()); ++s)
        {
            const DiffSegment &segment = segments[s];
            const bool collapsed = collapseAllowed && !segment.changed;
            if (segment.block >= 0 && !collapsed)
            {
                Row header;
                header.type = RowType::Header;
                header.segment = s;
                header.text = QString::fromStdString(segment.title);
                if (!segment.changed)
                {
                    header.text += tr("  ·  unchanged");
                }
                header.tooltip = QString::fromStdString(segment.tooltipHtml);
                m_rows.push_back(header);
            }

            const size_t count = m_mode == Mode::SideBySide ? std::max(segment.left.size(), segment.right.size())
                                                            : segment.unified.size();
            auto addRow = [&](size_t i) {
                Row row;
                row.segment = s;
                if (m_mode == Mode::SideBySide)
                {
                    row.left = i < segment.left.size() ? &segment.left[i] : nullptr;
                    row.right = i < segment.right.size() ? &segment.right[i] : nullptr;
                    if (segment.changed)
                    {
                        const int index = static_cast<int>(m_rows.size());
                        if (row.left && row.left->kind == CellKind::Instruction)
                        {
                            m_leftRowOf[row.left->instruction] = index;
                        }
                        if (row.right && row.right->kind == CellKind::Instruction)
                        {
                            m_rightRowOf[row.right->instruction] = index;
                        }
                    }
                }
                else
                {
                    row.unified = &segment.unified[i];
                }
                m_rows.push_back(row);
            };
            if (collapsed && count > 6)
            {
                addRow(0);
                addRow(1);
                Row fold;
                fold.type = RowType::Fold;
                fold.segment = s;
                fold.text = tr("⋯  %1 unchanged lines  ⋯").arg(count - 4);
                m_rows.push_back(fold);
                addRow(count - 2);
                addRow(count - 1);
            }
            else
            {
                for (size_t i = 0; i < count; ++i)
                {
                    addRow(i);
                }
            }
        }
    }
    updateMetrics();
    setMinimumSize(minimumSizeHint());
    updateGeometry();
    update();
}

QRect ReorderDiffView::rowRect(int row) const
{
    return QRect(0, kTopMargin + row * m_rowHeight, width(), m_rowHeight);
}

int ReorderDiffView::paneWidth() const
{
    return std::max(0, (width() - kStripWidth) / 2);
}

ReorderDiffView::Hit ReorderDiffView::hitTest(const QPoint &pos) const
{
    Hit hit;
    if (m_rows.empty() || pos.y() < kTopMargin)
    {
        return hit;
    }
    const int row = (pos.y() - kTopMargin) / m_rowHeight;
    if (row < 0 || row >= static_cast<int>(m_rows.size()))
    {
        return hit;
    }
    hit.row = row;
    const Row &entry = m_rows[row];
    if (entry.type != RowType::Cells)
    {
        return hit;
    }
    if (m_mode == Mode::SideBySide)
    {
        hit.newSide = pos.x() >= paneWidth() + kStripWidth / 2;
        hit.cell = hit.newSide ? entry.right : entry.left;
    }
    else
    {
        hit.cell = &entry.unified->cell;
        hit.newSide = entry.unified->sign != UnifiedSign::Removed;
    }
    if (hit.cell)
    {
        hit.line = hit.cell->line;
    }
    return hit;
}

QString ReorderDiffView::htmlFor(const Hit &hit) const
{
    if (hit.row < 0)
    {
        return QString();
    }
    const Row &entry = m_rows[hit.row];
    if (entry.type == RowType::Header)
    {
        return entry.tooltip;
    }
    if (hit.cell)
    {
        return QString::fromStdString(hit.cell->tooltipHtml);
    }
    return QString();
}

void ReorderDiffView::setHover(const Hit &hit)
{
    if (hit.row == m_hoverRow && hit.newSide == m_hoverNewSide)
    {
        return;
    }
    m_hoverRow = hit.row;
    m_hoverNewSide = hit.newSide;
    m_hoverInstruction = hit.cell ? hit.cell->instruction : -1;
    m_related = hit.cell ? hit.cell->related : std::vector<int>{};
    update();
    emit detailHovered(htmlFor(hit));
}

bool ReorderDiffView::isRelated(const DiffCell *cell) const
{
    if (!cell || cell->instruction < 0)
    {
        return false;
    }
    if (cell->instruction == m_hoverInstruction)
    {
        return true;
    }
    return std::find(m_related.begin(), m_related.end(), cell->instruction) != m_related.end();
}

bool ReorderDiffView::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip)
    {
        auto *help = static_cast<QHelpEvent *>(event);
        const Hit hit = hitTest(help->pos());
        const QString html = htmlFor(hit);
        if (html.isEmpty())
        {
            QToolTip::hideText();
            event->ignore();
            return true;
        }
        QToolTip::showText(help->globalPos(), QStringLiteral("<qt>") + html + QStringLiteral("</qt>"), this,
                           rowRect(hit.row));
        return true;
    }
    return QWidget::event(event);
}

void ReorderDiffView::mouseMoveEvent(QMouseEvent *event)
{
    setHover(hitTest(event->position().toPoint()));
    QWidget::mouseMoveEvent(event);
}

void ReorderDiffView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        const Hit hit = hitTest(event->position().toPoint());
        emit detailClicked(htmlFor(hit), hit.line, hit.cell ? hit.cell->sourceLine : -1, hit.newSide);
    }
    QWidget::mousePressEvent(event);
}

void ReorderDiffView::leaveEvent(QEvent *event)
{
    setHover(Hit{});
    QWidget::leaveEvent(event);
}

void ReorderDiffView::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::FontChange)
    {
        updateMetrics();
        setMinimumSize(minimumSizeHint());
        updateGeometry();
        update();
    }
    QWidget::changeEvent(event);
}

void ReorderDiffView::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    const ThemeManager &theme = ThemeManager::getInstance();
    painter.fillRect(event->rect(), theme.getEditorBackgroundColor());
    painter.setFont(font());

    if (!m_result || !m_result->ok || m_rows.empty())
    {
        painter.setPen(theme.getEditorMutedForegroundColor());
        painter.drawText(rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
        return;
    }

    const int first = std::max(0, (event->rect().top() - kTopMargin) / m_rowHeight);
    const int last = std::min(static_cast<int>(m_rows.size()) - 1, (event->rect().bottom() - kTopMargin) / m_rowHeight);
    for (int row = first; row <= last; ++row)
    {
        paintRow(painter, row);
    }
    if (m_mode == Mode::SideBySide)
    {
        paintConnectors(painter, event->rect());
    }
}

void ReorderDiffView::paintRow(QPainter &painter, int index)
{
    const Row &row = m_rows[index];
    const QRect rect = rowRect(index);
    const ThemeManager &theme = ThemeManager::getInstance();
    const QColor background = theme.getEditorBackgroundColor();
    const QColor foreground = theme.getEditorForegroundColor();
    const QColor muted = theme.getEditorMutedForegroundColor();

    if (row.type == RowType::Header)
    {
        painter.fillRect(rect, blend(background, foreground, index == m_hoverRow ? 0.12 : 0.08));
        painter.setPen(withAlpha(foreground, 40));
        painter.drawLine(rect.topLeft(), rect.topRight());
        QFont bold = font();
        bold.setBold(true);
        painter.setFont(bold);
        painter.setPen(foreground);
        const QFontMetrics metrics(bold);
        painter.drawText(rect.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(row.text, Qt::ElideRight, rect.width() - 16));
        painter.setFont(font());
        return;
    }
    if (row.type == RowType::Fold)
    {
        painter.fillRect(rect, blend(background, foreground, 0.04));
        painter.setPen(muted);
        painter.drawText(rect, Qt::AlignCenter, row.text);
        return;
    }

    if (m_mode == Mode::SideBySide)
    {
        const int pane = paneWidth();
        const QRect leftRect(0, rect.top(), pane, rect.height());
        const QRect strip(pane, rect.top(), kStripWidth, rect.height());
        const QRect rightRect(pane + kStripWidth, rect.top(), width() - pane - kStripWidth, rect.height());
        painter.fillRect(strip, blend(background, foreground, 0.03));
        paintCell(painter, leftRect, row.left, row.left ? row.left->line : -1, -1, 0,
                  index == m_hoverRow && !m_hoverNewSide);
        paintCell(painter, rightRect, row.right, row.right ? row.right->line : -1, -1, 0,
                  index == m_hoverRow && m_hoverNewSide);
        return;
    }

    const UnifiedRow &unified = *row.unified;
    const int sign = unified.sign == UnifiedSign::Removed ? -1 : unified.sign == UnifiedSign::Added ? 1 : 0;
    paintCell(painter, rect, &unified.cell, unified.oldLine, unified.newLine, sign, index == m_hoverRow);
}

QColor ReorderDiffView::tintFor(const DiffCell &cell, int sign) const
{
    const ThemeManager &theme = ThemeManager::getInstance();
    const bool dark = isDark(theme.getEditorBackgroundColor());
    const int strong = dark ? 56 : 42;
    const int weak = dark ? 30 : 24;
    if (m_mode == Mode::Unified)
    {
        if (isGhost(cell))
        {
            return withAlpha(cell.kind == CellKind::MissingNop ? theme.getEditorErrorColor() : kRed, weak);
        }
        if (sign < 0)
        {
            return withAlpha(kRed, strong);
        }
        if (sign > 0)
        {
            return withAlpha(cell.kind == CellKind::Nop ? kBlue : kGreen, strong);
        }
        return cell.kind == CellKind::Nop ? withAlpha(kBlue, weak) : QColor(0, 0, 0, 0);
    }
    switch (cell.change)
    {
    case CellChange::MovedUp:
        return withAlpha(kGreen, strong);
    case CellChange::MovedDown:
        return withAlpha(kAmber, strong);
    case CellChange::Removed:
        return withAlpha(kRed, strong);
    case CellChange::Added:
        return withAlpha(cell.kind == CellKind::Nop ? kBlue : kGreen, strong);
    case CellChange::None:
        break;
    }
    switch (cell.kind)
    {
    case CellKind::Stall:
        return withAlpha(kRed, weak);
    case CellKind::MissingNop:
        return withAlpha(theme.getEditorErrorColor(), strong);
    case CellKind::Nop:
        return withAlpha(kBlue, weak);
    default:
        return QColor(0, 0, 0, 0);
    }
}

void ReorderDiffView::paintCell(QPainter &painter, const QRect &rect, const DiffCell *cell, int lineNumber,
                                int secondLineNumber, int sign, bool hovered)
{
    if (!cell || rect.width() <= 0)
    {
        return;
    }
    const ThemeManager &theme = ThemeManager::getInstance();
    const QColor background = theme.getEditorBackgroundColor();
    const QColor foreground = theme.getEditorForegroundColor();
    const QColor muted = theme.getEditorMutedForegroundColor();
    const bool dark = isDark(background);
    QColor accent = palette().color(QPalette::Highlight);
    if (accent.lightness() < 60 || accent.lightness() > 230)
    {
        accent = kBlue;
    }

    const QColor tint = tintFor(*cell, sign);
    if (tint.alpha() > 0)
    {
        painter.fillRect(rect, tint);
    }
    if (isGhost(*cell))
    {
        const QColor hatch = cell->kind == CellKind::MissingNop ? theme.getEditorErrorColor() : kRed;
        painter.fillRect(rect, QBrush(withAlpha(hatch, dark ? 80 : 64), Qt::BDiagPattern));
    }
    if (hovered)
    {
        painter.fillRect(rect, withAlpha(foreground, dark ? 26 : 18));
    }
    if (hovered || isRelated(cell))
    {
        painter.save();
        painter.setPen(QPen(withAlpha(accent, 230), 1, hovered ? Qt::SolidLine : Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect.adjusted(0, 0, -1, -1));
        painter.restore();
    }

    // colored marker on the left edge of changed rows
    QColor marker(0, 0, 0, 0);
    if (m_mode == Mode::Unified)
    {
        marker = sign < 0 ? kRed : sign > 0 ? kGreen : marker;
    }
    else if (cell->change == CellChange::MovedUp || cell->change == CellChange::Added)
    {
        marker = cell->kind == CellKind::Nop ? kBlue : kGreen;
    }
    else if (cell->change == CellChange::MovedDown)
    {
        marker = kAmber;
    }
    else if (cell->change == CellChange::Removed)
    {
        marker = kRed;
    }
    if (marker.alpha() > 0)
    {
        painter.fillRect(QRect(rect.left(), rect.top(), kMarkerWidth, rect.height()), marker);
    }

    // gutters
    int x = rect.left() + kMarkerWidth + 2;
    const QFont small = badgeFont(font());
    painter.setFont(small);
    painter.setPen(withAlpha(muted, 200));
    if (m_mode == Mode::SideBySide)
    {
        if (cell->cycle > 0)
        {
            painter.drawText(QRect(x, rect.top(), m_cycleWidth - 6, rect.height()), Qt::AlignVCenter | Qt::AlignRight,
                             QString::number(cell->cycle));
        }
        x += m_cycleWidth;
        painter.setFont(font());
        painter.setPen(muted);
        painter.drawText(QRect(x, rect.top(), m_lineWidth - 8, rect.height()), Qt::AlignVCenter | Qt::AlignRight,
                         lineNumber > 0 ? QString::number(lineNumber) : QStringLiteral("·"));
        x += m_lineWidth;
    }
    else
    {
        painter.setFont(font());
        painter.setPen(muted);
        const int left = lineNumber;
        const int right = secondLineNumber;
        painter.drawText(QRect(x, rect.top(), m_lineWidth - 6, rect.height()), Qt::AlignVCenter | Qt::AlignRight,
                         left > 0 ? QString::number(left) : QString());
        x += m_lineWidth;
        painter.drawText(QRect(x, rect.top(), m_lineWidth - 6, rect.height()), Qt::AlignVCenter | Qt::AlignRight,
                         right > 0 ? QString::number(right) : QString());
        x += m_lineWidth;
        painter.setPen(sign < 0 ? accentText(kRed, background) : sign > 0 ? accentText(kGreen, background) : muted);
        painter.drawText(QRect(x, rect.top(), m_charWidth * 2, rect.height()), Qt::AlignVCenter | Qt::AlignLeft,
                         sign < 0 ? QStringLiteral("-") : sign > 0 ? QStringLiteral("+") : QStringLiteral(" "));
        x += m_charWidth * 2;
    }

    // badge on the right
    int badgeWidth = 0;
    const QString badge = QString::fromStdString(cell->badge);
    if (!badge.isEmpty())
    {
        painter.setFont(small);
        const QFontMetrics smallMetrics(small);
        const int available = std::max(0, (rect.right() - x) * 2 / 5);
        const QString shown = smallMetrics.elidedText(badge, Qt::ElideRight, available);
        badgeWidth = smallMetrics.horizontalAdvance(shown) + 12;
        QColor badgeColor = muted;
        if (cell->change == CellChange::MovedUp)
        {
            badgeColor = accentText(kGreen, background);
        }
        else if (cell->change == CellChange::MovedDown)
        {
            badgeColor = accentText(kAmber, background);
        }
        else if (cell->kind == CellKind::MissingNop || cell->kind == CellKind::Stall)
        {
            badgeColor = accentText(kRed, background);
        }
        else if (badge.startsWith(QStringLiteral("✓")))
        {
            badgeColor = accentText(kGreen, background);
        }
        painter.setPen(badgeColor);
        painter.drawText(QRect(rect.right() - badgeWidth, rect.top(), badgeWidth - 6, rect.height()),
                         Qt::AlignVCenter | Qt::AlignRight, shown);
        painter.setFont(font());
    }

    const QRect textRect(x + 4, rect.top(), std::max(0, rect.right() - badgeWidth - x - 8), rect.height());
    if (isGhost(*cell))
    {
        QFont italic = font();
        italic.setItalic(true);
        painter.setFont(italic);
        paintCode(painter, textRect, displayText(cell->text), true);
        painter.setFont(font());
    }
    else
    {
        paintCode(painter, textRect, displayText(cell->text), false);
        if (cell->change == CellChange::Removed && m_mode == Mode::SideBySide)
        {
            const QString shown = displayText(cell->text);
            const QString trimmed = shown.trimmed();
            const QFontMetrics metrics(font());
            const int indent = metrics.horizontalAdvance(shown.left(shown.indexOf(trimmed.isEmpty() ? QChar() : trimmed[0])));
            const int width = std::min(textRect.width() - indent, metrics.horizontalAdvance(trimmed));
            painter.setPen(QPen(withAlpha(kRed, 200), 1));
            const int y = textRect.center().y();
            painter.drawLine(textRect.left() + indent, y, textRect.left() + indent + std::max(0, width), y);
        }
    }
}

void ReorderDiffView::paintCode(QPainter &painter, const QRect &rect, const QString &text, bool muted)
{
    if (rect.width() <= 0)
    {
        return;
    }
    const ThemeManager &theme = ThemeManager::getInstance();
    const QColor foreground = muted ? theme.getEditorMutedForegroundColor() : theme.getEditorForegroundColor();
    const QFontMetrics metrics(painter.font());

    struct Run
    {
        QString text;
        QColor color;
    };
    std::vector<Run> runs;
    if (muted)
    {
        runs.push_back({text, foreground});
    }
    else
    {
        static const QRegularExpression registerPattern(
            QStringLiteral("^(x([0-9]|[12][0-9]|3[01])|zero|ra|sp|gp|tp|fp|t[0-6]|s([0-9]|1[01])|a[0-7]|"
                           "f([0-9]|[12][0-9]|3[01])|ft([0-9]|1[01])|fs([0-9]|1[01])|fa[0-7])$"));
        static const QRegularExpression numberPattern(QStringLiteral("^-?(0[xX][0-9a-fA-F]+|0[bB][01]+|[0-9]+)$"));
        static const QRegularExpression tokenPattern(QStringLiteral("(\\s+|[,()]|[^\\s,()]+)"));

        int comment = -1;
        for (int i = 0; i < text.size(); ++i)
        {
            if (text[i] == QLatin1Char('#') || text[i] == QLatin1Char(';'))
            {
                comment = i;
                break;
            }
        }
        QString code = comment >= 0 ? text.left(comment) : text;
        const int colon = code.indexOf(QLatin1Char(':'));
        if (colon > 0 && !code.left(colon).trimmed().isEmpty())
        {
            runs.push_back({code.left(colon + 1), syntaxColor(SyntaxStyle::Label, foreground)});
            code = code.mid(colon + 1);
        }
        bool firstWord = true;
        auto matches = tokenPattern.globalMatch(code);
        while (matches.hasNext())
        {
            const QString piece = matches.next().captured(0);
            QColor color = foreground;
            const bool word = !piece.trimmed().isEmpty() && piece != QLatin1String(",") &&
                              piece != QLatin1String("(") && piece != QLatin1String(")");
            if (word)
            {
                if (firstWord)
                {
                    color = piece.startsWith(QLatin1Char('.')) ? syntaxColor(SyntaxStyle::PseudoInstruction, foreground)
                                                               : syntaxColor(SyntaxStyle::R32Instruction, foreground);
                    firstWord = false;
                }
                else if (registerPattern.match(piece).hasMatch())
                {
                    color = syntaxColor(SyntaxStyle::Register, foreground);
                }
                else if (numberPattern.match(piece).hasMatch())
                {
                    color = syntaxColor(SyntaxStyle::NumberLiteral, foreground);
                }
            }
            runs.push_back({piece, color});
        }
        if (comment >= 0)
        {
            runs.push_back({text.mid(comment), syntaxColor(SyntaxStyle::Comment, foreground)});
        }
    }

    int x = rect.left();
    const int baseline = rect.top() + (rect.height() + metrics.ascent() - metrics.descent()) / 2;
    for (const Run &run : runs)
    {
        const int width = metrics.horizontalAdvance(run.text);
        painter.setPen(run.color);
        if (x + width > rect.right())
        {
            painter.drawText(x, baseline, metrics.elidedText(run.text, Qt::ElideRight, rect.right() - x));
            break;
        }
        painter.drawText(x, baseline, run.text);
        x += width;
    }
}

void ReorderDiffView::paintConnectors(QPainter &painter, const QRect &area)
{
    const ThemeManager &theme = ThemeManager::getInstance();
    const QColor muted = theme.getEditorMutedForegroundColor();
    const int pane = paneWidth();
    const double x0 = pane;
    const double x1 = pane + kStripWidth;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(QRect(pane, area.top(), kStripWidth, area.height()));
    for (const auto &[instruction, leftRow] : m_leftRowOf)
    {
        const auto it = m_rightRowOf.find(instruction);
        if (it == m_rightRowOf.end())
        {
            continue;
        }
        const int rightRow = it->second;
        const double y0 = rowRect(leftRow).center().y() + 0.5;
        const double y1 = rowRect(rightRow).center().y() + 0.5;
        if (std::max(y0, y1) < area.top() - 4 || std::min(y0, y1) > area.bottom() + 4)
        {
            continue;
        }
        const DiffCell *cell = m_rows[rightRow].right;
        const bool moved = cell && (cell->change == CellChange::MovedUp || cell->change == CellChange::MovedDown);
        const bool hovered = instruction == m_hoverInstruction;
        if (!moved && !hovered && leftRow == rightRow)
        {
            continue;
        }
        QColor color = moved ? (cell->change == CellChange::MovedUp ? kGreen : kAmber) : muted;
        int alpha = moved ? 200 : 80;
        double penWidth = moved ? 1.6 : 1.0;
        if (m_hoverInstruction >= 0 && !hovered)
        {
            alpha /= 3;
        }
        if (hovered)
        {
            alpha = 255;
            penWidth = 2.6;
        }
        QPainterPath path(QPointF(x0, y0));
        path.cubicTo(x0 + kStripWidth * 0.55, y0, x1 - kStripWidth * 0.55, y1, x1, y1);
        painter.setPen(QPen(withAlpha(color, alpha), penWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(color, alpha));
        painter.drawEllipse(QPointF(x0 + 2.0, y0), 2.2, 2.2);
        painter.drawEllipse(QPointF(x1 - 2.0, y1), 2.2, 2.2);
    }
    painter.restore();
}

} // namespace Kites
