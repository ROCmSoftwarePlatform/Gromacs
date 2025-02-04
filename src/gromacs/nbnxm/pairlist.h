/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2012,2013,2014,2015,2016 by the GROMACS development team.
 * Copyright (c) 2017,2018,2019,2020,2021, by the GROMACS development team, led by
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

#ifndef GMX_NBNXM_PAIRLIST_H
#define GMX_NBNXM_PAIRLIST_H

#include <cstddef>

#include "gromacs/gpu_utils/hostallocator.h"
#include "gromacs/math/vectypes.h"
#include "gromacs/mdtypes/locality.h"
#include "gromacs/utility/basedefinitions.h"
#include "gromacs/utility/defaultinitializationallocator.h"
#include "gromacs/utility/enumerationhelpers.h"
#include "gromacs/utility/real.h"

#include "pairlistparams.h"

struct NbnxnPairlistCpuWork;
struct NbnxnPairlistGpuWork;
struct t_nblist;


//! Convenience type for vector with aligned memory
template<typename T>
using AlignedVector = std::vector<T, gmx::AlignedAllocator<T>>;

//! Convenience type for vector that avoids initialization at resize()
template<typename T>
using FastVector = std::vector<T, gmx::DefaultInitializationAllocator<T>>;

/*! \brief Cache-line protection buffer
 *
 * A buffer data structure of 64 bytes
 * to be placed at the beginning and end of structs
 * to avoid cache invalidation of the real contents
 * of the struct by writes to neighboring memory.
 */
typedef struct
{
    //! Unused field used to create space to protect cache lines that are in use
    int dummy[16];
} gmx_cache_protect_t;

/*! \brief This is the actual cluster-pair list j-entry.
 *
 * cj is the j-cluster.
 * The interaction bits in excl are indexed i-major, j-minor.
 * The cj entries are sorted such that ones with exclusions come first.
 * This means that once a full mask (=NBNXN_INTERACTION_MASK_ALL)
 * is found, all subsequent j-entries in the i-entry also have full masks.
 */
struct nbnxn_cj_t
{
    //! The j-cluster
    int cj;
    //! The exclusion (interaction) bits
    unsigned int excl;
};

/*! \brief Constants for interpreting interaction flags
 *
 * In nbnxn_ci_t the integer shift contains the shift in the lower 7 bits.
 * The upper bits contain information for non-bonded kernel optimization.
 * Simply calculating LJ and Coulomb for all pairs in a cluster pair is fine.
 * But three flags can be used to skip interactions, currently only for subc=0
 * !(shift & NBNXN_CI_DO_LJ(subc))   => we can skip LJ for all pairs
 * shift & NBNXN_CI_HALF_LJ(subc)    => we can skip LJ for the second half of i
 * !(shift & NBNXN_CI_DO_COUL(subc)) => we can skip Coulomb for all pairs
 */
//! \{
#define NBNXN_CI_SHIFT 127
#define NBNXN_CI_DO_LJ(subc) (1 << (7 + 3 * (subc)))
#define NBNXN_CI_HALF_LJ(subc) (1 << (8 + 3 * (subc)))
#define NBNXN_CI_DO_COUL(subc) (1 << (9 + 3 * (subc)))
//! \}

/*! \brief Cluster-pair Interaction masks
 *
 * Bit i*j-cluster-size + j tells if atom i and j interact.
 */
//! \{
// TODO: Rename according to convention when moving into Nbnxn namespace
//! All interaction mask is the same for all kernels
constexpr unsigned int NBNXN_INTERACTION_MASK_ALL = 0xffffffffU;
//! 4x4 kernel diagonal mask
constexpr unsigned int NBNXN_INTERACTION_MASK_DIAG = 0x08ceU;
//! 4x2 kernel diagonal masks
//! \{
constexpr unsigned int NBNXN_INTERACTION_MASK_DIAG_J2_0 = 0x0002U;
constexpr unsigned int NBNXN_INTERACTION_MASK_DIAG_J2_1 = 0x002fU;
//! \}
//! 4x8 kernel diagonal masks
//! \{
constexpr unsigned int NBNXN_INTERACTION_MASK_DIAG_J8_0 = 0xf0f8fcfeU;
constexpr unsigned int NBNXN_INTERACTION_MASK_DIAG_J8_1 = 0x0080c0e0U;
//! \}
//! \}

/*! \brief Lower limit for square interaction distances in nonbonded kernels.
 *
 * For smaller values we will overflow when calculating r^-1 or r^-12, but
 * to keep it simple we always apply the limit from the tougher r^-12 condition.
 */
#if GMX_DOUBLE
// Some double precision SIMD architectures use single precision in the first
// step, so although the double precision criterion would allow smaller rsq,
// we need to stay in single precision with some margin for the N-R iterations.
constexpr double c_nbnxnMinDistanceSquared = 1.0e-36;
#else
// The worst intermediate value we might evaluate is r^-12, which
// means we should ensure r^2 stays above pow(GMX_FLOAT_MAX,-1.0/6.0)*1.01 (some margin)
constexpr float c_nbnxnMinDistanceSquared = 3.82e-07F; // r > 6.2e-4
#endif


