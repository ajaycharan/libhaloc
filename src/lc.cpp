#include "libhaloc/lc.h"
#include "libhaloc/utils.h"
#include <iostream>
#include <numeric>
#include <stdlib.h>
#include <fstream>
#include <sstream>
#include <string>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

namespace fs=boost::filesystem;

/** \brief Parameter constructor. Sets the parameter struct to default values.
  */
haloc::LoopClosure::Params::Params() :
  work_dir(""),
  desc_type("SIFT"),
  num_proj(DEFAULT_NUM_PROJ),
  desc_thresh(DEFAULT_DESC_THRESH),
  epipolar_thresh(DEFAULT_EPIPOLAR_THRESH),
  min_neighbour(DEFAULT_MIN_NEIGHBOUR),
  n_candidates(DEFAULT_N_CANDIDATES),
  min_matches(DEFAULT_MIN_MATCHES),
  min_inliers(DEFAULT_MIN_INLIERS),
  max_reproj_err(DEFAULT_MAX_REPROJ_ERR),
  validate(DEFAULT_VALIDATE)
{}

/** \brief LoopClosure class constructor.
  */
haloc::LoopClosure::LoopClosure(){}

/** \brief Sets the class parameters.
  * \param stuct of parameters.
  */
void haloc::LoopClosure::setParams(const Params& params) 
{
  params_ = params;
}

/** \brief Sets the camera model.
  * \param Stereo camera model.
  */
void haloc::LoopClosure::setCameraModel(image_geometry::StereoCameraModel stereo_camera_model, Mat camera_matrix)
{
  img_.setCameraModel(stereo_camera_model);
  camera_matrix_ = camera_matrix;
}

/** \brief Initializes the loop closure class.
  */
void haloc::LoopClosure::init()
{
  // Working directory sanity check
  if (params_.work_dir[params_.work_dir.length()-1] != '/')
    params_.work_dir += "/ex_" + boost::lexical_cast<string>(time(0));
  else
    params_.work_dir += "ex_" + boost::lexical_cast<string>(time(0));

  // Create the directory to store the keypoints and descriptors
  if (fs::is_directory(params_.work_dir))
    fs::remove_all(params_.work_dir);
  fs::path dir(params_.work_dir);
  if (!fs::create_directory(dir))
    ROS_ERROR("[Haloc:] ERROR -> Impossible to create the execution directory.");

  // Initialize image properties
  haloc::Image::Params img_params;
  img_params.desc_type = params_.desc_type;
  img_params.desc_thresh = params_.desc_thresh;
  img_params.epipolar_thresh = params_.epipolar_thresh;
  img_.setParams(img_params);

  // Initialize hash
  haloc::Hash::Params hash_params;
  hash_params.num_proj = params_.num_proj;
  hash_.setParams(hash_params);

  // Init main variables
  hash_table_.clear();
  img_idx_ = 0;
}

/** \brief Finalizes the loop closure class.
  */
void haloc::LoopClosure::finalize()
{
  // Remove the temporal directory
  if (fs::is_directory(params_.work_dir))
    fs::remove_all(params_.work_dir);
}

/** \brief Compute kp, desc and hash for one image (mono verion).
  * \param cvMat containing the image.
  * \param human readable name for this image
  */
void haloc::LoopClosure::setNode(Mat img, string name)
{
  // Set the image
  img_.setMono(img);

  // Save kp and descriptors
  vector<Point3f> empty;
  FileStorage fs(params_.work_dir+"/"+boost::lexical_cast<string>(img_idx_)+".yml", FileStorage::WRITE);
  write(fs, "name", name);
  write(fs, "kp", img_.getKp());
  write(fs, "desc", img_.getDesc());
  write(fs, "threed", empty);
  fs.release();
  img_idx_++;
}

/** \brief Compute kp, desc and hash for two images (stereo verion).
  * \param cvMat containing the left image.
  * \param cvMat containing the right image.
  * \param human readable name for this image
  */
void haloc::LoopClosure::setNode(Mat img_l, Mat img_r, string name)
{
  // Set the image
  img_.setStereo(img_l, img_r);

  // Save kp and descriptors
  FileStorage fs(params_.work_dir+"/"+boost::lexical_cast<string>(img_idx_)+".yml", FileStorage::WRITE);
  write(fs, "name", name);
  write(fs, "kp", img_.getKp());
  write(fs, "desc", img_.getDesc());
  write(fs, "threed", img_.get3D());
  fs.release();
  img_idx_++;
}

/** \brief Try to find a loop closure between last node and all other nodes.
  * @return true if valid loop closure, false otherwise.
  * \param Return the index of the image that closes loop (-1 if no loop).
  */
bool haloc::LoopClosure::getLoopClosure(int& lc_img_idx, string& lc_name)
{
  tf::Transform trans;
  return getLoopClosure(lc_img_idx, lc_name, trans);
}
/** \brief Try to find a loop closure between last node and all other nodes.
  * @return true if valid loop closure, false otherwise.
  * \param Return the index of the image that closes loop (-1 if no loop).
  * \param Return the name of the image that closes loop (empty if no loop).
  * \param Return the transform between nodes if loop closure is valid.
  */
