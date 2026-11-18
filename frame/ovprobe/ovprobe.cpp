// ovprobe <overlay-key> [aim <u> <v> [sx sy sz] | ray | fix <sent 7> <target 7>] : runs on the headset.
// Prints a SteamVR overlay's visibility and where a ray from the standing-space source (default 0,1.6,0) hits it.
// `aim` searches for the ray from the source that lands on overlay uv (u,v) and prints `aim px py pz qw qx qy qz`:
// a controller pose whose -Z axis points there, in the RAW tracking universe, which is the "world" that
// driver_framectl's `pose <dev> world` takes. The source is given in standing space (near the user's head) and
// converted, so the aim stays right after a recenter or with Recline VR (both move standing away from raw).
// `std` prints the same pose in standing space.
// `fix <sent: px py pz qw qx qy qz> <target: same>` reads back where SteamVR put the right controller after it was
// posed at `sent` (raw universe), measures the world transform C applied on top (Recline VR rotates every device pose
// for lying down), and prints `fix <pose>`: what to send so the controller lands on `target`, plus `away <pose>`,
// the target turned to face backwards (so its laser leaves the window), also corrected.
// `ray` casts the laser of the device holding the right-hand role (its -Z axis, pose read back from SteamVR in
// standing space) and prints `ray uv=u,v` or `ray miss`: what the laser really hits, so frame-ctl can check its aim.
// frame-ctl uses it to click a flat app's VR window with a virtual controller, which is what makes gamescope give
// that app mouse/keyboard focus.
#include "openvr.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace vr;

static VROverlayHandle_t h;
static float S[3] = {0, 1.6f, 0};

// rigid poses: 3x4 row-major matrices, built from position + quaternion (w x y z)
struct M34 { double m[3][4]; };
static M34 fromPQ(const double *v) {
    double w = v[3], x = v[4], y = v[5], z = v[6], n = 1 / sqrt(w * w + x * x + y * y + z * z);
    w *= n; x *= n; y *= n; z *= n;
    return {{{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), v[0]},
             {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x), v[1]},
             {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y), v[2]}}};
}
static M34 mul(const M34 &a, const M34 &b) {
    M34 r{};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) {
        r.m[i][j] = j == 3 ? a.m[i][3] : 0;
        for (int k = 0; k < 3; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
    }
    return r;
}
static M34 inv(const M34 &a) {
    M34 r{};
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) r.m[i][j] = a.m[j][i]; r.m[i][3] = 0; }
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) r.m[i][3] -= a.m[k][i] * a.m[k][3];
    return r;
}
static void printPQ(const char *tag, const M34 &a) {  // rotation matrix -> quaternion (Shepperd)
    const double (*m)[4] = a.m; double q[4], t = m[0][0] + m[1][1] + m[2][2];
    if (t > 0) { double s = 2 * sqrt(t + 1); q[0] = s / 4; q[1] = (m[2][1] - m[1][2]) / s; q[2] = (m[0][2] - m[2][0]) / s; q[3] = (m[1][0] - m[0][1]) / s; }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) { double s = 2 * sqrt(1 + m[0][0] - m[1][1] - m[2][2]); q[0] = (m[2][1] - m[1][2]) / s; q[1] = s / 4; q[2] = (m[0][1] + m[1][0]) / s; q[3] = (m[0][2] + m[2][0]) / s; }
    else if (m[1][1] > m[2][2]) { double s = 2 * sqrt(1 + m[1][1] - m[0][0] - m[2][2]); q[0] = (m[0][2] - m[2][0]) / s; q[1] = (m[0][1] + m[1][0]) / s; q[2] = s / 4; q[3] = (m[1][2] + m[2][1]) / s; }
    else { double s = 2 * sqrt(1 + m[2][2] - m[0][0] - m[1][1]); q[0] = (m[1][0] - m[0][1]) / s; q[1] = (m[0][2] + m[2][0]) / s; q[2] = (m[1][2] + m[2][1]) / s; q[3] = s / 4; }
    printf("%s %.4f %.4f %.4f %.5f %.5f %.5f %.5f\n", tag, m[0][3], m[1][3], m[2][3], q[0], q[1], q[2], q[3]);
}
// right-hand device: role, or (headset virtual controllers hold role 0) the one whose role hint says right
static TrackedDeviceIndex_t rightDevice() {
    TrackedDeviceIndex_t i = VRSystem()->GetTrackedDeviceIndexForControllerRole(TrackedControllerRole_RightHand);
    for (TrackedDeviceIndex_t d = 0; i >= k_unMaxTrackedDeviceCount && d < k_unMaxTrackedDeviceCount; d++)
        if (VRSystem()->GetTrackedDeviceClass(d) == TrackedDeviceClass_Controller && VRSystem()->IsTrackedDeviceConnected(d) &&
            VRSystem()->GetInt32TrackedDeviceProperty(d, Prop_ControllerRoleHint_Int32) == TrackedControllerRole_RightHand) i = d;
    return i;
}

