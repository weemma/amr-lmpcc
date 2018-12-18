
//This file containts read parameter from server, callback, call class objects, control all class, objects of all class

#include <lmpcc/lmpcc_controller.h>

ACADOvariables acadoVariables;
ACADOworkspace acadoWorkspace;

LMPCC::~LMPCC()
{
    clearDataMember();
}

void LMPCC::spinNode()
{
    ROS_INFO(" lmpcc node is running, now it's 'Spinning Node'");
    ros::spin();
}

// disallocated memory
void LMPCC::clearDataMember()
{
    last_position_ = Eigen::VectorXd(3);
    last_velocity_ = Eigen::VectorXd(3);

}

// initialize all helper class of predictive control and subscibe joint state and publish controlled joint velocity
bool LMPCC::initialize()
{
    // make sure node is still running
    if (ros::ok())
    {
        // initialize parameter configuration class
        lmpcc_config_.reset(new LMPCC_configuration());
        bool lmpcc_config_success = lmpcc_config_->initialize();

        if (lmpcc_config_success == false)
         {
            ROS_ERROR("LMPCC: FAILED TO INITIALIZE!!");
            std::cout << "States: \n"
                                << " pd_config: " << std::boolalpha << lmpcc_config_success << "\n"
                                << " pd config init success: " << std::boolalpha << lmpcc_config_->initialize_success_
                                << std::endl;
            return false;
        }


        // Check if all reference vectors are of the same length
        if (!( (lmpcc_config_->ref_x_.size() == lmpcc_config_->ref_y_.size()) && ( lmpcc_config_->ref_x_.size() == lmpcc_config_->ref_theta_.size() ) && (lmpcc_config_->ref_y_.size() == lmpcc_config_->ref_theta_.size()) ))
        {
            ROS_ERROR("Reference path inputs should be of equal length");
        }

        //Controller options
        enable_output_ = lmpcc_config_->activate_output_;
        n_iterations_ = lmpcc_config_->max_num_iteration_;

        /** Initialize reconfigurable parameters **/
        cost_contour_weight_factors_ = transformStdVectorToEigenVector(lmpcc_config_->contour_weight_factors_);
        cost_control_weight_factors_ = transformStdVectorToEigenVector(lmpcc_config_->control_weight_factors_);

        slack_weight_ = lmpcc_config_->slack_weight_;
        repulsive_weight_ = lmpcc_config_->repulsive_weight_;
        reference_velocity_ = lmpcc_config_->reference_velocity_;
        collision_free_delta_max_ = lmpcc_config_->delta_max_;
        occupied_threshold_ = lmpcc_config_->occupied_threshold_;
        n_search_points_ = lmpcc_config_->n_search_points_;
        window_size_ = lmpcc_config_->search_window_size_;

        /** Set task flags and counters **/
        tracking_ = true;
        move_action_result_.reach = false;
        idx = 1;
        goal_reached_ = false;              // Flag for reaching the goal
        last_poly_ = false;                 // Flag for last segment
        segment_counter = 0;                          // Initialize reference path segment counter

        // resize position vectors
        current_state_ = Eigen::Vector3d(0,0,0);
        last_state_ = Eigen::Vector3d(0,0,0);
        goal_pose_ = Eigen::Vector3d(0,0,0);
        goal_pose_.setZero();

        // DEBUG
        if (lmpcc_config_->activate_debug_output_)
        {
            ROS_WARN("===== DEBUG INFO ACTIVATED =====");
        }

        if (ACADO_N != lmpcc_config_->discretization_intervals_)
        {
            ROS_WARN("Number of discretization steps differs from generated OCP");
        }

        /** Control output topic **/
        if (lmpcc_config_->simulation_mode_)
        {
            cmd_topic_ = lmpcc_config_->cmd_sim_;
        }
        else
        {
            cmd_topic_ = lmpcc_config_->cmd_;
        }

        /** ROS MoveIt! interfaces **/
        static const std::string MOVE_ACTION_NAME = "move_action";
        move_action_server_.reset(new actionlib::SimpleActionServer<lmpcc::moveAction>(nh, MOVE_ACTION_NAME, false));
        move_action_server_->registerGoalCallback(boost::bind(&LMPCC::moveGoalCB, this));
        move_action_server_->registerPreemptCallback(boost::bind(&LMPCC::movePreemptCB, this));
        move_action_server_->start();

	    static const std::string MOVEIT_ACTION_NAME = "fake_base_controller";
	    moveit_action_server_.reset(new actionlib::SimpleActionServer<lmpcc::trajAction>(nh, MOVEIT_ACTION_NAME, false));
	    moveit_action_server_->registerGoalCallback(boost::bind(&LMPCC::moveitGoalCB, this));
	    moveit_action_server_->start();

	    /** Subscribers **/
        robot_state_sub_ = nh.subscribe(lmpcc_config_->robot_state_, 1, &LMPCC::StateCallBack, this);
        obstacle_feed_sub_ = nh.subscribe(lmpcc_config_->ellipse_objects_feed_, 1, &LMPCC::ObstacleCallBack, this);

        /** Publishers **/
        controlled_velocity_pub_ = nh.advertise<geometry_msgs::Twist>(cmd_topic_,1);
		pred_cmd_pub_ = nh.advertise<nav_msgs::Path>("predicted_cmd",1);
		cost_pub_ = nh.advertise<std_msgs::Float64>("cost",1);
        contour_error_pub_ = nh.advertise<std_msgs::Float64MultiArray>("contour_error",1);
		feedback_pub_ = nh.advertise<lmpcc_msgs::lmpcc_feedback>("controller_feedback",1);

        /** Services **/
        map_service_ = nh.serviceClient<nav_msgs::GetMap>("static_map");
        update_trigger = nh.serviceClient<std_srvs::Empty>("update_trigger");

		ros::Duration(1).sleep();

		/** Set timer for control loop **/
        timer_ = nh.createTimer(ros::Duration((double)1/lmpcc_config_->controller_frequency_), &LMPCC::controlLoop, this);
        timer_.start();

        /** Initialize received moveit planned trajectory **/
		moveit_msgs::RobotTrajectory j;
		traj = j;

        /** Setting up dynamic_reconfigure server for the LmpccConfig parameters **/
        ros::NodeHandle nh_lmpcc("lmpcc");
        reconfigure_server_.reset(new dynamic_reconfigure::Server<lmpcc::LmpccConfig>(reconfig_mutex_, nh_lmpcc));
        reconfigure_server_->setCallback(boost::bind(&LMPCC::reconfigureCallback, this, _1, _2));

	    /** Initialize obstacle positions over the time horizon **/
        pred_traj_.poses.resize(ACADO_N);
        pred_cmd_.poses.resize(ACADO_N);
        obstacles_.lmpcc_obstacles.resize(lmpcc_config_->n_obstacles_);

        for (int obst_it = 0; obst_it < lmpcc_config_->n_obstacles_; obst_it++)
        {
            obstacles_.lmpcc_obstacles[obst_it].trajectory.poses.resize(ACADO_N);

            for(int i=0;i < ACADO_N; i++)
            {
                obstacles_.lmpcc_obstacles[obst_it].trajectory.poses[i].pose.position.x = current_state_(0) - 100;
                obstacles_.lmpcc_obstacles[obst_it].trajectory.poses[i].pose.position.y = 0;
                obstacles_.lmpcc_obstacles[obst_it].trajectory.poses[i].pose.orientation.z = 0;
            }
        }

        pred_traj_.header.frame_id = lmpcc_config_->planning_frame_;
        for(int i=0;i < ACADO_N; i++)
        {
            pred_traj_.poses[i].header.frame_id = lmpcc_config_->planning_frame_;
        }

		computeEgoDiscs();

		// Initialize OCP solver
        acado_initializeSolver( );

        collision_free_C1.resize(ACADO_N);
        collision_free_C2.resize(ACADO_N);
        collision_free_C3.resize(ACADO_N);
        collision_free_C4.resize(ACADO_N);

        collision_free_a1x.resize(ACADO_N);
        collision_free_a1y.resize(ACADO_N);
        collision_free_a2x.resize(ACADO_N);
        collision_free_a2y.resize(ACADO_N);
        collision_free_a3x.resize(ACADO_N);
        collision_free_a3y.resize(ACADO_N);
        collision_free_a4x.resize(ACADO_N);
        collision_free_a4y.resize(ACADO_N);

        collision_free_xmin.resize(ACADO_N);
        collision_free_xmax.resize(ACADO_N);
        collision_free_ymin.resize(ACADO_N);
        collision_free_ymax.resize(ACADO_N);

		// Initialize global reference path
        referencePath.SetGlobalPath(lmpcc_config_->ref_x_, lmpcc_config_->ref_y_, lmpcc_config_->ref_theta_);

        if (lmpcc_config_->activate_debug_output_) {
            referencePath.PrintGlobalPath();    // Print global reference path
        }

        if (lmpcc_config_->activate_visualization_) {
            initialize_visuals();
        }

		ROS_WARN("PREDICTIVE CONTROL INTIALIZED!!");
		return true;
	}
	else
	{
		ROS_ERROR("LMPCC: Failed to initialize as ROS Node is shoutdown");
		return false;
	}
}

