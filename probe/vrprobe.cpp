// vrprobe: independent OpenVR client readback for frame-ctl checks.
// Prints one line per connected device: index class role activity pose(raw) buttons axes.
#include "openvr.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
using namespace vr;
int main(int argc, char **argv) {
  EVRInitError e;
  EVRApplicationType type = argc < 2 || !strcmp(argv[1], "setbool") ? VRApplication_Background : !strcmp(argv[1], "scene") ? VRApplication_Scene : VRApplication_Overlay;
  IVRSystem *s = VR_Init(&e, type);
  if (!s) { printf("init error %d\n", e); return 1; }
  if (argc == 5 && !strcmp(argv[1], "setbool")) {  // vrprobe setbool <section> <key> 0|1
    EVRSettingsError se; VRSettings()->SetBool(argv[2], argv[3], atoi(argv[4]) != 0, &se);
    printf("setbool %s.%s=%s err=%d\n", argv[2], argv[3], argv[4], se); VR_Shutdown(); return se != VRSettingsError_None;
  }
  // scene mode: wait for input focus, let legacy input settle (argv[2] = seconds, default 2)
  if (type == VRApplication_Scene) { double w = argc > 2 ? atof(argv[2]) : 2;
    TrackedDevicePose_t wp[k_unMaxTrackedDeviceCount]; VREvent_t ev;
    for (int n = 0; n < w * 90; n++) { if (VRCompositor()) VRCompositor()->WaitGetPoses(wp, k_unMaxTrackedDeviceCount, nullptr, 0); else usleep(11000); while (s->PollNextEvent(&ev, sizeof ev)) {} }
    printf("scene_focus_pid=%u input_available=%d\n", VRCompositor() ? VRCompositor()->GetCurrentSceneFocusProcess() : 0, s->IsInputAvailable()); }
  TrackedDevicePose_t p[k_unMaxTrackedDeviceCount];
  s->GetDeviceToAbsoluteTrackingPose(TrackingUniverseRawAndUncalibrated, 0, p, k_unMaxTrackedDeviceCount);
  printf("hmd_activity=%d dashboard_visible=%d input_available=%d\n", s->GetTrackedDeviceActivityLevel(0), VROverlay() ? VROverlay()->IsDashboardVisible() : -1, s->IsInputAvailable());
  for (uint32_t i = 0; i < k_unMaxTrackedDeviceCount; i++) {
    if (!s->IsTrackedDeviceConnected(i)) continue;
    auto &m = p[i].mDeviceToAbsoluteTracking.m;
    printf("%u class=%d role=%d valid=%d pos=%.3f,%.3f,%.3f", i, s->GetTrackedDeviceClass(i),
           s->GetControllerRoleForTrackedDeviceIndex(i), p[i].bPoseIsValid, m[0][3], m[1][3], m[2][3]);
    VRControllerState_t c;
    if (s->GetTrackedDeviceClass(i) == TrackedDeviceClass_Controller || i == 0) {
      printf(" state=%d", s->GetControllerState(i, &c, sizeof c));
      printf(" pressed=0x%llx touched=0x%llx", (unsigned long long)c.ulButtonPressed, (unsigned long long)c.ulButtonTouched);
      for (int a = 0; a < 5; a++) printf(" a%d=%.2f,%.2f", a, c.rAxis[a].x, c.rAxis[a].y);
    }
    printf("\n");
  }
  VR_Shutdown();
}
