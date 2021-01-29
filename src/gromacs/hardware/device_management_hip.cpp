#include "hip/hip_runtime.h"
/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2012,2013,2014,2015,2016, by the GROMACS development team.
 * Copyright (c) 2017,2018,2019,2020, by the GROMACS development team, led by
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
 *  \brief Defines the CUDA implementations of the device management.
 *
 *  \author Anca Hamuraru <anca@streamcomputing.eu>
 *  \author Dimitrios Karkoulis <dimitris.karkoulis@gmail.com>
 *  \author Teemu Virolainen <teemu@streamcomputing.eu>
 *  \author Mark Abraham <mark.j.abraham@gmail.com>
 *  \author Szilárd Páll <pall.szilard@gmail.com>
 *  \author Artem Zhmurov <zhmurov@gmail.com>
 *
 * \ingroup module_hardware
 */
#include "gmxpre.h"

#include "device_management.h"

#include <assert.h>

#include "gromacs/gpu_utils/cudautils_hip.h"
#include "gromacs/gpu_utils/device_context.h"
#include "gromacs/gpu_utils/device_stream.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/programcontext.h"
#include "gromacs/utility/smalloc.h"
#include "gromacs/utility/stringutil.h"

#include "device_information.h"

/*! \internal \brief
 * Max number of devices supported by CUDA (for consistency checking).
 *
 * In reality it is 16 with CUDA <=v5.0, but let's stay on the safe side.
 */
static int c_cudaMaxDeviceCount = 32;

/** Dummy kernel used for sanity checking. */
static __global__ void dummy_kernel(void) {}

static hipError_t checkCompiledTargetCompatibility(int deviceId, const hipDeviceProp_t& deviceProp)
{
    hipFuncAttributes attributes;
    hipError_t        stat = hipFuncGetAttributes(&attributes, reinterpret_cast<const void*>(dummy_kernel));

    if (hipErrorInvalidDeviceFunction == stat)
    {
        fprintf(stderr,
                "\nWARNING: The %s binary does not include support for the CUDA architecture of "
                "the GPU ID #%d (compute capability %d.%d) detected during detection. "
                "By default, GROMACS supports all architectures of compute "
                "capability >= 3.0, so your GPU "
                "might be rare, or some architectures were disabled in the build. \n"
                "Consult the install guide for how to use the GMX_CUDA_TARGET_SM and "
                "GMX_CUDA_TARGET_COMPUTE CMake variables to add this architecture. \n",
                gmx::getProgramContext().displayName(), deviceId, deviceProp.major, deviceProp.minor);
    }

    return stat;
}

/*!
 * \brief Runs GPU sanity checks.
 *
 * Runs a series of checks to determine that the given GPU and underlying CUDA
 * driver/runtime functions properly.
 *
 * \todo Currently we do not make a distinction between the type of errors
 *       that can appear during functionality checks. This needs to be improved,
 *       e.g if the dummy test kernel fails to execute with a "device busy message"
 *       we should appropriately report that the device is busy instead of NonFunctional.
 *
 * \todo Introduce errors codes and handle errors more smoothly.
 *
 *
 * \param[in]  deviceInfo  Device information on the device to check.
 * \returns                The status enumeration value for the checked device:
 */
static DeviceStatus isDeviceFunctional(const DeviceInformation& deviceInfo)
{
    hipError_t cu_err;

    /* both major & minor is 9999 if no CUDA capable devices are present */
    if (deviceInfo.prop.major == 9999 && deviceInfo.prop.minor == 9999)
    {
        return DeviceStatus::NonFunctional;
    }
    /* we don't care about emulation mode */
    if (deviceInfo.prop.major == 0)
    {
        return DeviceStatus::NonFunctional;
    }

    cu_err = hipSetDevice(deviceInfo.id);
    if (cu_err != hipSuccess)
    {
        fprintf(stderr, "Error while switching to device #%d. %s\n", deviceInfo.id,
                gmx::getDeviceErrorString(cu_err).c_str());
        return DeviceStatus::NonFunctional;
    }

    cu_err = checkCompiledTargetCompatibility(deviceInfo.id, deviceInfo.prop);
    // Avoid triggering an error if GPU devices are in exclusive or prohibited mode;
    // it is enough to check for cudaErrorDevicesUnavailable only here because
    // if we encounter it that will happen in hipFuncGetAttributes in the above function.
    if (cu_err == hipErrorInvalidDevice)
    {
        return DeviceStatus::Unavailable;
    }
    else if (cu_err != hipSuccess)
    {
        return DeviceStatus::NonFunctional;
    }

    /* try to execute a dummy kernel */
    try
    {
        KernelLaunchConfig config;
        config.blockSize[0]                = 512;
        const auto          dummyArguments = prepareGpuKernelArguments(dummy_kernel, config);
        const DeviceContext deviceContext(deviceInfo);
        const DeviceStream  deviceStream(deviceContext, DeviceStreamPriority::Normal, false);
        //launchGpuKernel(dummy_kernel, config, deviceStream, nullptr, "Dummy kernel", dummyArguments);
        launchGpuKernel(dummy_kernel, config, deviceStream, nullptr, "Dummy kernel");
    }
    catch (gmx::GromacsException& ex)
    {
        // launchGpuKernel error is not fatal and should continue with marking the device bad
        fprintf(stderr,
                "Error occurred while running dummy kernel sanity check on device #%d:\n %s\n",
                deviceInfo.id, formatExceptionMessageToString(ex).c_str());
        return DeviceStatus::NonFunctional;
    }

    if (hipDeviceSynchronize() != hipSuccess)
    {
        return DeviceStatus::NonFunctional;
    }

    cu_err = hipDeviceReset();
    CU_RET_ERR(cu_err, "hipDeviceReset failed");

    return DeviceStatus::Compatible;
}

