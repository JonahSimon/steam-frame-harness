#include <map>
// vrprobe: independent OpenVR client readback for frame-ctl checks.
// Prints one line per connected device: index class role activity pose(raw) buttons axes.
#include "openvr.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <cstdlib>
#include <unistd.h>
using namespace vr;
int main(int argc, char **argv) {
  EVRInitError e;
  EVRApplicationType type = argc < 2 || !strcmp(argv[1], "setbool") || !strcmp(argv[1], "timing") ? VRApplication_Background : !strcmp(argv[1], "scene") ? VRApplication_Scene : VRApplication_Overlay;
  IVRSystem *s = VR_Init(&e, type);
  if (!s) { printf("init error %d\n", e); return 1; }
  if (argc >= 2 && !strcmp(argv[1], "timing")) {  // vrprobe timing <secs>: per-frame compositor timings
    double secs = argc > 2 ? atof(argv[2]) : 30;
    std::vector<Compositor_FrameTiming> all; uint32_t last = 0;
    for (int t = 0; t < secs * 4; t++) {
      Compositor_FrameTiming ft[128]; ft[0].m_nSize = sizeof(Compositor_FrameTiming);
      uint32_t n = VRCompositor()->GetFrameTimings(ft, 128);
      for (uint32_t i = 0; i < n; i++) if (ft[i].m_nFrameIndex > last) { all.push_back(ft[i]); last = ft[i].m_nFrameIndex; }
      usleep(250000);
    }
    auto stat = [&](const char *name, auto f) {
      std::vector<float> v; for (auto &x : all) v.push_back(f(x));
      std::sort(v.begin(), v.end());
      double sum = 0; for (float x : v) sum += x;
      if (v.empty()) { printf("%s n=0\n", name); return; }
      printf("%-22s mean=%6.3f median=%6.3f p95=%6.3f\n", name, sum / v.size(), v[v.size() / 2], v[v.size() * 95 / 100]);
    };
    printf("frames=%zu\n", all.size());
    stat("total_gpu_ms", [](const Compositor_FrameTiming &x) { return x.m_flTotalRenderGpuMs; });
    stat("compositor_gpu_ms", [](const Compositor_FrameTiming &x) { return x.m_flCompositorRenderGpuMs; });
    stat("compositor_cpu_ms", [](const Compositor_FrameTiming &x) { return x.m_flCompositorRenderCpuMs; });
    stat("app_interval_ms", [](const Compositor_FrameTiming &x) { return x.m_flClientFrameIntervalMs; });
    stat("app_submit_cpu_ms", [](const Compositor_FrameTiming &x) { return x.m_flSubmitFrameMs + x.m_flWaitGetPosesCalledMs; });
    uint32_t dropped = 0, repro = 0; for (auto &x : all) { dropped += x.m_nNumDroppedFrames; repro += (x.m_nReprojectionFlags != 0); }
    printf("dropped=%u reprojected_frames=%u\n", dropped, repro);
    std::map<uint32_t, uint32_t> flags; for (auto &x : all) flags[x.m_nReprojectionFlags]++;
    printf("reprojection_flags:"); for (auto &f : flags) printf(" 0x%x=%u", f.first, f.second); printf("\n");
    VR_Shutdown(); return 0;
  }
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
