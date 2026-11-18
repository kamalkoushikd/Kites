#pragma once

#include "reorder/reorder_analysis.h"

#include <QWidget>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Kites
{

/**
 * @brief Paints the suggested reordering as a diff (side by side or unified).
 *
 * Side by side shows the original program on the left and the suggestion on the
 * right, each with its pipeline bubbles drawn as hatched rows, and curves that
 * connect every moved instruction's old and new place. Hovering a row shows
 * why it changed (tooltip) and highlights the lines that explanation mentions.
 */
class ReorderDiffView : public QWidget
{
    Q_OBJECT
public:
    enum class Mode
    {
        SideBySide,
        Unified
    };

    explicit ReorderDiffView(QWidget *parent = nullptr);

    void setResult(std::shared_ptr<const reorder::AnalysisResult> result);
    void setMode(Mode mode);
    Mode mode() const;
    void setCollapseUnchanged(bool collapse);
    void setPlaceholderText(const QString &text);
    /// y position of the first changed block's header, -1 if nothing changed
    int firstChangeY() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /// html of the row under the mouse, empty when the mouse leaves the rows
    void detailHovered(const QString &html);
    /// a row was clicked; line = 1-based line on that side, sourceLine = line in the original text (-1 if none)
    void detailClicked(const QString &html, int line, int sourceLine, bool newSide);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    enum class RowType
    {
        Header,
        Cells,
        Fold
    };

    struct Row
    {
        RowType type = RowType::Cells;
        int segment = -1;
        const reorder::DiffCell *left = nullptr;
        const reorder::DiffCell *right = nullptr;
        const reorder::UnifiedRow *unified = nullptr;
        QString text;
        QString tooltip;
    };

    struct Hit
    {
        int row = -1;
        bool newSide = false;
        const reorder::DiffCell *cell = nullptr;
        int line = -1;
    };

    void rebuild();
    void updateMetrics();
    Hit hitTest(const QPoint &pos) const;
    QRect rowRect(int row) const;
    int paneWidth() const;
    QString htmlFor(const Hit &hit) const;
    void setHover(const Hit &hit);

    void paintRow(QPainter &painter, int row);
    void paintCell(QPainter &painter, const QRect &rect, const reorder::DiffCell *cell, int lineNumber,
                   int secondLineNumber, int sign, bool hovered);
    void paintCode(QPainter &painter, const QRect &rect, const QString &text, bool muted);
    void paintConnectors(QPainter &painter, const QRect &area);
    QColor tintFor(const reorder::DiffCell &cell, int sign) const;
    bool isRelated(const reorder::DiffCell *cell) const;

    std::shared_ptr<const reorder::AnalysisResult> m_result;
    std::vector<Row> m_rows;
    std::unordered_map<int, int> m_leftRowOf;  ///< instruction -> row (side by side, changed blocks)
    std::unordered_map<int, int> m_rightRowOf; ///< instruction -> row
    Mode m_mode = Mode::SideBySide;
    bool m_collapse = true;
    QString m_placeholder;

    int m_rowHeight = 18;
    int m_charWidth = 8;
    int m_cycleWidth = 30;
    int m_lineWidth = 30;

    int m_hoverRow = -1;
    bool m_hoverNewSide = false;
    int m_hoverInstruction = -1;
    std::vector<int> m_related;
};

} // namespace Kites
