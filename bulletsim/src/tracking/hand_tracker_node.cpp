#include <pcl/ros/conversions.h>
#include <pcl/common/transforms.h>
#include <pcl/point_cloud.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/io/pcd_io.h>
#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/Image.h>
#include <tf/tf.h>
#include <tf/transform_listener.h>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <cv_bridge/cv_bridge.h>
#include <cv.h>
#include <bulletsim_msgs/TrackedObject.h>

#include "clouds/utils_pcl.h"
#include "utils_tracking.h"
#include "utils/logging.h"
#include "utils/utils_vector.h"
#include "visibility.h"
#include "physics_tracker.h"
#include "feature_extractor.h"
#include "initialization.h"
#include "simulation/simplescene.h"
#include "config_tracking.h"
#include "simulation/hand.h"
#include "tracking/tracked_compound.h"
#include "clouds/cloud_ops.h"
#include "utils/conversions.h"
#include "plotting_tracking.h"
#include <osg/Depth>
#include "tracking/utils_tracking.h"
#include "clouds/utils_ros.h"
using sensor_msgs::PointCloud2;
using sensor_msgs::Image;
using namespace std;

namespace cv {
	typedef Vec<uchar, 3> Vec3b;
}

vector<cv::Mat> depthImages;
vector<cv::Mat> rgbImages;
vector<CoordinateTransformer*> transformer_images;
vector<bool> pending_images;

ColorCloudPtr filteredCloud(new ColorCloud()); // filtered cloud in ground frame
CoordinateTransformer* transformer;
bool pending = false; // new message received, waiting to be processed

tf::TransformListener* listener;

void cloudCallback (const sensor_msgs::PointCloud2ConstPtr& cloudMsg) {
  if (transformer == NULL) {
    transformer = new CoordinateTransformer(waitForAndGetTransform(*listener, "/ground",cloudMsg->header.frame_id));
  }

  pcl::fromROSMsg(*cloudMsg, *filteredCloud);
  pcl::transformPointCloud(*filteredCloud, *filteredCloud, transformer->worldFromCamEigen);

  pending = true;
  //cout << "pending " << pending << endl;
}

void imagesCallback (const sensor_msgs::ImageConstPtr& depthImageMsg,
										 const sensor_msgs::ImageConstPtr& rgbImageMsg,
										 int i) {
	if (transformer_images[i] == NULL) {
		transformer_images[i] = new CoordinateTransformer(waitForAndGetTransform(*listener, "/ground", TrackingConfig::cameraTopics[0]+"_rgb_optical_frame"));
  }

  depthImages[i] = cv_bridge::toCvCopy(depthImageMsg)->image;
  rgbImages[i] = cv_bridge::toCvCopy(rgbImageMsg)->image;

  pending_images[i] = true;
  //cout << "pending_images[" << i << "] " << pending_images[i] << endl;
}

