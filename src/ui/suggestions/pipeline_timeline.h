#pragma once

#include "scheduler/scheduler.h"

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
 */
class PipelineTimeline : public QWidget
{
    Q_OBJECT
public:
    explicit PipelineTimeline(QWidget *parent = nullptr);

    void setBlock(const std::vector<scheduler::TimelineRow> &before, const std::vector<scheduler::TimelineRow> &after,
                  bool hazardUnit, int highlightLine);
    void clear();
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    /// Draws one timeline (title + rows) starting at y; returns the y below it.
    int paintTimeline(QPainter &painter, int y, const QString &title, const std::vector<scheduler::TimelineRow> &rows,
                      bool after);
    int cycles() const;

    std::vector<scheduler::TimelineRow> m_before;
    std::vector<scheduler::TimelineRow> m_after;
    bool m_hazardUnit = true;
    int m_highlightLine = -1; ///< line of the moved instruction
    int m_labelWidth = 260;
    int m_cellWidth = 34;
    int m_rowHeight = 20;
};

} // namespace Kites
