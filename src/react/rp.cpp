#include "rp.hpp"

REACT::REACT(){


	// Should be read as param
	ros::param::get("~thresh",thresh_);
	ros::param::get("~debug",debug_);
	ros::param::get("~safe_distance",safe_distance_);
	ros::param::get("~buffer",buffer_);

	last_goal_ = Eigen::Vector3d::Zero();
	pose_= Eigen::Vector3d::Zero();
	goal_ = Eigen::Vector3d::Zero();
	X_ = Eigen::MatrixXd::Zero(4,2);
	X_stop_ = Eigen::MatrixXd::Zero(4,2);
	XE_ = Eigen::MatrixXd::Zero(4,2);


	ros::param::get("~goal_x",goal_(0));
	ros::param::get("~goal_y",goal_(1));
	ros::param::get("~goal_z",goal_(2));

	heading_ = atan2(goal_(1),goal_(0));
	last_goal_ << goal_;
	local_goal_ << goal_;

	ros::param::get("cntrl/spinup_time",spinup_time_);
	ros::param::get("~speed",v_max_);

	ros::param::get("~jerk",j_max_);
	ros::param::get("~accel",a_max_);

	ros::param::get("~plan_eval",plan_eval_time_);

	ros::param::get("~K",K_);
	ros::param::get("~K_buffer",K_buffer_);

	ros::param::get("~h_fov",h_fov_);
	h_fov_ = h_fov_*PI/180;

	v_ = 0;

	angle_seg_inc_ = 10*PI/180;

	num_of_clusters_ = 0;
	collision_counter_corridor_ = 0;
	collision_counter_ = 0;
	can_reach_goal_ = false;
	inf_ = std::numeric_limits<double>::max();

	quad_status_ = state_.NOT_FLYING;

	quad_goal_.cut_power = true;

	ROS_INFO("Planner initialized.");

}

void REACT::global_goalCB(const geometry_msgs::PointStamped& msg){
	goal_ << msg.point.x, msg.point.y, msg.point.z;
	heading_ = atan2(goal_(1),goal_(0));
}

void REACT::stateCB(const acl_system::ViconState& msg)
{
	// TODO time check.
	if (msg.has_pose) {
		pose_ << msg.pose.position.x, msg.pose.position.y, msg.pose.position.z; 

		yaw_ = tf::getYaw(msg.pose.orientation);

		if (quad_status_ == state_.NOT_FLYING){
			X_.row(0) << pose_.head(2).transpose();
		}
	} 
	// if (msg.has_twist) velCallback(msg.twist);
}

void REACT::sendGoal(const ros::TimerEvent& e)
{	
	if (gen_new_traj_){
		gen_new_traj_ = false;
		mtx.lock();
		get_traj(X_,local_goal_,v_,t_xf_,t_yf_,Xf_switch_,Yf_switch_,false);
		mtx.unlock();
		t0_ = ros::Time::now().toSec() - plan_eval_time_;
	}

	if (quad_status_== state_.TAKEOFF){
		takeoff();
		if (quad_goal_.pos.z == goal_(2)){
			quad_status_ = state_.FLYING;
			ROS_INFO("Take-off Complete");
		}
	}

	else if (quad_status_== state_.LAND){
			land();
			if (quad_goal_.pos.z == -0.1){
				quad_status_ = state_.NOT_FLYING;
				quad_goal_.cut_power = true;
				ROS_INFO("Landing Complete");
			}
		}

	else if (quad_status_ == state_.GO){
			if (!stop_){
				get_stop_dist(X_,local_goal_,goal_,stop_);
				if (stop_){
					ROS_INFO("Stop");
					v_ = 0;
					gen_new_traj_ = true;
				}
			}

			tE_ = ros::Time::now().toSec() - t0_;
			
			mtx.lock();		
			eval_trajectory(Xf_switch_,Yf_switch_,t_xf_,t_yf_,tE_,X_);
			mtx.unlock();

			eigen2quadGoal(X_,quad_goal_);
			quad_goal_.yaw = heading_;

			if (X_.block(1,0,1,2).norm() == 0){
				// We're done
				ROS_INFO("Flight Complete");
				quad_status_ = state_.FLYING;
				stop_ = false;
			}
		}

	quad_goal_.header.stamp = ros::Time::now();
	quad_goal_.header.frame_id = "vicon";
	quad_goal_pub.publish(quad_goal_);
}


