/*
 * tracked_sponge.cpp
 *
 *  Created on: Sep 13, 2012
 *      Author: alex
 */

#include "tracked_object.h"
#include <boost/foreach.hpp>
#include <utility>
#include "config_tracking.h"
#include "simulation/config_bullet.h"
#include "utils/conversions.h"
#include <BulletSoftBody/btSoftBody.h>
#include <BulletSoftBody/btSoftBodyHelpers.h>
#include <boost/format.hpp>
#include "simulation/bullet_io.h"
#include <pcl/common/transforms.h>
#include "clouds/utils_pcl.h"
#include "feature_extractor.h"
#include <algorithm>
#include "utils/utils_vector.h"
#include "utils/cvmat.h"
#include "simulation/softBodyHelpers.h"
#include "tracking/utils_tracking.h"
#include "clouds/plane_finding.h"
#include "utils_tracking.h"

//DEBUG
#include "simulation/util.h"
#include <pcl/io/pcd_io.h>

//Do you need the includes above?

#include "clouds/cloud_ops.h"
#include "simulation/tetgen_helpers.h"
#include <pcl/ModelCoefficients.h>
#include <pcl/point_types.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/filters/project_inliers.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/surface/concave_hull.h>

using namespace std;
using namespace Eigen;

TrackedSponge::TrackedSponge(BulletSoftObject::Ptr sim) : TrackedObject(sim, "sponge") {
	for (int iVert=0; iVert<getSim()->softBody->m_nodes.size(); iVert++) {
		if (getSim()->node_boundaries[iVert])
			m_node2vert.push_back(iVert);
	}
  m_nNodes = m_node2vert.size();

	const btSoftBody::tNodeArray& verts = getSim()->softBody->m_nodes;
  m_masses.resize(m_nNodes);
  for (int i=0; i < m_nNodes; ++i) {
    m_masses(i) = 1/verts[m_node2vert[i]].m_im;
  }
}

vector<btVector3> TrackedSponge::getPoints() {
	vector<btVector3> out(m_nNodes);
	btAlignedObjectArray<btSoftBody::Node>& verts = getSim()->softBody->m_nodes;
	for (int iNode=0; iNode < m_nNodes; ++iNode)
		out[iNode] = verts[m_node2vert[iNode]].m_x;
	return out;
}

void TrackedSponge::applyEvidence(const Eigen::MatrixXf& corr, const Eigen::MatrixXf& obsPts) {
  vector<btVector3> estPos(m_nNodes);
  vector<btVector3> estVel(m_nNodes);

  btAlignedObjectArray<btSoftBody::Node>& verts = getSim()->softBody->m_nodes;
  for (int iNode=0; iNode < m_nNodes; ++iNode)  {
    estPos[iNode] = verts[m_node2vert[iNode]].m_x;
    estVel[iNode] = verts[m_node2vert[iNode]].m_v;
  }

  vector<btVector3> nodeImpulses = calcImpulsesDamped(estPos, estVel, toBulletVectors(obsPts), corr, toVec(m_masses), TrackingConfig::kp_cloth, TrackingConfig::kd_cloth);

  for (int iNode=0; iNode < m_nNodes; iNode++) {
    getSim()->softBody->addForce(nodeImpulses[iNode], m_node2vert[iNode]);
  }
}

// Returns the approximate polygon of the concave hull of the cloud
// The points are being projected to the xy plane
vector<btVector3> polyCorners(ColorCloudPtr cloud) {

  vector<float> coeffs = getPlaneCoeffsRansac(cloud);
  ColorCloudPtr cloud_projected = projectPointsOntoPlane(cloud, coeffs);

  std::vector<pcl::Vertices> polygons;
  ColorCloudPtr cloud_hull = findConcaveHull(cloud_projected, .05*METERS, polygons);

	vector<btVector3> pts = toBulletVectors(cloud_hull);

	vector<float> pts_z;
	for (int i=0; i<pts.size(); i++)
		pts_z.push_back(pts[i].z());
	float median_z = median(pts_z);

	vector<cv::Point2f> pts_2d;
	for (int i=0; i<pts.size(); i++)
		pts_2d.push_back(cv::Point2f(pts[i].x(), pts[i].y()));

	vector<cv::Point2f> pts_approx_2d;
	cv::approxPolyDP(cv::Mat(pts_2d), pts_approx_2d, 0.01*METERS, true);

	vector<btVector3> pts_approx;
	for (int i=0; i<pts_approx_2d.size(); i++)
		pts_approx.push_back(btVector3(pts_approx_2d[i].x, pts_approx_2d[i].y, median_z));

	return pts_approx;
}
