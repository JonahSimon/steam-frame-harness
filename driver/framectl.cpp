// driver_framectl: SteamVR driver that hooks the shared IVRDriverInput / IVRServerDriverHost vtables so other
// drivers' (vrlink = Steam Frame) inputs and poses can be overridden from a localhost UDP socket.
// Protocol (one command per datagram, text reply): see README.md.
#define _USE_MATH_DEFINES
#include "openvr_driver.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace vr;

static const int kPort = 47823;
static std::mutex mu;
static FILE *logfile;

static void L(const char *fmt, ...) {
  if (!logfile) return;
  char ts[32]; auto now = std::chrono::system_clock::now();
  time_t t = std::chrono::system_clock::to_time_t(now);
  int ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  strftime(ts, sizeof ts, "%F %T", localtime(&t));
  fprintf(logfile, "%s.%03d ", ts, ms);
  va_list a; va_start(a, fmt); vfprintf(logfile, fmt, a); va_end(a);
  fputc('\n', logfile); fflush(logfile);
}

// ---- component / pose state ----
struct Comp { PropertyContainerHandle_t cont; std::string name; bool scalar; void *owner; float real = 0; bool ov = false; float ovv = 0; };
static std::map<VRInputComponentHandle_t, Comp> comps;
struct PoseSt { bool have = false; DriverPose_t real{}; bool ov = false; DriverPose_t ovp{}; void *owner = nullptr; int ctl = -1; };
// While the virtual controllers are spawned, real controllers report disconnected so SteamVR gives the hand roles
// to the virtual ones (otherwise a woken real controller can take "left"/"right" and swallow the injected input).
static std::atomic<bool> hideReal{false};
static PoseSt poses[k_unMaxTrackedDeviceCount];

// vrserver hands each driver its own interface object (shared vtable) and attributes calls by object,
// so we call originals with the object of the driver that owns the component / device.
static void *ourIn, *ourHost;
typedef EVRInputError (*CreateBoolFn)(void *, PropertyContainerHandle_t, const char *, VRInputComponentHandle_t *);
typedef EVRInputError (*UpdBoolFn)(void *, VRInputComponentHandle_t, bool, double);
typedef EVRInputError (*CreateScalFn)(void *, PropertyContainerHandle_t, const char *, VRInputComponentHandle_t *, EVRScalarType, EVRScalarUnits);
typedef EVRInputError (*UpdScalFn)(void *, VRInputComponentHandle_t, float, double);
typedef void (*PoseFn)(void *, uint32_t, const DriverPose_t &, uint32_t);
// vrlink turns the Frame's EVTYPE_OPEN_DASHBOARD into IVRServerInternal_XXX slot 57 (0, "remote", ...); found by
// disassembling driver_vrlink.so (SteamVR 2.18.4). With nodash on we drop it so a game keeps input focus unattended.
typedef uint64_t (*Gen5Fn)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static Gen5Fn oShowDash;
static std::atomic<bool> nodash{false};
static void *serverInternal;
static uint64_t hShowDash(void *s, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
  const char *key = b ? (const char *)b : "";
  bool drop = nodash && !strcmp(key, "remote");
  L("IVRServerInternal::ShowDashboard(%llu, \"%s\")%s", (unsigned long long)a, key, drop ? " DROPPED (nodash)" : "");
  return drop ? 0 : oShowDash(s, a, b, c, d, e);
}
static CreateBoolFn oCreateBool; static UpdBoolFn oUpdBool; static CreateScalFn oCreateScal; static UpdScalFn oUpdScal; static PoseFn oPose;

// Device label for a property container: hmd / left / right / <serial>
static std::string label(PropertyContainerHandle_t c) {
  ETrackedPropertyError e;
  int cls = VRProperties()->GetInt32Property(c, Prop_DeviceClass_Int32, &e);
  if (cls == TrackedDeviceClass_HMD) return "hmd";
  int role = VRProperties()->GetInt32Property(c, Prop_ControllerRoleHint_Int32, &e);
  if (role == TrackedControllerRole_LeftHand) return "left";
  if (role == TrackedControllerRole_RightHand) return "right";
  return VRProperties()->GetStringProperty(c, Prop_SerialNumber_String, &e);
}