void REACT::eventCB(const acl_system::QuadFlightEvent& msg)
{
	// Takeoff
	if (msg.mode == msg.TAKEOFF && quad_status_== state_.NOT_FLYING){

		ROS_INFO("Waiting for spinup");
		quad_goal_.pos.x = pose_(0);
		quad_goal_.pos.y = pose_(1);
		quad_goal_.pos.z = pose_(2);

		X_(0,0) = pose_(0);
		X_(0,1) = pose_(1);

		quad_goal_.vel.x = 0;
		quad_goal_.vel.y = 0;
		quad_goal_.vel.z = 0;

		quad_goal_.yaw = yaw_;
		quad_goal_.dyaw = 0;

		quad_goal_.cut_power = false;

		ros::Duration(spinup_time_).sleep();
		ROS_INFO("Taking off");

		quad_status_ = state_.TAKEOFF; 
		

	}
	// Emergency kill
	else if (msg.mode == msg.KILL && quad_status_ != state_.NOT_FLYING){
		quad_status_ = state_.NOT_FLYING;
		quad_goal_.cut_power = true;
		ROS_ERROR("Killing");
	}
	// Landing
	else if (msg.mode == msg.LAND && quad_status_ == state_.FLYING){
		quad_status_ = state_.LAND;
		ROS_INFO("Landing");
	}
	// Initializing
	else if (msg.mode == msg.INIT && quad_status_ == state_.FLYING){
		quad_goal_.yaw = heading_;
		ROS_INFO("Initialized");
	}
	// GO!!!!
	else if (msg.mode == msg.START && quad_status_ == state_.FLYING){
		// Set speed to desired speed
		v_ = v_max_;
		// Generate new traj
		gen_new_traj_ = true;
		quad_status_ = state_.GO;
		ROS_INFO("Starting");
		
	}
	// STOP!!!
	else if (msg.mode == msg.ESTOP && quad_status_ == state_.GO){
		ROS_WARN("Stopping");
		// Stay in go command but set speed to zero
		v_ = 0;

		// Generate new traj
		gen_new_traj_ = true;
		
	}
}

void REACT::pclCB(const sensor_msgs::PointCloud2ConstPtr& msg)
 {

 	msg_received_ = ros::WallTime::now().toSec();

	// Convert pcl
	convert2pcl(msg,cloud_);
 	
	// Build k-d tree
	kdtree_.setInputCloud (cloud_);

	// Generate allowable final states
	sample_ss(Goals_);

	// Sort allowable final states
	sort_ss(last_goal_,Goals_,pose_,goal_,Sorted_Goals_);

	// Pick desired final state
	pick_ss(cloud_, Sorted_Goals_, X_, last_goal_, local_goal_, can_reach_goal_);

 	// if (!can_reach_goal_){
 	// 	// Need to stop!!!
 	// 	v_ = 0;
 	// 	ROS_ERROR("Emergency stop -- no feasible path");
 	// }

 	gen_new_traj_ = true;

 	traj_gen_ = ros::WallTime::now().toSec();

 	latency_.data = traj_gen_ - msg_received_;
 	latency_.header.stamp = ros::Time::now();

 	std::cout << "Latency [ms]: " << 1000*(traj_gen_ - msg_received_) << std::endl;

 	// if(debug_){
 	// 	convert2ROS(Goals_);
 	// 	pubROS();
 	// } 	
}

void REACT::sample_ss(Eigen::MatrixXd& Goals){
	Goals = Eigen::MatrixXd::Zero(12,1);
	Goals.col(0).setLinSpaced(12,-h_fov_/2+yaw_,h_fov_/2+yaw_);
	// std::cout << Goals << std::endl;
}

