/**
 *  \author Okan Asik
 *  \desc   Gazebo Plugin to control Zephyr fixed wing plane
 */

#include <ros/ros.h>
#include <sensor_msgs/JointState.h>
#include <nav_msgs/Odometry.h>
#include <std_msgs/String.h>
#include <suruiha_gazebo_plugins/zephyr_controller.h>
#include <gazebo/common/Plugin.hh>
#include <ignition/math.hh>
#include <sdf/sdf.hh>

namespace gazebo {

    // Register this plugin with the simulator
    GZ_REGISTER_MODEL_PLUGIN(ZephyrController);

    // define class static const variables
//    const unsigned int ZephyrController::PROPELLER_JOINT = 0;
//    const unsigned int ZephyrController::FLAP_LEFT_JOINT = 1;
//    const unsigned int ZephyrController::FLAP_RIGHT_JOINT = 2;
//
//    std::string ZephyrController::PROPELLER_JOINT_STR = "propeller_joint";
//    std::string ZephyrController::FLAP_LEFT_JOINT_STR = "flap_left_joint";
//    std::string ZephyrController::FLAP_RIGHT_JOINT_STR = "flap_right_joint";

    ZephyrController::ZephyrController() {
//        control_topic_name_ = "zephyr_control";
//        pose_topic_name_ = "zephyr_pose";
//        user_command_topic_name_ = "zephyr_user_command";
        lastUpdateTime = 0;
        targetThrottle = 0.0;
        targetPitch = 0.0;
        targetRoll = 0.0;
        poseUpdateRate = 100;
    }

    ZephyrController::~ZephyrController() {
        this->update_connection_.reset();
        this->rosnode_->shutdown();
        this->queue_.clear();
        this->queue_.disable();
        this->callback_queue_thread_.join();
        delete this->rosnode_;
        for (unsigned i = 0; i < joints_.size(); i++) {
        	delete joints_[i];
        }
        joints_.clear();
    }

    void ZephyrController::Load(physics::ModelPtr _parent, sdf::ElementPtr _sdf) {
        this->model_ = _parent;
        this->world_ = this->model_->GetWorld();

        poseUpdateRate = _sdf->Get<int>("poseUpdateRate");
        // load joints
        sdf::ElementPtr jointControlSDF = _sdf->GetElement("joint_control");
        while (jointControlSDF) {
            JointControl* jointControl = new JointControl();
            jointControl->jointName = jointControlSDF->Get<std::string>("name");
            jointControl->SetJointType(jointControlSDF->Get<std::string>("type"));
            jointControl->joint = this->model_->GetJoint(jointControl->jointName);
            if (jointControl->joint == nullptr) {
            	gzerr << "cannot get joint with name:" << jointControl->jointName << "\n";
            }
            this->SetPIDParams(jointControl, jointControlSDF);
            joints_.push_back(jointControl);
//            gzdbg << "joint added " << jointControl->jointName << " with type:" << jointControl->jointType << "\n";
            jointControlSDF = jointControlSDF->GetNextElement("joint_control");
        }

        // Make sure the ROS node for Gazebo has already been initalized
        if (!ros::isInitialized()) {
            ROS_FATAL_STREAM_NAMED("template", "A ROS node for Gazebo has not been initialized, unable to load plugin. "
                    << "Load the Gazebo system plugin 'libgazebo_ros_api_plugin.so' in the gazebo_ros package)");
            return;
        }

        std::string model_name = _sdf->GetParent()->GetAttribute("name")->GetAsString();
        std::string control_topic_name = model_name + "_control";
        std::string pose_topic_name = model_name + "_pose";

        this->rosnode_ = new ros::NodeHandle(this->robot_namespace_);
//        if (this->control_topic_name_ != "") {
            ros::SubscribeOptions joints_so =
                    ros::SubscribeOptions::create<geometry_msgs::Twist>(
                            control_topic_name, 100, boost::bind(
                                    &ZephyrController::SetControl, this, _1),
                            ros::VoidPtr(), &this->queue_);
            this->control_twist_sub_ = this->rosnode_->subscribe(joints_so);
//        }

        // subscribe for user commands
//        ros::SubscribeOptions user_command_so =
//                            ros::SubscribeOptions::create<std_msgs::String>(
//                                    this->user_command_topic_name_, 100, boost::bind(
//                                            &ZephyrController::ProcessUserCommand, this, _1),
//                                    ros::VoidPtr(), &this->queue_);
//        this->user_command_sub_ = this->rosnode_->subscribe(user_command_so);

        // start custom queue for controller plugin ros topics
        this->callback_queue_thread_ =
                boost::thread(boost::bind(&ZephyrController::QueueThread, this));

        // create the publisher
        this->pose_pub_ = this->rosnode_->advertise<geometry_msgs::Pose>(pose_topic_name, 1);

        // New Mechanism for Updating every World Cycle
        // Listen to the update event. This event is broadcast every
        // simulation iteration.
        this->update_connection_ = event::Events::ConnectWorldUpdateBegin(
                boost::bind(&ZephyrController::UpdateStates, this));
    }

