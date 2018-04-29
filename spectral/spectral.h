/*
Spectral primitives

(c) 2017, Pericles Lopes Machado
*/

#pragma once

#include <Eigen/Core>
#include <chrono>
#include <cmath>
#include <complex>
#include <fftw3.h>
#include <iostream>
#include <omp.h>
#include <vector>

namespace spectral
{

using index = long long;
using fftw_array_raw = fftw_complex *;

struct complex_array {
    complex_array();
    complex_array(index N);
    complex_array(index M, index N);
    complex_array(index M, index N, index K);
    complex_array(fftw_array_raw d);
    complex_array(const complex_array &other);
    complex_array(complex_array &&other);
    complex_array(const Eigen::MatrixXd &m);

    complex_array &operator=(complex_array &&other);
    complex_array &operator=(const complex_array &other);

    virtual ~complex_array();

    index idx(index i) const;
    index idx(index i, index j) const;
    index idx(index i, index j, index k) const;

    fftw_complex &operator()(index i);
    fftw_complex &operator()(index i, index j);
    fftw_complex &operator()(index i, index j, index k);

    fftw_complex &operator()(index i) const;
    fftw_complex &operator()(index i, index j) const;
    fftw_complex &operator()(index i, index j, index k) const;

    index size() const;
    const double &real(index i) const;
    const double &imag(index i) const;

    double norm(index i) const;

    double arg(index i) const;

    std::complex<double> complex(index i) const;

    fftw_complex &operator[](index i);

    void dot(const complex_array &a, const complex_array &other);

    // this  = a * b
    void dot_conj(const complex_array &a, const complex_array &b);

    fftw_array_raw data();
    void set_data(fftw_array_raw d, index M);
    void set_data(fftw_array_raw d, index M, index N);
    void set_data(fftw_array_raw d, index M, index N, index K);
    void set_size(index M, index N, index K);
    void set_size(index M, index N);
    void set_size(index M);

    index ndim() const { return ndim_; }
    index M() const { return M_; }
    index N() const { return N_; }
    index K() const { return K_; }

    fftw_array_raw d_ = nullptr;
    index ndim_ = 1;
    index size_ = 0;
    index M_ = 1;
    index N_ = 1;
    index K_ = 1;
};

struct array {
    array();
    array(index M, double default_value = 0.0);
    array(index M, index N, double default_value = 0.0);
    array(index M, index N, index K, double default_value = 0.0);
    array(const array &other);
    array(array &&other);
    array(const Eigen::MatrixXd &m);

    array &operator=(array &&other);
    array &operator=(const array &other);

    array &operator+=(const array &other);

    array operator*( double scalar );

    virtual ~array();

    double &operator()(index i);
    double &operator()(index i, index j);
    double &operator()(index i, index j, index k);

    const double &operator()(index i) const;
    const double &operator()(index i, index j) const;
    const double &operator()(index i, index j, index k) const;

    index ndim() const { return ndim_; }
    index M() const { return M_; }
    index N() const { return N_; }
    index K() const { return K_; }
    index size() const { return d_.size(); }
    void resize(index K) { d_.resize(K); }

    double &operator[](index i) { return d_[i]; }
    const double &operator[](index i) const { return d_[i]; }
    std::vector<double> &data() { return d_; }

    void set_size(index M, index N, index K);
    void set_size(index M, index N);
    void set_size(index M);

    std::vector<double> d_;
    index ndim_ = 1;
    index M_ = 1; // dim 1
    index N_ = 1; // dim 2
    index K_ = 1; // dim 3
};

// fft 1D
void foward(complex_array &out, double *in, index M);
void foward(complex_array &out, std::vector<double> &in, index M);
void foward(complex_array &out, complex_array &in, index M);

// fft 2D
void foward(complex_array &out, double *in, index M, index N);
void foward(complex_array &out, std::vector<double> &in, index M, index N);
void foward(complex_array &out, complex_array &in, index M, index N);

// fft 3D
void foward(complex_array &out, double *in, index M, index N, index K);
void foward(complex_array &out, std::vector<double> &in, index M, index N, index K);
void foward(complex_array &out, complex_array &in, index M, index N, index K);

// multidim fft
void foward(complex_array &out, array &in);
void foward(complex_array &out, complex_array &in);

// ifft 1D
void backward(std::vector<double> &out, complex_array &in, index M);
void backward(complex_array &out, complex_array &in, index M);

// ifft 2D
void backward(std::vector<double> &out, complex_array &in, index M, index N);
void backward(complex_array &out, complex_array &in, index M, index N);

// ifft 3D
void backward(std::vector<double> &out, complex_array &in, index M, index N, index K);
void backward(complex_array &out, complex_array &in, index M, index N, index K);

// multidim ifft
void backward(array &out, complex_array &in);
void backward(complex_array &out, complex_array &in);

// convolutions
void conv1d(std::vector<double> &out, const std::vector<double> &a,
            const std::vector<double> &b);
void conv2d(array &out, const array &a, const array &b);
void conv3d(array &out, const array &a, const array &b);

void conv1d_naive(std::vector<double> &out, const std::vector<double> &a,
                  const std::vector<double> &b);
void conv2d_naive(array &out, const array &a, const array &b);
void conv3d_naive(array &out, const array &a, const array &b);

void conv_naive(array &out, const array &a, const array &b);

void conv(array &out, const array &a, const array &b);
void autoconv(array &out, const array &a);

// covariance
void covariance1d(std::vector<double> &out, std::vector<double> &np,
                  const std::vector<double> &a, const std::vector<double> &b,
                  bool centered);
void covariance2d(array &out, array &np, const array &a, const array &b, bool centered);
void covariance3d(array &out, array &np, const array &a, const array &b, bool centered);

void covariance(array &out, array &np, const array &a, const array &b, bool centered);
void covariance(array &out, const array &a, const array &b, bool centered);

void autocovariance(array &out, const array &a, bool centered);
void autocovariance(array &out, array &np, const array &a, bool centered);

void covariance1d_naive(std::vector<double> &out, std::vector<double> &np,
                        const std::vector<double> &a, const std::vector<double> &b,
                        bool centered);
void covariance2d_naive(array &out, array &np, const array &a, const array &b,
                        bool centered);
void covariance3d_naive(array &out, array &np, const array &a, const array &b,
                        bool centered);

void covariance_naive(array &out, array &np, const array &a, const array &b,
                      bool centered);
void autocovariance_naive(array &out, array &np, const array &a, bool centered);

void covariance_naive(array &out, const array &a, const array &b, bool centered);
void autocovariance_naive(array &out, const array &a, bool centered);

void normalize(complex_array &in, const std::complex<double> &K);
void normalize(complex_array &in, double K);
void normalize(array &in, double K);

double mse(const array &A, const array &B);
double absolute_error(const array &A, const array &B);

array to_array(const Eigen::MatrixXd &m);
array to_array(const Eigen::MatrixXd &m, index M, index N, index K);

complex_array to_complex_array(const array &A);
complex_array to_complex_array(const array &A, double scale);

array real(const complex_array &in);
array imag(const complex_array &in);

Eigen::MatrixXd to_2d(const array &A);

void print( const array &A );

/**
 *  Creates a new array by shifting all elements such that the elements in the corners
 *  are in the center.  This is useful to display spectrograms and variogram maps, as
 *  this makes interpretation of results intuitive for people (zero frequency or zero
 *  separation in the center).
 */
array shiftByHalf(const array &in);

} // namepsace spectral

