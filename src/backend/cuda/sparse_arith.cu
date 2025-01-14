/*******************************************************
 * Copyright (c) 2014, ArrayFire
 * All rights reserved.
 *
 * This file is distributed under 3-clause BSD license.
 * The complete license agreement can be obtained at:
 * http://arrayfire.com/licenses/BSD-3-Clause
 ********************************************************/

#include <sparse_arith.hpp>

#include <arith.hpp>
#include <common/cast.hpp>
#include <common/err_common.hpp>
#include <common/unique_handle.hpp>
#include <complex.hpp>
#include <copy.hpp>
#include <cusparse.hpp>
#include <kernel/sparse_arith.hpp>
#include <lookup.hpp>
#include <math.hpp>
#include <platform.hpp>
#include <sparse.hpp>
#include <where.hpp>

#include <stdexcept>
#include <string>

namespace cuda {

using namespace common;
using std::numeric_limits;

template<typename T>
T getInf() {
    return scalar<T>(numeric_limits<T>::infinity());
}

template<>
cfloat getInf() {
    return scalar<cfloat, float>(
        NAN, NAN);  // Matches behavior of complex division by 0 in CUDA
}

template<>
cdouble getInf() {
    return scalar<cdouble, double>(
        NAN, NAN);  // Matches behavior of complex division by 0 in CUDA
}

template<typename T, af_op_t op>
Array<T> arithOpD(const SparseArray<T> &lhs, const Array<T> &rhs,
                  const bool reverse) {
    lhs.eval();
    rhs.eval();

    Array<T> out  = createEmptyArray<T>(dim4(0));
    Array<T> zero = createValueArray<T>(rhs.dims(), scalar<T>(0));
    switch (op) {
        case af_add_t: out = copyArray<T>(rhs); break;
        case af_sub_t:
            out = reverse ? copyArray<T>(rhs)
                          : arithOp<T, af_sub_t>(zero, rhs, rhs.dims());
            break;
        default: out = copyArray<T>(rhs);
    }
    out.eval();
    switch (lhs.getStorage()) {
        case AF_STORAGE_CSR:
            kernel::sparseArithOpCSR<T, op>(out, lhs.getValues(),
                                            lhs.getRowIdx(), lhs.getColIdx(),
                                            rhs, reverse);
            break;
        case AF_STORAGE_COO:
            kernel::sparseArithOpCOO<T, op>(out, lhs.getValues(),
                                            lhs.getRowIdx(), lhs.getColIdx(),
                                            rhs, reverse);
            break;
        default:
            AF_ERROR("Sparse Arithmetic only supported for CSR or COO",
                     AF_ERR_NOT_SUPPORTED);
    }

    return out;
}

template<typename T, af_op_t op>
SparseArray<T> arithOp(const SparseArray<T> &lhs, const Array<T> &rhs,
                       const bool reverse) {
    lhs.eval();
    rhs.eval();

    SparseArray<T> out = createArrayDataSparseArray<T>(
        lhs.dims(), lhs.getValues(), lhs.getRowIdx(), lhs.getColIdx(),
        lhs.getStorage(), true);
    out.eval();
    switch (lhs.getStorage()) {
        case AF_STORAGE_CSR:
            kernel::sparseArithOpCSR<T, op>(out.getValues(), out.getRowIdx(),
                                            out.getColIdx(), rhs, reverse);
            break;
        case AF_STORAGE_COO:
            kernel::sparseArithOpCOO<T, op>(out.getValues(), out.getRowIdx(),
                                            out.getColIdx(), rhs, reverse);
            break;
        default:
            AF_ERROR("Sparse Arithmetic only supported for CSR or COO",
                     AF_ERR_NOT_SUPPORTED);
    }

    return out;
}

#define SPARSE_ARITH_OP_FUNC_DEF(FUNC) \
    template<typename T>               \
    FUNC##_def<T> FUNC##_func();

#define SPARSE_ARITH_OP_FUNC(FUNC, TYPE, INFIX)  \
    template<>                                   \
    FUNC##_def<TYPE> FUNC##_func<TYPE>() {       \
        cusparseModule &_ = getCusparsePlugin(); \
        return _.cusparse##INFIX##FUNC;          \
    }

#if CUDA_VERSION >= 11000

template<typename T>
using csrgeam2_buffer_size_def = cusparseStatus_t (*)(
    cusparseHandle_t, int, int, const T *, const cusparseMatDescr_t, int,
    const T *, const int *, const int *, const T *, const cusparseMatDescr_t,
    int, const T *, const int *, const int *, const cusparseMatDescr_t,
    const T *, const int *, const int *, size_t *);