void REACT::sort_ss(Eigen::Vector3d last_goal, Eigen::MatrixXd Goals, Eigen::Vector3d pose, Eigen::Vector3d goal, Eigen::MatrixXd& Sorted_Goals){
 	// Re-initialize
	cost_queue_ = std::priority_queue<double, std::vector<double>, std::greater<double> > ();
	Sorted_Goals = Eigen::MatrixXd::Zero(Goals.rows()+1,Goals.cols()+1);
	cost_v_.clear();

	angle_2_goal_ = atan2( goal(1) -pose(1), goal(0) - pose(0)) ;
	angle_2_last_goal = atan2( last_goal(1) -pose(1), last_goal(0) - pose(0)) ;

	num_of_clusters_ = Goals.rows();

 	for (int i=0; i < num_of_clusters_ ; i++){
		angle_i_ = Goals(i,0);
 		angle_diff_  =  std::abs(angle_i_)  - std::abs(angle_2_goal_ );

 		angle_diff_last_ = std::abs(angle_i_) - std::abs(angle_2_last_goal);

 		cost_i_ = pow(angle_diff_,2) + pow(angle_diff_last_,2);

 		cost_queue_.push(cost_i_);
 		cost_v_.push_back(cost_i_);

 	}

 	Sorted_Goals.row(0)<< angle_2_goal_, 0;

 	for (int i=0; i < num_of_clusters_ ; i++){

	 	min_cost_ = cost_queue_.top();

		it_ = std::find(cost_v_.begin(),cost_v_.end(),min_cost_);
		goal_index_ = it_ - cost_v_.begin();

		Sorted_Goals.row(i+1) << Goals.row(goal_index_), min_cost_;

		cost_queue_.pop();
	}
}

void REACT::pick_ss(pcl::PointCloud<pcl::PointXYZ>::Ptr cloud, Eigen::MatrixXd Sorted_Goals, Eigen::MatrixXd X, Eigen::Vector3d& last_goal, Eigen::Vector3d& local_goal, bool& can_reach_goal){
	goal_index_ = 0;
	can_reach_goal = false;
 	last_goal = local_goal;

	pcl::PointXYZ searchPoint;

	searchPoint.x = 0;
	searchPoint.y = 0;
	searchPoint.z = 0;

	// std::vector<int> pointIdxNKNSearch(K_);
 //  	std::vector<float> pointNKNSquaredDistance(K_);

	// kdtree_.nearestKSearch (searchPoint, K_, pointIdxNKNSearch, pointNKNSquaredDistance);

	// for (size_t i = 0; i < pointIdxNKNSearch.size (); ++i)
	//       std::cout << "    "  <<   cloud->points[ pointIdxNKNSearch[i] ].x 
	//                 << " " << cloud->points[ pointIdxNKNSearch[i] ].y 
	//                 << " " << cloud->points[ pointIdxNKNSearch[i] ].z 
	//                 << " (distance: " << std::sqrt(pointNKNSquaredDistance[i]) << ")" << std::endl;


 	while(!can_reach_goal && goal_index_ < Sorted_Goals.rows()){
 		// if (Sorted_Goals(goal_index_,0) > safe_distance_ || goal_index_==0){
 		// 	collision_check(X,Sorted_Goals,goal_index_,buffer_,v_max_,partition_,tf_,can_reach_goal);
 		// }
 		can_reach_goal = true;
 		goal_index_++;
 	}

 	if(can_reach_goal){
 		goal_index_--;
 		local_goal << 0,0,0;
 	}
 	else{
 		ROS_ERROR("Need to stop");
 	}
}


void REACT::get_traj(Eigen::MatrixXd X, Eigen::Vector3d local_goal, double v, std::vector<double>& t_fx, std::vector<double>& t_fy, Eigen::Matrix4d& Xf_switch, Eigen::Matrix4d& Yf_switch, bool stop_check ){
	//Generate new traj
	get_vels(local_goal,X,v,vfx_,vfy_);

	x0_ << X.block(0,0,4,1);
	y0_ << X.block(0,1,4,1);

	find_times(x0_, vfx_, t_fx, Xf_switch,stop_check);
	find_times(y0_, vfy_, t_fy, Yf_switch,stop_check);
}


