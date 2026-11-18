#pragma once

#include <QColor>
#include <QMap>
#include <QPointer>
#include <QString>
#include <QWidget>

class QPlainTextEdit;

namespace Kites
{

/**
 * @brief Wavy underlines under the lines that wait for a result, drawn over the editor.
 *
 * It is a transparent widget lying on top of the editor's viewport, so the
 * editor itself is not changed. Hovering an underlined line shows the
 * suggestion's three lines (Moved / Why / Effect) as a tooltip.
 */
class HazardSquiggles : public QWidget
{
    Q_OBJECT
public:
    explicit HazardSquiggles(QPlainTextEdit *editor);

    /// line (1-based) -> tooltip text; color = yellow (hazard unit on) or red (off)
    void setLines(const QMap<int, QString> &tooltips, const QColor &color);
    void clearLines();

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QPointer<QPlainTextEdit> m_editor;
    QMap<int, QString> m_tooltips;
    QColor m_color;
};

} // namespace Kites