#define SPARSE_ARITH_OP_BUFFER_SIZE_FUNC_DEF(FUNC) \
    template<typename T>                           \
    FUNC##_buffer_size_def<T> FUNC##_buffer_size_func();

SPARSE_ARITH_OP_BUFFER_SIZE_FUNC_DEF(csrgeam2);

#define SPARSE_ARITH_OP_BUFFER_SIZE_FUNC(FUNC, TYPE, INFIX)        \
    template<>                                                     \
    FUNC##_buffer_size_def<TYPE> FUNC##_buffer_size_func<TYPE>() { \
        cusparseModule &_ = getCusparsePlugin();                   \
        return _.cusparse##INFIX##FUNC##_bufferSizeExt;            \
    }

SPARSE_ARITH_OP_BUFFER_SIZE_FUNC(csrgeam2, float, S);
SPARSE_ARITH_OP_BUFFER_SIZE_FUNC(csrgeam2, double, D);
SPARSE_ARITH_OP_BUFFER_SIZE_FUNC(csrgeam2, cfloat, C);
SPARSE_ARITH_OP_BUFFER_SIZE_FUNC(csrgeam2, cdouble, Z);

template<typename T>
using csrgeam2_def = cusparseStatus_t (*)(cusparseHandle_t, int, int, const T *,
                                          const cusparseMatDescr_t, int,
                                          const T *, const int *, const int *,
                                          const T *, const cusparseMatDescr_t,
                                          int, const T *, const int *,
                                          const int *, const cusparseMatDescr_t,
                                          T *, int *, int *, void *);

SPARSE_ARITH_OP_FUNC_DEF(csrgeam2);

SPARSE_ARITH_OP_FUNC(csrgeam2, float, S);
SPARSE_ARITH_OP_FUNC(csrgeam2, double, D);
SPARSE_ARITH_OP_FUNC(csrgeam2, cfloat, C);
SPARSE_ARITH_OP_FUNC(csrgeam2, cdouble, Z);

#else

template<typename T>
using csrgeam_def = cusparseStatus_t (*)(cusparseHandle_t, int, int, const T *,
                                         const cusparseMatDescr_t, int,
                                         const T *, const int *, const int *,
                                         const T *, const cusparseMatDescr_t,
                                         int, const T *, const int *,
                                         const int *, const cusparseMatDescr_t,
                                         T *, int *, int *);

SPARSE_ARITH_OP_FUNC_DEF(csrgeam);

SPARSE_ARITH_OP_FUNC(csrgeam, float, S);
SPARSE_ARITH_OP_FUNC(csrgeam, double, D);
SPARSE_ARITH_OP_FUNC(csrgeam, cfloat, C);
SPARSE_ARITH_OP_FUNC(csrgeam, cdouble, Z);

#endif

