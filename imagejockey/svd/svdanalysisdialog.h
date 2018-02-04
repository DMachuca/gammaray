#ifndef SVDANALYSISDIALOG_H
#define SVDANALYSISDIALOG_H

#include <QDialog>
#include <QModelIndex>

class SVDFactorTree;
class SVDFactor;
class QMenu;

namespace Ui {
	class SVDAnalysisDialog;
}

namespace spectral{
    struct array;
}

class ImageJockeyGridPlot;

class SVDAnalysisDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SVDAnalysisDialog(QWidget *parent = 0);
    ~SVDAnalysisDialog();

	void setTree( SVDFactorTree* tree );

	/** Sets whether the tree pointed to by m_tree will be deleted upon closing this dialog. Default is false.
	 *  This is normally set when the dialog is non-modal.
	 */
	void setDeleteTreeOnClose( bool flag ){ m_deleteTree = flag; }

signals:
    /** Emitted when the user clicks on the "Save" or "Sum" button.
     *  The slot is responsible for deleting or managing the object.
     */
    void sumOfFactorsComputed( spectral::array* sumOfFactors );

private:
    Ui::SVDAnalysisDialog *ui;
	SVDFactorTree *m_tree;
	bool m_deleteTree;
    //factor tree context menu
    QMenu *m_factorContextMenu;
    SVDFactor *m_right_clicked_factor;
	int m_numberOfSVDFactorsSetInTheDialog;
	ImageJockeyGridPlot* m_gridPlot;
	void refreshTreeStyle();
    void forcePlotUpdate();
    void adjustColorTableWidgets( int cmbIndex );

private slots:
    void onFactorContextMenu(const QPoint &mouse_location);
    void onFactorizeFurther();
	void onUserSetNumberOfSVDFactors(int number);
	void onOpenFactor();
	void onFactorClicked( QModelIndex index );
    void onCmbColorScaleValueChanged( int index );
    void onCmbPlaneChanged( int index );
    void onSpinSliceChanged( int value );
    void onSave();
};

#endif // SVDANALYSISDIALOG_H
