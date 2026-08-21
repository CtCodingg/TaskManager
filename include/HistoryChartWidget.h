#pragma once

#include <QWidget>
#include <QVector>
#include <QColor>
#include <QString>
#include <QtGlobal>

// QtCharts namespace handling differs between Qt5 and Qt6:
//  - Qt5: chart classes live in the "QtCharts" namespace, so code needs
//    "using namespace QtCharts;" to use unqualified names.
//  - Qt6: chart classes live directly in the global (or QT_NAMESPACE)
//    namespace, no "QtCharts" wrapper exists at all. The old
//    QT_CHARTS_BEGIN_NAMESPACE/QT_CHARTS_USE_NAMESPACE macros from Qt5
//    Charts are gone in Qt6, so we do not rely on them.
#if QT_VERSION_MAJOR >= 6
class QChart;
class QChartView;
class QXYSeries;
class QValueAxis;
#else
QT_BEGIN_NAMESPACE
namespace QtCharts {
    class QChart;
    class QChartView;
    class QXYSeries;
    class QValueAxis;
}
QT_END_NAMESPACE
using namespace QtCharts;
#endif

// A small rolling line-chart widget used for CPU / RAM / Network history
// sparkline-style panels. Supports 1..N series sharing one 0-100 (or custom)
// Y axis and a fixed-length rolling X window. Renders as a smoothed spline
// in a dark "telemetry" style matching the rest of the UI.
class HistoryChartWidget : public QWidget {
    Q_OBJECT
public:
    explicit HistoryChartWidget(const QString& title, int maxPoints = 120, QWidget* parent = nullptr);

    int addSeries(const QString& name, const QColor& color);
    void pushValue(int seriesIndex, double value);
    void setYRange(double minVal, double maxVal);
    void setYAutoScale(bool autoScale);

private:
    struct SeriesData {
        QXYSeries* line = nullptr; // actually a QSplineSeries instance, see .cpp
        QVector<double> values;
    };

    QChart* m_chart = nullptr;
    QChartView* m_view = nullptr;
    QValueAxis* m_axisX = nullptr;
    QValueAxis* m_axisY = nullptr;
    QVector<SeriesData> m_series;
    int m_maxPoints;
    bool m_autoScaleY = false;
};