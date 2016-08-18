#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <iterator>
#include <ctime>

#include <pcl/point_types.h>
#include <sensor_msgs/PointCloud2.h>
#include <pcl_conversions/pcl_conversions.h>
#include <tf/transform_listener.h>
#include "pcl_ros/transforms.h"

#include <octomap/octomap.h>
#include <ros/ros.h>
#include <pcl_ros/point_cloud.h>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>
#include <geometry_msgs/Pose.h>
#include <octomap_msgs/Octomap.h>
#include <octomap_msgs/conversions.h>
#include <octomap_msgs/GetOctomap.h>
#include "navigation_utils.h"
#include <ros/callback_queue.h>


using namespace std;
// using namespace std::chrono;

typedef octomap::point3d point3d;
typedef pcl::PointXYZ PointType;
typedef pcl::PointCloud<pcl::PointXYZ> PointCloud;
const double PI = 3.1415926;
const double free_prob = 0.3; // what is this?##########
const double octo_reso = 0.1; // what is this?##########

octomap::OcTree* cur_tree;  // include sensor date, sensor position and sensor_max_range
octomap::OcTree* cur_tree_2d;

bool octomap_flag = 0; // 0 : msg not received
bool kinect_flag = 0; // 0 : msg not received
tf::TransformListener *tf_listener; 
int octomap_seq = 0;
   
point3d position, laser_orig, velo_orig; // sensor positon

ofstream explo_log_file; //what's that? #########
std::string octomap_name_2d, octomap_name_3d;


struct SensorModel {
    double horizontal_fov;
    double vertical_fov;
    double angle_inc_hor;
    double angle_inc_vel;
    double width;
    double height;
    double max_range;
    vector<pair<double, double>> pitch_yaws;
    octomap::Pointcloud SensorRays;
    point3d InitialVector;

    SensorModel(double _width, double _height, double _horizontal_fov, double _vertical_fov, double _max_range)
            : width(_width), height(_height), horizontal_fov(_horizontal_fov), vertical_fov(_vertical_fov), max_range(_max_range) {
        angle_inc_hor = horizontal_fov / width;
        angle_inc_vel = vertical_fov / height;
        for(double j = -height / 2; j < height / 2; ++j) 
            for(double i = -width / 2; i < width / 2; ++i) {
                InitialVector = point3d(1.0, 0.0, 0.0);
                InitialVector.rotate_IP(0.0, j * angle_inc_vel, i * angle_inc_hor);
                SensorRays.push_back(InitialVector);
                // pitch_yaws.push_back(make_pair(j * angle_inc_vel, i * angle_inc_hor));
                // pitch_yaws.push_back(make_pair(j * angle_inc_vel, i * angle_inc_hor));
        }
    }
}; 

// Establish sensor kinect
SensorModel Kinect_360(64, 48, 2*PI*57/360, 2*PI*43/360, 10);

//entropy Input: octree   Output:volume
double get_free_volume(const octomap::OcTree *octree) {
    double volume = 0;
    for(octomap::OcTree::leaf_iterator n = octree->begin_leafs(octree->getTreeDepth()); n != octree->end_leafs(); ++n) {
        if(!octree->isNodeOccupied(*n))
            volume += pow(n.getSize(), 3);
    }
    return volume;
}

// Input: octree, position, direction. Output: hits
octomap::Pointcloud cast_sensor_rays(const octomap::OcTree *octree, const point3d &position,
                                 const point3d &direction) {
    octomap::Pointcloud hits;

    octomap::Pointcloud SensorRays_copy;
    SensorRays_copy.push_back(Kinect_360.SensorRays);// what is SensorRay?#########
    SensorRays_copy.rotate(0.0,0.0,direction.z());
    point3d end;
    // #pragma omp parallel for
    for(int i = 0; i < SensorRays_copy.size(); i++) {
        if(octree->castRay(position, SensorRays_copy.getPoint(i), end, true, Kinect_360.max_range)) {
            // hits.push_back(end);
        } else {
            end = SensorRays_copy.getPoint(i) * Kinect_360.max_range;
            end += position;
            hits.push_back(end);
        }
    }
    return hits;
}

