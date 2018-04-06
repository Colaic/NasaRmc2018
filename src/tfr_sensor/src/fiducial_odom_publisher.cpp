/**
 * Fiducial odometry publisher, currently it's a quick and dirty test class to
 * get sensor fusion, and navigation up and running. 
 *
 * If the proof of concept is reliable in any way, we will refactor to a more
 * maintainable form. 
 *
 * Functionally it subscribes to a camera topic, feeds that to the fiducial
 * action server, and publishes the relevant Odometry information at a supplied
 * camera_link frame.
 *
 * It only publishes odometry if the fiducial action server is successful.
 *
 * parameters:
 *   ~camera_frame: The reference frame of the camera (string, default="camera_link")
 *   ~footprint_frame: The reference frame of the robot_footprint(string,
 *   default="footprint")
 *   ~bin_frame: The reference frame of the bin (string, default="bin_footprint")
 *   ~odom_frame: The reference frame of odom  (string, default="odom")
 *   ~debug: print debugging info (bool, default: false)
 *   ~rate: how fast to process images
 * subscribed topics:
 *   image (sensor_msgs/Image) - the camera topic
 * published topics:
 *   odom (geometry_msgs/Odometry)- the odometry topic 
 * */
#include <ros/ros.h>
#include <ros/console.h>
#include <nav_msgs/Odometry.h>
#include <geometry_msgs/PoseStamped.h>
#include <tfr_msgs/ArucoAction.h>
#include <tfr_msgs/WrappedImage.h>
#include <tfr_utilities/tf_manipulator.h>
#include <actionlib/client/simple_action_client.h>
#include <tf2/convert.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2/LinearMath/Scalar.h>
#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2_ros/transform_listener.h>

class FiducialOdom
{
    public:
        FiducialOdom(ros::NodeHandle& n, 
                const std::string& c_frame, 
                const std::string& f_frame, 
                const std::string& b_frame,
                const std::string& o_frame,
                bool debugging) :
            aruco{"aruco_action_server", true},
            tf_manipulator{},
            camera_frame{c_frame},
            footprint_frame{f_frame},
            bin_frame{b_frame},
            odometry_frame{o_frame},
            last_pose{},
            debug{debugging}
        {
            rear_cam_client = n.serviceClient<tfr_msgs::WrappedImage>("/on_demand/rear_cam/image_raw");
            kinect_client = n.serviceClient<tfr_msgs::WrappedImage>("/on_demand/kinect/image_raw");
            publisher = n.advertise<nav_msgs::Odometry>("odom", 10 );
            ROS_INFO("Fiducial Odom Publisher Connecting to Server");
            aruco.waitForServer();
            ROS_INFO("Fiducial Odom Publisher Connected to Server");
            //fill transform buffer
            ros::Duration(2).sleep();
            //connect to the image clients
            tfr_msgs::WrappedImage request{};
            while(!rear_cam_client.call(request));
            while(!kinect_client.call(request));
            ROS_INFO("Fiducial Odom Publisher: Connected Image Clients");
        }

