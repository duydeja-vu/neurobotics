#include <neuro_local_planner_wrapper/neuro_local_planner_wrapper.h>
#include <pluginlib/class_list_macros.h>

#include <math.h>

// Register this planner as a BaseLocalPlanner plugin
PLUGINLIB_EXPORT_CLASS(neuro_local_planner_wrapper::NeuroLocalPlannerWrapper, nav_core::BaseLocalPlanner)

namespace neuro_local_planner_wrapper
{
    // Constructor
    // --> Part of interface
    NeuroLocalPlannerWrapper::NeuroLocalPlannerWrapper() : initialized_(false),
                                                 blp_loader_("nav_core", "nav_core::BaseLocalPlanner")
    {
    }

    // Destructor
    // --> Part of interface
    NeuroLocalPlannerWrapper::~NeuroLocalPlannerWrapper()
    {
        tc_.reset();
    }

    // Initialize the planner
    // --> Part of interface
    // name:                some string, not important
    // tf:                  this will tell the planner the robots location (i think)
    // costmap_ros:         the costmap
    // Return:              nothing
    void NeuroLocalPlannerWrapper::initialize(std::string name, tf::TransformListener* tf,
                                         costmap_2d::Costmap2DROS* costmap_ros)
    {
        // If we are not ininialized do so
        if (!initialized_)
        {
            ros::NodeHandle private_nh("~/" + name);

            // Publishers & Subscribers
            g_plan_pub_ = private_nh.advertise<nav_msgs::Path>("global_plan", 1);
            l_plan_pub_ = private_nh.advertise<nav_msgs::Path>("local_plan", 1);

            state_pub_ = private_nh.advertise<std_msgs::Bool>("new_round", 1);

            laser_scan_sub_ = private_nh.subscribe("/scan", 1000, &NeuroLocalPlannerWrapper::getLaserScanPoints, this);

            customized_costmap_pub_ = private_nh.advertise<nav_msgs::OccupancyGrid>("customized_costmap", 1);

            transition_pub_ = private_nh.advertise<neuro_local_planner_wrapper::Transition>("transition", 1);

            marker_array_pub_ = private_nh.advertise<visualization_msgs::MarkerArray>("visualization_marker_array", 1); // to_delete

            actions_sub_ = private_nh.subscribe("action_output", 1000, &NeuroLocalPlannerWrapper::computeVelocityCommands, this);


            // Setup tf
            tf_ = tf;

            // Setup the costmap_ros interface
            costmap_ros_ = costmap_ros;
            costmap_ros_->getRobotPose(current_pose_);

            // Get the actual costmap object
            costmap_ = costmap_ros_->getCostmap();

            is_customized_costmap_initialized_ = false;

            // Should we use the dwa planner?
            existing_plugin_ = false;
            std::string local_planner = "dwa_local_planner/DWAPlannerROS";

            // If we want to, lets load a local planner plugin to do the work for us
            if (existing_plugin_)
            {
                try
                {
                    tc_ = blp_loader_.createInstance(local_planner);
                    ROS_INFO("Created local_planner %s", local_planner.c_str());
                    tc_->initialize(blp_loader_.getName(local_planner), tf, costmap_ros);
                }
                catch (const pluginlib::PluginlibException& ex)
                {
                    ROS_FATAL("Failed to create plugin");
                    exit(1);
                }
            }

            // We are now initialized
            initialized_ = true;
        }
        else
        {
            ROS_WARN("This planner has already been initialized, doing nothing.");
        }
    }