void REACT::get_stop_dist(Eigen::MatrixXd X, Eigen::Vector3d local_goal,Eigen::Vector3d goal, bool& stop){
	if (local_goal == goal){
		mtx.lock();
		get_traj(X,goal,0,t_x_stop_,t_y_stop_,X_switch_stop_,Y_switch_stop_,true);
		mtx.unlock();

		t_stop_ = std::max(std::accumulate(t_x_stop_.begin(), t_x_stop_.end(), 0.0), std::accumulate(t_y_stop_.begin(), t_y_stop_.end(), 0.0));

		mtx.lock();
		eval_trajectory(X_switch_stop_,Y_switch_stop_,t_x_stop_,t_y_stop_,t_stop_,X_stop_);
		mtx.unlock();

		d_stop_ = (X_stop_.block(0,0,1,2) - X.block(0,0,1,2)).norm();
		d_goal_ = (X.block(0,0,1,2).transpose() - goal.head(2)).norm();

		// Prevents oscillation is our stopping distance is really small (low speed)
		saturate(d_stop_,0.1,d_stop_);

		if (d_stop_ >= d_goal_){
			// Need to stop
			stop = true;
		}
	}
}


void REACT::get_vels(Eigen::Vector3d local_goal, Eigen::MatrixXd X, double v, double& vx, double& vy){
	angle_2_goal_ = atan2( local_goal(1) - X(0,1), local_goal(0) - X(0,0));

	vx = v*cos(angle_2_goal_);
	vy = v*sin(angle_2_goal_);
}


void REACT::collision_check(Eigen::MatrixXd X, Eigen::MatrixXd Sorted_Goals, int goal_counter, double buff, double v, int partition, double& tf, bool& can_reach_goal){
	//Re-intialize
	can_reach_goal = false;
	collision_detected_ = false;
	min_d_ind = 0;
	ranges_ = Eigen::VectorXd::Zero(Sorted_Goals.cols());
	ranges_ = Sorted_Goals.col(3);
	X_prop_ = Eigen::MatrixXd::Zero(4,2);
	X_prop_ << X;

	current_local_goal_ << Sorted_Goals.block(goal_counter,0,1,3).transpose();
	
	mtx.lock();
	get_traj(X,current_local_goal_,v,t_x_,t_y_,X_switch_,Y_switch_,false);
	mtx.unlock();

	// Find closest obstacle (aka)
	d_min_ = ranges_.minCoeff(&min_d_ind);

	partition = Sorted_Goals.rows();

	// If the closest obstacle is the goal we're heading towards then we're good
	if (min_d_ind==goal_index_){
		can_reach_goal = true;
	} 
	// Something else is closer, need to prop to next time step
	else{
		// evaluate at time required to travel d_min
		t_ = std::max(buff/v,d_min_/v);

		while (!collision_detected_ && !can_reach_goal){
			mtx.lock();
			eval_trajectory(X_switch_,Y_switch_,t_x_,t_y_,t_,X_prop_);
			mtx.unlock();
			// Re-calculate ranges based on prop state
			for(int i=0;i<partition;i++){
				ranges_(i) = (Sorted_Goals.block(i,0,1,2)-X_prop_.row(0)).norm();
			}

			d_min_  = ranges_.minCoeff(&min_d_ind);

			// Check if the min distance is the current goal
			if (min_d_ind==goal_counter){
				can_reach_goal = true;
			}
			// Check if the distance is less than our buffer
			else if (d_min_ < buff){
				collision_detected_ = true;
				can_reach_goal = false;
			}
			// Neither have happened so propogate again
			else{
				t_ += d_min_/v;
			}
		}
	}
	tf = t_;
}


