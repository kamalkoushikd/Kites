#pragma once

#include "reorder/suggestions.h"

#include <QWidget>
#include <vector>

namespace Kites
{

/**
 * @brief Draws a block's before/after pipeline timeline.
 *
 * One row per instruction, one column per cycle. Each row shows IF ID EX MEM WB;
 * the cycles an instruction waits between IF and ID are shaded (yellow = the
 * hazard unit stalls it, red = a NOP is needed because there is no hazard unit).
 * In the "after" timeline the moved instruction's row is highlighted and
 * labelled "fills stall".
 *
 * The widget fills the width it is given: "Before" and "After" are drawn side by
 * side when both fit, otherwise one below the other.
 */
class PipelineTimeline : public QWidget
{
    Q_OBJECT
public:
    explicit PipelineTimeline(QWidget *parent = nullptr);

    void setBlock(const std::vector<suggestions::TimelineRow> &before, const std::vector<suggestions::TimelineRow> &after,
                  bool hazardUnit, int highlightLine);
    void clear();
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    /// Draws one timeline (title + rows) at (x, y) in a column of the given width; returns the y below it.
    int paintTimeline(QPainter &painter, int x, int y, int width, const QString &title,
                      const std::vector<suggestions::TimelineRow> &rows, bool after);
    int cycles() const;
    int columnWidth(bool after) const; ///< width one timeline needs (After: with room for "fills stall")
    int sideBySideHeight() const;  ///< height with Before | After next to each other
    int stackedHeight() const;     ///< height with Before above After
    bool sideBySide() const;       ///< true if both timelines fit next to each other
    void updateMinimumHeight();

    std::vector<suggestions::TimelineRow> m_before;
    std::vector<suggestions::TimelineRow> m_after;
    bool m_hazardUnit = true;
    int m_highlightLine = -1; ///< line of the moved instruction
    int m_labelWidth = 260;
    int m_cellWidth = 34;
    int m_rowHeight = 20;
    int m_noteWidth = 100; ///< room for "← fills stall" after a row
};

} // namespace Kites