    // Sets the plan
    // --> Part of interface
    // orig_global_plan:    this is the global plan we're supposed to follow (a vector of positions forms the
    //                      line)
    // Return:              True if plan was succesfully received...
    bool NeuroLocalPlannerWrapper::setPlan(const std::vector<geometry_msgs::PoseStamped>& orig_global_plan)
    {

        // Check if the planner has been initialized
        if (!initialized_)
        {
            ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
            return false;
        }

        // Safe the global plan
        global_plan_.clear();
        global_plan_ = orig_global_plan;

        // Set the goal position so we can check if we have arrived or not
        goal_.position.x = orig_global_plan.at(orig_global_plan.size() - 1).pose.position.x;
        goal_.position.y = orig_global_plan.at(orig_global_plan.size() - 1).pose.position.y;

        // If we use the dwa:
        // This code is copied from the dwa_planner
        if (existing_plugin_)
        {
            if (!tc_->setPlan(orig_global_plan))
            {
                ROS_ERROR("Failed to set plan for existing plugin");
                return false;
            }
        }
        return true;
    }

    // Compute the velocity commands
    // --> Part of interface
    // cmd_vel:             fill this vector with our velocity commands (the actual output we're producing)
    // Return:              True if we didn't fail
    bool NeuroLocalPlannerWrapper::computeVelocityCommands(geometry_msgs::Twist& cmd_vel)
    {
        // Should we use the network as a planner or the dwa planner?
        if (!existing_plugin_)
        {
            // Lets drive in circles
            cmd_vel.angular.z = 0.0;
            cmd_vel.linear.x = 0.5;
            return true;
        }
        // Use the existing local planner plugin
        else
        {
            geometry_msgs::Twist cmd;

            if(tc_->computeVelocityCommands(cmd))
            {
                cmd_vel = cmd;
                return true;
            }
            else
            {
                ROS_ERROR("Failed computing a command");
                return false;
            }
        }
    }


    // Tell if goal was reached
    // --> Part of interface
    // Return:              True if goal pose was reached
    bool NeuroLocalPlannerWrapper::isGoalReached()
    {
        // Get current position
        costmap_ros_->getRobotPose(current_pose_);

        // Get distance from position to goal, probably there is a better way to do this
        double dist = sqrt(pow((current_pose_.getOrigin().getX() - goal_.position.x
                                + costmap_->getSizeInMetersX()/2), 2.0)
                           + pow((current_pose_.getOrigin().getY()  - goal_.position.y
                                  + costmap_->getSizeInMetersY()/2), 2.0));

        // More or less an arbitrary number. With above dist calculation this seems to be te best the robot can do...
        if(dist < 0.2)
        {
            ROS_INFO("We made it to the goal!");

            // Publish that a new round can be started with the stage_sim_bot
            std_msgs::Bool new_round;
            new_round.data = true;
            state_pub_.publish(new_round);
            global_plan_.clear();
            return true;
        }
        else
        {
            return false;
        }
    }




    // Callback function for the subscriber to the local costmap
    // costmap:             this is the costmap message
    // Return:              nothing

    /*void NeuroLocalPlannerWrapper::filterCostmap(nav_msgs::OccupancyGrid costmap)
    {
        filtereded_costmap_ = costmap;

        // Get costmap size
        int width = filtereded_costmap_.info.width;
        int height = filtereded_costmap_.info.height;

        // Change the costmap
        for (int i = 0; i < height; i++)
        {
            for (int j = 0; j < width; j++)
            {
                if (filtereded_costmap_.data[i * width + j] < 100)
                {
                    filtereded_costmap_.data[i * width + j] = 50;
                }
                else
                {
                    filtereded_costmap_.data[i * width + j] = 100;
                }
            }
        }

        // Transform the global plan into costmap coordinates
        unsigned int x, y;
        geometry_msgs::PoseStamped pose_fixed_frame; // pose given in fixed frame of global plan which is by default "map"
        geometry_msgs::PoseStamped pose_robot_base_frame; // pose given in global frame of the local cost map

        for(std::vector<geometry_msgs::PoseStamped>::iterator it = global_plan_.begin(); it != global_plan_.end(); it++)
        {
            // Transform pose from fixed frame of global plan to global frame of local cost map
            pose_fixed_frame = *it;
            try
            {
                pose_fixed_frame.header.stamp = costmap.header.stamp;
                tf_->transformPose(costmap.header.frame_id, pose_fixed_frame, pose_robot_base_frame);
            }
            catch (tf::TransformException ex)
            {
                ROS_ERROR("%s",ex.what());
            }

            // Transformtion to costmap coordinates
            if (costmap_->worldToMap(pose_robot_base_frame.pose.position.x, pose_robot_base_frame.pose.position.y, x, y))
            {
                filtereded_costmap_.data[x + y*width] = 0;
            }
        }

        updated_costmap_pub_.publish(filtereded_costmap_);
    }*/