static bool castRay(ETrackingUniverseOrigin o, const float src[3], const float dir[3], float uv[2]) {
    VROverlayIntersectionParams_t p{}; VROverlayIntersectionResults_t r{};
    p.eOrigin = o; p.vSource = {{src[0], src[1], src[2]}}; p.vDirection = {{dir[0], dir[1], dir[2]}};
    if (!VROverlay()->ComputeOverlayIntersection(h, &p, &r)) return false;
    uv[0] = r.vUVs.v[0]; uv[1] = r.vUVs.v[1]; return true;
}
static bool cast(float dx, float dy, float uv[2]) {  // standing space, from S, direction (dx, dy, -1) normalized
    float n = 1 / sqrtf(dx * dx + dy * dy + 1), d[3] = {dx * n, dy * n, -n};
    return castRay(TrackingUniverseStanding, S, d, uv);
}
// quaternion (w x y z) rotating -Z onto unit d: axis = (-Z) x d, angle = acos((-Z) . d)
static void aimQuat(const float d[3], float q[4]) {
    float ax[3] = {d[1], -d[0], 0};
    float c = -d[2], s = sqrtf(ax[0] * ax[0] + ax[1] * ax[1]), half = acosf(fminf(1, fmaxf(-1, c))) / 2;
    float k = s > 1e-6f ? sinf(half) / s : 0;
    q[0] = cosf(half); q[1] = ax[0] * k; q[2] = ax[1] * k; q[3] = ax[2] * k;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ovprobe <overlay-key> [aim <u> <v> [sx sy sz] | ray | fix <sent 7> <target 7>]\n"); return 2; }
    EVRInitError ie; VR_Init(&ie, VRApplication_Background);
    if (ie != VRInitError_None) { fprintf(stderr, "VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(ie)); return 1; }
    if (VROverlay()->FindOverlay(argv[1], &h) != VROverlayError_None) { fprintf(stderr, "no overlay %s\n", argv[1]); VR_Shutdown(); return 1; }
    if (argc >= 17 && !strcmp(argv[2], "fix")) {
        double sv[7], tv[7];
        for (int k = 0; k < 7; k++) { sv[k] = atof(argv[3 + k]); tv[k] = atof(argv[10 + k]); }
        TrackedDeviceIndex_t i = rightDevice();
        TrackedDevicePose_t ps[k_unMaxTrackedDeviceCount];
        VRSystem()->GetDeviceToAbsoluteTrackingPose(TrackingUniverseRawAndUncalibrated, 0, ps, k_unMaxTrackedDeviceCount);
        if (i >= k_unMaxTrackedDeviceCount || !ps[i].bPoseIsValid) { printf("fix nodevice\n"); VR_Shutdown(); return 4; }
        M34 A{}; for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) A.m[r][c] = ps[i].mDeviceToAbsoluteTracking.m[r][c];
        M34 Ci = inv(mul(A, inv(fromPQ(sv))));  // A = C * sent, so C^-1 undoes the extra world transform
        M34 T = fromPQ(tv), back = T;            // back: T turned 180 deg about its own Y axis
        for (int r = 0; r < 3; r++) { back.m[r][0] = -T.m[r][0]; back.m[r][2] = -T.m[r][2]; }
        double ang = acos(fmin(1, fmax(-1, (Ci.m[0][0] + Ci.m[1][1] + Ci.m[2][2] - 1) / 2))) * 180 / M_PI;
        printf("correction deg=%.2f t=%.3f,%.3f,%.3f\n", ang, Ci.m[0][3], Ci.m[1][3], Ci.m[2][3]);
        printPQ("fix", mul(Ci, T)); printPQ("away", mul(Ci, back));
        VR_Shutdown(); return 0;
    }
    if (argc >= 3 && !strcmp(argv[2], "ray")) {
        TrackedDeviceIndex_t i = rightDevice();
        TrackedDevicePose_t ps[k_unMaxTrackedDeviceCount];
        VRSystem()->GetDeviceToAbsoluteTrackingPose(TrackingUniverseStanding, 0, ps, k_unMaxTrackedDeviceCount);
        if (i >= k_unMaxTrackedDeviceCount || !ps[i].bPoseIsValid) {
            printf("ray nodevice (right-hand role -> %u)\n", i);
            for (TrackedDeviceIndex_t d = 0; d < k_unMaxTrackedDeviceCount; d++)
                if (VRSystem()->GetTrackedDeviceClass(d) != TrackedDeviceClass_Invalid)
                    printf("  dev %u class=%d role=%d connected=%d valid=%d\n", d, VRSystem()->GetTrackedDeviceClass(d),
                           VRSystem()->GetControllerRoleForTrackedDeviceIndex(d), ps[d].bDeviceIsConnected, ps[d].bPoseIsValid);
            VR_Shutdown(); return 4;
        }
        const HmdMatrix34_t &m = ps[i].mDeviceToAbsoluteTracking;
        float src[3] = {m.m[0][3], m.m[1][3], m.m[2][3]}, dir[3] = {-m.m[0][2], -m.m[1][2], -m.m[2][2]}, uv[2];
        bool hit = castRay(TrackingUniverseStanding, src, dir, uv);
        if (hit) printf("ray uv=%.3f,%.3f", uv[0], uv[1]); else printf("ray miss");
        printf(" dev=%u src=%.3f,%.3f,%.3f dir=%.3f,%.3f,%.3f\n", i, src[0], src[1], src[2], dir[0], dir[1], dir[2]);
        VR_Shutdown(); return hit ? 0 : 3;
    }
    bool aim = argc >= 5 && !strcmp(argv[2], "aim");
    if (aim && argc >= 8) for (int i = 0; i < 3; i++) S[i] = atof(argv[5 + i]);
    float w = 0; VROverlay()->GetOverlayWidthInMeters(h, &w);
    printf("visible=%d width_m=%.3f\n", VROverlay()->IsOverlayVisible(h), w);
    // find any hit: fan of directions from the source
    float dx = 0, dy = 0, uv[2]; bool hit = false;
    for (float y : {0.f, -0.2f, 0.2f, -0.4f, 0.4f}) for (float x : {0.f, -0.3f, 0.3f, -0.6f, 0.6f})
        if (!hit && cast(x, y, uv)) { dx = x; dy = y; hit = true; }
    if (!hit) { printf("nohit\n"); VR_Shutdown(); return 3; }
    printf("hit dir=%.3f,%.3f uv=%.3f,%.3f\n", dx, dy, uv[0], uv[1]);
    if (aim) {
        float tu = atof(argv[3]), tv = atof(argv[4]);
        for (int it = 0; it < 30; it++) {  // Newton on (dx,dy) -> uv with a numeric Jacobian
            float e = 1e-3f, a[2], b[2];
            if (!cast(dx, dy, uv) || !cast(dx + e, dy, a) || !cast(dx, dy + e, b)) break;
            float j00 = (a[0] - uv[0]) / e, j10 = (a[1] - uv[1]) / e, j01 = (b[0] - uv[0]) / e, j11 = (b[1] - uv[1]) / e;
            float ru = tu - uv[0], rv = tv - uv[1], det = j00 * j11 - j01 * j10;
            if (fabsf(ru) < 1e-3f && fabsf(rv) < 1e-3f) break;
            if (fabsf(det) < 1e-9f) break;
            float sx = (j11 * ru - j01 * rv) / det, sy = (-j10 * ru + j00 * rv) / det;
            for (float k = 1; k > 0.01f; k /= 2) if (cast(dx + k * sx, dy + k * sy, a)) { dx += k * sx; dy += k * sy; break; }
        }
        cast(dx, dy, uv);
        float n = 1 / sqrtf(dx * dx + dy * dy + 1), d[3] = {dx * n, dy * n, -n}, q[4];
        aimQuat(d, q);
        printf("got uv=%.3f,%.3f\nstd %.3f %.3f %.3f %.5f %.5f %.5f %.5f\n", uv[0], uv[1], S[0], S[1], S[2], q[0], q[1], q[2], q[3]);
        // standing = M * raw (M = raw zero pose in standing space), so raw = R^T (standing - t), dir_raw = R^T dir
        HmdMatrix34_t M = VRSystem()->GetRawZeroPoseToStandingAbsoluteTrackingPose();
        float rs[3], rd[3];
        for (int i = 0; i < 3; i++) {
            rs[i] = rd[i] = 0;
            for (int j = 0; j < 3; j++) { rs[i] += M.m[j][i] * (S[j] - M.m[j][3]); rd[i] += M.m[j][i] * d[j]; }
        }
        printf("rawzero_to_standing t=%.3f,%.3f,%.3f\n", M.m[0][3], M.m[1][3], M.m[2][3]);
        aimQuat(rd, q);
        printf("aim %.3f %.3f %.3f %.5f %.5f %.5f %.5f\n", rs[0], rs[1], rs[2], q[0], q[1], q[2], q[3]);
    }
    VR_Shutdown(); return 0;
}