bool LMPCC::initialize_visuals()
{
    robot_collision_space_pub_ = nh.advertise<visualization_msgs::MarkerArray>("/robot_collision_space", 100);
    pred_traj_pub_ = nh.advertise<nav_msgs::Path>("predicted_trajectory",1);
    global_plan_pub_ = nh.advertise<visualization_msgs::MarkerArray>("global_plan",1);
    collision_free_pub_ = nh.advertise<visualization_msgs::MarkerArray>("collision_free_area",1);
    traj_pub_ = nh.advertise<visualization_msgs::MarkerArray>("pd_trajectory",1);

    /** Initialize local reference path segment publishers **/
    local_spline_traj_pub1_ = nh.advertise<nav_msgs::Path>("reference_trajectory_seg1",1);
    local_spline_traj_pub2_ = nh.advertise<nav_msgs::Path>("reference_trajectory_seg2",1);
    local_spline_traj_pub3_ = nh.advertise<nav_msgs::Path>("reference_trajectory_seg3",1);
    local_spline_traj_pub4_ = nh.advertise<nav_msgs::Path>("reference_trajectory_seg4",1);
    local_spline_traj_pub5_ = nh.advertise<nav_msgs::Path>("reference_trajectory_seg5",1);

    //initialize trajectory variable to plot prediction trajectory
    local_spline_traj1_.poses.resize(50);
    local_spline_traj2_.poses.resize(50);
    local_spline_traj3_.poses.resize(50);
    local_spline_traj4_.poses.resize(50);
    local_spline_traj5_.poses.resize(50);

    /** Initialize visualization markers **/
    ellips1.type = visualization_msgs::Marker::CYLINDER;
    ellips1.id = 60;
    ellips1.color.b = 1.0;
    ellips1.color.a = 0.5;
    ellips1.header.frame_id = lmpcc_config_->planning_frame_;
    ellips1.ns = "trajectory";
    ellips1.action = visualization_msgs::Marker::ADD;
    ellips1.lifetime = ros::Duration(0.1);
    ellips1.scale.x = r_discs_*2.0;
    ellips1.scale.y = r_discs_*2.0;
    ellips1.scale.z = 0.05;

    cube1.type = visualization_msgs::Marker::CUBE;
    cube1.id = 60;
    cube1.color.r = 0.5;
    cube1.color.g = 0.5;
    cube1.color.b = 0.0;
    cube1.color.a = 0.1;
    cube1.header.frame_id = lmpcc_config_->planning_frame_;
    cube1.ns = "trajectory";
    cube1.action = visualization_msgs::Marker::ADD;
    cube1.lifetime = ros::Duration(0.1);

    global_plan.type = visualization_msgs::Marker::CYLINDER;
    global_plan.id = 800;
    global_plan.color.r = 0.8;
    global_plan.color.g = 0.0;
    global_plan.color.b = 0.0;
    global_plan.color.a = 0.8;
    global_plan.header.frame_id = lmpcc_config_->planning_frame_;
    global_plan.ns = "trajectory";
    global_plan.action = visualization_msgs::Marker::ADD;
    global_plan.lifetime = ros::Duration(0);
    global_plan.scale.x = 0.1;
    global_plan.scale.y = 0.1;
    global_plan.scale.z = 0.05;

}

void LMPCC::reconfigureCallback(lmpcc::LmpccConfig& config, uint32_t level){

    if (lmpcc_config_->activate_debug_output_) {
        ROS_INFO("Reconfigure callback");
    }

    cost_contour_weight_factors_(0) = config.Wcontour;
    cost_contour_weight_factors_(1) = config.Wlag;
    cost_control_weight_factors_(0) = config.Kv;
    cost_control_weight_factors_(1) = config.Kw;

    slack_weight_= config.Ws;
    repulsive_weight_ = config.WR;

    reference_velocity_ = config.vRef;
    collision_free_delta_max_ = config.deltaMax;
    occupied_threshold_ = config.occThres;

    enable_output_ = config.enable_output;
    loop_mode_ = config.loop_mode;
    n_iterations_ = config.n_iterations;

    //Search window parameters
    window_size_ = config.window_size;
    n_search_points_ = config.n_search_points;
}

void LMPCC::computeEgoDiscs()
{
    // Collect parameters for disc representation
    int n_discs = lmpcc_config_->n_discs_;
    double length = lmpcc_config_->ego_l_;
    double width = lmpcc_config_->ego_w_;

    // Initialize positions of discs
    x_discs_.resize(n_discs);

    // Loop over discs and assign positions
    for ( int discs_it = 0; discs_it < n_discs; discs_it++){
        x_discs_[discs_it] = -length/2 + (discs_it + 1)*(length/(n_discs + 1));
    }

    // Compute radius of the discs
    r_discs_ = sqrt(pow(x_discs_[n_discs - 1] - length/2,2) + pow(width/2,2));
    ROS_WARN_STREAM("Generated " << n_discs <<  " ego-vehicle discs with radius " << r_discs_ );
}

void LMPCC::broadcastPathPose(){

	geometry_msgs::TransformStamped transformStamped;
	transformStamped.header.stamp = ros::Time::now();
	transformStamped.header.frame_id = lmpcc_config_->planning_frame_;
	transformStamped.child_frame_id = "path";

	transformStamped.transform.translation.x = referencePath.ref_path_x(acadoVariables.x[3]);
	transformStamped.transform.translation.y = referencePath.ref_path_y(acadoVariables.x[3]);
	transformStamped.transform.translation.z = 0.0;
	tf::Quaternion q = tf::createQuaternionFromRPY(0, 0, pred_traj_.poses[1].pose.orientation.z);
	transformStamped.transform.rotation.x = 0;
	transformStamped.transform.rotation.y = 0;
	transformStamped.transform.rotation.z = 0;
	transformStamped.transform.rotation.w = 1;

	path_pose_pub_.sendTransform(transformStamped);
}

void LMPCC::broadcastTF(){

	geometry_msgs::TransformStamped transformStamped;
	transformStamped.header.stamp = ros::Time::now();
	transformStamped.header.frame_id = lmpcc_config_->planning_frame_;
	transformStamped.child_frame_id = lmpcc_config_->robot_base_link_;

	if(!enable_output_){
		transformStamped.transform.translation.x = current_state_(0);
		transformStamped.transform.translation.y = current_state_(1);
		transformStamped.transform.translation.z = 0.0;
		tf::Quaternion q = tf::createQuaternionFromRPY(0, 0, pred_traj_.poses[1].pose.orientation.z);
		transformStamped.transform.rotation.x = q.x();
		transformStamped.transform.rotation.y = q.y();
		transformStamped.transform.rotation.z = q.z();
		transformStamped.transform.rotation.w = q.w();
	}

	else{
		transformStamped.transform.translation.x = pred_traj_.poses[1].pose.position.x;
		transformStamped.transform.translation.y = pred_traj_.poses[1].pose.position.y;
		transformStamped.transform.translation.z = 0.0;

		tf::Quaternion q = tf::createQuaternionFromRPY(0, 0, pred_traj_.poses[1].pose.orientation.z);
		transformStamped.transform.rotation.x = q.x();
		transformStamped.transform.rotation.y = q.y();
		transformStamped.transform.rotation.z = q.z();
		transformStamped.transform.rotation.w = q.w();
	}

	state_pub_.sendTransform(transformStamped);

	sensor_msgs::JointState empty;
	empty.position.resize(4);
	empty.name ={"front_left_wheel", "front_right_wheel", "rear_left_wheel", "rear_right_wheel"};
	empty.header.stamp = ros::Time::now();
}

