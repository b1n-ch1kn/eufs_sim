/*
 * AMZ-Driverless
 * Copyright (c) 2018 Authors:
 *   - Juraj Kabzan <kabzanj@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// Main Include
#include "gazebo_race_car_plugin/gazebo_ros_race_car.hpp"

// STD Include
#include <algorithm>
#include <fstream>
#include <mutex>   // NOLINT(build/c++11)
#include <thread>  // NOLINT(build/c++11)

namespace gazebo_plugins {
namespace eufs_plugins {

RaceCarPlugin::RaceCarPlugin() {}

RaceCarPlugin::~RaceCarPlugin() { _update_connection.reset(); }

void RaceCarPlugin::Load(gazebo::physics::ModelPtr model, sdf::ElementPtr sdf) {
    _rosnode = gazebo_ros::Node::Get(sdf);

    RCLCPP_DEBUG(_rosnode->get_logger(), "Loading RaceCarPlugin");

    _model = model;
    _world = _model->GetWorld();

    _tf_br = std::make_unique<tf2_ros::TransformBroadcaster>(_rosnode);
    _state_machine = std::make_unique<StateMachine>(_rosnode);

    // Initialize parameters
    initParams(sdf);

    // ROS Publishers
    // Wheel odom
    _pub_wheel_odom = _rosnode->create_publisher<nav_msgs::msg::Odometry>("/vehicle/wheel_odom", 1);
    // Steering angle
    _pub_steering_angle = _rosnode->create_publisher<std_msgs::msg::Float32>("/vehicle/steering_angle", 1);
    // Pose (from slam output)
    if (_simulate_slam) {
        _pub_pose = _rosnode->create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>("slam/car_pose", 1);
    }
    // Ground truth
    if (_pub_gt) {
        _pub_gt_wheel_odom = _rosnode->create_publisher<nav_msgs::msg::Odometry>("/ground_truth/wheel_odom", 1);
        _pub_gt_pose = _rosnode->create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>("ground_truth/car_pose", 1);
        _pub_gt_steering_angle = _rosnode->create_publisher<std_msgs::msg::Float32>("/ground_truth/steering_angle", 1);
    }

    // ROS Services
    _reset_vehicle_pos_srv = _rosnode->create_service<std_srvs::srv::Trigger>(
        "/ros_can/reset_vehicle_pos",
        std::bind(&RaceCarPlugin::resetVehiclePosition, this, std::placeholders::_1, std::placeholders::_2));
    _command_mode_srv = _rosnode->create_service<std_srvs::srv::Trigger>(
        "/race_car_model/command_mode",
        std::bind(&RaceCarPlugin::returnCommandMode, this, std::placeholders::_1, std::placeholders::_2));

    // ROS Subscriptions
    _sub_cmd = _rosnode->create_subscription<ackermann_msgs::msg::AckermannDriveStamped>(
        "/control/driving_command", 1, std::bind(&RaceCarPlugin::onCmd, this, std::placeholders::_1));

    // Connect to Gazebo
    _update_connection = gazebo::event::Events::ConnectWorldUpdateBegin(std::bind(&RaceCarPlugin::update, this));
    _last_sim_time = _world->SimTime();

    _max_steering_rate = (_vehicle->getParam().input_ranges.delta.max - _vehicle->getParam().input_ranges.delta.min) /
                         _steering_lock_time;

    // Set offset
    setPositionFromWorld();

    RCLCPP_INFO(_rosnode->get_logger(), "RaceCarPlugin Loaded");
}

void RaceCarPlugin::initParams(const sdf::ElementPtr &sdf) {
    // SDF Parameters
    _update_rate = get_double_parameter(sdf, "updateRate", 1.0, "1.0", _rosnode->get_logger());
    _publish_rate = get_double_parameter(sdf, "publishRate", 1.0, "1.0", _rosnode->get_logger());
    _reference_frame = get_string_parameter(sdf, "referenceFrame", "map", "map", _rosnode->get_logger());
    _robot_frame = get_string_parameter(sdf, "robotFrame", "base_link", "base_link", _rosnode->get_logger());
    _control_delay = get_double_parameter(sdf, "controlDelay", 1.0, "1.0", _rosnode->get_logger());
    _steering_lock_time = get_double_parameter(sdf, "steeringLockTime", 1.0, "1.0", _rosnode->get_logger());

    _publish_tf = get_bool_parameter(sdf, "publishTransform", false, "false", _rosnode->get_logger());
    _pub_gt = get_bool_parameter(sdf, "pubGroundTruth", false, "false", _rosnode->get_logger());
    _simulate_slam = get_bool_parameter(sdf, "simulateSLAM", false, "false", _rosnode->get_logger());

    std::string command_str = get_string_parameter(sdf, "commandMode", "acceleration", "acceleration", _rosnode->get_logger());

    if (command_str.compare("acceleration") == 0) {
        _command_mode = acceleration;
    } else if (command_str.compare("velocity") == 0) {
        _command_mode = velocity;
    } else {
        RCLCPP_FATAL(_rosnode->get_logger(), "gazebo_ros_race_car plugin invalid command mode, cannot proceed");
        return;
    }

    // Vehicle model
    std::string vehicle_model_ = get_string_parameter(sdf, "vehicleModel", "DynamicBicycle", "DynamicBicycle", _rosnode->get_logger());
    std::string vehicle_yaml_name = get_string_parameter(sdf, "yamlConfig", "vehicle.yaml", "null", _rosnode->get_logger());

    if (vehicle_yaml_name == "null") {
        RCLCPP_FATAL(_rosnode->get_logger(), "gazebo_ros_race_car plugin missing <yamlConfig>, cannot proceed");
        return;
    }

    if (vehicle_model_ == "PointMass") {
        _vehicle = std::unique_ptr<eufs::models::VehicleModel>(new eufs::models::PointMass(vehicle_yaml_name));
    } else if (vehicle_model_ == "DynamicBicycle") {
        _vehicle = std::unique_ptr<eufs::models::VehicleModel>(new eufs::models::DynamicBicycle(vehicle_yaml_name));
    } else {
        RCLCPP_FATAL(_rosnode->get_logger(), "gazebo_ros_race_car plugin invalid vehicle model, cannot proceed");
        return;
    }

    // Steering joints
    std::string leftSteeringJointName = _model->GetName() + "::left_steering_hinge_joint";
    _left_steering_joint = _model->GetJoint(leftSteeringJointName);
    std::string rightSteeringJointName = _model->GetName() + "::right_steering_hinge_joint";
    _right_steering_joint = _model->GetJoint(rightSteeringJointName);

    // Noise
    std::string noise_yaml_name = get_string_parameter(sdf, "noiseConfig", "noise.yaml", "null", _rosnode->get_logger());
    if (noise_yaml_name == "null") {
        RCLCPP_FATAL(_rosnode->get_logger(), "gazebo_ros_race_car plugin missing <noise_config>, cannot proceed");
        return;
    }
    _noise = std::make_unique<eufs::models::Noise>(noise_yaml_name);
}

void RaceCarPlugin::setPositionFromWorld() {
    _offset = _model->WorldPose();

    RCLCPP_DEBUG(_rosnode->get_logger(), "Got starting offset %f %f %f", _offset.Pos()[0], _offset.Pos()[1],
                 _offset.Pos()[2]);

    _state.x = 0.0;
    _state.y = 0.0;
    _state.z = 0.0;
    _state.yaw = 0.0;
    _state.v_x = 0.0;
    _state.v_y = 0.0;
    _state.v_z = 0.0;
    _state.r_x = 0.0;
    _state.r_y = 0.0;
    _state.r_z = 0.0;
    _state.a_x = 0.0;
    _state.a_y = 0.0;
    _state.a_z = 0.0;

    _last_cmd.drive.steering_angle = 0;
    _last_cmd.drive.acceleration = -100;
    _last_cmd.drive.speed = 0;
}

bool RaceCarPlugin::resetVehiclePosition(std::shared_ptr<std_srvs::srv::Trigger::Request>,
                                         std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
    _state.x = 0.0;
    _state.y = 0.0;
    _state.z = 0.0;
    _state.yaw = 0.0;
    _state.v_x = 0.0;
    _state.v_y = 0.0;
    _state.v_z = 0.0;
    _state.r_x = 0.0;
    _state.r_y = 0.0;
    _state.r_z = 0.0;
    _state.a_x = 0.0;
    _state.a_y = 0.0;
    _state.a_z = 0.0;

    const ignition::math::Vector3d vel(0.0, 0.0, 0.0);
    const ignition::math::Vector3d angular(0.0, 0.0, 0.0);

    _model->SetWorldPose(_offset);
    _model->SetAngularVel(angular);
    _model->SetLinearVel(vel);

    return response->success;
}

void RaceCarPlugin::returnCommandMode(std::shared_ptr<std_srvs::srv::Trigger::Request>,
                                      std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
    std::string command_mode_str;
    if (_command_mode == acceleration) {
        command_mode_str = "acceleration";
    } else {
        command_mode_str = "velocity";
    }

    response->success = true;
    response->message = command_mode_str;
}

void RaceCarPlugin::setModelState() {
    double yaw = _state.yaw + _offset.Rot().Yaw();

    double x = _offset.Pos().X() + _state.x * cos(_offset.Rot().Yaw()) - _state.y * sin(_offset.Rot().Yaw());
    double y = _offset.Pos().Y() + _state.x * sin(_offset.Rot().Yaw()) + _state.y * cos(_offset.Rot().Yaw());
    double z = _state.z;

    double vx = _state.v_x * cos(yaw) - _state.v_y * sin(yaw);
    double vy = _state.v_x * sin(yaw) + _state.v_y * cos(yaw);

    const ignition::math::Pose3d pose(x, y, z, 0, 0.0, yaw);
    const ignition::math::Vector3d vel(vx, vy, 0.0);
    const ignition::math::Vector3d angular(0.0, 0.0, _state.r_z);

    _model->SetWorldPose(pose);
    _model->SetAngularVel(angular);
    _model->SetLinearVel(vel);
}

geometry_msgs::msg::PoseWithCovarianceStamped RaceCarPlugin::stateToPoseMsg(const eufs::models::State &state) {
    geometry_msgs::msg::PoseWithCovarianceStamped pose_msg;

    pose_msg.header.stamp.sec = _last_sim_time.sec;
    pose_msg.header.stamp.nanosec = _last_sim_time.nsec;
    pose_msg.header.frame_id = _reference_frame;

    pose_msg.pose.pose.position.x = state.x;
    pose_msg.pose.pose.position.y = state.y;
    pose_msg.pose.pose.position.z = state.z;

    std::vector<double> orientation = {state.yaw, 0.0, 0.0};

    orientation = ToQuaternion(orientation);

    pose_msg.pose.pose.orientation.x = orientation[0];
    pose_msg.pose.pose.orientation.y = orientation[1];
    pose_msg.pose.pose.orientation.z = orientation[2];
    pose_msg.pose.pose.orientation.w = orientation[3];

    return pose_msg;
}

void RaceCarPlugin::publishCarPose() {
    geometry_msgs::msg::PoseWithCovarianceStamped pose = stateToPoseMsg(_state);

    if (has_subscribers(_pub_gt_pose)) {
        _pub_gt_pose->publish(pose);
    }

    // Add noise
    geometry_msgs::msg::PoseWithCovarianceStamped pose_noisy = stateToPoseMsg(_noise->applyNoise(_state));

    // Fill in covariance matrix
    const eufs::models::NoiseParam &noise_param = _noise->getNoiseParam();
    pose_noisy.pose.covariance[0] = pow(noise_param.position[0], 2);
    pose_noisy.pose.covariance[7] = pow(noise_param.position[1], 2);
    pose_noisy.pose.covariance[14] = pow(noise_param.position[2], 2);

    pose_noisy.pose.covariance[21] = pow(noise_param.orientation[0], 2);
    pose_noisy.pose.covariance[28] = pow(noise_param.orientation[1], 2);
    pose_noisy.pose.covariance[35] = pow(noise_param.orientation[2], 2);

    if (has_subscribers(_pub_pose)) {
        _pub_pose->publish(pose_noisy);
    }
}

nav_msgs::msg::Odometry RaceCarPlugin::getWheelOdometry(const std::vector<double> &speeds,
                                                        const double &input) {
    nav_msgs::msg::Odometry wheel_odom;

    // Calculate avg wheel speeds
    double rf = speeds[0];
    double lf = speeds[1];
    double rb = speeds[2];
    double lb = speeds[3];
    double avg_wheel_speed = (rf + lf + rb + lb) / 4.0;

    // Calculate odom with wheel speed and steering angle
    wheel_odom.twist.twist.linear.x = avg_wheel_speed;
    wheel_odom.twist.twist.linear.y = 0.0;

    wheel_odom.twist.twist.angular.z = avg_wheel_speed * tan(input) / _vehicle->getParam().kinematic.axle_width;

    wheel_odom.header.stamp.sec = _last_sim_time.sec;
    wheel_odom.header.stamp.nanosec = _last_sim_time.nsec;
    wheel_odom.header.frame_id = _reference_frame;
    wheel_odom.child_frame_id = _robot_frame;

    return wheel_odom;
}

void RaceCarPlugin::publishVehicleOdom() {
    // Publish steering angle
    std_msgs::msg::Float32 steering_angle;
    steering_angle.data = _act_input.delta;

    if (has_subscribers(_pub_gt_steering_angle)) {
        _pub_gt_steering_angle->publish(steering_angle);
    }

    // Publish wheel odometry
    std::vector<double> wheel_speeds = {_state.v_x, _state.v_x, _state.v_x, _state.v_x};
    nav_msgs::msg::Odometry wheel_odom = getWheelOdometry(wheel_speeds, steering_angle.data);

    if (has_subscribers(_pub_gt_wheel_odom)) {
        _pub_gt_wheel_odom->publish(wheel_odom);
    }

    // Add noise
    std_msgs::msg::Float32 steering_angle_noisy;
    steering_angle_noisy.data = _noise->applyNoiseToSteering(steering_angle.data);

    if (has_subscribers(_pub_steering_angle)) {
        _pub_steering_angle->publish(steering_angle_noisy);
    }

    std::vector<double> wheel_speeds_noisy = _noise->applyNoiseToWheels(wheel_speeds);
    nav_msgs::msg::Odometry wheel_odom_noisy = getWheelOdometry(wheel_speeds_noisy, steering_angle_noisy.data);

    if (has_subscribers(_pub_wheel_odom)) {
        _pub_wheel_odom->publish(wheel_odom_noisy);
    }
}


void RaceCarPlugin::publishTf() {
    eufs::models::State state_noisy = _noise->applyNoise(_state);

    // Position
    tf2::Transform transform;
    transform.setOrigin(tf2::Vector3(state_noisy.x, state_noisy.y, 0.0));

    // Orientation
    tf2::Quaternion q;
    q.setRPY(0.0, 0.0, state_noisy.yaw);
    transform.setRotation(q);

    // Send TF
    geometry_msgs::msg::TransformStamped transform_stamped;

    transform_stamped.header.stamp.sec = _last_sim_time.sec;
    transform_stamped.header.stamp.nanosec = _last_sim_time.nsec;
    transform_stamped.header.frame_id = _reference_frame;
    transform_stamped.child_frame_id = _robot_frame;
    tf2::convert(transform, transform_stamped.transform);

    _tf_br->sendTransform(transform_stamped);
}

void RaceCarPlugin::update() {
    // Check against update rate
    gazebo::common::Time curTime = _world->SimTime();
    double dt = calc_dt(_last_sim_time, curTime);
    if (dt < (1 / _update_rate)) {
        return;
    }

    _last_sim_time = curTime;

    _des_input.acc = _last_cmd.drive.acceleration;
    _des_input.vel = _last_cmd.drive.speed;
    _des_input.delta = _last_cmd.drive.steering_angle * 3.1415 / 180 /
                       (180 / 26);  // scales from steering wheel 90* to front wheels 26*

    if (_command_mode == velocity) {
        double current_speed = std::sqrt(std::pow(_state.v_x, 2) + std::pow(_state.v_y, 2));
        _act_input.acc = (_des_input.vel - current_speed) / dt;
    }

    // Make sure steering rate is within limits
    _act_input.delta += (_des_input.delta - _act_input.delta >= 0 ? 1 : -1) *
                        std::min(_max_steering_rate * dt, std::abs(_des_input.delta - _act_input.delta));

    // Ensure vehicle can drive
    if (!_state_machine->canDrive() || (_world->SimTime() - _last_cmd_time).Double() > 0.5) {
        _act_input.acc = -100.0;
        _act_input.vel = 0.0;
        _act_input.delta = 0.0;
    }

    counter++;
    if (counter == 100) {
        RCLCPP_DEBUG(_rosnode->get_logger(), "steering %f", _act_input.delta);
        counter = 0;
    }

    // Update z value from simulation
    // This allows the state to have the most up to date value of z. Without this
    // the vehicle in simulation has problems interacting with the ground plane.
    // This may cause problems if the vehicle models start to take into account z
    // but because this simulation isn't for flying cars we should be ok (at least for now).
    _state.z = _model->WorldPose().Pos().Z();

    _vehicle->updateState(_state, _act_input, dt);

    _left_steering_joint->SetPosition(0, _act_input.delta);
    _right_steering_joint->SetPosition(0, _act_input.delta);
    setModelState();

    double time_since_last_published = (_last_sim_time - _time_last_published).Double();
    if (time_since_last_published < (1 / _publish_rate)) {
        return;
    }
    _time_last_published = _last_sim_time;

    // Publish Everything
    publishCarPose();
    publishVehicleOdom();

    if (_publish_tf) {
        publishTf();
    }

    _state_machine->spinOnce(_last_sim_time);
}

void RaceCarPlugin::onCmd(const ackermann_msgs::msg::AckermannDriveStamped::SharedPtr msg) {
    RCLCPP_INFO(_rosnode->get_logger(), "Last time: %f", (_world->SimTime() - _last_cmd_time).Double());
    while ((_world->SimTime() - _last_cmd_time).Double() < _control_delay) {
        RCLCPP_DEBUG(_rosnode->get_logger(), "Waiting until control delay is over");
    }
    _last_cmd.drive.acceleration = msg->drive.acceleration;
    _last_cmd.drive.speed = msg->drive.speed;
    _last_cmd.drive.steering_angle = msg->drive.steering_angle;
    _last_cmd_time = _world->SimTime();
}

std::vector<double> RaceCarPlugin::ToQuaternion(std::vector<double> &euler) {
    // Abbreviations for the various angular functions
    double cy = cos(euler[0] * 0.5);
    double sy = sin(euler[0] * 0.5);
    double cp = cos(euler[1] * 0.5);
    double sp = sin(euler[1] * 0.5);
    double cr = cos(euler[2] * 0.5);
    double sr = sin(euler[2] * 0.5);

    std::vector<double> q;
    q.push_back(cy * cp * sr - sy * sp * cr);  // x
    q.push_back(sy * cp * sr + cy * sp * cr);  // y
    q.push_back(sy * cp * cr - cy * sp * sr);  // z
    q.push_back(cy * cp * cr + sy * sp * sr);  // w

    return q;
}

GZ_REGISTER_MODEL_PLUGIN(RaceCarPlugin)

}  // namespace eufs_plugins
}  // namespace gazebo_plugins
