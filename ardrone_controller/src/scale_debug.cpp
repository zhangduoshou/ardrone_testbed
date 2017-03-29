#include <queue>

#include <ros/ros.h>
#include <sensor_msgs/Imu.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <ardrone_autonomy/Navdata.h>

#include <std_msgs/Float32.h>

#include <tf/tf.h>
#include <tf/LinearMath/Transform.h>
#include <tf/transform_listener.h>
#include <tf/LinearMath//Vector3.h>
#include <tf/tfMessage.h>
#include <tf/transform_broadcaster.h>
#include <linear_system.h>

using namespace std;
typedef geometry_msgs::PoseWithCovarianceStamped poseMsgStamped;
typedef geometry_msgs::Pose poseMsg;


void printVector(const std::string& str, const tf::Vector3& vctr){
  std::cout << str << ": (" << vctr.x() << "," << vctr.y() << "," << vctr.z() << ")" << std::endl;
}

void printVector(const std::string& str, const geometry_msgs::Pose& vctr){
  std::cout << str << ": (" << vctr.position.x << "," << vctr.position.y << "," << vctr.position.z << ")" << std::endl;
}

void printVector(const std::string& str, const geometry_msgs::PoseWithCovarianceStamped& vctr){
  std::cout << str << ": (" << vctr.pose.pose.position.x << "," << vctr.pose.pose.position.y << "," << vctr.pose.pose.position.z << ")" << std::endl;
}

void printVector(const std::string& str, const tf::StampedTransform& vctr){
  std::cout << str << ": (" << vctr.getOrigin().getX() << "," << vctr.getOrigin().getY() << "," << vctr.getOrigin().getZ() << ")" << std::endl;
}

// directly copied from tum
class ScaleStruct
{
public:
  tf::Vector3 ptam;
  tf::Vector3 imu;
  double ptamNorm;
  double imuNorm;
  double alphaSingleEstimate;
  double pp, ii, pi;
  
  static inline double computeEstimator(double spp, double sii, double spi, double stdDevPTAM = 0.2, double stdDevIMU = 0.1){
    double sII = stdDevPTAM * stdDevPTAM * sii;
    double sPP = stdDevIMU * stdDevIMU * spp;
    double sPI = stdDevIMU * stdDevPTAM * spi;
    
    int sgn = copysign(1, spi);
    
    double tmp = (sII-sPP)*(sII-sPP) + 4*sPI*sPI;
    if(tmp <= 0) 
    {
      tmp = 1e-5; // numeric issues
    }
    return 0.5*(-(sII-sPP)+sgn*sqrt(tmp)) / (stdDevIMU * stdDevIMU * spi);
    
  }
  
  inline ScaleStruct(tf::Vector3 ptamDist, tf::Vector3 imuDist)
  {
    ptam = ptamDist;
    imu = imuDist;
    pp = ptam[0]*ptam[0] + ptam[1]*ptam[1] + ptam[2]*ptam[2];
    ii = imu[0]*imu[0] + imu[1]*imu[1] + imu[2]*imu[2];
    pi = imu[0]*ptam[0] + imu[1]*ptam[1] + imu[2]*ptam[2];
    
    ptamNorm = sqrt(pp);
    imuNorm = sqrt(ii);
    
    alphaSingleEstimate = computeEstimator(pp,ii,pi);
  }
  
  inline bool operator < (const ScaleStruct& comp) const
  {
    return alphaSingleEstimate < comp.alphaSingleEstimate;
  }
};
//

class ScaleEstimator {
  std::shared_ptr<LTI::SisoSystem> nav_x, nav_y, nav_z;
  std::shared_ptr<LTI::SisoSystem> orb_x,orb_y,orb_z;
  bool first_msg;
  bool fixed_scale = false;
  float scale = 1; // has units m^-1
  float scale_ubound = 1;
  float scale_lbound = 1;
  
  // nav data correction
  bool quat_init = false;
  bool correction_made = false;
  tf::StampedTransform T_init, T_curr, T_prev;
  tf::Transform T_correction;
  tf::TransformBroadcaster br;
  
  // tuning parameters
  const float orb_noise=0.2, nav_noise=0.01; // noise params must be tuned (init vals from tum)
  const float dot_prod_tol = 0.05;
  const float ratio = 0.2;
  const int scale_samples = 10;
  tf::Vector3 orb_signal;
  tf::Vector3 nav_signal;
//   tf::Vector3 init_displacement = tf::Vector3 ( 0,0,0 );
  geometry_msgs::PoseWithCovarianceStamped filt_pose, latest_pose;
  tf::TransformListener tf_listener;
  deque<geometry_msgs::PoseWithCovarianceStamped> orb_data_queue;
  vector<ScaleStruct> scale_vector;
  
public:
  
