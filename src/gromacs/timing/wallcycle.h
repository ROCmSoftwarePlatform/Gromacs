/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2008, The GROMACS development team.
 * Copyright (c) 2013,2014,2015,2017,2018 by the GROMACS development team.
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
#ifndef GMX_TIMING_WALLCYCLE_H
#define GMX_TIMING_WALLCYCLE_H

/* NOTE: None of the routines here are safe to call within an OpenMP
 * region */

#include "config.h"

#include <stdio.h>

#include <array>
#include <memory>
#include <vector>

#if GMX_USE_ROCTRACER
#include <roctracer/roctx.h>
#endif

#include "gromacs/timing/cyclecounter.h"
#include "gromacs/utility/basedefinitions.h"
#include "gromacs/utility/enumerationhelpers.h"

#ifndef DEBUG_WCYCLE
/*! \brief Enables consistency checking for the counters.
 *
 * If the macro is set to 1, code checks if you stop a counter different from the last
 * one that was opened and if you do nest too deep.
 */
#    define DEBUG_WCYCLE 0
#endif

struct t_commrec;

#ifndef DEBUG_WCYCLE
/*! \brief Enables consistency checking for the counters.
 *
 * If the macro is set to 1, code checks if you stop a counter different from the last
 * one that was opened and if you do nest too deep.
 */
#    define DEBUG_WCYCLE 0
#endif

enum class WallCycleCounter : int
{
    Run,
    Step,
    PpDuringPme,
    Domdec,
    DDCommLoad,
    DDCommBound,
    VsiteConstr,
    PpPmeSendX,
    NS,
    LaunchGpu,
    MoveX,
    Force,
    MoveF,
    PmeMesh,
    PmeRedistXF,
    PmeSpread,
    PmeGather,
    PmeFft,
    PmeFftComm,
    LJPme,
    PmeSolve,
    PmeWaitComm,
    PpPmeWaitRecvF,
    WaitGpuPmeSpread,
    PmeFftMixedMode,
    PmeSolveMixedMode,
    WaitGpuPmeGather,
    WaitGpuBonded,
    PmeGpuFReduction,
    WaitGpuNbNL,
    WaitGpuNbL,
    WaitGpuStatePropagatorData,
    NbXFBufOps,
    VsiteSpread,
    PullPot,
    Awh,
    Traj,
    Update,
    Constr,
    MoveE,
    Rot,
    RotAdd,
    Swap,
    Imd,
    Test,
    Count
};

const char* enumValuetoString(WallCycleCounter enumValue);

enum class WallCycleSubCounter : int
{
    DDRedist,
    DDGrid,
    DDSetupComm,
    DDMakeTop,
    DDMakeConstr,
    DDTopOther,
    DDGpu,
    NBSGridLocal,
    NBSGridNonLocal,
    NBSSearchLocal,
    NBSSearchNonLocal,
    Listed,
    ListedFep,
    Restraints,
    ListedBufOps,
    NonbondedPruning,
    NonbondedKernel,
    NonbondedClear,
    NonbondedFep,
    NonbondedFepReduction,
    LaunchGpuNonBonded,
    LaunchGpuBonded,
    LaunchGpuPme,
    LaunchStatePropagatorData,
    EwaldCorrection,
    NBXBufOps,
    NBFBufOps,
    ClearForceBuffer,
    LaunchGpuNBXBufOps,
    LaunchGpuNBFBufOps,
    LaunchGpuMoveX,
    LaunchGpuMoveF,
    LaunchGpuUpdateConstrain,
    Test,
    Count
};

const char* enumValuetoString(WallCycleSubCounter enumValue);

static constexpr int sc_numWallCycleCounters        = static_cast<int>(WallCycleCounter::Count);
static constexpr int sc_numWallCycleSubCounters     = static_cast<int>(WallCycleSubCounter::Count);
static constexpr int sc_numWallCycleCountersSquared = sc_numWallCycleCounters * sc_numWallCycleCounters;
static constexpr bool sc_useCycleSubcounters        = GMX_CYCLE_SUBCOUNTERS;

struct wallcc_t
{
    int          n;
    gmx_cycles_t c;
    gmx_cycles_t start;
};

#if DEBUG_WCYCLE
static constexpr int c_MaxWallCycleDepth = 6;
#endif


struct gmx_wallcycle
{
    gmx::EnumerationArray<WallCycleCounter, wallcc_t> wcc;
    /* did we detect one or more invalid cycle counts */
    bool haveInvalidCount;
    /* variables for testing/debugging */
    bool                  wc_barrier;
    std::vector<wallcc_t> wcc_all;
    int                   wc_depth;
#if DEBUG_WCYCLE
    std::array<WallCycleCounter, c_MaxWallCycleDepth> counterlist;
    int                                               count_depth;
    bool                                              isMasterRank;
#endif
    WallCycleCounter                                     ewc_prev;
    gmx_cycles_t                                         cycle_prev;
    int64_t                                              reset_counters;
    const t_commrec*                                     cr;
    gmx::EnumerationArray<WallCycleSubCounter, wallcc_t> wcsc;
};

//! Returns if cycle counting is supported
bool wallcycle_have_counter();

//! Returns the wall cycle structure.
std::unique_ptr<gmx_wallcycle> wallcycle_init(FILE* fplog, int resetstep, const t_commrec* cr);

//! Adds custom barrier for wallcycle counting.
void wallcycleBarrier(gmx_wallcycle* wc);

void wallcycle_sub_get(gmx_wallcycle* wc, WallCycleSubCounter ewcs, int* n, double* c);
/* Returns the cumulative count and sub cycle count for ewcs */

