#ifndef IMAGEJOCKEYUTILS_H
#define IMAGEJOCKEYUTILS_H

#include <cassert>
#include <cstdint>

class ImageJockeyUtils
{
public:
	ImageJockeyUtils();

	/**
	 * Perfoms a reliable way to compare floating-point values.
	 * credit: Bruce Dawson
	 * 32-bit version source: http://www.cygnus-software.com/papers/comparingfloats/comparingfloats.htm
	 * 64-bit version of almostEqual2sComplement by GammaRay authors.
	 */
	inline static bool almostEqual2sComplement(double A, double B, int maxUlps)
	{
		// Make sure maxUlps is non-negative and small enough that the
		// default NAN won't compare as equal to anything.
		//<cassert>'s assert doesn't accept longs
		// assert(maxUlps > 0 && maxUlps < 2 * 1024 * 1024 * 1024 * 1024 * 1024);
		assert(maxUlps > 0 && maxUlps < 4 * 1024 * 1024);
		int64_t aLong = *reinterpret_cast<int64_t *>(&A); // use the raw bytes from the
														  // double to make a long int
														  // value (type punning)
		// Make aLong lexicographically ordered as a twos-complement long
		if (aLong < 0)
			aLong = 0x8000000000000000 - aLong;
		// Make bLong lexicographically ordered as a twos-complement long
		int64_t bLong = *reinterpret_cast<int64_t *>(&B); // use the raw bytes from the
														  // double to make a long int
														  // value (type punning)
		if (bLong < 0)
			bLong = 0x8000000000000000 - bLong;
		int64_t longDiff = (aLong - bLong) & 0x7FFFFFFFFFFFFFFF;
		if (longDiff <= maxUlps)
			return true;
		return false;
	}


	/** Returns decibels of an input value with respect to a reference value.
	 * Examples.: dBm is defined as the decibel level of value in milliwatts with respect
	 * to 1mW.
	 *            dBi is defined as the gain in decibels of an antenna with respect to the
	 * ideal isotropic antenna.
	 * refLevel cannot be zero or a divison by zero error ensues.
	 * epsilon The smallest allowed absolute value as to avoid large negative results or
	 * even -inf (value = 0.0).
	 * @param scaleFactor Use 10.0 for most applications. Use 20.0 for power (square law) applications.
	 */
	static double dB(double value, double refLevel, double epsilon, double scaleFactor = 10.0);
};

#endif // IMAGEJOCKEYUTILS_H
