/*
  This ROS node listens to data from all trackers in the system and provides
  a global solution to the tracking problem. That is, it solves for the
  relative location of the lighthouses and trackers as a function of time.
*/

// ROS includes
#include <ros/ros.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/static_transform_broadcaster.h>

// We will use OpenCV to bootstrap solution
#include <opencv2/calib3d/calib3d.hpp>

// Third-party includes
#include <geometry_msgs/TransformStamped.h>
#include <visualization_msgs/MarkerArray.h>
#include <nav_msgs/Path.h>
#include <std_srvs/Trigger.h>

// Non-standard datra messages
#include <deepdive_ros/Light.h>
#include <deepdive_ros/Lighthouses.h>
#include <deepdive_ros/Trackers.h>

// Ceres and logging
#include <ceres/ceres.h>
#include <ceres/rotation.h>

// Ceres and logging
#include <Eigen/Core>
#include <Eigen/Geometry>

// C++ libraries
#include <map>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>

// Shared local code
#include "deepdive.hh"

// GLOBAL PARAMETERS

// List of lighthouses
TrackerMap trackers_;
LighthouseMap lighthouses_;
MeasurementMap measurements_;

// Global strings
std::string calfile_ = "deepdive.tf2";
std::string frame_world_ = "world";
std::string frame_vive_ = "vive";
std::string frame_body_ = "truth";

// Whether to apply corrections
bool correct_ = false;

// What to solve for
bool refine_sensors_ = false;
bool refine_params_ = false;

// Rejection thresholds
int thresh_count_ = 4;
double thresh_angle_ = 60.0;
double thresh_duration_ = 1.0;

// Graph resolution
double res_;

// What to weight motion cost relative to the other errors
double weight_light_ = 1e-6;
double weight_motion_ = 1.0;

// Solver parameters
ceres::Solver::Options options_;

// Are we running in "offline" mode
bool offline_ = false;

// Should we publish rviz markers
bool visualize_ = true;

// Are we recording right now?
bool recording_ = false;

// World -> vive registatration
double registration_[6];

// Sensor visualization publisher
std::map<std::string, ros::Publisher> pub_sensors_;
std::map<std::string, std::map<std::string, ros::Publisher>> pub_path_;

// Timer for managing offline
ros::Timer timer_;

struct TransformCost {
  explicit TransformCost() {}
  template <typename T>
  bool operator()(const T* const mTs,         // Slave -> master transform
                  const T* const mTt,         // Tracker -> master transform
                  const T* const sTt,         // Tracker -> slave transform
                  T* residual) const {
    // Rotational component
    //  err = mRt * (mRs * sRt)^-1
    T q[4], mRs[4], sRt[4], mRt[4], mRt_inv[4];
    ceres::AngleAxisToQuaternion(&mTt[3], mRt);
    ceres::AngleAxisToQuaternion(&mTs[3], mRs);
    ceres::AngleAxisToQuaternion(&sTt[3], sRt);
    ceres::QuaternionProduct(mRs, sRt, mRt_inv);
    mRt_inv[1] = -mRt_inv[1];
    mRt_inv[2] = -mRt_inv[2];
    mRt_inv[3] = -mRt_inv[3];
    ceres::QuaternionProduct(mRt, mRt_inv, q);
    ceres::QuaternionToAngleAxis(q, &residual[3]);
    // Translational component
    for (size_t i = 0; i < 3; i++)
      residual[i] = mTt[i] - (mTs[i] + sTt[i]);
    return true;
  }
};

// Get the average of a vector of doubles
bool Mean(std::vector<double> const& v, double & d) {
  if (v.empty()) return false;
  d = std::accumulate(v.begin(), v.end(), 0.0) / v.size(); 
  return true;
}