    void NeuroLocalPlannerWrapper::initializeCustomizedCostmap()
    {
        customized_costmap_ = nav_msgs::OccupancyGrid();

        // header
        customized_costmap_.header.frame_id = "/base_footprint";
        customized_costmap_.header.stamp = ros::Time::now();
        customized_costmap_.header.seq = 0;

        // info
        customized_costmap_.info.width = costmap_->getSizeInCellsX(); // e.g. 80
        customized_costmap_.info.height = costmap_->getSizeInCellsY(); // e.g. 80
        customized_costmap_.info.resolution = costmap_->getResolution(); // e.g. 0.05
        customized_costmap_.info.origin.position.x = -costmap_->getSizeInMetersX()/2.0; // e.g.-1.95
        customized_costmap_.info.origin.position.y = -costmap_->getSizeInMetersY()/2.0; // e.g.-1.95
        customized_costmap_.info.origin.position.z = 0.0;
        customized_costmap_.info.origin.orientation.x = 0.0;
        customized_costmap_.info.origin.orientation.y = 0.0;
        customized_costmap_.info.origin.orientation.z = 0.0;
        customized_costmap_.info.origin.orientation.w = 1.0;
        // customized_costmap_.info.map_load_time important?

        // data
        std::vector<int8_t> data(customized_costmap_.info.width*customized_costmap_.info.height,70);
        customized_costmap_.data = data;

        initializeTransitionMsg();
    }

    void NeuroLocalPlannerWrapper::initializeTransitionMsg()
    {
        transition_msg_.width = customized_costmap_.info.width;
        transition_msg_.height = customized_costmap_.info.height;
        transition_msg_.depth = 4; // use four consecutive maps for state representation

        transition_msg_.header.seq = 0;
    }

    /*void NeuroLocalPlannerWrapper::addMarkerToArray(double x, double y, std::string frame, ros::Time stamp) {

        visualization_msgs::Marker marker;

        marker.header.frame_id = frame;
        marker.header.stamp = stamp;

        marker.id = marker_array_.markers.size();

        marker.type = visualization_msgs::Marker::SPHERE;
        marker.action = visualization_msgs::Marker::ADD;

        marker.pose.position.x = x;
        marker.pose.position.y = y;
        marker.pose.position.z = 0;
        marker.pose.orientation.x = 0.0;
        marker.pose.orientation.y = 0.0;
        marker.pose.orientation.z = 0.0;
        marker.pose.orientation.w = 1.0;

        marker.scale.x = 0.05;
        marker.scale.y = 0.05;
        marker.scale.z = 0.05;

        marker.color.r = 0.0f;
        marker.color.g = 1.0f;
        marker.color.b = 0.0f;
        marker.color.a = 1.0;

        marker_array_.markers.push_back(marker);
    }*/

