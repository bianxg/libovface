#include <string>
#include <vector>
#include <ie_iextension.h>
#include "cnn.hpp"
#include "face_reid.hpp"
#include "image_grabber.hpp"
#include "ovface_impl.h"

using namespace InferenceEngine;
using namespace ovface;

bool checkDynamicBatchSupport(const Core& ie, const std::string& device)  {
    try  {
        if (ie.GetConfig(device, CONFIG_KEY(DYN_BATCH_ENABLED)).as<std::string>() != PluginConfigParams::YES)
            return false;
    }
    catch(const std::exception&)  {
        return false;
    }
    return true;
}

VAChannel *VAChannel::create(const CVAChanParams &params) {
  VAChannelImpl * t = new VAChannelImpl();
  std::cout << "VAChannel::create " << t << std::endl;
  if (t) {
    t->init(params);
    return t;
  }
  
  return nullptr;
}

void VAChannel::destroyed(VAChannel *pChan) {
  std::cout << "VAChannel::destroy " << pChan << std::endl;
  if (pChan) {
    VAChannel *tmp = pChan;
    pChan = nullptr;
    delete tmp;
  }
}
std::vector<CIdentityParams> m_params;
std::unique_ptr<AsyncDetection<DetectedObject>> m_fd;
std::unique_ptr<FaceRecognizer> m_fr;
std::unique_ptr<Tracker> m_tracker;
cv::Mat m_frame;
cv::Mat m_prevframe;
int m_frameid;

VAChannelImpl::VAChannelImpl()
  : m_frameid(0) {

}

VAChannelImpl::~VAChannelImpl() {
  std::cout << "~VAChannelVino" << std::endl;
}

int VAChannelImpl::init(const CVAChanParams &param) {
  m_vaChanParams = param;
  const std::string fd_model_path = param.faceDetectModelPath;
  const std::string fr_model_path = param.faceRecogModelPath;
  const std::string lm_model_path = param.landmarksModelPath;
  
  std::string device = param.device;
  if (device == "")
    device = "CPU";

  std::cout << "Loading Inference Engine" << std::endl;
  Core ie;

  std::set<std::string> loadedDevices;

  std::cout << "Device info: " << device << std::endl;

  std::cout << ie.GetVersions(device) << std::endl;

  if (device.find("CPU") != std::string::npos) {
    ie.SetConfig({{PluginConfigParams::KEY_DYN_BATCH_ENABLED, PluginConfigParams::YES}}, "CPU");
  } else if (device.find("GPU") != std::string::npos) {
    ie.SetConfig({{PluginConfigParams::KEY_DYN_BATCH_ENABLED, PluginConfigParams::YES}}, "GPU");
  }
  
  loadedDevices.insert(device);
  
  if (!fd_model_path.empty()) {
    // Load face detector
    DetectorConfig face_config(fd_model_path);
    face_config.deviceName = device;
    face_config.ie = ie;
    face_config.is_async = true;
    face_config.confidence_threshold = param.detectThreshold;
    m_fd.reset(new FaceDetection(face_config));
  } else {
    m_fd.reset(new NullDetection<DetectedObject>);
  }

  if (!fd_model_path.empty() && !fr_model_path.empty() && !lm_model_path.empty()) {
    // Create face recognizer
    DetectorConfig face_registration_det_config(fd_model_path);
    face_registration_det_config.deviceName = device;
    face_registration_det_config.ie = ie;
    face_registration_det_config.is_async = false;
    face_registration_det_config.confidence_threshold = 0.9;
    CnnConfig reid_config(fr_model_path);
    reid_config.deviceName = device;
    if (checkDynamicBatchSupport(ie, device))
      reid_config.max_batch_size = param.maxBatchSize;
    else
      reid_config.max_batch_size = 1;
    reid_config.ie = ie;

    CnnConfig landmarks_config(lm_model_path);
    landmarks_config.deviceName = device;
    if (checkDynamicBatchSupport(ie, device))
      landmarks_config.max_batch_size = param.maxBatchSize;
    else
      landmarks_config.max_batch_size = 1;
    landmarks_config.ie = ie;
         
    m_fr.reset(new FaceRecognizerDefault(
    landmarks_config, reid_config,
    face_registration_det_config,
    param.reidGalleryPath, param.reidThreshold, 112, false, true));
  } else {
    std::cout << "Face recognition models are disabled!" << std::endl;
    m_fr.reset(new FaceRecognizerNull);
  }

  // Create tracker for reid
  TrackerParams tracker_reid_params;
  tracker_reid_params.min_track_duration = 1;
  tracker_reid_params.forget_delay = 50;
  tracker_reid_params.affinity_thr = 0.8f;
  tracker_reid_params.averaging_window_size_for_rects = 1;
  tracker_reid_params.averaging_window_size_for_labels = std::numeric_limits<int>::max();
  tracker_reid_params.bbox_heights_range = cv::Vec2f(10, 1080);
  tracker_reid_params.drop_forgotten_tracks = false;
  tracker_reid_params.max_num_objects_in_track = std::numeric_limits<int>::max();
  tracker_reid_params.objects_type = "face";
  m_tracker.reset(new Tracker(tracker_reid_params));
  
  return 0;
}