/*! \brief Returns true if the gpu characterized by the device properties is supported
 *         by the native gpu acceleration.
 *
 * \param[in] deviceProperties  The CUDA device properties of the gpus to test.
 * \returns                     True if the GPU properties passed indicate a compatible
 *                              GPU, otherwise false.
 */
static bool isDeviceGenerationSupported(const hipDeviceProp_t& deviceProperties)
{
    return (deviceProperties.major >= 3);
}

/*! \brief Checks if a GPU with a given ID is supported by the native GROMACS acceleration.
 *
 *  Returns a status value which indicates compatibility or one of the following
 *  errors: incompatibility or insanity (=unexpected behavior).
 *
 *  As the error handling only permits returning the state of the GPU, this function
 *  does not clear the CUDA runtime API status allowing the caller to inspect the error
 *  upon return. Note that this also means it is the caller's responsibility to
 *  reset the CUDA runtime state.
 *
 *  \param[in]  deviceInfo The device information on the device to check.
 *  \returns               the status of the requested device
 */
static DeviceStatus checkDeviceStatus(const DeviceInformation& deviceInfo)
{
    if (!isDeviceGenerationSupported(deviceInfo.prop))
    {
        return DeviceStatus::Incompatible;
    }
    return isDeviceFunctional(deviceInfo);
}

bool isDeviceDetectionFunctional(std::string* errorMessage)
{
    hipError_t stat;
    int         driverVersion = -1;
    stat                      = hipDriverGetVersion(&driverVersion);
    GMX_ASSERT(stat != hipErrorInvalidValue,
               "An impossible null pointer was passed to hipDriverGetVersion");
    GMX_RELEASE_ASSERT(stat == hipSuccess,
                       ("An unexpected value was returned from hipDriverGetVersion. "
                        + gmx::getDeviceErrorString(stat))
                               .c_str());
    bool foundDriver = (driverVersion > 0);
    if (!foundDriver)
    {
        // Can't detect GPUs if there is no driver
        if (errorMessage != nullptr)
        {
            errorMessage->assign("No valid CUDA driver found");
        }
        return false;
    }

    int numDevices;
    stat = hipGetDeviceCount(&numDevices);
    if (stat != hipSuccess)
    {
        if (errorMessage != nullptr)
        {
            /* hipGetDeviceCount failed which means that there is
             * something wrong with the machine: driver-runtime
             * mismatch, all GPUs being busy in exclusive mode,
             * invalid CUDA_VISIBLE_DEVICES, or some other condition
             * which should result in GROMACS issuing at least a
             * warning. */
            errorMessage->assign(hipGetErrorString(stat));
        }

        // Consume the error now that we have prepared to handle
        // it. This stops it reappearing next time we check for
        // errors. Note that if CUDA_VISIBLE_DEVICES does not contain
        // valid devices, then hipGetLastError returns the
        // (undocumented) hipErrorNoDevice, but this should not be a
        // problem as there should be no future CUDA API calls.
        // NVIDIA bug report #2038718 has been filed.
        hipGetLastError();
        // Can't detect GPUs
        return false;
    }

    // We don't actually use numDevices here, that's not the job of
    // this function.
    return true;
}

