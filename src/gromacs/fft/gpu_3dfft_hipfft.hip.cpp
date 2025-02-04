/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2016,2017,2018,2019,2020,2021, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */

/*! \internal \file
 *  \brief Implements GPU 3D FFT routines for HIP.
 *
 *  \author Aleksei Iupinov <a.yupinov@gmail.com>
 *  \author Mark Abraham <mark.j.abraham@gmail.com>
 *  \ingroup module_fft
 */

#include "gmxpre.h"

#include "gpu_3dfft_hipfft.hpp"

#include "gromacs/gpu_utils/device_stream.h"
#include "gromacs/gpu_utils/devicebuffer.h"
#include "gromacs/utility/arrayref.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/gmxassert.h"

namespace gmx
{
static void handleHipfftError(hipfftResult_t status, const char* msg)
{
    if (status != HIPFFT_SUCCESS)
    {
        gmx_fatal(FARGS, "%s (error code %d)\n", msg, status);
    }
}

Gpu3dFft::ImplHipFft::ImplHipFft(bool allocateGrids,
                                 MPI_Comm /*comm*/,
                                 ArrayRef<const int> gridSizesInXForEachRank,
                                 ArrayRef<const int> gridSizesInYForEachRank,
                                 const int /*nz*/,
                                 bool                 performOutOfPlaceFFT,
                                 const DeviceContext& context,
                                 const DeviceStream&  pmeStream,
                                 ivec                 realGridSize,
                                 ivec                 realGridSizePadded,
                                 ivec                 complexGridSizePadded,
                                 DeviceBuffer<float>* realGrid,
                                 DeviceBuffer<float>* complexGrid) :
    realGrid_(reinterpret_cast<hipfftReal*>(*realGrid)), performOutOfPlaceFFT_(performOutOfPlaceFFT)
{
    GMX_RELEASE_ASSERT(allocateGrids == false, "Grids needs to be pre-allocated");
    GMX_RELEASE_ASSERT(gridSizesInXForEachRank.size() == 1 && gridSizesInYForEachRank.size() == 1,
                       "FFT decomposition not implemented with cuFFT backend");

    if (performOutOfPlaceFFT_)
    {
        const int newComplexGridSize =
                complexGridSizePadded[XX] * complexGridSizePadded[YY] * complexGridSizePadded[ZZ] * 2;

        reallocateDeviceBuffer(
                complexGrid, newComplexGridSize, &complexGridSize_, &complexGridCapacity_, context);
    }
    else
    {
        *complexGrid = *realGrid;
    }

    complexGrid_ = *complexGrid;


    const int complexGridSizePaddedTotal =
            complexGridSizePadded[XX] * complexGridSizePadded[YY] * complexGridSizePadded[ZZ];
    const int realGridSizePaddedTotal =
            realGridSizePadded[XX] * realGridSizePadded[YY] * realGridSizePadded[ZZ];

    GMX_RELEASE_ASSERT(realGrid_, "Bad (null) input real-space grid");
    GMX_RELEASE_ASSERT(complexGrid_, "Bad (null) input complex grid");


#ifdef GMX_GPU_USE_VKFFT
    configuration = {};
    appR2C = {};
    configuration.FFTdim = 3;
    configuration.size[0] = realGridSize[ZZ];
    configuration.size[1] = realGridSize[YY];
    configuration.size[2] = realGridSize[XX];

    configuration.performR2C = 1;
    //configuration.disableMergeSequencesR2C = 1;
    configuration.device = (hipDevice_t*)malloc(sizeof(hipDevice_t));
    hipError_t result = hipGetDevice(configuration.device);
    configuration.stream = pmeStream.stream_pointer();
    configuration.num_streams=1;

    uint64_t bufferSize = complexGridSizePadded[XX]* complexGridSizePadded[YY]* complexGridSizePadded[ZZ] * sizeof(hipfftComplex);
    configuration.bufferSize=&bufferSize;
    configuration.aimThreads = 64;
    configuration.bufferStride[0] = complexGridSizePadded[ZZ];
    configuration.bufferStride[1] = complexGridSizePadded[ZZ]* complexGridSizePadded[YY];
    configuration.bufferStride[2] = complexGridSizePadded[ZZ]* complexGridSizePadded[YY]* complexGridSizePadded[XX];
    configuration.buffer = (void**)&complexGrid_;

    configuration.isInputFormatted = 1;
    configuration.inverseReturnToInputBuffer = 1;
    uint64_t inputBufferSize = realGridSizePadded[XX]* realGridSizePadded[YY]* realGridSizePadded[ZZ] * sizeof(hipfftReal);
    configuration.inputBufferSize = &inputBufferSize;
    configuration.inputBufferStride[0] = realGridSizePadded[ZZ];
    configuration.inputBufferStride[1] = realGridSizePadded[ZZ]* realGridSizePadded[YY];
    configuration.inputBufferStride[2] = realGridSizePadded[ZZ]* realGridSizePadded[YY]* realGridSizePadded[XX];
    configuration.inputBuffer = (void**)&realGrid_;
    VkFFTResult resFFT = initializeVkFFT(&appR2C, configuration);
    if (resFFT!=VKFFT_SUCCESS) printf ("VkFFT error: %d\n", resFFT);
#else

    hipfftResult_t result;
    /* Commented code for a simple 3D grid with no padding */
    /*
       result = hipfftPlan3d(&planR2C_, realGridSize[XX], realGridSize[YY], realGridSize[ZZ],
       HIPFFT_R2C); handleHipfftError(result, "hipfftPlan3d R2C plan failure");

       result = hipfftPlan3d(&planC2R_, realGridSize[XX], realGridSize[YY], realGridSize[ZZ],
       HIPFFT_C2R); handleHipfftError(result, "hipfftPlan3d C2R plan failure");
     */

    const int rank = 3, batch = 1;
    result = hipfftPlanMany(&planR2C_,
                           rank,
                           realGridSize,
                           realGridSizePadded,
                           1,
                           realGridSizePaddedTotal,
                           complexGridSizePadded,
                           1,
                           complexGridSizePaddedTotal,
                           HIPFFT_R2C,
                           batch);
    handleHipfftError(result, "hipfftPlanMany R2C plan failure");

    result = hipfftPlanMany(&planC2R_,
                           rank,
                           realGridSize,
                           complexGridSizePadded,
                           1,
                           complexGridSizePaddedTotal,
                           realGridSizePadded,
                           1,
                           realGridSizePaddedTotal,
                           HIPFFT_C2R,
                           batch);
    handleHipfftError(result, "hipfftPlanMany C2R plan failure");

    hipStream_t stream = pmeStream.stream();
    GMX_RELEASE_ASSERT(stream, "Can not use the default HIP stream for PME cuFFT");

    result = hipfftSetStream(planR2C_, stream);
    handleHipfftError(result, "hipfftSetStream R2C failure");

    result = hipfftSetStream(planC2R_, stream);
    handleHipfftError(result, "hipfftSetStream C2R failure");
#endif
}

Gpu3dFft::ImplHipFft::~ImplHipFft()
{
#ifdef GMX_GPU_USE_VKFFT
    deleteVkFFT(&appR2C);
    free(configuration.device);
#else
    if (performOutOfPlaceFFT_)
    {
        freeDeviceBuffer(&complexGrid_);
    }

    hipfftResult_t result;
    result = hipfftDestroy(planR2C_);
    handleHipfftError(result, "hipfftDestroy R2C failure");
    result = hipfftDestroy(planC2R_);
    handleHipfftError(result, "hipfftDestroy C2R failure");
#endif
}

void Gpu3dFft::ImplHipFft::perform3dFft(gmx_fft_direction dir, CommandEvent* /*timingEvent*/)
{
#ifdef GMX_GPU_USE_VKFFT
    VkFFTResult resFFT = VKFFT_SUCCESS;
#else
    hipfftResult_t result;
#endif
    if (dir == GMX_FFT_REAL_TO_COMPLEX)
    {
#ifdef GMX_GPU_USE_VKFFT
        resFFT = VkFFTAppend(&appR2C, -1, NULL);
        if (resFFT!=VKFFT_SUCCESS) printf ("VkFFT error: %d\n", resFFT);
#else
        result = hipfftExecR2C(planR2C_, realGrid_, (hipfftComplex*)complexGrid_);
        handleHipfftError(result, "hipFFT R2C execution failure");
#endif
    }
    else
    {
#ifdef GMX_GPU_USE_VKFFT
        resFFT = VkFFTAppend(&appR2C, 1, NULL);
        if (resFFT!=VKFFT_SUCCESS) printf ("VkFFT error: %d\n", resFFT);
#else
        result = hipfftExecC2R(planC2R_, (hipfftComplex*)complexGrid_, realGrid_);
        handleHipfftError(result, "hipFFT C2R execution failure");
#endif
    }
}

} // namespace gmx
