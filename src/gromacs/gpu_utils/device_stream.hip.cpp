/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2020, by the GROMACS development team, led by
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
 * \brief Implements the DeviceStream for HIP.
 *
 * \author Artem Zhmurov <zhmurov@gmail.com>
 *
 * \ingroup module_gpu_utils
 */
#include "gmxpre.h"

#include "device_stream.h"

#include "gromacs/gpu_utils/hiputils.hpp"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/gmxassert.h"
#include "gromacs/utility/stringutil.h"

DeviceStream::DeviceStream(const DeviceContext& /* deviceContext */,
                           DeviceStreamPriority priority,
                           const bool /* useTiming */)
{
    hipError_t stat;

    stream_pointer_ = new hipStream_t[1];

    if (priority == DeviceStreamPriority::Normal)
    {
        stat = hipStreamCreate(&stream_);
        gmx::checkDeviceError(stat, "Could not create HIP stream.");
    }
    else if (priority == DeviceStreamPriority::High)
    {
        // Note that the device we're running on does not have to
        // support priorities, because we are querying the priority
        // range, which in that case will be a single value.
        int highestPriority;
        stat = hipDeviceGetStreamPriorityRange(nullptr, &highestPriority);
        gmx::checkDeviceError(stat, "Could not query HIP stream priority range.");

        stat = hipStreamCreateWithPriority(&stream_, hipStreamDefault, highestPriority);
        gmx::checkDeviceError(stat, "Could not create HIP stream with high priority.");
    }
    stream_pointer_[0] = stream_;
}

DeviceStream::~DeviceStream()
{
    if (isValid())
    {
        hipError_t stat = hipStreamDestroy(stream_);
        GMX_RELEASE_ASSERT(stat == hipSuccess,
                           ("Failed to release HIP stream. " + gmx::getDeviceErrorString(stat)).c_str());
        stream_ = nullptr;

        delete stream_pointer_;
        stream_pointer_ = nullptr;
    }
}

hipStream_t DeviceStream::stream() const
{
    return stream_;
}

hipStream_t* DeviceStream::stream_pointer() const
{
    return stream_pointer_;
}

bool DeviceStream::isValid() const
{
    return (stream_ != nullptr);
}

void DeviceStream::synchronize() const
{
    hipError_t stat = hipStreamSynchronize(stream_);
    GMX_RELEASE_ASSERT(stat == hipSuccess,
                       ("hipStreamSynchronize failed. " + gmx::getDeviceErrorString(stat)).c_str());
}