inline void wallcycle_all_start(gmx_wallcycle* wc, WallCycleCounter ewc, gmx_cycles_t cycle)
{
    wc->ewc_prev   = ewc;
    wc->cycle_prev = cycle;
}

inline void wallcycle_all_stop(gmx_wallcycle* wc, WallCycleCounter ewc, gmx_cycles_t cycle)
{
    const int prev    = static_cast<int>(wc->ewc_prev);
    const int current = static_cast<int>(ewc);
    wc->wcc_all[prev * sc_numWallCycleCounters + current].n += 1;
    wc->wcc_all[prev * sc_numWallCycleCounters + current].c += cycle - wc->cycle_prev;
}

//! Starts the cycle counter (and increases the call count)
inline void wallcycle_start(gmx_wallcycle* wc, WallCycleCounter ewc)
{
    if (wc == nullptr)
    {
        return;
    }
#if GMX_USE_ROCTRACER
    roctxRangePush(enumValuetoString(ewc));
#endif

    wallcycleBarrier(wc);

#if DEBUG_WCYCLE
    debug_start_check(wc, ewc);
#endif
    gmx_cycles_t cycle = gmx_cycles_read();
    wc->wcc[ewc].start = cycle;
    if (!wc->wcc_all.empty())
    {
        wc->wc_depth++;
        if (ewc == WallCycleCounter::Run)
        {
            wallcycle_all_start(wc, ewc, cycle);
        }
        else if (wc->wc_depth == 3)
        {
            wallcycle_all_stop(wc, ewc, cycle);
        }
    }
}

//! Starts the cycle counter without increasing the call count
inline void wallcycle_start_nocount(gmx_wallcycle* wc, WallCycleCounter ewc)
{
    if (wc == nullptr)
    {
        return;
    }
    wallcycle_start(wc, ewc);
    wc->wcc[ewc].n--;
}

//! Stop the cycle count for ewc , returns the last cycle count
inline double wallcycle_stop(gmx_wallcycle* wc, WallCycleCounter ewc)
{
    gmx_cycles_t cycle, last;

    if (wc == nullptr)
    {
        return 0;
    }

    wallcycleBarrier(wc);

#if DEBUG_WCYCLE
    debug_stop_check(wc, ewc);
#endif

    /* When processes or threads migrate between cores, the cycle counting
     * can get messed up if the cycle counter on different cores are not
     * synchronized. When this happens we expect both large negative and
     * positive cycle differences. We can detect negative cycle differences.
     * Detecting too large positive counts if difficult, since count can be
     * large, especially for ewcRUN. If we detect a negative count,
     * we will not print the cycle accounting table.
     */
    cycle = gmx_cycles_read();
    if (cycle >= wc->wcc[ewc].start)
    {
        last = cycle - wc->wcc[ewc].start;
    }
    else
    {
        last                 = 0;
        wc->haveInvalidCount = true;
    }
    wc->wcc[ewc].c += last;
    wc->wcc[ewc].n++;
    if (!wc->wcc_all.empty())
    {
        wc->wc_depth--;
        if (ewc == WallCycleCounter::Run)
        {
            wallcycle_all_stop(wc, ewc, cycle);
        }
        else if (wc->wc_depth == 2)
        {
            wallcycle_all_start(wc, ewc, cycle);
        }
    }

#if GMX_USE_ROCTRACER
    roctxRangePop();
#endif
    return last;
}

//! Only increment call count for ewc by one
inline void wallcycle_increment_event_count(gmx_wallcycle* wc, WallCycleCounter ewc)
{
    if (wc == nullptr)
    {
        return;
    }
    wc->wcc[ewc].n++;
}

//! Returns the cumulative count and cycle count for ewc
void wallcycle_get(gmx_wallcycle* wc, WallCycleCounter ewc, int* n, double* c);

//! Resets all cycle counters to zero
void wallcycle_reset_all(gmx_wallcycle* wc);

//! Scale the cycle counts to reflect how many threads run for that number of cycles
void wallcycle_scale_by_num_threads(gmx_wallcycle* wc, bool isPmeRank, int nthreads_pp, int nthreads_pme);

//! Return reset_counters from wc struct
int64_t wcycle_get_reset_counters(gmx_wallcycle* wc);

//! Set reset_counters
void wcycle_set_reset_counters(gmx_wallcycle* wc, int64_t reset_counters);

//! Set the start sub cycle count for ewcs
inline void wallcycle_sub_start(gmx_wallcycle* wc, WallCycleSubCounter ewcs)
{
    if (sc_useCycleSubcounters && wc != nullptr)
    {
    #if GMX_USE_ROCTRACER
        roctxRangePush(enumValuetoString(ewcs));
    #endif
        wc->wcsc[ewcs].start = gmx_cycles_read();
    }
}

//! Set the start sub cycle count for ewcs without increasing the call count
inline void wallcycle_sub_start_nocount(gmx_wallcycle* wc, WallCycleSubCounter ewcs)
{
    if (sc_useCycleSubcounters && wc != nullptr)
    {
        wallcycle_sub_start(wc, ewcs);
        wc->wcsc[ewcs].n--;
    }
}

//! Stop the sub cycle count for ewcs
inline void wallcycle_sub_stop(gmx_wallcycle* wc, WallCycleSubCounter ewcs)
{
    if (sc_useCycleSubcounters && wc != nullptr)
    {
#if GMX_USE_ROCTRACER
        roctxRangePop();
#endif
        wc->wcsc[ewcs].c += gmx_cycles_read() - wc->wcsc[ewcs].start;
        wc->wcsc[ewcs].n++;
    }
}

#endif