template<typename T, af_op_t op>
SparseArray<T> arithOp(const SparseArray<T> &lhs, const SparseArray<T> &rhs) {
    lhs.eval();
    rhs.eval();

    af::storage sfmt      = lhs.getStorage();
    auto desc             = make_handle<cusparseMatDescr_t>();
    const dim4 ldims      = lhs.dims();
    const int M           = ldims[0];
    const int N           = ldims[1];
    const dim_t nnzA      = lhs.getNNZ();
    const dim_t nnzB      = rhs.getNNZ();
    const int *csrRowPtrA = lhs.getRowIdx().get();
    const int *csrColPtrA = lhs.getColIdx().get();
    const int *csrRowPtrB = rhs.getRowIdx().get();
    const int *csrColPtrB = rhs.getColIdx().get();

    auto outRowIdx = createEmptyArray<int>(dim4(M + 1));

    int *csrRowPtrC = outRowIdx.get();
    int baseC, nnzC;
    int *nnzcDevHostPtr = &nnzC;

    T alpha           = scalar<T>(1);
    T beta            = op == af_sub_t ? scalar<T>(-1) : alpha;
    cusparseModule &_ = getCusparsePlugin();

#if CUDA_VERSION >= 11000
    size_t pBufferSize = 0;

    csrgeam2_buffer_size_func<T>()(
        sparseHandle(), M, N, &alpha, desc, nnzA, lhs.getValues().get(),
        csrRowPtrA, csrColPtrA, &beta, desc, nnzB, rhs.getValues().get(),
        csrRowPtrB, csrColPtrB, desc, NULL, csrRowPtrC, NULL, &pBufferSize);

    auto tmpBuffer = createEmptyArray<char>(dim4(pBufferSize));

    CUSPARSE_CHECK(_.cusparseXcsrgeam2Nnz(
        sparseHandle(), M, N, desc, nnzA, csrRowPtrA, csrColPtrA, desc, nnzB,
        csrRowPtrB, csrColPtrB, desc, csrRowPtrC, nnzcDevHostPtr,
        tmpBuffer.get()));
#else
    CUSPARSE_CHECK(_.cusparseXcsrgeamNnz(
        sparseHandle(), M, N, desc, nnzA, csrRowPtrA, csrColPtrA, desc, nnzB,
        csrRowPtrB, csrColPtrB, desc, csrRowPtrC, nnzcDevHostPtr));
#endif
    if (NULL != nnzcDevHostPtr) {
        nnzC = *nnzcDevHostPtr;
    } else {
        CUDA_CHECK(cudaMemcpyAsync(&nnzC, csrRowPtrC + M, sizeof(int),
                                   cudaMemcpyDeviceToHost,
                                   cuda::getActiveStream()));
        CUDA_CHECK(cudaMemcpyAsync(&baseC, csrRowPtrC, sizeof(int),
                                   cudaMemcpyDeviceToHost,
                                   cuda::getActiveStream()));
        CUDA_CHECK(cudaStreamSynchronize(cuda::getActiveStream()));
        nnzC -= baseC;
    }

    auto outColIdx = createEmptyArray<int>(dim4(nnzC));
    auto outValues = createEmptyArray<T>(dim4(nnzC));
#if CUDA_VERSION >= 11000
    csrgeam2_func<T>()(sparseHandle(), M, N, &alpha, desc, nnzA,
                       lhs.getValues().get(), csrRowPtrA, csrColPtrA, &beta,
                       desc, nnzB, rhs.getValues().get(), csrRowPtrB,
                       csrColPtrB, desc, outValues.get(), csrRowPtrC,
                       outColIdx.get(), tmpBuffer.get());
#else
    csrgeam_func<T>()(sparseHandle(), M, N, &alpha, desc, nnzA,
                      lhs.getValues().get(), csrRowPtrA, csrColPtrA, &beta,
                      desc, nnzB, rhs.getValues().get(), csrRowPtrB, csrColPtrB,
                      desc, outValues.get(), csrRowPtrC, outColIdx.get());
#endif
    SparseArray<T> retVal = createArrayDataSparseArray(
        ldims, outValues, outRowIdx, outColIdx, sfmt);
    return retVal;
}

#define INSTANTIATE(T)                                                         \
    template Array<T> arithOpD<T, af_add_t>(                                   \
        const SparseArray<T> &lhs, const Array<T> &rhs, const bool reverse);   \
    template Array<T> arithOpD<T, af_sub_t>(                                   \
        const SparseArray<T> &lhs, const Array<T> &rhs, const bool reverse);   \
    template Array<T> arithOpD<T, af_mul_t>(                                   \
        const SparseArray<T> &lhs, const Array<T> &rhs, const bool reverse);   \
    template Array<T> arithOpD<T, af_div_t>(                                   \
        const SparseArray<T> &lhs, const Array<T> &rhs, const bool reverse);   \
    template SparseArray<T> arithOp<T, af_add_t>(                              \
        const SparseArray<T> &lhs, const Array<T> &rhs, const bool reverse);   \
    template SparseArray<T> arithOp<T, af_sub_t>(                              \
        const SparseArray<T> &lhs, const Array<T> &rhs, const bool reverse);   \
    template SparseArray<T> arithOp<T, af_mul_t>(                              \
        const SparseArray<T> &lhs, const Array<T> &rhs, const bool reverse);   \
    template SparseArray<T> arithOp<T, af_div_t>(                              \
        const SparseArray<T> &lhs, const Array<T> &rhs, const bool reverse);   \
    template SparseArray<T> arithOp<T, af_add_t>(                              \
        const common::SparseArray<T> &lhs, const common::SparseArray<T> &rhs); \
    template SparseArray<T> arithOp<T, af_sub_t>(                              \
        const common::SparseArray<T> &lhs, const common::SparseArray<T> &rhs); \
    template SparseArray<T> arithOp<T, af_mul_t>(                              \
        const common::SparseArray<T> &lhs, const common::SparseArray<T> &rhs); \
    template SparseArray<T> arithOp<T, af_div_t>(                              \
        const common::SparseArray<T> &lhs, const common::SparseArray<T> &rhs);

INSTANTIATE(float)
INSTANTIATE(double)
INSTANTIATE(cfloat)
INSTANTIATE(cdouble)

}  // namespace cuda
