/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2015,2016,2017,2018,2019,2020,2021, by the GROMACS development team, led by
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
 * \brief
 * Implements the Awh class.
 *
 * \author Viveca Lindahl
 * \author Berk Hess <hess@kth.se>
 * \author Magnus Lundborg
 * \ingroup module_awh
 */

#include "gmxpre.h"

#include "awh.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <vector>

#include "gromacs/fileio/enxio.h"
#include "gromacs/gmxlib/network.h"
#include "gromacs/math/units.h"
#include "gromacs/math/utilities.h"
#include "gromacs/mdrunutility/multisim.h"
#include "gromacs/mdtypes/awh_history.h"
#include "gromacs/mdtypes/awh_params.h"
#include "gromacs/mdtypes/commrec.h"
#include "gromacs/mdtypes/forceoutput.h"
#include "gromacs/mdtypes/inputrec.h"
#include "gromacs/mdtypes/pull_params.h"
#include "gromacs/mdtypes/state.h"
#include "gromacs/pbcutil/pbc.h"
#include "gromacs/pulling/pull.h"
#include "gromacs/timing/wallcycle.h"
#include "gromacs/trajectory/energyframe.h"
#include "gromacs/utility/arrayref.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/gmxassert.h"
#include "gromacs/utility/pleasecite.h"
#include "gromacs/gpu_utils/gpu_utils.h"

#include "bias.h"
#include "biassharing.h"
#include "correlationgrid.h"
#include "pointstate.h"
#ifdef GMX_USE_ROCTX
#include "roctx.h"
#endif

namespace gmx
{

/*! \internal
 * \brief A bias and its coupling to the system.
 *
 * This struct is used to separate the bias machinery in the Bias class,
 * which should be independent from the reaction coordinate, from the
 * obtaining of the reaction coordinate values and passing the computed forces.
 * Currently the AWH method couples to the system by mapping each
 * AWH bias to a pull coordinate. This can easily be generalized here.
 */
struct BiasCoupledToSystem
{
    /*! \brief Constructor, couple a bias to a set of pull coordinates.
     *
     * \param[in] bias            The bias.
     * \param[in] pullCoordIndex  The pull coordinate indices.
     */
    BiasCoupledToSystem(Bias bias, const std::vector<int>& pullCoordIndex);

    Bias                   bias_;           /**< The bias. */
    const std::vector<int> pullCoordIndex_; /**< The pull coordinates this bias acts on. */

