#include <ros/ros.h>
#include <iostream>
#include <numeric>
#include <stdlib.h>
#include <fstream>
#include <sstream>
#include <string>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include "lc.h"

using namespace std;
namespace fs=boost::filesystem;

/**
 * Stores incoming point clouds in a map transforming
 * each cloud to a global fixed frame using tf.
 */
class Mono
{
  public:

    /** \brief Mono class constructor
     */
    Mono(ros::NodeHandle nh, ros::NodeHandle nhp) : nh_(nh), nh_private_(nhp)
    {
      // Read the parameters
      readParams();
    }

    /** \brief Processes the image dataset
     */
    void processData()
    {
      // Sort directory of images
      typedef vector<fs::path> vec;
      vec v;
      copy(
            fs::directory_iterator(img_dir_), 
            fs::directory_iterator(),
            back_inserter(v)
          );

      sort(v.begin(), v.end());
      vec::const_iterator it(v.begin());

      // Read the ground truth file
      int total_lc = 0;
      vector< vector<int> > ground_truth;
      ifstream in(gt_file_.c_str());
      if (!in) ROS_ERROR("[HashMatching: ] Ground truth file does not exist.");
      for (int x=0; x<(int)v.size(); x++) 
      {
        vector<int> row;
        for (int y=0; y<(int)v.size(); y++)
        {
          int num;
          in >> num;
          row.push_back(num);
        }
        ground_truth.push_back(row);
        int sum_of_elems = accumulate(row.begin(),row.end(),0);
        if (sum_of_elems > 0)
          total_lc++;
      }
      in.close();

      // Init Haloc
      haloc::LoopClosure::Params lc_params;
      lc_params.work_dir = output_path_;
      lc_params.desc_type = desc_type_;
      lc_params.num_proj = num_proj_;
      lc_params.desc_thresh = desc_thresh_;
      lc_params.epipolar_thresh = epipolar_thresh_;
      lc_params.min_neighbour = min_neighbour_;
      lc_params.n_candidates = n_candidates_;
      lc_params.min_matches = min_matches_;
      lc_params.min_inliers = min_inliers_;
      lc_params.validate = validate_;
      lc_.setParams(lc_params);
      lc_.init();

      // Count the overall loop time
      ros::WallTime overall_time_start = ros::WallTime::now();
      
      // Iterate over all images
      int img_i = 0;
      bool first = true;
      int found_lc = 0;
      int true_positives = 0;
      int false_positives = 0;
      while (it!=v.end())
      {
        // Check if the directory entry is an directory.
        if (!fs::is_directory(*it)) 
        {
          // Get image
          string filename = it->filename().string();
          Mat img = imread(img_dir_+"/"+filename, CV_LOAD_IMAGE_COLOR);

          // Set the new image
          lc_.setNode(img);

          // Get the loop closure (if any)
          int img_lc;
          bool valid = lc_.getLoopClosure(img_lc);

          // Check ground truth
          int tp = 0;
          int fa = 0;
          if (valid)
          {
            found_lc++;
            int gt_valid = 0;
            for (int i=0; i<2*gt_tolerance_+1; i++)
            {
              int img_j = img_lc - gt_tolerance_ + i;
              if (img_j<0) img_j = 0;
              gt_valid += ground_truth[img_i][img_j];
            }
            
            if(gt_valid >= 1)
            {
              true_positives++;
              tp = 1;
            }
            else
            {
              false_positives++;
              fa = 1;
            }
          }

          // Log
          ROS_INFO_STREAM( img_i << " cl with " << img_lc << ": " << valid << " (" << tp << "|" << fa << ")");

          img_i++;
        }

        // Next directory entry
        it++;
      }

      // Show the results

      // Stop time
      ros::WallDuration overall_time = ros::WallTime::now() - overall_time_start;

      // Compute precision and recall
      int false_negatives = total_lc - found_lc;
      double precision = round( 100 * true_positives / (true_positives + false_positives) );
      double recall = round( 100 * true_positives / (true_positives + false_negatives) );

      // Print the results
      ROS_INFO_STREAM("TOTAL #LC: " << total_lc);
      ROS_INFO_STREAM("FOUND #LC: " << found_lc);
      ROS_INFO_STREAM("#TP: " << true_positives);
      ROS_INFO_STREAM("#FP: " << false_positives);
      ROS_INFO_STREAM("PRECISION: " << precision << "%");
      ROS_INFO_STREAM("RECALL: " << recall << "%");
      ROS_INFO_STREAM("TOTAL EXECUTION TIME: " << overall_time.toSec() << " sec.");
    }

  protected:

    // Node handlers
    ros::NodeHandle nh_;
    ros::NodeHandle nh_private_;
    
  private:

    // Properties
    string img_dir_, desc_type_, output_path_, gt_file_;
    double desc_thresh_, epipolar_thresh_;
    bool validate_;
    int num_proj_, min_neighbour_, n_candidates_, min_matches_, min_inliers_, gt_tolerance_;
    haloc::LoopClosure lc_;

    /** \brief Read the parameters from the ros parameter server
     */
    void readParams()
    {
      nh_private_.param("output_path", output_path_, std::string(""));
      nh_private_.param("img_dir", img_dir_, std::string(""));
      nh_private_.param("gt_file", gt_file_, std::string(""));
      nh_private_.param("desc_type", desc_type_, std::string("SIFT"));
      nh_private_.getParam("desc_thresh", desc_thresh_);
      nh_private_.getParam("num_proj", num_proj_);
      nh_private_.getParam("min_neighbour", min_neighbour_);
      nh_private_.getParam("n_candidates", n_candidates_);
      nh_private_.getParam("min_matches", min_matches_);
      nh_private_.getParam("min_inliers", min_inliers_);
      nh_private_.getParam("epipolar_thresh", epipolar_thresh_);
      nh_private_.param("validate", validate_, false);
      nh_private_.getParam("gt_tolerance", gt_tolerance_);

      // Log
      cout << "  output_path      = " << output_path_ << endl;
      cout << "  img_dir          = " << img_dir_ << endl;
      cout << "  desc_type        = " << desc_type_ << endl;
      cout << "  desc_thresh      = " << desc_thresh_ << endl;
      cout << "  num_proj         = " << num_proj_ << endl;
      cout << "  min_neighbour    = " << min_neighbour_ << endl;
      cout << "  n_candidates     = " << n_candidates_ << endl;
      cout << "  min_matches      = " << min_matches_ << endl;
      cout << "  min_inliers      = " << min_inliers_ << endl;
      cout << "  epipolar_thresh  = " << epipolar_thresh_ << endl;
      cout << "  validate         = " << validate_ << endl;
      cout << "  gt_tolerance     = " << gt_tolerance_ << endl;

      // Files path sanity check
      if (output_path_[output_path_.length()-1] != '/')
        output_path_ += "/";

      // Sanity checks
      if (!fs::exists(img_dir_) || !fs::is_directory(img_dir_)) 
      {
        ROS_ERROR_STREAM("[HashMatching:] The image directory does not exists: " << 
                         img_dir_);
      }
    }
};

// Main entry point
int main(int argc, char **argv)
{
  ros::init(argc,argv,"example_mono");
  ros::NodeHandle nh;
  ros::NodeHandle nh_private("~");

  // Init node
  Mono mono(nh,nh_private);

  // Process the data
  mono.processData();

  // Subscription is handled at start and stop service callbacks.
  //ros::spin();
  
  return 0;
}
