#ifndef TRANSIOGRAMCHARTVIEW_H
#define TRANSIOGRAMCHARTVIEW_H

#include <QChartView>

class QGraphicsSimpleTextItem;

namespace QtCharts {
    class QValueAxis;
    class QLineSeries;
}

enum class TransiogramType : int {
    AUTO_TRANSIOGRAM,
    CROSS_TRANSIOGRAM
};

class TransiogramChartView : public QtCharts::QChartView
{
public:
    TransiogramChartView(QtCharts::QChart* chart,
                         TransiogramType type,
                         double hMax,
                         QtCharts::QValueAxis *axisX,
                         QtCharts::QValueAxis *axisY,
                         QWidget *parent = nullptr);

    void setModelParameters( double range, double sill );

    double getRange() const { return m_range; }
    double getSill() const { return m_sill; }

private:
    QtCharts::QChart* m_chart;

    // QWidget interface
protected:
    virtual void mousePressEvent(QMouseEvent *event);
    virtual void mouseReleaseEvent(QMouseEvent *event);
    virtual void mouseMoveEvent(QMouseEvent *event);

private:
    QRubberBand* m_rubberBandH;
    QRubberBand* m_rubberBandV;
    double m_range;
    double m_sill;
    TransiogramType m_type;
    QtCharts::QValueAxis *m_axisX;
    QtCharts::QValueAxis *m_axisY;
    double m_hMax; //maximum h
    QtCharts::QLineSeries *m_seriesTransiogramModel;
    bool m_mouseDown;
    void showOrHideCrossHairs();
    void updateModelSeries();
};

#endif // TRANSIOGRAMCHARTVIEW_H