// Jointly solve
bool Solve() {
  // Check that we have enough measurements
  if (measurements_.empty()) {
    ROS_WARN("Insufficient measurements received, so cannot solve problem.");
    return false;
  } else {
    double t = (measurements_.rbegin()->first
      - measurements_.begin()->first).toSec();
    ROS_INFO_STREAM("Processing " << measurements_.size()
      << " measurements running for " << t << " seconds from "
      << measurements_.begin()->first << " to "
      << measurements_.rbegin()->first);
  }

  // Keep a log of the data used
  std::map<std::string, size_t> l_tally;
  std::map<std::string, size_t> t_tally;
  
  // Data storage for next step

  typedef std::map<ros::Time,             // Time
            std::map<uint8_t,             // Sensor
              std::map<uint8_t,           // Axis
                std::vector<double>       // Sample
              >
            >
          > Bundle;
  std::map<std::string,               // Tracker
    std::map<std::string,             // Lighthouse
      Bundle
    >
  > bundle;

  // We bundle measurements into into bins of width "resolution". This allows
  // us to take the average of the measurements to improve accuracy
  {
    ROS_INFO("Bundling measurements into larger discrete time units.");
    MeasurementMap::iterator mt;
    for (mt = measurements_.begin(); mt != measurements_.end(); mt++) {
      std::string const& tserial = mt->second.light.header.frame_id;
      std::string const& lserial = mt->second.light.lighthouse;
      size_t const& a = mt->second.light.axis;
      ros::Time t = ros::Time(round(mt->first.toSec() / res_) * res_);
      std::vector<deepdive_ros::Pulse>::iterator pt;
      for (pt = mt->second.light.pulses.begin(); pt != mt->second.light.pulses.end(); pt++)
        bundle[tserial][lserial][t][pt->sensor][a].push_back(pt->angle);
    }
  }

  // Data storage for next step

  std::map<std::string,               // Tracker
    std::map<ros::Time,               // Time
      std::map<std::string,           // Lighthouse
        double[6]                     // Transform
      >
    >
  > poses;

  // We are going to estimate the pose of each slave lighthouse in the frame
  // of the master lighthouse (vive frame) using PNP. We can think of the
  // two lighthouses as a stereo pair that are looking at a set of corresp-
  // ondences (photosensors). We want to calibrate this stereo pair.
  {
    ROS_INFO("Using P3P to estimate pose sequence in every lighthouse frame.");
    double fov = 2.0944;                          // 120deg FOV
    double w = 1.0;                               // 1m synthetic image plane
    double z = w / (2.0 * std::tan(fov / 2.0));   // Principle distance
    uint32_t count = 0;                           // Track num transforms
    // Iterate over lighthouses
    LighthouseMap::iterator lt;
    for (lt = lighthouses_.begin(); lt != lighthouses_.end(); lt++) {
      // Iterate over trackers
      TrackerMap::iterator tt;
      for (tt = trackers_.begin(); tt != trackers_.end(); tt++) {
        ROS_INFO_STREAM("- Slave " << lt->first << " and tracker " << tt->first);
        tt->second.vTt.clear();
        // Iterate over time epochs
        Bundle::iterator bt;
        for (bt = bundle[tt->first][lt->first].begin();
          bt != bundle[tt->first][lt->first].end(); bt++) {
          // One for each time instance
          std::vector<cv::Point3f> obj;
          std::vector<cv::Point2f> img;
          // Try and find correspondences for every possible sensor
          for (uint8_t s = 0; s < NUM_SENSORS; s++) {
            // Mean angles for the <lighthouse, axis>
            double angles[2];
            // Check that we have azimuth/elevation for both lighthouses
            if (!Mean(bundle[tt->first][lt->first][bt->first][s][0], angles[0]) ||
                !Mean(bundle[tt->first][lt->first][bt->first][s][1], angles[1]))
              continue;
            // Correct the angles using the lighthouse parameters
            if (correct_) { 
              Lighthouse const& lh = lt->second;
              for (uint8_t a = 0; a < 2; a++) {
                angles[a] -= lh.params[a][PARAM_PHASE];
                angles[a] -= lh.params[a][PARAM_TILT] * angles[1-a];
                angles[a] -= lh.params[a][PARAM_CURVE] * angles[1-a] * angles[1-a];
                angles[a] -= lh.params[a][PARAM_GIB_MAG] 
                  * std::cos(angles[1-a] + lh.params[a][PARAM_GIB_PHASE]);
              }
            }
            // Push on the correct world sensor position
            obj.push_back(cv::Point3f(
              trackers_[tt->first].sensors[s * 6 + 0],
              trackers_[tt->first].sensors[s * 6 + 1],
              trackers_[tt->first].sensors[s * 6 + 2]));
            // Push on the coordinate in the slave image plane
            img.push_back(cv::Point2f(z * tan(angles[0]), z * tan(angles[1])));
            // ROS_INFO_STREAM("-- Sensor " << static_cast<int>(s) << " at " <<
            //  (bt->first - bundle[tt->first][lt->first].begin()->first).toSec());
          }
          // In the case that we have 4 or more measurements, then we can try
          // and estimate the trackers location in the lighthouse frame.
          if (obj.size() > 3) {
            cv::Mat cam = cv::Mat::eye(3, 3, cv::DataType<double>::type);
            cv::Mat dist;
            cam.at<double>(0, 0) = z;
            cam.at<double>(1, 1) = z;
            cv::Mat R(3, 1, cv::DataType<double>::type);
            cv::Mat T(3, 1, cv::DataType<double>::type);
            cv::Mat C(3, 3, cv::DataType<double>::type);
            if (cv::solvePnP(obj, img, cam, dist, R, T, false, cv::SOLVEPNP_EPNP)) {
              cv::Rodrigues(R, C);
              // double dist = std::sqrt(T.at<double>(0, 0) * T.at<double>(0, 0)
              //  + T.at<double>(1, 0) * T.at<double>(1, 0)
              //  + T.at<double>(2, 0) * T.at<double>(2, 0));
              // ROS_INFO_STREAM("- DIF " << dist);
              // ROS_INFO_STREAM("- TRA " << T);
              // ROS_INFO_STREAM("- ROT " << C);
              Eigen::Matrix3d rot;
              for (size_t r = 0; r < 3; r++)
                for (size_t c = 0; c < 3; c++)
                  rot(r, c) = C.at<double>(r, c);
              Eigen::AngleAxisd aa(rot);
              poses[tt->first][bt->first][lt->first][0] = T.at<double>(0, 0);
              poses[tt->first][bt->first][lt->first][1] = T.at<double>(1, 0);
              poses[tt->first][bt->first][lt->first][2] = T.at<double>(2, 0);
              poses[tt->first][bt->first][lt->first][3] = aa.angle() * aa.axis()[0];
              poses[tt->first][bt->first][lt->first][4] = aa.angle() * aa.axis()[1];
              poses[tt->first][bt->first][lt->first][5] = aa.angle() * aa.axis()[2];
              count++;
            }
          }
        }
      }
    }
    ROS_INFO_STREAM("Using " << count << " PNP solutions");
  }
  // We now have a separate pose sequence for each tracker in each lighthouse
  // frame. We now need to find the transform from the slave to master light-
  // house in a way that projects one pose sequence into the other.
  {
    ROS_INFO("Estimating master -> slave lighthouse transforms.");
    // Define a NLS problem
    ceres::Problem problem;
    ceres::Solver::Summary summary;
    // Add residual blocks to the problem
    LighthouseMap::iterator lm = lighthouses_.begin();  // First is master
    LighthouseMap::iterator lt;                         //
    for (lt = lighthouses_.begin(); lt != lighthouses_.end(); lt++) {
      // Special case for master lighthouse
      if (lt == lighthouses_.begin()) {
        for (size_t i = 0; i < 6; i++)
          lt->second.vTl[0] = 0;
        continue;
      }
      // Only get here for slave lighthouses
      TrackerMap::iterator tt;
      for (tt = trackers_.begin(); tt != trackers_.end(); tt++) {
        std::map<ros::Time, std::map<std::string, double[6]>>::iterator pt;
        for (pt = poses[tt->first].begin(); pt != poses[tt->first].end(); pt++) {
          // We must have a pose for both the master AND the slave
          if (pt->second.find(lt->first) == pt->second.end() ||
              pt->second.find(lm->first) == pt->second.end()) continue;
          // If we get here, then we have a correspondence
          ceres::CostFunction* cost = new ceres::AutoDiffCostFunction<
            TransformCost, 6, 6, 6, 6>(new TransformCost());
          // Add a residual block to represent this measurement
          problem.AddResidualBlock(cost, new ceres::HuberLoss(1.0),
            reinterpret_cast<double*>(lt->second.vTl),
            reinterpret_cast<double*>(pt->second[lm->first]),
            reinterpret_cast<double*>(pt->second[lt->first]));
          // Make sure we mark the poses as constant
          problem.SetParameterBlockConstant(pt->second[lm->first]);
          problem.SetParameterBlockConstant(pt->second[lt->first]);
        }
      }
    }
    // Solve the problem
    ceres::Solve(options_, &problem, &summary);
    if (summary.IsSolutionUsable()) {
      ROS_INFO("- Solution found");
      LighthouseMap::iterator lt;
      for (lt = lighthouses_.begin(); lt != lighthouses_.end(); lt++) {
        double d = std::sqrt(
          lt->second.vTl[0]  * lt->second.vTl[0] +
          lt->second.vTl[1]  * lt->second.vTl[1] +
          lt->second.vTl[2]  * lt->second.vTl[2]);
        ROS_INFO_STREAM(lt->first << ": " << lt->second.vTl[0] << " " <<
          lt->second.vTl[1] << " " << lt->second.vTl[2] << " (" << d << "m)");
      }
    }
    else {
      ROS_INFO("- Solution not found");
    }
  }
  // We now have the correct sens
  {
    SendTransforms(frame_world_, frame_vive_, frame_body_,
      registration_, lighthouses_, trackers_);
    // Write the solution to a config file
    if (WriteConfig(calfile_, frame_world_, frame_vive_, frame_body_,
      registration_, lighthouses_, trackers_))
      ROS_INFO_STREAM("Calibration written to " << calfile_);
    else
      ROS_INFO_STREAM("Could not write calibration to" << calfile_);
    // Print the trajectory of the body-frame in the world-frame
    if (visualize_) {
      LighthouseMap::iterator lt;
      for (lt = lighthouses_.begin(); lt != lighthouses_.end(); lt++) {
        // Get the lighthouse -> world transform
        Eigen::Vector3d v(
          lt->second.vTl[3], lt->second.vTl[4], lt->second.vTl[5]);
        Eigen::AngleAxisd aa;
        if (v.norm() > 0) {
          aa.angle() = v.norm();
          aa.axis() = v.normalized();
        }
        Eigen::Affine3d vTl = Eigen::Affine3d::Identity();
        vTl.translation()[0] = lt->second.vTl[0];
        vTl.translation()[1] = lt->second.vTl[1];
        vTl.translation()[2] = lt->second.vTl[2];
        vTl.linear() = aa.toRotationMatrix();
        // Now project the trajectory
        TrackerMap::iterator jt;
        for (jt = trackers_.begin(); jt != trackers_.end(); jt++) {
          // Iterate over timestamp
          std::map<ros::Time, std::map<std::string, double[6]>>::iterator pt;
          for (pt = poses[tt->first].begin(); pt != poses[tt->first].end(); pt++) {
            // We must have a pose for both the master AND the slave
            if (pt->second.find(lt->first) == pt->second.end())
              continue;
            Eigen::Vector3d v(it->second[0], it->second[1], it->second[2]);
            v = vTl * v;
            // PUblish pose
            geometry_msgs::PoseStamped ps;
            ps.header.stamp = it->first;
            ps.header.frame_id = frame_vive_;
            ps.pose.position.x = v[0];
            ps.pose.position.y = v[1];
            ps.pose.position.z = v[2];
            ps.pose.orientation.w = 1.0;
            ps.pose.orientation.x = 0.0;
            ps.pose.orientation.y = 0.0;
            ps.pose.orientation.z = 0.0;
            msg.poses.push_back(ps);
          }
          pub_path_[lt->first][jt->first].publish(msg);
        }
      }
    }
  }
  return true;
}

