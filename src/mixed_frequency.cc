// Copyright 2011 Google Inc. All Rights Reserved.
// Author: stevescott@google.com (Steve Scott)

#include <sstream>

#include "r_interface/check_interrupt.h"
#include "r_interface/error.h"

#include "r_interface/boom_r_tools.hpp"
#include "r_interface/create_state_model.hpp"
#include "r_interface/list_io.hpp"
#include "r_interface/print_R_timestamp.hpp"
#include "r_interface/seed_rng_from_R.hpp"
#include "r_interface/handle_exception.hpp"

#include "Models/Glm/PosteriorSamplers/BregVsSampler.hpp"
#include "Models/Glm/RegressionModel.hpp"

#include "Models/StateSpace/AggregatedStateSpaceRegression.hpp"
#include "Models/StateSpace/StateSpaceModel.hpp"
#include "Models/StateSpace/PosteriorSamplers/AggregatedStateSpacePosteriorSampler.hpp"
#include "Models/StateSpace/PosteriorSamplers/StateSpacePosteriorSampler.hpp"

#include "R_ext/Arith.h"  // for ISNA/R_IsNA

// BOOM Linear algebra
using BOOM::Vec;
using BOOM::Mat;
using BOOM::Spd;

// Misc. BOOM
using BOOM::AggregatedStateSpaceRegression;
using BOOM::StateSpaceModel;
using BOOM::FineNowcastingData;
using BOOM::Ptr;

// R interface
using BOOM::getListElement;
using BOOM::NativeVectorListElement;
using BOOM::RListIoManager;
using BOOM::StandardDeviationListElement;
using BOOM::ToBoomVector;
using BOOM::ToBoomMatrix;
using BOOM::ToBoomSpd;
using BOOM::VectorListElement;
using BOOM::GlmCoefsListElement;

using BOOM::RCheckInterrupt;
using BOOM::RErrorReporter;
using BOOM::RInterface::handle_exception;
using BOOM::RInterface::handle_unknown_exception;

namespace {

// A callback to manage recording the contributions from each state
// component.
class StateContributionCallback : public BOOM::MatrixIoCallback {
 public:
  explicit StateContributionCallback(BOOM::StateSpaceModelBase *model)
      : model_(model) {}
  virtual int nrow() const {return model_->nstate();}
  virtual int ncol() const {return model_->time_dimension();}
  virtual BOOM::Mat get_matrix()const {
    BOOM::Mat ans(nrow(), ncol());
    for (int state = 0; state < model_->nstate(); ++state) {
      ans.row(state) = model_->state_contribution(state);
    }
    return ans;
  }
 private:
  BOOM::StateSpaceModelBase *model_;
};

class StateRowCallback : public BOOM::VectorIoCallback {
 public:
  StateRowCallback(const BOOM::StateSpaceModelBase *model,
                   int row_number,
                   bool from_front)
      : model_(model), row_number_(row_number), from_front_(from_front) {}

  virtual int dim() const {
    return model_->time_dimension();
  }

  virtual Vec get_vector() const {
    if (from_front_) return model_->state().row(row_number_);
    // otherwise, row number counts from back
    int last_row = model_->state().nrow() - 1;
    return model_->state().row(last_row - row_number_);
  }

