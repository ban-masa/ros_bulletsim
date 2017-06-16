#include "utils/my_exceptions.h"
#include <ros/ros.h>
#include <pcl/point_types.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <cv_bridge/cv_bridge.h>
#include <pcl/ros/conversions.h>
#include "clouds/utils_pcl.h"
#include "clouds/cloud_ops.h"
#include "get_table2.h"
#include <cmath>
#include "utils/config.h"
#include <tf/transform_broadcaster.h>
#include <geometry_msgs/PolygonStamped.h>
#include "utils/conversions.h"
#include <boost/thread.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>

using namespace std;
using namespace Eigen;

struct LocalConfig : Config {
  static std::string inputTopic;
  static std::string nodeNS;
  static float zClipLow;
  static float zClipHigh;
  static bool updateParams;
  static float downsample;
  static bool removeOutliers;
  static float clusterTolerance;
  static float clusterMinSize;
  static bool boxFilter;
  static bool trackbars;

  LocalConfig() : Config() {
    params.push_back(new Parameter<string>("inputTopic", &inputTopic, "input topic"));
    params.push_back(new Parameter<string> ("nodeNS", &nodeNS, "node namespace"));
    params.push_back(new Parameter<float>("zClipLow", &zClipLow, "clip points that are less than this much above table"));
    params.push_back(new Parameter<float>("zClipHigh", &zClipHigh, "clip points that are more than this much above table"));
    params.push_back(new Parameter<bool>("updateParams", &updateParams, "start a thread to periodically update the parameters thru the parameter server"));
    params.push_back(new Parameter<float>("downsample", &downsample, "downsample voxel grid size. 0 means no"));
    params.push_back(new Parameter<bool>("removeOutliers", &removeOutliers, "remove outliers"));
    params.push_back(new Parameter<float>("clusterTolerance", &clusterTolerance, "points within this distance are in the same cluster"));
    params.push_back(new Parameter<float>("clusterMinSize", &clusterMinSize, "the clusters found must have at least this number of points. 0 means no filtering"));
    params.push_back(new Parameter<bool>("boxFilter", &boxFilter, "box filter"));
    params.push_back(new Parameter<bool>("trackbars", &trackbars, "show hue trackbars"));
  }
};

string LocalConfig::inputTopic = "/camera/rgb/points";
string LocalConfig::nodeNS = "/preprocessor/kinect1";
float LocalConfig::zClipLow = .0025;
float LocalConfig::zClipHigh = 1000;
bool LocalConfig::updateParams = true;
float LocalConfig::downsample = .02;
bool LocalConfig::removeOutliers = false;
float LocalConfig::clusterTolerance = 0.03;
float LocalConfig::clusterMinSize = 40;
bool LocalConfig::boxFilter = false;
bool LocalConfig::trackbars = false;

static int MIN_HUE, MAX_HUE, MIN_SAT, MAX_SAT, MIN_VAL, MAX_VAL;

template <typename T>
void getOrSetParam(const ros::NodeHandle& nh, std::string paramName, T& ref, T defaultVal) {
	if (!nh.getParam(paramName, ref)) {
		nh.setParam(paramName, defaultVal);
		ref = defaultVal;
		ROS_INFO_STREAM("setting " << paramName << " to default value " << defaultVal);
	}
}
void setParams(const ros::NodeHandle& nh) {
	if (LocalConfig::trackbars) {
		nh.setParam("min_hue", MIN_HUE);
		nh.setParam("max_hue", MAX_HUE);
		nh.setParam("min_sat", MIN_SAT);
		nh.setParam("max_sat", MAX_SAT);
		nh.setParam("min_val", MIN_VAL);
		nh.setParam("max_val", MAX_VAL);
	} else {
		getOrSetParam(nh, "min_hue", MIN_HUE, 160);
		getOrSetParam(nh, "max_hue", MAX_HUE, 10);
		getOrSetParam(nh, "min_sat", MIN_SAT, 150);
		getOrSetParam(nh, "max_sat", MAX_SAT, 255);
		getOrSetParam(nh, "min_val", MIN_VAL, 100);
		getOrSetParam(nh, "max_val", MAX_VAL, 255);
	}
}

void setParamLoop(ros::NodeHandle& nh) {
	// red
	/*MIN_HUE = 160; MAX_HUE = 10;
	MIN_SAT = 150; MAX_SAT = 255;
	MIN_VAL = 100; MAX_VAL = 255;*/

	// yellow sponge
	MIN_HUE = 163; MAX_HUE = 44;
	MIN_SAT = 107; MAX_SAT = 184;
	MIN_VAL = 133; MAX_VAL = 255;

	while (nh.ok()) {
		if (LocalConfig::trackbars) {
			// Needs this to update the opencv windows
			char key = cv::waitKey(20);
			if (key == 'q')
				exit(0);

			cv::namedWindow("hue trackbars");
			cv::createTrackbar("hue min", "hue trackbars", &MIN_HUE, 255);
			cv::createTrackbar("hue max", "hue trackbars", &MAX_HUE, 255);
			cv::createTrackbar("sat min", "hue trackbars", &MIN_SAT, 255);
			cv::createTrackbar("sat max", "hue trackbars", &MAX_SAT, 255);
			cv::createTrackbar("val min", "hue trackbars", &MIN_VAL, 255);
			cv::createTrackbar("val max", "hue trackbars", &MAX_VAL, 255);
		}

		setParams(nh);
		sleep(0.2);
	}
}

