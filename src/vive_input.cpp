#include <string>
#include <openvr.h>
#include <chrono> // Include this for std::chrono
#include <thread>

#include "VRUtils.hpp"
#include "json.hpp" // Include nlohmann/json
#include "server.hpp"


class ViveInput {
public:
    ViveInput(std::mutex &mutex, std::condition_variable &cv, VRControllerData &data);
    ~ViveInput();
    void runVR();

private:
    vr::IVRSystem *pHMD = nullptr;
    vr::EVRInitError eError = vr::VRInitError_None;
    vr::TrackedDevicePose_t trackedDevicePose[vr::k_unMaxTrackedDeviceCount];

    std::mutex &data_mutex;
    std::condition_variable &data_cv;
    VRControllerData &shared_data;
    VRControllerData local_data;

    bool initVR();
    bool shutdownVR();

    // Variables to store previous position and time
    vr::HmdVector3_t prev_position;
    std::chrono::steady_clock::time_point prev_time;
    bool first_run = true;
    // const float velocity_threshold = 2.5f;
    const float distance_threshold = 0.1f;

};

ViveInput::ViveInput(std::mutex &mutex, std::condition_variable &cv, VRControllerData &data) 
    : data_mutex(mutex), data_cv(cv), shared_data(data) {
    if (!initVR()) {
        shutdownVR();
        throw std::runtime_error("Failed to initialize VR");
    }
}
ViveInput::~ViveInput() {
    shutdownVR();
}

void ViveInput::runVR() {
  logMessage(Info, "Starting VR loop");
  auto lastLogTime = std::chrono::steady_clock::now(); // Initialize the last log time

  while (true) {
    bool trackerDetected = false;
    VRUtils::resetJsonData(local_data);

    // update the poses
    pHMD->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, trackedDevicePose, vr::k_unMaxTrackedDeviceCount);

    for (uint32_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
      if (trackedDevicePose[i].bDeviceIsConnected && trackedDevicePose[i].bPoseIsValid
          && trackedDevicePose[i].eTrackingResult == vr::TrackingResult_Running_OK) {

      if (pHMD->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_GenericTracker) {
          trackerDetected = true;

          // Get the pose of the device
          vr::HmdMatrix34_t steamVRMatrix = trackedDevicePose[i].mDeviceToAbsoluteTracking;
          vr::HmdVector3_t position = VRTransformUtils::GetPosition(steamVRMatrix);
          vr::HmdQuaternion_t quaternion = VRTransformUtils::GetQuaternion(steamVRMatrix);
          EulerAngle euler = VRTransformUtils::QuaternionToEulerXYZ(quaternion);
          logMessage(Debug, "[POSE CM]: " + std::to_string(position.v[0] * 100) + " " + std::to_string(position.v[1] * 100) + " " + std::to_string(position.v[2] * 100));
          logMessage(Debug, "[EULER DEG]: " + std::to_string(euler.x * (180.0 / M_PI)) + " " + std::to_string(euler.y * (180.0 / M_PI)) + " " + std::to_string(euler.z * (180.0 / M_PI)));

          local_data.time = Server::getCurrentTimeWithMilliseconds();
          local_data.role = VRUtils::controllerRoleCheck(pHMD, i);
          local_data.pose_x = position.v[0];
          local_data.pose_y = position.v[1];
          local_data.pose_z = position.v[2];
          local_data.pose_qx = quaternion.x;
          local_data.pose_qy = quaternion.y;
          local_data.pose_qz = quaternion.z;
          local_data.pose_qw = quaternion.w;

          // TODO Use the data from the pogo pin connector

          // Check if the input data is reasonable
          auto current_time = std::chrono::steady_clock::now();
          if (!first_run) {
              std::chrono::duration<float> time_diff = current_time - prev_time;
              float delta_time = time_diff.count();
              float delta_x = position.v[0] - prev_position.v[0];
              float delta_y = position.v[1] - prev_position.v[1];
              float delta_z = position.v[2] - prev_position.v[2];
              float delta_distance = std::sqrt(delta_x * delta_x + delta_y * delta_y + delta_z * delta_z);
              float velocity = delta_distance / delta_time;

              logMessage(Debug, "Velocity: " + std::to_string(velocity) + " units/s");
              logMessage(Debug, "Delta pos: " + std::to_string(delta_distance) + " units");
              logMessage(Debug, "prev pos: " + std::to_string(prev_position.v[0]) + " " + std::to_string(prev_position.v[1]) + " " + std::to_string(prev_position.v[2]));
              logMessage(Debug, "cur t: " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(current_time.time_since_epoch()).count()));
              logMessage(Debug, "prev t: " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(prev_time.time_since_epoch()).count()));

              // check if delta distance is too high
              if (delta_distance > 0.05) {
                  logMessage(Warning, "Unreasonable delta_distance detected: " + std::to_string(delta_distance) + " units. Skipping this data." + "\n");
                  continue; // Skip this iteration if delta_distance is too high
              } else {
                  logMessage(Debug, "Will publish this data");
              }
          } else {
              first_run = false; // Set the flag to false after the first run
          }

          // Update previous record
          prev_position = position;
          prev_time = current_time;

          // Update shared data
          {
            std::lock_guard<std::mutex> lock(data_mutex);
            shared_data = local_data;
            shared_data.time = Server::getCurrentTimeWithMilliseconds();
          }
          data_cv.notify_one(); // Notify the server thread
        }
      }
    }

    auto currentTime = std::chrono::steady_clock::now();
    if (!trackerDetected) {
      if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastLogTime).count() >= 1) {
        logMessage(Info, "no tracker detected, currentTime: " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(currentTime.time_since_epoch()).count()));
        first_run = true; // Reset the first run flag
        lastLogTime = currentTime; // Update the last log time
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50)); // ~20Hz
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(5)); // ~200Hz
      lastLogTime = currentTime;
    }
  }
}

bool ViveInput::initVR() {
    // Initialize VR runtime
    eError = vr::VRInitError_None;
    pHMD = vr::VR_Init(&eError, vr::VRApplication_Background);
    if (eError != vr::VRInitError_None) {
        pHMD = NULL;
        std::string error_msg = vr::VR_GetVRInitErrorAsEnglishDescription(eError);
        logMessage(Error, "Unable to init VR runtime: " + error_msg);
        return false;
    } else {
        logMessage(Info, "VR runtime initialized");
    }
    return true;
}
bool ViveInput::shutdownVR() {
    // Shutdown VR runtime
    if (pHMD) {
        logMessage(Info, "Shutting down VR runtime");
        vr::VR_Shutdown();
    }
    return true;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    Server::setupSignalHandlers();

    std::mutex data_mutex;
    std::condition_variable data_cv;
    VRControllerData shared_data;

    Server server(12345, data_mutex, data_cv, shared_data);
    std::thread serverThread(&Server::start, &server);
    serverThread.detach(); // Detach the server thread

    ViveInput vive_input(data_mutex, data_cv, shared_data);
    vive_input.runVR();

    serverThread.join(); // Wait for the server thread to finish

    return 0;
}