    /* Here AWH can be extended to work on other coordinates than pull. */
};

/*! \brief Checks whether any dimension uses the given coordinate provider type.
 *
 * \param[in] awhBiasParams The bias params to check.
 * \param[in] awhCoordProvider The type of coordinate provider
 * \returns true if any dimension of the bias is linked to the given provider
 */
static bool anyDimUsesProvider(const AwhBiasParams&            awhBiasParams,
                               const AwhCoordinateProviderType awhCoordProvider)
{
    return std::any_of(awhBiasParams.dimParams().begin(),
                       awhBiasParams.dimParams().end(),
                       [&awhCoordProvider](const auto& awhDimParam) {
                           return awhDimParam.coordinateProvider() == awhCoordProvider;
                       });
}

/*! \brief Checks whether any dimension uses the given coordinate provider type.
 *
 * \param[in] awhParams The AWH params to check.
 * \param[in] awhCoordProvider The type of coordinate provider
 * \returns true if any dimension of awh is linked to the given provider type.
 */
static bool anyDimUsesProvider(const AwhParams& awhParams, const AwhCoordinateProviderType awhCoordProvider)
{
    return std::any_of(awhParams.awhBiasParams().begin(),
                       awhParams.awhBiasParams().end(),
                       [&awhCoordProvider](const auto& awhBiasParam) {
                           return anyDimUsesProvider(awhBiasParam, awhCoordProvider);
                       });
}

/*! \brief Checks whether any dimension uses pulling as a coordinate provider.
 *
 * \param[in] biasCoupledToSystem The AWH biases to check.
 * \returns true if any dimension of the provided biases is linked to pulling.
 */
static bool anyDimUsesPull(const ArrayRef<BiasCoupledToSystem> biasCoupledToSystem)
{
    return std::any_of(biasCoupledToSystem.begin(), biasCoupledToSystem.end(), [](const auto& biasCts) {
        return !biasCts.pullCoordIndex_.empty();
    });
}

BiasCoupledToSystem::BiasCoupledToSystem(Bias bias, const std::vector<int>& pullCoordIndex) :
    bias_(std::move(bias)), pullCoordIndex_(pullCoordIndex)
{
    /* We already checked for this in grompp, but check again here. */
    GMX_RELEASE_ASSERT(
            static_cast<size_t>(bias_.ndim()) == pullCoordIndex_.size() + bias_.hasFepLambdaDimension() ? 1 : 0,
            "The bias dimensionality should match the number of pull and lambda coordinates.");
}

Awh::Awh(FILE*                 fplog,
         const t_inputrec&     inputRecord,
         const t_commrec*      commRecord,
         const gmx_multisim_t* multiSimRecord,
         const AwhParams&      awhParams,
         const std::string&    biasInitFilename,
         pull_t*               pull_work,
         int                   numFepLambdaStates,
         int                   fepLambdaState) :
    seed_(awhParams.seed()),
    nstout_(awhParams.nstout()),
    commRecord_(commRecord),
    pull_(pull_work),
    potentialOffset_(0),
    numFepLambdaStates_(numFepLambdaStates),
    fepLambdaState_(fepLambdaState)
{
    if (anyDimUsesProvider(awhParams, AwhCoordinateProviderType::Pull))
    {
        GMX_RELEASE_ASSERT(inputRecord.pull != nullptr, "With AWH we should have pull parameters");
        GMX_RELEASE_ASSERT(pull_work != nullptr,
                           "With AWH pull should be initialized before initializing AWH");
    }

    if (fplog != nullptr)
    {
        please_cite(fplog, "Lindahl2014");

        if (anyDimUsesProvider(awhParams, AwhCoordinateProviderType::FreeEnergyLambda))
        {
            please_cite(fplog, "Lundborg2021");
        }
    }

    if (haveBiasSharingWithinSimulation(awhParams))
    {
        /* This has likely been checked by grompp, but throw anyhow. */
        GMX_THROW(
                InvalidInputError("Biases within a simulation are shared, currently sharing of "
                                  "biases is only supported between simulations"));
    }

    if (awhParams.shareBiasMultisim() && multiSimRecord != nullptr)
    {
        GMX_RELEASE_ASSERT(commRecord, "Need a valid commRecord");
        biasSharing_ = std::make_unique<BiasSharing>(awhParams, *commRecord, multiSimRecord->mastersComm_);
        if (fplog)
        {
            for (int k = 0; k < awhParams.numBias(); k++)
            {
                const int shareGroup = awhParams.awhBiasParams()[k].shareGroup();
                if (shareGroup > 0)
                {
                    fprintf(fplog,
                            "awh%d: bias with share group %d is shared between %d simulations\n",
                            1 + k,
                            shareGroup,
                            biasSharing_->numSharingSimulations(k));
                }
                else
                {
                    fprintf(fplog, "awh%d: bias is not shared between simulations\n", 1 + k);
                }
            }
        }
    }

    /* Initialize all the biases */
    const double beta          = 1 / (gmx::c_boltz * inputRecord.opts.ref_t[0]);
    const auto&  awhBiasParams = awhParams.awhBiasParams();
    for (int k = 0; k < gmx::ssize(awhBiasParams); k++)
    {
        std::vector<int>       pullCoordIndex;
        std::vector<DimParams> dimParams;
        for (const auto& awhDimParam : awhBiasParams[k].dimParams())
        {
            if (awhDimParam.coordinateProvider() != AwhCoordinateProviderType::Pull
                && awhDimParam.coordinateProvider() != AwhCoordinateProviderType::FreeEnergyLambda)
            {
                GMX_THROW(
                        InvalidInputError("Currently only the pull code and lambda are supported "
                                          "as coordinate providers"));
            }
            if (awhDimParam.coordinateProvider() == AwhCoordinateProviderType::Pull)
            {
                const t_pull_coord& pullCoord = inputRecord.pull->coord[awhDimParam.coordinateIndex()];
                if (pullCoord.eGeom == PullGroupGeometry::DirectionPBC)
                {
                    GMX_THROW(InvalidInputError(
                            "Pull geometry 'direction-periodic' is not supported by AWH"));
                }
                double conversionFactor = pull_conversion_factor_userinput2internal(pullCoord);
                pullCoordIndex.push_back(awhDimParam.coordinateIndex());
                dimParams.emplace_back(DimParams::pullDimParams(
                        conversionFactor, awhDimParam.forceConstant(), beta));
            }
            else
            {
                dimParams.push_back(DimParams::fepLambdaDimParams(numFepLambdaStates_, beta));
            }
        }

        /* Construct the bias and couple it to the system. */
        Bias::ThisRankWillDoIO thisRankWillDoIO =
                (MASTER(commRecord_) ? Bias::ThisRankWillDoIO::Yes : Bias::ThisRankWillDoIO::No);
        biasCoupledToSystem_.emplace_back(Bias(k,
                                               awhParams,
                                               awhBiasParams[k],
                                               dimParams,
                                               beta,
                                               inputRecord.delta_t,
                                               biasSharing_.get(),
                                               biasInitFilename,
                                               thisRankWillDoIO),
                                          pullCoordIndex);

        biasCoupledToSystem_.back().bias_.printInitializationToLog(fplog);
    }

    /* Need to register the AWH coordinates to be allowed to apply forces to the pull coordinates. */
    registerAwhWithPull(awhParams, pull_);

    if (biasSharing_ && MASTER(commRecord_))
    {
        std::vector<size_t> pointSize;
        for (auto const& biasCts : biasCoupledToSystem_)
        {
            pointSize.push_back(biasCts.bias_.state().points().size());
        }
        /* Ensure that the shared biased are compatible between simulations */
        biasesAreCompatibleForSharingBetweenSimulations(awhParams, pointSize, *biasSharing_);
    }
}

Awh::~Awh() = default;

bool Awh::isOutputStep(int64_t step) const
{
    return (nstout_ > 0 && step % nstout_ == 0);
}

real Awh::applyBiasForcesAndUpdateBias(PbcType                pbcType,
                                       ArrayRef<const real>   masses,
                                       ArrayRef<const double> neighborLambdaEnergies,
                                       ArrayRef<const double> neighborLambdaDhdl,
                                       const matrix           box,
                                       gmx::ForceWithVirial*  forceWithVirial,
                                       double                 t,
                                       int64_t                step,
                                       gmx_wallcycle*         wallcycle,
                                       FILE*                  fplog)
{
    if (anyDimUsesPull(biasCoupledToSystem_))
    {
        GMX_ASSERT(forceWithVirial, "Need a valid ForceWithVirial object");
    }

    wallcycle_start(wallcycle, WallCycleCounter::Awh);

    t_pbc pbc;
    set_pbc(&pbc, pbcType, box);

    /* During the AWH update the potential can instantaneously jump due to either
       an bias update or moving the umbrella. The jumps are kept track of and
       subtracted from the potential in order to get a useful conserved energy quantity. */
    double awhPotential = potentialOffset_;

    for (auto& biasCts : biasCoupledToSystem_)
    {
        /* Update the AWH coordinate values with those of the corresponding
         * pull coordinates.
         */
        awh_dvec coordValue           = { 0, 0, 0, 0 };
        int      numLambdaDimsCounted = 0;
        for (int d = 0; d < biasCts.bias_.ndim(); d++)
        {
            if (biasCts.bias_.dimParams()[d].isPullDimension())
            {
                coordValue[d] = get_pull_coord_value(
                        pull_, biasCts.pullCoordIndex_[d - numLambdaDimsCounted], pbc);
            }
            else
            {
                coordValue[d] = fepLambdaState_;
                numLambdaDimsCounted += 1;
            }
        }

        /* Perform an AWH biasing step: this means, at regular intervals,
         * sampling observables based on the input pull coordinate value,
         * setting the bias force and/or updating the AWH bias state.
         */
        double biasPotential;
        double biasPotentialJump;
        /* Note: In the near future this call will be split in calls
         *       to supports bias sharing within a single simulation.
         */

        hipRangePush("calcForceAndUpdateBias");
        gmx::ArrayRef<const double> biasForce =
                biasCts.bias_.calcForceAndUpdateBias(coordValue,
                                                     neighborLambdaEnergies,
                                                     neighborLambdaDhdl,
                                                     &biasPotential,
                                                     &biasPotentialJump,
                                                     t,
                                                     step,
                                                     seed_,
                                                     fplog);
        hipRangePop();

        awhPotential += biasPotential;

        /* Keep track of the total potential shift needed to remove the potential jumps. */
        potentialOffset_ -= biasPotentialJump;

        /* Communicate the bias force to the pull struct.
         * The bias potential is returned at the end of this function,
         * so that it can be added externally to the correct energy data block.
         */
        numLambdaDimsCounted = 0;
        hipRangePush("apply_external_pull_coord_force");
        for (int d = 0; d < biasCts.bias_.ndim(); d++)
        {
            if (biasCts.bias_.dimParams()[d].isPullDimension())
            {
                
                apply_external_pull_coord_force(pull_,
                                                biasCts.pullCoordIndex_[d - numLambdaDimsCounted],
                                                biasForce[d],
                                                masses,
                                                forceWithVirial);
            }
            else
            {
                int umbrellaGridpointIndex = biasCts.bias_.state().coordState().umbrellaGridpoint();
                fepLambdaState_ = biasCts.bias_.getGridCoordValue(umbrellaGridpointIndex)[d];
                numLambdaDimsCounted += 1;
            }
        }
        hipRangePop();

        if (isOutputStep(step))
        {
            /* We might have skipped updates for part of the grid points.
             * Ensure all points are updated before writing out their data.
             */
            biasCts.bias_.doSkippedUpdatesForAllPoints();
        }
    }

    wallcycle_stop(wallcycle, WallCycleCounter::Awh);

    return MASTER(commRecord_) ? static_cast<real>(awhPotential) : 0;
}

std::shared_ptr<AwhHistory> Awh::initHistoryFromState() const
{
    if (MASTER(commRecord_))
    {
        std::shared_ptr<AwhHistory> awhHistory(new AwhHistory);
        awhHistory->bias.clear();
        awhHistory->bias.resize(biasCoupledToSystem_.size());

        for (size_t k = 0; k < awhHistory->bias.size(); k++)
        {
            biasCoupledToSystem_[k].bias_.initHistoryFromState(&awhHistory->bias[k]);
        }

        return awhHistory;
    }
    else
    {
        /* Return an empty pointer */
        return std::shared_ptr<AwhHistory>();
    }
}

void Awh::restoreStateFromHistory(const AwhHistory* awhHistory)
{
    /* Restore the history to the current state */
    if (MASTER(commRecord_))
    {
        GMX_RELEASE_ASSERT(awhHistory != nullptr,
                           "The master rank should have a valid awhHistory when restoring the "
                           "state from history.");

        if (awhHistory->bias.size() != biasCoupledToSystem_.size())
        {
            GMX_THROW(InvalidInputError(
                    "AWH state and history contain different numbers of biases. Likely you "
                    "provided a checkpoint from a different simulation."));
        }

        potentialOffset_ = awhHistory->potentialOffset;
    }
    if (PAR(commRecord_))
    {
        gmx_bcast(sizeof(potentialOffset_), &potentialOffset_, commRecord_->mpi_comm_mygroup);
    }

    for (size_t k = 0; k < biasCoupledToSystem_.size(); k++)
    {
        biasCoupledToSystem_[k].bias_.restoreStateFromHistory(
                awhHistory ? &awhHistory->bias[k] : nullptr, commRecord_);
    }
}

void Awh::updateHistory(AwhHistory* awhHistory) const
{
    if (!MASTER(commRecord_))
    {
        return;
    }

    /* This assert will also catch a non-master rank calling this function. */
    GMX_RELEASE_ASSERT(awhHistory->bias.size() == biasCoupledToSystem_.size(),
                       "AWH state and history bias count should match");

    awhHistory->potentialOffset = potentialOffset_;

    for (size_t k = 0; k < awhHistory->bias.size(); k++)
    {
        biasCoupledToSystem_[k].bias_.updateHistory(&awhHistory->bias[k]);
    }
}

const char* Awh::externalPotentialString()
{
    return "AWH";
}

void Awh::registerAwhWithPull(const AwhParams& awhParams, pull_t* pull_work)
{
    GMX_RELEASE_ASSERT(!anyDimUsesProvider(awhParams, AwhCoordinateProviderType::Pull) || pull_work,
                       "Need a valid pull object");

    for (const auto& biasParam : awhParams.awhBiasParams())
    {
        for (const auto& dimParam : biasParam.dimParams())
        {
            if (dimParam.coordinateProvider() == AwhCoordinateProviderType::Pull)
            {
                register_external_pull_potential(
                        pull_work, dimParam.coordinateIndex(), Awh::externalPotentialString());
            }
        }
    }
}

/* Fill the AWH data block of an energy frame with data (if there is any). */
void Awh::writeToEnergyFrame(int64_t step, t_enxframe* frame) const
{
    GMX_ASSERT(MASTER(commRecord_), "writeToEnergyFrame should only be called on the master rank");
    GMX_ASSERT(frame != nullptr, "Need a valid energy frame");

    if (!isOutputStep(step))
    {
        /* This is not an AWH output step, don't write any AWH data */
        return;
    }

    /* Get the total number of energy subblocks that AWH needs */
    int numSubblocks = 0;
    for (const auto& biasCoupledToSystem : biasCoupledToSystem_)
    {
        numSubblocks += biasCoupledToSystem.bias_.numEnergySubblocksToWrite();
    }
    GMX_ASSERT(numSubblocks > 0, "We should always have data to write");

    /* Add 1 energy block */
    add_blocks_enxframe(frame, frame->nblock + 1);

    /* Take the block that was just added and set the number of subblocks. */
    t_enxblock* awhEnergyBlock = &(frame->block[frame->nblock - 1]);
    add_subblocks_enxblock(awhEnergyBlock, numSubblocks);

    /* Claim it as an AWH block. */
    awhEnergyBlock->id = enxAWH;

    /* Transfer AWH data blocks to energy sub blocks */
    int energySubblockCount = 0;
    for (const auto& biasCoupledToSystem : biasCoupledToSystem_)
    {
        energySubblockCount += biasCoupledToSystem.bias_.writeToEnergySubblocks(
                &(awhEnergyBlock->sub[energySubblockCount]));
    }
}

bool Awh::hasFepLambdaDimension() const
{
    return std::any_of(
            std::begin(biasCoupledToSystem_),
            std::end(biasCoupledToSystem_),
            [](const auto& coupledBias) { return coupledBias.bias_.hasFepLambdaDimension(); });
}

bool Awh::needForeignEnergyDifferences(const int64_t step) const
{
    /* If there is no FEP lambda dimension at all in any bias there will be no need for foreign
     * energy differences */
    if (!hasFepLambdaDimension())
    {
        return false;
    }
    if (step == 0)
    {
        return true;
    }
    /* Check whether the bias(es) that has/have a FEP lambda dimension should sample coordinates
     * this step. Since the biases may have different sampleCoordStep it is necessary to check
     * this combination. */
    return std::any_of(biasCoupledToSystem_.begin(), biasCoupledToSystem_.end(), [step](const auto& biasCts) {
        return biasCts.bias_.hasFepLambdaDimension() && biasCts.bias_.isSampleCoordStep(step);
    });
}

std::unique_ptr<Awh> prepareAwhModule(FILE*                 fplog,
                                      const t_inputrec&     inputRecord,
                                      t_state*              stateGlobal,
                                      const t_commrec*      commRecord,
                                      const gmx_multisim_t* multiSimRecord,
                                      const bool            startingFromCheckpoint,
                                      const bool            usingShellParticles,
                                      const std::string&    biasInitFilename,
                                      pull_t*               pull_work)
{
    if (!inputRecord.bDoAwh)
    {
        return nullptr;
    }
    if (usingShellParticles)
    {
        GMX_THROW(InvalidInputError("AWH biasing does not support shell particles."));
    }

    auto awh = std::make_unique<Awh>(fplog,
                                     inputRecord,
                                     commRecord,
                                     multiSimRecord,
                                     *inputRecord.awhParams,
                                     biasInitFilename,
                                     pull_work,
                                     inputRecord.fepvals->n_lambda,
                                     inputRecord.fepvals->init_fep_state);

    if (startingFromCheckpoint)
    {
        // Restore the AWH history read from checkpoint
        awh->restoreStateFromHistory(MASTER(commRecord) ? stateGlobal->awhHistory.get() : nullptr);
    }
    else if (MASTER(commRecord))
    {
        // Initialize the AWH history here
        stateGlobal->awhHistory = awh->initHistoryFromState();
    }
    return awh;
}

} // namespace gmx
