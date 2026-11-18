// ovprobe <overlay-key> [aim <u> <v> [sx sy sz]] : runs on the headset.
// Prints a SteamVR overlay's visibility and where a ray from the standing-space source (default 0,1.6,0) hits it.
// `aim` searches for the ray from the source that lands on overlay uv (u,v) and prints `aim px py pz qw qx qy qz`:
// a controller pose (standing space) whose -Z axis points there. frame-ctl uses it to click a flat app's VR
// window with a virtual controller, which is what makes gamescope give that app mouse/keyboard focus.
#include "openvr.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace vr;

static VROverlayHandle_t h;
static float S[3] = {0, 1.6f, 0};

static bool cast(float dx, float dy, float uv[2]) {  // direction (dx, dy, -1), normalized
    VROverlayIntersectionParams_t p{}; VROverlayIntersectionResults_t r{};
    float n = 1 / sqrtf(dx * dx + dy * dy + 1);
    p.eOrigin = TrackingUniverseStanding;
    p.vSource = {{S[0], S[1], S[2]}}; p.vDirection = {{dx * n, dy * n, -n}};
    if (!VROverlay()->ComputeOverlayIntersection(h, &p, &r)) return false;
    uv[0] = r.vUVs.v[0]; uv[1] = r.vUVs.v[1]; return true;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ovprobe <overlay-key> [aim <u> <v> [sx sy sz]]\n"); return 2; }
    EVRInitError ie; VR_Init(&ie, VRApplication_Background);
    if (ie != VRInitError_None) { fprintf(stderr, "VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(ie)); return 1; }
    if (VROverlay()->FindOverlay(argv[1], &h) != VROverlayError_None) { fprintf(stderr, "no overlay %s\n", argv[1]); VR_Shutdown(); return 1; }
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
        // quaternion rotating -Z onto d = norm(dx, dy, -1): axis = (-Z) x d, angle = acos((-Z) . d)
        float n = 1 / sqrtf(dx * dx + dy * dy + 1), d[3] = {dx * n, dy * n, -n};
        float ax[3] = {d[1], -d[0], 0};  // (0,0,-1) x d
        float c = -d[2], s = sqrtf(ax[0] * ax[0] + ax[1] * ax[1]), half = acosf(fminf(1, fmaxf(-1, c))) / 2;
        float qw = cosf(half), k = s > 1e-6f ? sinf(half) / s : 0;
        printf("got uv=%.3f,%.3f\naim %.3f %.3f %.3f %.5f %.5f %.5f %.5f\n", uv[0], uv[1], S[0], S[1], S[2], qw, ax[0] * k, ax[1] * k, ax[2] * k);
    }
    VR_Shutdown(); return 0;
}