// update this function 1/controller_frequency
void LMPCC::controlLoop(const ros::TimerEvent &event)
{
    int N_iter;
    acado_timer t;
    acado_tic( &t );

    acado_initializeSolver( );

    int trajectory_length = traj.multi_dof_joint_trajectory.points.size();
	if(!lmpcc_config_->gazebo_simulation_)
		broadcastTF();

    if (trajectory_length>0) {
        acadoVariables.x[0] = current_state_(0);
        acadoVariables.x[1] = current_state_(1);
        acadoVariables.x[2] = current_state_(2);
        acadoVariables.x[4] = 0.0000001;          //dummy state
        acadoVariables.x[5] = 0.0000001;          //dummy state

        acadoVariables.u[0] = controlled_velocity_.linear.x;
        acadoVariables.u[1] = controlled_velocity_.angular.z;
        acadoVariables.u[2] = 0.0000001;           //slack variable
        acadoVariables.u[3] = 0.0000001;           //slack variable

		if(acadoVariables.x[3] > ss[2]) {

		    if (segment_counter + lmpcc_config_->n_local_ > referencePath.GlobalPathLenght()*lmpcc_config_->n_poly_per_clothoid_){
		        goal_reached_ = true;
                ROS_ERROR_STREAM("GOAL REACHED");
                if (loop_mode_)
                {
                    segment_counter = 0;
                    goal_reached_ = false;
                    last_poly_ = false;
                    acadoVariables.x[3] = referencePath.GetS0();
                    ROS_ERROR_STREAM("LOOP STARTED");
                }
            } else{
			    segment_counter++;
                referencePath.UpdateLocalRefPath(segment_counter, ss, xx, yy, vv);
                acadoVariables.x[3] = referencePath.GetS0();
                ROS_ERROR_STREAM("SWITCH SPLINE, segment_counter =  " << segment_counter);
		    }
        }

        ComputeCollisionFreeArea();

        if(idx == 1) {
            double smin;
            smin = referencePath.ClosestPointOnPath(current_state_, ss[1], 100, acadoVariables.x[3], window_size_, n_search_points_);
            acadoVariables.x[3] = smin;
        }
        else
            acadoVariables.x[3] = acadoVariables.x[3];


        for (N_iter = 0; N_iter < ACADO_N; N_iter++) {

            // Initialize Online Data variables
            acadoVariables.od[(ACADO_NOD * N_iter) + 0 ] = referencePath.ref_path_x.m_a[1];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 1 ] = referencePath.ref_path_x.m_b[1];
            acadoVariables.od[(ACADO_NOD * N_iter) + 2 ] = referencePath.ref_path_x.m_c[1];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 3 ] = referencePath.ref_path_x.m_d[1];
            acadoVariables.od[(ACADO_NOD * N_iter) + 4 ] = referencePath.ref_path_y.m_a[1];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 5 ] = referencePath.ref_path_y.m_b[1];
            acadoVariables.od[(ACADO_NOD * N_iter) + 6 ] = referencePath.ref_path_y.m_c[1];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 7 ] = referencePath.ref_path_y.m_d[1];

            acadoVariables.od[(ACADO_NOD * N_iter) + 8 ] = referencePath.ref_path_x.m_a[2];         // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 9 ] = referencePath.ref_path_x.m_b[2];
            acadoVariables.od[(ACADO_NOD * N_iter) + 10] = referencePath.ref_path_x.m_c[2];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 11] = referencePath.ref_path_x.m_d[2];
            acadoVariables.od[(ACADO_NOD * N_iter) + 12] = referencePath.ref_path_y.m_a[2];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 13] = referencePath.ref_path_y.m_b[2];
            acadoVariables.od[(ACADO_NOD * N_iter) + 14] = referencePath.ref_path_y.m_c[2];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 15] = referencePath.ref_path_y.m_d[2];

            acadoVariables.od[(ACADO_NOD * N_iter) + 16] = referencePath.ref_path_x.m_a[3];         // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 17] = referencePath.ref_path_x.m_b[3];
            acadoVariables.od[(ACADO_NOD * N_iter) + 18] = referencePath.ref_path_x.m_c[3];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 19] = referencePath.ref_path_x.m_d[3];
            acadoVariables.od[(ACADO_NOD * N_iter) + 20] = referencePath.ref_path_y.m_a[3];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 21] = referencePath.ref_path_y.m_b[3];
            acadoVariables.od[(ACADO_NOD * N_iter) + 22] = referencePath.ref_path_y.m_c[3];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 23] = referencePath.ref_path_y.m_d[3];

            acadoVariables.od[(ACADO_NOD * N_iter) + 24] = referencePath.ref_path_x.m_a[4];         // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 25] = referencePath.ref_path_x.m_b[4];
            acadoVariables.od[(ACADO_NOD * N_iter) + 26] = referencePath.ref_path_x.m_c[4];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 27] = referencePath.ref_path_x.m_d[4];
            acadoVariables.od[(ACADO_NOD * N_iter) + 28] = referencePath.ref_path_y.m_a[4];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 29] = referencePath.ref_path_y.m_b[4];
            acadoVariables.od[(ACADO_NOD * N_iter) + 30] = referencePath.ref_path_y.m_c[4];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 31] = referencePath.ref_path_y.m_d[4];

            acadoVariables.od[(ACADO_NOD * N_iter) + 32] = referencePath.ref_path_x.m_a[5];         // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 33] = referencePath.ref_path_x.m_b[5];
            acadoVariables.od[(ACADO_NOD * N_iter) + 34] = referencePath.ref_path_x.m_c[5];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 35] = referencePath.ref_path_x.m_d[5];
            acadoVariables.od[(ACADO_NOD * N_iter) + 36] = referencePath.ref_path_y.m_a[5];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 37] = referencePath.ref_path_y.m_b[5];
            acadoVariables.od[(ACADO_NOD * N_iter) + 38] = referencePath.ref_path_y.m_c[5];        // spline coefficients
            acadoVariables.od[(ACADO_NOD * N_iter) + 39] = referencePath.ref_path_y.m_d[5];

            acadoVariables.od[(ACADO_NOD * N_iter) + 40] = cost_contour_weight_factors_(0);       // weight factor on contour error
            acadoVariables.od[(ACADO_NOD * N_iter) + 41] = cost_contour_weight_factors_(1);       // weight factor on lag error
            acadoVariables.od[(ACADO_NOD * N_iter) + 42] = cost_control_weight_factors_(0);      // weight factor on theta
            acadoVariables.od[(ACADO_NOD * N_iter) + 43] = cost_control_weight_factors_(1);      // weight factor on v

            acadoVariables.od[(ACADO_NOD * N_iter) + 44 ] = ss[1];
            acadoVariables.od[(ACADO_NOD * N_iter) + 45 ] = ss[2];
            acadoVariables.od[(ACADO_NOD * N_iter) + 46 ] = ss[3];
            acadoVariables.od[(ACADO_NOD * N_iter) + 47 ] = ss[4];
            acadoVariables.od[(ACADO_NOD * N_iter) + 48 ] = ss[5];

            acadoVariables.od[(ACADO_NOD * N_iter) + 49] = vv[0]*reference_velocity_;
            acadoVariables.od[(ACADO_NOD * N_iter) + 50] = vv[1]*reference_velocity_;
            acadoVariables.od[(ACADO_NOD * N_iter) + 51] = vv[2]*reference_velocity_;
            acadoVariables.od[(ACADO_NOD * N_iter) + 52] = vv[3]*reference_velocity_;
            acadoVariables.od[(ACADO_NOD * N_iter) + 53] = vv[4]*reference_velocity_;

            acadoVariables.od[(ACADO_NOD * N_iter) + 54] = ss[2] + 0.02;
            acadoVariables.od[(ACADO_NOD * N_iter) + 55] = ss[3] + 0.02;
            acadoVariables.od[(ACADO_NOD * N_iter) + 56] = ss[4] + 0.02;
            acadoVariables.od[(ACADO_NOD * N_iter) + 57] = ss[5] + 0.02;

            acadoVariables.od[(ACADO_NOD * N_iter) + 58] = slack_weight_;        // weight on the slack variable
            acadoVariables.od[(ACADO_NOD * N_iter) + 59] = repulsive_weight_;    // weight on the repulsive cost

            acadoVariables.od[(ACADO_NOD * N_iter) + 62] = obstacles_.lmpcc_obstacles[0].trajectory.poses[N_iter].pose.position.x;      // x position of obstacle 1
            acadoVariables.od[(ACADO_NOD * N_iter) + 63] = obstacles_.lmpcc_obstacles[0].trajectory.poses[N_iter].pose.position.y;      // y position of obstacle 1
            acadoVariables.od[(ACADO_NOD * N_iter) + 64] = obstacles_.lmpcc_obstacles[0].trajectory.poses[N_iter].pose.orientation.z;   // heading of obstacle 1
            acadoVariables.od[(ACADO_NOD * N_iter) + 65] = obstacles_.lmpcc_obstacles[0].major_semiaxis;                                // major semiaxis of obstacle 1
            acadoVariables.od[(ACADO_NOD * N_iter) + 66] = obstacles_.lmpcc_obstacles[0].minor_semiaxis;                                // minor semiaxis of obstacle 1

            acadoVariables.od[(ACADO_NOD * N_iter) + 67] = obstacles_.lmpcc_obstacles[1].trajectory.poses[N_iter].pose.position.x;      // x position of obstacle 2
            acadoVariables.od[(ACADO_NOD * N_iter) + 68] = obstacles_.lmpcc_obstacles[1].trajectory.poses[N_iter].pose.position.y;      // y position of obstacle 2
            acadoVariables.od[(ACADO_NOD * N_iter) + 69] = obstacles_.lmpcc_obstacles[1].trajectory.poses[N_iter].pose.orientation.z;   // heading of obstacle 2
            acadoVariables.od[(ACADO_NOD * N_iter) + 70] = obstacles_.lmpcc_obstacles[1].major_semiaxis;                                // major semiaxis of obstacle 2
            acadoVariables.od[(ACADO_NOD * N_iter) + 71] = obstacles_.lmpcc_obstacles[1].minor_semiaxis;                                // minor semiaxis of obstacle 2

            acadoVariables.od[(ACADO_NOD * N_iter) + 72] = collision_free_xmin[N_iter];
            acadoVariables.od[(ACADO_NOD * N_iter) + 73] = collision_free_xmax[N_iter];
            acadoVariables.od[(ACADO_NOD * N_iter) + 74] = collision_free_ymin[N_iter];
            acadoVariables.od[(ACADO_NOD * N_iter) + 75] = collision_free_ymax[N_iter];

            acadoVariables.od[(ACADO_NOD * N_iter) + 76] = collision_free_a1x[N_iter];
            acadoVariables.od[(ACADO_NOD * N_iter) + 77] = collision_free_a2x[N_iter];
            acadoVariables.od[(ACADO_NOD * N_iter) + 78] = collision_free_a3x[N_iter];
            acadoVariables.od[(ACADO_NOD * N_iter) + 79] = collision_free_a4x[N_iter];

            acadoVariables.od[(ACADO_NOD * N_iter) + 80] = collision_free_a1y[N_iter];
            acadoVariables.od[(ACADO_NOD * N_iter) + 81] = collision_free_a2y[N_iter];
            acadoVariables.od[(ACADO_NOD * N_iter) + 82] = collision_free_a3y[N_iter];
            acadoVariables.od[(ACADO_NOD * N_iter) + 83] = collision_free_a4y[N_iter];

            acadoVariables.od[(ACADO_NOD * N_iter) + 84] = collision_free_C1[N_iter];
            acadoVariables.od[(ACADO_NOD * N_iter) + 85] = collision_free_C2[N_iter];
            acadoVariables.od[(ACADO_NOD * N_iter) + 86] = collision_free_C3[N_iter];
            acadoVariables.od[(ACADO_NOD * N_iter) + 87] = collision_free_C4[N_iter];
}

        acadoVariables.x0[ 0 ] = current_state_(0);
        acadoVariables.x0[ 1 ] = current_state_(1);
        acadoVariables.x0[ 2 ] = current_state_(2);
		acadoVariables.x0[ 3 ] = acadoVariables.x[3];
        acadoVariables.x0[ 4 ] = 0.0000001;             //dummy state
        acadoVariables.x0[ 5 ] = 0.0000001;             //dummy state

        acado_preparationStep();

        acado_feedbackStep();