// MESSAGE CALLBACKS

void LightCallback(deepdive_ros::Light::ConstPtr const& msg) {
  // Reset the timer use din offline mode to determine the end of experiment
  timer_.stop();
  timer_.start();
  // Check that we are recording and that the tracker/lighthouse is ready
  if (!recording_ ||
    trackers_.find(msg->header.frame_id) == trackers_.end() ||
    lighthouses_.find(msg->lighthouse) == lighthouses_.end() ||
    !trackers_[msg->header.frame_id].ready ||
    !lighthouses_[msg->lighthouse].ready) return;
  // Copy over the data
  size_t deleted = 0;
  deepdive_ros::Light data = *msg;
  std::vector<deepdive_ros::Pulse>::iterator it = data.pulses.end();
  while (it-- > data.pulses.begin()) {
    // std::cout << it->duration << std::endl;
    if (it->angle > thresh_angle_ / 57.2958 ||     // Check angle
        it->duration < thresh_duration_ / 1e6) {  // Check duration
      it = data.pulses.erase(it);
      deleted++;
    }
  }
  if (data.pulses.size() < thresh_count_)
    return; 
  // Add the data
  measurements_[ros::Time::now()].light = data;
}

bool TriggerCallback(std_srvs::Trigger::Request  &req,
                     std_srvs::Trigger::Response &res)
{
  if (!recording_) {
    res.success = true;
    res.message = "Recording started.";
  }
  if (recording_) {
    // Solve the problem
    res.success = Solve();
    if (res.success)
      res.message = "Recording stopped. Solution found.";
    else
      res.message = "Recording stopped. Solution not found.";
    // Clear all the data and corrections
    measurements_.clear();
  }
  // Toggle recording state
  recording_ = !recording_;
  // Success
  return true;
}