  ScaleEstimator():orb_signal(0,0,0),nav_signal(0,0,0),first_msg(true){
    T_init.setIdentity();
    T_prev.setIdentity();
    T_curr.setIdentity();
    T_correction.setIdentity();
  }
  
//   tf::Vector3 orb_displacement = tf::Vector3 ( 0,0,0 );
//   tf::Vector3 nav_data_displacement = tf::Vector3 ( 0,0,0 );
  
  
  void reset() {//TODO used?
    orb_data_queue.clear();
  }
  
  void hard_reset(){//TODO used?
    reset();
    scale = 1;
    fixed_scale = false;
    scale_vector.clear();
    geometry_msgs::PoseWithCovarianceStamped empty_pose;
    filt_pose = empty_pose;
  }   
  
  void estimateScale() {//TODO ugly code
    sort(scale_vector.begin(), scale_vector.end());
    
    double median; 
    if (scale_vector.size() < 5){median = 1;}
    else{median = scale_vector[(scale_vector.size()+1)/2].alphaSingleEstimate;}
    
    // find sums and median.
    // do separately for xy and z and xyz-all and xyz-filtered
    double sumII = 0;
    double sumPP = 0;
    double sumPI = 0;

    int count = 0;
    
    for(unsigned int i=0;i<scale_vector.size();i++){
      if(scale_vector.size() < 5 || (scale_vector[i].alphaSingleEstimate > median * ratio && scale_vector[i].alphaSingleEstimate < median / ratio)){
        sumII += scale_vector[i].ii;
        sumPP += scale_vector[i].pp;
        sumPI += scale_vector[i].pi;
	
        count++;
      }
    }
    fixed_scale = fixed_scale or count > scale_samples;
    
    if (not fixed_scale)
    {
      scale_ubound = ScaleStruct::computeEstimator(sumPP,sumII,sumPI, .0001,10000);
      scale_lbound = ScaleStruct::computeEstimator(sumPP,sumII,sumPI, 100,.01);
      scale = ScaleStruct::computeEstimator(sumPP,sumII,sumPI, orb_noise,nav_noise);
    } 
    else{cout << "scale fixed" << endl;}
  }
  
  void printAll(){      
    float cur_orb_x = filt_pose.pose.pose.position.x / scale; 
    float cur_orb_y = filt_pose.pose.pose.position.y / scale; 
    float cur_orb_z = filt_pose.pose.pose.position.z / scale;
    
    tf::Vector3 u_bound_pos = tf::Vector3(cur_orb_x * scale_ubound, cur_orb_y * scale_ubound, cur_orb_z * scale_ubound);
    tf::Vector3 l_bound_pos = tf::Vector3(cur_orb_x * scale_lbound, cur_orb_y * scale_lbound, cur_orb_z * scale_lbound);
    
    cout << endl;
    
    cout << "lower s: " << scale_lbound << "\t s: " << scale << "\t upper s: " << scale_ubound << endl;
    
    printVector("position lbound", l_bound_pos);
    printVector("position ubound", u_bound_pos);
    printVector("position", filt_pose);
  }
  
  //This gets called at every iteration of the ros loop
  void processQueue() {
    //Only process orb data if batch size is greater than 100 
    if(orb_data_queue.size()<100){return;}
    
    //Initialize nav datatype and orb message datatype to add sensor data to fution routine
    tf::StampedTransform nav_tf;
    poseMsgStamped orb_msg;
    double t = orb_data_queue.back().header.stamp.toSec();
    
    while(orb_data_queue.size()>0){
      //get oldest message
      orb_msg = orb_data_queue.back();
      orb_data_queue.pop_back();
      t = orb_msg.header.stamp.toSec();
      
      if(first_msg){//initialize filters 
        first_msg = false;
        double pole2Hz = 2.0*3.14*0.5;
        double pole5Hz = 2.0*3.14*0.5;
        
        //numerator and denominator of transfer function
        LTI::array num(2),den(3); 
        num[0]=0; num[1]=pole2Hz*pole5Hz;
        den[0]=pole2Hz*pole5Hz; den[1]=pole2Hz+pole5Hz; den[2]=1;
        
        orb_x = std::shared_ptr<LTI::SisoSystem>(new LTI::SisoSystem(num,den, t, 0.005));
        orb_y = std::shared_ptr<LTI::SisoSystem>(new LTI::SisoSystem(num,den, t, 0.005));
        orb_z = std::shared_ptr<LTI::SisoSystem>(new LTI::SisoSystem(num,den, t, 0.005));
        
        nav_x = std::shared_ptr<LTI::SisoSystem>(new LTI::SisoSystem(num,den, t, 0.005));
        nav_y = std::shared_ptr<LTI::SisoSystem>(new LTI::SisoSystem(num,den, t, 0.005));
        nav_z = std::shared_ptr<LTI::SisoSystem>(new LTI::SisoSystem(num,den, t, 0.005));
      }
      
      orb_x->timeStep(t,orb_msg.pose.pose.position.x);
      orb_y->timeStep(t,orb_msg.pose.pose.position.y);
      orb_z->timeStep(t,orb_msg.pose.pose.position.z);
      
      if( tf_listener.waitForTransform( "/odom", "/ardrone_base_link_corrected",orb_msg.header.stamp,
        ros::Duration(1.0),ros::Duration(0.1))){
      try {
        tf_listener.lookupTransform ( "odom", "/ardrone_base_link_corrected", orb_msg.header.stamp, nav_tf );
        nav_x->timeStep(t,nav_tf.getOrigin().getX());
        nav_y->timeStep(t,nav_tf.getOrigin().getY());
        nav_z->timeStep(t,nav_tf.getOrigin().getZ());
      } catch( tf2::ExtrapolationException err ){cout << err.what() << endl; return;}
    }
    }
    
    orb_signal.setX(orb_x->getOutput(t));
    orb_signal.setY(orb_y->getOutput(t));
    orb_signal.setZ(orb_z->getOutput(t));
    
    nav_signal.setX(nav_x->getOutput(t));
    nav_signal.setY(nav_y->getOutput(t));
    nav_signal.setZ(nav_z->getOutput(t));
    
      // add new point estimate
      ScaleStruct s = ScaleStruct(orb_signal, nav_signal);
      cout << "tol = " << s.pi/s.ptamNorm << endl;
      if (s.pi > dot_prod_tol * s.ptamNorm){
        cout << "taking scale" << endl;
        scale_vector.push_back(s);
      }
      estimateScale();
  }
  
