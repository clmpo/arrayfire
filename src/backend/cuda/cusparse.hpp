/*******************************************************
 * Copyright (c) 2014, ArrayFire
 * All rights reserved.
 *
 * This file is distributed under 3-clause BSD license.
 * The complete license agreement can be obtained at:
 * http://arrayfire.com/licenses/BSD-3-Clause
 ********************************************************/

#pragma once

#include <common/defines.hpp>
#include <common/err_common.hpp>
#include <common/unique_handle.hpp>
#include <cusparseModule.hpp>
#include <cusparse_v2.h>

// clang-format off
DEFINE_HANDLER(cusparseHandle_t, cuda::getCusparsePlugin().cusparseCreate, cuda::getCusparsePlugin().cusparseDestroy);
DEFINE_HANDLER(cusparseMatDescr_t, cuda::getCusparsePlugin().cusparseCreateMatDescr, cuda::getCusparsePlugin().cusparseDestroyMatDescr);
#if defined(AF_USE_NEW_CUSPARSE_API)
DEFINE_HANDLER(cusparseSpMatDescr_t, cuda::getCusparsePlugin().cusparseCreateCsr, cuda::getCusparsePlugin().cusparseDestroySpMat);
DEFINE_HANDLER(cusparseDnVecDescr_t, cuda::getCusparsePlugin().cusparseCreateDnVec, cuda::getCusparsePlugin().cusparseDestroyDnVec);
DEFINE_HANDLER(cusparseDnMatDescr_t, cuda::getCusparsePlugin().cusparseCreateDnMat, cuda::getCusparsePlugin().cusparseDestroyDnMat);
#endif
// clang-format on

namespace cuda {

const char* errorString(cusparseStatus_t err);

#define CUSPARSE_CHECK(fn)                                                    \
    do {                                                                      \
        cusparseStatus_t _error = fn;                                         \
        if (_error != CUSPARSE_STATUS_SUCCESS) {                              \
            char _err_msg[1024];                                              \
            snprintf(_err_msg, sizeof(_err_msg), "CUSPARSE Error (%d): %s\n", \
                     (int)(_error), cuda::errorString(_error));               \
                                                                              \
            AF_ERROR(_err_msg, AF_ERR_INTERNAL);                              \
        }                                                                     \
    } while (0)

}  // namespace cuda