//        printf("\tReal-Time Iteration:  KKT Tolerance = %.3e\n\n", acado_getKKT());

		int j=1;
        while (acado_getKKT() > 1e-3 && j < n_iterations_){ //  && acado_getKKT() < 100

			acado_preparationStep();

            acado_feedbackStep();

//            printf("\tReal-Time Iteration:  KKT Tolerance = %.3e\n\n", acado_getKKT());

            j++;    //        acado_printDifferentialVariables();
        }

        te_ = acado_toc(&t);

		controlled_velocity_.linear.x = acadoVariables.u[0];
		controlled_velocity_.angular.z = acadoVariables.u[1];

		publishPredictedOutput();
		publishLocalRefPath();
		broadcastPathPose();
        publishContourError();
		cost_.data = acado_getObjective();
		publishCost();

        if (lmpcc_config_->activate_visualization_)
        {
            publishPredictedTrajectory();
            publishPredictedCollisionSpace();
            publishPosConstraint();
        }

		if (lmpcc_config_->activate_feedback_message_)
        {
            publishFeedback(j,te_);
        }


        if (lmpcc_config_->activate_timing_output_)
    		ROS_INFO_STREAM("Solve time " << te_ * 1e6 << " us");

    // publish zero controlled velocity
        if (!tracking_)
        {
            actionSuccess();
        }
	}
    if(!enable_output_ || acado_getKKT() > 1e-3) {
		publishZeroJointVelocity();
		idx = 2; // used to keep computation of the mpc and did not let it move because we set the initial state as the predicted state
	}
	else {
		idx=1;
		controlled_velocity_pub_.publish(controlled_velocity_);
        update_trigger.call(emptyCall);
	}

}

void LMPCC::moveGoalCB()
{
//    ROS_INFO("MOVEGOALCB");
    if(move_action_server_->isNewGoalAvailable())
    {
        boost::shared_ptr<const lmpcc::moveGoal> move_action_goal_ptr = move_action_server_->acceptNewGoal();
        tracking_ = false;

        //erase previous trajectory
        for (auto it = traj_marker_array_.markers.begin(); it != traj_marker_array_.markers.end(); ++it)
        {
            it->action = visualization_msgs::Marker::DELETE;
            traj_pub_.publish(traj_marker_array_);
        }

        traj_marker_array_.markers.clear();
    }
}

void LMPCC::moveitGoalCB()
{
    ROS_INFO_STREAM("Got new MoveIt goal!!!");

    //Reset trajectory index
    idx = 1;

    if(moveit_action_server_->isNewGoalAvailable())
    {
        boost::shared_ptr<const lmpcc::trajGoal> moveit_action_goal_ptr = moveit_action_server_->acceptNewGoal();
        traj = moveit_action_goal_ptr->trajectory;
        tracking_ = false;

        /** Set goal pose **/
        int trajectory_length = traj.multi_dof_joint_trajectory.points.size();
        goal_pose_(0) = traj.multi_dof_joint_trajectory.points[trajectory_length - 1].transforms[0].translation.x;
        goal_pose_(1) = traj.multi_dof_joint_trajectory.points[trajectory_length - 1].transforms[0].translation.y;
        goal_pose_(2) = traj.multi_dof_joint_trajectory.points[trajectory_length - 1].transforms[0].rotation.z;

        /** Initialize constant Online Data Variables **/
        int N_iter;
		for (N_iter = 0; N_iter < ACADO_N; N_iter++) {
            acadoVariables.od[(ACADO_NOD * N_iter) + 60] = r_discs_;                                // radius of car discs
            acadoVariables.od[(ACADO_NOD * N_iter) + 61] = 0; //x_discs_[1];                        // position of the car discs
        }

        /** Initialize OCP solver **/
        acado_initializeSolver( );

        /** Request static environment map **/
        if (map_service_.call(map_srv_))
        {
            ROS_ERROR("Service GetMap succeeded.");
            environment_grid_ = map_srv_.response.map;
        }
        else
        {
            ROS_ERROR("Service GetMap failed.");
        }

        /** Set task flags and counters **/
        segment_counter = 0;
		goal_reached_ = false;
		last_poly_ = false;

		/** Initialize local reference path **/
        referencePath.InitLocalRefPath(lmpcc_config_->n_local_,lmpcc_config_->n_poly_per_clothoid_,ss,xx,yy,vv);

        if (lmpcc_config_->activate_debug_output_)
            referencePath.PrintLocalPath(ss,xx,yy);     // Print local reference path
		if (lmpcc_config_->activate_visualization_)
        {
            publishLocalRefPath();                      // Publish local reference path for visualization
            publishGlobalPlan();                        // Publish global reference path for visualization
        }

        /** Compute collision free area wrt static environment **/
		ComputeCollisionFreeArea();
    }
}

