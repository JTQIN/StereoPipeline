// __BEGIN_LICENSE__
//  Copyright (c) 2006-2013, United States Government as represented by the
//  Administrator of the National Aeronautics and Space Administration. All
//  rights reserved.
//
//  The NASA Vision Workbench is licensed under the Apache License,
//  Version 2.0 (the "License"); you may not use this file except in
//  compliance with the License. You may obtain a copy of the License at
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// __END_LICENSE__

#include <vw/Math/EulerAngles.h>
#include <vw/Camera/CameraModel.h>
#include <vw/Camera/CameraSolve.h>
#include <asp/Camera/OpticalBarModel.h>

#include <vw/Cartography/Datum.h>  // DEBUG

#include <iomanip>
#include <boost/filesystem/convenience.hpp>


namespace asp {
namespace camera {

using namespace vw;
using namespace vw::camera;


Vector2 OpticalBarModel::pixel_to_sensor_plane(Vector2 const& pixel) const {
  Vector2 result = (pixel - m_center_loc_pixels) * m_pixel_size;
  //result[1] *= -1; // Make upper pixels have larger values // TODO: CHECK!
  return result;
}

double OpticalBarModel::pixel_to_time_delta(Vector2 const& pix) const {

  // This is the amount of time required for one complete image scan.
  const double scan_time = m_scan_angle_radians / m_scan_rate_radians;

  // Since the camera sweeps a scan through columns, use that to
  //  determine the fraction of the way it is through the image.
  const int max_col = m_image_size[0]-1;
  double scan_fraction = 0;
  if (m_scan_left_to_right)
    scan_fraction = pix[0] / max_col; // TODO: Add 0.5 pixels to calculation?
  else // Right to left scan direction
    scan_fraction = (max_col - pix[0]) / max_col;
  double time_delta = scan_fraction * scan_time;
  return time_delta;
}

Vector3 OpticalBarModel::get_velocity(vw::Vector2 const& pixel) const {
  
 
  // TODO: For speed, store the pose*velocity vector.
  // Convert the velocity from sensor coords to GCC coords
  //Matrix3x3 pose = transpose(camera_pose(pixel).rotation_matrix());
  Matrix3x3 pose = camera_pose(pixel).rotation_matrix();

  // Recover the satellite attitude relative to the tilted camera position
  Matrix3x3 m = vw::math::rotation_x_axis(-m_forward_tilt_radians)*pose;

  return m*Vector3(0,m_speed,0);
}

Vector3 OpticalBarModel::camera_center(Vector2 const& pix) const {
  // We model with a constant velocity.
  double dt = pixel_to_time_delta(pix);

  return m_initial_position + dt*get_velocity(pix);
}


Quat OpticalBarModel::camera_pose(Vector2 const& pix) const {
  // Camera pose is treated as constant for the duration of a scan.
  return axis_angle_to_quaternion(m_initial_orientation);
}

Vector3 OpticalBarModel::pixel_to_vector_uncorrected(Vector2 const& pixel) const {
 
  Vector2 sensor_plane_pos = pixel_to_sensor_plane(pixel);
  Vector3 cam_center       = camera_center(pixel);
  Quat    cam_pose         = camera_pose  (pixel);

  // This is the horizontal angle away from the center point (from straight out of the camera)
  double alpha = sensor_plane_pos[0] / m_focal_length;

  // Distance from the camera center to the ground.
  double H = norm_2(cam_center) - (m_mean_surface_elevation + m_mean_earth_radius);


  // Distortion caused by compensation for the satellite's forward motion during the image.
  // - The film was actually translated underneath the lens to compensate for the motion.
  double image_motion_compensation = 0.0;
  //if (m_use_motion_compensation)
  //  image_motion_compensation = ((-m_focal_length * m_speed) / (H*m_scan_rate_radians))
  //                               * sin(alpha);
  image_motion_compensation = ((m_focal_length * m_speed) / (H*m_scan_rate_radians))
                                 * sin(alpha) * m_use_motion_compensation;
  if (!m_scan_left_to_right) // Sync alpha with motion compensation.
    image_motion_compensation *= -1.0;

  //Matrix3x3 M_inv = transpose(cam_pose.rotation_matrix());

  // TODO: Consolidate the rotations when things are correct.
  // This vector is ESD format, consistent with the linescan model.
  Vector3 r(m_focal_length * sin(alpha),
            sensor_plane_pos[1] + image_motion_compensation,
            m_focal_length * cos(alpha));
  r = normalize(r);

  // TODO: Why is this needed for Gambit?
//  Matrix3x3 m = vw::math::rotation_z_axis(-M_PI/2.0);
//  r = m*r;

  // r is the ray vector in the local camera system
  
  /*
  std::cout << "pixel = " << pixel << std::endl;
  std::cout << "sensor_plane_pos = " << sensor_plane_pos << std::endl;
  std::cout << "alpha = " << alpha << std::endl;
  std::cout << "H = " << H << std::endl;
  std::cout << "r = " << r << std::endl;
  std::cout << "image_motion_compensation = " << image_motion_compensation << std::endl;
  */

  // Convert the ray vector into GCC coordinates.
  //Vector3 result = M_inv * r; // == scale(gcc_point - cam_center)
  Vector3 result = cam_pose.rotate(r);

  return result;
}

Vector3 OpticalBarModel::pixel_to_vector(Vector2 const& pixel) const {
  try {
    Vector3 output_vector = pixel_to_vector_uncorrected(pixel);

    Vector3 cam_ctr = camera_center(pixel);
    if (!m_correct_atmospheric_refraction) 
      output_vector = apply_atmospheric_refraction_correction(cam_ctr, m_mean_earth_radius,
                                                              m_mean_surface_elevation, output_vector);

    if (!m_correct_velocity_aberration) 
      return output_vector;
    else
      return apply_velocity_aberration_correction(cam_ctr, get_velocity(pixel),
                                                  m_mean_earth_radius, output_vector);

  } catch(const vw::Exception &e) {
    // Repackage any of our exceptions thrown below this point as a 
    //  pixel to ray exception that other code will be able to handle.
    vw_throw(vw::camera::PixelToRayErr() << e.what());
  }
}

Vector2 OpticalBarModel::point_to_pixel(Vector3 const& point) const {

  // Use the generic solver to find the pixel 
  // - This method will be slower but works for more complicated geometries
  CameraGenericLMA model( this, point );
  int status;
  Vector2 start = m_image_size / 2.0; // Use the center as the initial guess

  // Solver constants
  const double ABS_TOL = 1e-16;
  const double REL_TOL = 1e-16;
  const int    MAX_ITERATIONS = 1e+5;

  Vector3 objective(0, 0, 0);
  Vector2 solution = math::levenberg_marquardt(model, start, objective, status,
                                               ABS_TOL, REL_TOL, MAX_ITERATIONS);
  VW_ASSERT( status > 0,
             camera::PointToPixelErr() << "Unable to project point into Linescan model" );

  return solution;
}

void OpticalBarModel::apply_transform(vw::Matrix3x3 const & rotation,
                                      vw::Vector3   const & translation,
                                      double                scale) {

  // Extract current parameters
  vw::Vector3 position = this->camera_center();
  vw::Quat    pose     = this->camera_pose();
  
  vw::Quat rotation_quaternion(rotation);
  
  // New position and rotation
  position = scale*rotation*position + translation;
  pose     = rotation_quaternion*pose;
  this->set_camera_center(position);
  this->set_camera_pose  (pose.axis_angle());
}


void OpticalBarModel::read(std::string const& filename) {

  // Open the input file
  std::ifstream cam_file;
  cam_file.open(filename.c_str());
  if (cam_file.fail())
    vw_throw( IOErr() << "OpticalBarModel::read_file: Could not open file: " << filename );

  // Check for version number on the first line
  std::string line;
  std::getline(cam_file, line);
  if (line.find("VERSION") == std::string::npos) {
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Version missing!\n" );
  }

  int file_version = 1;
  sscanf(line.c_str(),"VERSION_%d", &file_version); // Parse the version of the input file
  if (file_version < 4)
    vw_throw( ArgumentErr() << "OpticalBarModel::read_file(): Versions prior to 4 are not supported!\n" );

  // Read the camera type
  std::getline(cam_file, line);
  if (line.find("OPTICAL_BAR") == std::string::npos)
        vw_throw( ArgumentErr() << "OpticalBarModel::read_file: Expected OPTICAL_BAR type, but got type "
                                << line );

  // Start parsing all the parameters from the lines.
  std::getline(cam_file, line);
  if (!cam_file.good() || sscanf(line.c_str(),"image_size = %d %d",
      &m_image_size[0], &m_image_size[1]) != 2) {
    cam_file.close();
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Could not read the image size\n" );
  }

  std::getline(cam_file, line);
  if (!cam_file.good() || sscanf(line.c_str(),"image_center = %lf %lf",
      &m_center_loc_pixels[0], &m_center_loc_pixels[1]) != 2) {
    cam_file.close();
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Could not read the image center\n" );
  }

  std::getline(cam_file, line);
  if (!cam_file.good() || sscanf(line.c_str(),"pitch = %lf", &m_pixel_size) != 1) {
    cam_file.close();
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Could not read the pixel pitch\n" );
  }

  std::getline(cam_file, line);
  if (!cam_file.good() || sscanf(line.c_str(),"f = %lf", &m_focal_length) != 1) {
    cam_file.close();
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Could not read the focal_length\n" );
  }

  std::getline(cam_file, line);
  if (!cam_file.good() || sscanf(line.c_str(),"scan_angle = %lf", &m_scan_angle_radians) != 1) {
    cam_file.close();
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Could not read the scan angle\n" );
  }
  
  std::getline(cam_file, line);
  if (!cam_file.good() || sscanf(line.c_str(),"scan_rate = %lf", &m_scan_rate_radians) != 1) {
    cam_file.close();
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Could not read the scan rate\n" );
  }

  std::getline(cam_file, line);
  if (!cam_file.good() || sscanf(line.c_str(),"forward_tilt = %lf", &m_forward_tilt_radians) != 1) {
    cam_file.close();
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Could not read the forward tilt angle\n" );
  }
  
  std::getline(cam_file, line);
  if (!cam_file.good() || sscanf(line.c_str(),"iC = %lf %lf %lf", 
        &m_initial_position(0), &m_initial_position(1), &m_initial_position(2)) != 3) {
    cam_file.close();
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Could not read the initial position\n" );
  }

  // Read and convert the rotation matrix.
  Matrix3x3 rot_mat;
  std::getline(cam_file, line);
  if ( !cam_file.good() ||
       sscanf(line.c_str(), "iR = %lf %lf %lf %lf %lf %lf %lf %lf %lf",
              &rot_mat(0,0), &rot_mat(0,1), &rot_mat(0,2),
              &rot_mat(1,0), &rot_mat(1,1), &rot_mat(1,2),
              &rot_mat(2,0), &rot_mat(2,1), &rot_mat(2,2)) != 9 ) {
    cam_file.close();
    vw_throw( IOErr() << "PinholeModel::read_file(): Could not read the rotation matrix\n" );
  }
  Quat q(rot_mat);
  m_initial_orientation = q.axis_angle();

  /*
  std::getline(cam_file, line);
  if (!cam_file.good() || sscanf(line.c_str(),"iR = %lf %lf %lf", 
        &m_initial_orientation(0), &m_initial_orientation(1), &m_initial_orientation(2)) != 3) {
    cam_file.close();
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Could not read the initial orientation\n" );
  }
  //// !!! DEBUG !!! -> Ignore the orientation, point it locally NED down.
  //vw::cartography::Datum d("WGS84");
  //Vector3 llh = d.cartesian_to_geodetic(m_initial_position);
  //Matrix3x3 m = d.lonlat_to_ned_matrix(Vector2(llh[0],llh[1]));
  //Quat q(m);
  //m_initial_orientation = q.axis_angle();
  */

  std::getline(cam_file, line);
  if (!cam_file.good() || sscanf(line.c_str(),"speed = %lf", &m_speed) != 1) {
    cam_file.close();
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Could not read the speed\n" );
  }

  std::getline(cam_file, line);
  if (!cam_file.good() || sscanf(line.c_str(),"mean_earth_radius = %lf", &m_mean_earth_radius) != 1) {
    cam_file.close();
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Could not read the mean earth radius\n" );
  }
  
  std::getline(cam_file, line);
  if (!cam_file.good() || sscanf(line.c_str(),"mean_surface_elevation = %lf", &m_mean_surface_elevation) != 1) {
    cam_file.close();
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Could not read the mean surface elevation\n" );
  }

  std::getline(cam_file, line);
  int temp_i;
  if (!cam_file.good() || sscanf(line.c_str(),"use_motion_compensation = %d", &temp_i) != 1) {
    cam_file.close();
    vw_throw( IOErr() << "OpticalBarModel::read_file(): Could not read use motion compensation\n" );
  }
  m_use_motion_compensation = temp_i;//(temp_i > 0);

  std::getline(cam_file, line);
  m_scan_left_to_right = line.find("scan_dir = left") == std::string::npos;

  cam_file.close();
}


void OpticalBarModel::write(std::string const& filename) const {

  // TODO: Make compatible with .tsai files!

  // Open the output file for writing
  std::ofstream cam_file(filename.c_str());
  if( !cam_file.is_open() ) 
    vw_throw( IOErr() << "OpticalBarModel::write: Could not open file: " << filename );

  // Write the pinhole camera model parts
  //   # digits to survive double->text->double conversion
  const size_t ACCURATE_DIGITS = 17; // = std::numeric_limits<double>::max_digits10
  cam_file << std::setprecision(ACCURATE_DIGITS); 
  cam_file << "VERSION_4\n";
  cam_file << "OPTICAL_BAR\n";
  cam_file << "image_size = "   << m_image_size[0] << " " 
                                << m_image_size[1]<< "\n";
  cam_file << "image_center = " << m_center_loc_pixels[0] << " "
                                << m_center_loc_pixels[1] << "\n";
  cam_file << "pitch = "        << m_pixel_size             << "\n";
  cam_file << "f = "            << m_focal_length           << "\n";
  cam_file << "scan_angle = "   << m_scan_angle_radians     << "\n";
  cam_file << "scan_rate = "    << m_scan_rate_radians      << "\n";
  cam_file << "forward_tilt = " << m_forward_tilt_radians   << "\n";
  cam_file << "iC = " << m_initial_position[0] << " "
                      << m_initial_position[1] << " "
                      << m_initial_position[2] << "\n";
  // Store in the same format as the pinhole camera model.
  Matrix3x3 rot_mat = camera_pose(Vector2(0,0)).rotation_matrix();
  //cam_file << "iR = " << rot_mat[0] << " " rot_mat[1] << " " rot_mat[2] << " "
  //                    << m_initial_orientation[1] << " "
  //                    << m_initial_orientation[2] << "\n";
  cam_file << "iR = " << rot_mat(0,0) << " " << rot_mat(0,1) << " " << rot_mat(0,2) << " "
                      << rot_mat(1,0) << " " << rot_mat(1,1) << " " << rot_mat(1,2) << " "
                      << rot_mat(2,0) << " " << rot_mat(2,1) << " " << rot_mat(2,2) << "\n";
  cam_file << "speed = " << m_speed << "\n";
  cam_file << "mean_earth_radius = "      << m_mean_earth_radius      << "\n";
  cam_file << "mean_surface_elevation = " << m_mean_surface_elevation << "\n";
  cam_file << "use_motion_compensation = " << m_use_motion_compensation << "\n";
  //if (m_use_motion_compensation)
  //  cam_file << "use_motion_compensation = 1\n";
  //else
  //  cam_file << "use_motion_compensation = 0\n";
  if (m_scan_left_to_right)
    cam_file << "scan_dir = right\n";
  else
    cam_file << "scan_dir = left\n";
  cam_file.close();
}



std::ostream& operator<<( std::ostream& os, OpticalBarModel const& camera_model) {
  os << "\n------------------------ Optical Bar Model -----------------------\n\n";
  os << " Image size :            " << camera_model.m_image_size             << "\n";
  os << " Center loc (pixels):    " << camera_model.m_center_loc_pixels      << "\n";
  os << " Pixel size (m) :        " << camera_model.m_pixel_size             << "\n";
  os << " Focal length (m) :      " << camera_model.m_focal_length           << "\n";
  os << " Scan angle (rad):       " << camera_model.m_scan_angle_radians     << "\n";
  os << " Scan rate (rad/s):      " << camera_model.m_scan_rate_radians      << "\n";
  os << " Scan left to right?:    " << camera_model.m_scan_left_to_right     << "\n";
  os << " Forward tilt (rad):     " << camera_model.m_forward_tilt_radians   << "\n";
  os << " Initial position:       " << camera_model.m_initial_position       << "\n";
  os << " Initial pose:           " << camera_model.m_initial_orientation    << "\n";
  os << " Speed:                  " << camera_model.m_speed                  << "\n";
  os << " Mean earth radius:      " << camera_model.m_mean_earth_radius      << "\n";
  os << " Mean surface elevation: " << camera_model.m_mean_surface_elevation << "\n";
  os << " Use motion comp:        " << camera_model.m_use_motion_compensation<< "\n";
  os << " Left to right scan:     " << camera_model.m_scan_left_to_right     << "\n";

  os << "\n------------------------------------------------------------------------\n\n";
  return os;
}


}} // namespace asp::camera

