/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Author: Wim Meeussen */

#ifndef TRACKER_NODE_H
#define TRACKER_NODE_H

// ros stuff
#include <ros/ros.h>
#include <tf/tf.h>
#include <tf/transform_listener.h>
#include <tf/transform_broadcaster.h>
#include "tracker.h"
//#include <robot_pose_ekf/GetStatus.h>

// messages
#include "nav_msgs/Odometry.h"
#include "geometry_msgs/Twist.h"
#include "sensor_msgs/Imu.h"
#include "geometry_msgs/PoseStamped.h"
#include "geometry_msgs/PoseWithCovarianceStamped.h"
#include <pedestrian_detector/DetectionList.h>

#include <boost/thread/mutex.hpp>

// log files
#include <fstream>

namespace estimation
{

/** \mainpage
 *  \htmlinclude manifest.html
 *
 * <b>Package Summary</b>
 * This package provides two main classes:
 *  1) OdomEstimation performs all sensor fusion operations, and
 *  2) PedestrianTrackingNode provides a ROS wrapper around OdomEstimation
*/

typedef boost::shared_ptr<nav_msgs::Odometry const> OdomConstPtr;
typedef boost::shared_ptr<pedestrian_detector::DetectionList> PedestrianConstPtr;
typedef boost::shared_ptr<geometry_msgs::Twist const> VelConstPtr;

class PedestrianTrackingNode
{
public:
  /// constructor
  PedestrianTrackingNode();

  /// destructor
  virtual ~PedestrianTrackingNode();

private:
  /// the mail filter loop that will be called periodically
  void spin(const ros::TimerEvent& e);

  /// callback function for odo data
  void odomCallback(const OdomConstPtr& odom);

  /// callback function for odo data
  void pedestrianCallback(const PedestrianConstPtr& pedestrian_msg);

  /// get the status of the filter
  //bool getStatus(robot_pose_ekf::GetStatus::Request& req, robot_pose_ekf::GetStatus::Response& resp);

  ros::NodeHandle node_;
  ros::Timer timer_;
  ros::Publisher pose_pub_;
  ros::Subscriber odom_sub_, pedestrian_sub_;
  ros::ServiceServer state_srv_;

  // ekf filter
  PedestrianTracking my_filter_;

  // estimated robot pose message to send
  geometry_msgs::PoseWithCovarianceStamped  output_;

  // robot state
  tf::TransformListener    robot_state_;
  tf::TransformBroadcaster odom_broadcaster_;

  // vectors
  tf::Transform odom_meas_, pedestrian_meas_;
  tf::StampedTransform camera_base_;
  ros::Time odom_time_, pedestrian_time_;
  ros::Time odom_stamp_, imu_stamp_, pedestrian_stamp_, filter_stamp_;
  ros::Time odom_init_stamp_,  pedestrian_init_stamp_;
  bool odom_active_, pedestrian_active_;
  bool odom_used_, pedestrian_used_;
  bool odom_initializing_, pedestrian_initializing_;
  double timeout_;
  MatrixWrapper::SymmetricMatrix odom_covariance_, pedestrian_covariance_;
  bool debug_, self_diagnose_;
  std::string output_frame_, base_footprint_frame_, tf_prefix_;

  // log files for debugging
  std::ofstream odom_file_, pedestrian_file_, corr_file_, time_file_, extra_file_;

  // counters
  unsigned int odom_callback_counter_, pedestrian_callback_counter_, ekf_sent_counter_;

}; // class

}; // namespace

#endif // TRACKER_NODE_H
