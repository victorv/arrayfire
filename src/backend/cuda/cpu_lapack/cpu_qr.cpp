/*******************************************************
 * Copyright (c) 2014, ArrayFire
 * All rights reserved.
 *
 * This file is distributed under 3-clause BSD license.
 * The complete license agreement can be obtained at:
 * http://arrayfire.com/licenses/BSD-3-Clause
 ********************************************************/

#if defined(WITH_CPU_LINEAR_ALGEBRA)

#include <cpu_lapack/cpu_qr.hpp>
#include <err_common.hpp>
#include <af/dim4.hpp>
#include <handle.hpp>
#include <copy.hpp>
#include <iostream>
#include <cassert>

#include <cpu_lapack/cpu_triangle.hpp>
#include "lapack_helper.hpp"

namespace cuda
{
namespace cpu
{

template<typename T>
using geqrf_func_def = int (*)(ORDER_TYPE, int, int,
                               T*, int,
                               T*);

template<typename T>
using gqr_func_def = int (*)(ORDER_TYPE, int, int, int,
                             T*, int,
                             const T*);

#define QR_FUNC_DEF( FUNC )                                         \
template<typename T> FUNC##_func_def<T> FUNC##_func();


#define QR_FUNC( FUNC, TYPE, PREFIX )                               \
template<> FUNC##_func_def<TYPE>     FUNC##_func<TYPE>()            \
{ return & LAPACK_NAME(PREFIX##FUNC); }

QR_FUNC_DEF( geqrf )
QR_FUNC(geqrf , float  , s)
QR_FUNC(geqrf , double , d)
QR_FUNC(geqrf , cfloat , c)
QR_FUNC(geqrf , cdouble, z)

#define GQR_FUNC_DEF( FUNC )                                         \
template<typename T> FUNC##_func_def<T> FUNC##_func();

#define GQR_FUNC( FUNC, TYPE, PREFIX )                               \
template<> FUNC##_func_def<TYPE>     FUNC##_func<TYPE>()             \
{ return & LAPACK_NAME(PREFIX); }

GQR_FUNC_DEF( gqr )
GQR_FUNC(gqr , float  , sorgqr)
GQR_FUNC(gqr , double , dorgqr)
GQR_FUNC(gqr , cfloat , cungqr)
GQR_FUNC(gqr , cdouble, zungqr)

template<typename T>
void qr(Array<T> &q, Array<T> &r, Array<T> &t, const Array<T> &in)
{
    dim4 iDims = in.dims();
    int M = iDims[0];
    int N = iDims[1];

    dim4 padDims(M, max(M, N));
    q = padArray<T, T>(in, padDims, scalar<T>(0));
    q.resetDims(iDims);

    dim4 qdims = q.dims();

    T *tPtr = NULL;
    T *qPtr = NULL;
    int nT = 0;
    {
        ///////////////////////////////////////////////
        // QR Inplace on q
        int M_ = qdims[0];
        int N_ = qdims[1];
        nT = min(M_, N_);

        tPtr = pinnedAlloc<T>(nT);
        qPtr = pinnedAlloc<T>(padDims.elements());
        q.resetDims(padDims);
        copyData(qPtr, q);
        q.resetDims(iDims);

        geqrf_func<T>()(AF_LAPACK_COL_MAJOR, M, N,
                        qPtr, M,
                        tPtr);
        ///////////////////////////////////////////////
    }

    // SPLIT into q and r
    dim4 rdims(M, N);
    T *rPtr = pinnedAlloc<T>(rdims.elements());

    dim4 rst(1, rdims[0], rdims[0] * rdims[1], rdims[0] * rdims[1] * rdims[2]);

    triangle<T, true, false>(rPtr, qPtr, rdims, rst, q.strides());

    gqr_func<T>()(AF_LAPACK_COL_MAJOR,
                  M, M, min(M, N),
                  qPtr, q.strides()[1],
                  tPtr);

    q.resetDims(dim4(M, M));

    t = createHostDataArray<T>(af::dim4(nT), tPtr);
    r = createHostDataArray<T>(rdims, rPtr);
    writeHostDataArray<T>(q, qPtr, q.elements() * sizeof(T));

    pinnedFree(tPtr);
    pinnedFree(rPtr);
    pinnedFree(qPtr);
}

template<typename T>
Array<T> qr_inplace(Array<T> &in)
{
    dim4 iDims = in.dims();
    int M = iDims[0];
    int N = iDims[1];

    T *tPtr  = pinnedAlloc<T>(min(M, N));
    T *inPtr = pinnedAlloc<T>(in.elements());
    copyData(inPtr, in);

    geqrf_func<T>()(AF_LAPACK_COL_MAJOR, M, N,
                    inPtr, in.strides()[1],
                    tPtr);

    writeHostDataArray<T>(in, inPtr, in.elements() * sizeof(T));
    Array<T> t = createHostDataArray<T>(af::dim4(min(M, N)), tPtr);

    pinnedFree(inPtr);
    pinnedFree(tPtr);

    return t;
}

#define INSTANTIATE_QR(T)                                                                           \
    template Array<T> qr_inplace<T>(Array<T> &in);                                                \
    template void qr<T>(Array<T> &q, Array<T> &r, Array<T> &t, const Array<T> &in);

INSTANTIATE_QR(float)
INSTANTIATE_QR(cfloat)
INSTANTIATE_QR(double)
INSTANTIATE_QR(cdouble)

}
}

#endif