void REACT::find_times( Eigen::Vector4d x0, double vf, std::vector<double>& t, Eigen::Matrix4d&  X_switch, bool stop_check){
 	if (vf == x0(1)){
 		j_V_[0] = 0;
		j_V_[1] = 0; 
		j_V_[2] = 0;

		t[0] = 0;
		t[1] = 0; 
		t[2] = 0;

		a0_V_[0] = 0;
		a0_V_[1] = 0; 
		a0_V_[2] = 0;
		a0_V_[3] = x0(2);

		v0_V_[0] = 0;
		v0_V_[1] = 0; 
		v0_V_[2] = 0;
		v0_V_[3] = x0(1);		

		x0_V_[0] = 0;
		x0_V_[1] = 0; 
		x0_V_[2] = 0;
		x0_V_[3] = x0(0);
 	}
 	else{
		
		double j_temp;
		if (stop_check){
			j_temp = j_max_;
		}
		else{
			// Could be interesting, need to justify 
			if (std::abs(vf-x0(1))/v_max_ < 0.2 && std::abs(x0(3)) != j_max_){
				j_temp = 5;
			}
			else{
				j_temp = j_max_;
			}
			// j_temp = std::min(j_max_/(0.5*v_max_)*std::abs(vf-x0(1)),j_max_);
		}
		j_temp = copysign(j_temp,vf-x0(1));

		double vfp = x0(1) + pow(x0(2),2)/(2*j_temp);

		if (std::abs(vfp-vf) < 0.05*std::abs(vf)){

			j_V_[0] = -j_temp;
			// No 2nd and 3rd stage
			j_V_[1] = 0;
			j_V_[2] = 0;

			t[0] = -x0(2)/j_V_[0];
			// No 2nd and 3rd stage
			t[1] = 0;
			t[2] = 0;

			v0_V_[0] = x0(1);
			// No 2nd and 3rd stage
			v0_V_[1] = 0;
			v0_V_[2] = 0;
			v0_V_[3] = vf;

			x0_V_[0] = x0(0);
			// No 2nd and 3rd stage
			x0_V_[1] = 0;
			x0_V_[2] = 0;
			x0_V_[3] = x0_V_[0] + v0_V_[0]*t[0];

			a0_V_[0] = x0(2);
			// No 2nd and 3rd stage
			a0_V_[1] = 0;
			a0_V_[2] = 0;
			a0_V_[3] = 0;
		}

		else{
			j_V_[0] = j_temp;
			j_V_[1] = 0;
			j_V_[2] = -j_temp;

			double t1 = -x0(2)/j_temp + std::sqrt(0.5*pow(x0(2),2) - j_temp*(x0(1)-vf))/j_temp;
			double t2 = -x0(2)/j_temp - std::sqrt(0.5*pow(x0(2),2) - j_temp*(x0(1)-vf))/j_temp;

			t1 = std::max(t1,t2);

			// Check to see if we'll saturate
			double a1f = x0(2) + j_temp*t1;

			if (std::abs(a1f) >= a_max_){
				double am = copysign(a_max_,j_temp);
				t[0] = (am-x0(2))/j_V_[0];
				t[2] = -am/j_V_[2];

				a0_V_[0] = x0(2);
				a0_V_[1] = a0_V_[0] + j_V_[0]*t[0];
				a0_V_[2] = am;
				a0_V_[3] = 0;

				v0_V_[0] = x0(1);
				v0_V_[1] = v0_V_[0] + a0_V_[0]*t[0] + 0.5*j_V_[0]*pow(t[0],2);	
				v0_V_[2] = vf - am*t[2] - 0.5*j_V_[2]*pow(t[2],2);
				v0_V_[3] = vf;

				t[1] = (v0_V_[2]-v0_V_[1])/am;		

				x0_V_[0] = x0(0);
				x0_V_[1] = x0_V_[0] + v0_V_[0]*t[0] + 0.5*a0_V_[0]*pow(t[0],2) + 1./6*j_V_[0]*pow(t[0],3);
				x0_V_[2] = x0_V_[1] + v0_V_[1]*t[1] + 0.5*am*pow(t[1],2) ;
				x0_V_[3] = x0_V_[2] + v0_V_[2]*t[2] + 0.5*am*pow(t[2],2) + 1./6*j_V_[2]*pow(t[2],3);

			}
			else{
				j_V_[0] = j_temp;
				j_V_[1] = 0; // No second phase
				j_V_[2] = -j_temp;

				t[0] = t1;
				t[1] = 0; // No second phase
				t[2] = -(x0(2)+j_V_[0]*t1)/j_V_[2];

				a0_V_[0] = x0(2);
				a0_V_[1] = 0; // No second phase
				a0_V_[2] = a0_V_[0] + j_V_[0]*t[0];
				a0_V_[3] = 0;

				v0_V_[0] = x0(1);
				v0_V_[1] = 0; // No second phase
				v0_V_[2] = v0_V_[0] + a0_V_[0]*t[0] + 0.5*j_V_[0]*pow(t[0],2);
				v0_V_[3] = vf;		

				x0_V_[0] = x0(0);
				x0_V_[1] = 0; // No second phase
				x0_V_[2] = x0_V_[0] + v0_V_[0]*t[0] + 0.5*a0_V_[0]*pow(t[0],2) + 1./6*j_V_[0]*pow(t[0],3);
				x0_V_[3] = x0_V_[2] + v0_V_[2]*t[2] + 0.5*a0_V_[2]*pow(t[2],2) + 1./6*j_V_[2]*pow(t[2],3);
			}
		}
	}

	X_switch.row(0) << x0_V_[0],x0_V_[1],x0_V_[2],x0_V_[3];
	X_switch.row(1) << v0_V_[0],v0_V_[1],v0_V_[2],v0_V_[3];
	X_switch.row(2) << a0_V_[0],a0_V_[1],a0_V_[2],a0_V_[3];
	X_switch.row(3) << j_V_[0],j_V_[1],j_V_[2],j_V_[3];
}