vector<vector<point3d>> generate_frontier_points(const octomap::OcTree *octree) {

    /*vector<point3d> frontier_points;
    octomap::OcTreeNode *n_cur_frontier;
    bool frontier_true;
    unsigned long int num_free;

    // find frontier points#############################
    for(octomap::OcTree::leaf_iterator n = octree->begin_leafs(octree->getTreeDepth()); n != octree->end_leafs(); ++n)
    {
        frontier_true = false;
        num_free = 0;

      if(!cur_tree_2d->isNodeOccupied(*n))
        {
         double x_cur = n.getX();
         double y_cur = n.getY();
         double z_cur = n.getZ();
         for (double x_cur_buf = x_cur - 0.1; x_cur_buf < x_cur + 0.15; x_cur_buf += octo_reso)
             for (double y_cur_buf = y_cur - 0.1; y_cur_buf < y_cur + 0.15; y_cur_buf += octo_reso)
                 //for (double z_cur_buf = z_cur - 0.05; z_cur_buf < z_cur + 0.05; z_cur_buf += octo_reso/2)
            {
                n_cur_frontier = cur_tree_2d->search(point3d(x_cur_buf, y_cur_buf, z_cur));
                if(!n_cur_frontier)
                {
                    frontier_true = true;
                   // frontier_points.push_back(point3d(x_cur,y_cur,z_cur));            
                }
                else if (!cur_tree_2d->isNodeOccupied(n_cur_frontier))
                {
                    num_free = num_free+1;

                }

            }
            if(frontier_true && num_free >5 )
            {
                frontier_points.push_back(point3d(x_cur,y_cur,z_cur));
            } 
        }
    }*/
    vector<vector<point3d>> frontier_lines;
    vector<point3d> frontier_points;
    octomap::OcTreeNode *n_cur_frontier;
    bool frontier_true;
    bool belong_old;
    double distance;
    //double x_frontier;
    //double y_frontier;
    //double z_frontier;

    for(octomap::OcTree::leaf_iterator n = octree->begin_leafs(octree->getTreeDepth()); n != octree->end_leafs(); ++n)
    {
        frontier_true = false;
        unsigned long int num_free = 0;

      if(!cur_tree_2d->isNodeOccupied(*n))
        {
         double x_cur = n.getX();
         double y_cur = n.getY();
         double z_cur = n.getZ();
         for (double x_cur_buf = x_cur - 0.1; x_cur_buf < x_cur + 0.15; x_cur_buf += octo_reso)
             for (double y_cur_buf = y_cur - 0.1; y_cur_buf < y_cur + 0.15; y_cur_buf += octo_reso)
                 //for (double z_cur_buf = z_cur - 0.05; z_cur_buf < z_cur + 0.05; z_cur_buf += octo_reso/2)
            {
                n_cur_frontier = cur_tree_2d->search(point3d(x_cur_buf, y_cur_buf, z_cur));
                if(!n_cur_frontier)
                {
                    frontier_true = true;
                   // frontier_points.push_back(point3d(x_cur,y_cur,z_cur));            
                }
                else if (!cur_tree_2d->isNodeOccupied(n_cur_frontier))
                {
                    num_free++;

                }

            }
            if(frontier_true && num_free >5 )
            {

                double x_frontier = x_cur;
                double y_frontier = y_cur;
                double z_frontier = z_cur;

                if(frontier_lines.size() < 1)
                {
                    frontier_points.resize(1);
                    frontier_points[0] = point3d(x_frontier,y_frontier,z_frontier);
                    frontier_lines.push_back(frontier_points);
                    frontier_points.clear();
                }
                else
                {
                    bool belong_old = false;
                    //unsigned long int b;
            

                    for(vector<vector<point3d>>::size_type u = 0; u < frontier_lines.size(); u++){
                        //b++;
                        //for(vector<point3d>::size_type v = 0; v < frontier_lines[u].size(); v++){
                            //frontier_lines.push_back(vector<point3d>());
                            distance = sqrt(pow(frontier_lines[u][0].x()-x_frontier, 2)+pow(frontier_lines[u][0].y()-y_frontier, 2)) ;
                            //distance = pow(distance, 0.5);
                           // ROS_INFO("Distance is %lf", distance);
                            if(distance < 0.3){
                               frontier_lines[u].push_back(point3d(x_frontier, y_frontier, z_frontier));
                               belong_old = true;
                               break;
                               //goto next_loop;
                            }
                        //}
                    }
                    //next_loop:
                    //ROS_INFO("finish %ld points ", b);
                    if(!belong_old){
                               frontier_points.resize(1);
                               frontier_points[0] = point3d(x_frontier, y_frontier, z_frontier);
                               frontier_lines.push_back(frontier_points);
                               frontier_points.clear();
                    }
                               


                }

            } 
        }
        
    }
    




    return frontier_lines;
    //##################################################
}








//generate candidates for moving. Input sensor_orig and initial_yaw, Output candidates
//senor_orig: locationg of sensor.   initial_yaw: yaw direction of sensor
vector<pair<point3d, point3d>> generate_candidates(vector<vector<point3d>> frontier_lines, point3d sensor_orig ) {
    double R = 0.2;   // Robot step, in meters.
    double n = 12;
    octomap::OcTreeNode *n_cur; // What is *n_cur################
    vector<pair<point3d, point3d>> candidates;
    double z = sensor_orig.z();                // fixed 
    double x, y;
    //double x_sum, y_sum;
    //double x_yaw, y_yaw;
    double yaw;

    //##################################################


    // for(z = sensor_orig.z() - 1; z <= sensor_orig.z() + 1; z += 1)
        //for(double yaw = initial_yaw-PI; yaw < initial_yaw+PI; yaw += PI*2 / n ) {
        for(vector<vector<point3d>>::size_type u = 0; u < frontier_lines.size(); u++){

            double initial_yaw = atan2(sensor_orig.y()-frontier_lines[u][0].y(), sensor_orig.x() - frontier_lines[u][0].x());

            for(double yaw = initial_yaw; yaw < initial_yaw+2*PI; yaw += PI*2 / n){
                x = frontier_lines[u][0].x() + R * cos(yaw);
                y = frontier_lines[u][0].y() + R * sin(yaw);

            
                /*x_sum = 0;
                y_sum = 0;
                for(vector<point3d>::size_type v = 0; v < frontier_lines[u].size(); v++){
                    x_sum = x_sum + frontier_lines[u][v].x();
                    y_sum = y_sum + frontier_lines[u][v].y();
                }
                x_yaw = x_sum/frontier_lines.size();
                y_yaw = y_sum/frontier_lines.size();
                x = (sensor_orig.x()*2 + x_sum)/(frontier_lines[u].size() + 2);
                y = (sensor_orig.y()*2 + y_sum)/(frontier_lines[u].size() + 2);
                yaw = atan2(y - y_yaw, x - x_yaw);*/
                //x = sensor_orig.x() + R * cos(yaw);
                //y = sensor_orig.y() + R * sin(yaw);


                // for every candidate goal, check surroundings
                bool candidate_valid = true;
                for (double x_buf = x - 0.2; x_buf < x + 0.2; x_buf += octo_reso/2) 
                    for (double y_buf = y - 0.2; y_buf < y + 0.2; y_buf += octo_reso/2)
                        for (double z_buf = z - 0.2; z_buf < z + 0.2; z_buf += octo_reso/2)
                {
                    n_cur = cur_tree_2d->search(point3d(x_buf, y_buf, z_buf));
                    if(!n_cur) {
                         ROS_WARN("Part of (%f, %f, %f) unknown", x_buf, y_buf, z_buf);
                         candidate_valid = false; // changede##########################
                    
                    }                                 
                    else if(cur_tree_2d->isNodeOccupied(n_cur)) {
                            // ROS_WARN("Part of (%f, %f, %f) occupied", x_buf, y_buf, z_buf);
                            candidate_valid = false;
                    }  
                }
                if (candidate_valid)
                {
                    candidates.push_back(make_pair<point3d, point3d>(point3d(x, y, z), point3d(0.0, 0.0, yaw)));
                }
                else
                    ROS_WARN("Part of Candidtae(%f, %f, %f) occupied", x, y, z);
            }
            
        }
    return candidates;
}
// Calculate Mutual Information. Input: octree, sensor_orig, hits, before
double calc_MI(const octomap::OcTree *octree, const point3d &sensor_orig, const octomap::Pointcloud &hits, const double before) {
    auto octree_copy = new octomap::OcTree(*octree);

    octree_copy->insertPointCloud(hits, sensor_orig, Kinect_360.max_range, true, true);
    double after = get_free_volume(octree_copy);
    delete octree_copy;
    return after - before;
}

