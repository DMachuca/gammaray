#ifndef GRIDFILE_H
#define GRIDFILE_H

#include "datafile.h"
#include "geostats/spatiallocation.h"

namespace spectral{
   class array;
}

/** Abstract class for all domain classes that have grid topology (IJK coordinates). */
class GridFile : public DataFile
{
public:
	GridFile( QString path );

	/**
	 * Returns a value from the data column (0 = 1st column) given a grid topological coordinate (IJK).
	 * @param i must be between 0 and NI-1.
	 * @param j must be between 0 and NJ-1.
	 * @param k must be between 0 and NK-1.
	 */
	inline double dataIJK(uint column, uint i, uint j, uint k){
		uint dataRow = i + j*m_nI + k*m_nJ*m_nI;
		return data( dataRow, column );
	}

	/** Creates a vector of complex numbers with the values taken from data columns.
	 *  Specify -1 to omit a column, which causes the repective part to be filled with zeros.
	 *  getArray(-1,-1) returns an array filled with zeroes.  The dimension of the array is that
	 *  of the grid (getNX(), getNY(), getNZ()).
	 *  @param indexColumRealPart Column index (starting with 0) with the values for the real part.
	 *  @param indexColumRealPart Column index (starting with 0) with the values for the imaginary part.
	 */
	std::vector< std::complex<double> > getArray( int indexColumRealPart, int indexColumImaginaryPart = -1 );

	/** Returns the value of the variable (given by its zero-based index) at the given spatial location.
	 *  The function returns the value of grid cell that contains the given location.  The z coordinate is ignored
	 * if the grid is 2D. If the coordinate lies outside the grid, No-data-value or NaN is returned, depending on
	 * whether the user has set a NDV for the data file.
	 * Make sure you load the desired realization with DataFile::setDataPage(), otherwise the value of the first
	 * realization will be returned.
	 */
	double valueAt( uint dataColumn, double x, double y, double z );

	/**
	 * Returns, via output variables (i,j and k), the IJK coordinates corresponding to a XYZ spatial coordinate.
	 * Returns false if the spatial coordinate lies outside the grid.
	 */
	virtual bool XYZtoIJK( double x, double y, double z, uint& i,   uint& j,   uint& k ) = 0;

	/**
	 * Make a call to DataFile::setDataPage() such that only the given realization number is loaded into memory.
	 * First realization is number 0 (zero).  To restore the default behavior (load entire data), call
	 * DataFile::setDataPageToAll().
	 */
	void setDataPageToRealization( uint nreal );

	/** Returns the grid's center. */
	virtual SpatialLocation getCenter() = 0;

	/**
	 * Returns, via output variables (z, y and z), the XYZ coordinates corresponding to a IJK topological coordinate.
	 * The returned coordinate is that of the center of the cell.
	 */
	virtual void IJKtoXYZ( uint i,    uint j,    uint k,
						   double& x, double& y, double& z ) = 0;

	/** Sets the number of realizations.
	 * This is declarative only.  No check is performed whether there are actually the number of
	 * realizations informed.
	 */
	void setNReal( uint n );


	/** Adds de contents of the given data array as new column to this Cartesian grid. */
	long append( const QString columnName, const spectral::array& array );

	/** Converts a data row index into topological coordinates (output parameters). */
	void indexToIJK(uint index, uint & i, uint & j, uint & k );

	/** Replaces the data in the column with the data in passed data array. */
	void setColumnData( uint dataColumn, spectral::array& array );

	uint getNI(){ return m_nI; }
	uint getNJ(){ return m_nJ; }
	uint getNK(){ return m_nK; }
	uint getNumberOfRealizations(){ return m_nreal; }

	/** Returns the data row index corresponding to the given topological coordinate (cell address). */
	uint IJKtoIndex( uint i, uint j, uint k );

// ICalcPropertyCollection interface
	virtual double getNeighborValue( int iRecord, int iVar, int dI, int dJ, int dK );

protected:
	uint m_nI, m_nJ, m_nK, m_nreal;

	/**
	 * Sets a value in the data column (0 = 1st column) given a grid topological coordinate (IJK).
	 * @param i must be between 0 and NX-1.
	 * @param j must be between 0 and NY-1.
	 * @param k must be between 0 and NZ-1.
	 */
	void setDataIJK( uint column, uint i, uint j, uint k, double value );
};

#endif // GRIDFILE_H