static EVRInputError hCreateBool(void *s, PropertyContainerHandle_t c, const char *n, VRInputComponentHandle_t *h) {
  EVRInputError r = oCreateBool(s, c, n, h);
  if (r == VRInputError_None && h) { std::lock_guard<std::mutex> g(mu); comps[*h] = {c, n, false, s}; L("create bool %llu cont=%llu %s", (unsigned long long)*h, (unsigned long long)c, n); }
  return r;
}
static EVRInputError hCreateScal(void *s, PropertyContainerHandle_t c, const char *n, VRInputComponentHandle_t *h, EVRScalarType t, EVRScalarUnits u) {
  EVRInputError r = oCreateScal(s, c, n, h, t, u);
  if (r == VRInputError_None && h) { std::lock_guard<std::mutex> g(mu); comps[*h] = {c, n, true, s}; L("create scalar %llu cont=%llu %s", (unsigned long long)*h, (unsigned long long)c, n); }
  return r;
}
static EVRInputError hUpdBool(void *s, VRInputComponentHandle_t h, bool v, double t) {
  { std::lock_guard<std::mutex> g(mu); auto it = comps.find(h);
    if (it != comps.end()) {
      if (it->second.owner != ourIn && (it->second.real != 0) != v) L("real %s %s=%d", label(it->second.cont).c_str(), it->second.name.c_str(), v);
      it->second.real = v; if (it->second.ov) v = it->second.ovv != 0;
    } }
  return oUpdBool(s, h, v, t);
}
static EVRInputError hUpdScal(void *s, VRInputComponentHandle_t h, float v, double t) {
  { std::lock_guard<std::mutex> g(mu); auto it = comps.find(h);
    if (it != comps.end()) { it->second.real = v; if (it->second.ov) v = it->second.ovv; } }
  return oUpdScal(s, h, v, t);
}
static void hPose(void *s, uint32_t i, const DriverPose_t &p, uint32_t sz) {
  const DriverPose_t *use = &p; DriverPose_t tmp;
  if (i < k_unMaxTrackedDeviceCount) {
    std::lock_guard<std::mutex> g(mu); poses[i].owner = s; poses[i].have = true; poses[i].real = p;
    if (poses[i].ctl < 0) { ETrackedPropertyError e;
      poses[i].ctl = VRProperties()->GetInt32Property(VRProperties()->TrackedDeviceToPropertyContainer(i), Prop_DeviceClass_Int32, &e) == TrackedDeviceClass_Controller; }
    if (poses[i].ov) { tmp = poses[i].ovp; use = &tmp; }
    else if (hideReal && poses[i].ctl == 1) {
      tmp = p; tmp.deviceIsConnected = false; tmp.poseIsValid = false; tmp.result = TrackingResult_Uninitialized; use = &tmp;
    }
  }
  oPose(s, i, *use, sz);
}

struct Patched { void **vt; int slot; void *orig; };
static std::vector<Patched> patched;
static void writeSlot(void **vt, int slot, void *fn) {
  long pg = sysconf(_SC_PAGESIZE); uintptr_t a = (uintptr_t)&vt[slot] & ~(pg - 1);
  mprotect((void *)a, pg * 2, PROT_READ | PROT_WRITE);
  vt[slot] = fn;
  mprotect((void *)a, pg * 2, PROT_READ);
}
template <class F> static void patch(void *obj, int slot, void *hook, F *orig) {
  void **vt = *(void ***)obj;
  *orig = (F)vt[slot];
  patched.push_back({vt, slot, vt[slot]});
  writeSlot(vt, slot, hook);
  L("patched vtable %p slot %d", (void *)vt, slot);
}

// ---- commands ----
static void push(VRInputComponentHandle_t h, const Comp &c) {
  float v = c.ov ? c.ovv : c.real;
  EVRInputError e = c.scalar ? oUpdScal(c.owner, h, v, 0) : oUpdBool(c.owner, h, v != 0, 0);
  if (e != VRInputError_None) L("update %s -> error %d", c.name.c_str(), e);
}