void REACT::eval_trajectory(Eigen::Matrix4d X_switch, Eigen::Matrix4d Y_switch, std::vector<double> t_x, std::vector<double> t_y, double t, Eigen::MatrixXd& Xc){
	// Eval x trajectory
	int k = 0;
	if (t < t_x[0]){
		k = 0;
		tx_ = t;
	}
	else if (t < t_x[0]+t_x[1]){
		tx_ = t - t_x[0];
		k = 1;
	}
	else if (t < t_x[0]+t_x[1]+t_x[2]){
		tx_ = t - (t_x[0]+t_x[1]);
		k = 2;
	}
	else{
		tx_ = t - (t_x[0]+t_x[1]+t_x[2]);
		k = 3;
	}

	// Eval y trajectory
	int l = 0;
	if (t < t_y[0]){
		ty_ = t;
		l = 0;
	}
	else if (t < t_y[0]+t_y[1]){
		ty_ = t - t_y[0];
		l = 1;
	}
	else if (t < t_y[0]+t_y[1]+t_y[2]){
		ty_ = t - (t_y[0]+t_y[1]);
		l = 2;
	}
	else{
		ty_ = t - (t_y[0]+t_y[1]+t_y[2]);
		l = 3;
	}

	Xc(0,0) = X_switch(0,k) + X_switch(1,k)*tx_ + 0.5*X_switch(2,k)*pow(tx_,2) + 1.0/6.0*X_switch(3,k)*pow(tx_,3);
	Xc(1,0) = X_switch(1,k) + X_switch(2,k)*tx_ + 0.5*X_switch(3,k)*pow(tx_,2);
	Xc(2,0) = X_switch(2,k) + X_switch(3,k)*tx_;
	Xc(3,0) = X_switch(3,k);

	Xc(0,1) = Y_switch(0,l) + Y_switch(1,l)*ty_ + 0.5*Y_switch(2,l)*pow(ty_,2) + 1.0/6.0*Y_switch(3,l)*pow(ty_,3);
	Xc(1,1) = Y_switch(1,l) + Y_switch(2,l)*ty_ + 0.5*Y_switch(3,l)*pow(ty_,2);
	Xc(2,1) = Y_switch(2,l) + Y_switch(3,l)*ty_;
	Xc(3,1) = Y_switch(3,l);
}