void LMPCC::ComputeCollisionFreeArea()
{
    // Initialize timer
    acado_timer t;
    acado_tic( &t );

    int x_path_i, y_path_i;
    double x_path, y_path, psi_path, theta_search, r;
    std::vector<double> C_N;

    int search_steps = 10;

    collision_free_delta_min_ = collision_free_delta_max_;

    // Iterate over points in prediction horizon to search for collision free circles
    for (int N_it = 0; N_it < ACADO_N; N_it++)
    {

        // Current search point of prediction horizon
        x_path = acadoVariables.x[N_it * ACADO_NX + 0];
        y_path = acadoVariables.x[N_it * ACADO_NX + 1];
        psi_path = acadoVariables.x[N_it * ACADO_NX + 2];

        // Find corresponding index of the point in the occupancy grid map
        x_path_i = (int) round((x_path - environment_grid_.info.origin.position.x)/environment_grid_.info.resolution);
        y_path_i = (int) round((y_path - environment_grid_.info.origin.position.y)/environment_grid_.info.resolution);

        // Compute the constraint
        computeConstraint(x_path_i,y_path_i,x_path, y_path, psi_path, N_it);

//        if (N_it == ACADO_N - 1)
//        {
//            ROS_INFO_STREAM("---------------------------------------------------------------------------");
//            ROS_INFO_STREAM("Searching last rectangle at x = " << x_path << " y = " << y_path << " psi = " << psi_path);
//        }

    }

    te_collision_free_ = acado_toc(&t);

    if (lmpcc_config_->activate_timing_output_)
        ROS_INFO_STREAM("Free space solve time " << te_collision_free_ * 1e6 << " us");
}

void LMPCC::computeConstraint(int x_i, int y_i, double x_path, double y_path, double psi_path, int N)
{
    // Initialize linear constraint normal vectors
    std::vector<double> t1(2, 0), t2(2, 0), t3(2, 0), t4(2, 0);

    // Declare search iterators
    int x_min, x_max, y_min, y_max;
    int search_x, search_y;
    int r_max_i_min, r_max_i_max;

    // define maximum search distance in occupancy grid cells, based on discretization
    r_max_i_min = (int) round(-collision_free_delta_max_ /environment_grid_.info.resolution);
    r_max_i_max = (int) round(collision_free_delta_max_/environment_grid_.info.resolution);

    // Initialize found rectabgle values with maxium search distance
    x_min = r_max_i_min;
    x_max = r_max_i_max;
    y_min = r_max_i_min;
    y_max = r_max_i_max;

    // Initialize search distance iterator
    int search_distance = 1;
    // Initialize boolean that indicates whether the region has been found
    bool search_region = true;

    // Iterate until the region is found
    while (search_region)
    {
        // Only search in x_min direction if no value has been found yet
        if (x_min == r_max_i_min)
        {
            search_x = -search_distance;
            for (int search_y_it = std::max(-search_distance,y_min); search_y_it < std::min(search_distance,y_max); search_y_it++)
            {
                // Correct search iterator if out of map bounds
                if (y_i + search_y_it > environment_grid_.info.height){search_y_it = environment_grid_.info.height - y_i;}
                if (y_i + search_y_it < 0){search_y_it = -y_i;}
                // Assign value if occupied cell is found
//                if (getOccupancy(x_i + search_x, y_i + search_y_it) > occupied_threshold_)
                if (getRotatedOccupancy(x_i, search_x, y_i, search_y_it, psi_path) > occupied_threshold_)
                {
                    x_min = search_x;
                }
            }
        } //else {ROS_INFO_STREAM("Already found x_min = " << x_min);}

        // Only search in x_max direction if no value has been found yet
        if (x_max == r_max_i_max)
        {
            search_x = search_distance;
            for (int search_y_it = std::max(-search_distance,y_min); search_y_it < std::min(search_distance,y_max); search_y_it++)
            {
                // Correct search iterator if out of map bounds
                if (y_i + search_y_it > environment_grid_.info.height){search_y_it = environment_grid_.info.height - y_i;}
                if (y_i + search_y_it < 0){search_y_it = -y_i;}
                // Assign value if occupied cell is found
//              if (getOccupancy(x_i + search_x, y_i + search_y_it) > occupied_threshold_)
                if (getRotatedOccupancy(x_i, search_x, y_i, search_y_it, psi_path) > occupied_threshold_)
                {
                    x_max = search_x;
                }
            }
        } //else {ROS_INFO_STREAM("Already found x_max = " << x_max);}

        // Only search in y_min direction if no value has been found yet
        if (y_min == r_max_i_min)
        {
            search_y = -search_distance;
            for (int search_x_it = std::max(-search_distance,x_min); search_x_it < std::min(search_distance,x_max); search_x_it++)
            {
                // Correct search iterator if out of map bounds
                if (x_i + search_x_it > environment_grid_.info.width){search_x_it = environment_grid_.info.width - x_i;}
                if (x_i + search_x_it < 0){search_x_it = -x_i;}
                // Assign value if occupied cell is found
//                if (getOccupancy(x_i + search_x_it, y_i + search_y) > occupied_threshold_)
                if (getRotatedOccupancy(x_i, search_x_it, y_i, search_y, psi_path) > occupied_threshold_)
                {
                    y_min = search_y;
                }
            }
        } //else {ROS_INFO_STREAM("Already found y_min = " << y_min);}

        // Only search in y_max direction if no value has been found yet
        if (y_max == r_max_i_max)
        {
            search_y = search_distance;
            for (int search_x_it = std::max(-search_distance,x_min); search_x_it < std::min(search_distance,x_max); search_x_it++)
            {
                // Correct search iterator if out of map bounds
                if (x_i + search_x_it > environment_grid_.info.width){search_x_it = environment_grid_.info.width - x_i;}
                if (x_i + search_x_it < 0){search_x_it = -x_i;}
                // Assign value if occupied cell is found
//                if (getOccupancy(x_i + search_x_it, y_i + search_y) > occupied_threshold_)
                if (getRotatedOccupancy(x_i, search_x_it, y_i, search_y, psi_path) > occupied_threshold_)
                {
                    y_max = search_y;
                }
            }
        } //else {ROS_INFO_STREAM("Already found y_max = " << y_max);}

        // Increase search distance
        search_distance++;
        // Determine whether the search is finished
        search_region = (search_distance < r_max_i_max) && ( x_min == r_max_i_min || x_max == r_max_i_max || y_min == r_max_i_min || y_max == r_max_i_max );
    }

    // Assign the rectangle values
    collision_free_xmin[N] = x_min*environment_grid_.info.resolution + 0.35;
    collision_free_xmax[N] = x_max*environment_grid_.info.resolution - 0.35;
    collision_free_ymin[N] = y_min*environment_grid_.info.resolution + 0.35;
    collision_free_ymax[N] = y_max*environment_grid_.info.resolution - 0.35;

    std::vector<double> sqx(4,0), sqy(4,0);

    sqx[0] = x_path + cos(psi_path)*collision_free_xmin[N] - sin(psi_path)*collision_free_ymin[N];
    sqx[1] = x_path + cos(psi_path)*collision_free_xmin[N] - sin(psi_path)*collision_free_ymax[N];
    sqx[2] = x_path + cos(psi_path)*collision_free_xmax[N] - sin(psi_path)*collision_free_ymax[N];
    sqx[3] = x_path + cos(psi_path)*collision_free_xmax[N] - sin(psi_path)*collision_free_ymin[N];

    sqy[0] = y_path + sin(psi_path)*collision_free_xmin[N] + cos(psi_path)*collision_free_ymin[N];
    sqy[1] = y_path + sin(psi_path)*collision_free_xmin[N] + cos(psi_path)*collision_free_ymax[N];
    sqy[2] = y_path + sin(psi_path)*collision_free_xmax[N] + cos(psi_path)*collision_free_ymax[N];
    sqy[3] = y_path + sin(psi_path)*collision_free_xmax[N] + cos(psi_path)*collision_free_ymin[N];

    t1[0] = (sqx[1] - sqx[0])/sqrt((sqx[1] - sqx[0])*(sqx[1] - sqx[0]) + (sqy[1] - sqy[0])*(sqy[1] - sqy[0]));
    t2[0] = (sqx[2] - sqx[1])/sqrt((sqx[2] - sqx[1])*(sqx[2] - sqx[1]) + (sqy[2] - sqy[1])*(sqy[2] - sqy[1]));
    t3[0] = (sqx[3] - sqx[2])/sqrt((sqx[3] - sqx[2])*(sqx[3] - sqx[2]) + (sqy[3] - sqy[2])*(sqy[3] - sqy[2]));
    t4[0] = (sqx[0] - sqx[3])/sqrt((sqx[0] - sqx[3])*(sqx[0] - sqx[3]) + (sqy[0] - sqy[3])*(sqy[0] - sqy[3]));

    t1[1] = (sqy[1] - sqy[0])/sqrt((sqx[1] - sqx[0])*(sqx[1] - sqx[0]) + (sqy[1] - sqy[0])*(sqy[1] - sqy[0]));
    t2[1] = (sqy[2] - sqy[1])/sqrt((sqx[2] - sqx[1])*(sqx[2] - sqx[1]) + (sqy[2] - sqy[1])*(sqy[2] - sqy[1]));
    t3[1] = (sqy[3] - sqy[2])/sqrt((sqx[3] - sqx[2])*(sqx[3] - sqx[2]) + (sqy[3] - sqy[2])*(sqy[3] - sqy[2]));
    t4[1] = (sqy[0] - sqy[3])/sqrt((sqx[0] - sqx[3])*(sqx[0] - sqx[3]) + (sqy[0] - sqy[3])*(sqy[0] - sqy[3]));

    collision_free_a1x[N] = t1[1];
    collision_free_a2x[N] = t2[1];
    collision_free_a3x[N] = t3[1];
    collision_free_a4x[N] = t4[1];

    collision_free_a1y[N] = -t1[0];
    collision_free_a2y[N] = -t2[0];
    collision_free_a3y[N] = -t3[0];
    collision_free_a4y[N] = -t4[0];

    collision_free_C1[N] = sqx[0]*collision_free_a1x[N] + sqy[0]*collision_free_a1y[N];
    collision_free_C2[N] = sqx[1]*collision_free_a2x[N] + sqy[1]*collision_free_a2y[N];
    collision_free_C3[N] = sqx[2]*collision_free_a3x[N] + sqy[2]*collision_free_a3y[N];
    collision_free_C4[N] = sqx[3]*collision_free_a4x[N] + sqy[3]*collision_free_a4y[N];

//    if (N == ACADO_N - 1)
//    {
//        ROS_INFO_STREAM("x = " << x_path << " y = " << y_path << " psi = " << psi_path);
//        ROS_INFO_STREAM("x_i = " << x_i << " y_i = " << y_i);
//        ROS_INFO_STREAM("xmin_i = " << x_min << " x_max_i = " << x_max << " ymin_i = " << y_min << " y_max_i = " << y_max);
//        ROS_INFO_STREAM("xmin = " << collision_free_xmin[N] << " x_max = " << collision_free_xmax[N] << " ymin = " << collision_free_ymin[N] << " y_max = " << collision_free_ymax[N]);
//        ROS_INFO_STREAM("xmin_R = " << collision_free_xmin[N] << " xmax_R = " << collision_free_xmax[N] << " ymin_R = " << collision_free_ymin[N] << " ymax_R = " << collision_free_ymax[N]);
//        ROS_INFO_STREAM("sq[0] = [" << sqx[0] << ", " << sqy[0] << "], sq[1] = [" << sqx[1] << ", " << sqy[1] << "], sq[2] = [" << sqx[2] << ", " << sqy[2] << "], sq[3] = [" << sqx[3] << ", " << sqy[3] << "]" );
//        ROS_INFO_STREAM("t1 = [" << t1[0] << ", " << t1[1] << "], t2 = [" << t2[0] << ", " << t2[1] << "], t3 = [" << t3[0] << ", " << t3[1] << "], t4 = [" << t4[0] << ", " << t4[1] << "]" );
//        ROS_INFO_STREAM("collision_free_a1 = [" << collision_free_a1x[N] << ", " << collision_free_a1y[N] << "], collision_free_a2 = [" << collision_free_a2x[N] << ", " << collision_free_a2y[N] << "], collision_free_a3 = [" << collision_free_a3x[N] << ", " << collision_free_a3y[N] << "], collision_free_a4 = [" << collision_free_a4x[N] << ", " << collision_free_a4y[N] << "]" );
//        ROS_INFO_STREAM("collision_free_C1 = [" << collision_free_C1[N] << "], collision_free_C2 = [" << collision_free_C2[N] << "], collision_free_C3 = [" << collision_free_C3[N] << "], collision_free_C4 = [" << collision_free_C4[N] << "]" );
//    }


//    ROS_INFO_STREAM("xi = " << x_i << " yi = " << y_i );
//    ROS_INFO_STREAM("xmin = " << collision_free_xmin[N] << " x_max = " << collision_free_xmax[N] << " ymin = " << collision_free_ymin[N] << " y_max = " << collision_free_ymax[N] );
}