static std::vector<int> devIndices(const std::string &dev) {  // needs mu held
  std::vector<int> r;
  if (!dev.empty() && isdigit(dev[0])) { r.push_back(atoi(dev.c_str()) % k_unMaxTrackedDeviceCount); return r; }
  for (uint32_t i = 0; i < k_unMaxTrackedDeviceCount; i++)
    if (poses[i].have && label(VRProperties()->TrackedDeviceToPropertyContainer(i)) == dev) r.push_back(i);
  return r;
}

static HmdQuaternion_t qmul(const HmdQuaternion_t &a, const HmdQuaternion_t &b) {
  return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
static void qrot(const HmdQuaternion_t &q, const double v[3], double out[3]) {
  HmdQuaternion_t r = qmul(qmul(q, {0, v[0], v[1], v[2]}), {q.w, -q.x, -q.y, -q.z});
  out[0] = r.x; out[1] = r.y; out[2] = r.z;
}

// Pose in the HMD's driver space at offset d (head frame, metres) and rotation q relative to the head. needs mu held
static DriverPose_t headRelative(const double d[3], HmdQuaternion_t q) {
  DriverPose_t p = poses[0].have ? poses[0].real : DriverPose_t{};
  if (!poses[0].have) { p.qRotation = p.qWorldFromDriverRotation = p.qDriverFromHeadRotation = {1, 0, 0, 0}; }
  double o[3]; qrot(p.qRotation, d, o);
  for (int k = 0; k < 3; k++) { p.vecPosition[k] += o[k]; p.vecDriverFromHeadTranslation[k] = 0; }
  p.qRotation = qmul(p.qRotation, q); p.qDriverFromHeadRotation = {1, 0, 0, 0};
  return p;
}

static HmdQuaternion_t qinv(const HmdQuaternion_t &q) { return {q.w, -q.x, -q.y, -q.z}; }

// Set p (driver space) from a world-space (vrserver raw universe) pose, keeping p's world-from-driver transform.
static void setWorld(DriverPose_t &p, const double pw[3], HmdQuaternion_t qw) {
  double d[3] = {pw[0] - p.vecWorldFromDriverTranslation[0], pw[1] - p.vecWorldFromDriverTranslation[1], pw[2] - p.vecWorldFromDriverTranslation[2]};
  HmdQuaternion_t inv = qinv(p.qWorldFromDriverRotation);
  qrot(inv, d, p.vecPosition);
  p.qRotation = qmul(inv, qw);
  p.qDriverFromHeadRotation = {1, 0, 0, 0};
  for (int k = 0; k < 3; k++) p.vecDriverFromHeadTranslation[k] = 0;
}
static void worldPos(const DriverPose_t &p, double out[3]) {
  double h[3]; qrot(p.qRotation, p.vecDriverFromHeadTranslation, h);
  double d[3] = {p.vecPosition[0] + h[0], p.vecPosition[1] + h[1], p.vecPosition[2] + h[2]};
  qrot(p.qWorldFromDriverRotation, d, out);
  for (int k = 0; k < 3; k++) out[k] += p.vecWorldFromDriverTranslation[k];
}

static void finishPose(DriverPose_t &p, bool connected) {
  for (int k = 0; k < 3; k++) p.vecVelocity[k] = p.vecAngularVelocity[k] = p.vecAcceleration[k] = p.vecAngularAcceleration[k] = 0;
  p.poseIsValid = p.deviceIsConnected = connected; p.result = connected ? TrackingResult_Running_OK : TrackingResult_Uninitialized;
  p.poseTimeOffset = 0; p.willDriftInYaw = false; p.shouldApplyHeadModel = false;
}

// ---- virtual Frame controllers (spawned on demand; real ones are asleep when nobody is around) ----
struct Btn { const char *path; bool scalar; EVRScalarUnits u; };
static const Btn kComps[] = {
  {"/input/system/click"}, {"/input/system/touch"}, {"/input/menu/click"}, {"/input/menu/touch"}, {"/input/view/click"}, {"/input/view/touch"},
  {"/input/a/click"}, {"/input/a/touch"}, {"/input/b/click"}, {"/input/b/touch"}, {"/input/x/click"}, {"/input/x/touch"},
  {"/input/y/click"}, {"/input/y/touch"}, {"/input/dpad_up/click"}, {"/input/dpad_up/touch"}, {"/input/dpad_right/click"},
  {"/input/dpad_right/touch"}, {"/input/dpad_down/click"}, {"/input/dpad_down/touch"}, {"/input/dpad_left/click"}, {"/input/dpad_left/touch"},
  {"/input/bumper/click"}, {"/input/bumper/touch"}, {"/input/trigger/click"}, {"/input/trigger/touch"},
  {"/input/trigger/value", true, VRScalarUnits_NormalizedOneSided}, {"/input/grip/click"}, {"/input/grip/touch"},
  {"/input/grip/value", true, VRScalarUnits_NormalizedOneSided}, {"/input/thumbstick/click"}, {"/input/thumbstick/touch"},
  {"/input/thumbstick/x", true, VRScalarUnits_NormalizedTwoSided}, {"/input/thumbstick/y", true, VRScalarUnits_NormalizedTwoSided},
  {"/input/thumbrest/touch"},
};

class VController : public ITrackedDeviceServerDriver {
 public:
  explicit VController(bool left) : left_(left) {}
  uint32_t idx = k_unTrackedDeviceIndexInvalid;
  EVRInitError Activate(uint32_t i) override {
    idx = i;
    auto c = VRProperties()->TrackedDeviceToPropertyContainer(i);
    // Same identity strings vrlink reports for real Frame controllers (vrserver.txt, SteamVR 2.18.4), so games pick
    // the same bindings, hand models and render models. Only the serial says these are virtual.
    VRProperties()->SetStringProperty(c, Prop_TrackingSystemName_String, "cv");
    VRProperties()->SetStringProperty(c, Prop_ModelNumber_String, left_ ? "Steam_Frame_Controller_Left" : "Steam_Frame_Controller_Right");
    VRProperties()->SetStringProperty(c, Prop_RenderModelName_String, left_ ? "{frame_controller}frame_controller_left" : "{frame_controller}frame_controller_right");
    VRProperties()->SetStringProperty(c, Prop_ResourceRoot_String, "frame_controller");
    VRProperties()->SetStringProperty(c, Prop_ManufacturerName_String, "Valve");
    VRProperties()->SetStringProperty(c, Prop_ControllerType_String, "frame_controller");
    VRProperties()->SetStringProperty(c, Prop_InputProfilePath_String, "{frame_controller}/input/frame_controller_profile.json");
    VRProperties()->SetInt32Property(c, Prop_ControllerRoleHint_Int32, left_ ? TrackedControllerRole_LeftHand : TrackedControllerRole_RightHand);
    VRProperties()->SetInt32Property(c, Prop_DeviceClass_Int32, TrackedDeviceClass_Controller);
    VRProperties()->SetBoolProperty(c, Prop_DeviceProvidesBatteryStatus_Bool, false);
    for (auto &b : kComps) {
      VRInputComponentHandle_t h;
      // through the hooked _005 object so the components land in `comps`
      if (b.scalar) hCreateScal(ourIn, c, b.path, &h, VRScalarType_Absolute, b.u); else hCreateBool(ourIn, c, b.path, &h);
    }
    VRInputComponentHandle_t hap; VRDriverInput()->CreateHapticComponent(c, "/output/haptic", &hap);
    std::lock_guard<std::mutex> g(mu);
    double d[3] = {left_ ? -0.2 : 0.2, -0.35, -0.35};
    poses[i].ovp = headRelative(d, {1, 0, 0, 0}); finishPose(poses[i].ovp, true);
    poses[i].real = poses[i].ovp; poses[i].owner = ourHost; poses[i].have = poses[i].ov = true;
    L("virtual %s controller active at index %u", left_ ? "left" : "right", i);
    return VRInitError_None;
  }
  void Deactivate() override { idx = k_unTrackedDeviceIndexInvalid; }
  void EnterStandby() override {}
  void *GetComponent(const char *) override { return nullptr; }
  void DebugRequest(const char *, char *r, uint32_t n) override { if (n) r[0] = 0; }
  DriverPose_t GetPose() override { std::lock_guard<std::mutex> g(mu); return idx < k_unMaxTrackedDeviceCount ? poses[idx].ovp : DriverPose_t{}; }
 private:
  bool left_;
};
static VController vleft(true), vright(false);
static std::atomic<int> spawnReq{0};  // handled in RunFrame (vrserver's driver thread)
static bool spawned;

static std::string cmd(const std::string &line) {
  std::istringstream in(line); std::string op, dev, name; in >> op >> dev >> name;
  std::ostringstream o;
  if (op == "ping") return "ok framectl 1";
  if (op == "list") {
    std::lock_guard<std::mutex> g(mu);
    for (auto &kv : comps) o << label(kv.second.cont) << '\t' << kv.second.name << '\t' << (kv.second.scalar ? "scalar" : "bool")
                             << '\t' << kv.second.real << '\t' << (kv.second.ov ? std::to_string(kv.second.ovv) : "-") << '\n';
    return o.str();
  }
  if (op == "set" || op == "rel") {  // set <dev> <component> <value> | rel <dev> <component|all>
    float v = 0; in >> v; int n = 0;
    std::lock_guard<std::mutex> g(mu);
    for (auto &kv : comps) {
      Comp &c = kv.second;
      if ((dev != "all" && label(c.cont) != dev) || (name != c.name && name != "all")) continue;
      if (op == "set") { c.ov = true; c.ovv = v; } else c.ov = false;
      push(kv.first, c); n++;
    }
    L("%s -> %d components", line.c_str(), n);
    return n ? "ok " + std::to_string(n) : "err no component " + dev + " " + name;
  }
  if (op == "poses") {
    std::lock_guard<std::mutex> g(mu);
    for (uint32_t i = 0; i < k_unMaxTrackedDeviceCount; i++) {
      if (!poses[i].have) continue;
      const DriverPose_t &p = poses[i].ov ? poses[i].ovp : poses[i].real;
      o << i << '\t' << label(VRProperties()->TrackedDeviceToPropertyContainer(i)) << '\t' << (poses[i].ov ? "override" : "real")
        << "\tconnected=" << p.deviceIsConnected << " valid=" << p.poseIsValid << " result=" << p.result
        << "\tpos=" << p.vecPosition[0] << ',' << p.vecPosition[1] << ',' << p.vecPosition[2]
        << "\tq=" << p.qRotation.w << ',' << p.qRotation.x << ',' << p.qRotation.y << ',' << p.qRotation.z;
      double w[3]; worldPos(p, w);
      o << "\tworld_pos=" << w[0] << ',' << w[1] << ',' << w[2] << '\n';
    }
    return o.str();
  }
  if (op == "pose") {  // pose <dev> off | hold | x y z [qw qx qy qz] | head dx dy dz [qw qx qy qz]
    std::lock_guard<std::mutex> g(mu);
    std::vector<int> ids = devIndices(dev);
    if (ids.empty()) return "err no device " + dev;
    bool head = name == "head", world = name == "world", level = name == "level";
    double v[3] = {0, 0, 0}, q[4] = {1, 0, 0, 0}, yaw = 0, height = NAN;
    if (level) { in >> yaw; if (!(in >> height)) height = NAN; }
    else if (name != "off" && name != "hold") {
      if (head || world) in >> v[0]; else v[0] = atof(name.c_str());
      in >> v[1] >> v[2];
      for (double &x : q) if (!(in >> x)) break;
      double qn = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); for (double &x : q) x /= qn;
    }
    for (int i : ids) {
      PoseSt &ps = poses[i];
      if (hideReal && ps.ctl == 1 && ps.owner != ourHost) { ps.ov = false; continue; }  // hidden real controller
      if (name == "off") { ps.ov = false; continue; }
      DriverPose_t p = ps.have ? ps.real : DriverPose_t{};
      if (head) p = headRelative(v, {q[0], q[1], q[2], q[3]});
      else if (world) setWorld(p, v, {q[0], q[1], q[2], q[3]});
      else if (level) {  // upright, facing yaw degrees (left-positive) from world -Z, at the current world position
        double w[3]; worldPos(ps.have ? ps.real : p, w); if (!std::isnan(height)) w[1] = height;
        double h = yaw * M_PI / 360; setWorld(p, w, {std::cos(h), 0, std::sin(h), 0});
      }
      else if (name != "hold") { for (int k = 0; k < 3; k++) p.vecPosition[k] = v[k]; p.qRotation = {q[0], q[1], q[2], q[3]}; }
      finishPose(p, true); ps.ovp = p; ps.ov = true;
    }
    L("%s -> %zu devices", line.c_str(), ids.size());
    return "ok " + std::to_string(ids.size());
  }
  if (op == "dash") {  // dash close: same call vrlink makes for the Frame's EVTYPE_CLOSE_DASHBOARD (slot 58)
    if (!serverInternal) return "err dash hooks disabled (unknown SteamVR build; see driver.log)";
    typedef uint64_t (*CloseFn)(void *, const char *, uint64_t);
    ((CloseFn)(*(void ***)serverInternal)[58])(serverInternal, "remote", 1);
    L("%s", line.c_str());
    return "ok";
  }
  if (op == "nodash") {  // nodash on|off: drop Frame-initiated dashboard opens
    nodash = dev == "on"; L("%s", line.c_str());
    return std::string("ok nodash ") + (nodash ? "on" : "off") + (oShowDash ? "" : " (hook not installed)");
  }
  if (op == "spawn" || op == "despawn") {  // virtual controllers on/off (despawn = reported disconnected)
    spawnReq = op == "spawn" ? 1 : -1; L("%s", line.c_str());
    return "ok";
  }
  if (op == "clear") {
    std::lock_guard<std::mutex> g(mu);
    for (auto &kv : comps) if (kv.second.ov) { kv.second.ov = false; push(kv.first, kv.second); }
    for (auto &p : poses) p.ov = false;
    L("clear");
    return "ok";
  }
  return "err unknown command: " + line;
}

