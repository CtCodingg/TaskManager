#include "HistoryChartWidget.h"
#include "UiTheme.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QPen>
#include <QBrush>
#include <QPainter>
#include <QFont>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QSplineSeries>
#include <QtCharts/QValueAxis>
#include <QtCharts/QLegend>

#if QT_VERSION_MAJOR < 6
using namespace QtCharts;
#endif

namespace {
// Muted grid-line / axis color that reads clearly on the dark card
// background without competing with the actual data series.
QColor gridColor() { return QColor("#1e242e"); }
QColor axisLabelColor() { return UiTheme::textTertiary(); }
}

HistoryChartWidget::HistoryChartWidget(const QString& title, int maxPoints, QWidget* parent)
    : QWidget(parent), m_maxPoints(maxPoints) {

    m_chart = new QChart();
    m_chart->setTitle(title);
    m_chart->setTitleBrush(QBrush(UiTheme::textSecondary()));
    QFont titleFont;
    titleFont.setPointSize(9);
    titleFont.setBold(true);
    m_chart->setTitleFont(titleFont);

    m_chart->legend()->setVisible(true);
    m_chart->legend()->setAlignment(Qt::AlignBottom);
    m_chart->legend()->setLabelColor(UiTheme::textSecondary());
    m_chart->legend()->setBackgroundVisible(false);

    m_chart->setAnimationOptions(QChart::NoAnimation);
    m_chart->setMargins(QMargins(4, 4, 4, 4));
    m_chart->setBackgroundVisible(false);   // let the dark card show through
    m_chart->setBackgroundRoundness(0);
    m_chart->setPlotAreaBackgroundVisible(false);

    m_axisX = new QValueAxis();
    m_axisX->setRange(0, maxPoints);
    m_axisX->setLabelsVisible(false);
    m_axisX->setTickCount(2);
    m_axisX->setGridLineColor(gridColor());
    m_axisX->setLinePenColor(gridColor());
    m_chart->addAxis(m_axisX, Qt::AlignBottom);

    m_axisY = new QValueAxis();
    m_axisY->setRange(0, 100);
    m_axisY->setLabelFormat("%d");
    m_axisY->setLabelsColor(axisLabelColor());
    m_axisY->setGridLineColor(gridColor());
    m_axisY->setLinePenColor(gridColor());
    m_chart->addAxis(m_axisY, Qt::AlignLeft);

    m_view = new QChartView(m_chart);
    m_view->setRenderHint(QPainter::Antialiasing);
    m_view->setMinimumHeight(150);
    m_view->setBackgroundBrush(Qt::NoBrush);
    m_view->setStyleSheet("background: transparent; border: none;");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
}

int HistoryChartWidget::addSeries(const QString& name, const QColor& color) {
    SeriesData sd;
    // QSplineSeries renders a smoothed curve through the same points a
    // QLineSeries would use -- a nicer "telemetry" look for live metrics,
    // and it shares the QXYSeries API (append/clear) so the rest of this
    // class doesn't need to know the difference.
    auto* spline = new QSplineSeries();
    spline->setName(name);
    QPen pen(color);
    pen.setWidth(2);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    spline->setPen(pen);
    sd.line = spline;

    m_chart->addSeries(sd.line);
    sd.line->attachAxis(m_axisX);
    sd.line->attachAxis(m_axisY);

    for (int i = 0; i < m_maxPoints; ++i) {
        sd.values.push_back(0.0);
        sd.line->append(i, 0.0);
    }

    m_series.push_back(sd);
    return m_series.size() - 1;
}

void HistoryChartWidget::pushValue(int seriesIndex, double value) {
    if (seriesIndex < 0 || seriesIndex >= m_series.size()) return;
    SeriesData& sd = m_series[seriesIndex];

    sd.values.push_back(value);
    if (sd.values.size() > m_maxPoints) sd.values.removeFirst();

    sd.line->clear();
    for (int i = 0; i < sd.values.size(); ++i) {
        sd.line->append(i, sd.values[i]);
    }

    if (m_autoScaleY) {
        double maxVal = 1.0;
        for (const SeriesData& s : m_series) {
            for (double v : s.values) maxVal = qMax(maxVal, v);
        }
        m_axisY->setRange(0, maxVal * 1.15);
    }
}

void HistoryChartWidget::setYRange(double minVal, double maxVal) {
    m_autoScaleY = false;
    m_axisY->setRange(minVal, maxVal);
}

void HistoryChartWidget::setYAutoScale(bool autoScale) {
    m_autoScaleY = autoScale;
}
