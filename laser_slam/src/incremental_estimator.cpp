#include "laser_slam/incremental_estimator.hpp"

#include <gtsam/nonlinear/GaussNewtonOptimizer.h>

using namespace gtsam;

namespace laser_slam {

IncrementalEstimator::IncrementalEstimator(const EstimatorParams& parameters,
                                           unsigned int n_laser_slam_workers) : params_(
                                               parameters),
                                               n_laser_slam_workers_(n_laser_slam_workers) {
  // Create the iSAM2 object.
  ISAM2Params isam2_params;
  isam2_params.setRelinearizeSkip(1);
  isam2_params.setRelinearizeThreshold(0.001);
  isam2_ = ISAM2(isam2_params);

  // Create the laser tracks.
  for (size_t i = 0u; i < n_laser_slam_workers_; ++i) {
    std::shared_ptr<LaserTrack> laser_track(new LaserTrack(parameters.laser_track_params, i));
    laser_tracks_.push_back(std::move(laser_track));
  }

  // Create the loop closure noise model.
  using namespace gtsam::noiseModel;
  if (params_.add_m_estimator_on_loop_closures) {
    LOG(INFO) << "Creating loop closure noise model with cauchy.";
    loop_closure_noise_model_  = gtsam::noiseModel::Robust::Create(
        gtsam::noiseModel::mEstimator::Cauchy::Create(1),
        gtsam::noiseModel::Diagonal::Sigmas(params_.loop_closure_noise_model));
  } else {
    loop_closure_noise_model_ =
        gtsam::noiseModel::Diagonal::Sigmas(params_.loop_closure_noise_model);
  }

  // Load the ICP configurations for adjusting the loop closure transformations.
  // TODO now using the same configuration as for the lidar odometry.
  std::ifstream ifs_icp_configurations(params_.laser_track_params.icp_configuration_file.c_str());
  if (ifs_icp_configurations.good()) {
    LOG(INFO) << "Loading ICP configurations from: " <<
        params_.laser_track_params.icp_configuration_file;
    icp_.loadFromYaml(ifs_icp_configurations);
  } else {
    LOG(WARNING) << "Could not open ICP configuration file. Using default configuration.";
    icp_.setDefault();
  }
}

void IncrementalEstimator::processLoopClosure(const RelativePose& loop_closure) {
  std::lock_guard<std::recursive_mutex> lock(full_class_mutex_);
  CHECK_LT(loop_closure.time_a_ns, loop_closure.time_b_ns) << "Loop closure has invalid time.";
  CHECK_GE(loop_closure.time_a_ns, laser_tracks_[loop_closure.track_id_a]->getMinTime()) <<
      "Loop closure has invalid time.";
  CHECK_LE(loop_closure.time_a_ns, laser_tracks_[loop_closure.track_id_a]->getMaxTime()) <<
      "Loop closure has invalid time.";
  CHECK_GE(loop_closure.time_b_ns, laser_tracks_[loop_closure.track_id_b]->getMinTime()) <<
      "Loop closure has invalid time.";
  CHECK_LE(loop_closure.time_b_ns, laser_tracks_[loop_closure.track_id_b]->getMaxTime()) <<
      "Loop closure has invalid time.";

  // Apply an ICP step if desired.
  RelativePose updated_loop_closure = loop_closure;
  if (params_.do_icp_step_on_loop_closures) {
    // Get the initial guess.
    PointMatcher::TransformationParameters initial_guess =
        loop_closure.T_a_b.getTransformationMatrix().cast<float>();

    LOG(INFO) << "Creating the submaps for loop closure ICP.";
    Clock clock;
    DataPoints sub_map_a;
    DataPoints sub_map_b;
    laser_tracks_[loop_closure.track_id_a]->buildSubMapAroundTime(
        loop_closure.time_a_ns, params_.loop_closures_sub_maps_radius, &sub_map_a);
    laser_tracks_[loop_closure.track_id_b]->buildSubMapAroundTime(
        loop_closure.time_b_ns, params_.loop_closures_sub_maps_radius, &sub_map_b);
    clock.takeTime();
    LOG(INFO) << "Took " << clock.getRealTime() << " ms to create loop closures sub maps.";

    LOG(INFO) << "Creating loop closure ICP.";
    clock.start();
    PointMatcher::TransformationParameters icp_solution = icp_.compute(sub_map_b, sub_map_a,
                                                                       initial_guess);
    clock.takeTime();
    LOG(INFO) << "Took " << clock.getRealTime() <<
        " ms to compute the icp_solution for the loop closure.";

    updated_loop_closure.T_a_b = convertTransformationMatrixToSE3(icp_solution);
  }

  LOG(INFO) << "Creating loop closure factor.";
  NonlinearFactorGraph new_factors;
  Expression<SE3> T_w_b(laser_tracks_[loop_closure.track_id_b]->getValueExpression(
      updated_loop_closure.time_b_ns));
  Expression<SE3> T_w_a(laser_tracks_[loop_closure.track_id_a]->getValueExpression(
      updated_loop_closure.time_a_ns));
  Expression<SE3> T_a_w(kindr::minimal::inverse(T_w_a));
  Expression<SE3> relative(kindr::minimal::compose(T_a_w, T_w_b));
  ExpressionFactor<SE3> new_factor(loop_closure_noise_model_, updated_loop_closure.T_a_b,
                                   relative);
  new_factors.push_back(new_factor);

  LOG(INFO) << "Estimating the trajectories.";
  Values new_values;
  Values result = estimateAndRemove(new_factors, new_values);

  LOG(INFO) << "Updating the trajectories after LC.";
  for (auto& track: laser_tracks_) {
    track->updateFromGTSAMValues(result);
  }
  LOG(INFO) << "Updating the trajectories after LC done.";
}

Values IncrementalEstimator::estimate(const gtsam::NonlinearFactorGraph& new_factors,
                                      const gtsam::Values& new_values) {
  std::lock_guard<std::recursive_mutex> lock(full_class_mutex_);
  Clock clock;
  // Update and force relinearization.
  isam2_.update(new_factors, new_values).print();
  // TODO Investigate why these two subsequent update calls are needed.
  isam2_.update();
  isam2_.update();

  Values result(isam2_.calculateEstimate());

  clock.takeTime();
  LOG(INFO) << "Took " << clock.getRealTime() << "ms to estimate the trajectory.";
  return result;
}

Values IncrementalEstimator::estimateAndRemove(
    const gtsam::NonlinearFactorGraph& new_factors,
    const gtsam::Values& new_values) {
  std::lock_guard<std::recursive_mutex> lock(full_class_mutex_);
  Clock clock;
  // Update and force relinearization.
  std::vector<size_t> factor_indices_to_remove;
  factor_indices_to_remove.push_back(factor_indice_to_remove_);
  isam2_.update(new_factors, new_values, factor_indices_to_remove).print();
  // TODO Investigate why these two subsequent update calls are needed.
  isam2_.update();
  isam2_.update();

  Values result(isam2_.calculateEstimate());

  clock.takeTime();
  LOG(INFO) << "Took " << clock.getRealTime() << "ms to estimate the trajectory.";
  return result;
}

gtsam::Values IncrementalEstimator::registerPrior(const gtsam::NonlinearFactorGraph& new_factors,
                                                  const gtsam::Values& new_values,
                                                  const unsigned int worker_id) {

  ISAM2Result update_result = isam2_.update(new_factors, new_values);

  CHECK_EQ(update_result.newFactorsIndices.size(), 1u);
  if (worker_id == 1) {
    factor_indice_to_remove_ = update_result.newFactorsIndices.at(0u);
  }
  // TODO Investigate why these two subsequent update calls are needed.
  isam2_.update();
  isam2_.update();
  Values result(isam2_.calculateEstimate());
  return result;
}

std::shared_ptr<LaserTrack> IncrementalEstimator::getLaserTrack(unsigned int laser_track_id) {
  std::lock_guard<std::recursive_mutex> lock(full_class_mutex_);
  CHECK_GE(laser_track_id, 0u);
  CHECK_LT(laser_track_id, laser_tracks_.size());
  return laser_tracks_[laser_track_id];
}

} // namespace laser_slam