static std::atomic<bool> running{false};
static std::thread netThread, pumpThread;

static void netLoop() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(kPort); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, (sockaddr *)&a, sizeof a) < 0) { L("bind %d failed: %s", kPort, strerror(errno)); return; }
  timeval tv{0, 200000}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  char buf[2048];
  while (running) {
    sockaddr_in from{}; socklen_t fl = sizeof from;
    ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, (sockaddr *)&from, &fl);
    if (n <= 0) continue;
    buf[n] = 0; std::string line(buf); while (!line.empty() && isspace(line.back())) line.pop_back();
    std::string r = cmd(line);
    sendto(fd, r.data(), r.size(), 0, (sockaddr *)&from, fl);
  }
  close(fd);
}

// Keeps overridden poses alive at ~90 Hz even when the real device (sleeping controller) sends nothing.
static void pumpLoop() {
  while (running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(11));
    struct P { uint32_t i; DriverPose_t p; void *o; };
    std::vector<P> out;
    { std::lock_guard<std::mutex> g(mu);
      for (uint32_t i = 0; i < k_unMaxTrackedDeviceCount; i++) if (poses[i].ov && poses[i].owner) out.push_back({i, poses[i].ovp, poses[i].owner}); }
    for (auto &x : out) oPose(x.o, x.i, x.p, sizeof(DriverPose_t));
  }
}

