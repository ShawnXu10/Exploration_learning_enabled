#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <iterator>

#include <pcl/point_types.h>
#include <sensor_msgs/PointCloud2.h>
#include <pcl_conversions/pcl_conversions.h>
#include <tf/transform_listener.h>
#include "pcl_ros/transforms.h"

#include <octomap/octomap.h>
#include <ros/ros.h>
#include <pcl_ros/point_cloud.h>
#include <visualization_msgs/Marker.h>
#include <geometry_msgs/Pose.h>
#include <octomap_msgs/Octomap.h>
#include <octomap_msgs/conversions.h>
#include <octomap_msgs/GetOctomap.h>
#include <tf/transform_broadcaster.h>
#include "navigation_utils.h"


using namespace std;
using namespace std::chrono;


typedef octomap::point3d point3d;
typedef pcl::PointXYZ PointType;
typedef pcl::PointCloud<pcl::PointXYZ> PointCloud;
const double PI = 3.1415926;
const double free_prob = 0.3;
const double z_sensor = 0.2972;
octomap::OcTree *tree;
octomap::OcTree* cur_tree;
octomap::OcTree *tree_kinect;
bool octomap_flag = 0; // 0 : msg not received
bool kinect_flag = 0; // 0 : msg not received
tf::TransformListener *tf_listener; 
   
point3d position;
point3d orientation;

ros::Publisher Map_pcl_pub;
ros::Publisher Free_pcl_pub;
ros::Publisher VScan_pcl_pub;

PointCloud::Ptr map_pcl (new PointCloud);
PointCloud::Ptr free_pcl (new PointCloud);
PointCloud::Ptr vsn_pcl (new PointCloud);

struct SensorModel {
    double horizontal_fov;
    double vertical_fov;
    double angle_inc_hor;
    double angle_inc_vel;
    double width;
    double height;
    double max_range;
    vector<pair<double, double>> pitch_yaws;

    SensorModel(double _width, double _height, double _horizontal_fov, double _vertical_fov, double _max_range)
            : width(_width), height(_height), horizontal_fov(_horizontal_fov), vertical_fov(_vertical_fov), max_range(_max_range) {
        angle_inc_hor = horizontal_fov / width;
        angle_inc_vel = vertical_fov / height;
        for(double i = -width / 2; i < width / 2; ++i) {
            for(double j = -height / 2; j < height / 2; ++j) {
                pitch_yaws.push_back(make_pair(j * angle_inc_vel, i * angle_inc_hor));
            }
        }
    }
}; 

SensorModel InitialScan(1000, 1000, 6.0, 1.0, 15.0);
SensorModel kinect(640, 40, 3.047198, 1.5, 5.0);
SensorModel Velodyne_puck(3600, 16, 2*PI, 2/9*PI, 100.0 );

double get_free_volume(const octomap::OcTree *octree) {
    double volume = 0;
    for(octomap::OcTree::leaf_iterator n = octree->begin_leafs(octree->getTreeDepth()); n != octree->end_leafs(); ++n) {
        if(!octree->isNodeOccupied(*n))
            volume += pow(n.getSize(), 3);
    }
    return volume;
}

void get_free_points(const octomap::OcTree *octree, PointCloud::Ptr pclPtr) {
    
    for(octomap::OcTree::leaf_iterator n = octree->begin_leafs(octree->getTreeDepth()); n != octree->end_leafs(); ++n) {
        if(!octree->isNodeOccupied(*n))
        {
            pclPtr->points.push_back(pcl::PointXYZ(n.getX()/5.0, n.getY()/5.0, n.getZ()/5.0));
            pclPtr->width++;
        }

    }
    return;
}

vector<point3d> cast_init_rays(const octomap::OcTree *octree, const point3d &position,
                                 const point3d &direction) {
    vector<point3d> hits;
    octomap::OcTreeNode *n;

    for(auto p_y : InitialScan.pitch_yaws) {
        double pitch = p_y.first;
        double yaw = p_y.second;
        point3d direction_copy(direction.normalized());
        point3d end;
        direction_copy.rotate_IP(0.0, pitch, yaw);
        if(octree->castRay(position, direction_copy, end, true, InitialScan.max_range)) {
            hits.push_back(end);
        } else {
            direction_copy *= InitialScan.max_range;
            direction_copy += position;
            n = octree->search(direction_copy);
            if (!n)
                continue;
            if (n->getOccupancy() < free_prob )
                continue;
            hits.push_back(direction_copy);
        }
    }        
    return hits;
}


