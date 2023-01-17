/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright 2014- The GROMACS Authors
 * and the project initiators Erik Lindahl, Berk Hess and David van der Spoel.
 * Consult the AUTHORS/COPYING files and https://www.gromacs.org for details.
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
 * https://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at https://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out https://www.gromacs.org.
 */
#include "gmxpre.h"

#include "gromacs/utility/baseversion.h"

#include "config.h"

#include "gromacs/utility/gmxassert.h"

#include "baseversion_gen.h"

const char* gmx_version()
{
    return gmx_ver_string;
}

const char* gmx_version_git_full_hash()
{
    return gmx_full_git_hash;
}

const char* gmx_version_git_central_base_hash()
{
    return gmx_central_base_hash;
}

const char* gmxDOI()
{
    return gmxSourceDoiString;
}

#if GMX_DOUBLE
void gmx_is_double_precision() {}
#else
void gmx_is_single_precision() {}
#endif

const char* getGpuImplementationString()
{
    // Some flavors of clang complain about unreachable returns.
#ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wunreachable-code-return"
#endif
    if (GMX_GPU)
    {
        if (GMX_GPU_CUDA)
        {
            return "CUDA";
        }
        else if (GMX_GPU_HIP)
        {
            return "HIP";
        }
        else if (GMX_GPU_OPENCL)
        {
            return "OpenCL";
        }
        else if (GMX_GPU_SYCL)
        {
            if (GMX_SYCL_DPCPP)
            {
                return "SYCL (DPCPP)";
            }
            else if (GMX_SYCL_HIPSYCL)
            {
                return "SYCL (hipSYCL)";
            }
            else
            {
                return "SYCL (unknown)";
            }
        }
        else
        {
            GMX_RELEASE_ASSERT(false, "Unknown GPU configuration");
            return "impossible";
        }
    }
    else
    {
        return "disabled";
    }
#ifdef __clang__
#    pragma clang diagnostic pop
#endif
}