// void RPY2Quaternion(double roll, double pitch, double yaw, double *x, double *y, double *z, double *w) {
//     double cr2, cp2, cy2, sr2, sp2, sy2;
//     cr2 = cos(roll*0.5);
//     cp2 = cos(pitch*0.5);
//     cy2 = cos(yaw*0.5);

//     sr2 = -sin(roll*0.5);
//     sp2 = -sin(pitch*0.5);
//     sy2 = sin(yaw*0.5);

//     *w = cr2*cp2*cy2 + sr2*sp2*sy2;
//     *x = sr2*cp2*cy2 - cr2*sp2*sy2;
//     *y = cr2*sp2*cy2 + sr2*cp2*sy2;
//     *z = cr2*cp2*sy2 - sr2*sp2*cy2;
// }


void kinect_callbacks( const sensor_msgs::PointCloud2ConstPtr& cloud2_msg ) {  // need to change########
    pcl::PCLPointCloud2 cloud2;
    pcl_conversions::toPCL(*cloud2_msg, cloud2);
    PointCloud* cloud (new PointCloud);
    PointCloud* cloud_local (new PointCloud);
    pcl::fromPCLPointCloud2(cloud2,*cloud_local);
    octomap::Pointcloud hits;

    ros::Duration(0.07).sleep();
    while(!pcl_ros::transformPointCloud("/map", *cloud_local, *cloud, *tf_listener))
    {
        ros::Duration(0.01).sleep();
    }
    // hits.push_back(point3d(cloud->at(j).x, cloud->at(j).y, cloud->at(j).z));
    // Insert points into octomap one by one...
    for (int i = 1; i< cloud->width; i++)
    {
        for (int j = 1; j< cloud->height; j++)
        {
            if(isnan(cloud->at(i,j).x)) continue;
            if(cloud->at(i,j).z < -1.0)    continue;  
            hits.push_back(point3d(cloud->at(i,j).x, cloud->at(i,j).y, cloud->at(i,j).z));
            // cur_tree->insertRay(point3d( velo_orig.x(),velo_orig.y(),velo_orig.z()), 
            //     point3d(cloud->at(j).x, cloud->at(j).y, cloud->at(j).z), Velodyne_puck.max_range);
        }
    }

    cur_tree->insertPointCloud(hits, velo_orig, Kinect_360.max_range);
    // cur_tree->updateInnerOccupancy();
    ROS_INFO("Entropy(3d map) : %f", get_free_volume(cur_tree));

    cur_tree->write(octomap_name_3d);
    delete cloud;
    delete cloud_local;
}

void hokuyo_callbacks( const sensor_msgs::PointCloud2ConstPtr& cloud2_msg )
{
    pcl::PCLPointCloud2 cloud2;
    pcl_conversions::toPCL(*cloud2_msg, cloud2);
    PointCloud* cloud (new PointCloud);
    PointCloud* cloud_local (new PointCloud);
    pcl::fromPCLPointCloud2(cloud2,*cloud_local);
    octomap::Pointcloud hits;

    ros::Duration(0.07).sleep();
    while(!pcl_ros::transformPointCloud("/map", *cloud_local, *cloud, *tf_listener))
    {
        ros::Duration(0.01).sleep();
    }

    // Insert points into octomap one by one...
    for (int j = 1; j< cloud->width; j++)
    {
        // if(isnan(cloud->at(j).x)) continue;
        hits.push_back(point3d(cloud->at(j).x, cloud->at(j).y, cloud->at(j).z));
        // cur_tree_2d->insertRay(point3d( laser_orig.x(),laser_orig.y(),laser_orig.z()), 
        //     point3d(cloud->at(j).x, cloud->at(j).y, cloud->at(j).z), 30.0);
    }
    cur_tree_2d->insertPointCloud(hits, laser_orig, 5.6);
    // cur_tree_2d->updateInnerOccupancy();
    ROS_INFO("Entropy(2d map) : %f", get_free_volume(cur_tree_2d));
    cur_tree_2d->write(octomap_name_2d);
    delete cloud;
    delete cloud_local;

}

