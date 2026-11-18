#include "hazard_squiggles.h"

#include <QHelpEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QToolTip>

namespace Kites
{

HazardSquiggles::HazardSquiggles(QPlainTextEdit *editor) : QWidget(editor->viewport()), m_editor(editor)
{
    setAttribute(Qt::WA_TransparentForMouseEvents); // clicks and typing go to the editor
    setAttribute(Qt::WA_NoSystemBackground);
    setGeometry(editor->viewport()->rect());
    editor->viewport()->installEventFilter(this);
    // repaint when the editor scrolls or its text changes
    connect(editor, &QPlainTextEdit::updateRequest, this, [this](const QRect &, int) { update(); });
    show();
}

void HazardSquiggles::setLines(const QMap<int, QString> &tooltips, const QColor &color)
{
    m_tooltips = tooltips;
    m_color = color;
    raise();
    update();
}

void HazardSquiggles::clearLines()
{
    m_tooltips.clear();
    update();
}

void HazardSquiggles::paintEvent(QPaintEvent *)
{
    if (!m_editor || m_tooltips.isEmpty())
    {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(m_color, 1.4));
    QTextDocument *document = m_editor->document();
    for (auto it = m_tooltips.constBegin(); it != m_tooltips.constEnd(); ++it)
    {
        const QTextBlock block = document->findBlockByNumber(it.key() - 1);
        if (!block.isValid() || !block.isVisible())
        {
            continue;
        }
        // underline from the first character to the end of the line's text
        const QString text = block.text();
        int first = 0;
        while (first < text.size() && text[first].isSpace())
        {
            ++first;
        }
        QTextCursor cursor(block);
        cursor.setPosition(block.position() + first);
        const QRect start = m_editor->cursorRect(cursor);
        cursor.setPosition(block.position() + text.size());
        const QRect end = m_editor->cursorRect(cursor);
        if (end.bottom() < 0 || start.top() > height())
        {
            continue;
        }
        const double y = start.bottom() - 1.5;
        QPainterPath wave(QPointF(start.left(), y));
        for (int x = start.left(), up = 0; x < end.left(); x += 3, up ^= 1)
        {
            wave.lineTo(QPointF(x + 3, up ? y : y + 2.5));
        }
        painter.drawPath(wave);
    }
}

bool HazardSquiggles::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_editor || watched != m_editor->viewport())
    {
        return false;
    }
    if (event->type() == QEvent::Resize)
    {
        setGeometry(m_editor->viewport()->rect());
    }
    else if (event->type() == QEvent::ToolTip && !m_tooltips.isEmpty())
    {
        auto *help = static_cast<QHelpEvent *>(event);
        const QTextCursor cursor = m_editor->cursorForPosition(help->pos());
        const int line = cursor.blockNumber() + 1;
        const QRect lineRect = m_editor->cursorRect(cursor);
        if (m_tooltips.contains(line) && help->pos().y() >= lineRect.top() - 2 &&
            help->pos().y() <= lineRect.bottom() + 2)
        {
            QToolTip::showText(help->globalPos(), m_tooltips.value(line), m_editor->viewport());
            return true; // ours; otherwise the editor shows its own (error) tooltips as before
        }
    }
    return false;
}

} // namespace Kites
