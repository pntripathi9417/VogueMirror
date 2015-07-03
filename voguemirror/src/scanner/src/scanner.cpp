#include <scanner/precomp.hpp>
#include <scanner/cuda/internal.hpp>

static inline float deg2rad (float alpha) { return alpha * 0.017453293f; }

vm::scanner::ScannerParams vm::scanner::ScannerParams::default_params()
{
	const int iters[] = {10, 5, 4, 0};
  const int levels = sizeof(iters)/sizeof(iters[0]);

  ScannerParams p;

  p.cols = 640;  //pixels
  p.rows = 480;  //pixels
  p.intr = Intr(525.f, 525.f, p.cols/2 - 0.5f, p.rows/2 - 0.5f);

  p.volume_dims = Vec3i::all(512);  //number of voxels
  p.volume_size = Vec3f::all(1.5f);  //meters
  p.volume_pose = Affine3f().translate(Vec3f(-p.volume_size[0]/2, -p.volume_size[1]/2, 0.5f));

  p.bilateral_sigma_depth = 0.04f;  //meter
  p.bilateral_sigma_spatial = 4.5; //pixels
  p.bilateral_kernel_size = 7;     //pixels

  p.icp_truncate_depth_dist = 0.f;        //meters, disabled
  p.icp_dist_thres = 0.1f;                //meters
  p.icp_angle_thres = deg2rad(30.f); //radians
  p.icp_iter_num.assign(iters, iters + levels);

  p.tsdf_min_camera_movement = 0.f; //meters, disabled
  p.tsdf_trunc_dist = 0.04f; //meters;
  p.tsdf_max_weight = 64;   //frames

  p.raycast_step_factor = 0.75f;  //in voxel sizes
  p.gradient_delta_factor = 0.5f; //in voxel sizes

  //p.light_pose = p.volume_pose.translation()/4; //meters
  p.light_pose = Vec3f::all(0.f); //meters

  return p;
}

vm::scanner::Scanner::Scanner(const ScannerParams& params) : frame_counter_(0), params_(params)
{
  CV_Assert(params.volume_dims[0] % 32 == 0);

  volume_ = cv::Ptr<cuda::TsdfVolume>(new cuda::TsdfVolume(params_.volume_dims));

  volume_->setTruncDist(params_.tsdf_trunc_dist);
  volume_->setMaxWeight(params_.tsdf_max_weight);
  volume_->setSize(params_.volume_size);
  volume_->setPose(params_.volume_pose);
  volume_->setRaycastStepFactor(params_.raycast_step_factor);
  volume_->setGradientDeltaFactor(params_.gradient_delta_factor);

  icp_ = cv::Ptr<cuda::ProjectiveICP>(new cuda::ProjectiveICP());
  icp_->setDistThreshold(params_.icp_dist_thres);
  icp_->setAngleThreshold(params_.icp_angle_thres);
  icp_->setIterationsNum(params_.icp_iter_num);

  allocate_buffers();
  reset();
}

const vm::scanner::ScannerParams& vm::scanner::Scanner::params() const
{ return params_; }

vm::scanner::ScannerParams& vm::scanner::Scanner::params()
{ return params_; }

const vm::scanner::cuda::TsdfVolume& vm::scanner::Scanner::tsdf() const
{ return *volume_; }

vm::scanner::cuda::TsdfVolume& vm::scanner::Scanner::tsdf()
{ return *volume_; }

const vm::scanner::cuda::ProjectiveICP& vm::scanner::Scanner::icp() const
{ return *icp_; }

vm::scanner::cuda::ProjectiveICP& vm::scanner::Scanner::icp()
{ return *icp_; }

