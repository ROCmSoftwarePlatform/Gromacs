#include "hip/hip_runtime.h"
/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2019,2020,2021, by the GROMACS development team, led by
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
 * \brief Implements Leap-Frog using HIP
 *
 * This file contains HIP implementation of back-end specific code for Leap-Frog.
 *
 * \author Artem Zhmurov <zhmurov@gmail.com>
 *
 * \ingroup module_mdlib
 */
#include "gmxpre.h"

#include "leapfrog_gpu_internal.h"

#include "gromacs/gpu_utils/hiputils.hpp"
#include "gromacs/gpu_utils/devicebuffer.h"
#include "gromacs/gpu_utils/typecasts.hpp"
#include "gromacs/gpu_utils/vectype_ops.hpp"
#include "gromacs/math/vec.h"
#include "gromacs/mdtypes/group.h"
#include "gromacs/pbcutil/pbc.h"
#include "gromacs/pbcutil/pbc_aiuc_hip.hpp"
#include "gromacs/utility/arrayref.h"

namespace gmx
{

/*!\brief Number of HIP threads in a block
 *
 * \todo Check if using smaller block size will lead to better performance.
 */
constexpr static int c_threadsPerBlock = 64;
//! Maximum number of threads in a block (for __launch_bounds__)
constexpr static int c_maxThreadsPerBlock = c_threadsPerBlock;

/*! \brief Main kernel for Leap-Frog integrator.
 *
 *  The coordinates and velocities are updated on the GPU. Also saves the intermediate values of the coordinates for
 *   further use in constraints.
 *
 *  Each GPU thread works with a single particle. Empty declaration is needed to
 *  avoid "no previous prototype for function" clang warning.
 *
 *  \todo Check if the force should be set to zero here.
 *  \todo This kernel can also accumulate incidental temperatures for each atom.
 *
 * \tparam        numTempScaleValues               The number of different T-couple values.
 * \tparam        velocityScaling                  Type of the Parrinello-Rahman velocity rescaling.
 * \param[in]     numAtoms                         Total number of atoms.
 * \param[in,out] gm_x                             Coordinates to update upon integration.
 * \param[out]    gm_xp                            A copy of the coordinates before the integration (for constraints).
 * \param[in,out] gm_v                             Velocities to update.
 * \param[in,out] gm_v                             PME grids to zero-out,
 * \param[in]     gm_f                             Atomic forces.
 * \param[in]     gm_inverseMasses                 Reciprocal masses.
 * \param[in]     dt                               Timestep.
 * \param[in]     gm_lambdas                       Temperature scaling factors (one per group)
 * \param[in]     gm_tempScaleGroups               Mapping of atoms into groups.
 * \param[in]     prVelocityScalingMatrixDiagonal  Diagonal elements of Parrinello-Rahman velocity scaling matrix
 */
template<NumTempScaleValues numTempScaleValues, VelocityScalingType velocityScaling>
__launch_bounds__(c_threadsPerBlock) __global__
        void leapfrog_kernel(const int numAtoms,
                             float3* __restrict__ gm_x,
                             float3* __restrict__ gm_xp,
                             float3* __restrict__ gm_v,
                             const bool           isPmeRank, 
                             const int            realGridSize, 
                             float*  __restrict__ gm_grid,
                             const float3* __restrict__ gm_f,
                             const float* __restrict__ gm_inverseMasses,
                             const float dt,
                             const float* __restrict__ gm_lambdas,
                             const unsigned short* __restrict__ gm_tempScaleGroups,
                             const float3 prVelocityScalingMatrixDiagonal)
{
    int threadIndex = blockIdx.x * c_threadsPerBlock + threadIdx.x;
    if (threadIndex < numAtoms)
    {
        float3 x    = gm_x[threadIndex];
        float3 v    = gm_v[threadIndex];
        float3 f    = gm_f[threadIndex];
        float  im   = gm_inverseMasses[threadIndex];
        float  imdt = im * dt;

        // Swapping places for xp and x so that the x will contain the updated coordinates and xp - the
        // coordinates before update. This should be taken into account when (if) constraints are applied
        // after the update: x and xp have to be passed to constraints in the 'wrong' order.
        // TODO: Issue #3727
        gm_xp[threadIndex] = x;

        if (numTempScaleValues != NumTempScaleValues::None || velocityScaling != VelocityScalingType::None)
        {
            float3 vp = v;

            if (numTempScaleValues != NumTempScaleValues::None)
            {
                float lambda = 1.0F;
                if (numTempScaleValues == NumTempScaleValues::Single)
                {
                    lambda = gm_lambdas[0];
                }
                else if (numTempScaleValues == NumTempScaleValues::Multiple)
                {
                    int tempScaleGroup = gm_tempScaleGroups[threadIndex];
                    lambda             = gm_lambdas[tempScaleGroup];
                }
                vp *= lambda;
            }

            if (velocityScaling == VelocityScalingType::Diagonal)
            {
                vp.x -= prVelocityScalingMatrixDiagonal.x * v.x;
                vp.y -= prVelocityScalingMatrixDiagonal.y * v.y;
                vp.z -= prVelocityScalingMatrixDiagonal.z * v.z;
            }

            v = vp;
        }

        v = v + f * imdt;

        x = x + v * dt;
        gm_v[threadIndex] = v;
        gm_x[threadIndex] = x;
    }

    // TODO remove as if updateGPU is true we obviously have a single-rank
    if(isPmeRank){
        // printf("hey doin isPmerank!\n");
        int stride = gridDim.x * blockDim.x;
        for(int k = threadIndex; k < realGridSize; k += stride)
        {
            // zero-out pme_grid
            gm_grid[k] = 0;
        }
    }
}

/*! \brief Select templated kernel.
 *
 * Returns pointer to a HIP kernel based on the number of temperature coupling groups and
 * whether or not the temperature and(or) pressure coupling is enabled.
 *
 * \param[in]  doTemperatureScaling   If the kernel with temperature coupling velocity scaling
 *                                    should be selected.
 * \param[in]  numTempScaleValues     Number of temperature coupling groups in the system.
 * \param[in]  prVelocityScalingType  Type of the Parrinello-Rahman velocity scaling.
 *
 * \retrun                         Pointer to HIP kernel
 */
inline auto selectLeapFrogKernelPtr(bool                doTemperatureScaling,
                                    int                 numTempScaleValues,
                                    VelocityScalingType prVelocityScalingType)
{
    // Check input for consistency: if there is temperature coupling, at least one coupling group should be defined.
    GMX_ASSERT(!doTemperatureScaling || (numTempScaleValues > 0),
               "Temperature coupling was requested with no temperature coupling groups.");
    auto kernelPtr = leapfrog_kernel<NumTempScaleValues::None, VelocityScalingType::None>;

    if (prVelocityScalingType == VelocityScalingType::None)
    {
        if (!doTemperatureScaling)
        {
            kernelPtr = leapfrog_kernel<NumTempScaleValues::None, VelocityScalingType::None>;
        }
        else if (numTempScaleValues == 1)
        {
            kernelPtr = leapfrog_kernel<NumTempScaleValues::Single, VelocityScalingType::None>;
        }
        else if (numTempScaleValues > 1)
        {
            kernelPtr = leapfrog_kernel<NumTempScaleValues::Multiple, VelocityScalingType::None>;
        }
    }
    else if (prVelocityScalingType == VelocityScalingType::Diagonal)
    {
        if (!doTemperatureScaling)
        {
            kernelPtr = leapfrog_kernel<NumTempScaleValues::None, VelocityScalingType::Diagonal>;
        }
        else if (numTempScaleValues == 1)
        {
            kernelPtr = leapfrog_kernel<NumTempScaleValues::Single, VelocityScalingType::Diagonal>;
        }
        else if (numTempScaleValues > 1)
        {
            kernelPtr = leapfrog_kernel<NumTempScaleValues::Multiple, VelocityScalingType::Diagonal>;
        }
    }
    else
    {
        GMX_RELEASE_ASSERT(false,
                           "Only isotropic Parrinello-Rahman pressure coupling is supported.");
    }
    return kernelPtr;
}


void launchLeapFrogKernel(const int                          numAtoms,
                          DeviceBuffer<Float3>               d_x,
                          DeviceBuffer<Float3>               d_xp,
                          DeviceBuffer<Float3>               d_v,
                          const bool                         isPmeRank, 
                          const int                          realGridSize, 
                          DeviceBuffer<real>                 d_realGrid,
                          const DeviceBuffer<Float3>         d_f,
                          const DeviceBuffer<float>          d_inverseMasses,
                          const float                        dt,
                          const bool                         doTemperatureScaling,
                          const int                          numTempScaleValues,
                          const DeviceBuffer<unsigned short> d_tempScaleGroups,
                          const DeviceBuffer<float>          d_lambdas,
                          const VelocityScalingType          prVelocityScalingType,
                          const Float3                       prVelocityScalingMatrixDiagonal,
                          const DeviceStream&                deviceStream)
{
    // Checking the buffer types against the kernel argument types
    static_assert(sizeof(*d_inverseMasses) == sizeof(float), "Incompatible types");

    KernelLaunchConfig kernelLaunchConfig;

    kernelLaunchConfig.gridSize[0]      = (numAtoms + c_threadsPerBlock - 1) / c_threadsPerBlock;
    kernelLaunchConfig.blockSize[0]     = c_threadsPerBlock;
    kernelLaunchConfig.blockSize[1]     = 1;
    kernelLaunchConfig.blockSize[2]     = 1;
    kernelLaunchConfig.sharedMemorySize = 0;

    auto kernelPtr =
            selectLeapFrogKernelPtr(doTemperatureScaling, numTempScaleValues, prVelocityScalingType);
    // fprintf(stderr, "Starting to prepare gpu kernel arguments\n");
    const auto kernelArgs = prepareGpuKernelArguments(kernelPtr,
                                                      kernelLaunchConfig,
                                                      &numAtoms,
                                                      asFloat3Pointer(&d_x),
                                                      asFloat3Pointer(&d_xp),
                                                      asFloat3Pointer(&d_v),
                                                      &isPmeRank, 
                                                      &realGridSize,
                                                      &d_realGrid,
                                                      asFloat3Pointer(&d_f),
                                                      &d_inverseMasses,
                                                      &dt,
                                                      &d_lambdas,
                                                      &d_tempScaleGroups,
                                                      &prVelocityScalingMatrixDiagonal);
    // hipError_t err = hipStreamSynchronize(deviceStream.stream());                                                  
    // fprintf(stderr, "Finishing preparing gpu kernel arguments, error = %d\n", err);
    // fprintf(stderr, "realGridSize %d d_grid %p\n", realGridSize, d_realGrid);
    launchGpuKernel(kernelPtr, kernelLaunchConfig, deviceStream, nullptr, "leapfrog_kernel", kernelArgs);
    // err = hipStreamSynchronize(deviceStream.stream());
    // fprintf(stderr, " finished leapFrogKernel, error = %d\n", err);
}

} // namespace gmx