int LMPCC::getOccupancy(int x_i, int y_i)
{
    return environment_grid_.data[environment_grid_.info.width*y_i + x_i];
}

int LMPCC::getRotatedOccupancy(int x_i, int search_x, int y_i, int search_y, double psi)
{
    int x_search_rotated = (int) round(cos(psi)*search_x - sin(psi)*search_y);
    int y_search_rotated = (int) round(sin(psi)*search_x + cos(psi)*search_y);

    return environment_grid_.data[environment_grid_.info.width*(y_i + y_search_rotated) + (x_i + x_search_rotated)];
}

void LMPCC::movePreemptCB()
{
    move_action_result_.reach = true;
    move_action_server_->setPreempted(move_action_result_, "Action has been preempted");
    tracking_ = true;
}

void LMPCC::actionSuccess()
{
    move_action_server_->setSucceeded(move_action_result_, "Goal succeeded!");
    tracking_ = true;
}

void LMPCC::actionAbort()
{
    move_action_server_->setAborted(move_action_result_, "Action has been aborted");
    tracking_ = true;
}

// read current position and velocity of robot joints
void LMPCC::StateCallBack(const geometry_msgs::Pose::ConstPtr& msg)
{
    if (lmpcc_config_->activate_debug_output_) {
//        ROS_INFO("LMPCC::StateCallBack");
    }

    last_state_ = current_state_;

    current_state_(0) =    msg->position.x;
    current_state_(1) =    msg->position.y;
    current_state_(2) =    msg->orientation.z;
}

//void LMPCC::ObstacleCallBack(const nav_msgs::Path& predicted_path)
//{
//    if (predicted_path.poses.size() != lmpcc_config_->discretization_intervals_)
//    {
//        ROS_WARN_STREAM("Received obstacle trajectory length is divergent");
//    }
//
//    if (predicted_path.header.frame_id == "1"){
////        ROS_INFO_STREAM("obstacle 1");
//
//        for (int path_it = 0; path_it < ACADO_N ; path_it++)
//        {
//            obst1_x[path_it] = predicted_path.poses[path_it].pose.position.x + 1.55;
//            obst1_y[path_it] = predicted_path.poses[path_it].pose.position.y + 2.75;
//        }
//    }
//    else if  (predicted_path.header.frame_id == "2"){
////        ROS_INFO_STREAM("obstacle 2");
//
//        for (int path_it = 0; path_it < ACADO_N ; path_it++)
//        {
//            obst2_x[path_it] = predicted_path.poses[path_it].pose.position.x + 1.55;
//            obst2_y[path_it] = predicted_path.poses[path_it].pose.position.y + 2.75;
//        }
//
//    }else if  (predicted_path.header.frame_id == "3"){
//
//    }
//    else {
//        ROS_INFO_STREAM("Obstacle id not recognized");
//    }
////    ROS_INFO_STREAM("obst1_x: " << obst1_x[0]);
//}

void LMPCC::ObstacleCallBack(const lmpcc_msgs::lmpcc_obstacle_array& received_obstacles)
{
    lmpcc_msgs::lmpcc_obstacle_array total_obstacles;
    total_obstacles.lmpcc_obstacles.resize(lmpcc_config_->n_obstacles_);

    total_obstacles.lmpcc_obstacles = received_obstacles.lmpcc_obstacles;

//    ROS_INFO_STREAM("-- Received # obstacles: " << obstacles.Obstacles.size());
//    ROS_INFO_STREAM("-- Expected # obstacles: " << lmpcc_config_->n_obstacles_);

    if (received_obstacles.lmpcc_obstacles.size() < lmpcc_config_->n_obstacles_)
    {
        for (int obst_it = received_obstacles.lmpcc_obstacles.size(); obst_it < lmpcc_config_->n_obstacles_; obst_it++)
        {
            total_obstacles.lmpcc_obstacles[obst_it].pose.position.x = current_state_(0) - 100;
            total_obstacles.lmpcc_obstacles[obst_it].pose.position.y = 0;
            total_obstacles.lmpcc_obstacles[obst_it].pose.orientation.z = 0;
            total_obstacles.lmpcc_obstacles[obst_it].major_semiaxis = 0.001;
            total_obstacles.lmpcc_obstacles[obst_it].minor_semiaxis = 0.001;

            for (int traj_it = 0; traj_it < ACADO_N; traj_it++)
            {
                total_obstacles.lmpcc_obstacles[obst_it].trajectory.poses[traj_it].pose.position.x = current_state_(0) - 100;
                total_obstacles.lmpcc_obstacles[obst_it].trajectory.poses[traj_it].pose.position.y = 0;
                total_obstacles.lmpcc_obstacles[obst_it].trajectory.poses[traj_it].pose.orientation.z = 0;
            }
        }
    }

    obstacles_.lmpcc_obstacles.resize(lmpcc_config_->n_obstacles_);

    for (int total_obst_it = 0; total_obst_it < lmpcc_config_->n_obstacles_; total_obst_it++)
    {
        obstacles_.lmpcc_obstacles[total_obst_it] = total_obstacles.lmpcc_obstacles[total_obst_it];
    }
}