// Fake a trigger when the timer expires
void TimerCallback(ros::TimerEvent const& event) {
  std_srvs::Trigger::Request req;
  std_srvs::Trigger::Response res;
  TriggerCallback(req, res);
}

// Called when a new lighthouse appears
void NewLighthouseCallback(LighthouseMap::iterator lighthouse) {
  ROS_INFO_STREAM("Found lighthouse " << lighthouse->first);
}

// Called when a new tracker appears
void NewTrackerCallback(TrackerMap::iterator tracker) {
  ROS_INFO_STREAM("Found tracker " << tracker->first);
  if (visualize_) {
    visualization_msgs::MarkerArray msg;
    std::map<std::string, Tracker>::iterator jt;
    for (jt = trackers_.begin(); jt != trackers_.end(); jt++)  {
      for (uint16_t i = 0; i < NUM_SENSORS; i++) {
        // All this code just to convert a normal to a quaternion
        Eigen::Vector3d vfwd(jt->second.sensors[6*i+3],
          jt->second.sensors[6*i+4], jt->second.sensors[6*i+5]);
        if (vfwd.norm() > 0) {
          Eigen::Vector3d vdown(0.0, 0.0, 1.0);
          Eigen::Vector3d vright = vdown.cross(vfwd);
          vfwd = vfwd.normalized();
          vright = vright.normalized();
          vdown = vdown.normalized();
          Eigen::Matrix3d dcm;
          dcm << vfwd.x(), vright.x(), vdown.x(),
                 vfwd.y(), vright.y(), vdown.y(),
                 vfwd.z(), vright.z(), vdown.z();
          Eigen::Quaterniond q(dcm);
          // Now plot an arrow representing the normal
          visualization_msgs::Marker marker;
          marker.header.frame_id = jt->first + "/light";
          marker.header.stamp = ros::Time::now();
          marker.ns = jt->first;
          marker.id = i;
          marker.type = visualization_msgs::Marker::ARROW;
          marker.action = visualization_msgs::Marker::ADD;
          marker.pose.position.x = jt->second.sensors[6*i+0];
          marker.pose.position.y = jt->second.sensors[6*i+1];
          marker.pose.position.z = jt->second.sensors[6*i+2];
          marker.pose.orientation.w = q.w();
          marker.pose.orientation.x = q.x();
          marker.pose.orientation.y = q.y();
          marker.pose.orientation.z = q.z();
          marker.scale.x = 0.010;
          marker.scale.y = 0.001;
          marker.scale.z = 0.001;
          marker.color.a = 1.0;
          marker.color.r = 1.0;
          marker.color.g = 0.0;
          marker.color.b = 0.0;
          msg.markers.push_back(marker);
        }
      }
      pub_sensors_[tracker->first].publish(msg);
    }
  }
}