        ~FiducialOdom() = default;
        FiducialOdom(const FiducialOdom&) = delete;
        FiducialOdom& operator=(const FiducialOdom&) = delete;
        FiducialOdom(FiducialOdom&&) = delete;
        FiducialOdom& operator=(FiducialOdom&&) = delete;
        void processOdometry()
        {
            tfr_msgs::WrappedImage rear_cam{}, kinect{};
            //grab an image
            rear_cam_client.call(rear_cam);
            kinect_client.call(kinect);

            tfr_msgs::ArucoResultConstPtr result = nullptr;

            tfr_msgs::ArucoGoal goal;
            goal.image = rear_cam.response.image;
            goal.camera_info = rear_cam.response.camera_info;
            //send it to the server
            aruco.sendGoal(goal);
            aruco.waitForResult();
            result = aruco.getResult();

            if (result != nullptr && result->number_found == 0)
            {
                goal.image = kinect.response.image;
                goal.camera_info = kinect.response.camera_info;
                aruco.sendGoal(goal);
                aruco.waitForResult();
                result = aruco.getResult();
            }

            if (result != nullptr && result->number_found !=0)
			{

				geometry_msgs::PoseStamped unprocessed_pose = result->relative_pose;

				if (debug)
					ROS_INFO("unprocessed data %s %f %f %f %f %f %f %f",
							unprocessed_pose.header.frame_id.c_str(),
							unprocessed_pose.pose.position.x,
							unprocessed_pose.pose.position.y,
							unprocessed_pose.pose.position.z,
							unprocessed_pose.pose.orientation.x,
							unprocessed_pose.pose.orientation.y,
							unprocessed_pose.pose.orientation.z,
							unprocessed_pose.pose.orientation.w);

				//transform from camera to footprint perspective
				geometry_msgs::PoseStamped processed_pose;
				if (!tf_manipulator.transform_pose(unprocessed_pose,
							processed_pose, footprint_frame))
					return;
				//note we have to reverse signs here
				processed_pose.pose.position.y *= -1;
                processed_pose.pose.position.z *= -1;
                if (debug)
                    ROS_INFO("processed data %s %f %f %f %f %f %f %f",
                            processed_pose.header.frame_id.c_str(),
                            processed_pose.pose.position.x,
                            processed_pose.pose.position.y,
                            processed_pose.pose.position.z,
                            processed_pose.pose.orientation.x,
                            processed_pose.pose.orientation.y,
                            processed_pose.pose.orientation.z,
                            processed_pose.pose.orientation.w);
                //so we have a point in terms of the footprint and bin

                //we need to express that in terms of odom
                geometry_msgs::Transform relative_bin_transform;
                //get odom bin transform
                if (!tf_manipulator.get_transform(relative_bin_transform,
                            odometry_frame, bin_frame))
                    return;
                if (debug)
                    ROS_INFO("relative transform %f %f %f %f %f %f %f",
                            relative_bin_transform.translation.x,
                            relative_bin_transform.translation.y,
                            relative_bin_transform.translation.z,
                            relative_bin_transform.rotation.x,
                            relative_bin_transform.rotation.y,
                            relative_bin_transform.rotation.z,
                            relative_bin_transform.rotation.w);
                //take a difference of the two transforms to find the

                //odom_camera transform
                tf2::Transform p_0{};
                tf2::convert(processed_pose.pose, p_0);
                tf2::Transform p_1{};
                tf2::convert(relative_bin_transform, p_1);

                //get the difference between the two transforms
                auto difference = p_1.inverseTimes(p_0);
                geometry_msgs::Transform relative_transform{};
                relative_transform = tf2::toMsg(difference);

                relative_transform.translation.x *= -1;
                relative_transform.translation.y *= -1;
                relative_transform.translation.z *= -1;

                //process the odometry
                geometry_msgs::PoseStamped relative_pose;
                relative_pose.header.stamp = unprocessed_pose.header.stamp;
                relative_pose.header.frame_id = camera_frame;
                relative_pose.pose.position.x = relative_transform.translation.x;
                relative_pose.pose.position.y = relative_transform.translation.y;
                relative_pose.pose.position.z = relative_transform.translation.z;
                relative_pose.pose.orientation = relative_transform.rotation;
                if (debug)
                    ROS_INFO("relative data %s %f %f %f %f %f %f %f",
                            relative_pose.header.frame_id.c_str(),
                            relative_pose.pose.position.x,
                            relative_pose.pose.position.y,
                            relative_pose.pose.position.z,
                            relative_pose.pose.orientation.x,
                            relative_pose.pose.orientation.y,
                            relative_pose.pose.orientation.z,
                            relative_pose.pose.orientation.w);

                // handle odometry data
                nav_msgs::Odometry odom;
                odom.header.frame_id = odometry_frame;
                odom.header.stamp = ros::Time::now();
                odom.child_frame_id = footprint_frame;

                //get our pose and fudge some covariances
                odom.pose.pose = relative_pose.pose;
                odom.pose.covariance = {  1e-1,   0,   0,   0,   0,   0,
                    0,1e-1,   0,   0,   0,   0,
                    0,   0,1e-1,   0,   0,   0,
                    0,   0,   0,1e-1,   0,   0,
                    0,   0,   0,   0,1e-1,   0,
                    0,   0,   0,   0,   0,1e-1};


                //handle uninitialized data
                if (    last_pose.pose.orientation.x == 0 &&
                        last_pose.pose.orientation.y == 0 &&
                        last_pose.pose.orientation.z == 0 &&
                        last_pose.pose.orientation.w == 0)
                    last_pose.pose.orientation.w = 1;

                //velocities are harder, we need to take a diffence and do some
                //conversions
                tf2::Transform t_0{};
                tf2::convert(last_pose.pose, t_0);
                tf2::Transform t_1{};
                tf2::convert(relative_pose.pose, t_1);

                /* take fast difference to get linear and angular delta inbetween
                 * timestamps
                 * https://answers.ros.org/question/12654/relative-pose-between-two-tftransforms/
                 */
                auto deltas = t_0.inverseTimes(t_1);
                auto out_deltas = tf2::toMsg(deltas);
                if (debug)
                    ROS_INFO("deltas %f %f %f %f %f %f %f",
                            out_deltas.translation.x,
                            out_deltas.translation.y,
                            out_deltas.translation.z,
                            out_deltas.rotation.x,
                            out_deltas.rotation.y,
                            out_deltas.rotation.z,
                            out_deltas.rotation.w);
                auto linear_deltas = deltas.getOrigin();
                auto angular_deltas = deltas.getRotation();

                //convert from quaternion to rpy for odom compatibility
                tf2::Matrix3x3 converter{};
                converter.setRotation(angular_deltas);
                tf2::Vector3 rpy_deltas{};
                converter.getRPY(rpy_deltas[0], rpy_deltas[1], rpy_deltas[2]); 

                const tf2Scalar delta_t{
                    relative_pose.header.stamp.toSec() - last_pose.header.stamp.toSec()};

                odom.twist.twist.linear =  tf2::toMsg(linear_deltas/delta_t);
                odom.twist.twist.angular = tf2::toMsg(rpy_deltas/delta_t);

                odom.twist.covariance = {  1e-1,   0,   0,   0,   0,   0,
                    0,1e-1,   0,   0,   0,   0,
                    0,   0,1e-1,   0,   0,   0,
                    0,   0,   0,1e-1,   0,   0,
                    0,   0,   0,   0,1e-1,   0,
                    0,   0,   0,   0,   0,1e-1};

                //fire it off! and cleanup
                publisher.publish(odom);
                last_pose = relative_pose;
            }
        }