 private:
  const BOOM::StateSpaceModelBase *model_;
  int row_number_;
  bool from_front_;
};
//======================================================================
// Add a regression prior to the RegressionModel managed by model.
// Args:
//   model: The AggregatedStateSpaceRegression that needs a regression
//     prior assigned to it.
//   r_regression_prior: An R object created by SpikeSlabPrior,
//     which is part of the BoomSpikeSlab package.
//   r_truth: Either an R NULL (in which case it is ignored), or an R
//     list containing true values for some model unknnowns.  The
//     specified unknowns will remain at the given value and not be
//     sampled by the MCMC.  Potential elements of the list include
//     'state', 'beta', and 'sigma.obs'.
void AddRegressionPriorAndSetSampler(
    Ptr<AggregatedStateSpaceRegression> model,
    SEXP r_regression_prior,
    SEXP r_truth) {
  Vec prior_inclusion_probs(ToBoomVector(getListElement(
      r_regression_prior, "prior.inclusion.probabilities")));
  Vec mu(ToBoomVector(getListElement(
      r_regression_prior, "mu")));
  Spd Siginv(ToBoomSpd(getListElement(
      r_regression_prior, "siginv")));
  double prior_df = Rf_asReal(getListElement(r_regression_prior, "prior.df"));
  double prior_sigma_guess = Rf_asReal(getListElement(
      r_regression_prior, "sigma.guess"));
  int max_flips = Rf_asInteger(getListElement(r_regression_prior, "max.flips"));
  Ptr<BOOM::BregVsSampler> sampler(new BOOM::BregVsSampler(
      model->regression_model(),
      mu,
      Siginv,
      prior_sigma_guess,
      prior_df,
      prior_inclusion_probs));
  sampler->limit_model_selection(max_flips);
  model->regression_model()->set_method(sampler);

  if (!Rf_isNull(r_truth)) {
    SEXP r_beta = getListElement(r_truth, "beta");
    if (r_beta != R_NilValue) {
      Vec beta = ToBoomVector(r_beta);
      model->regression_model()->set_Beta(beta);
      sampler->supress_model_selection();
      sampler->supress_beta_draw();
    }

    SEXP r_sigma_obs = getListElement(r_truth, "sigma.obs");
    if (r_sigma_obs != R_NilValue) {
      double sigma_obs = Rf_asReal(r_sigma_obs);
      model->regression_model()->set_sigsq(sigma_obs * sigma_obs);
      sampler->supress_sigma_draw();
    }

    SEXP r_state = getListElement(r_truth, "state");
    if (r_state != R_NilValue) {
      Mat state = ToBoomMatrix(r_state);
      model->permanently_set_state(state);
    }
  }
}

//======================================================================
// Add the state models from 'state_specification' to 'model'.  Also
// set up the associated posterior samplers, set up the poseterior
// sampler for the regression component, and set up the IO manager to
// keep track of all the simulations.
// Args:
//   model: The model to which the specified components of state will
//     be added.
//   augmented_model: A StateSpaceModel with no predictor variables
//     that shares the same state as 'model'.  This is the model that
//     will be used for forecasting beyond the range of the predictor
//     variables.
//   state_specification: An R list specifying the components of state
//     to be added.
//   regression_prior: An R list created by SpikeSlabPrior,
//     containing the prior distribution for the regression component
//     of the model.
//   save_state_history: If true then MCMC simulations will record the
//     value of the state vector at the time of the final observation
//     (which is useful for forecasting later).
//   final_state: A pointer to a BOOM::Vec containing storage for the
//     state vector as of the final time point, for 'model'.
//   augmented_final_state: Final state storage for 'augmented model'.
//   r_truth: For debugging puposes onlyAn R list containing one or
//     more of the following elements.  If this list is non-NULL then
//     the named elements will remain fixed through the life of the
//     MCMC.
//     * state: A matrix giving the true state of the model (from a
//       fake-data simulation)
//     * beta:  A vector of regression coefficients.
//     * sigma.obs:  The value of sigma.obs.
// Returns:
//   An RListIoManager containing information needed to allocate space,
//     record, and stream the model parameters and related information.
RListIoManager SpecifyModel(Ptr<AggregatedStateSpaceRegression> model,
                            Ptr<StateSpaceModel> augmented_model,
                            SEXP state_specification,
                            SEXP regression_prior,
                            bool save_state_history,
                            Vec *final_state,
                            Vec *augmented_final_state,
                            SEXP r_truth) {
  RListIoManager io_manager;

  io_manager.add_list_element(
      new GlmCoefsListElement(model->regression_model()->coef_prm(),
                              "coefficients"));
  io_manager.add_list_element(
      new StandardDeviationListElement(model->regression_model()->Sigsq_prm(),
                                       "sigma.obs"));

  BOOM::RInterface::StateModelFactory factory(&io_manager, model.get());
  factory.AddState(state_specification);

  BOOM::RInterface::StateModelFactory augmented_factory(
      &io_manager, augmented_model.get());
  augmented_factory.AddState(state_specification, "augmented_");

  AddRegressionPriorAndSetSampler(model, regression_prior, r_truth);
  model->set_method(
      new BOOM::AggregatedStateSpacePosteriorSampler(model.get()));

  augmented_model->set_method(
      new BOOM::StateSpacePosteriorSampler(augmented_model.get()));

  // We need to add final.state to io_manager last,
  // because we need to know how big the state vector is.
  factory.SaveFinalState(final_state);
  augmented_factory.SaveFinalState(augmented_final_state,
                                   "augmented.final.state");

  if (save_state_history) {
    // The final NULL argument is because we won't be streaming state
    // contributions in future calculations.  They are for reporting
    // only.
    io_manager.add_list_element(
        new BOOM::NativeMatrixListElement(
            new StateContributionCallback(model.get()),
            "state.contributions",
            NULL));

    io_manager.add_list_element(
        new BOOM::NativeVectorListElement(
            new StateRowCallback(model.get(),
                                 1,
                                 false),
            "latent.fine",
            NULL));
    io_manager.add_list_element(
        new BOOM::NativeVectorListElement(
            new StateRowCallback(model.get(),
                                 0,
                                 false),
            "cumulator",
            NULL));

    io_manager.add_list_element(
        new BOOM::NativeMatrixListElement(
            new StateContributionCallback(augmented_model.get()),
            "augmented.state.contributions",
            NULL));
  }
  return io_manager;
}

//======================================================================
// Transcribes the observations sampled from 'model' into the raw data
// for 'augmented_model'.  The sampled observations are the
// next-to-last elements in the state vector for 'model'.
// Returns true on success, false on error.
void TranscribeResponseData(Ptr<AggregatedStateSpaceRegression> model,
                            Ptr<StateSpaceModel> augmented_model,
                            RErrorReporter *error_reporter) {
  const Mat &state(model->state());
  std::vector<Ptr<BOOM::DoubleData> > & data(augmented_model->dat());
  int next_to_last_row = nrow(state) - 2;
  const BOOM::ConstVectorView imputed_data(state.row(next_to_last_row));
  if (data.size() != imputed_data.size()) {
    std::ostringstream err;
    err << "Imputed data (" << imputed_data.size()
        << ") and augmented data (" << data.size()
        << ") are not the same size";
    error_reporter->SetError(err.str());
    return;
  }
  for (int i = 0; i < imputed_data.size(); ++i) {
    data[i]->set(imputed_data[i]);
  }
}

//======================================================================
// Compute the vector of training data from R's inputs.
std::vector<Ptr<FineNowcastingData> > ComputeTrainingData(
    SEXP r_target_series,
    SEXP r_predictors,
    SEXP r_which_coarse_interval,
    SEXP r_membership_fraction,
    SEXP r_ends_interval) {
  const BOOM::Vec target_series(ToBoomVector(r_target_series));
  const BOOM::Mat predictors(ToBoomMatrix(r_predictors));
  const int * which_coarse_interval(INTEGER(r_which_coarse_interval));
  const BOOM::Vec membership_fraction(ToBoomVector(r_membership_fraction));
  const int *ends_interval(LOGICAL(r_ends_interval));

  std::vector<Ptr<FineNowcastingData> > training_data;
  training_data.reserve(nrow(predictors));

  for (int i = 0; i < nrow(predictors); ++i) {
    bool coarse_observation_observed = false;
    double monthly_observation = 0;
    if (!R_IsNA(which_coarse_interval[i])) {
      // Subtract 1 to convert from R's unit-offset index to a C-style
      // zero-offset index.
      int which_month = which_coarse_interval[i] - 1;
      if ((which_month > 0)
          && (which_month < target_series.size())
          && ends_interval[i]
          && !R_IsNA(target_series[which_month])) {
        coarse_observation_observed = true;
        monthly_observation = target_series[which_month];
      }
    }
    Ptr<FineNowcastingData> data = new BOOM::FineNowcastingData(
        predictors.row(i),
        monthly_observation,
        coarse_observation_observed,
        ends_interval[i],
        membership_fraction[i]);
    training_data.push_back(data);
  }
  return training_data;
}
}  // unnamed namespace