// MAIN ENTRY POINT

int main(int argc, char **argv) {
  // Initialize ROS and create node handle
  ros::init(argc, argv, "deepdive_calibration");
  ros::NodeHandle nh("~");

  // If we are in offline mode when we will replay the data back at 10x the
  // speed, using it all to find a calibration solution for both the body
  // as a function of  time and the lighthouse positions.
  if (!nh.getParam("offline", offline_))
    ROS_FATAL("Failed to get if we are running in offline mode.");
  if (offline_) {
    ROS_INFO("We are in offline mode. Speeding up bag replay by 10x");
    recording_ = true;
  }

  // Reset the registration information
  for (size_t i = 0; i < 6; i++)
    registration_[i] = 0;

  // Get the parent information
  if (!nh.getParam("calfile", calfile_))
    ROS_FATAL("Failed to get the calfile file.");

  // Get some global information
  if (!nh.getParam("frames/world", frame_world_))
    ROS_FATAL("Failed to get frames/world parameter.");  
  if (!nh.getParam("frames/vive", frame_vive_))
    ROS_FATAL("Failed to get frames/vive parameter.");  
  if (!nh.getParam("frames/body", frame_body_))
    ROS_FATAL("Failed to get frames/body parameter.");  

  // Get the thresholds
  if (!nh.getParam("thresholds/count", thresh_count_))
    ROS_FATAL("Failed to get threshods/count parameter.");
  if (!nh.getParam("thresholds/angle", thresh_angle_))
    ROS_FATAL("Failed to get thresholds/angle parameter.");
  if (!nh.getParam("thresholds/duration", thresh_duration_))
    ROS_FATAL("Failed to get thresholds/duration parameter.");

  // What to refine
  if (!nh.getParam("refine/sensors", refine_sensors_))
    ROS_FATAL("Failed to get refine/sensors parameter.");
  if (!nh.getParam("refine/params", refine_params_))
    ROS_FATAL("Failed to get refine/params parameter.");

  // Tracking resolution
  if (!nh.getParam("resolution", res_))
    ROS_FATAL("Failed to get resolution parameter.");

  // What weights to use
  if (!nh.getParam("weight/light", weight_light_))
    ROS_FATAL("Failed to get weight/light parameter.");
  if (!nh.getParam("weight/motion", weight_motion_))
    ROS_FATAL("Failed to get weight/motion parameter.");

  // Whether to apply light corrections
  if (!nh.getParam("correct", correct_))
    ROS_FATAL("Failed to get correct parameter.");
  if (!correct_)
    refine_params_ = false;

  // Define the ceres problem
  options_.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
  if (!nh.getParam("solver/max_time", options_.max_solver_time_in_seconds))
    ROS_FATAL("Failed to get the solver/max_time parameter.");
  if (!nh.getParam("solver/max_iterations", options_.max_num_iterations))
    ROS_FATAL("Failed to get the solver/max_iterations parameter.");
  if (!nh.getParam("solver/threads", options_.num_threads))
    ROS_FATAL("Failed to get the solver/threads parameter.");
  if (!nh.getParam("solver/debug", options_.minimizer_progress_to_stdout))
    ROS_FATAL("Failed to get the solver/debug parameter.");

  // Visualization option
  if (!nh.getParam("visualize", visualize_))
    ROS_FATAL("Failed to get the visualize parameter.");

  // Get the parent information
  std::vector<std::string> lighthouses;
  if (!nh.getParam("lighthouses", lighthouses))
    ROS_FATAL("Failed to get the lighthouse list.");
  std::vector<std::string>::iterator it;
  for (it = lighthouses.begin(); it != lighthouses.end(); it++) {
    std::string serial;
    if (!nh.getParam(*it + "/serial", serial))
      ROS_FATAL("Failed to get the lighthouse serial.");
    std::vector<double> transform;
    if (!nh.getParam(*it + "/transform", transform))
      ROS_FATAL("Failed to get the lighthouse transform.");
    if (transform.size() != 7) {
      ROS_FATAL("Failed to parse lighthouse transform.");
      continue;
    }
    Eigen::Quaterniond q(transform[6], transform[3], transform[4], transform[5]);
    Eigen::AngleAxisd aa(q);
    lighthouses_[serial].vTl[0] = transform[0];
    lighthouses_[serial].vTl[1] = transform[1];
    lighthouses_[serial].vTl[2] = transform[2];
    lighthouses_[serial].vTl[3] = aa.angle() * aa.axis()[0];
    lighthouses_[serial].vTl[4] = aa.angle() * aa.axis()[1];
    lighthouses_[serial].vTl[5] = aa.angle() * aa.axis()[2];
    lighthouses_[serial].ready = false;
  }

  // Get the parent information
  std::vector<std::string> trackers;
  if (!nh.getParam("trackers", trackers))
    ROS_FATAL("Failed to get the tracker list.");
  std::vector<std::string>::iterator jt;
  for (jt = trackers.begin(); jt != trackers.end(); jt++) {
    std::string serial;
    if (!nh.getParam(*jt + "/serial", serial))
      ROS_FATAL("Failed to get the tracker serial.");
    std::vector<double> transform;
    if (!nh.getParam(*jt + "/transform", transform))
      ROS_FATAL("Failed to get the tracker transform.");
    if (transform.size() != 7) {
      ROS_FATAL("Failed to parse tracker transform.");
      continue;
    }
    Eigen::Quaterniond q(
      transform[6],  // qw
      transform[3],  // qx
      transform[4],  // qy
      transform[5]); // qz
    Eigen::AngleAxisd aa(q);
    trackers_[serial].bTh[0] = transform[0];
    trackers_[serial].bTh[1] = transform[1];
    trackers_[serial].bTh[2] = transform[2];
    trackers_[serial].bTh[3] = aa.angle() * aa.axis()[0];
    trackers_[serial].bTh[4] = aa.angle() * aa.axis()[1];
    trackers_[serial].bTh[5] = aa.angle() * aa.axis()[2];
    trackers_[serial].ready = false;
    // Publish sensor location and body trajectory 
    pub_sensors_[serial] = nh.advertise<visualization_msgs::MarkerArray>(
      "/sensors/" + *jt, 10, true);
    LighthouseMap::iterator lt;
    for (lt = lighthouses_.begin(); lt != lighthouses_.end(); lt++)
      pub_path_[lt->first][serial] = nh.advertise<nav_msgs::Path>(
        "/path/" + *jt + "/" + lt->first, 10, true);
  }

  // If reading the configuration file results in inserting the correct
  // number of static transforms into the problem, then we can publish
  // the solution for use by other entities in the system.
  if (ReadConfig(calfile_, frame_world_, frame_vive_, frame_body_,
    registration_, lighthouses_, trackers_)) {
    ROS_INFO("Read transforms from calibration");
  } else {
    ROS_INFO("Could not read calibration file");
  }
  SendTransforms(frame_world_, frame_vive_, frame_body_,
    registration_, lighthouses_, trackers_);

  // Subscribe to tracker and lighthouse updates
  ros::Subscriber sub_tracker  = 
    nh.subscribe<deepdive_ros::Trackers>("/trackers", 1000, std::bind(
      TrackerCallback, std::placeholders::_1, std::ref(trackers_),
        NewTrackerCallback));
  ros::Subscriber sub_lighthouse = 
    nh.subscribe<deepdive_ros::Lighthouses>("/lighthouses", 1000, std::bind(
      LighthouseCallback, std::placeholders::_1, std::ref(lighthouses_),
        NewLighthouseCallback));
  ros::Subscriber sub_light =
    nh.subscribe("/light", 1000, LightCallback);
  ros::ServiceServer service =
    nh.advertiseService("/trigger", TriggerCallback);

  // Setup a timer to automatically trigger solution on end of experiment
  timer_ = nh.createTimer(ros::Duration(1.0), TimerCallback, true, false);

  // Block until safe shutdown
  ros::spin();

  // Success!
  return 0;
}