void LMPCC::publishZeroJointVelocity()
{
    if (lmpcc_config_->activate_debug_output_)
    {
        ROS_INFO("Publishing ZERO joint velocity!!");
    }

    geometry_msgs::Twist pub_msg;

	if(!lmpcc_config_->gazebo_simulation_)
		broadcastTF();

    controlled_velocity_ = pub_msg;
    controlled_velocity_pub_.publish(controlled_velocity_);
}

void LMPCC::publishGlobalPlan(void)
{
    // Create MarkerArray for global path point visualization
    visualization_msgs::MarkerArray plan;

    // Initialize vectors for global path points
    std::vector<double> X_global, Y_global;

    // Request global path points
    referencePath.GetGlobalPath(X_global,Y_global);

    // Iterate over all points in global path
    for (int i = 0; i < X_global.size(); i++)
    {
        global_plan.id = 800+i;
        global_plan.pose.position.x = X_global[i];
        global_plan.pose.position.y = Y_global[i];
        global_plan.pose.orientation.x = 0;
        global_plan.pose.orientation.y = 0;
        global_plan.pose.orientation.z = 0;
        global_plan.pose.orientation.w = 1;
        plan.markers.push_back(global_plan);
    }

    // Publish markerarray of global path points
    global_plan_pub_.publish(plan);
}

void LMPCC::publishLocalRefPath(void)
{
    local_spline_traj1_.header.stamp = ros::Time::now();
    local_spline_traj2_.header.stamp = ros::Time::now();
    local_spline_traj3_.header.stamp = ros::Time::now();
    local_spline_traj4_.header.stamp = ros::Time::now();
    local_spline_traj5_.header.stamp = ros::Time::now();

    local_spline_traj1_.header.frame_id = lmpcc_config_->planning_frame_;
    local_spline_traj2_.header.frame_id = lmpcc_config_->planning_frame_;
    local_spline_traj3_.header.frame_id = lmpcc_config_->planning_frame_;
    local_spline_traj4_.header.frame_id = lmpcc_config_->planning_frame_;
    local_spline_traj5_.header.frame_id = lmpcc_config_->planning_frame_;

    double s1,s2,s3,s4,s5;
    int j=0;
    for (int i = 0; i < 50; i++)
    {
        s1= i*(ss[2] - ss[1])/50.0;
        s2= i*(ss[3] - ss[2])/50.0;
        s3= i*(ss[4] - ss[3])/50.0;
        s4= i*(ss[5] - ss[4])/50.0;
        s5= i*(ss[6] - ss[5])/50.0;


        local_spline_traj1_.poses[i].pose.position.x = referencePath.ref_path_x.m_a[1]*s1*s1*s1+referencePath.ref_path_x.m_b[1]*s1*s1+referencePath.ref_path_x.m_c[1]*s1+referencePath.ref_path_x.m_d[1]; //x
        local_spline_traj1_.poses[i].pose.position.y = referencePath.ref_path_y.m_a[1]*s1*s1*s1+referencePath.ref_path_y.m_b[1]*s1*s1+referencePath.ref_path_y.m_c[1]*s1+referencePath.ref_path_y.m_d[1]; //y
        local_spline_traj1_.poses[i].header.stamp = ros::Time::now();
        local_spline_traj1_.poses[i].header.frame_id = lmpcc_config_->planning_frame_;

        local_spline_traj2_.poses[i].pose.position.x = referencePath.ref_path_x.m_a[2]*s2*s2*s2+referencePath.ref_path_x.m_b[2]*s2*s2+referencePath.ref_path_x.m_c[2]*s2+referencePath.ref_path_x.m_d[2]; //x
        local_spline_traj2_.poses[i].pose.position.y = referencePath.ref_path_y.m_a[2]*s2*s2*s2+referencePath.ref_path_y.m_b[2]*s2*s2+referencePath.ref_path_y.m_c[2]*s2+referencePath.ref_path_y.m_d[2]; //y
        local_spline_traj2_.poses[i].header.stamp = ros::Time::now();
        local_spline_traj2_.poses[i].header.frame_id = lmpcc_config_->planning_frame_;

        local_spline_traj3_.poses[i].pose.position.x = referencePath.ref_path_x.m_a[3]*s3*s3*s3+referencePath.ref_path_x.m_b[3]*s3*s3+referencePath.ref_path_x.m_c[3]*s3+referencePath.ref_path_x.m_d[3]; //x
        local_spline_traj3_.poses[i].pose.position.y = referencePath.ref_path_y.m_a[3]*s3*s3*s3+referencePath.ref_path_y.m_b[3]*s3*s3+referencePath.ref_path_y.m_c[3]*s3+referencePath.ref_path_y.m_d[3]; //y
        local_spline_traj3_.poses[i].header.stamp = ros::Time::now();
        local_spline_traj3_.poses[i].header.frame_id = lmpcc_config_->planning_frame_;

        local_spline_traj4_.poses[i].pose.position.x = referencePath.ref_path_x.m_a[4]*s4*s4*s4+referencePath.ref_path_x.m_b[4]*s4*s4+referencePath.ref_path_x.m_c[4]*s4+referencePath.ref_path_x.m_d[4]; //x
        local_spline_traj4_.poses[i].pose.position.y = referencePath.ref_path_y.m_a[4]*s4*s4*s4+referencePath.ref_path_y.m_b[4]*s4*s4+referencePath.ref_path_y.m_c[4]*s4+referencePath.ref_path_y.m_d[4]; //y
        local_spline_traj4_.poses[i].header.stamp = ros::Time::now();
        local_spline_traj4_.poses[i].header.frame_id = lmpcc_config_->planning_frame_;

        local_spline_traj5_.poses[i].pose.position.x = referencePath.ref_path_x.m_a[5]*s5*s5*s5+referencePath.ref_path_x.m_b[5]*s5*s5+referencePath.ref_path_x.m_c[5]*s5+referencePath.ref_path_x.m_d[5]; //x
        local_spline_traj5_.poses[i].pose.position.y = referencePath.ref_path_y.m_a[5]*s5*s5*s5+referencePath.ref_path_y.m_b[5]*s5*s5+referencePath.ref_path_y.m_c[5]*s5+referencePath.ref_path_y.m_d[5]; //y
        local_spline_traj5_.poses[i].header.stamp = ros::Time::now();
        local_spline_traj5_.poses[i].header.frame_id = lmpcc_config_->planning_frame_;
    }

    local_spline_traj_pub1_.publish(local_spline_traj1_);
    local_spline_traj_pub2_.publish(local_spline_traj2_);
    local_spline_traj_pub3_.publish(local_spline_traj3_);
    local_spline_traj_pub4_.publish(local_spline_traj4_);
    local_spline_traj_pub5_.publish(local_spline_traj5_);
}

void LMPCC::publishPredictedTrajectory(void)
{
    for (int i = 0; i < ACADO_N; i++)
    {
        pred_traj_.poses[i].pose.position.x = acadoVariables.x[i * ACADO_NX + 0]; //x
        pred_traj_.poses[i].pose.position.y = acadoVariables.x[i * ACADO_NX + 1]; //y
		pred_traj_.poses[i].pose.orientation.z = acadoVariables.x[i * ACADO_NX + 2]; //theta
    }

	pred_traj_pub_.publish(pred_traj_);
}

void LMPCC::publishPredictedOutput(void)
{
	for (int i = 0; i < ACADO_N; i++)
	{
		pred_cmd_.poses[i].pose.position.x = acadoVariables.u[i + 0]; //x
		pred_cmd_.poses[i].pose.position.y = acadoVariables.u[i + 1]; //y
	}

	pred_cmd_pub_.publish(pred_cmd_);
}