int main(int argc, char **argv) {
    ros::init(argc, argv, "explo_sam_2d_turtlebot");
    ros::NodeHandle nh;
    // ros::Subscriber octomap_sub;
    // octomap_sub = nh.subscribe<octomap_msgs::Octomap>("/octomap_binary", 10, octomap_callback);

    // Initialize time
    time_t rawtime;
    struct tm * timeinfo;
    char buffer[80];
    time (&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buffer,80,"Trajectory_%R_%S_%m%d_DA.txt",timeinfo);
    std::string logfilename(buffer);
    std::cout << logfilename << endl;
    strftime(buffer,80,"octomap_2d_%R_%S_%m%d_DA.ot",timeinfo);
    octomap_name_2d = buffer;
    strftime(buffer,80,"octomap_3d_%R_%S_%m%d_DA.ot",timeinfo);
    octomap_name_3d = buffer;


    ros::Subscriber kinect_sub = nh.subscribe<sensor_msgs::PointCloud2>("/camera/depth_registered/points", 1, kinect_callbacks);// need to change##########
    ros::Subscriber hokuyo_sub = nh.subscribe<sensor_msgs::PointCloud2>("/hokuyo_points", 1, hokuyo_callbacks);// need to change#############
    ros::Publisher GoalMarker_pub = nh.advertise<visualization_msgs::Marker>( "Goal_Marker", 1 );
    ros::Publisher JackalMarker_pub = nh.advertise<visualization_msgs::Marker>( "Jackal_Marker", 1 );
    ros::Publisher Candidates_pub = nh.advertise<visualization_msgs::MarkerArray>("Candidate_MIs", 1);
    ros::Publisher Octomap_marker_pub = nh.advertise<visualization_msgs::Marker>("Occupied_MarkerArray", 1);
    ros::Publisher Frontier_points_pub = nh.advertise<visualization_msgs::Marker>("Frontier_points", 1);//changed here#######
    ros::Publisher Free_marker_pub = nh.advertise<visualization_msgs::Marker>("Free_MarkerArray", 1);


    tf_listener = new tf::TransformListener();
    tf::StampedTransform transform;
    tf::Quaternion Goal_heading; // robot's heading direction

    visualization_msgs::MarkerArray CandidatesMarker_array;
    visualization_msgs::Marker OctomapOccupied_cubelist;
    //visualization_msgs::Marker Frontier_points_cubelist;
    //visualization_msgs::Marker Frontier_points_delete;
    visualization_msgs::Marker Free_cubelist;
    //bool frontier_old;

    
    ros::Time now_marker = ros::Time::now();
   
    double R_velo, P_velo, Y_velo;

    // Initialize parameters 
    // ros::Rate r(10); // 1 hz
    int max_idx = 0;

    position = point3d(0, 0, 0.0);
    point3d Sensor_PrincipalAxis(1, 0, 0);
    octomap::OcTreeNode *n;
    octomap::OcTree new_tree(octo_reso);
    octomap::OcTree new_tree_2d(octo_reso);
    cur_tree = &new_tree;
    cur_tree_2d = &new_tree_2d;

    bool got_tf = false;

    //Update the pose of velodyne from predefined tf.
    got_tf = false;
    while(!got_tf){
    try{
        tf_listener->lookupTransform("/base_link", "/camera_rgb_frame", ros::Time(0), transform);// need to change tf of kinect###############
        velo_orig = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
        tf::Matrix3x3(transform.getRotation()).getRPY(R_velo, P_velo, Y_velo);
        Sensor_PrincipalAxis.rotate_IP(R_velo, P_velo, Y_velo);
        ROS_INFO("Current Kinect heading: vector(%2.2f, %2.2f, %2.2f) -  RPY(%3.1f, %3.1f, %3.1f).", Sensor_PrincipalAxis.x(), Sensor_PrincipalAxis.y(), Sensor_PrincipalAxis.z(), R_velo/PI*180.0, P_velo/PI*180.0, Y_velo/PI*180.0);
        got_tf = true;
        }
    catch (tf::TransformException ex) {
        ROS_WARN("Wait for tf: initial pose of Kinect"); 
        ros::Duration(0.05).sleep();
        } 
    }   
    
     //Rotate Sensor Model based on Velodyn Pose
    Kinect_360.SensorRays.rotate(R_velo, P_velo, Y_velo);
    
    // Update the initial location of the robot
    got_tf = false;
    while(!got_tf){
    try{
        tf_listener->lookupTransform("/map", "/camera_rgb_frame", ros::Time(0), transform);// need to change tf of kinect##############
        velo_orig = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
        got_tf = true;
    }
    catch (tf::TransformException ex) {
        ROS_WARN("Wait for tf: Kinect to map"); 
    } 
    ros::Duration(0.05).sleep();
    }

    got_tf = false;
    while(!got_tf){
    try{
        tf_listener->lookupTransform("/map", "/laser", ros::Time(0), transform);
        laser_orig = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
        got_tf = true;
    }
    catch (tf::TransformException ex) {
        ROS_WARN("Wait for tf: laser to map"); 
    } 
    ros::Duration(0.05).sleep();
    }
    ROS_INFO("Initial  Position : %3.2f, %3.2f, %3.2f - Yaw : %3.1f ", laser_orig.x(), laser_orig.y(), laser_orig.z(), transform.getRotation().getAngle()*PI/180);
    // Take a Initial Scan
    ros::spinOnce();

    // Rotate ~60 degrees 
    point3d next_vp(laser_orig.x(), laser_orig.y(),laser_orig.z());
    Goal_heading.setRPY(0.0, 0.0, transform.getRotation().getAngle()-0.5233); // why 0.5233?###
    Goal_heading.normalize();
    bool arrived = goToDest(laser_orig, Goal_heading);

    // Update the pose of the robot
    got_tf = false;
    while(!got_tf){
    try{
        tf_listener->lookupTransform("/map", "/camera_rgb_frame", ros::Time(0), transform);// need to change tf of kinect###############
        velo_orig = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
        got_tf = true;
    }
    catch (tf::TransformException ex) {
        ROS_WARN("Wait for tf: velodyne to map"); 
    } 
    ros::Duration(0.05).sleep();
    }

    got_tf = false;
    while(!got_tf){
    try{
        tf_listener->lookupTransform("/map", "/laser", ros::Time(0), transform);
        laser_orig = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
        got_tf = true;
    }
    catch (tf::TransformException ex) {
        ROS_WARN("Wait for tf: laser to map"); 
    } 
    ros::Duration(0.05).sleep();
    }

    // Take a Second Scan
    ros::spinOnce();

    // Rotate another 60 degrees
    Goal_heading.setRPY(0.0, 0.0, transform.getRotation().getAngle()-0.5233);
    arrived = goToDest(laser_orig, Goal_heading);

    // Update the pose of the robot
    got_tf = false;
    while(!got_tf){
    try{
        tf_listener->lookupTransform("/map", "/camera_rgb_frame", ros::Time(0), transform);// need to change tf of kinect###############
        velo_orig = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
        got_tf = true;
    }
    catch (tf::TransformException ex) {
        ROS_WARN("Wait for tf: velodyne to map"); 
    } 
    ros::Duration(0.05).sleep();
    }

    got_tf = false;
    while(!got_tf){
    try{
        tf_listener->lookupTransform("/map", "/laser", ros::Time(0), transform);
        laser_orig = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
        got_tf = true;
    }
    catch (tf::TransformException ex) {
        ROS_WARN("Wait for tf: laser to map"); 
    } 
    ros::Duration(0.05).sleep();
    }

    // Take a Third Scan
    ros::spinOnce();

    // Rotate another 60 degrees
    Goal_heading.setRPY(0.0, 0.0, transform.getRotation().getAngle()-0.5233);
    arrived = goToDest(laser_orig, Goal_heading);

    // Update the pose of the robot
    got_tf = false;
    while(!got_tf){
    try{
        tf_listener->lookupTransform("/map", "/camera_rgb_frame", ros::Time(0), transform);// need to change tf of kinect###############
        velo_orig = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
        got_tf = true;
    }
    catch (tf::TransformException ex) {
        ROS_WARN("Wait for tf: velodyne to map"); 
    } 
    ros::Duration(0.05).sleep();
    }

    got_tf = false;
    while(!got_tf){
    try{
        tf_listener->lookupTransform("/map", "/laser", ros::Time(0), transform);
        laser_orig = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
        got_tf = true;
    }
    catch (tf::TransformException ex) {
        ROS_WARN("Wait for tf: laser to map"); 
    } 
    ros::Duration(0.05).sleep();
    }
    // Take a Fourth Scan
    ros::spinOnce();

    // steps robot taken, counter
    int robot_step_counter = 0;

    while (ros::ok())
    {
                    // Publish frontier points#############
            //vector<point3d> frontier_points;
            //frontier_points.clear();
            //Frontier_points_cubelist.action=visualization_msgs::Marker::DELETE;
            //long int c = Frontier_points_cubelist.size();
            //ROS_INFO("Frontier has %ld points", c);
            //vector<point3d> frontier_points_old;
            //vector<point3d> frontier_points_delete;
            start_over:
            
            vector<vector<point3d>> frontier_lines=generate_frontier_points( cur_tree_2d );
            unsigned long int t = 0;
            
            visualization_msgs::Marker Frontier_points_cubelist[frontier_lines.size()];
            //try if really need to resize#########################################################
            //for(vector<vector<point3d>>::size_type n = 0; n < frontier_lines.size(); n++) {
                //Frontier_points_cubelist[n].points.resize(frontier_lines[n].size());
            //}
            
            
            for(vector<vector<point3d>>::size_type n = 0; n < frontier_lines.size(); n++) {
                //o = o+frontier_lines[n].size();
                Frontier_points_cubelist[n].points.resize(frontier_lines[n].size());

                //Frontier_points_cubelist.points.resize(frontier_lines[n].size());
                //ROS_INFO("frontier points %ld", o);
                now_marker = ros::Time::now();
                Frontier_points_cubelist[n].header.frame_id = "map";
                Frontier_points_cubelist[n].header.stamp = now_marker;
                Frontier_points_cubelist[n].ns = "frontier_points_array";
                Frontier_points_cubelist[n].id = 0;
                Frontier_points_cubelist[n].type = visualization_msgs::Marker::CUBE_LIST;
                Frontier_points_cubelist[n].action = visualization_msgs::Marker::ADD;
                Frontier_points_cubelist[n].scale.x = octo_reso;
                Frontier_points_cubelist[n].scale.y = octo_reso;
                Frontier_points_cubelist[n].scale.z = octo_reso;
                Frontier_points_cubelist[n].color.a = 1.0;
                Frontier_points_cubelist[n].color.r = (double)255/255;
                Frontier_points_cubelist[n].color.g = 0;
                Frontier_points_cubelist[n].color.b = (double)6*n/255;
                Frontier_points_cubelist[n].lifetime = ros::Duration();
                
                //int l = 0;
                geometry_msgs::Point q;
                for(vector<point3d>::size_type m = 0; m < frontier_lines[n].size(); m++){

                    q.x = frontier_lines[n][m].x();
                    q.y = frontier_lines[n][m].y();
                    q.z = frontier_lines[n][m].z()+octo_reso;
                    Frontier_points_cubelist[n].points.push_back(q);   

                }
                t++;
                Frontier_points_pub.publish(Frontier_points_cubelist[n]); //publish frontier_points############
                Frontier_points_cubelist[n].points.clear(); 
            }
            
            ROS_INFO("Publishing %ld frontier_lines", t);
            
            //int delete_points = 0;
            
            //frontier_lines.clear();//in the next line
            


        // Generate Candidates
        vector<pair<point3d, point3d>> candidates = generate_candidates(frontier_lines, laser_orig); // what is transform ?#####
        frontier_lines.clear();
        while(candidates.size() <= 1)
        {
            // Get the current heading
            got_tf = false;
            while(!got_tf){
            try{
                tf_listener->lookupTransform("/map", "/laser", ros::Time(0), transform);
                laser_orig = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
                got_tf = true;
            }
            catch (tf::TransformException ex) {
                ROS_WARN("Wait for tf: laser to map"); 
            } 
            ros::Duration(0.05).sleep();
            }
            // Rotate negative along Yaw for 30 deg to look for more open areas
            Goal_heading.setRPY(0.0, 0.0, transform.getRotation().getAngle() - PI/6); 
            Goal_heading.normalize();
            arrived = goToDest(laser_orig, Goal_heading); // order to move_base
            vector<pair<point3d, point3d>> candidates = generate_candidates(frontier_lines, laser_orig);
        }
        
        ROS_INFO("%lu candidates generated.", candidates.size());
        vector<double> MIs(candidates.size());
        double before = get_free_volume(cur_tree);
        max_idx = 0;
        long int max_order[candidates.size()];
        unsigned int p = 0;
        int m=0;

        // for every candidate...
        double Secs_CastRay, Secs_InsertRay, Secs_tmp;  //What are those? ####
        Secs_InsertRay = 0;
        Secs_CastRay = 0;

        #pragma omp parallel for
        for(int i = 0; i < candidates.size(); i++) 
        {   
            max_order[i] = i;
            auto c = candidates[i];
            // Evaluate Mutual Information
            Secs_tmp = ros::Time::now().toSec();
            Sensor_PrincipalAxis.rotate_IP(c.second.roll(), c.second.pitch(), c.second.yaw() );
            octomap::Pointcloud hits = cast_sensor_rays(cur_tree, c.first, Sensor_PrincipalAxis);  // what are those?#####
            Secs_CastRay += ros::Time::now().toSec() - Secs_tmp;
            Secs_tmp = ros::Time::now().toSec();
            MIs[i] = calc_MI(cur_tree, c.first, hits, before)-30*sqrt(pow(c.first.x()-laser_orig.x(), 2)+pow(c.first.y() - laser_orig.y(), 2));
            Secs_InsertRay += ros::Time::now().toSec() - Secs_tmp;
        }

        for(int j=0; j<candidates.size(); j++)
           {
            p=0;
           for(int m=0; m<candidates.size(); m++)
              {
            
              if (MIs[j] > MIs[m])
                 {
                 p++;
                 }
              }
           max_order[p] = j;
           }
       // ROS_INFO("max_order : %ld %ld %ld %ld %ld %ld %3.2f %3.2f %3.2f %3.2f %3.2f %3.2f", max_order[0], max_order[1], max_order[2], max_order[3], max_order[4], max_order[5], MIs[max_order[0]], MIs[max_order[1]], MIs[max_order[2]], MIs[max_order[3]], MIs[max_order[4]], MIs[max_order[5]]);
        
        p = candidates.size()-1;
        loop:
        max_idx = max_order[p];
 
        next_vp = point3d(candidates[max_idx].first.x(),candidates[max_idx].first.y(),candidates[max_idx].first.z());
        Goal_heading.setRPY(0.0, 0.0, candidates[max_idx].second.yaw());
        Goal_heading.normalize();
        ROS_INFO("Max MI : %f , @ location: %3.2f  %3.2f  %3.2f", MIs[max_idx], next_vp.x(), next_vp.y(), next_vp.z() );
        ROS_INFO("CastRay Time: %2.3f Secs. InsertRay Time: %2.3f Secs.", Secs_CastRay, Secs_InsertRay);

        // Publish the candidates as marker array in rviz
        tf::Quaternion MI_heading;
        MI_heading.setRPY(0.0, -PI/2, 0.0);
        MI_heading.normalize();
        
        CandidatesMarker_array.markers.resize(candidates.size());
        for (int i = 0; i < candidates.size(); i++)
        {
            CandidatesMarker_array.markers[i].header.frame_id = "map";
            CandidatesMarker_array.markers[i].header.stamp = ros::Time::now();
            CandidatesMarker_array.markers[i].ns = "candidates";
            CandidatesMarker_array.markers[i].id = i;
            CandidatesMarker_array.markers[i].type = visualization_msgs::Marker::ARROW;
            CandidatesMarker_array.markers[i].action = visualization_msgs::Marker::ADD;
            CandidatesMarker_array.markers[i].pose.position.x = candidates[i].first.x();
            CandidatesMarker_array.markers[i].pose.position.y = candidates[i].first.y();
            CandidatesMarker_array.markers[i].pose.position.z = candidates[i].first.z();
            CandidatesMarker_array.markers[i].pose.orientation.x = MI_heading.x();
            CandidatesMarker_array.markers[i].pose.orientation.y = MI_heading.y();
            CandidatesMarker_array.markers[i].pose.orientation.z = MI_heading.z();
            CandidatesMarker_array.markers[i].pose.orientation.w = MI_heading.w();
            CandidatesMarker_array.markers[i].scale.x = (double)MIs[i]/MIs[max_idx];
            CandidatesMarker_array.markers[i].scale.y = 0.05;
            CandidatesMarker_array.markers[i].scale.z = 0.05;
            CandidatesMarker_array.markers[i].color.a = (double)MIs[i]/MIs[max_idx];
            CandidatesMarker_array.markers[i].color.r = 0.0;
            CandidatesMarker_array.markers[i].color.g = 1.0;
            CandidatesMarker_array.markers[i].color.b = 0.0;
        }
        Candidates_pub.publish(CandidatesMarker_array); //publish candidates##########
        CandidatesMarker_array.markers.clear();
        candidates.clear();

        // Publish the goal as a Marker in rviz
        visualization_msgs::Marker marker;
        marker.header.frame_id = "map";
        marker.header.stamp = ros::Time();
        marker.ns = "goal_marker";
        marker.id = 0;
        marker.type = visualization_msgs::Marker::ARROW;
        marker.action = visualization_msgs::Marker::ADD;
        marker.pose.position.x = next_vp.x();
        marker.pose.position.y = next_vp.y();
        marker.pose.position.z = 1.0;
        marker.pose.orientation.x = Goal_heading.x();
        marker.pose.orientation.y = Goal_heading.y();
        marker.pose.orientation.z = Goal_heading.z();
        marker.pose.orientation.w = Goal_heading.w();
        marker.scale.x = 0.5;
        marker.scale.y = 0.1;
        marker.scale.z = 0.1;
        marker.color.a = 1.0; // Don't forget to set the alpha!
        marker.color.r = 1.0;
        marker.color.g = 1.0;
        marker.color.b = 0.0;
        GoalMarker_pub.publish( marker ); //publish goal##########

        // Send the Robot 
        Goal_heading.setRPY(0.0, 0.0, candidates[max_idx].second.yaw());
        arrived = goToDest(next_vp, Goal_heading); // What is goToDest ?######

        if(arrived)
        {
            // Update the initial location of the robot
            got_tf = false;
            while(!got_tf){
            try{
                tf_listener->lookupTransform("/map", "/camera_rgb_frame", ros::Time(0), transform);// need to change tf of kinect###############
                velo_orig = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
                got_tf = true;
            }
            catch (tf::TransformException ex) {
                ROS_WARN("Wait for tf: velodyne to map"); 
            } 
            ros::Duration(0.05).sleep();
            }

            got_tf = false;
            while(!got_tf){
            try{
                tf_listener->lookupTransform("/map", "/laser", ros::Time(0), transform);
                laser_orig = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
                got_tf = true;
            }
            catch (tf::TransformException ex) {
                ROS_WARN("Wait for tf: laser to map"); 
            } 
            ros::Duration(0.05).sleep();
            }

            // Update Octomap
            ros::spinOnce();
            ROS_INFO("Succeed, new Map Free Volume: %f", get_free_volume(cur_tree));
            robot_step_counter++;

            // Prepare the header for occupied array
            now_marker = ros::Time::now();
            OctomapOccupied_cubelist.header.frame_id = "map";
            OctomapOccupied_cubelist.header.stamp = now_marker;
            OctomapOccupied_cubelist.ns = "octomap_occupied_array";
            OctomapOccupied_cubelist.id = 0;
            OctomapOccupied_cubelist.type = visualization_msgs::Marker::CUBE_LIST;
            OctomapOccupied_cubelist.action = visualization_msgs::Marker::ADD;
            OctomapOccupied_cubelist.scale.x = octo_reso;
            OctomapOccupied_cubelist.scale.y = octo_reso;
            OctomapOccupied_cubelist.scale.z = octo_reso;
            OctomapOccupied_cubelist.color.a = 0.5;
            OctomapOccupied_cubelist.color.r = (double)19/255;
            OctomapOccupied_cubelist.color.g = (double)121/255;
            OctomapOccupied_cubelist.color.b = (double)156/255;

            unsigned long int j = 0;
            geometry_msgs::Point p;
            for(octomap::OcTree::leaf_iterator n = cur_tree_2d->begin_leafs(cur_tree_2d->getTreeDepth()); n != cur_tree_2d->end_leafs(); ++n) { // changed there#######
                if(!cur_tree_2d->isNodeOccupied(*n)) continue;
                p.x = n.getX();
                p.y = n.getY();
                p.z = n.getZ();
                OctomapOccupied_cubelist.points.push_back(p); 
                j++;
            }
            ROS_INFO("Publishing %ld occupied cells", j);
            Octomap_marker_pub.publish(OctomapOccupied_cubelist); //publish octomap############

            // Prepare the header for free array
            now_marker = ros::Time::now();
            Free_cubelist.header.frame_id = "map";
            Free_cubelist.header.stamp = now_marker;
            Free_cubelist.ns = "octomap_free_array";
            Free_cubelist.id = 0;
            Free_cubelist.type = visualization_msgs::Marker::CUBE_LIST;
            Free_cubelist.action = visualization_msgs::Marker::ADD;
            Free_cubelist.scale.x = octo_reso;
            Free_cubelist.scale.y = octo_reso;
            Free_cubelist.scale.z = octo_reso;
            Free_cubelist.color.a = 0.3;
            Free_cubelist.color.r = (double)102/255;
            Free_cubelist.color.g = (double)255/255;
            Free_cubelist.color.b = (double)102/255;

            unsigned long int w = 0;
            //geometry_msgs::Point p;
            for(octomap::OcTree::leaf_iterator n = cur_tree_2d->begin_leafs(cur_tree_2d->getTreeDepth()); n != cur_tree_2d->end_leafs(); ++n) { // changed there#######
                if(cur_tree_2d->isNodeOccupied(*n)) continue;
                p.x = n.getX();
                p.y = n.getY();
                p.z = n.getZ();
                Free_cubelist.points.push_back(p); 
                w++;
            }
            ROS_INFO("Publishing %ld free cells", w);
            Free_marker_pub.publish(Free_cubelist); //publish octomap############
            unsigned long int g;
       
            /*// Publish frontier points#############
            //vector<point3d> frontier_points;
            //frontier_points.clear();
            //Frontier_points_cubelist.action=visualization_msgs::Marker::DELETE;
            //long int c = Frontier_points_cubelist.size();
            //ROS_INFO("Frontier has %ld points", c);
            //vector<point3d> frontier_points_old;
            //vector<point3d> frontier_points_delete;
            vector<vector<point3d>> frontier_points=generate_frontier_points( cur_tree_2d );
            
            unsigned long int o = 0;
            for(vector<vector<point3d>>::size_type e = 0; e < frontier_points.size(); e++) {
                o = o+frontier_points[e].size();
            }

            Frontier_points_cubelist.points.resize(o);
            ROS_INFO("frontier points %ld", o);
            now_marker = ros::Time::now();
            Frontier_points_cubelist.header.frame_id = "map";
            Frontier_points_cubelist.header.stamp = now_marker;
            Frontier_points_cubelist.ns = "frontier_points_array";
            Frontier_points_cubelist.id = 0;
            Frontier_points_cubelist.type = visualization_msgs::Marker::CUBE_LIST;
            Frontier_points_cubelist.action = visualization_msgs::Marker::ADD;
            Frontier_points_cubelist.scale.x = octo_reso;
            Frontier_points_cubelist.scale.y = octo_reso;
            Frontier_points_cubelist.scale.z = octo_reso;
            Frontier_points_cubelist.color.a = 1.0;
            Frontier_points_cubelist.color.r = (double)255/255;
            Frontier_points_cubelist.color.g = 0;
            Frontier_points_cubelist.color.b = (double)0/255;
            Frontier_points_cubelist.lifetime = ros::Duration();

            unsigned long int t = 0;
            int l = 0;
            geometry_msgs::Point q;
            for(vector<vector<point3d>>::size_type n = 0; n < frontier_points.size(); n++) { // changed there#######
                for(vector<point3d>::size_type m = 0; m < frontier_points[n].size(); m++){


                   q.x = frontier_points[n][m].x();
                   q.y = frontier_points[n][m].y();
                   q.z = frontier_points[n][m].z()+octo_reso;
                   Frontier_points_cubelist.points.push_back(q); 
                   
                }
                t++;
            }
            ROS_INFO("Publishing %ld frontier_lines", t);
            
            //int delete_points = 0;
            Frontier_points_pub.publish(Frontier_points_cubelist); //publish frontier_points############
            frontier_points.clear();
            Frontier_points_cubelist.points.clear(); */


            // Send out results to file.
            explo_log_file.open(logfilename, std::ofstream::out | std::ofstream::app);
            explo_log_file << "DA Step: " << robot_step_counter << "  | Current Entropy: " << get_free_volume(cur_tree) << endl;
            explo_log_file.close();

        }
        else
        {
            ROS_ERROR("Failed to navigate to goal");
            // if max MI candidates can't be arrived, try second max MI candidates

            // Get the current heading
            got_tf = false;
            while(!got_tf){
            try{
                tf_listener->lookupTransform("/map", "/laser", ros::Time(0), transform);
                laser_orig = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
                got_tf = true;
            }
            catch (tf::TransformException ex) {
                ROS_WARN("Wait for tf: laser to map");
            }
            ros::Duration(0.05).sleep();
            }
            // Rotate negative along Yaw for 30 deg to look for more open areas
            Goal_heading.setRPY(0.0, 0.0, transform.getRotation().getAngle() - PI/6);
            Goal_heading.normalize();
            arrived = goToDest(laser_orig, Goal_heading); // order to move_base
            ros::spinOnce();
            

            if(p == 0){
               goto start_over;
            }
            p--;
            goto loop;
        }
        // r.sleep();
    }
    nh.shutdown();          
    return 0;
}