int main(int argc, char* argv[]) {
  Eigen::setNbThreads(2);

  GeneralConfig::scale = 100;
  BulletConfig::maxSubSteps = 0;
  BulletConfig::gravity = btVector3(0,0,0.1);

  Parser parser;
  parser.addGroup(TrackingConfig());
  parser.addGroup(GeneralConfig());
  parser.addGroup(BulletConfig());
  parser.read(argc, argv);

  int nCameras = TrackingConfig::cameraTopics.size();
  transformer_images.assign(nCameras, NULL);
  pending_images.assign(nCameras, false);

  ros::init(argc, argv,"tracker_node");
  ros::NodeHandle nh;

  listener = new tf::TransformListener();

  ros::Subscriber cloudSub = nh.subscribe(TrackingConfig::filteredCloudTopic, 1, &cloudCallback);

  depthImages.resize(nCameras);
  rgbImages.resize(nCameras);
	vector<message_filters::Subscriber<Image>*> depthImagesSub(nCameras, NULL);
	vector<message_filters::Subscriber<Image>*> rgbImagesSub(nCameras, NULL);
	typedef message_filters::sync_policies::ApproximateTime<Image, Image> ApproxSyncPolicy;
	vector<message_filters::Synchronizer<ApproxSyncPolicy>*> imagesSync(nCameras, NULL);
	for (int i=0; i<nCameras; i++) {
		depthImagesSub[i] = new message_filters::Subscriber<Image>(nh, TrackingConfig::cameraTopics[i] + "/depth_registered/image_rect", 1);
		rgbImagesSub[i] = new message_filters::Subscriber<Image>(nh, TrackingConfig::cameraTopics[i] + "/rgb/image_rect_color", 1);
		imagesSync[i] = new message_filters::Synchronizer<ApproxSyncPolicy>(ApproxSyncPolicy(30), *depthImagesSub[i], *rgbImagesSub[i]);
		imagesSync[i]->registerCallback(boost::bind(&imagesCallback,_1,_2,i));
	}

  ros::Publisher objPub = nh.advertise<bulletsim_msgs::TrackedObject>(trackedObjectTopic,10);

  // wait for first message, then initialize
  while (!(pending && cwiseAnd(pending_images))) {
    ros::spinOnce();
    sleep(.001);
    if (!ros::ok()) throw runtime_error("caught signal while waiting for first message");
  }
  // set up scene
  Scene scene;
  scene.startViewer();

//  BulletObject::Ptr camera(new BulletObject(1, new btConeShapeZ(.2*METERS, .2*METERS),
//							  util::scaleTransform(transformer->worldFromCamUnscaled, METERS),true));
//  scene.env->add(camera);

  btVector3 initHandPos = transformer->toWorldFromCam(btVector3(0,0,.7));
  btTransform initHandTrans = btTransform(btQuaternion(0,0,1,0), initHandPos);
  Hand::Ptr hand = makeHand(19, initHandTrans);
  scene.env->add(hand);
  TrackedObject::Ptr trackedObj(new TrackedCompound(hand, scene.env->bullet->dynamicsWorld));
  trackedObj->init();

  btVector3 filterBoxHalfExtents = btVector3(.15,.15,.15)*METERS;
  BoxObject::Ptr filterBox(new BoxObject(0, filterBoxHalfExtents, initHandTrans));
  scene.env->add(filterBox);
  scene.env->bullet->dynamicsWorld->removeRigidBody(filterBox->rigidBody.get());
  filterBox->setColor(0,1,0,.2);
  osg::Depth* depth = new osg::Depth;
  depth->setWriteMask( false );
  filterBox->node->getOrCreateStateSet()->setAttributeAndModes( depth, osg::StateAttribute::ON );




  // actual tracking algorithm
	MultiVisibility::Ptr visInterface(new MultiVisibility());
	for (int i=0; i<nCameras; i++) {
//		visInterface->addVisibility(BulletRaycastVisibility::Ptr(new BulletRaycastVisibility(scene.env->bullet->dynamicsWorld, transformer)));
		visInterface->addVisibility(EverythingIsVisible::Ptr(new EverythingIsVisible()));
	}

	TrackedObjectFeatureExtractor::Ptr objectFeatures(new TrackedObjectFeatureExtractor(trackedObj));
	CloudFeatureExtractor::Ptr cloudFeatures(new CloudFeatureExtractor());
	PhysicsTracker::Ptr alg(new PhysicsTracker(objectFeatures, cloudFeatures, visInterface));
	PhysicsTrackerVisualizer::Ptr trakingVisualizer(new PhysicsTrackerVisualizer(&scene, alg));


	PointCloudPlot::Ptr allCloud(new PointCloudPlot(4));
	scene.env->add(allCloud);

	bool applyEvidence = true;
  scene.addVoidKeyCallback('a',boost::bind(toggle, &applyEvidence));
  scene.addVoidKeyCallback('=',boost::bind(&EnvironmentObject::adjustTransparency, trackedObj->getSim(), 0.1f));
  scene.addVoidKeyCallback('-',boost::bind(&EnvironmentObject::adjustTransparency, trackedObj->getSim(), -0.1f));
  scene.addVoidKeyCallback('q',boost::bind(exit, 0));

  while (ros::ok()) {
  	//Update the inputs of the featureExtractors and visibilities (if they have any inputs)
     	allCloud->setPoints1(filteredCloud);
	 filteredCloud = boxFilter(filteredCloud, toEigenVector(initHandPos - filterBoxHalfExtents),
											  toEigenVector(initHandPos + filterBoxHalfExtents));
	 LOG_DEBUG("cloud size: " << filteredCloud->size());
 	cloudFeatures->updateInputs(filteredCloud);
  	//TODO update arbitrary number of depth images)
    visInterface->visibilities[0]->updateInput(depthImages[0]);
    pending = false;
    while (ros::ok() && !pending) {
    	//Do iteration
      alg->updateFeatures();
      alg->expectationStep();
      alg->maximizationStep(applyEvidence);

      trakingVisualizer->update();
      scene.env->step(.03,2,.015);
      scene.viewer.frame();
      ros::spinOnce();
    }
    //TODO
    //objPub.publish(toTrackedObjectMessage(trackedObj));
  }
}