//! The number of clusters in a super-cluster, used for GPU
constexpr int c_nbnxnGpuNumClusterPerSupercluster = 8;

/*! \brief With GPU kernels we group cluster pairs in 4 to optimize memory usage
 * of integers containing 32 bits.
 */
constexpr int c_nbnxnGpuJgroupSize = (32 / c_nbnxnGpuNumClusterPerSupercluster);

/*! \internal
 * \brief Simple pair-list i-unit
 */
struct nbnxn_ci_t
{
    int cjIndEnd() const { return cj_ind_start + cj_length; }
    //! i-cluster
    int ci;
    //! Start index into cj
    int cj_ind_start;
    //! End index into cj
    short cj_length;
    //! Shift vector index plus possible flags, see above
    short shift;
};

//! Grouped pair-list i-unit
typedef struct nbnxn_sci
{
    //! Returns the number of j-cluster groups in this entry
    int numJClusterGroups() const { return static_cast<int>(cj4_length); }

    int cj4IndEnd() const { return cj4_ind_start + cj4_length; }

    //! i-super-cluster
    int sci;
    //! Start index into cj4
    int cj4_ind_start;
    //! End index into cj4
    short cj4_length;
    //! Shift vector index plus possible flags
    short shift;
} nbnxn_sci_t;

//! Interaction data for a j-group for one warp
struct nbnxn_im_ei_t
{
    //! The i-cluster interactions mask for 1 warp
    unsigned int imask = 0U;
    //! Index into the exclusion array for 1 warp, default index 0 which means no exclusions
    int excl_ind = 0;
};

//! Four-way j-cluster lists
typedef struct
#if GMX_GPU_HIP
alignas(32) // Make sizeof(nbnxn_cj4_t) = 32 when c_nbnxnGpuClusterpairSplit is 1
#endif
{
    //! The 4 j-clusters
    int cj[c_nbnxnGpuJgroupSize];
    //! The i-cluster mask data for 2 warps
    nbnxn_im_ei_t imei[c_nbnxnGpuClusterpairSplit];
} nbnxn_cj4_t;

//! Struct for storing the atom-pair interaction bits for a cluster pair in a GPU pairlist
struct nbnxn_excl_t
{
    //! Constructor, sets no exclusions, so all atom pairs interacting
    MSVC_DIAGNOSTIC_IGNORE(26495) // pair is not being initialized!
    nbnxn_excl_t()
    {
        for (unsigned int& pairEntry : pair)
        {
            pairEntry = NBNXN_INTERACTION_MASK_ALL;
        }
    }
    MSVC_DIAGNOSTIC_RESET

    //! Topology exclusion interaction bits per warp
    unsigned int pair[c_nbnxnGpuExclSize];
};

//! Cluster pairlist type for use on CPUs
struct NbnxnPairlistCpu
{
    NbnxnPairlistCpu();

    //! Cache protection
    gmx_cache_protect_t cp0;

    //! The number of atoms per i-cluster
    int na_ci;
    //! The number of atoms per j-cluster
    int na_cj;
    //! The radius for constructing the list
    real rlist;
    //! The i-cluster list
    FastVector<nbnxn_ci_t> ci;
    //! The outer, unpruned i-cluster list
    FastVector<nbnxn_ci_t> ciOuter;

    //! The j-cluster list, size ncj
    FastVector<nbnxn_cj_t> cj;
    //! The outer, unpruned j-cluster list
    FastVector<nbnxn_cj_t> cjOuter;
    //! The number of j-clusters that are used by ci entries in this list, will be <= cj.size()
    int ncjInUse;

    //! The total number of i clusters
    int nci_tot;

    //! Working data storage for list construction
    std::unique_ptr<NbnxnPairlistCpuWork> work;

    //! Cache protection
    gmx_cache_protect_t cp1;
};

/* Cluster pairlist type, with extra hierarchies, for on the GPU
 *
 * NOTE: for better performance when combining lists over threads,
 *       all vectors should use default initialization. But when
 *       changing this, excl should be intialized when adding entries.
 */
struct NbnxnPairlistGpu
{
    /*! \brief Constructor
     *
     * \param[in] pinningPolicy  Sets the pinning policy for all buffers used on the GPU
     */
    NbnxnPairlistGpu(gmx::PinningPolicy pinningPolicy);

    //! Cache protection
    gmx_cache_protect_t cp0;

    //! The number of atoms per i-cluster
    int na_ci;
    //! The number of atoms per j-cluster
    int na_cj;
    //! The number of atoms per super cluster
    int na_sc;
    //! The radius for constructing the list
    real rlist;
    // The i-super-cluster list, indexes into cj4;
    gmx::HostVector<nbnxn_sci_t> sci;
    // The list of 4*j-cluster groups
    gmx::HostVector<nbnxn_cj4_t> cj4;
    // Atom interaction bits (non-exclusions)
    gmx::HostVector<nbnxn_excl_t> excl;
    // The total number of i-clusters
    int nci_tot;

    //! Working data storage for list construction
    std::unique_ptr<NbnxnPairlistGpuWork> work;

    //! Cache protection
    gmx_cache_protect_t cp1;
};

#endif