std::vector<std::unique_ptr<DeviceInformation>> findDevices()
{
    int         numDevices;
    hipError_t stat = hipGetDeviceCount(&numDevices);
    gmx::checkDeviceError(stat,
                          "Invalid call of findDevices() when CUDA API returned an error, perhaps "
                          "canPerformDeviceDetection() was not called appropriately beforehand.");

    /* things might go horribly wrong if cudart is not compatible with the driver */
    numDevices = std::min(numDevices, c_cudaMaxDeviceCount);

    // We expect to start device support/sanity checks with a clean runtime error state
    gmx::ensureNoPendingDeviceError("Trying to find available CUDA devices.");

    std::vector<std::unique_ptr<DeviceInformation>> deviceInfoList(numDevices);
    for (int i = 0; i < numDevices; i++)
    {
        hipDeviceProp_t prop;
        memset(&prop, 0, sizeof(hipDeviceProp_t));
        stat = hipGetDeviceProperties(&prop, i);

        deviceInfoList[i]               = std::make_unique<DeviceInformation>();
        deviceInfoList[i]->id           = i;
        deviceInfoList[i]->prop         = prop;
        deviceInfoList[i]->deviceVendor = DeviceVendor::Nvidia;

        const DeviceStatus checkResult = (stat != hipSuccess) ? DeviceStatus::NonFunctional
                                                               : checkDeviceStatus(*deviceInfoList[i]);

        deviceInfoList[i]->status = checkResult;

        if (checkResult != DeviceStatus::Compatible)
        {
            // TODO:
            //  - we inspect the CUDA API state to retrieve and record any
            //    errors that occurred during is_gmx_supported_gpu_id() here,
            //    but this would be more elegant done within is_gmx_supported_gpu_id()
            //    and only return a string with the error if one was encountered.
            //  - we'll be reporting without rank information which is not ideal.
            //  - we'll end up warning also in cases where users would already
            //    get an error before mdrun aborts.
            //
            // Here we also clear the CUDA API error state so potential
            // errors during sanity checks don't propagate.
            const std::string errorMessage = gmx::formatString(
                    "An error occurred while sanity checking device #%d.", deviceInfoList[i]->id);
            gmx::ensureNoPendingDeviceError(errorMessage);
        }
    }

    stat = hipPeekAtLastError();
    GMX_RELEASE_ASSERT(
            stat == hipSuccess,
            ("We promise to return with clean CUDA state, but non-success state encountered. "
             + gmx::getDeviceErrorString(stat))
                    .c_str());

    return deviceInfoList;
}

void setActiveDevice(const DeviceInformation& deviceInfo)
{
    int         deviceId = deviceInfo.id;
    hipError_t stat;

    stat = hipSetDevice(deviceId);
    if (stat != hipSuccess)
    {
        auto message = gmx::formatString("Failed to initialize GPU #%d", deviceId);
        CU_RET_ERR(stat, message.c_str());
    }

    if (debug)
    {
        fprintf(stderr, "Initialized GPU ID #%d: %s\n", deviceId, deviceInfo.prop.name);
    }
}

void releaseDevice(DeviceInformation* deviceInfo)
{
    // device was used is that deviceInfo will be non-null.
    if (deviceInfo != nullptr)
    {
        hipError_t stat;

        int gpuid;
        stat = hipGetDevice(&gpuid);
        if (stat == hipSuccess)
        {
            if (debug)
            {
                fprintf(stderr, "Cleaning up context on GPU ID #%d.\n", gpuid);
            }

            stat = hipDeviceReset();
            if (stat != hipSuccess)
            {
                gmx_warning("Failed to free GPU #%d. %s", gpuid, gmx::getDeviceErrorString(stat).c_str());
            }
        }
    }
}

std::string getDeviceInformationString(const DeviceInformation& deviceInfo)
{
    bool gpuExists = (deviceInfo.status != DeviceStatus::Nonexistent
                      && deviceInfo.status != DeviceStatus::NonFunctional);

    if (!gpuExists)
    {
        return gmx::formatString("#%d: %s, stat: %s", deviceInfo.id, "N/A",
                                 c_deviceStateString[deviceInfo.status]);
    }
    else
    {
        return gmx::formatString("#%d: NVIDIA %s, compute cap.: %d.%d, ECC: %3s, stat: %s",
                                 deviceInfo.id, deviceInfo.prop.name, deviceInfo.prop.major,
                                 deviceInfo.prop.minor, deviceInfo.prop.ECCEnabled ? "yes" : " no",
                                 c_deviceStateString[deviceInfo.status]);
    }
}