void vm::scanner::Scanner::allocate_buffers()
{
  const int LEVELS = cuda::ProjectiveICP::MAX_PYRAMID_LEVELS;

  int cols = params_.cols;
  int rows = params_.rows;

  dists_.create(rows, cols);

  curr_.depth_pyr.resize(LEVELS);
  curr_.normals_pyr.resize(LEVELS);
  prev_.depth_pyr.resize(LEVELS);
  prev_.normals_pyr.resize(LEVELS);

  curr_.points_pyr.resize(LEVELS);
  prev_.points_pyr.resize(LEVELS);

  for(int i = 0; i < LEVELS; ++i)
  {
    curr_.depth_pyr[i].create(rows, cols);
    curr_.normals_pyr[i].create(rows, cols);

    prev_.depth_pyr[i].create(rows, cols);
    prev_.normals_pyr[i].create(rows, cols);

    curr_.points_pyr[i].create(rows, cols);
    prev_.points_pyr[i].create(rows, cols);

    cols /= 2;
    rows /= 2;
  }

  depths_.create(params_.rows, params_.cols);
  normals_.create(params_.rows, params_.cols);
  points_.create(params_.rows, params_.cols);
}

void vm::scanner::Scanner::reset()
{
  if (frame_counter_)
    std::cout << "Reset" << std::endl;

  frame_counter_ = 0;
  poses_.clear();
  poses_.reserve(30000);
  poses_.push_back(Affine3f::Identity());
  volume_->clear();
}

vm::scanner::Affine3f vm::scanner::Scanner::getCameraPose (int time) const
{
  if (time > (int)poses_.size () || time < 0)
    time = (int)poses_.size () - 1;
  return poses_[time];
}