bool haloc::LoopClosure::getLoopClosure(int& lc_img_idx, string& lc_name, tf::Transform& trans)
{
  // Initialize hash
  if (!hash_.isInitialized())
  {
    hash_.init(img_.getDesc(), true);
    return false;
  }

  // Compute the hash for this image
  vector<float> hash_val = hash_.getHash(img_.getDesc());
  hash_table_.push_back(make_pair(img_idx_-1, hash_val));

  // Check if enough neighours
  if (hash_table_.size() <= params_.min_neighbour)
    return false;

  // Compute the hash matchings for this image with all other sequence
  vector< pair<int,float> > matchings;
  for (uint i=0; i<hash_table_.size()-params_.min_neighbour; i++)
  {
    // Hash matching
    vector<float> cur_hash = hash_table_[i].second;
    float m = hash_.match(hash_val, cur_hash);
    matchings.push_back(make_pair(hash_table_[i].first, m));
  }

  // Sort the hash matchings
  sort(matchings.begin(), matchings.end(), haloc::Utils::sortByMatching);

  // Check for loop closure
  trans.setIdentity();
  lc_img_idx = -1;
  lc_name = "";
  int best_m = 0;
  int matches = 0;
  int inliers = 0;
  bool valid = false;
  while (best_m<params_.n_candidates)
  {
    // Sanity check
    if(best_m >= matchings.size())
    {
      best_m = 0;
      break;
    }

    // Loop-closure?
    valid = compute(img_, 
                    params_.work_dir+"/"+boost::lexical_cast<string>(matchings[best_m].first)+".yml", 
                    lc_name, 
                    matches, 
                    inliers,
                    trans);

    // If the loop closure is valid and the seconds step validation is disabled, that's all.
    if (valid && !params_.validate) break;

    // Validate the loop closure?
    if (valid && params_.validate)
    {
      // Initialize validation
      bool validate_valid = false;
      int matches_val, inliers_val;
      tf::Transform trans_val;
      string tmp_name;

      // Loop closure for the previous image?
      validate_valid = compute(img_, 
                               params_.work_dir+"/"+boost::lexical_cast<string>(matchings[best_m].first - 1)+".yml", 
                               tmp_name, 
                               matches_val, 
                               inliers_val,
                               trans_val);

      if (!validate_valid)
      {
        // Previous validation does not works, try to validate with the next image
        validate_valid = compute(img_, 
                                 params_.work_dir+"/"+boost::lexical_cast<string>(matchings[best_m].first + 1)+".yml", 
                                 tmp_name, 
                                 matches_val, 
                                 inliers_val,
                                 trans_val);
      }

      // If validation, exit. If not, mark as non-valid
      if (validate_valid)
        break;
      else
        valid = false;
    }

    best_m++;
  }

  // Get the image of the loop closure
  if (valid && best_m < matchings.size())
    lc_img_idx = matchings[best_m].first;
  else
    lc_name = "";

  // Return true if any valid loop closure has been found.
  return valid;
}

/** \brief Compute the loop closure (if any).
  * @return true if valid loop closure, false otherwise.
  * \param Reference image object.
  * \param Current image filename with all the properties.
  * \param Return the number of matches found.
  * \param Return the number of inliers found.
  * \param Return the transform between nodes if loop closure is valid.
  */
bool haloc::LoopClosure::compute(Image ref_image,
                                 string cur_filename,
                                 string &lc_name, 
                                 int &matches,
                                 int &inliers,
                                 tf::Transform& trans)
{
  // Initialize outputs
  matches = 0;
  inliers = 0;

  // Sanity check
  if ( !fs::exists(cur_filename) ) return false;

  // Get the image keypoints and descriptors
  FileStorage fs; 
  fs.open(cur_filename, FileStorage::READ);
  if (!fs.isOpened()) 
    ROS_ERROR("[Haloc:] ERROR -> Failed to open the image keypoints and descriptors.");
  vector<Point2f> cur_kp;
  Mat cur_desc;
  vector<Point3f> points_3d;
  fs["name"] >> lc_name;
  fs["kp"] >> cur_kp;
  fs["desc"] >> cur_desc;
  fs["threed"] >> points_3d;
  fs.release();

  // Descriptors crosscheck matching
  vector<DMatch> desc_matches;
  Mat match_mask;
  haloc::Utils::crossCheckThresholdMatching(ref_image.getDesc(), 
                                            cur_desc, 
                                            params_.desc_thresh, 
                                            match_mask, desc_matches);
  matches = (int)desc_matches.size();

  // Check matches size
  if (matches < params_.min_matches)
    return false;

  // Get the matched keypoints
  vector<Point2f> ref_kp = ref_image.getKp();
  vector<Point2f> ref_points;
  vector<Point2f> cur_points;
  for(int i=0; i<matches; i++)
  {
    ref_points.push_back(ref_kp[desc_matches[i].queryIdx]);
    cur_points.push_back(cur_kp[desc_matches[i].trainIdx]);
  }

  // Proceed depending on mono or stereo
  if (points_3d.size() == 0) // Mono
  {
    // Check the epipolar geometry
    Mat status;
    Mat F = findFundamentalMat(ref_points, cur_points, FM_RANSAC, params_.epipolar_thresh, 0.999, status);

    // Is the fundamental matrix valid?
    Scalar f_sum_parts = cv::sum(F);
    float f_sum = (float)f_sum_parts[0] + (float)f_sum_parts[1] + (float)f_sum_parts[2];
    if (f_sum < 1e-3)
      return false;

    // Check inliers size
    inliers = (int)cv::sum(status)[0];
    if (inliers < params_.min_inliers)
      return false;
  }
  else // Stereo
  {
    Mat rvec, tvec;
    vector<int> solvepnp_inliers;
    solvePnPRansac(points_3d, cur_points, camera_matrix_, 
                   cv::Mat(), rvec, tvec, false, 
                   100, params_.max_reproj_err, 
                   40, solvepnp_inliers);

    inliers = (int)solvepnp_inliers.size();
    if (inliers < params_.min_inliers)
      return false;

    trans = haloc::Utils::buildTransformation(rvec, tvec);
  }

  // If we arrive here, there is a loop closure.
  return true;
}