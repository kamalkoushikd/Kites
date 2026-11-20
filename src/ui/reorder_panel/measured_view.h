#pragma once

#include "reorder/reorder_analysis.h"
#include "reorder/run_profile.h"

#include <QWidget>
#include <memory>

class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;

namespace Kites
{

/**
 * @brief The "Measured" view of the Reorder panel: run the original and the reordered
 *        program on the chosen Kites pipeline and compare the stalls they really had.
 *
 * The other views count stalls statically (every instruction once). Here a loop that
 * runs 100 times counts 100 times. The runs go a few thousand cycles per timer tick,
 * so the window keeps painting (the processors must live in the GUI thread).
 */
class MeasuredView : public QWidget
{
    Q_OBJECT
  public:
    explicit MeasuredView(QWidget *parent = nullptr);
    ~MeasuredView() override;

    /// A new analysis: a running measurement stops and old numbers are cleared.
    void setAnalysis(std::shared_ptr<const reorder::AnalysisResult> result);

  public slots:
    void run();

  private:
    void stepRun();
    void showResults();
    void fillSummary();
    void fillLines();

    std::shared_ptr<const reorder::AnalysisResult> m_result;
    std::unique_ptr<reorder::ProfiledRun> m_running;
    int m_stage = 0; ///< 0 = running the original, 1 = running the reordered program
    reorder::RunProfile m_before;
    reorder::RunProfile m_after;

    QPushButton *m_runButton = nullptr;
    QLabel *m_status = nullptr;
    QTableWidget *m_summary = nullptr;
    QTableWidget *m_lines = nullptr;
    QTimer *m_timer = nullptr;
};

} // namespace Kites
