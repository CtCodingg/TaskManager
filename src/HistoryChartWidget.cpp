#include "HistoryChartWidget.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QPen>
#include <QPainter>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#include <QtCharts/QLegend>

#if QT_VERSION_MAJOR < 6
using namespace QtCharts;
#endif

HistoryChartWidget::HistoryChartWidget(const QString& title, int maxPoints, QWidget* parent)
    : QWidget(parent), m_maxPoints(maxPoints) {

    m_chart = new QChart();
    m_chart->setTitle(title);
    m_chart->legend()->setVisible(true);
    m_chart->legend()->setAlignment(Qt::AlignBottom);
    m_chart->setAnimationOptions(QChart::NoAnimation);
    m_chart->setMargins(QMargins(4, 4, 4, 4));

    m_axisX = new QValueAxis();
    m_axisX->setRange(0, maxPoints);
    m_axisX->setLabelsVisible(false);
    m_axisX->setTickCount(2);
    m_chart->addAxis(m_axisX, Qt::AlignBottom);

    m_axisY = new QValueAxis();
    m_axisY->setRange(0, 100);
    m_axisY->setLabelFormat("%d");
    m_chart->addAxis(m_axisY, Qt::AlignLeft);

    m_view = new QChartView(m_chart);
    m_view->setRenderHint(QPainter::Antialiasing);
    m_view->setMinimumHeight(150);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
}

int HistoryChartWidget::addSeries(const QString& name, const QColor& color) {
    SeriesData sd;
    sd.line = new QLineSeries();
    sd.line->setName(name);
    QPen pen(color);
    pen.setWidth(2);
    sd.line->setPen(pen);

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