    private:
        ros::Publisher publisher;
        ros::ServiceClient rear_cam_client;
        ros::ServiceClient kinect_client;
        actionlib::SimpleActionClient<tfr_msgs::ArucoAction> aruco;
        TfManipulator tf_manipulator;

        geometry_msgs::PoseStamped last_pose;

        const std::string& camera_frame;
        const std::string& footprint_frame;
        const std::string& bin_frame;
        const std::string& odometry_frame;
        bool debug;
};

int main(int argc, char** argv)
{
    ros::init(argc, argv, "fiducial_odom_publisher");
    ros::NodeHandle n{};

    std::string camera_frame, footprint_frame, bin_frame, odometry_frame;
    bool debug;
    double rate;
    ros::param::param<std::string>("~camera_frame", camera_frame, "camera_link");
    ros::param::param<std::string>("~footprint_frame", footprint_frame, "footprint");
    ros::param::param<std::string>("~bin_frame", bin_frame, "bin_footprint");
    ros::param::param<std::string>("~odometry_frame", odometry_frame, "odom");
    ros::param::param<double>("~rate",rate, 5);
    ros::param::param<bool>("~debug",debug, false);

    FiducialOdom fiducial_odom{n, camera_frame,footprint_frame, bin_frame,
        odometry_frame, debug};

    ros::Rate r(rate);
    while(ros::ok())
    {
        fiducial_odom.processOdometry();
        ros::spinOnce();
        r.sleep();
    }

    return 0;
}