  //Copies oldest message in orb msg buffer into internal pose variable
  poseMsgStamped getScaledOrbPose() {
    
    filt_pose.header.stamp = latest_pose.header.stamp;
    filt_pose.header.frame_id = "odom";
    
    filt_pose.pose.pose.orientation = latest_pose.pose.pose.orientation;
    filt_pose.pose.pose.position.x = latest_pose.pose.pose.position.x /scale;
    filt_pose.pose.pose.position.y = latest_pose.pose.pose.position.y /scale;
    filt_pose.pose.pose.position.z = latest_pose.pose.pose.position.z /scale;
    
    return filt_pose;
  }
  
  void orbCallback ( geometry_msgs::PoseWithCovarianceStamped msg ){
    orb_data_queue.push_front ( msg );
    latest_pose = msg;
  }
  void nav_callback ( ardrone_autonomy::Navdata navdata ) {
    float deg_to_rad = 3.14159 / 180;
    
    // get current frame
    try 
    {
      tf_listener.lookupTransform("odom", "/ardrone_base_link", navdata.header.stamp, T_curr);
      T_curr.setOrigin(tf::Vector3(0,0,0));
    }
    catch (tf2::ExtrapolationException e) {cout << e.what() << endl;}
    
    
    // initialize previous frame
    if ( not quat_init) { T_init = T_prev = T_curr; quat_init = true;} 
    
    if (not correction_made) 
    {
      // calculate correction transformation
      tf::Transform delta_T = T_curr.inverse() * T_prev;
      
      // if angle jump is too large then assign the correction transform
      float angle_deg = delta_T.getRotation().getAngle() / deg_to_rad;
      if (abs(angle_deg) > 5) { T_correction = T_curr.inverse() * T_init; correction_made = true;}
      
      T_prev = T_curr;
    }
    
    // publish corrected frame
    br.sendTransform(tf::StampedTransform(T_correction, navdata.header.stamp, "/ardrone_base_link", "/ardrone_base_link_corrected"));
  }
  
//   void publish_scale ( ros::Publisher &publisher ) {publisher.publish ( scale );}
//   void publish_scaled_pose ( ros::Publisher &publisher ) {publisher.publish ( filt_pose );}
//   void set_initial_nav_position ( tf::Vector3 pos ) {init_displacement = pos;}
};

int main ( int argc, char **argv ) {
  ros::init ( argc, argv, "scale_estimator" );
  ros::NodeHandle nh;
  
  // estimates scale
  ScaleEstimator scale_est;
  
  // subscribe to orb pose and accelerometer data from imu
  ros::Subscriber orb_sub = nh.subscribe ( "/orb/pose_unscaled", 10, &ScaleEstimator::orbCallback, &scale_est );
  
  // get rotation due to magnetic field
  ros::Subscriber nav_sub = nh.subscribe ( "/ardrone/navdata", 10, &ScaleEstimator::nav_callback, &scale_est );
  
  // publish scale and filtered orb pose
  ros::Publisher scaled_orb_pub = nh.advertise<poseMsgStamped> ( "/orb/pose_scaled",2 );
  
  int rate = 20;
  ros::Rate loop_rate ( rate );
  
  scale_est.reset();
  
  while ( ros::ok() ) {
    ros::spinOnce();
    
    scale_est.processQueue();//doesn't do anything if queue size less than 50 
    scale_est.printAll();
    poseMsgStamped scale_pose_for_publish = scale_est.getScaledOrbPose();
    scaled_orb_pub.publish(scale_pose_for_publish);
    loop_rate.sleep();
  }
  
  return 0;
}