extern "C" {
  //======================================================================
  // This is the main entry point for fitting mixed frequency time
  // series models.
  SEXP bsts_fit_mixed_frequency_model_(
      SEXP r_target_series,
      SEXP r_predictors,
      SEXP r_which_coarse_interval,
      SEXP r_membership_fraction,
      SEXP r_contains_end,
      SEXP r_state_specification,
      SEXP r_regression_prior,
      SEXP r_niter,
      SEXP r_ping,
      SEXP r_seed,
      SEXP r_truth) {
    RErrorReporter error_reporter;
    try {
      BOOM::RInterface::seed_rng_from_R(r_seed);

      std::vector<Ptr<FineNowcastingData> > data = ComputeTrainingData(
          r_target_series,
          r_predictors,
          r_which_coarse_interval,
          r_membership_fraction,
          r_contains_end);

      int xdim = data[0]->regression_data()->xdim();

      Ptr<AggregatedStateSpaceRegression> model(
          new AggregatedStateSpaceRegression(xdim));
      model->set_data(data);

      // This is the data that will be used by the secondary model.
      Vec augmented_data(data.size());
      Ptr<BOOM::StateSpaceModel> augmented_model(
          new BOOM::StateSpaceModel(augmented_data));

      RListIoManager io_manager = SpecifyModel(
          model,
          augmented_model,
          r_state_specification,  // as in bsts
          r_regression_prior,     // from SpikeSlabPrior
          true,                   // yes, save final state
          NULL,                   // NULL storage for streaming final_state
          NULL,                   // NULL storage for augmented_final_state
          r_truth);

      // Do one posterior sampling step before getting ready to write.
      // This will ensure that any dynamically allocated objects have
      // the correct size before any R memory gets allocated in the
      // call to prepare_to_write().
      model->sample_posterior();

      int niter = Rf_asInteger(r_niter);
      int ping = Rf_asInteger(r_ping);
      SEXP ans = PROTECT(io_manager.prepare_to_write(niter));

      for (int i = 0; i < niter; ++i) {
        if (RCheckInterrupt()) {
          error_reporter.SetError("Canceled by user.");
          return R_NilValue;
        }
        BOOM::print_R_timestamp(i, ping);
        try {
          model->sample_posterior();
          TranscribeResponseData(model, augmented_model, &error_reporter);
          if (error_reporter.HasError()) {
            return R_NilValue;
          } else {
            augmented_model->sample_posterior();
            io_manager.write();
          }
        } catch(std::exception &e) {
          std::ostringstream err;
          err << "mixed_frequency.cc: caught an exception with "
              << "the following error message in MCMC iteration "
              << i << "." << std::endl << e.what() << std::endl;
          error_reporter.SetError(err.str());
          return R_NilValue;
          // undetermined state.
        } catch(...) {
          std::ostringstream err;
          err << "mixed_frequency.cc: caught unknown exception at "
              << "MCMC iteration " << i << "." << std::endl;
          error_reporter.SetError(err.str());
          return R_NilValue;
        }
      }
      UNPROTECT(1);
      return ans;
    } catch(std::exception &e) {
      handle_exception(e);
    } catch(...) {
      handle_unknown_exception();
    }
    return R_NilValue;
  }
}
