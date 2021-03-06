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
*   * Redistributions of source code must retain the above copyright
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

#include "tracker.h"

using namespace MatrixWrapper;
using namespace BFL;
using namespace tf;
using namespace std;
using namespace ros;


namespace estimation
{
// constructor
PedestrianTracking::PedestrianTracking():
    prior_(NULL),
    filter_(NULL),
    filter_initialized_(false),
    odom_initialized_(false),
    pedestrian_initialized_(false),
    output_frame_(std::string("odom_combined")),
    base_footprint_frame_(std::string("base_footprint"))
{
    // create SYSTEM MODEL
    ColumnVector sysNoise_Mu(6);  sysNoise_Mu = 0;
    SymmetricMatrix sysNoise_Cov(6); sysNoise_Cov = 0;
    for (unsigned int i=1; i<=6; i++) sysNoise_Cov(i,i) = pow(1000.0,2);
    Gaussian system_Uncertainty(sysNoise_Mu, sysNoise_Cov);
    sys_pdf_   = new NonLinearAnalyticConditionalGaussianOdo(system_Uncertainty);
    sys_model_ = new AnalyticSystemModelGaussianUncertainty(sys_pdf_);

    // create MEASUREMENT MODEL ODOM
    ColumnVector measNoiseOdom_Mu(6);  measNoiseOdom_Mu = 0;
    SymmetricMatrix measNoiseOdom_Cov(6);  measNoiseOdom_Cov = 0;
    for (unsigned int i=1; i<=6; i++) measNoiseOdom_Cov(i,i) = 1;
    Gaussian measurement_Uncertainty_Odom(measNoiseOdom_Mu, measNoiseOdom_Cov);
    Matrix Hodom(6,6);  Hodom = 0;
    Hodom(1,1) = 1;    Hodom(2,2) = 1;    Hodom(6,6) = 1;
    odom_meas_pdf_   = new LinearAnalyticConditionalGaussian(Hodom, measurement_Uncertainty_Odom);
    odom_meas_model_ = new LinearAnalyticMeasurementModelGaussianUncertainty(odom_meas_pdf_);

    // create MEASUREMENT MODEL PEDESTRIAN
    ColumnVector measNoiseImu_Mu(3);  measNoiseImu_Mu = 0;
    SymmetricMatrix measNoiseImu_Cov(3);  measNoiseImu_Cov = 0;
    for (unsigned int i=1; i<=3; i++) measNoiseImu_Cov(i,i) = 1;
    Gaussian measurement_Uncertainty_Imu(measNoiseImu_Mu, measNoiseImu_Cov);
    Matrix Himu(3,6);  Himu = 0;
    Himu(1,4) = 1;    Himu(2,5) = 1;    Himu(3,6) = 1;
    pedestrian_meas_pdf_   = new LinearAnalyticConditionalGaussian(Himu, measurement_Uncertainty_Imu);
    pedestrian_meas_model_ = new LinearAnalyticMeasurementModelGaussianUncertainty(pedestrian_meas_pdf_);
};



// destructor
PedestrianTracking::~PedestrianTracking(){
    if (filter_) delete filter_;
    if (prior_)  delete prior_;
    delete odom_meas_model_;
    delete odom_meas_pdf_;
    delete pedestrian_meas_model_;
    delete pedestrian_meas_pdf_;
    delete sys_pdf_;
    delete sys_model_;
};


// initialize prior density of filter
void PedestrianTracking::initialize(const Transform& prior, const Time& time)
{
    // set prior of filter
    ColumnVector prior_Mu(6);
    decomposeTransform(prior, prior_Mu(1), prior_Mu(2), prior_Mu(3), prior_Mu(4), prior_Mu(5), prior_Mu(6));
    SymmetricMatrix prior_Cov(6);
    for (unsigned int i=1; i<=6; i++) {
        for (unsigned int j=1; j<=6; j++){
            if (i==j)  prior_Cov(i,j) = pow(0.001,2);
            else prior_Cov(i,j) = 0;
        }
    }
    prior_  = new Gaussian(prior_Mu,prior_Cov);
    filter_ = new ExtendedKalmanFilter(prior_);

    // remember prior
    addMeasurement(StampedTransform(prior, time, output_frame_, base_footprint_frame_));
    filter_estimate_old_vec_ = prior_Mu;
    filter_estimate_old_ = prior;
    filter_time_old_     = time;

    // filter initialized
    filter_initialized_ = true;
}

// update filter
bool PedestrianTracking::update(bool odom_active, bool pedestrian_active, const Time&  filter_time, bool& diagnostics_res)
{
    // only update filter when it is initialized
    if (!filter_initialized_)
    {
        ROS_INFO("Cannot update filter when filter was not initialized first.");
        return false;
    }

    // only update filter for time later than current filter time
    double dt = (filter_time - filter_time_old_).toSec();
    if (dt == 0) return false;
    if (dt <  0)
    {
        ROS_INFO("Will not update robot pose with time %f sec in the past.", dt);
        return false;
    }
    ROS_DEBUG("Update filter at time %f with dt %f", filter_time.toSec(), dt);


    // system update filter
    // --------------------
    // for now only add system noise
    ColumnVector vel_desi(2); vel_desi = 0;
    filter_->Update(sys_model_, vel_desi);


    // process odom measurement
    // ------------------------
    ROS_DEBUG("Process odom meas");
    if (odom_active)
    {
        if (!transformer_.canTransform(base_footprint_frame_,"wheelodom", filter_time))
        {
            ROS_ERROR("filter time older than odom message buffer");
            return false;
        }
        transformer_.lookupTransform("wheelodom", base_footprint_frame_, filter_time, odom_meas_);
        if (odom_initialized_)
        {
            // convert absolute odom measurements to relative odom measurements in horizontal plane
            Transform odom_rel_frame =  Transform(tf::createQuaternionFromYaw(filter_estimate_old_vec_(6)),
                                                  filter_estimate_old_.getOrigin()) * odom_meas_old_.inverse() * odom_meas_;
            ColumnVector odom_rel(6);
            decomposeTransform(odom_rel_frame, odom_rel(1), odom_rel(2), odom_rel(3), odom_rel(4), odom_rel(5), odom_rel(6));
            angleOverflowCorrect(odom_rel(6), filter_estimate_old_vec_(6));
            // update filter
            odom_meas_pdf_->AdditiveNoiseSigmaSet(odom_covariance_ * pow(dt,2));

            ROS_DEBUG("Update filter with odom measurement %f %f %f %f %f %f",
                      odom_rel(1), odom_rel(2), odom_rel(3), odom_rel(4), odom_rel(5), odom_rel(6));
            filter_->Update(odom_meas_model_, odom_rel);
            diagnostics_odom_rot_rel_ = odom_rel(6);
        }
        else
        {
            odom_initialized_ = true;
            diagnostics_odom_rot_rel_ = 0;
        }
        odom_meas_old_ = odom_meas_;
    }
    // sensor not active
    else odom_initialized_ = false;


    // process pedestrian measurement
    // -----------------------
    if (pedestrian_active)
    {
        if (!transformer_.canTransform(base_footprint_frame_,"imu", filter_time))
        {
            ROS_ERROR("filter time older than imu message buffer");
            return false;
        }
        transformer_.lookupTransform("imu", base_footprint_frame_, filter_time, pedestrian_meas_);
        if (pedestrian_initialized_)
        {
            // convert absolute imu yaw measurement to relative imu yaw measurement
            Transform pedestrian_rel_frame =  filter_estimate_old_ * pedestrian_meas_old_.inverse() * pedestrian_meas_;
            ColumnVector pedestrian_rel(3); double tmp;
            decomposeTransform(pedestrian_rel_frame, tmp, tmp, tmp, tmp, tmp, pedestrian_rel(3));
            decomposeTransform(pedestrian_meas_,     tmp, tmp, tmp, pedestrian_rel(1), pedestrian_rel(2), tmp);
            angleOverflowCorrect(pedestrian_rel(3), filter_estimate_old_vec_(6));
            diagnostics_imu_rot_rel_ = pedestrian_rel(3);
            // update filter
            pedestrian_meas_pdf_->AdditiveNoiseSigmaSet(pedestrian_covariance_ * pow(dt,2));
            filter_->Update(pedestrian_meas_model_,  pedestrian_rel);
        }
        else
        {
            pedestrian_initialized_ = true;
            diagnostics_imu_rot_rel_ = 0;
        }
        pedestrian_meas_old_ = pedestrian_meas_;
    }
    // sensor not active
    else pedestrian_initialized_ = false;


    // remember last estimate
    filter_estimate_old_vec_ = filter_->PostGet()->ExpectedValueGet();
    tf::Quaternion q;
    q.setRPY(filter_estimate_old_vec_(4), filter_estimate_old_vec_(5), filter_estimate_old_vec_(6));
    filter_estimate_old_ = Transform(q, Vector3(filter_estimate_old_vec_(1), filter_estimate_old_vec_(2), filter_estimate_old_vec_(3)));
    filter_time_old_ = filter_time;
    addMeasurement(StampedTransform(filter_estimate_old_, filter_time, output_frame_, base_footprint_frame_));

    // diagnostics
    diagnostics_res = true;
    if (odom_active && pedestrian_active)
    {
        double diagnostics = fabs(diagnostics_odom_rot_rel_ - diagnostics_imu_rot_rel_)/dt;
        if (diagnostics > 0.3 && dt > 0.01)
        {
            diagnostics_res = false;
        }
    }

    return true;
}

void PedestrianTracking::addMeasurement(const StampedTransform& meas)
{
    ROS_DEBUG("AddMeasurement from %s to %s:  (%f, %f, %f)  (%f, %f, %f, %f)",
              meas.frame_id_.c_str(), meas.child_frame_id_.c_str(),
              meas.getOrigin().x(), meas.getOrigin().y(), meas.getOrigin().z(),
              meas.getRotation().x(),  meas.getRotation().y(),
              meas.getRotation().z(), meas.getRotation().w());
    transformer_.setTransform( meas );
}

void PedestrianTracking::addMeasurement(const StampedTransform& meas, const MatrixWrapper::SymmetricMatrix& covar)
{
    // check covariance
    for (unsigned int i=0; i<covar.rows(); i++)
    {
        if (covar(i+1,i+1) == 0)
        {
            ROS_ERROR("Covariance specified for measurement on topic %s is zero", meas.child_frame_id_.c_str());
            return;
        }
    }
    // add measurements
    addMeasurement(meas);
    if (meas.child_frame_id_ == "wheelodom") odom_covariance_ = covar;
    else if (meas.child_frame_id_ == "imu")  pedestrian_covariance_  = covar;
    else ROS_ERROR("Adding a measurement for an unknown sensor %s", meas.child_frame_id_.c_str());
}


// get latest filter posterior as vector
void PedestrianTracking::getEstimate(MatrixWrapper::ColumnVector& estimate)
{
    estimate = filter_estimate_old_vec_;
}

// get filter posterior at time 'time' as Transform
void PedestrianTracking::getEstimate(Time time, Transform& estimate)
{
    StampedTransform tmp;
    if (!transformer_.canTransform(base_footprint_frame_,output_frame_, time))
    {
        ROS_ERROR("Cannot get transform at time %f", time.toSec());
        return;
    }
    transformer_.lookupTransform(output_frame_, base_footprint_frame_, time, tmp);
    estimate = tmp;
}

// get filter posterior at time 'time' as Stamped Transform
void PedestrianTracking::getEstimate(Time time, StampedTransform& estimate)
{
    if (!transformer_.canTransform(output_frame_, base_footprint_frame_, time)){
        ROS_ERROR("Cannot get transform at time %f", time.toSec());
        return;
    }
    transformer_.lookupTransform(output_frame_, base_footprint_frame_, time, estimate);
}

// get most recent filter posterior as PoseWithCovarianceStamped
void PedestrianTracking::getEstimate(geometry_msgs::PoseWithCovarianceStamped& estimate)
{
    // pose
    StampedTransform tmp;
    if (!transformer_.canTransform(output_frame_, base_footprint_frame_, ros::Time())){
        ROS_ERROR("Cannot get transform at time %f", 0.0);
        return;
    }
    transformer_.lookupTransform(output_frame_, base_footprint_frame_, ros::Time(), tmp);
    poseTFToMsg(tmp, estimate.pose.pose);

    // header
    estimate.header.stamp = tmp.stamp_;
    estimate.header.frame_id = "odom";

    // covariance
    SymmetricMatrix covar =  filter_->PostGet()->CovarianceGet();
    for (unsigned int i=0; i<6; i++)
        for (unsigned int j=0; j<6; j++)
            estimate.pose.covariance[6*i+j] = covar(i+1,j+1);
}

// correct for angle overflow
void PedestrianTracking::angleOverflowCorrect(double& a, double ref)
{
    while ((a-ref) >  M_PI) a -= 2*M_PI;
    while ((a-ref) < -M_PI) a += 2*M_PI;
}

// decompose Transform into x,y,z,Rx,Ry,Rz
void PedestrianTracking::decomposeTransform(const StampedTransform& trans, double& x, double& y, double&z, double&Rx, double& Ry, double& Rz)
{
    x = trans.getOrigin().x();
    y = trans.getOrigin().y();
    z = trans.getOrigin().z();
    trans.getBasis().getEulerYPR(Rz, Ry, Rx);
}

// decompose Transform into x,y,z,Rx,Ry,Rz
void PedestrianTracking::decomposeTransform(const Transform& trans, double& x, double& y, double&z, double&Rx, double& Ry, double& Rz)
{
    x = trans.getOrigin().x();
    y = trans.getOrigin().y();
    z = trans.getOrigin().z();
    trans.getBasis().getEulerYPR(Rz, Ry, Rx);
}

void PedestrianTracking::setOutputFrame(const std::string& output_frame)
{
    output_frame_ = output_frame;
}

void PedestrianTracking::setBaseFootprintFrame(const std::string& base_frame)
{
    base_footprint_frame_ = base_frame;
}

}; // namespace