class PreprocessorNode {
public:
  ros::NodeHandle& m_nh;
  ros::Publisher m_cloudPub, m_imagePub, m_depthPub;
  ros::Publisher m_polyPub;
  tf::TransformBroadcaster br;
  ros::Subscriber m_sub;

  bool m_inited;

  Matrix3f m_axes;
  Vector3f m_mins, m_maxes;

  btTransform m_transform;
  geometry_msgs::Polygon m_poly;


  void callback(const sensor_msgs::PointCloud2& msg_in) {
    ColorCloudPtr cloud_in(new ColorCloud());
    pcl::fromROSMsg(msg_in, *cloud_in);



    if (!m_inited) {
      initTable(cloud_in);
    }

    ColorCloudPtr cloud_out = cloud_in;
    if (LocalConfig::boxFilter) cloud_out = orientedBoxFilter(cloud_in, m_axes, m_mins, m_maxes);
    cloud_out = hueFilter(cloud_out, MIN_HUE, MAX_HUE, MIN_SAT, MAX_SAT, MIN_VAL, MAX_VAL);
    if (LocalConfig::downsample > 0) cloud_out = downsampleCloud(cloud_out, LocalConfig::downsample);
    if (LocalConfig::removeOutliers) cloud_out = removeOutliers(cloud_out, 1, 10);
    if (LocalConfig::clusterMinSize > 0) cloud_out = clusterFilter(cloud_out, LocalConfig::clusterTolerance, LocalConfig::clusterMinSize);



    sensor_msgs::PointCloud2 msg_out;
    pcl::toROSMsg(*cloud_out, msg_out);
    msg_out.header = msg_in.header;
    m_cloudPub.publish(msg_out);

    cv_bridge::CvImage image_msg;
    image_msg.header = msg_in.header;
    image_msg.encoding = sensor_msgs::image_encodings::TYPE_8UC4;
    cv::Mat image = toCVMatImage(cloud_in);
    cv::cvtColor(image, image, CV_BGR2BGRA);
    image_msg.image = image;
    m_imagePub.publish(image_msg.toImageMsg());

    cv_bridge::CvImage depth_msg;
    depth_msg.header   = msg_in.header;
    depth_msg.encoding = sensor_msgs::image_encodings::TYPE_32FC1;
    depth_msg.image    = toCVMatDepthImage(cloud_in);
    m_depthPub.publish(depth_msg.toImageMsg());

    br.sendTransform(tf::StampedTransform(toTfTransform(m_transform), ros::Time::now(), msg_in.header.frame_id, "/ground"));

    geometry_msgs::PolygonStamped polyStamped;
    polyStamped.polygon = m_poly;
    polyStamped.header.frame_id = msg_in.header.frame_id;
    polyStamped.header.stamp = ros::Time::now();
    m_polyPub.publish(polyStamped);


  }

  void initTable(ColorCloudPtr cloud) {
    MatrixXf corners = getTableCornersRansac(cloud);

    Vector3f xax = corners.row(1) - corners.row(0);
    xax.normalize();
    Vector3f yax = corners.row(3) - corners.row(0);
    yax.normalize();
    Vector3f zax = xax.cross(yax);

    float zsgn = (zax(2) > 0) ? 1 : -1;
    xax *= - zsgn;
    zax *= - zsgn; // so z axis points up

    m_axes.col(0) = xax;
    m_axes.col(1) = yax;
    m_axes.col(2) = zax;

    MatrixXf rotCorners = corners * m_axes;

    m_mins = rotCorners.colwise().minCoeff();
    m_maxes = rotCorners.colwise().maxCoeff();
    m_mins(2) = rotCorners(0,2) + LocalConfig::zClipLow;
    m_maxes(2) = rotCorners(0,2) + LocalConfig::zClipHigh;



    m_transform.setBasis(btMatrix3x3(
		  xax(0),yax(0),zax(0),
		  xax(1),yax(1),zax(1),
		  xax(2),yax(2),zax(2)));
    m_transform.setOrigin(btVector3(corners(0,0), corners(0,1), corners(0,2)));

    m_poly.points = toROSPoints32(toBulletVectors(corners));



    m_inited = true;

  }

  PreprocessorNode(ros::NodeHandle& nh) :
    m_inited(false),
    m_nh(nh),
    m_cloudPub(nh.advertise<sensor_msgs::PointCloud2> (LocalConfig::nodeNS + "/points", 5)),
    m_imagePub(nh.advertise<sensor_msgs::Image> (LocalConfig::nodeNS + "/image", 5)),
    m_depthPub(nh.advertise<sensor_msgs::Image> (LocalConfig::nodeNS + "/depth", 5)),
    m_polyPub(nh.advertise<geometry_msgs::PolygonStamped>("polygon",5)),
    m_sub(nh.subscribe(LocalConfig::inputTopic, 1, &PreprocessorNode::callback, this))
    {
    }


  };

int main(int argc, char* argv[]) {
  Parser parser;
  parser.addGroup(LocalConfig());
  parser.read(argc, argv);


  ros::init(argc, argv,"preprocessor");
  ros::NodeHandle nh(LocalConfig::nodeNS);

  setParams(nh);
  if (LocalConfig::updateParams) boost::thread setParamThread(setParamLoop, nh);


  PreprocessorNode tp(nh);
  ros::spin();
}