void LMPCC::publishPredictedCollisionSpace(void)
{
	visualization_msgs::MarkerArray collision_space;

	for (int i = 0; i < ACADO_N; i++)
	{
		ellips1.id = 60+i;
		ellips1.pose.position.x = acadoVariables.x[i * ACADO_NX + 0];
		ellips1.pose.position.y = acadoVariables.x[i * ACADO_NX + 1];
		ellips1.pose.orientation.x = 0;
		ellips1.pose.orientation.y = 0;
		ellips1.pose.orientation.z = 0;
		ellips1.pose.orientation.w = 1;
		collision_space.markers.push_back(ellips1);
	}

	robot_collision_space_pub_.publish(collision_space);
}

void LMPCC::publishCost(void){

	cost_pub_.publish(cost_);
}

void LMPCC::publishContourError(void){

    // Compute contour and lag error to publish
    double x_path, y_path, dx_path, dy_path, abs_grad, dx_path_norm, dy_path_norm;

    x_path = (referencePath.ref_path_x.m_a[segment_counter]*(acadoVariables.x[3]-ss[segment_counter])*(acadoVariables.x[3]-ss[segment_counter])*(acadoVariables.x[3]-ss[segment_counter]) + referencePath.ref_path_x.m_b[segment_counter]*(acadoVariables.x[3]-ss[segment_counter])*(acadoVariables.x[3]-ss[segment_counter]) + referencePath.ref_path_x.m_c[segment_counter]*(acadoVariables.x[3]-ss[segment_counter]) + referencePath.ref_path_x.m_d[segment_counter]);
    y_path = (referencePath.ref_path_y.m_a[segment_counter]*(acadoVariables.x[3]-ss[segment_counter])*(acadoVariables.x[3]-ss[segment_counter])*(acadoVariables.x[3]-ss[segment_counter]) + referencePath.ref_path_y.m_b[segment_counter]*(acadoVariables.x[3]-ss[segment_counter])*(acadoVariables.x[3]-ss[segment_counter]) + referencePath.ref_path_y.m_c[segment_counter]*(acadoVariables.x[3]-ss[segment_counter]) + referencePath.ref_path_y.m_d[segment_counter]);
    dx_path = (3*referencePath.ref_path_x.m_a[segment_counter]*(acadoVariables.x[3]-ss[segment_counter])*(acadoVariables.x[3]-ss[segment_counter]) + 2*referencePath.ref_path_x.m_b[segment_counter]*(acadoVariables.x[3]-ss[segment_counter]) + referencePath.ref_path_x.m_c[segment_counter]);
    dy_path = (3*referencePath.ref_path_y.m_a[segment_counter]*(acadoVariables.x[3]-ss[segment_counter])*(acadoVariables.x[3]-ss[segment_counter]) + 2*referencePath.ref_path_y.m_b[segment_counter]*(acadoVariables.x[3]-ss[segment_counter]) + referencePath.ref_path_y.m_c[segment_counter]);

    abs_grad = sqrt(pow(dx_path,2) + pow(dy_path,2));

    dx_path_norm = dx_path/abs_grad;
    dy_path_norm = dy_path/abs_grad;

    contour_error_ =  dy_path_norm * (acadoVariables.x[0] - x_path) - dx_path_norm * (acadoVariables.x[1] - y_path);
    lag_error_ = -dx_path_norm * (acadoVariables.x[0] - x_path) - dy_path_norm * (acadoVariables.x[1] - y_path);

    std_msgs::Float64MultiArray errors;

    errors.data.resize(2);

    errors.data[0] = contour_error_;
    errors.data[1] = lag_error_;

    contour_error_pub_.publish(errors);
}

void LMPCC::ZRotToQuat(geometry_msgs::Pose& pose)
{
    pose.orientation.w = cos(pose.orientation.z * 0.5);
    pose.orientation.x = 0;
    pose.orientation.y = 0;
    pose.orientation.z = sin(pose.orientation.z * 0.5);
}

void LMPCC::publishPosConstraint(){

    visualization_msgs::MarkerArray collision_free;
    double x_center, y_center;

    for (int i = 0; i < ACADO_N; i++)
    {
        cube1.scale.x = -collision_free_xmin[i] + collision_free_xmax[i];
        cube1.scale.y = -collision_free_ymin[i] + collision_free_ymax[i];
        cube1.scale.z = 0.01;

        // Find the center of the collision free area to be able to draw it properly
        x_center = collision_free_xmax[i] - (-collision_free_xmin[i] + collision_free_xmax[i])/2;
        y_center = collision_free_ymax[i] - (-collision_free_ymin[i] + collision_free_ymax[i])/2;

        // Assign center of cube
        cube1.pose.position.x = acadoVariables.x[i * ACADO_NX + 0] + cos(acadoVariables.x[i * ACADO_NX + 2])*x_center - sin(acadoVariables.x[i * ACADO_NX + 2])*y_center;
        cube1.pose.position.y = acadoVariables.x[i * ACADO_NX + 1] + sin(acadoVariables.x[i * ACADO_NX + 2])*x_center + cos(acadoVariables.x[i * ACADO_NX + 2])*y_center;

        cube1.id = 400+i;
        cube1.pose.orientation.x = 0;
        cube1.pose.orientation.y = 0;
        cube1.pose.orientation.z = acadoVariables.x[i * ACADO_NX + 2];
        cube1.pose.orientation.w = 1;
        ZRotToQuat(cube1.pose);
        collision_free.markers.push_back(cube1);
    }

    collision_free_pub_.publish(collision_free);
}

void LMPCC::publishFeedback(int& it, double& time)
{

    lmpcc_msgs::lmpcc_feedback feedback_msg;

    feedback_msg.header.stamp = ros::Time::now();
    feedback_msg.header.frame_id = lmpcc_config_->planning_frame_;

    feedback_msg.cost = cost_.data;
    feedback_msg.iterations = it;
    feedback_msg.computation_time = time;
    feedback_msg.freespace_time = te_collision_free_;
    feedback_msg.kkt = acado_getKKT();

    feedback_msg.wC = cost_contour_weight_factors_(0);       // weight factor on contour error
    feedback_msg.wL = cost_contour_weight_factors_(1);       // weight factor on lag error
    feedback_msg.wV = cost_control_weight_factors_(0);       // weight factor on theta
    feedback_msg.wW = cost_control_weight_factors_(1);

    // Compute contour errors
    feedback_msg.contour_errors.data.resize(2);

    feedback_msg.contour_errors.data[0] = contour_error_;
    feedback_msg.contour_errors.data[1] = lag_error_;

//    feedback_msg.reference_path = spline_traj2_;
    feedback_msg.prediction_horizon = pred_traj_;
    feedback_msg.prediction_horizon.poses[0].pose.position.z = acadoVariables.x[3];
    feedback_msg.computed_control = controlled_velocity_;

    feedback_msg.enable_output = enable_output_;

    feedback_msg.vRef = reference_velocity_;

    feedback_msg.obstacle_distance1 = sqrt(pow(pred_traj_.poses[0].pose.position.x - obstacles_.lmpcc_obstacles[0].pose.position.x ,2) + pow(pred_traj_.poses[0].pose.position.y - obstacles_.lmpcc_obstacles[0].pose.position.y,2));
    feedback_msg.obstacle_distance2 = sqrt(pow(pred_traj_.poses[0].pose.position.x - obstacles_.lmpcc_obstacles[1].pose.position.x ,2) + pow(pred_traj_.poses[0].pose.position.y - obstacles_.lmpcc_obstacles[1].pose.position.y,2));

    feedback_msg.obstx_0 = obstacles_.lmpcc_obstacles[0].pose.position.x;
    feedback_msg.obsty_0 = obstacles_.lmpcc_obstacles[0].pose.position.y;
    feedback_msg.obsth_0 = obstacles_.lmpcc_obstacles[0].pose.orientation.z;
    feedback_msg.obsta_0 = obstacles_.lmpcc_obstacles[0].major_semiaxis;
    feedback_msg.obstb_0 = obstacles_.lmpcc_obstacles[0].minor_semiaxis;

    feedback_msg.obstx_1 = obstacles_.lmpcc_obstacles[1].pose.position.x;
    feedback_msg.obsty_1 = obstacles_.lmpcc_obstacles[1].pose.position.y;
    feedback_msg.obsth_1 = obstacles_.lmpcc_obstacles[1].pose.orientation.z;
    feedback_msg.obsta_1 = obstacles_.lmpcc_obstacles[1].major_semiaxis;
    feedback_msg.obstb_1 = obstacles_.lmpcc_obstacles[1].minor_semiaxis;

    //Search window parameters
    feedback_msg.window = window_size_;
    feedback_msg.search_points = n_search_points_;

    feedback_pub_.publish(feedback_msg);
}