vector<point3d> cast_sensor_rays(const octomap::OcTree *octree, const point3d &position,
                                 const point3d &direction) {
    vector<point3d> hits;
    octomap::OcTreeNode *n;
    // cout << "d_x : " << direction.normalized().x() << " d_y : " << direction.normalized().y() << " d_z : " 
    //   << direction.normalized().z() << endl;
    for(auto p_y : Velodyne_puck.pitch_yaws) {
        double pitch = p_y.first;
        double yaw = p_y.second;
        point3d direction_copy(direction.normalized());
        point3d end;
        direction_copy.rotate_IP(0.0, pitch, yaw);
        if(octree->castRay(position, direction_copy, end, true, Velodyne_puck.max_range)) {
            hits.push_back(end);
        } else {
            direction_copy *= Velodyne_puck.max_range;
            direction_copy += position;
            n = octree->search(direction_copy);
            if (!n)
                continue;
            if (n->getOccupancy() < free_prob )
                continue;
            hits.push_back(direction_copy);
        }
    }
    return hits;
}

vector<pair<point3d, point3d>> generate_candidates(point3d position) {
    double R = 1;   // Robot step, in meters.
    double n = 10;
    int counter = 0;

    vector<pair<point3d, point3d>> candidates;
    double z = position.z();                // fixed 
    double pitch = 0;     // fixed
    double x, y;

    // for(z = position.z() - 1; z <= position.z() + 1; z += 1)
        for(double yaw = 0; yaw < 2 * PI; yaw += PI / n) {
            x = position.x() + R * cos(yaw);
            y = position.y() + R * sin(yaw);
            candidates.push_back(make_pair<point3d, point3d>(point3d(x, y, z), point3d(0, pitch, yaw)));
            counter++;
        }

    cout << "Generate Candidates : " << counter << endl;
    return candidates;
}

double calc_MI(const octomap::OcTree *octree, const point3d &position, const vector<point3d> &hits, const double before) {
    auto octree_copy = new octomap::OcTree(*octree);

    for(const auto h : hits) {
        octree_copy->insertRay(position, h, Velodyne_puck.max_range);
    }
    octree_copy->updateInnerOccupancy();
    double after = get_free_volume(octree_copy);
    delete octree_copy;
    return after - before;
}

void RPY2Quaternion(double roll, double pitch, double yaw, double *x, double *y, double *z, double *w) {
    double cr2, cp2, cy2, sr2, sp2, sy2;
    cr2 = cos(roll*0.5);
    cp2 = cos(pitch*0.5);
    cy2 = cos(yaw*0.5);

    sr2 = -sin(roll*0.5);
    sp2 = -sin(pitch*0.5);
    sy2 = sin(yaw*0.5);

    *w = cr2*cp2*cy2 + sr2*sp2*sy2;
    *x = sr2*cp2*cy2 - cr2*sp2*sy2;
    *y = cr2*sp2*cy2 + sr2*cp2*sy2;
    *z = cr2*cp2*sy2 - sr2*sp2*cy2;
}


void velodyne_callbacks( const sensor_msgs::PointCloud2ConstPtr& cloud2_msg ) {
    // kinect_flag = 1;
    pcl::PCLPointCloud2 cloud2;
    pcl_conversions::toPCL(*cloud2_msg, cloud2);
    PointCloud* cloud (new PointCloud);
    PointCloud* cloud_local (new PointCloud);
    pcl::fromPCLPointCloud2(cloud2,*cloud_local);

    pcl_ros::transformPointCloud("/map", *cloud_local, *cloud, *tf_listener);

    map_pcl->clear();
    // map_pcl->header.frame_id = "/camera_depth_frame";
        for (int j = 1; j< cloud->width; j++)
        {
            if(isnan(cloud->at(j).x)) continue;
            cur_tree->insertRay(point3d( position.x(),position.y(),z_sensor), 
                point3d(cloud->at(j).x, cloud->at(j).y, cloud->at(j).z), Velodyne_puck.max_range);
            // add the point into map
            map_pcl->points.push_back(pcl::PointXYZ(cloud_local->at(j).x, cloud_local->at(j).y, cloud_local->at(j).z));
            map_pcl->width++;
        }
        map_pcl->height = 1;
    
    cout << "height : " << cloud->height << "  , width :" << cloud->width << " resulting map free volume" << get_free_volume(cur_tree) << endl;
    // delete octree_copy;
    map_pcl->header.stamp = ros::Time::now().toNSec() / 1e3;
    Map_pcl_pub.publish(map_pcl);
    cout << "Updating current map in pcl (points): " << map_pcl->width << endl;
    delete cloud;
    delete cloud_local;
}