    void ZephyrController::UpdateStates() {
    	boost::mutex::scoped_lock lock(this->update_mutex_);
    	common::Time currTime = this->world_->SimTime();

    	if (this->pose_pub_.getNumSubscribers() > 0) {
    		double dt_ = (currTime - lastPosePublishTime).Double()*1000; // miliseconds
    		if (dt_ > poseUpdateRate) {
    			ignition::math::Pose3d pose = this->model_->WorldPose();
    			geometry_msgs::Pose poseMsg;
    			poseMsg.position.x = pose.Pos().X();
    			poseMsg.position.y = pose.Pos().Y();
    			poseMsg.position.z = pose.Pos().Z();
    			poseMsg.orientation.x = pose.Rot().X();
    			poseMsg.orientation.y = pose.Rot().Y();
    			poseMsg.orientation.z = pose.Rot().Z();
    			poseMsg.orientation.w = pose.Rot().W();
    			this->pose_pub_.publish(poseMsg);
    			lastPosePublishTime = currTime;
    		}
    	}



        if (control_twist_sub_.getNumPublishers() > 0) {
            double dt_ = (currTime - lastUpdateTime).Double();
            if (lastUpdateTime.Double() == 0.0) {
            	dt_ = 0.0;
            }
        	CalculateJoints(targetThrottle, targetPitch, targetRoll, dt_);
        }

        this->lastUpdateTime = currTime;
    }

    void ZephyrController::CalculateJoints(double targetThrottle, double targetPitch, double targetRoll, common::Time dt) {
		ignition::math::Pose3d pose = this->model_->WorldPose();
		double pitch = pose.Rot().Euler().X();
		double roll = pose.Rot().Euler().Y();

		pitch = pitch - targetPitch;
		roll = roll - targetRoll;

		joints_[0]->SetCommand(targetThrottle, dt);
		joints_[1]->SetCommand(pitch - roll, dt);
		joints_[2]->SetCommand(pitch + roll, dt);
    }

    void ZephyrController::SetControl(const geometry_msgs::Twist::ConstPtr& _twist) {
        boost::mutex::scoped_lock lock(this->update_mutex_);
        targetThrottle = _twist->linear.x;
        targetPitch = _twist->angular.y;
        targetRoll = _twist->angular.x;
    }

//    void ZephyrController::ProcessUserCommand(const std_msgs::String::ConstPtr& user_command) {
//    	gzdbg << "processing user command:" << user_command->data << "\n";
////    	planner.ProcessUserCommand(user_command->data);
//    }

    void ZephyrController::QueueThread() {
        static const double timeout = 0.01;
        while (this->rosnode_->ok()) {
            this->queue_.callAvailable(ros::WallDuration(timeout));
        }
    }

    void ZephyrController::SetPIDParams(JointControl* jointControl, sdf::ElementPtr _sdf) {
        if (_sdf->HasElement("p")) {
            double p = _sdf->Get<double>("p");
            double i = _sdf->Get<double>("i");
            double d = _sdf->Get<double>("d");
            double imax = _sdf->Get<double>("imax");
            double imin = _sdf->Get<double>("imin");
            double cmdmax = _sdf->Get<double>("cmdmax");
            double cmdmin = _sdf->Get<double>("cmdmin");
            jointControl->SetPIDParams(p, i, d, imax, imin, cmdmax, cmdmin);
        }
    }
}
