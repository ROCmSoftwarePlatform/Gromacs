#include "hip/hip_runtime.h"
/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2020,2021, by the GROMACS development team, led by
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
 *
 * \brief Implements GPU Force Reduction using HIP
 *
 * \author Alan Gray <alang@nvidia.com>
 *
 * \ingroup module_mdlib
 */

#include "gmxpre.h"

#include "gpuforcereduction_impl_internal.h"

#include "gromacs/gpu_utils/hiputils.hpp"
#include "gromacs/gpu_utils/devicebuffer.h"
#include "gromacs/gpu_utils/typecasts.hpp"
#include "gromacs/gpu_utils/vectype_ops.hpp"

namespace gmx
{

constexpr static int c_threadsPerBlock = 64;

template<bool addRvecForce, bool accumulateForce>
__launch_bounds__(c_threadsPerBlock)
static __global__ void reduceKernel(const float3* __restrict__ gm_nbnxmForce,
                                    const float3* __restrict__ rvecForceToAdd,
                                    float3*    gm_fTotal,
                                    const int* gm_cell,
                                    const int  numAtoms)
{

    // map particle-level parallelism to 1D HIP thread and block index
    const int threadIndex = blockIdx.x * c_threadsPerBlock + threadIdx.x;

    // perform addition for each particle
    if (threadIndex < numAtoms)
    {

        float3* gm_fDest = &gm_fTotal[threadIndex];
        float3  temp;

        // Accumulate or set nbnxm force
        if (accumulateForce)
        {
            temp = *gm_fDest;
            temp = temp + gm_nbnxmForce[gm_cell[threadIndex]];
        }
        else
        {
            temp = gm_nbnxmForce[gm_cell[threadIndex]];
        }

        if (addRvecForce)
        {
            temp = temp + rvecForceToAdd[threadIndex];
        }

        *gm_fDest = temp;
    }
}

void launchForceReductionKernel(int                        numAtoms,
                                int                        atomStart,
                                bool                       addRvecForce,
                                bool                       accumulate,
                                const DeviceBuffer<Float3> d_nbnxmForceToAdd,
                                const DeviceBuffer<Float3> d_rvecForceToAdd,
                                DeviceBuffer<Float3>       d_baseForce,
                                DeviceBuffer<int>          d_cell,
                                const DeviceStream&        deviceStream)
{
    float3* d_baseForcePtr      = &(asFloat3(d_baseForce)[atomStart]);
    float3* d_nbnxmForcePtr     = asFloat3(d_nbnxmForceToAdd);
    float3* d_rvecForceToAddPtr = &(asFloat3(d_rvecForceToAdd)[atomStart]);

    // Configure and launch kernel
    KernelLaunchConfig config;
    config.blockSize[0]     = c_threadsPerBlock;
    config.blockSize[1]     = 1;
    config.blockSize[2]     = 1;
    config.gridSize[0]      = ((numAtoms + 1) + c_threadsPerBlock - 1) / c_threadsPerBlock;
    config.gridSize[1]      = 1;
    config.gridSize[2]      = 1;
    config.sharedMemorySize = 0;

    auto kernelFn = addRvecForce
                            ? (accumulate ? reduceKernel<true, true> : reduceKernel<true, false>)
                            : (accumulate ? reduceKernel<false, true> : reduceKernel<false, false>);

    const auto kernelArgs = prepareGpuKernelArguments(
            kernelFn, config, &d_nbnxmForcePtr, &d_rvecForceToAddPtr, &d_baseForcePtr, &d_cell, &numAtoms);

    launchGpuKernel(kernelFn, config, deviceStream, nullptr, "Force Reduction", kernelArgs);
}

} // namespace gmx