bool vm::scanner::Scanner::operator()(const vm::scanner::cuda::Depth& depth, const vm::scanner::cuda::Image& image)
{

  images_ = image;

  const ScannerParams& p = params_;
  const int LEVELS = icp_->getUsedLevelsNum();

  cuda::computeDists(depth, dists_, p.intr);
  cuda::depthBilateralFilter(depth, curr_.depth_pyr[0], p.bilateral_kernel_size, p.bilateral_sigma_spatial, p.bilateral_sigma_depth);

  if (p.icp_truncate_depth_dist > 0)
      vm::scanner::cuda::depthTruncation(curr_.depth_pyr[0], p.icp_truncate_depth_dist);

  for (int i = 1; i < LEVELS; ++i)
      cuda::depthBuildPyramid(curr_.depth_pyr[i-1], curr_.depth_pyr[i], p.bilateral_sigma_depth);

  for (int i = 0; i < LEVELS; ++i)
#if defined USE_DEPTH
    cuda::computeNormalsAndMaskDepth(p.intr, curr_.depth_pyr[i], curr_.normals_pyr[i]);
#else
    cuda::computePointNormals(p.intr(i), curr_.depth_pyr[i], curr_.points_pyr[i], curr_.normals_pyr[i]);
#endif

    cuda::waitAllDefaultStream();

    //can't perform more on first frame
    if (frame_counter_ == 0)
    {
      //volume_->integrate(dists_, poses_.back(), p.intr);
      volume_->integrate(dists_, images_, poses_.back(), p.intr);
#if defined USE_DEPTH
      curr_.depth_pyr.swap(prev_.depth_pyr);
#else
      curr_.points_pyr.swap(prev_.points_pyr);
#endif
      curr_.normals_pyr.swap(prev_.normals_pyr);
      return ++frame_counter_, false;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // ICP
    Affine3f affine; // cuur -> prev
    {
      //ScopeTime time("icp");
#if defined USE_DEPTH
      bool ok = icp_->estimateTransform(affine, p.intr, curr_.depth_pyr, curr_.normals_pyr, prev_.depth_pyr, prev_.normals_pyr);
#else
      bool ok = icp_->estimateTransform(affine, p.intr, curr_.points_pyr, curr_.normals_pyr, prev_.points_pyr, prev_.normals_pyr);
#endif
      if (!ok)
        return reset(), false;
    }

    poses_.push_back(poses_.back() * affine); // curr -> global

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Volume integration

    // We do not integrate volume if camera does not move.
    float rnorm = (float)cv::norm(affine.rvec());
    float tnorm = (float)cv::norm(affine.translation());
    bool integrate = (rnorm + tnorm)/2 >= p.tsdf_min_camera_movement;
    if (integrate)
    {
      //ScopeTime time("tsdf");
      //volume_->integrate(dists_, poses_.back(), p.intr);
      volume_->integrate(dists_, images_, poses_.back(), p.intr);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Ray casting
    {
      //ScopeTime time("ray-cast-all");
#if defined USE_DEPTH
      volume_->raycast(poses_.back(), p.intr, prev_.depth_pyr[0], prev_.normals_pyr[0]);
      for (int i = 1; i < LEVELS; ++i)
        resizeDepthNormals(prev_.depth_pyr[i-1], prev_.normals_pyr[i-1], prev_.depth_pyr[i], prev_.normals_pyr[i]);
#else
        volume_->raycast(poses_.back(), p.intr, prev_.points_pyr[0], prev_.normals_pyr[0]);
        for (int i = 1; i < LEVELS; ++i)
            resizePointsNormals(prev_.points_pyr[i-1], prev_.normals_pyr[i-1], prev_.points_pyr[i], prev_.normals_pyr[i]);
#endif
        cuda::waitAllDefaultStream();
    }

    return ++frame_counter_, true;
}

void vm::scanner::Scanner::renderImage(cuda::Image& image, int flag)
{
  const ScannerParams& p = params_;
  image.create(p.rows, flag != 3 ? p.cols : p.cols * 2);

#if defined USE_DEPTH
  #define PASS1 prev_.depth_pyr
#else
  #define PASS1 prev_.points_pyr
#endif

  if (flag < 1 || flag > 3)
    cuda::renderImage(PASS1[0], prev_.normals_pyr[0], params_.intr, params_.light_pose, image);
  else if (flag == 2)
    cuda::renderTangentColors(prev_.normals_pyr[0], image);
  else /* if (flag == 3) */
  {
    cuda::DeviceArray2D<RGB> i1(p.rows, p.cols, image.ptr(), image.step());
    cuda::DeviceArray2D<RGB> i2(p.rows, p.cols, image.ptr() + p.cols, image.step());

    cuda::renderImage(PASS1[0], prev_.normals_pyr[0], params_.intr, params_.light_pose, i1);
    //cuda::renderTangentColors(prev_.normals_pyr[0], i2);
    cuda::renderVertexColors(PASS1[0], prev_.normals_pyr[0], params_.intr, params_.light_pose, images_, i2);
  }
	#undef PASS1
}

void vm::scanner::Scanner::renderImage(cuda::Image& image, const Affine3f& pose, int flag)
{
  const ScannerParams& p = params_;
  image.create(p.rows, flag != 3 ? p.cols : p.cols * 2);
  depths_.create(p.rows, p.cols);
  normals_.create(p.rows, p.cols);
  points_.create(p.rows, p.cols);

#if defined USE_DEPTH
  #define PASS1 depths_
#else
  #define PASS1 points_
#endif

  volume_->raycast(pose, p.intr, PASS1, normals_);

  if (flag < 1 || flag > 3)
    cuda::renderImage(PASS1, normals_, params_.intr, params_.light_pose, image);
	else if (flag == 2)
    cuda::renderTangentColors(normals_, image);
  else /* if (flag == 3) */
  {
    cuda::DeviceArray2D<RGB> i1(p.rows, p.cols, image.ptr(), image.step());
    cuda::DeviceArray2D<RGB> i2(p.rows, p.cols, image.ptr() + p.cols, image.step());

    cuda::renderImage(PASS1, normals_, params_.intr, params_.light_pose, i1);
    cuda::renderTangentColors(normals_, i2);
	}
	#undef PASS1
}