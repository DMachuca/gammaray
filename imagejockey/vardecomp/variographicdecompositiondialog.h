#ifndef VARIOGRAPHICDECOMPOSITIONDIALOG_H
#define VARIOGRAPHICDECOMPOSITIONDIALOG_H

#include <QDialog>

namespace Ui {
class VariographicDecompositionDialog;
}

class IJCartesianGridSelector;
class IJVariableSelector;
class IJAbstractVariable;
class IJAbstractCartesianGrid;
namespace spectral {
    class array;
    class complex_array;
}

class VariographicDecompositionDialog : public QDialog
{
    Q_OBJECT

public:
	explicit VariographicDecompositionDialog(const std::vector<IJAbstractCartesianGrid *> && grids, QWidget *parent = 0);
    ~VariographicDecompositionDialog();

	/** Displays a single volume grid for debugging purposes. */
	static void displayGrid(const spectral::array& grid,
							const std::string& title,
							bool shiftByHalf );

Q_SIGNALS:
    void saveArray( spectral::array *gridData, IJAbstractCartesianGrid* gridWithGridSpecs );
	void error( QString message );
	void warning( QString message );
	void info( QString message );

private:
    Ui::VariographicDecompositionDialog *ui;

	/** Selector of the grid with the data. */
	IJCartesianGridSelector* m_gridSelector;

	/** Target variable. */
	IJVariableSelector* m_variableSelector;

	/** The list of available Cartesian grids. */
	std::vector< IJAbstractCartesianGrid* > m_grids;

	/** Computes the SVD factors for the given variable of the given grid.
	 * @param infoContentToKeepForSVD The ammount of information content to keep, for example, 0.95 for 95%.
	 *        The lower, the less SVD factors are generated, at the cost of accuracy.
	 */
	void doSVDonData(const spectral::array* gridInputData,
					 double infoContentToKeepForSVD,
					 std::vector<spectral::array> & svdFactors);

	/** Computes the fundamental factors for the given variable of the given grid using a Fourier image partitioning method
	 * to factorize the input image into a series of additive images.
	 * @param nTracks The greater, the more divided is the spectrum and the more fundamental frequency factors.  A good number
	 *                ranges between 10 and 30.
	 */
	void doFourierPartitioningOnData(const spectral::array* gridInputData,
									 std::vector<spectral::array> & frequencyFactors,
									 int nTracks);

	void doVariographicDecomposition2( bool useSVD );

private Q_SLOTS:
	void doVariographicDecomposition();
	//you can use this function to see the contents of large matrices and grids during debug.
	//TIP C++11: use displayGrids({A}, {"A matrix"}, {false}); to display a single object.
	void displayGrids(const std::vector< spectral::array >& grids,
					  const std::vector< std::string >& titles,
					  const std::vector< bool >& shiftByHalves );
    void onSumOfFactorsWasComputed(spectral::array *gridData); //called to save grid data as a Cartesian grid
    void doVariographicDecomposition2();
	void doVariographicDecomposition3();
};

#endif // VARIOGRAPHICDECOMPOSITIONDIALOG_H
