#include "variographicdecompositiondialog.h"
#include "ui_variographicdecompositiondialog.h"
#include "../widgets/ijcartesiangridselector.h"
#include "../widgets/ijvariableselector.h"
#include "../ijabstractcartesiangrid.h"
#include "../ijabstractvariable.h"
#include "../imagejockeyutils.h"
#include "spectral/svd.h"
#include "../svd/svdfactortree.h"
#include "../svd/svdfactor.h"
#include "../svd/svdanalysisdialog.h"
#include "../widgets/ijquick3dviewer.h"

#include <QMessageBox>
#include <QProgressDialog>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <limits>
#include <mutex>
#include <vtkPolyData.h>
#include <vtkDelaunay2D.h>
#include <vtkCleanPolyData.h>

std::mutex mutexObjectiveFunction;

struct objectiveFunctionFactors{
    double f1, f2, f3, f4, f5, f6, f7;
};

/** The objective function for the optimization process (SVD on varmap).
 * See complete theory in the program manual for in-depth explanation of the method's parameters below.
 * @param originalGrid  The grid with original data for comparison.
 * @param vectorOfParameters The column-vector with the free paramateres.
 * @param A The LHS of the linear system originated from the information conservation constraints.
 * @param Adagger The pseudo-inverse of A.
 * @param B The RHS of the linear system originated from the information conservation constraints.
 * @param I The identity matrix compatible with the formula: [a] = Adagger.B + (I-Adagger.A)[w]
 * @param m The desired number of geological factors.
 * @param fundamentalFactors  The list with the original data's fundamental factors computed with SVD.
 * @param fftOriginalGridMagAndPhase The Fourier image of the original data in polar form.
 * @return A distance/difference measure.
 */
