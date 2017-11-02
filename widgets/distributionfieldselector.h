#ifndef DISTRIBUTIONFIELDSELECTOR_H
#define DISTRIBUTIONFIELDSELECTOR_H

#include <QWidget>

namespace Ui {
class DistributionFieldSelector;
}

class Distribution;
class DistributionColumn;

class DistributionFieldSelector : public QWidget
{
    Q_OBJECT

public:
    explicit DistributionFieldSelector(QWidget *parent = 0);
    ~DistributionFieldSelector();

    /** Returns the name of the selected variable. */
    QString getSelectedVariableName();

signals:
    void fieldSelected( DistributionColumn* field );

private slots:
    void onSelection( int index );
    void onListVariables(Distribution *dist);

private:
    Ui::DistributionFieldSelector *ui;
    Distribution *m_dist;
};

#endif // DISTRIBUTIONFIELDSELECTOR_H