void REACT::convert2pcl(const sensor_msgs::PointCloud2ConstPtr msg,pcl::PointCloud<pcl::PointXYZ>::Ptr &cloud_out){
	pcl::PCLPointCloud2* cloud2 = new pcl::PCLPointCloud2; 
	pcl_conversions::toPCL(*msg, *cloud2);
	pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);
	pcl::fromPCLPointCloud2(*cloud2,*cloud);

	cloud_out = cloud;
}


void REACT::eigen2quadGoal(Eigen::MatrixXd Xc, acl_system::QuadGoal& quad_goal){
 	quad_goal_.pos.x = Xc(0,0);
 	quad_goal_.vel.x = Xc(1,0);
 	quad_goal_.accel.x = Xc(2,0);
 	quad_goal_.jerk.x = Xc(3,0);

 	quad_goal_.pos.y = Xc(0,1);
 	quad_goal_.vel.y = Xc(1,1);
 	quad_goal_.accel.y = Xc(2,1);
 	quad_goal_.jerk.y = Xc(3,1);
 }



void REACT::takeoff(){
	quad_goal_.pos.z+=0.003;
	saturate(quad_goal_.pos.z,-0.1,goal_(2));
}


void REACT::land(){
	if (quad_goal_.pos.z > 0.4){
		quad_goal_.pos.z-=0.003;
		saturate(quad_goal_.pos.z,-0.1,goal_(2));
	}
	else{
		quad_goal_.pos.z-=0.001;
		saturate(quad_goal_.pos.z,-0.1,goal_(2));
	}
}

void REACT::saturate(double &var, double min, double max){
	if (var < min){
		var = min;
	}
	else if (var > max){
		var = max;
	}
}

void REACT::convert2ROS(Eigen::MatrixXd Goals){
 	// Cluster
 	goal_points_ros_.poses.clear();
 	goal_points_ros_.header.stamp = ros::Time::now();
 	goal_points_ros_.header.frame_id = "vicon";

 	for (int i=0; i < Goals.rows(); i++){
 		temp_goal_point_ros_.position.x = Goals(i,0);
 		temp_goal_point_ros_.position.y = Goals(i,1);
 		temp_goal_point_ros_.position.z = Goals(i,2);

 		temp_goal_point_ros_.orientation.w = cos(Goals(i,4)/2) ;
 		temp_goal_point_ros_.orientation.z = sin(Goals(i,4)/2);

 		goal_points_ros_.poses.push_back(temp_goal_point_ros_);
	}

	// Trajectory
	traj_ros_.poses.clear();
	traj_ros_.header.stamp = ros::Time::now();
	traj_ros_.header.frame_id = "vicon";

	dt_ = tf_/num_;
	t_ = 0;
	XE_ << X_;
	for(int i=0; i<num_; i++){
		mtx.lock();
		eval_trajectory(Xf_switch_,Yf_switch_,t_xf_,t_yf_,t_,XE_);
		mtx.unlock();
		temp_path_point_ros_.pose.position.x = XE_(0,0);
		temp_path_point_ros_.pose.position.y = XE_(0,1);
		temp_path_point_ros_.pose.position.z = goal_(2);
		t_+=dt_;
		traj_ros_.poses.push_back(temp_path_point_ros_);
	}

	ros_new_global_goal_.header.stamp = ros::Time::now();
	ros_new_global_goal_.header.frame_id = "vicon";
	ros_new_global_goal_.point.x = local_goal_(0);
	ros_new_global_goal_.point.y = local_goal_(1);
	ros_new_global_goal_.point.z = local_goal_(2);

	ros_last_global_goal_.header.stamp = ros::Time::now();
	ros_last_global_goal_.header.frame_id = "vicon";
	ros_last_global_goal_.point.x = last_goal_(0);
	ros_last_global_goal_.point.y = last_goal_(1);
	ros_last_global_goal_.point.z = last_goal_(2);

	new_goal_pub.publish(ros_new_global_goal_);
	last_goal_pub.publish(ros_last_global_goal_);

 }

void REACT::pubROS(){
	int_goal_pub.publish(goal_points_ros_);
	traj_pub.publish(traj_ros_);
	latency_pub.publish(latency_);
}