    // Callback function for the subscriber to the laser scan
    // laser_scan:          this is the laser scan message
    // Return:              nothing
    void NeuroLocalPlannerWrapper::getLaserScanPoints(sensor_msgs::LaserScan laser_scan)
    {
        // --- 1. CLEAR COSTMAP/SET ALL PIXEL GRAY ---
        if (!is_customized_costmap_initialized_) // initialize costmap
        {
            initializeCustomizedCostmap();
            is_customized_costmap_initialized_ = true;
        } else { // clear costmap -> set all pixel of costmap to same value e.g. 70
            std::vector<int8_t> data(customized_costmap_.info.width*customized_costmap_.info.height,70);
            customized_costmap_.data = data;
        }

        // --- 2. ADD LASER SCAN POINTS AS INVALID/BLACK PIXEL ---
        // get source frame and target frame of laser scan points
        std::string laser_scan_source_frame = laser_scan.header.frame_id;
        std::string laser_scan_target_frame = customized_costmap_.header.frame_id;

        ros::Time laser_scan_stamp = laser_scan.header.stamp; // stamp of first laser point in range
        ros::Time customized_costmap_stamp = laser_scan_stamp;

        customized_costmap_.header.stamp = customized_costmap_stamp; // update stamp of costmap

        // get transformation between robot base frame and frame of laser scan
        tf::StampedTransform stamped_transform;
        try
        {
            //tf_->lookupTransform(laser_scan_target_frame, customized_costmap_stamp, laser_scan_source_frame, laser_scan_stamp, "/map", stamped_transform);
            //tf_->lookupTransform(laser_scan_target_frame, laser_scan_source_frame, laser_scan_stamp, stamped_transform);
            tf_->lookupTransform(laser_scan_target_frame, laser_scan_source_frame, ros::Time(0), stamped_transform); // ros::Time(0) gives us the latedt availkable transform
        }
        catch (tf::TransformException ex)
        {
            ROS_ERROR("%s",ex.what());
        }

        // marker_array_.markers.clear(); // marker array serves for visualization in rviz and has no functional meaning

        double x_position_laser_scan_frame; // x position of laser scan point in frame of laser scan
        double y_position_laser_scan_frame; // y position of laser scan point in frame of laser scan
        double x_position_robot_base_frame; // x position of laser scan point in robot base frame
        double y_position_robot_base_frame; // y position of laser scan point in robot base frame
        // iteration over all laser scan points
        for(int i = 0; i < laser_scan.ranges.size(); i++)
        {
            if ((laser_scan.ranges.at(i) > laser_scan.range_min) && (laser_scan.ranges.at(i) < laser_scan.range_max))
            {
                // to be precise we would have to get transformation for each laser scan point seperatly but for now we don't:
                // laser_scan_source_stamp = laser_scan_source_stamp + ros::Duration(laser_scan.scan_time); // as robot base is moving laser_scan_source_stamp is different for every laser scan point

                // get x and y coordinates of laser scan point in frame of laser scan, z coordinate is ignored as we are working with a 2D costmap
                x_position_laser_scan_frame = laser_scan.ranges.at(i) * cos(laser_scan.angle_min + i*laser_scan.angle_increment);
                y_position_laser_scan_frame = laser_scan.ranges.at(i) * sin(laser_scan.angle_min + i*laser_scan.angle_increment);

                // translation
                x_position_robot_base_frame = x_position_laser_scan_frame + stamped_transform.getOrigin().getX();
                y_position_robot_base_frame = y_position_laser_scan_frame + stamped_transform.getOrigin().getY();
                // rotation
                double roll, pitch, yaw;
                stamped_transform.getBasis().getRPY(roll, pitch, yaw);
                double x_temp = x_position_robot_base_frame;
                double y_temp = y_position_robot_base_frame;
                x_position_robot_base_frame = cos(yaw)*x_temp - sin(yaw)*y_temp;
                y_position_robot_base_frame = sin(yaw)*x_temp + cos(yaw)*y_temp;

                // visualization
                // addMarkerToArray(x_position_robot_base_frame, y_position_robot_base_frame, laser_scan_target_frame, customized_costmap_stamp);

                // transformation to costmap coordinates
                int x, y;
                x = round(((x_position_robot_base_frame - customized_costmap_.info.origin.position.x)/costmap_->getSizeInMetersX())*customized_costmap_.info.width-0.5);
                y = round(((y_position_robot_base_frame - customized_costmap_.info.origin.position.y)/costmap_->getSizeInMetersY())*customized_costmap_.info.height-0.5);
                if ((x >=0) && (y >=0) && (x < customized_costmap_.info.width) && (y < customized_costmap_.info.height))
                {
                    customized_costmap_.data[x + y*customized_costmap_.info.width] = 100;
                }
            }
        }

        // visualization
        // marker_array_pub_.publish(marker_array_);

        // --- 3. ADD GLOBAL PATH AS WHITE PIXEL ---
        // Transform the global plan into costmap coordinates
        geometry_msgs::PoseStamped pose_fixed_frame; // pose given in fixed frame of global plan which is by default "map"
        geometry_msgs::PoseStamped pose_robot_base_frame; // pose given in global frame of the local cost map

        std::vector<geometry_msgs::Point> global_plan_map_coordinates;
        geometry_msgs::Point a_global_plan_map_coordinate;

        std::vector<geometry_msgs::PoseStamped> global_plan_temp = global_plan_;

        //for(std::vector<geometry_msgs::PoseStamped>::reverse_iterator it = global_plan_.rbegin(); it != global_plan_.rend(); it++)
        for(std::vector<geometry_msgs::PoseStamped>::iterator it = global_plan_temp.begin(); it != global_plan_temp.end(); it++)
        {
            // Transform pose from fixed frame of global plan to global frame of local cost map
            pose_fixed_frame = *it;
            try
            {
                pose_fixed_frame.header.stamp = customized_costmap_.header.stamp;
                tf_->waitForTransform(customized_costmap_.header.frame_id, pose_fixed_frame.header.frame_id,
                                      customized_costmap_.header.stamp, ros::Duration(0.2));
                tf_->transformPose(customized_costmap_.header.frame_id, pose_fixed_frame, pose_robot_base_frame);
            }
            catch (tf::TransformException ex)
            {
                ROS_ERROR("%s",ex.what());
            }

            // transformtion to costmap coordinates
            int x, y;
            x = round(((pose_robot_base_frame.pose.position.x - customized_costmap_.info.origin.position.x)/costmap_->getSizeInMetersX())*customized_costmap_.info.width-0.5);
            y = round(((pose_robot_base_frame.pose.position.y - customized_costmap_.info.origin.position.y)/costmap_->getSizeInMetersY())*customized_costmap_.info.height-0.5);
            if ((x >=0) && (y >=0) && (x < customized_costmap_.info.width) && (y < customized_costmap_.info.height))
            {
                a_global_plan_map_coordinate.x = x;
                a_global_plan_map_coordinate.y = y;

                global_plan_map_coordinates.push_back(a_global_plan_map_coordinate);
            }
        }

        int total_plan_pixel_number = global_plan_map_coordinates.size();
        int counter = 0;
        for(std::vector<geometry_msgs::Point>::iterator it = global_plan_map_coordinates.begin(); it != global_plan_map_coordinates.end(); it++) {
            a_global_plan_map_coordinate = *it;
            customized_costmap_.data[a_global_plan_map_coordinate.x + a_global_plan_map_coordinate.y*customized_costmap_.info.width] = 50 - round((double)counter/(double)(total_plan_pixel_number-1)*50.0);
            counter++;
        }

        // --- 4. PUBLISH CUSTOMIZED COSTMAP
        customized_costmap_pub_.publish(customized_costmap_); // publish costmap
        customized_costmap_.header.seq = customized_costmap_.header.seq + 1; // increment seq for next costmap

        // --- 5. BUFFER WITH CONSECUTIVE COSTMAPS ---
        if (transition_msg_.state_representation.size() == transition_msg_.width*
                                                        transition_msg_.height*
                                                        transition_msg_.depth)
        {
            // publish
            transition_msg_.header.stamp = customized_costmap_.header.stamp;
            transition_msg_.header.frame_id = customized_costmap_.header.frame_id;

            transition_pub_.publish(transition_msg_);
            transition_msg_.header.seq = transition_msg_.header.seq + 1;
            // clear buffer
            transition_msg_.state_representation.clear();
        } else {
            // add to buffer
            transition_msg_.state_representation.insert(transition_msg_.state_representation.end(), customized_costmap_.data.begin(), customized_costmap_.data.end());
        }
    }
};