//TODO: PERFORMANCE: F is costly and is called often.  Invest optimization effort here first.
double F(const spectral::array &originalGrid,
		 const spectral::array &vectorOfParameters,
		 const spectral::array &A,
		 const spectral::array &Adagger,
		 const spectral::array &B,
		 const spectral::array &I,
		 const int m,
		 const std::vector<spectral::array> &fundamentalFactors,
		 const spectral::complex_array& fftOriginalGridMagAndPhase,
		 const bool addSparsityPenalty,
         const bool addOrthogonalityPenalty,
         const double sparsityThreshold)
{
	std::unique_lock<std::mutex> lck (mutexObjectiveFunction, std::defer_lock);

	int nI = originalGrid.M();
	int nJ = originalGrid.N();
	int nK = originalGrid.K();
	int n = fundamentalFactors.size();

	//Compute the vector of weights [a] = Adagger.B + (I-Adagger.A)[w]
	spectral::array va;
	{
		Eigen::MatrixXd eigenAdagger = spectral::to_2d( Adagger );
		Eigen::MatrixXd eigenB = spectral::to_2d( B );
		Eigen::MatrixXd eigenI = spectral::to_2d( I );
		Eigen::MatrixXd eigenA = spectral::to_2d( A );
		Eigen::MatrixXd eigenvw = spectral::to_2d( vectorOfParameters );
		Eigen::MatrixXd eigenva = eigenAdagger * eigenB + ( eigenI - eigenAdagger * eigenA ) * eigenvw;
		va = spectral::to_array( eigenva );
	}

	//Compute the sparsity of the solution matrix
	double sparsityPenalty = 1.0;
	if( addSparsityPenalty ){
		int nNonZeros = va.size();
		for (int i = 0; i < va.size(); ++i )
            if( std::abs(va.d_[i]) <= sparsityThreshold )
				--nNonZeros;
		sparsityPenalty = nNonZeros/(double)va.size();
	}

	//Make the m geological factors (expected variographic structures)
	std::vector< spectral::array > geologicalFactors;
	{
		for( int iGeoFactor = 0; iGeoFactor < m; ++iGeoFactor){
			spectral::array geologicalFactor( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
			for( int iSVDFactor = 0; iSVDFactor < n; ++iSVDFactor){
				geologicalFactor += fundamentalFactors[iSVDFactor] * va.d_[ iGeoFactor * m + iSVDFactor ];
			}
			geologicalFactors.push_back( std::move( geologicalFactor ) );
		}
	}

	//Compute the grid derived form the geological factors (ideally it must match the input grid)
	spectral::array derivedGrid( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
	{
		//Sum up all geological factors.
		spectral::array sum( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
		std::vector< spectral::array >::iterator it = geologicalFactors.begin();
		for( ; it != geologicalFactors.end(); ++it ){
			sum += *it;
		}
		//Compute FFT of the sum
		spectral::complex_array tmp;
		lck.lock();                   //
		spectral::foward( tmp, sum ); //fftw crashes when called simultaneously
		lck.unlock();                 //
		//inbue the sum's FFT with the phase field of the original data.
		for( int idx = 0; idx < tmp.size(); ++idx)
		{
			std::complex<double> value;
			//get the complex number value in rectangular form
			value.real( tmp.d_[idx][0] ); //real part
			value.imag( tmp.d_[idx][1] ); //imaginary part
			//convert to polar form
			//but phase is replaced with that of the original data
			tmp.d_[idx][0] = std::abs( value ); //magnitude part
			tmp.d_[idx][1] = fftOriginalGridMagAndPhase.d_[idx][1]; //std::arg( value ); //phase part (this should be zero all over the grid)
			//convert back to rectangular form (recall that the varmap holds covariance values, then it is necessary to take its square root)
			value = std::polar( std::sqrt(tmp.d_[idx][0]), tmp.d_[idx][1] );
			tmp.d_[idx][0] = value.real();
			tmp.d_[idx][1] = value.imag();
		}
		//Compute RFFT (with the phase of the original data imbued)
		spectral::array rfftResult( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
		lck.lock();                            //
		spectral::backward( rfftResult, tmp ); //fftw crashes when called simultaneously
		lck.unlock();                          //
		//Divide the RFFT result (due to fftw3's RFFT implementation) by the number of grid cells
		rfftResult = rfftResult * (1.0/(nI*nJ*nK));
		derivedGrid += rfftResult;
	}

	//Compute the penalty caused by the angles between the vectors formed by the fundamental factors in each geological factor
	//The more orthogonal (angle == PI/2) the better.  Low angles result in more penalty.
	//The penalty value equals the smallest angle in radians between any pair of weights vectors.
	double orthogonalityPenalty = 1.0;
	if( addOrthogonalityPenalty ){
		//make the vectors of weights for each geological factor
		std::vector<spectral::array*> vectors;
		spectral::array* currentVector = new spectral::array();
		for( size_t i = 0; i < va.d_.size(); ++i){
			if( currentVector->size() < n )
				currentVector->d_.push_back( va.d_[i] );
			else{
				vectors.push_back( currentVector );
				currentVector = new spectral::array();
			}
		}
		//compute the penalty
		for( size_t i = 0; i < vectors.size()-1; ++i ){
			for( size_t j = i+1; j < vectors.size(); ++j ){
				spectral::array* vectorA = vectors[i];
				spectral::array* vectorB = vectors[j];
				double angle = spectral::angle( *vectorA, *vectorB );
				if( angle < orthogonalityPenalty )
					orthogonalityPenalty = angle;
			}
		}
		orthogonalityPenalty = 1.0 - orthogonalityPenalty/1.571; //1.571 radians ~ 90 degrees
		//cleanup the vectors
		for( size_t i = 0; i < vectors.size(); ++i )
			delete vectors[i];
	}

	//Return the measure of difference between the original data and the derived grid
	// The measure is multiplied by a factor that is a function of weights vector angle penalty (the more close to orthogonal the less penalty )
	return spectral::sumOfAbsDifference( originalGrid, derivedGrid )
		   * sparsityPenalty
		   * orthogonalityPenalty;
}

/** The objective function for the optimization process (SVD on original data).
 * See complete theory in the program manual for in-depth explanation of the method's parameters below.
 * @param originalGrid  The grid with original data for comparison.
 * @param vectorOfParameters The column-vector with the free paramateres.
 * @param A The LHS of the linear system originated from the information conservation constraints.
 * @param Adagger The pseudo-inverse of A.
 * @param B The RHS of the linear system originated from the information conservation constraints.
 * @param I The identity matrix compatible with the formula: [a] = Adagger.B + (I-Adagger.A)[w]
 * @param m The desired number of geological factors.
 * @param fundamentalFactors  The list with the original data's fundamental factors computed with SVD.
 * @return A distance/difference measure.
 */
double F2(const spectral::array &originalGrid,
         const spectral::array &vectorOfParameters,
         const spectral::array &A,
         const spectral::array &Adagger,
         const spectral::array &B,
         const spectral::array &I,
         const int m,
         const std::vector<spectral::array> &fundamentalFactors,
         const bool addSparsityPenalty,
         const bool addOrthogonalityPenalty,
		 const double sparsityThreshold,
		 const int nSkipOutermost,
         const int nIsosurfs,
         const int nMinIsoVertexes,
         const objectiveFunctionFactors& off )
{

	// A mutex to create critical sections to avoid crashes in multithreaded calls.
	std::unique_lock<std::mutex> lck (mutexObjectiveFunction, std::defer_lock);

	///Visualizing the results on the fly is optional/////////////
	lck.lock();
	static IJQuick3DViewer* q3Dv[50]{}; //initializes all elements to zero (null pointer)
	for( int i = 0; i < m; ++i){
		if( ! q3Dv[i] )
			q3Dv[i] = new IJQuick3DViewer();
		q3Dv[i]->show();
	}
	lck.unlock();
	///////////////////////////////////////////////////////////////

	int nI = originalGrid.M();
    int nJ = originalGrid.N();
    int nK = originalGrid.K();
    int n = fundamentalFactors.size();

    //Compute the vector of weights [a] = Adagger.B + (I-Adagger.A)[w]
    spectral::array va;
    {
        Eigen::MatrixXd eigenAdagger = spectral::to_2d( Adagger );
        Eigen::MatrixXd eigenB = spectral::to_2d( B );
        Eigen::MatrixXd eigenI = spectral::to_2d( I );
        Eigen::MatrixXd eigenA = spectral::to_2d( A );
        Eigen::MatrixXd eigenvw = spectral::to_2d( vectorOfParameters );
        Eigen::MatrixXd eigenva = eigenAdagger * eigenB + ( eigenI - eigenAdagger * eigenA ) * eigenvw;
        va = spectral::to_array( eigenva );
    }

	//Compute the sparsity of the solution matrix
    double sparsityPenalty = 1.0;
    if( addSparsityPenalty ){
        int nNonZeros = va.size();
        for (int i = 0; i < va.size(); ++i )
            if( std::abs(va.d_[i]) <= sparsityThreshold  )
                --nNonZeros;
        sparsityPenalty = nNonZeros/(double)va.size() * 1.0;
    }

	//Make the m geological factors (data decomposed into grids with features with different spatial correlation)
	//The geological factors are linear combinations of fundamental factors whose weights are given by the array va.
    std::vector< spectral::array > geologicalFactors;
    {
        for( int iGeoFactor = 0; iGeoFactor < m; ++iGeoFactor){
            spectral::array geologicalFactor( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
            for( int iSVDFactor = 0; iSVDFactor < n; ++iSVDFactor){
                geologicalFactor += fundamentalFactors[iSVDFactor] * va.d_[ iGeoFactor * m + iSVDFactor ];
            }
            geologicalFactors.push_back( std::move( geologicalFactor ) );
        }
    }

	//Get the Fourier transforms of the geological factors
	std::vector< spectral::complex_array > geologicalFactorsFTs;
	{
		std::vector< spectral::array >::iterator it = geologicalFactors.begin();
		for( ; it != geologicalFactors.end(); ++it ){
			spectral::array& geologicalFactor = *it;
			spectral::complex_array tmp;
			lck.lock();                                //
			spectral::foward( tmp, geologicalFactor ); //fftw crashes when called simultaneously
			lck.unlock();                              //
			geologicalFactorsFTs.push_back( std::move( tmp ));
		}
	}

	//Get the varmaps from the FTs of the geological factors.
	std::vector< spectral::array > geologicalFactorsVarmaps;
	{
		std::vector< spectral::complex_array >::iterator it = geologicalFactorsFTs.begin();
		for( ; it != geologicalFactorsFTs.end(); ++it )
		{
			spectral::complex_array& geologicalFactorFT = *it;
			spectral::array gridVarmap( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
			{
				spectral::complex_array gridNormSquaredAndZeroPhase( nI, nJ, nK );
				for(int k = 0; k < nK; ++k) {
					for(int j = 0; j < nJ; ++j){
						for(int i = 0; i < nI; ++i){
							std::complex<double> value;
							//the scan order of fftw follows is the opposite of the GSLib convention
							int idx = k + nK * (j + nJ * i );
							//compute the complex number norm squared
							double normSquared = geologicalFactorFT.d_[idx][0]*geologicalFactorFT.d_[idx][0] +
												 geologicalFactorFT.d_[idx][1]*geologicalFactorFT.d_[idx][1];
							double phase = 0.0;
							//convert to rectangular form
							value = std::polar( normSquared, phase );
							//save the rectangular form in the grid
							gridNormSquaredAndZeroPhase.d_[idx][0] = value.real();
							gridNormSquaredAndZeroPhase.d_[idx][1] = value.imag();
						}
					}
				}
				lck.lock();                                                    //
				spectral::backward( gridVarmap, gridNormSquaredAndZeroPhase ); //fftw crashes when called simultaneously
				lck.unlock();                                                  //
			}
			//divide the varmap (due to fftw3's RFFT implementation) values by the number of cells of the grid.
			gridVarmap = gridVarmap * (1.0/(nI*nJ*nK));
			geologicalFactorsVarmaps.push_back( std::move( gridVarmap ) );
		}
	}

	//Get isocontours/isosurfaces from the varmaps.
    std::vector< vtkSmartPointer<vtkPolyData> > geolgicalFactorsVarmapsIsosurfaces;
	{
		std::vector< spectral::array >::iterator it = geologicalFactorsVarmaps.begin();
		for( int i = 0 ; it != geologicalFactorsVarmaps.end(); ++it, ++i )
		{
            // Get the geological factor's varmap.
			spectral::array& geologicalFactorVarmap = *it;
            // Get the geological factor's varmap with h=0 in the center of the grid.
			spectral::array geologicalFactorVarmapShifted = spectral::shiftByHalf( geologicalFactorVarmap );
            // Get the isocontour/isosurface.
			lck.lock(); // not all VTK algorithms are thread-safe, so put all VTK-using code in a critical zone just in case.
			vtkSmartPointer<vtkPolyData> poly;
			{
				poly = ImageJockeyUtils::computeIsosurfaces( geologicalFactorVarmapShifted,
															 nIsosurfs,
															 geologicalFactorVarmapShifted.min(),
															 geologicalFactorVarmapShifted.max() );
				// Get the isomap's bounding box.
				double bbox[6];
				poly->GetBounds( bbox );

				// Remove open isocontours/isosurfaces.
				//TODO: currently ineffective with 3D models (isosurfaces)
				ImageJockeyUtils::removeOpenPolyLines( poly );

				// Remove the non-concentric iscontours/isosurfaces.
                ImageJockeyUtils::removeNonConcentricPolyLines( poly,
																(bbox[1]+bbox[0])/2,
																(bbox[3]+bbox[2])/2,
																(bbox[5]+bbox[4])/2,
																 1.0,
                                                                 nMinIsoVertexes );
				///Visualizing the results on the fly is optional/////////////
				q3Dv[i]->clearScene();
				q3Dv[i]->display( poly, 0, 255, 255 );
				//////////////////////////////////////////////////////////////
			}
			lck.unlock();

			geolgicalFactorsVarmapsIsosurfaces.push_back( poly );
        }
	}

	// Fit ellipses to the isocontours/isosurfaces of the varmaps, computing the fitting error.
	double objectiveFunctionValue = 0.0;
    double angle_variance_mean = 0.0; // mean of the variances of the ellipses angles in the geological factors. Zero is ideal.
    double ratio_variance_mean = 0.0; // mean of the variances of the ellipses aspect ratio in the geological factors. Zero is ideal.
    double angle_mean_variance = 0.0; // variance of the means of the ellipses angles in the geological factors.  The greater, the better.
    double ratio_mean_variance = 0.0; // variance of the means of the ellipses aspect ratio in the geological factors. The greater, the better.
    {
        // Collect the ellipse angle and ratio means of each geological factor.
        std::vector<double> angle_means, ratio_means;

        // For each geological factor.
		std::vector< vtkSmartPointer<vtkPolyData> >::iterator it = geolgicalFactorsVarmapsIsosurfaces.begin();
		for( int i = 0 ; it != geolgicalFactorsVarmapsIsosurfaces.end(); ++it, ++i )
		{
			// Get the isocontours/isosurfaces.
			vtkSmartPointer<vtkPolyData> isos = *it;

			// Fit ellipses to them.
			lck.lock(); // not all VTK algorithms are thread-safe, so put all VTK-using code in a critical zone just in case.
			{
				vtkSmartPointer<vtkPolyData> ellipses = vtkSmartPointer<vtkPolyData>::New(); //nullptr == disables display of ellipses == faster execution.
				double max_error, mean_error, sum_error;
                double angle_variance, ratio_variance, angle_mean, ratio_mean;
				ImageJockeyUtils::fitEllipses( isos, ellipses, mean_error, max_error, sum_error, angle_variance, ratio_variance, angle_mean, ratio_mean, nSkipOutermost );
				objectiveFunctionValue += sum_error;
                angle_variance_mean += angle_variance;
                ratio_variance_mean += ratio_variance;
                angle_means.push_back( angle_mean );
                ratio_means.push_back( ratio_mean );
				///Visualizing the results on the fly is optional/////////////
				q3Dv[i]->display( ellipses, 255, 0, 0 );
				//////////////////////////////////////////////////////////////
			}
			lck.unlock();
		}

        // Compute the means of the variances of ellipses angle and ratio all geological factors.
        angle_variance_mean /= n;
        ratio_variance_mean /= n;

        // Compute the variances of the ellipses angle and ratio means of each geological factor.
        double unused;
        ImageJockeyUtils::getStats( angle_means, angle_mean_variance, unused );
        ImageJockeyUtils::getStats( ratio_means, ratio_mean_variance, unused );
    }

    //Compute the penalty caused by the angles between the vectors formed by the fundamental factors in each geological factor
    //The more orthogonal (angle == PI/2) the better.  Low angles result in more penalty.
    //The penalty value equals the smallest angle in radians between any pair of weights vectors.
    double orthogonalityPenalty = 1.0;
    if( addOrthogonalityPenalty ){
        //make the vectors of weights for each geological factor
        std::vector<spectral::array*> vectors;
        spectral::array* currentVector = new spectral::array();
		for( size_t i = 0; i < va.d_.size(); ++i){
            if( currentVector->size() < n )
                currentVector->d_.push_back( va.d_[i] );
            else{
                vectors.push_back( currentVector );
                currentVector = new spectral::array();
            }
        }
        //compute the penalty
		for( size_t i = 0; i < vectors.size()-1; ++i ){
			for( size_t j = i+1; j < vectors.size(); ++j ){
                spectral::array* vectorA = vectors[i];
                spectral::array* vectorB = vectors[j];
                double angle = spectral::angle( *vectorA, *vectorB );
                if( angle < orthogonalityPenalty )
                    orthogonalityPenalty = angle;
            }
        }
        orthogonalityPenalty = 1.0 - orthogonalityPenalty/1.571; //1.571 radians ~ 90 degrees
        //cleanup the vectors
		for( size_t i = 0; i < vectors.size(); ++i )
            delete vectors[i];
    }

    // Finally, return the objective function value.
    return  std::pow( objectiveFunctionValue,    off.f1 ) *
            std::pow( sparsityPenalty,           off.f2 ) *
            std::pow( orthogonalityPenalty,      off.f3 ) *
            std::pow( angle_variance_mean,       off.f4 ) *
            std::pow( ratio_variance_mean,       off.f5 ) *
            std::pow( angle_mean_variance,       off.f6 ) *
            std::pow( ratio_mean_variance,       off.f7 ) ;
}

/**
 * The code for multithreaded gradient vector calculation for objective function F().
 */
void taskOnePartialDerivative(
							   const spectral::array& vw,
							   const std::vector< int >& parameterIndexBin,
							   const double epsilon,
							   const spectral::array* gridData,
							   const spectral::array& A,
							   const spectral::array& Adagger,
							   const spectral::array& B,
							   const spectral::array& I,
							   const int m,
							   const std::vector< spectral::array >& svdFactors,
							   const spectral::complex_array& gridMagnitudeAndPhaseParts,
							   const bool addSparsityPenalty,
							   const bool addOrthogonalityPenalty,
                               const double sparsityThreshold,
							   spectral::array* gradient //output object: for some reason, the thread object constructor does not compile with non-const references.
							   ){
	std::vector< int >::const_iterator it = parameterIndexBin.cbegin();
	for(; it != parameterIndexBin.cend(); ++it ){
		int iParameter = *it;
		//Make a set of parameters slightly shifted to the right (more positive) along one parameter.
		spectral::array vwFromRight( vw );
		vwFromRight(iParameter) = vwFromRight(iParameter) + epsilon;
		//Make a set of parameters slightly shifted to the left (more negative) along one parameter.
		spectral::array vwFromLeft( vw );
		vwFromLeft(iParameter) = vwFromLeft(iParameter) - epsilon;
		//Compute (numerically) the partial derivative with respect to one parameter.
        (*gradient)(iParameter) = (F( *gridData, vwFromRight, A, Adagger, B, I, m, svdFactors, gridMagnitudeAndPhaseParts, addSparsityPenalty, addOrthogonalityPenalty, sparsityThreshold )
									 -
                                   F( *gridData, vwFromLeft, A, Adagger, B, I, m, svdFactors, gridMagnitudeAndPhaseParts, addSparsityPenalty, addOrthogonalityPenalty, sparsityThreshold ))
									 /
								   ( 2 * epsilon );
	}
}

/**
 * The code for multithreaded gradient vector calculation for objective function F2().
 */
void taskOnePartialDerivative2(
							   const spectral::array& vw,
							   const std::vector< int >& parameterIndexBin,
							   const double epsilon,
							   const spectral::array* gridData,
							   const spectral::array& A,
							   const spectral::array& Adagger,
							   const spectral::array& B,
							   const spectral::array& I,
							   const int m,
							   const std::vector< spectral::array >& svdFactors,
							   const bool addSparsityPenalty,
							   const bool addOrthogonalityPenalty,
                               const double sparsityThreshold,
							   const int nSkipOutermost,
							   const int nIsosurfs,
                               const int nMinIsoVertexes,
                               const objectiveFunctionFactors& off,
							   spectral::array* gradient //output object: for some reason, the thread object constructor does not compile with non-const references.
							   ){
	std::vector< int >::const_iterator it = parameterIndexBin.cbegin();
	for(; it != parameterIndexBin.cend(); ++it ){
		int iParameter = *it;
		//Make a set of parameters slightly shifted to the right (more positive) along one parameter.
		spectral::array vwFromRight( vw );
		vwFromRight(iParameter) = vwFromRight(iParameter) + epsilon;
		//Make a set of parameters slightly shifted to the left (more negative) along one parameter.
		spectral::array vwFromLeft( vw );
		vwFromLeft(iParameter) = vwFromLeft(iParameter) - epsilon;
		//Compute (numerically) the partial derivative with respect to one parameter.
        (*gradient)(iParameter) = (F2( *gridData, vwFromRight, A, Adagger, B, I, m, svdFactors, addSparsityPenalty, addOrthogonalityPenalty, sparsityThreshold, nSkipOutermost, nIsosurfs, nMinIsoVertexes, off )
									 -
                                   F2( *gridData, vwFromLeft, A, Adagger, B, I, m, svdFactors, addSparsityPenalty, addOrthogonalityPenalty, sparsityThreshold, nSkipOutermost, nIsosurfs, nMinIsoVertexes, off ))
									 /
								   ( 2 * epsilon );
	}
}



VariographicDecompositionDialog::VariographicDecompositionDialog(const std::vector<IJAbstractCartesianGrid *> &&grids, QWidget *parent) :
    QDialog(parent),
	ui(new Ui::VariographicDecompositionDialog),
	m_grids( std::move( grids ) )
{
    ui->setupUi(this);

	//deletes dialog from memory upon user closing it
	this->setAttribute(Qt::WA_DeleteOnClose);

	setWindowTitle( "Variographic Decomposition" );

	//the combo box to choose a Cartesian grid containing a Fourier image
    m_gridSelector = new IJCartesianGridSelector( m_grids );
	ui->frmGridSelectorPlaceholder->layout()->addWidget( m_gridSelector );

	//the combo box to choose the variable
	m_variableSelector = new IJVariableSelector();
	ui->frmAttributeSelectorPlaceholder->layout()->addWidget( m_variableSelector );
	connect( m_gridSelector, SIGNAL(cartesianGridSelected(IJAbstractCartesianGrid*)),
			 m_variableSelector, SLOT(onListVariables(IJAbstractCartesianGrid*)) );

    //calling this slot causes the variable comboboxes to update, so they show up populated
    //otherwise the user is required to choose another file and then back to the first file
    //if the desired sample file happens to be the first one in the list.
    m_gridSelector->onSelection( 0 );

	//get the number of threads from logical CPUs or number of free parameters (whichever is the lowest)
	ui->spinNumberOfThreads->setValue( (int)std::thread::hardware_concurrency() );
}

VariographicDecompositionDialog::~VariographicDecompositionDialog()
{
    delete ui;
}

void VariographicDecompositionDialog::doVariographicDecomposition()
{
	// Get the data objects.
	IJAbstractCartesianGrid* grid = m_gridSelector->getSelectedGrid();
	IJAbstractVariable* variable = m_variableSelector->getSelectedVariable();

	// Get the grid's dimensions.
	unsigned int nI = grid->getNI();
	unsigned int nJ = grid->getNJ();
	unsigned int nK = grid->getNK();

	// Fetch data from the data source.
	grid->dataWillBeRequested();

	// The user-given number of geological factors (m).
	int m = ui->spinNumberOfGeologicalFactors->value();

    // The user-given epsilon (useful for numerical calculus).
	double epsilon = std::pow(10, ui->spinLogEpsilon->value() );

	// The other user-given parameters
	int maxNumberOfOptimizationSteps = ui->spinMaxSteps->value();
	double initialAlpha = ui->spinInitialAlpha->value();
	int maxNumberOfAlphaReductionSteps = ui->spinMaxStepsAlphaReduction->value();
	double convergenceCriterion = std::pow(10, ui->spinConvergenceCriterion->value() );
	double infoContentToKeepForSVD = ui->spinInfoContentToKeepForSVD->value() / 100.0;
	bool addSparsityPenalty = ui->chkEnableSparsityPenalty->isChecked();
	bool addOrthogonalityPenalty = ui->chkEnableOrthogonalityPenalty->isChecked();
    double sparsityThreshold = ui->spinSparsityThreshold->value();

	//-------------------------------------------------------------------------------------------------
	//-----------------------------------PREPARATION STEPS---------------------------------------------
	//-------------------------------------------------------------------------------------------------

	// PRODUCTS: 1) grid with phase of FFT transform of the input variable.
	//           2) collection of grids with the fundamental SVD factors of the variable's varmap.
	//           3) n: number of fundamental factors.
	spectral::complex_array gridMagnitudeAndPhaseParts( nI, nJ, nK );
	std::vector< spectral::array > svdFactors;
	int n = 0;
	{
        // Perform FFT to get a grid of complex numbers in rectangular form: real part (a), imaginary part (b).
		// PRODUCTS: 2 grids: real and imaginary parts.
		spectral::complex_array gridRealAndImaginaryParts;
		{
			QProgressDialog progressDialog;
			progressDialog.setRange(0,0);
			progressDialog.show();
			progressDialog.setLabelText("Computing FFT...");
			QCoreApplication::processEvents(); //let Qt repaint widgets
			spectral::array *gridData = grid->createSpectralArray( variable->getIndexInParentGrid() );
			spectral::foward( gridRealAndImaginaryParts, *gridData );
			delete gridData;
		}

		// 1) Convert real and imaginary parts to magnitude and phase (phi).
		// 2) Make ||z|| = zz* = (a+bi)(a-bi) = a^2+b^2 (Convolution Theorem: a convolution reduces to a cell-to-cell product in frequency domain).
		// 3) RFFT of ||z|| as magnitude and a grid filled with zeros as phase (zero phase).
		// PRODUCTS: 3 grids: magnitude and phase parts; variographic map.
		spectral::array gridVarmap( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
		{
			QProgressDialog progressDialog;
			progressDialog.setRange(0,0);
			progressDialog.show();
			progressDialog.setLabelText("Converting FFT results to polar form...");
			spectral::complex_array gridNormSquaredAndZeroPhase( nI, nJ, nK );
			QCoreApplication::processEvents(); //let Qt repaint widgets
			for(unsigned int k = 0; k < nK; ++k) {
				for(unsigned int j = 0; j < nJ; ++j){
					for(unsigned int i = 0; i < nI; ++i){
						std::complex<double> value;
						//the scan order of fftw follows is the opposite of the GSLib convention
						int idx = k + nK * (j + nJ * i );
						//get the complex number values
						value.real( gridRealAndImaginaryParts.d_[idx][0] ); //real part
						value.imag( gridRealAndImaginaryParts.d_[idx][1] ); //imaginary part
						//fills the output array with the angular form
						gridMagnitudeAndPhaseParts.d_[idx][0] = std::abs( value ); //magnitude part
						gridMagnitudeAndPhaseParts.d_[idx][1] = std::arg( value ); //phase part
						//compute the complex number norm squared
						double normSquared = gridRealAndImaginaryParts.d_[idx][0]*gridRealAndImaginaryParts.d_[idx][0] +
											 gridRealAndImaginaryParts.d_[idx][1]*gridRealAndImaginaryParts.d_[idx][1];
						double phase = 0.0;
						//convert to rectangular form
                        value = std::polar( normSquared, phase );
						//save the rectangular form in the grid
						gridNormSquaredAndZeroPhase.d_[idx][0] = value.real();
						gridNormSquaredAndZeroPhase.d_[idx][1] = value.imag();
					}
				}
			}
			progressDialog.setLabelText("Computing RFFT to get varmap...");
			QCoreApplication::processEvents(); //let Qt repaint widgets
			spectral::backward( gridVarmap, gridNormSquaredAndZeroPhase );
		}

        //divide the varmap (due to fftw3's RFFT implementation) values by the number of cells of the grid.
        gridVarmap = gridVarmap * (1.0/(nI*nJ*nK));

		//Compute SVD of varmap
		{
			//Get the number of usable fundamental SVD factors.
			{
				QProgressDialog progressDialog;
				progressDialog.setRange(0,0);
				progressDialog.setLabelText("Computing SVD factors of varmap...");
				progressDialog.show();
				QCoreApplication::processEvents();
				doSVDonData( &gridVarmap, infoContentToKeepForSVD, svdFactors );
				n = svdFactors.size();
				progressDialog.hide();
			}
		}
	}

	//---------------------------------------------------------------------------------------------------------------------------
	//---------------------------------------SET UP THE LINEAR SYSTEM TO IMPOSE THE CONSERVATION CONSTRAINTS---------------------
	//---------------------------------------------------------------------------------------------------------------------------

	//Make the A matrix of the linear system originated by the information
	//conservation constraint (see program manual for the complete theory).
	//elements are initialized to zero.
	spectral::array A( (spectral::index)n, (spectral::index)m*n );
	for( int line = 0; line < n; ++line )
		for( int column = line * m; column < ((line+1) * m); ++column)
			 A( line, column ) = 1.0;

	//Make the B matrix of said system
	spectral::array B( (spectral::index)n, (spectral::index)1 );
	for( int line = 0; line < n; ++line )
		B( line, 0 ) = 1.0;

    //Make the identity matrix to find the fundamental factors weights [a] from the parameters w:
    //     [a] = Adagger.B + (I-Adagger.A)[w]
	spectral::array I( (spectral::index)m*n, (spectral::index)m*n );
	for( int i = 0; i < m*n; ++i )
		I( i, i ) = 1.0;

	//Get the U, Sigma and V* matrices from SVD on A.
	spectral::SVD svd = spectral::svd( A );
	spectral::array U = svd.U();
	spectral::array Sigma = svd.S();
    spectral::array V = svd.V(); //SVD yields V* already transposed, that is V, but to check, you must transpose V
                                 //to get A = U.Sigma.V*

	//Make a full Sigma matrix (to be compatible with multiplication with the other matrices)
	{
		spectral::array SigmaTmp( (spectral::index)n, (spectral::index)m*n );
		for( int i = 0; i < n; ++i)
			SigmaTmp(i, i) = Sigma.d_[i];
		Sigma = SigmaTmp;
	}

	//Make U*
	//U contains only real numbers, thus U's transpose conjugate is its transpose.
	spectral::array Ustar( U );
	//transpose U to get U*
	{
		Eigen::MatrixXd tmp = spectral::to_2d( Ustar );
		tmp.transposeInPlace();
		Ustar = spectral::to_array( tmp );
	}

	//Make Sigmadagger (pseudoinverse of Sigma)
	spectral::array SigmaDagger( Sigma );
	{
		//compute reciprocals of the non-zero elements in the main diagonal.
		for( int i = 0; i < n; ++i){
			double value = SigmaDagger(i, i);
			//only absolute values greater than the machine epsilon are considered non-zero.
			if( std::abs( value ) > std::numeric_limits<double>::epsilon() )
				SigmaDagger( i, i ) = 1.0 / value;
			else
				SigmaDagger( i, i ) = 0.0;
		}
		//transpose
		Eigen::MatrixXd tmp = spectral::to_2d( SigmaDagger );
		tmp.transposeInPlace();
		SigmaDagger = spectral::to_array( tmp );
	}

	//Make Adagger (pseudoinverse of A) by "reversing" the transform U.Sigma.V*,
	//hence, Adagger = V.Sigmadagger.Ustar .
	spectral::array Adagger;
	{
		Eigen::MatrixXd eigenV = spectral::to_2d( V );
		Eigen::MatrixXd eigenSigmadagger = spectral::to_2d( SigmaDagger );
		Eigen::MatrixXd eigenUstar = spectral::to_2d( Ustar );
		Eigen::MatrixXd eigenAdagger = eigenV * eigenSigmadagger * eigenUstar;
		Adagger = spectral::to_array( eigenAdagger );
	}

    //Initialize the vector of linear system parameters [w]=[0]
	spectral::array vw( (spectral::index)m*n );

	//---------------------------------------------------------------------------------------------------------------
	//-------------------------SIMULATED ANNEALING TO INITIALIZE THE PARAMETERS [w] NEAR A GLOBAL MINIMUM------------
	//---------------------------------------------------------------------------------------------------------------
	{
		//...................................Annealing Parameters.................................
		//Intial temperature.
		double f_Tinitial = ui->spinInitialTemperature->value();
		//Final temperature.
		double f_Tfinal = ui->spinFinalTemperature->value();
		//Max number of SA steps.
		int i_kmax = ui->spinMaxStepsSA->value();
		//Minimum value allowed for the parameters w (all zeros). DOMAIN CONSTRAINT
		spectral::array L_wMin( vw.size(), 0.0d );
		//Maximum value allowed for the parameters w (all ones). DOMAIN CONSTRAINT
		spectral::array L_wMax( vw.size(), 1.0d );
		/*Factor used to control the size of the random state “hop”.  For example, if the maximum “hop” must be
		 10% of domain size, set 0.1.  Small values (e.g. 0.001) result in slow, but more accurate convergence.
		 Large values (e.g. 100.0) covers more space faster, but falls outside the domain are more frequent,
		 resulting in more re-searches due to more invalid parameter value penalties. */
		double f_factorSearch = ui->spinMaxHopFactor->value();
		//Intialize the random number generator with the same seed
		std::srand ((unsigned)ui->spinSeed->value());
		//.................................End of Annealing Parameters.............................
		//Returns the current “temperature” of the system.  It yields a log curve that decays as the step number increase.
		// The initial temperature plays an important role: curve starting with 5.000 is steeper than another that starts with 1.000.
		//  This means the the lower the temperature, the more linear the temperature decreases.
		// i_stepNumber: the current step number of the annealing process ( 0 = first ).
		auto temperature = [=](int i_stepNumber) { return f_Tinitial * std::exp( -i_stepNumber / (double)1000 * (1.5 * std::log10( f_Tinitial ) ) ); };
		/*Returns the probability of acceptance of the energy state for the next iteration.
		  This allows acceptance of higher values to break free from local minima.
		  f_eCurrent: current energy of the system.
		  f_eNew: energy level of the next step.
		  f_T: current “temperature” of the system.*/
		auto probAcceptance = [=]( double f_eCurrent, double f_eNewLocal, double f_T ) {
		   //If the new state is more energetic, calculates a probability of acceptance
		   //which is as high as the current “temperature” of the process.  The “temperature”
		   //diminishes with iterations.
		   if ( f_eNewLocal > f_eCurrent )
			  return ( f_T - f_Tfinal ) / ( f_Tinitial - f_Tfinal );
		   //If the new state is less energetic, the probability of acceptance is 100% (natural search for minima).
		   else
			  return 1.0;
		};
		//Get the number of parameters.
		int i_nPar = vw.size();
		//Make a copy of the initial state (parameter set.
		spectral::array L_wCurrent( vw );
		//The parameters variations (maxes - mins)
		spectral::array L_wDelta = L_wMax - L_wMin;
		//Get the input data.
		spectral::array *gridData = grid->createSpectralArray( variable->getIndexInParentGrid() );
		//...................Main annealing loop...................
		QProgressDialog progressDialog;
		progressDialog.setRange(0,0);
		progressDialog.show();
		progressDialog.setLabelText("Simulated Annealing in progress...");
		QCoreApplication::processEvents();
		double f_eNew = std::numeric_limits<double>::max();
		double f_lowestEnergyFound = std::numeric_limits<double>::max();
		spectral::array L_wOfLowestEnergyFound;
		int k = 0;
		for( ; k < i_kmax; ++k ){
			emit info( "Commencing SA step #" + QString::number( k ) );
			//Get current temperature.
			double f_T = temperature( k );
			//Quit if temperature is lower than the minimum annealing temperature.
			if( f_T < f_Tfinal )
				break;
			//Randomly searches for a neighboring state with respect to current state.
			spectral::array L_wNew(L_wCurrent);
			for( int i = 0; i < i_nPar; ++i ){ //for each parameter
			   //Ensures that the values randomly obtained are inside the domain.
			   double f_tmp = 0.0;
			   while( true ){
				  double LO = L_wCurrent[i] - (f_factorSearch * L_wDelta[i]);
				  double HI = L_wCurrent[i] + (f_factorSearch * L_wDelta[i]);
				  f_tmp = LO + std::rand() / (RAND_MAX/(HI-LO)) ;
				  if ( f_tmp >= L_wMin[i] && f_tmp <= L_wMax[i] )
					 break;
			   }
			   //Updates the parameter value.
			   L_wNew[i] = f_tmp;
			}
			//Computes the “energy” of the current state (set of parameters).
			//The “energy” in this case is how different the image as given the parameters is with respect
			//the data grid, considered the reference image.
            double f_eCurrent = F( *gridData, L_wCurrent, A, Adagger, B, I, m, svdFactors, gridMagnitudeAndPhaseParts, addSparsityPenalty, addOrthogonalityPenalty, sparsityThreshold );
			//Computes the “energy” of the neighboring state.
            f_eNew = F( *gridData, L_wNew, A, Adagger, B, I, m, svdFactors, gridMagnitudeAndPhaseParts, addSparsityPenalty, addOrthogonalityPenalty, sparsityThreshold );
			//Changes states stochastically.  There is a probability of acceptance of a more energetic state so
			//the optimization search starts near the global minimum and is not trapped in local minima (hopefully).
			double f_probMov = probAcceptance( f_eCurrent, f_eNew, f_T );
			if( f_probMov >= ( (double)std::rand() / RAND_MAX ) ) {//draws a value between 0.0 and 1.0
				L_wCurrent = L_wNew; //replaces the current state with the neighboring random state
				emit info("  moved to energy level " + QString::number( f_eNew ));
				//if the energy is the record low, store it, just in case the SA loop ends without converging.
				if( f_eNew < f_lowestEnergyFound ){
					f_lowestEnergyFound = f_eNew;
					L_wOfLowestEnergyFound = spectral::array( L_wCurrent );
				}
			}
		}
		// The input data is no longer necessary.
		delete gridData;
		// Delivers the set of parameters near the global minimum (hopefully) for the Gradient Descent algorithm.
		// The SA loop may end in a higher energy state, so we return the lowest found in that case
		if( k == i_kmax && f_lowestEnergyFound < f_eNew )
			emit info( "SA completed by number of steps." );
		else
			emit info( "SA completed by reaching the lowest temperature." );
		vw = L_wOfLowestEnergyFound;
		emit info( "Using the state of lowest energy found (" + QString::number( f_lowestEnergyFound ) + ")" );
	}

	//---------------------------------------------------------------------------------------------------------
	//--------------------------------------OPTIMIZATION LOOP (GRADIENT DESCENT)-------------------------------
	//---------------------------------------------------------------------------------------------------------
	unsigned int nThreads = ui->spinNumberOfThreads->value();
	QProgressDialog progressDialog;
	progressDialog.setRange(0,0);
	progressDialog.show();
	progressDialog.setLabelText("Gradient Descent in progress...");
	QCoreApplication::processEvents();
	int iOptStep = 0;
	spectral::array va;
	for( ; iOptStep < maxNumberOfOptimizationSteps; ++iOptStep ){

		emit info( "Commencing GD step #" + QString::number( iOptStep ) );

		//Compute the vector of weights [a] = Adagger.B + (I-Adagger.A)[w] (see program manual for theory)
        {
            Eigen::MatrixXd eigenAdagger = spectral::to_2d( Adagger );
            Eigen::MatrixXd eigenB = spectral::to_2d( B );
            Eigen::MatrixXd eigenI = spectral::to_2d( I );
            Eigen::MatrixXd eigenA = spectral::to_2d( A );
            Eigen::MatrixXd eigenvw = spectral::to_2d( vw );
			Eigen::MatrixXd eigenva = eigenAdagger * eigenB + ( eigenI - eigenAdagger * eigenA ) * eigenvw;
            va = spectral::to_array( eigenva );
        }

		{
			spectral::array debug_va( va );
			debug_va.set_size( (spectral::index)n, (spectral::index)m );
		}

        //Compute the gradient vector of objective function F with the current [w] parameters.
        spectral::array gradient( vw.size() );
        {
            spectral::array *gridData = grid->createSpectralArray( variable->getIndexInParentGrid() );

			//distribute the parameter indexes among the n-threads
			std::vector<int> parameterIndexBins[nThreads];
			int parameterIndex = 0;
			for( unsigned int iThread = 0; parameterIndex < vw.size(); ++parameterIndex, ++iThread)
				parameterIndexBins[ iThread % nThreads ].push_back( parameterIndex );

			//create and run the partial derivative calculation threads
			std::thread threads[nThreads];
			for( unsigned int iThread = 0; iThread < nThreads; ++iThread){
				threads[iThread] = std::thread( taskOnePartialDerivative,
												vw,
												parameterIndexBins[iThread],
												epsilon,
												gridData,
												A,
												Adagger,
												B,
												I,
												m,
												svdFactors,
												gridMagnitudeAndPhaseParts,
												addSparsityPenalty,
												addOrthogonalityPenalty,
                                                sparsityThreshold,
												&gradient);
			}

			//wait for the threads to finish.
			for( unsigned int iThread = 0; iThread < nThreads; ++iThread)
				threads[iThread].join();

            delete gridData;
        }

        //Update the system's parameters according to gradient descent.
		double currentF = 999.0;
		double nextF = 1.0;
		{
            spectral::array *gridData = grid->createSpectralArray( variable->getIndexInParentGrid() );
			double alpha = initialAlpha;
			//halves alpha until we get a descent (current gradient vector may result in overshooting)
			int iAlphaReductionStep = 0;
			for( ; iAlphaReductionStep < maxNumberOfAlphaReductionSteps; ++iAlphaReductionStep ){
                spectral::array new_vw = vw - gradient * alpha;
                //Impose domain constraints to the parameters.
                for( int i = 0; i < new_vw.size(); ++i){
					if( new_vw.d_[i] < 0.0 )
                        new_vw.d_[i] = 0.0;
                    if( new_vw.d_[i] > 1.0 )
                        new_vw.d_[i] = 1.0;
                }
                currentF = F( *gridData, vw, A, Adagger, B, I, m, svdFactors, gridMagnitudeAndPhaseParts, addSparsityPenalty, addOrthogonalityPenalty, sparsityThreshold );
                nextF = F( *gridData, new_vw, A, Adagger, B, I, m, svdFactors, gridMagnitudeAndPhaseParts, addSparsityPenalty, addOrthogonalityPenalty, sparsityThreshold );
                if( nextF < currentF ){
                    vw = new_vw;
                    break;
                }
                alpha /= 2.0;
            }
			if( iAlphaReductionStep == maxNumberOfAlphaReductionSteps )
				emit warning( "WARNING: reached maximum alpha reduction steps." );
            delete gridData;
        }

		//Check the convergence criterion.
		double ratio = currentF / nextF;
		if( ratio  < (1.0 + convergenceCriterion) )
			break;

		emit info( "F(k)/F(k+1) ratio: " + QString::number( ratio ) );

	}
	progressDialog.hide();

	//-------------------------------------------------------------------------------------------------
	//------------------------------------PRESENT THE RESULTS------------------------------------------
	//-------------------------------------------------------------------------------------------------

	if( iOptStep == maxNumberOfOptimizationSteps )
		QMessageBox::warning( this, "Warning", "Completed by reaching maximum number of optimization steps. Check results.");
	else
		QMessageBox::information( this, "Info", "Completed by satisfaction of convergence criterion.");

	std::vector< spectral::array > grids;
	std::vector< std::string > titles;
	std::vector< bool > shiftByHalves;

	spectral::array derivedGrid( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
	{
		//Make the m geological factors (expected variographic structures)
		std::vector< spectral::array > geologicalFactors;
		{
			QProgressDialog progressDialog;
			progressDialog.setRange(0,0);
			progressDialog.show();
			progressDialog.setLabelText("Making the geological factors...");
			QCoreApplication::processEvents();
			for( int iGeoFactor = 0; iGeoFactor < m; ++iGeoFactor){
				spectral::array geologicalFactor( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
				for( int iSVDFactor = 0; iSVDFactor < n; ++iSVDFactor){
					double weight = va.d_[ iGeoFactor * m + iSVDFactor ];
					geologicalFactor += svdFactors[iSVDFactor] * weight;
				}
				geologicalFactors.push_back( std::move( geologicalFactor ) );
			}
		}

		//Compute the grid derived form the geological factors (ideally it must match the input grid)
		{
			//Sum up all geological factors.
			spectral::array sum( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
			std::vector< spectral::array >::iterator it = geologicalFactors.begin();
			for( ; it != geologicalFactors.end(); ++it ){
				sum += *it;
			}
			//Compute FFT of the sum
			spectral::complex_array tmp;
			spectral::foward( tmp, sum ); //fftw crashes when called simultaneously
			//inbue the sum's FFT with the phase field of the original data.
			for( int idx = 0; idx < tmp.size(); ++idx)
			{
				std::complex<double> value;
				//get the complex number value in rectangular form
				value.real( tmp.d_[idx][0] ); //real part
				value.imag( tmp.d_[idx][1] ); //imaginary part
				//convert to polar form
				//but phase is replaced with that of the original data
				tmp.d_[idx][0] = std::abs( value ); //magnitude part
				tmp.d_[idx][1] = gridMagnitudeAndPhaseParts.d_[idx][1]; //std::arg( value ); //phase part (this should be zero all over the grid)
				//convert back to rectangular form (recall that the varmap holds covariance values, then it is necessary to take its square root)
				value = std::polar( std::sqrt(tmp.d_[idx][0]), tmp.d_[idx][1] );
				tmp.d_[idx][0] = value.real();
				tmp.d_[idx][1] = value.imag();
			}
			//Compute RFFT (with the phase of the original data imbued)
			spectral::array rfftResult( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
			spectral::backward( rfftResult, tmp ); //fftw crashes when called simultaneously
			//Divide the RFFT result (due to fftw3's RFFT implementation) by the number of grid cells
			rfftResult = rfftResult * (1.0/(nI*nJ*nK));
			derivedGrid += rfftResult;
		}

		//Change the weights m*n vector to a n by m matrix for displaying (fundamental factors as columns and geological factors as lines)
		va.set_size( (spectral::index)n, (spectral::index)m );
		displayGrids( {va}, {"parameters"}, {false} );

		//Collect the geological factors (variographic structures and reconstructed information)
		//as separate grids for display.
		std::vector< spectral::array >::iterator it = geologicalFactors.begin();
		for( int iGeoFactor = 0; it != geologicalFactors.end(); ++it, ++iGeoFactor ){
			//Compute FFT of the geological factor
			spectral::complex_array tmp;
			spectral::foward( tmp, *it );
			//inbue the geological factor's FFT with the phase field of the original data.
			for( int idx = 0; idx < tmp.size(); ++idx)
			{
				std::complex<double> value;
				//get the complex number value in rectangular form
				value.real( tmp.d_[idx][0] ); //real part
				value.imag( tmp.d_[idx][1] ); //imaginary part
				//convert to polar form
				//but phase is replaced with that of the original data
				tmp.d_[idx][0] = std::abs( value ); //magnitude part
				tmp.d_[idx][1] = gridMagnitudeAndPhaseParts.d_[idx][1]; //std::arg( value ); //phase part (this should be zero all over the grid)
				//convert back to rectangular form
				value = std::polar( std::sqrt(tmp.d_[idx][0]), tmp.d_[idx][1] );
				tmp.d_[idx][0] = value.real();
				tmp.d_[idx][1] = value.imag();
			}
			//Compute RFFT (with the phase of the original data imbued)
			spectral::array rfftResult( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
			spectral::backward( rfftResult, tmp );
			//Divide the RFFT result (due to fftw3's RFFT implementation) by the number of grid cells
			rfftResult = rfftResult * (1.0/(nI*nJ*nK));
			//Collect the grids.
			QString title = QString("Factor #") + QString::number(iGeoFactor+1);
			titles.push_back( title.toStdString() );
			grids.push_back( std::move( rfftResult ) );
			shiftByHalves.push_back( false );
			title = QString("Variographic structure #") + QString::number(iGeoFactor+1);
			titles.push_back( title.toStdString() );
			grids.push_back( std::move( *it ) );
			shiftByHalves.push_back( true );
		}
	}

	//Display the derived grid and its difference with respect to the original grid.
	//Also display the geological factors and their resulting partial grids.
	{
		spectral::array *gridData = grid->createSpectralArray( variable->getIndexInParentGrid() );
		spectral::array diff = *gridData - derivedGrid;
		grids.push_back( *gridData ); titles.push_back( "Original grid"); shiftByHalves.push_back( false );
		grids.push_back( derivedGrid ); titles.push_back( "Derived grid"); shiftByHalves.push_back( false );
		grids.push_back( diff ); titles.push_back( "difference"); shiftByHalves.push_back( false );
		displayGrids( grids, titles, shiftByHalves );
		delete gridData;
	}
}

void VariographicDecompositionDialog::displayGrids(const std::vector<spectral::array> &grids,
												   const std::vector<std::string> &titles,
												   const std::vector<bool> & shiftByHalves)
{
    //Create the structure to store the geological factors
    SVDFactorTree * factorTree = new SVDFactorTree( 0.0 ); //the split factor of 0.0 has no special meaning here
    //Populate the factor container with the geological factors.
    std::vector< spectral::array >::const_iterator it = grids.begin();
    std::vector< std::string >::const_iterator itTitles = titles.begin();
	std::vector< bool >::const_iterator itShiftByHalves = shiftByHalves.begin();
	for(int i = 1; it != grids.end(); ++it, ++i, ++itTitles, ++itShiftByHalves){
        //make a local copy of the geological factor data
        spectral::array geoFactorDataCopy;
		if( *itShiftByHalves )
            geoFactorDataCopy = spectral::shiftByHalf( *it );
        else
            geoFactorDataCopy = spectral::array( *it );
        //Create a displayble object from the geological factor data
        //This pointer will be managed by the SVDFactorTree object.
        SVDFactor* geoFactor = new SVDFactor( std::move(geoFactorDataCopy), i, 1/(grids.size()),
                                           0, 0, 0, 1, 1, 1, 0.0);
        //Declare it as a geological factor (decomposable, not fundamental)
        geoFactor->setType( SVDFactorType::GEOLOGICAL );
        geoFactor->setCustomName( QString( (*itTitles).c_str() ) );
        //add the displayable object to the factor tree (container)
        factorTree->addFirstLevelFactor( geoFactor );
    }
    //use the SVD analysis dialog to display the geological factors.
    //NOTE: do not use heap to allocate the dialog, unless you remove the Qt::WA_DeleteOnClose behavior of the dialog.
	SVDAnalysisDialog* svdad = new SVDAnalysisDialog( this );
    svdad->setTree( factorTree );
    svdad->setDeleteTreeOnClose( true ); //the three and all data it contains will be deleted on dialog close
    connect( svdad, SIGNAL(sumOfFactorsComputed(spectral::array*)),
             this, SLOT(onSumOfFactorsWasComputed(spectral::array*)) );
    svdad->exec(); //open the dialog modally
}

void VariographicDecompositionDialog::onSumOfFactorsWasComputed(spectral::array *gridData)
{
    IJAbstractCartesianGrid* grid = m_gridSelector->getSelectedGrid();
    emit saveArray( gridData, grid );
}

void VariographicDecompositionDialog::doVariographicDecomposition2( bool useSVD )
{
    // Get the data objects.
    IJAbstractCartesianGrid* grid = m_gridSelector->getSelectedGrid();
    IJAbstractVariable* variable = m_variableSelector->getSelectedVariable();

    // Get the grid's dimensions.
    unsigned int nI = grid->getNI();
    unsigned int nJ = grid->getNJ();
    unsigned int nK = grid->getNK();

    // Fetch data from the data source.
    grid->dataWillBeRequested();

    // The user-given number of geological factors (m).
    int m = ui->spinNumberOfGeologicalFactors->value();

    // The user-given epsilon (useful for numerical calculus).
    double epsilon = std::pow(10, ui->spinLogEpsilon->value() );

    // The other user-given parameters
    int maxNumberOfOptimizationSteps = ui->spinMaxSteps->value();
    double initialAlpha = ui->spinInitialAlpha->value();
    int maxNumberOfAlphaReductionSteps = ui->spinMaxStepsAlphaReduction->value();
    double convergenceCriterion = std::pow(10, ui->spinConvergenceCriterion->value() );
    double infoContentToKeepForSVD = ui->spinInfoContentToKeepForSVD->value() / 100.0;
    bool addSparsityPenalty = ui->chkEnableSparsityPenalty->isChecked();
    bool addOrthogonalityPenalty = ui->chkEnableOrthogonalityPenalty->isChecked();
    double sparsityThreshold = ui->spinSparsityThreshold->value();
	int nTracks = ui->spinNumberOfSpectrumTracks->value();
	int nSkipOutermost = ui->spinSkipOuterNIsolinesEllipseFitting->value();
	int nIsosurfs = ui->spinNumberOfIsolines->value();
    int nMinIsoVertexes = ui->spinIsoMinVertexes->value();
    objectiveFunctionFactors off;
    {
        off.f1 = ui->spinOJFactor_1->value();
        off.f2 = ui->spinOJFactor_2->value();
        off.f3 = ui->spinOJFactor_3->value();
        off.f4 = ui->spinOJFactor_4->value();
        off.f5 = ui->spinOJFactor_5->value();
        off.f6 = ui->spinOJFactor_6->value();
        off.f7 = ui->spinOJFactor_7->value();
    }

    //-------------------------------------------------------------------------------------------------
    //-----------------------------------PREPARATION STEPS---------------------------------------------
    //-------------------------------------------------------------------------------------------------

	// PRODUCT: a collection of grids with the fundamental factors of the variable.
	std::vector< spectral::array > fundamentalFactors;
    int n = 0;
    {
		//Atom learning method: SVD of input variable.
		if( useSVD ){
			//Get the number of usable fundamental factors.
			QProgressDialog progressDialog;
			progressDialog.setRange(0,0);
			progressDialog.setLabelText("Computing SVD factors of input data...");
			progressDialog.show();
			QCoreApplication::processEvents();
			spectral::array* gridInputData = grid->createSpectralArray( variable->getIndexInParentGrid() );
			doSVDonData( gridInputData, infoContentToKeepForSVD, fundamentalFactors );
			delete gridInputData;
			n = fundamentalFactors.size();
		//Atom learning method: frequency spectrum partitioning of input variable.
		} else {
			QProgressDialog progressDialog;
			progressDialog.setRange(0,0);
			progressDialog.setLabelText("Computing Fourier partitioning of input data...");
			progressDialog.show();
			QCoreApplication::processEvents();
			spectral::array* gridInputData = grid->createSpectralArray( variable->getIndexInParentGrid() );
			doFourierPartitioningOnData( gridInputData, fundamentalFactors, nTracks );
			delete gridInputData;
			n = fundamentalFactors.size();
		}
    }

	if( n == 0 ){
		emit error( "VariographicDecompositionDialog::doVariographicDecomposition2(): No fundamental factors. Aborted." );
		return;
	}

    //---------------------------------------------------------------------------------------------------------------------------
    //---------------------------------------SET UP THE LINEAR SYSTEM TO IMPOSE THE CONSERVATION CONSTRAINTS---------------------
    //---------------------------------------------------------------------------------------------------------------------------

    //Make the A matrix of the linear system originated by the information
    //conservation constraint (see program manual for the complete theory).
    //elements are initialized to zero.
    spectral::array A( (spectral::index)n, (spectral::index)m*n );
    for( int line = 0; line < n; ++line )
        for( int column = line * m; column < ((line+1) * m); ++column)
             A( line, column ) = 1.0;

    //Make the B matrix of said system
    spectral::array B( (spectral::index)n, (spectral::index)1 );
    for( int line = 0; line < n; ++line )
        B( line, 0 ) = 1.0;

    //Make the identity matrix to find the fundamental factors weights [a] from the parameters w:
    //     [a] = Adagger.B + (I-Adagger.A)[w]
    spectral::array I( (spectral::index)m*n, (spectral::index)m*n );
    for( int i = 0; i < m*n; ++i )
        I( i, i ) = 1.0;

    //Get the U, Sigma and V* matrices from SVD on A.
    spectral::SVD svd = spectral::svd( A );
    spectral::array U = svd.U();
    spectral::array Sigma = svd.S();
    spectral::array V = svd.V(); //SVD yields V* already transposed, that is V, but to check, you must transpose V
                                 //to get A = U.Sigma.V*

    //Make a full Sigma matrix (to be compatible with multiplication with the other matrices)
    {
        //All elementes are initialized with zeros
        spectral::array SigmaTmp( (spectral::index)n, (spectral::index)m*n );
        for( int i = 0; i < n; ++i)
            SigmaTmp(i, i) = Sigma.d_[i];
        Sigma = SigmaTmp;
    }

    //Make U*
    //U contains only real numbers, thus U's transpose conjugate is its transpose.
    spectral::array Ustar( U );
    //transpose U to get U*
    {
        Eigen::MatrixXd tmp = spectral::to_2d( Ustar );
        tmp.transposeInPlace();
        Ustar = spectral::to_array( tmp );
    }

    //Make Sigmadagger (pseudoinverse of Sigma)
    spectral::array SigmaDagger( Sigma );
    {
        //compute reciprocals of the non-zero elements in the main diagonal.
        for( int i = 0; i < n; ++i){
            double value = SigmaDagger(i, i);
            //only absolute values greater than the machine epsilon are considered non-zero.
            if( std::abs( value ) > std::numeric_limits<double>::epsilon() )
                SigmaDagger( i, i ) = 1.0 / value;
            else
                SigmaDagger( i, i ) = 0.0;
        }
        //transpose
        Eigen::MatrixXd tmp = spectral::to_2d( SigmaDagger );
        tmp.transposeInPlace();
        SigmaDagger = spectral::to_array( tmp );
    }

    //Make Adagger (pseudoinverse of A) by "reversing" the transform U.Sigma.V*,
    //hence, Adagger = V.Sigmadagger.Ustar .
    spectral::array Adagger;
    {
        Eigen::MatrixXd eigenV = spectral::to_2d( V );
        Eigen::MatrixXd eigenSigmadagger = spectral::to_2d( SigmaDagger );
        Eigen::MatrixXd eigenUstar = spectral::to_2d( Ustar );
        Eigen::MatrixXd eigenAdagger = eigenV * eigenSigmadagger * eigenUstar;
        Adagger = spectral::to_array( eigenAdagger );
    }

    //Initialize the vector of linear system parameters [w]=[0]
    spectral::array vw( (spectral::index)m*n );

    //---------------------------------------------------------------------------------------------------------------
    //-------------------------SIMULATED ANNEALING TO INITIALIZE THE PARAMETERS [w] NEAR A GLOBAL MINIMUM------------
    //---------------------------------------------------------------------------------------------------------------
    {
        //...................................Annealing Parameters.................................
        //Intial temperature.
        double f_Tinitial = ui->spinInitialTemperature->value();
        //Final temperature.
        double f_Tfinal = ui->spinFinalTemperature->value();
        //Max number of SA steps.
        int i_kmax = ui->spinMaxStepsSA->value();
        //Minimum value allowed for the parameters w (all zeros). DOMAIN CONSTRAINT
        spectral::array L_wMin( vw.size(), 0.0d );
        //Maximum value allowed for the parameters w (all ones). DOMAIN CONSTRAINT
        spectral::array L_wMax( vw.size(), 1.0d );
        /*Factor used to control the size of the random state “hop”.  For example, if the maximum “hop” must be
         10% of domain size, set 0.1.  Small values (e.g. 0.001) result in slow, but more accurate convergence.
         Large values (e.g. 100.0) covers more space faster, but falls outside the domain are more frequent,
         resulting in more re-searches due to more invalid parameter value penalties. */
        double f_factorSearch = ui->spinMaxHopFactor->value();
        //Intialize the random number generator with the same seed
        std::srand ((unsigned)ui->spinSeed->value());
        //.................................End of Annealing Parameters.............................
        //Returns the current “temperature” of the system.  It yields a log curve that decays as the step number increase.
        // The initial temperature plays an important role: curve starting with 5.000 is steeper than another that starts with 1.000.
        //  This means the the lower the temperature, the more linear the temperature decreases.
        // i_stepNumber: the current step number of the annealing process ( 0 = first ).
        auto temperature = [=](int i_stepNumber) { return f_Tinitial * std::exp( -i_stepNumber / (double)1000 * (1.5 * std::log10( f_Tinitial ) ) ); };
        /*Returns the probability of acceptance of the energy state for the next iteration.
          This allows acceptance of higher values to break free from local minima.
          f_eCurrent: current energy of the system.
          f_eNew: energy level of the next step.
          f_T: current “temperature” of the system.*/
        auto probAcceptance = [=]( double f_eCurrent, double f_eNewLocal, double f_T ) {
           //If the new state is more energetic, calculates a probability of acceptance
           //which is as high as the current “temperature” of the process.  The “temperature”
           //diminishes with iterations.
           if ( f_eNewLocal > f_eCurrent )
              return ( f_T - f_Tfinal ) / ( f_Tinitial - f_Tfinal );
           //If the new state is less energetic, the probability of acceptance is 100% (natural search for minima).
           else
              return 1.0;
        };
        //Get the number of parameters.
        int i_nPar = vw.size();
        //Make a copy of the initial state (parameter set.
        spectral::array L_wCurrent( vw );
        //The parameters variations (maxes - mins)
        spectral::array L_wDelta = L_wMax - L_wMin;
        //Get the input data.
        spectral::array *gridData = grid->createSpectralArray( variable->getIndexInParentGrid() );
        //...................Main annealing loop...................
        QProgressDialog progressDialog;
        progressDialog.setRange(0,0);
        progressDialog.show();
        progressDialog.setLabelText("Simulated Annealing in progress...");
		double f_eNew = std::numeric_limits<double>::max();
        double f_lowestEnergyFound = std::numeric_limits<double>::max();
        spectral::array L_wOfLowestEnergyFound;
        int k = 0;
        for( ; k < i_kmax; ++k ){
            emit info( "Commencing SA step #" + QString::number( k ) );
            //Get current temperature.
            double f_T = temperature( k );
            //Quit if temperature is lower than the minimum annealing temperature.
            if( f_T < f_Tfinal )
                break;
            //Randomly searches for a neighboring state with respect to current state.
            spectral::array L_wNew(L_wCurrent);
			for( int i = 0; i < i_nPar; ++i ){ //for each parameter
               //Ensures that the values randomly obtained are inside the domain.
               double f_tmp = 0.0;
               while( true ){
                  double LO = L_wCurrent[i] - (f_factorSearch * L_wDelta[i]);
                  double HI = L_wCurrent[i] + (f_factorSearch * L_wDelta[i]);
                  f_tmp = LO + std::rand() / (RAND_MAX/(HI-LO)) ;
                  if ( f_tmp >= L_wMin[i] && f_tmp <= L_wMax[i] )
                     break;
               }
               //Updates the parameter value.
               L_wNew[i] = f_tmp;
			}
            //Computes the “energy” of the current state (set of parameters).
            //The “energy” in this case is how different the image as given the parameters is with respect
            //the data grid, considered the reference image.
            double f_eCurrent = F2( *gridData, L_wCurrent, A, Adagger, B, I, m, fundamentalFactors, addSparsityPenalty, addOrthogonalityPenalty, sparsityThreshold, nSkipOutermost, nIsosurfs, nMinIsoVertexes, off );
            //Computes the “energy” of the neighboring state.
            f_eNew = F2( *gridData, L_wNew, A, Adagger, B, I, m, fundamentalFactors, addSparsityPenalty, addOrthogonalityPenalty, sparsityThreshold, nSkipOutermost, nIsosurfs, nMinIsoVertexes, off );
            //Changes states stochastically.  There is a probability of acceptance of a more energetic state so
            //the optimization search starts near the global minimum and is not trapped in local minima (hopefully).
            double f_probMov = probAcceptance( f_eCurrent, f_eNew, f_T );
            if( f_probMov >= ( (double)std::rand() / RAND_MAX ) ) {//draws a value between 0.0 and 1.0
                L_wCurrent = L_wNew; //replaces the current state with the neighboring random state
                emit info("  moved to energy level " + QString::number( f_eNew ));
                //if the energy is the record low, store it, just in case the SA loop ends without converging.
                if( f_eNew < f_lowestEnergyFound ){
                    f_lowestEnergyFound = f_eNew;
                    L_wOfLowestEnergyFound = spectral::array( L_wCurrent );
                }
            }
			if( ! (k % 10) ) //to avoid excess calls to processEvents.
				QCoreApplication::processEvents();
		}
        // The input data is no longer necessary.
        delete gridData;
        // Delivers the set of parameters near the global minimum (hopefully) for the Gradient Descent algorithm.
        // The SA loop may end in a higher energy state, so we return the lowest found in that case
        if( k == i_kmax && f_lowestEnergyFound < f_eNew )
            emit info( "SA completed by number of steps." );
        else
            emit info( "SA completed by reaching the lowest temperature." );
        vw = L_wOfLowestEnergyFound;
        emit info( "Using the state of lowest energy found (" + QString::number( f_lowestEnergyFound ) + ")" );
	}

	//---------------------------------------------------------------------------------------------------------
	//--------------------------------------OPTIMIZATION LOOP (GRADIENT DESCENT)-------------------------------
	//---------------------------------------------------------------------------------------------------------
	unsigned int nThreads = ui->spinNumberOfThreads->value();
	QProgressDialog progressDialog;
	progressDialog.setRange(0,0);
	progressDialog.show();
	progressDialog.setLabelText("Gradient Descent in progress...");
	int iOptStep = 0;
	spectral::array va;
	for( ; iOptStep < maxNumberOfOptimizationSteps; ++iOptStep ){

		emit info( "Commencing GD step #" + QString::number( iOptStep ) );


		//Compute the vector of weights [a] = Adagger.B + (I-Adagger.A)[w] (see program manual for theory)
		{
			Eigen::MatrixXd eigenAdagger = spectral::to_2d( Adagger );
			Eigen::MatrixXd eigenB = spectral::to_2d( B );
			Eigen::MatrixXd eigenI = spectral::to_2d( I );
			Eigen::MatrixXd eigenA = spectral::to_2d( A );
			Eigen::MatrixXd eigenvw = spectral::to_2d( vw );
			Eigen::MatrixXd eigenva = eigenAdagger * eigenB + ( eigenI - eigenAdagger * eigenA ) * eigenvw;
			va = spectral::to_array( eigenva );
		}

		{
			spectral::array debug_va( va );
			debug_va.set_size( (spectral::index)n, (spectral::index)m );
		}

		//Compute the gradient vector of objective function F with the current [w] parameters.
		spectral::array gradient( vw.size() );
		{
			spectral::array *gridData = grid->createSpectralArray( variable->getIndexInParentGrid() );

			//distribute the parameter indexes among the n-threads
			std::vector<int> parameterIndexBins[nThreads];
			int parameterIndex = 0;
			for( unsigned int iThread = 0; parameterIndex < vw.size(); ++parameterIndex, ++iThread)
				parameterIndexBins[ iThread % nThreads ].push_back( parameterIndex );

			//create and run the partial derivative calculation threads
			std::thread threads[nThreads];
			for( unsigned int iThread = 0; iThread < nThreads; ++iThread){
				threads[iThread] = std::thread( taskOnePartialDerivative2,
												vw,
												parameterIndexBins[iThread],
												epsilon,
												gridData,
												A,
												Adagger,
												B,
												I,
												m,
												fundamentalFactors,
												addSparsityPenalty,
												addOrthogonalityPenalty,
                                                sparsityThreshold,
												nSkipOutermost,
												nIsosurfs,
                                                nMinIsoVertexes,
                                                off,
												&gradient);
			}

			//wait for the threads to finish.
			for( unsigned int iThread = 0; iThread < nThreads; ++iThread)
				threads[iThread].join();

			delete gridData;
		}

		//Update the system's parameters according to gradient descent.
		double currentF = 999.0;
		double nextF = 1.0;
		{
			spectral::array *gridData = grid->createSpectralArray( variable->getIndexInParentGrid() );
			double alpha = initialAlpha;
			//halves alpha until we get a descent (current gradient vector may result in overshooting)
			int iAlphaReductionStep = 0;
			for( ; iAlphaReductionStep < maxNumberOfAlphaReductionSteps; ++iAlphaReductionStep ){
				spectral::array new_vw = vw - gradient * alpha;
				//Impose domain constraints to the parameters.
				for( int i = 0; i < new_vw.size(); ++i){
					if( new_vw.d_[i] < 0.0 )
						new_vw.d_[i] = 0.0;
					if( new_vw.d_[i] > 1.0 )
						new_vw.d_[i] = 1.0;
				}
                currentF = F2( *gridData, vw, A, Adagger, B, I, m, fundamentalFactors, addSparsityPenalty, addOrthogonalityPenalty, sparsityThreshold, nSkipOutermost, nIsosurfs, nMinIsoVertexes, off );
                nextF = F2( *gridData, new_vw, A, Adagger, B, I, m, fundamentalFactors, addSparsityPenalty, addOrthogonalityPenalty, sparsityThreshold, nSkipOutermost, nIsosurfs, nMinIsoVertexes, off );
				if( nextF < currentF ){
					vw = new_vw;
					break;
				}
				alpha /= 2.0;
			}
			if( iAlphaReductionStep == maxNumberOfAlphaReductionSteps )
				emit warning( "WARNING: reached maximum alpha reduction steps." );
			delete gridData;
		}

		//Check the convergence criterion.
		double ratio = currentF / nextF;
		if( ratio  < (1.0 + convergenceCriterion) )
			break;

		emit info( "F2(k)/F2(k+1) ratio: " + QString::number( ratio ) );

		if( ! ( iOptStep % 10) ) //to avoid excess calls to processEvents.
			QCoreApplication::processEvents();
	}
	progressDialog.hide();

	//-------------------------------------------------------------------------------------------------
	//------------------------------------PRESENT THE RESULTS------------------------------------------
	//-------------------------------------------------------------------------------------------------

	if( iOptStep == maxNumberOfOptimizationSteps )
		QMessageBox::warning( this, "Warning", "Completed by reaching maximum number of optimization steps. Check results.");
	else
		QMessageBox::information( this, "Info", "Completed by satisfaction of convergence criterion.");

	std::vector< spectral::array > grids;
	std::vector< std::string > titles;
	std::vector< bool > shiftByHalves;

	spectral::array derivedGrid( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
	{
		//Make the m geological factors (expected maps with different variographic structures)
		std::vector< spectral::array > geologicalFactors;
		{
			QProgressDialog progressDialog;
			progressDialog.setRange(0,0);
			progressDialog.show();
			progressDialog.setLabelText("Making the geological factors...");
			for( int iGeoFactor = 0; iGeoFactor < m; ++iGeoFactor){
				spectral::array geologicalFactor( (spectral::index)nI, (spectral::index)nJ, (spectral::index)nK );
				for( int iSVDFactor = 0; iSVDFactor < n; ++iSVDFactor){
					double weight = va.d_[ iGeoFactor * m + iSVDFactor ];
					geologicalFactor += fundamentalFactors[iSVDFactor] * weight;
				}
				geologicalFactors.push_back( std::move( geologicalFactor ) );
				QCoreApplication::processEvents();
			}
		}

		//Change the weights m*n vector to a n by m matrix for displaying (fundamental factors as columns and geological factors as lines)
		va.set_size( (spectral::index)n, (spectral::index)m );
		displayGrids( {va}, {"parameters"}, {false} );

		//Collect the geological factors (expected maps with different variographic structures)
		//as separate grids for display.
		std::vector< spectral::array >::iterator it = geologicalFactors.begin();
		for( int iGeoFactor = 0; it != geologicalFactors.end(); ++it, ++iGeoFactor ){
			spectral::array rfftResult( *it );
			//Collect the grids.
			QString title = QString("Factor #") + QString::number(iGeoFactor+1);
			titles.push_back( title.toStdString() );
			grids.push_back( std::move( rfftResult ) );
			shiftByHalves.push_back( false );
		}
	}

	//Display the derived grid and its difference with respect to the original grid.
	//Also display the geological factors and their resulting partial grids.
	displayGrids( grids, titles, shiftByHalves );
}

void VariographicDecompositionDialog::doVariographicDecomposition3()
{
	doVariographicDecomposition2( false );
}

void VariographicDecompositionDialog::displayGrid(const spectral::array & grid, const std::string & title, bool shiftByHalf)
{
	//Create the structure to store the geological factors
	SVDFactorTree * factorTree = new SVDFactorTree( 0.0 ); //the split factor of 0.0 has no special meaning here
	//make a local copy of the grid data
	spectral::array geoFactorDataCopy;
	if( shiftByHalf )
		geoFactorDataCopy = spectral::shiftByHalf( grid );
	else
		geoFactorDataCopy = spectral::array( grid );
	//Create a displayble object from the grid data
	//This pointer will be managed by the SVDFactorTree object.
	SVDFactor* geoFactor = new SVDFactor( std::move(geoFactorDataCopy), 1, 1.0,
											0, 0, 0, 1, 1, 1, 0.0);
	//Declare it as a geological factor (decomposable, not fundamental)
	geoFactor->setType( SVDFactorType::GEOLOGICAL );
	geoFactor->setCustomName( QString( title.c_str() ) );
	//add the displayable object to the factor tree (container)
	factorTree->addFirstLevelFactor( geoFactor );
	//use the SVD analysis dialog to display the geological factors.
	//NOTE: do not use heap to allocate the dialog, unless you remove the Qt::WA_DeleteOnClose behavior of the dialog.
	SVDAnalysisDialog* svdad = new SVDAnalysisDialog( nullptr );
	svdad->setTree( factorTree );
	svdad->setDeleteTreeOnClose( true ); //the three and all data it contains will be deleted on dialog close
	svdad->exec(); //open the dialog modally
}

void VariographicDecompositionDialog::doSVDonData(const spectral::array* gridInputData,
												  double infoContentToKeepForSVD,
												  std::vector<spectral::array> & svdFactors)
{
	//Get the number of usable fundamental SVD factors.
	{
		int n = 0;
		spectral::SVD svd = spectral::svd( *gridInputData );
		//get the list with the factor weights (information quantity)
		spectral::array weights = svd.factor_weights();
		//get the number of fundamental factors that have the total information content as specified by the user.
		{
			double cumulative = 0.0;
			uint i = 0;
			for(; i < weights.size(); ++i){
				cumulative += weights.d_[i];
				if( cumulative > infoContentToKeepForSVD )
					break;
			}
			n = i+1;
		}
		if( n < 3 ){
			QMessageBox::warning( this, "Warning", "The data must be decomposable into at least three usable fundamental factors to proceed.");
			return;
		} else {
			emit info( "Using " + QString::number(n) + " fundamental factors out of " + QString::number(weights.size()) +
					   " to cover " + QString::number(ui->spinInfoContentToKeepForSVD->value()) + "% of information content." );
		}
		//Get the usable fundamental SVD factors.
		{
			QProgressDialog progressDialog;
			progressDialog.setRange(0,0);
			progressDialog.show();
			progressDialog.setLabelText("Retrieving fundamental SVD factors...");
			QCoreApplication::processEvents();
			for (long i = 0; i < n; ++i) {
				spectral::array factor = svd.factor(i);
				svdFactors.push_back( std::move( factor ) );
			}
		}
	}
}

void VariographicDecompositionDialog::doFourierPartitioningOnData(const spectral::array * gridInputData,
																  std::vector<spectral::array> & frequencyFactors,
																  int nTracks )
{
	///Visualizing the results on the fly is optional/////////////
	IJQuick3DViewer q3Dv;
	q3Dv.show();
	///////////////////////////////////////////////////////////////

	///////////////////////////////// ACESS GRID GEOMETRY ///////////////////////////

	double cellWidth = 1.0;
	double cellHeight = 1.0;
	// double cellThickness = 1.0; // distance criterion currently only in 2D.
	double gridWidth = gridInputData->M() * cellWidth;
	double gridHeight = gridInputData->N() * cellHeight;
	// double gridDepth = gridInputData.K() * cellThickness; // distance criterion currently only in 2D.
	int nI = gridInputData->M();
	int nJ = gridInputData->N();
	int nK = gridInputData->K();

	////////////////// DEFINE SOME STRUCTURES TO ORGANIZE CODE ///////////////////////

	// A Sector is a division of a HalfTrack spanning some angles.
	struct Sector{
		double startAzimuth;
		double endAzimuth;
		spectral::array grid;
		bool wasTouched = false;
		void makeGrid( int pnI, int pnJ, int pnK ){
			grid = spectral::array( pnI, pnJ, pnK, 0.0d );
		}
		bool isAligned( double azimuth ){
			return azimuth >= startAzimuth && azimuth < endAzimuth;
		}
		bool assignValue( double azimuth, double value, int i, int j, int k ){
			if( isAligned( azimuth )){
				grid( i, j, k ) = value;
				wasTouched = true;
				return true;
			}
			return false;
		}
	};

	// A HalfTrack is a half-circular band centered at the grid's center and arcing between N000E (north) and N180E (south)
	// passing through east (N090E).  The spectrogram of a real-valued grid is symmetric, meaning that a values
	// along the N045E azimuth, for example, equals those along the N225E azimuth, thus it is not necessary to model
	// an all-around track.	It is divided into Sectors.
	struct HalfTrack{
		double innerRadius;
		double outerRadius;
		int index;
		std::vector< Sector > sectors;
		void makeSectors( int pnI, int pnJ, int pnK ){
			int nSectors = 1;
			if( index > 0 )
				nSectors = 2 * (index+1); //innermost track = 1 sector, then 4, then 6, then 8, then 10...
			double azimuthSpan = 180.0d / nSectors;
			double currentStartAzimuth = 0.0;
			double currentEndAzimuth = azimuthSpan;
			for( int i = 0; i < nSectors; ++i ){
				Sector sector;
				sector.startAzimuth = currentStartAzimuth;
				sector.endAzimuth = currentEndAzimuth;
				sector.makeGrid( pnI, pnJ, pnK );
				currentStartAzimuth += azimuthSpan;
				currentEndAzimuth += azimuthSpan;
				sectors.push_back( sector );
			}
		}
		bool isInside( double distance ){
			return distance >= innerRadius && distance < outerRadius;
		}
		bool assignValue( double distance, double azimuth, double value, int i, int j, int k ){
			if( isInside( distance ) ){
				std::vector< Sector >::iterator itSector = sectors.begin();
				for( ; itSector != sectors.end(); ++itSector ){
					bool wasAssigned = (*itSector).assignValue( azimuth, value, i, j, k );
					if( wasAssigned )
						return true; //abort the search if a matching sector was found.
				}
			}
			return false;
		}
	};

	///////////////////// BEGIN OF ALGORITHM ITSELF //////////////////////////////////

	//Compute FFT of the input data
	spectral::array inputFFTmagnitudes;
	spectral::array inputFFTphases;
	{
		spectral::array inputCopy( *gridInputData );
		spectral::complex_array inputFFT;
		spectral::foward( inputFFT, inputCopy );
		inputFFT = spectral::to_polar_form( inputFFT );
		inputFFTmagnitudes = spectral::real( inputFFT );
		inputFFTphases = spectral::imag( inputFFT );
	}

	//Shift the Fourier image so that frequency zero is in the grid center.
	inputFFTmagnitudes = spectral::shiftByHalf( inputFFTmagnitudes );

	// Compute the track geometries.
	std::vector< HalfTrack > tracks;
	{
		// Get global geometry.
		double diagonal = std::sqrt( gridWidth*gridWidth + gridHeight*gridHeight );
		double trackWidth = (diagonal/2) / nTracks;
		double currentInnerRadius = 0.0;
		double currentOuterRadius = trackWidth;
		// Create the tracks from center outwards.
		for( int i = 0; i < nTracks; ++i){
			HalfTrack track;
			track.innerRadius = currentInnerRadius;
			track.outerRadius = currentOuterRadius;
			track.index = i;
			track.makeSectors( nI, nJ, nK );
			tracks.push_back( track );
			currentInnerRadius += trackWidth;
			currentOuterRadius += trackWidth;
		}
	}

	// Scan the Fourier image, assigning cell values to the tracks/sectors.
	double gridCenterX = gridWidth/2;
	double gridCenterY = gridHeight/2;
	// double gridCenterZ = gridDepth/2; // distance criterion currently only in 2D.
	for( int i = 0; i < inputFFTmagnitudes.M(); ++i ){
		double cellCenterX = cellWidth/2 + i * cellHeight;
		for( int j = 0; j < inputFFTmagnitudes.N(); ++j ){
			double cellCenterY = cellHeight/2 + j * cellHeight;
			for( int k = 0; k < inputFFTmagnitudes.K(); ++k ){
				// double cellCenterZ = cellThickness/2 + k * cellThickness; // distance criterion currently only in 2D.
				double dX = cellCenterX - gridCenterX;
				double dY = cellCenterY - gridCenterY;
				// double dZ = cellCenterZ - gridCenterZ; // distance criterion currently only in 2D.
				double distance = std::sqrt( dX*dX + dY*dY /*+ dZ*dZ*/ ); // distance criterion currently only in 2D.
				double azimuth = ImageJockeyUtils::getAzimuth( cellCenterX, cellCenterY, gridCenterX, gridCenterY, true );
				std::vector< HalfTrack >::iterator trackIt = tracks.begin();
				for( ; trackIt != tracks.end(); ++trackIt ){
					bool wasAssigned = (*trackIt).assignValue( distance, azimuth, inputFFTmagnitudes(i,j,k), i, j, k );
					if( wasAssigned )
						break; //abort the search if a matching sector in a matching track was found.
				}
			}
		}
	}

	// Discard all-zeros factors ( resulted from sectors out of grid boundaries ).
	{
		std::vector< HalfTrack >::iterator trackIt = tracks.begin();
		int totalTracks = 0;
		for( ; trackIt != tracks.end(); ++trackIt ){
			std::vector< Sector >::iterator itSector = (*trackIt).sectors.begin();
			for( ; itSector != (*trackIt).sectors.end(); ){ // erase() already increments the iterator.
				if( ! (*itSector).wasTouched )
					itSector = (*trackIt).sectors.erase( itSector );
				else{
					++itSector;
					++totalTracks;
				}
			}
		}
		emit info( "VariographicDecompositionDialog::doFourierPartitioningOnData(): " + QString::number( totalTracks ) + " fundamental frequency factors." );
	}

	// De-shift all factors so they become compatible with reverse FFT.
	{
		std::vector< HalfTrack >::iterator trackIt = tracks.begin();
		for( ; trackIt != tracks.end(); ++trackIt ){
			std::vector< Sector >::iterator itSector = (*trackIt).sectors.begin();
			for( ; itSector != (*trackIt).sectors.end(); ++itSector ){
				(*itSector).grid = spectral::shiftByHalf( (*itSector).grid );
			}
		}
	}

	// Compute RFFT for all factors and return them via the output collection.
	{
		std::vector< HalfTrack >::iterator trackIt = tracks.begin();
		for( ; trackIt != tracks.end(); ++trackIt ){
			std::vector< Sector >::iterator itSector = (*trackIt).sectors.begin();
			for( ; itSector != (*trackIt).sectors.end(); ++itSector ){
				spectral::complex_array input = spectral::to_complex_array( (*itSector).grid, inputFFTphases );
				spectral::array backtrans( nI, nJ, nK, 0.0d );
				input = spectral::to_rectangular_form( input );
				spectral::backward( backtrans, input );
				(*itSector).grid = backtrans / static_cast<double>(nI * nJ * nK);
				frequencyFactors.push_back( (*itSector).grid );
			}
		}
	}

	///Visualizing the results on the fly is optional/////////////
	{
		std::vector< HalfTrack >::iterator trackIt = tracks.begin();
		for( ; trackIt != tracks.end(); ++trackIt ){
			std::vector< Sector >::iterator itSector = (*trackIt).sectors.begin();
			for( ; itSector != (*trackIt).sectors.end(); ++itSector ){
				q3Dv.clearScene();
				q3Dv.display( (*itSector).grid, (*itSector).grid.min(), (*itSector).grid.max() );
			}
		}
	}
	////////////////////////////////////////////////////////////

}

void VariographicDecompositionDialog::doVariographicDecomposition2( )
{
	doVariographicDecomposition2( true );
}