class Provider : public IServerTrackedDeviceProvider {
 public:
  EVRInitError Init(IVRDriverContext *ctx) override {
    VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
    std::string dir = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.local/state/frame-ctl";
    std::string lp = dir + "/driver.log";
    struct stat st;
    if (!stat(lp.c_str(), &st) && st.st_size > 20 << 20) rename(lp.c_str(), (lp + ".1").c_str());  // keep ~40 MB max
    logfile = fopen(lp.c_str(), "a");
    L("init");
    EVRInitError e;
    // vrlink (Steam Frame) uses IVRDriverInput_005; its first 4 slots match the public _004 layout.
    // ponytail: one version hooked; drivers on other input versions pass through untouched.
    for (const char *v : {"IVRDriverInput_005"}) {
      void *in = ctx->GetGenericInterface(v, &e);
      L("%s -> %p", v, in);
      if (!in) continue;
      ourIn = in;
      patch(in, 0, (void *)hCreateBool, &oCreateBool); patch(in, 1, (void *)hUpdBool, &oUpdBool);
      patch(in, 2, (void *)hCreateScal, &oCreateScal); patch(in, 3, (void *)hUpdScal, &oUpdScal);
    }
    // ponytail: only _006 hooked; if a driver uses another host version its poses pass through untouched.
    void *host = ctx->GetGenericInterface("IVRServerDriverHost_006", &e);
    L("IVRServerDriverHost_006 -> %p", host);
    if (host) { ourHost = host; patch(host, 1, (void *)hPose, &oPose); }
    // The slot numbers come from SteamVR 2.18.4 (bin/version.txt 1792185270); on any other build the slots may be
    // different functions, so don't touch them unless FRAMECTL_FORCE_NODASH=1.
    std::string build;
    { char exe[4096]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
      if (n > 0) { exe[n] = 0; std::string v = std::string(exe);
        v = v.substr(0, v.rfind('/')); v = v.substr(0, v.rfind('/')) + "/version.txt";  // bin/linux64/vrserver -> bin/version.txt
        if (FILE *f = fopen(v.c_str(), "r")) { char b[64] = {0}; if (fgets(b, sizeof b, f)) build = b; fclose(f); }
        while (!build.empty() && isspace(build.back())) build.pop_back(); } }
    bool knownBuild = build == "1792185270" || (getenv("FRAMECTL_FORCE_NODASH") && !strcmp(getenv("FRAMECTL_FORCE_NODASH"), "1"));
    void *si = knownBuild ? ctx->GetGenericInterface("IVRServerInternal_XXX", &e) : nullptr;
    L("SteamVR build %s, nodash/dash hooks %s, IVRServerInternal_XXX -> %p", build.c_str(), knownBuild ? "enabled" : "DISABLED (unknown build)", si);
    serverInternal = si;
    if (si) patch(si, 57, (void *)hShowDash, &oShowDash);
    running = true; netThread = std::thread(netLoop); pumpThread = std::thread(pumpLoop);
    VRDriverLog()->Log("framectl: hooks installed, UDP 127.0.0.1:47823");
    return VRInitError_None;
  }
  void Cleanup() override {
    running = false;
    if (netThread.joinable()) netThread.join();
    if (pumpThread.joinable()) pumpThread.join();
    for (auto &p : patched) writeSlot(p.vt, p.slot, p.orig);
    L("cleanup"); VR_CLEANUP_SERVER_DRIVER_CONTEXT();
  }
  const char *const *GetInterfaceVersions() override { return k_InterfaceVersions; }
  void RunFrame() override {
    VREvent_t ev;
    while (VRServerDriverHost()->PollNextEvent(&ev, sizeof ev))
      if (ev.eventType == VREvent_Input_HapticVibration)
        L("haptic %s dur=%.3f freq=%.1f amp=%.2f", ev.data.hapticVibration.containerHandle == VRProperties()->TrackedDeviceToPropertyContainer(vleft.idx) ? "left" : "right",
          ev.data.hapticVibration.fDurationSeconds, ev.data.hapticVibration.fFrequency, ev.data.hapticVibration.fAmplitude);
    int r = spawnReq.exchange(0);
    if (r) hideReal = r > 0;
    if (r > 0 && !spawned) {
      spawned = true;
      VRServerDriverHost()->TrackedDeviceAdded("framectl-left", TrackedDeviceClass_Controller, &vleft);
      VRServerDriverHost()->TrackedDeviceAdded("framectl-right", TrackedDeviceClass_Controller, &vright);
    } else if (r) {
      std::lock_guard<std::mutex> g(mu);
      for (VController *v : {&vleft, &vright}) if (v->idx < k_unMaxTrackedDeviceCount) {
        DriverPose_t &p = poses[v->idx].ovp; if (r > 0) { double d[3] = {v == &vleft ? -0.2 : 0.2, -0.35, -0.35}; p = headRelative(d, {1, 0, 0, 0}); }
        finishPose(p, r > 0); poses[v->idx].ov = true;
      }
    }
  }
  bool ShouldBlockStandbyMode() override { return false; }
  void EnterStandby() override {}
  void LeaveStandby() override {}
};

static Provider provider;
extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *name, int *ret) {
  if (!strcmp(name, IServerTrackedDeviceProvider_Version)) return &provider;
  if (ret) *ret = VRInitError_Init_InterfaceNotFound;
  return nullptr;
}