int main(int argc, char **argv) {
    ros::init(argc, argv, "Info_Exploration_Octomap");
    ros::NodeHandle nh;
    // Initial sub topic
    ros::Subscriber octomap_sub;
    ros::Subscriber lidar_sub;
    // octomap_sub = nh.subscribe<octomap_msgs::Octomap>("/octomap_binary", 10, octomap_callback);
    lidar_sub = nh.subscribe<sensor_msgs::PointCloud2>("/velodyne_points", 1, velodyne_callbacks);
    
    Map_pcl_pub = nh.advertise<PointCloud>("Current_Map", 1);
    VScan_pcl_pub = nh.advertise<PointCloud>("virtual_Scans", 1);
    Free_pcl_pub = nh.advertise<PointCloud>("Free_points", 1);

    tf_listener = new tf::TransformListener();
    tf::StampedTransform transform;

    map_pcl->header.frame_id = "/velodyne";
    map_pcl->height = 1;
    map_pcl->width = 0;

    free_pcl->header.frame_id = "/map";
    free_pcl->height = 1;
    free_pcl->width = 0;

    vsn_pcl->header.frame_id = "/map";
    vsn_pcl->height = 1;
    vsn_pcl->width = 0;
   
    // Initialize parameters 
    ros::Rate r(1); // 10 hz
    int max_idx = 0;

    position = point3d(0, 0, 0.0);
    point3d dif_pos = point3d(0,0,0);
    point3d eu2dr(1, 0, 0);
    octomap::OcTreeNode *n;
    octomap::OcTree tree_new(0.1);
    cur_tree = &tree_new;


 
    // Get the current localization from tf
        for (int ini_i = 0; ini_i < 4; ini_i++)
    {
        try{
        tf_listener->lookupTransform("/map", "/base_link", ros::Time(0), transform);
        position = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
        }
        catch (tf::TransformException ex){
        ROS_ERROR("%s",ex.what());
        }
    }
    cout << "Initial  Position : " << position << " Heading : " << transform.getRotation().getAngle() << endl;

    double qx, qy, qz, qw;
    RPY2Quaternion(0, 0, 1, &qx, &qy, &qz, &qw);

    // Initial Scan
    ros::spinOnce();


    double before = get_free_volume(cur_tree);
    cout << "CurMap  Entropy : " << get_free_volume(cur_tree) << endl;

    while (ros::ok())
    {
        // Generate Candidates
        vector<pair<point3d, point3d>> candidates = generate_candidates(position);
        vector<double> MIs(candidates.size());
        max_idx = 0;

        // for every candidate...
        #pragma omp parallel for
        for(int i = 0; i < candidates.size(); ++i) 
        {
            auto c = candidates[i];
            n = cur_tree->search(c.first);

            if (!n)     continue;
            if(n->getOccupancy() > free_prob)       continue;

            // Evaluate Mutual Information
            eu2dr.rotate_IP(c.second.roll(), c.second.pitch(), c.second.yaw() );
            vector<point3d> hits = cast_sensor_rays(cur_tree, c.first, eu2dr);
            MIs[i] = calc_MI(cur_tree, c.first, hits, before);

            // Pick the Best Candidate
            if (MIs[i] > MIs[max_idx])
            {
                max_idx = i;
            }
        }

        // Send the Robot 
        cout << "Sending the Goal : " << candidates[max_idx].first << " , Yaw : " << candidates[max_idx].second.yaw() << endl;
        RPY2Quaternion(0, 0, candidates[max_idx].second.yaw(), &qx, &qy, &qz, &qw);

        // dif_pos = point3d(candidates[max_idx].first.x()-position.x(), 
        //     candidates[max_idx].first.y()-position.y(), 
        //     candidates[max_idx].first.z()-position.z()  );
        bool arrived = goToDest(candidates[max_idx].first, qx, qy, qz, qw);

        // Get the current localization from tf
        try{
        tf_listener->lookupTransform("/map", "/base_link", ros::Time(0), transform);
        position = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
        }
        catch (tf::TransformException ex){
        ROS_ERROR("%s",ex.what());
        }


        // cout << "Current Robot Location : " << position << endl;
        if(arrived)
          {
            cout << "We made one step forward" << endl;
            // Updating octomap with kinect call back.
            // RPY2Quaternion(0, 0, 1, &qx, &qy, &qz, &qw);
            // for (int ini_i = 0; ini_i < 6; ini_i++)
            // {
            // try{
            // tf_listener->lookupTransform("/map", "/base_link", ros::Time(0), transform);
            // position = point3d(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z());
            // }
            // catch (tf::TransformException ex){
            // ROS_ERROR("%s",ex.what());
            // }
            // bool arrived = goToDest(position, qx, qy, qz, qw);
            // cout << "make more observation.. " << ini_i << "." << endl;
            // ros::spinOnce();
            // }
                ros::spinOnce();
            // position = candidates[max_idx].first;
          }
        else
        {
            cout << "We didn't make it... starting over..." << endl;
        }
          
        cout << "CurMap  Entropy after movement : " << get_free_volume(cur_tree) << endl;
        r.sleep();
    }
    nh.shutdown();          
    return 0;
}
