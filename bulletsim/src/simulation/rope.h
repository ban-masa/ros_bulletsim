#pragma once
#include "environment.h"
#include "basicobjects.h"
#include "btBulletDynamicsCommon.h"
#include <vector>

class CapsuleRope : public CompoundObject<BulletObject> {
private:
  float angStiffness;
  float angDamping;
  float linDamping;
  float angLimit;
public:
  typedef boost::shared_ptr<CapsuleRope> Ptr;
  std::vector<boost::shared_ptr<btCollisionShape> > shapes;
  std::vector<BulletConstraint::Ptr> joints;
  btScalar radius;
  int nLinks;

  CapsuleRope(const std::vector<btVector3>& ctrlPoints, float radius_, float angStiffness_=.1, float angDamping_=1, float linDamping_=.75, float angLimit_=.4, float linStopErp_=.2);
  void init();
  void destroy();
  std::vector<btVector3> getNodes();
  std::vector<btVector3> getControlPoints();
  vector<btMatrix3x3> getRotations();
  vector<float> getHalfHeights();
	void setTexture(cv::Mat image, cv::Mat mask, const btTransform& camFromWorld);
};