int VAChannelImpl::setIdentityDB(const std::vector<CIdentityParams> &params) {

  return 0;
}

int VAChannelImpl::setIdentityDB(const char *filePath) {

  return 0;
}

int VAChannelImpl::process(const CFrameData &frameData, std::vector<CResult> &results, bool bForce) {
  int ret = -1;
  if (frameData.pFrame == NULL)
    return ret;

  if (m_vaChanParams.detectInterval > 0 && m_frameid % m_vaChanParams.detectInterval != 0 && !bForce) {
    m_frameid++;
    return ret;
  }
    
  if (m_frameid > 0)
    m_prevframe = m_frame;
  
  if (frameData.format == FRAME_FOMAT_I420) {
    cv::Mat yuv(frameData.height + frameData.height/2, frameData.width, CV_8UC1, frameData.pFrame);
    cv::Mat bgr(frameData.height, frameData.width, CV_8UC3);
    cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);
    m_frame = bgr;
  } else if (frameData.format == FRAME_FOMAT_RGB) {
    cv::Mat rgb(frameData.height, frameData.width, CV_8UC3, frameData.pFrame);
    cv::Mat bgr(frameData.height, frameData.width, CV_8UC3);
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    m_frame = bgr;
  } else if (frameData.format == FRAME_FOMAT_BGR) {
    cv::Mat bgr(frameData.height, frameData.width, CV_8UC3, frameData.pFrame);
    m_frame = bgr;
  } else {
    return ret;
  }

  m_fd->enqueue(m_frame);
  m_fd->submitRequest();
  
  m_fd->wait();
  DetectedObjects faces = m_fd->fetchResults();
  
  std::vector<int> ids = m_fr->Recognize(m_frame, faces);
  
  TrackedObjects tracked_face_objects;
  for (size_t i = 0; i < faces.size(); i++) {
    tracked_face_objects.emplace_back(faces[i].rect, faces[i].confidence, ids[i]);
  }
  
  m_tracker->Process(m_frame, tracked_face_objects, m_frameid);
  
  const TrackedObjects tracked_faces = m_tracker->TrackedDetectionsWithLabels();
  for (size_t j = 0; j < tracked_faces.size(); j++) {
      const TrackedObject& face = tracked_faces[j];
      std::string face_label = m_fr->GetLabelByID(face.label);
  
      std::string label_to_draw;
      if (face.label != EmbeddingsGallery::unknown_id)
          label_to_draw += face_label;

      CResult result;
      result.rect.left = face.rect.x;
      result.rect.top = face.rect.y;
      result.rect.right = face.rect.x + face.rect.width;
      result.rect.bottom = face.rect.y + face.rect.height;
      result.frameId = m_frameid;
      result.label = label_to_draw;
      result.trackId = face.object_id;
      results.push_back(result);
  }
  
  m_frameid++;
  
  return 0;
}


