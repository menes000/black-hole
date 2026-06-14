// ============================================================================
//  render_wasm.cpp — BLACK HOLE + OBJECT LAUNCH (WebAssembly)
//
//  Camera is fixed in space, initially facing the black hole; drag to look
//  around, click to launch an object in that direction. (Checkered room and
//  three reflective spheres moved to a separate demo: oda_wasm.cpp / oda.html.)
//  A physically rendered Schwarzschild black hole:
//
//   • Light rays DO NOT travel straight near the black hole: the null geodesic
//     equation from general relativity is integrated step by step:
//         a = -(3/2)·h²·r̂/r⁴      (h = conserved angular momentum, rs = 1 unit)
//     This equation is the exact vector form of d²u/dφ² + u = (3/2)·rs·u²
//     Schwarzschild orbit — the photon sphere (r = 1.5·rs) and critical impact
//     parameter b = (3√3/2)·rs emerge naturally.
//   • Event horizon (r < rs): light cannot escape → absolute black.
//   • Accretion disk: inner edge at ISCO (r = 3·rs). Brightness profile
//     Shakura–Sunyaev (~r^-2.5), Kepler velocity v = √(rs/2r):
//       - Doppler beaming  δ³  (approaching side bright/bluish)
//       - gravitational redshift √(1 - rs/r)
//   • Far rays (b > 9·rs) use first-order exact deflection α = 2·rs/b
//     analytically (for speed); close rays fully integrated.
//   • Background stars lensed into arcs around the black hole.
//
//  Approximation: disk is geometrically thin and opaque.
//
//  ZERO dependencies: no SDL, no stdlib, no libm, no Emscripten.
//  Build: ./build.sh  (zig c++ -target wasm32-freestanding + base64 embedding)
// ============================================================================

#define EXPORT(name) extern "C" __attribute__((export_name(name), used))

typedef unsigned int  u32;
typedef unsigned char u8;

extern "C" void* memset(void* d, int v, __SIZE_TYPE__ n){
    u8* p = (u8*)d; while(n--) *p++ = (u8)v; return d;
}
extern "C" void* memcpy(void* d, const void* s, __SIZE_TYPE__ n){
    u8* p = (u8*)d; const u8* q = (const u8*)s; while(n--) *p++ = *q++; return d;
}

// ------------------------------------------------- mini math (no libm)
static const float PI  = 3.14159265358979f;
static const float TAU = 6.28318530717959f;

static inline float f_sqrt (float x){ return __builtin_sqrtf(x);  }
static inline float f_floor(float x){ return __builtin_floorf(x); }
static inline float f_max  (float a, float b){ return a > b ? a : b; }
static inline float f_min  (float a, float b){ return a < b ? a : b; }
static inline float clampf (float v, float a, float b){ return f_min(f_max(v, a), b); }

static float f_sin(float x){
    x = x - f_floor((x + PI) * (1.0f/TAU)) * TAU;
    if(x >  PI*0.5f) x =  PI - x;
    if(x < -PI*0.5f) x = -PI - x;
    float x2 = x*x;
    return x * (1.0f + x2*(-1.0f/6 + x2*(1.0f/120 + x2*(-1.0f/5040))));
}
static inline float f_cos(float x){ return f_sin(x + PI*0.5f); }

static inline u32 wang(u32 h){               // integer hash (for stars)
    h = (h ^ 61u) ^ (h >> 16); h *= 9u; h ^= h >> 4;
    h *= 0x27d4eb2du; h ^= h >> 15; return h;
}

// ------------------------------------------------------------ vector math
struct Vec3 { float x, y, z; };
static inline Vec3  operator+(Vec3 a, Vec3 b){ return {a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline Vec3  operator-(Vec3 a, Vec3 b){ return {a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline Vec3  operator*(Vec3 a, float s){ return {a.x*s, a.y*s, a.z*s}; }
static inline float dot (Vec3 a, Vec3 b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline Vec3  cross(Vec3 a, Vec3 b){
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
static inline float len (Vec3 a){ return f_sqrt(dot(a, a)); }
static inline Vec3  norm(Vec3 a){ float l = len(a); return l > 1e-8f ? a*(1.0f/l) : Vec3{0,0,0}; }
static inline Vec3  lerp3(Vec3 a, Vec3 b, float t){ return a + (b - a)*t; }

// -------------------------------------------------------------- framebuffer
static const int W = 480, H = 480;
static u32 fb[W * H];

static inline u32 packRGBA(Vec3 c){
    int r = (int)(c.x*255.0f + 0.5f); r = r < 0 ? 0 : (r > 255 ? 255 : r);
    int g = (int)(c.y*255.0f + 0.5f); g = g < 0 ? 0 : (g > 255 ? 255 : g);
    int b = (int)(c.z*255.0f + 0.5f); b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return 0xFF000000u | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
}

// ------------------------------------------------------------ scene state
static const Vec3 camPos = {0.0f, 0.0f, 0.35f};   // CAMERA FIXED — no movement
static float camYaw = PI, camPitch = 0.0f;        // initially facing the black hole (-z)
static int   animate = 1;
static float animT   = 1.2f;
static int   quality = 2;

// ---- black hole: local scale so rs = 1 unit ----
static const float RS_W    = 4.0f;                  // Schwarzschild radius (world units) — 2x
// The black hole doesn't stand still: like a binary system, it slowly orbits
// in a tilted plane (camera is in the frame of the equal-mass companion).
static Vec3 bhPos = {0.0f, 0.0f, -40.0f};           // current position (updateScene updates this)
static const Vec3  BH_CTR  = {0.0f, 0.0f, -40.0f};  // orbit center
static const Vec3  ORB_E1  = {1.0f, 0.0f, 0.0f};    // orbit plane axes
static const Vec3  ORB_E2  = {0.0f, 1.0f, 0.0f};    // fully vertical (symmetric circle facing camera)
static const float ORB_R   = 8.0f;                 // orbit radius
static const float ORB_W   = 0.15f;                 // angular velocity (full turn ≈ 42 s)
static const float DISK_IN = 3.0f;                  // ISCO: last stable orbit = 3·rs
static const float DISK_OUT= 7.0f;
static const Vec3  DISK_N  = {0.0995f, 0.9950f, 0.0f};   // disk normal (no z-tilt: top/bottom appear equal)
static const Vec3  DISK_E1 = {0.9952f, -0.0978f, 0.0f};    // disk inner axis (for texture)

static Vec3 bhVel = {0.0f, 0.0f, 0.0f};            // world units/s (frame correction when launching objects)

static void updateScene(float dt){
    if(animate) animT += dt;
    // black hole orbit: sideways + slow up-down loop
    float a = animT * ORB_W;
    bhPos = BH_CTR + ORB_E1*(ORB_R*f_cos(a)) + ORB_E2*(ORB_R*f_sin(a));
    bhVel = animate ? (ORB_E1*(-f_sin(a)) + ORB_E2*f_cos(a))*(ORB_R*ORB_W) : Vec3{0,0,0};
}

// ===================================== LAUNCHED OBJECTS (click & watch) ====
// Clicking the screen launches an object from the camera in the ray direction
// of that pixel. Objects move under massive-particle geodesics in the black
// hole frame (rs=1, c=1), spaghettify from tidal forces, and fade out as they
// cross the horizon.

static const int   MAX_OBJ    = 20;
static const float OBJ_R      = 0.15f;   // base radius (rs units; 0.6 world units)
static const float V0         = 0.28f;   // launch speed (c). Corner click aimed away from BH
                                         // h≈2.2 → peri≈4rs → stable orbit;
                                         // click near center h<2 → plunge.
                                         // E<0 (bound), apoapsis ≈ 33rs → returns
static const float SPAWN_DIST = 2.0f;    // how far in front of camera it spawns (world units)
static const float TIME_SCALE = 12.0f;   // physics time (rs/c) / wall-clock second
                                         // (orbit period ~40s, central plunge ~2s)
static const float KILL_R     = 1.03f;   // object removed at this radius (rs)
static const float K_DOP      = 2.0f;    // Doppler color gain

struct Obj {
    Vec3  posR, velR;          // BH-centered position (rs) and velocity (c) — bhPos moves!
    Vec3  col;                 // base color
    float age;
    int   alive;
    float r, stretch, bound;   // per-frame cache (updateObjects fills this)
    Vec3  axis;                // unit long axis pointing toward BH
};
static Obj objs[MAX_OBJ];
static int nAlive = 0;
static u32 spawnCounter = 0;

static const Vec3 PALETTE[8] = {
    {1.00f,0.85f,0.20f},{0.30f,1.00f,0.50f},{1.00f,0.45f,0.85f},{0.40f,0.80f,1.00f},
    {1.00f,0.55f,0.15f},{0.75f,0.55f,1.00f},{0.55f,1.00f,0.95f},{1.00f,0.95f,0.80f},
};

// Schwarzschild massive-particle acceleration (rs=1, c=1):
//   a = -(1/(2r²) + (3/2)·h²/r⁴)·r̂ ,  h = |p×v|
// ISCO=3rs, photon sphere behavior, and capture at h<2 emerge naturally.
static Vec3 objAccel(Vec3 p, Vec3 v){
    float r2 = dot(p, p), r = f_sqrt(r2);
    Vec3 hv = cross(p, v);
    return p * (-(0.5f/r2 + 1.5f*dot(hv, hv)/(r2*r2)) / r);
}

static void updateObjects(float dt){
    nAlive = 0;
    float T = dt * TIME_SCALE;
    for(int i = 0; i < MAX_OBJ; i++){
        Obj& o = objs[i];
        if(!o.alive) continue;
        o.age += dt;
        float t = 0.0f;                               // velocity-Verlet, adaptive sub-step
        for(int s = 0; s < 48 && t < T; s++){
            float r = len(o.posR);
            if(r < KILL_R || r > 60.0f){ o.alive = 0; break; }
            float h = f_min(0.25f, 0.06f*(r - 1.0f));
            if(h > T - t) h = T - t;
            Vec3 a0 = objAccel(o.posR, o.velR);
            o.posR = o.posR + o.velR*h + a0*(0.5f*h*h);
            Vec3 a1 = objAccel(o.posR, o.velR);
            o.velR = o.velR + (a0 + a1)*(0.5f*h);
            // Tidal drag: objects passing near the shadow (r<3.2rs) lose energy via
            // tidal heating → no "very close flyby escape"; grazing objects spiral
            // down a bit each pass, captured in 2-4 passes. Orbits with peri≥4rs
            // never enter this region and remain stable.
            if(r < 3.2f){
                float drag = (3.2f - r)*0.11f;
                o.velR = o.velR*(1.0f - f_min(drag*h, 0.5f));
            }
            t += h;
        }
        if(!o.alive) continue;
        float r = len(o.posR);
        if(r < KILL_R || r > 60.0f){ o.alive = 0; continue; }
        o.r    = r;
        o.axis = norm(o.posR)*-1.0f;
        // tidal stretching: sphere at r≥5rs, ~5x spaghetti at r≈1.3rs
        float k   = clampf((5.0f - r)/3.7f, 0.0f, 1.0f);
        o.stretch = 1.0f + 4.0f*k*k;
        o.bound   = OBJ_R*o.stretch + 0.02f;
        nAlive++;
    }
}

// Ray/segment – ellipsoid intersection (BH-centered rs space, d must be unit).
// Long axis points toward BH; semi-axes (R·s, R/√s, R/√s) → volume ~conserved.
static bool hitObj(const Obj& o, Vec3 p0, Vec3 d, float tmax, float& t, Vec3& n){
    Vec3 u  = o.axis;
    Vec3 w1 = norm(cross(u, (u.x*u.x < 0.9f) ? Vec3{1,0,0} : Vec3{0,1,0}));
    Vec3 w2 = cross(u, w1);
    float sL = OBJ_R*o.stretch, sT = OBJ_R/f_sqrt(o.stretch);
    Vec3 rel = p0 - o.posR;
    Vec3 lo = { dot(rel,u)/sL, dot(rel,w1)/sT, dot(rel,w2)/sT };
    Vec3 ld = { dot(d,  u)/sL, dot(d,  w1)/sT, dot(d,  w2)/sT };
    float A = dot(ld, ld), B = dot(lo, ld), C = dot(lo, lo) - 1.0f;
    float disc = B*B - A*C;
    if(disc < 0.0f || A < 1e-12f) return false;
    float q  = f_sqrt(disc);
    float t0 = (-B - q)/A, t1 = (-B + q)/A;
    t = t0 > 1e-4f ? t0 : t1;
    if(t < 1e-4f || t > tmax) return false;
    Vec3 lp = lo + ld*t;                              // point on unit sphere
    n = norm(u*(lp.x/sL) + w1*(lp.y/sT) + w2*(lp.z/sT));
    return true;
}

// Object color: base color + three-layer effect
//  1) Doppler (falling toward BH → blue, receding → red)
//  2) attenuation by gravitational redshift √(1−1/r)
//  3) full fade as horizon approaches (~0 at KILL_R, no sudden pop)
static Vec3 objColor(const Obj& o, Vec3 n, Vec3 d){
    float vr = dot(o.velR, norm(o.posR));             // >0: receding from BH
    Vec3 c = o.col;
    c = lerp3(c, {0.45f,0.65f,1.00f}, clampf(-vr*K_DOP, 0.0f, 0.7f));
    c = lerp3(c, {1.00f,0.40f,0.25f}, clampf( vr*K_DOP, 0.0f, 0.7f));
    float gz   = f_sqrt(f_max(0.0f, 1.0f - 1.0f/o.r));
    float fade = clampf((o.r - KILL_R)/(1.45f - KILL_R), 0.0f, 1.0f);
    float lum  = 0.35f + 0.65f*f_max(0.0f, -dot(n, d));
    return c * (lum * gz * fade);
}

// Per-ray candidate pre-filter: objects far from the straight line are culled
// so the 320-step geodesic walk doesn't test every object per step. margin
// accounts for rays with small impact parameter (heavily bent); [t0,t1] window
// ensures intersection tests only along path segments near each object. Returns candidate count.
struct Cand { int i; float t0, t1; };

static int objCandidates(Vec3 p, Vec3 d, float b, Cand* out){
    int nc = 0;
    float bend = 2.0f/f_max(b, 0.7f);                 // total Einstein deflection (rad)
    for(int i = 0; i < MAX_OBJ; i++){
        if(!objs[i].alive) continue;
        Vec3 rel = objs[i].posR - p;
        float tca = dot(rel, d);
        if(tca < -(objs[i].bound + 0.5f)) continue;   // fully behind
        float m2 = dot(rel, rel) - tca*tca;
        // CRITICAL: ray curves toward BH after closest approach; straight-line
        // deviation ≈ bend·path. Without scaling the margin, an object in flight
        // gets culled as "too far from line" and suddenly disappears on screen.
        float lim = objs[i].bound + 0.5f + bend*f_max(tca, 0.0f);
        if(m2 < lim*lim){
            float slack = 2.0f + 0.6f*bend*f_max(tca, 0.0f);
            out[nc++] = { i, tca - objs[i].bound - slack, tca + objs[i].bound + slack };
        }
    }
    return nc;
}

// ====================================================== STARS ====
// Direction vector split into cells; sparse stars generated per cell by hash.
// Lensed rays sample with bent direction, so stars arc around the black hole.

static Vec3 stars(Vec3 d){
    Vec3 c = {0, 0, 0};
    float scl = 24.0f;
    for(int layer = 0; layer < 2; layer++, scl *= 2.3f){
        float gx = d.x*scl, gy = d.y*scl, gz = d.z*scl;
        int ix = (int)f_floor(gx), iy = (int)f_floor(gy), iz = (int)f_floor(gz);
        u32 h = wang((u32)ix*73856093u ^ (u32)iy*19349663u ^ (u32)iz*83492791u ^ (u32)layer*2654435769u);
        if((h & 7u) == 0u){                          // star in 1 of 8 cells
            float sx = ((h >>  8) & 255u) / 255.0f;  // random position within cell
            float sy = ((h >> 16) & 255u) / 255.0f;
            float sz = ((h >> 24) & 255u) / 255.0f;
            float fx = gx - ix - sx, fy = gy - iy - sy, fz = gz - iz - sz;
            float d2 = fx*fx + fy*fy + fz*fz;
            float br = 1.35f*(0.35f + 0.65f*((h >> 5) & 7u) / 7.0f) * f_max(0.0f, 1.0f - d2*55.0f);
            float t  = ((h >> 11) & 3u) / 3.0f;      // slight color temperature variety
            c = c + Vec3{br*(0.75f + 0.25f*t), br*0.85f, br*(1.0f - 0.25f*t)};
        }
    }
    return c;
}

// ====================================================== ACCRETION DISK ====
static Vec3 diskColor(Vec3 p, Vec3 d, float rd){     // p: intersection (rs units), d: photon direction
    // Kepler orbital velocity (c=1, GM=rs/2): v = sqrt(1/(2r))
    float v   = f_sqrt(0.5f/rd);
    Vec3 tang = norm(cross(DISK_N, p));              // orbital direction (tangent)
    float gam = 1.0f / f_sqrt(1.0f - v*v);
    // Doppler factor toward observer: photon travels in -d direction toward camera
    float dop = 1.0f / (gam * (1.0f + v*dot(tang, d)));
    float gz  = f_sqrt(1.0f - 1.0f/rd);              // gravitational redshift
    // Shakura–Sunyaev brightness profile: (r_in/r)^2.5
    float xr  = DISK_IN/rd;
    float prof = xr*xr*f_sqrt(xr);
    // inward-drifting ring texture (differential rotation feel)
    float wave = 0.82f + 0.13f*f_sin(rd*6.0f - animT*1.6f) + 0.05f*f_sin(rd*19.0f + animT*0.7f);
    // edge softening
    float edge = f_min(1.0f, (rd - DISK_IN)*2.2f) * f_min(1.0f, (DISK_OUT - rd)*0.9f);
    float I = 4.2f * prof * dop*dop*dop * gz*gz*gz * wave * edge;
    // temperature gradient: inner incandescent white → outer orange-red
    Vec3 col = lerp3({1.00f, 0.97f, 0.92f}, {1.00f, 0.42f, 0.10f},
                      clampf((rd - DISK_IN)/(DISK_OUT - DISK_IN), 0.0f, 1.0f));
    // blueshift: approaching side slightly white-blue
    col = lerp3(col, {0.75f, 0.88f, 1.0f}, clampf((dop - 1.0f)*0.9f, 0.0f, 0.55f));
    return col * I;
}

// ================================================== BLACK HOLE TRACING ====
static Vec3 traceBH(Vec3 oW, Vec3 dW){
    Vec3 p = (oW - bhPos) * (1.0f/RS_W);            // convert to rs = 1 units
    Vec3 d = dW;
    float b = len(cross(p, d));                      // impact parameter (ray's closest approach)

    Cand cand[MAX_OBJ]; int nc = 0;                  // launched object candidates
    if(nAlive) nc = objCandidates(p, d, b, cand);

    if(b > 9.0f && nc == 0){                         // WEAK FIELD: analytic deflection
        float tca = -dot(p, d);                      // (skipped if candidate objects present:
        if(tca > 0.0f){                              //  walk catches them on the bent path)
            Vec3 m = p + d*tca;                      // closest approach point (|m| = b)
            d = norm(d - m*(2.0f/(b*b)));            // Einstein deflection: α = 2·rs/b
        }
        return stars(d);                             // b > disk radius → can't hit disk
    }

    float h2 = b*b;                                  // conserved angular momentum squared
    float s  = 0.0f;                                 // path traveled (for window clipping)
    for(int i = 0; i < 320; i++){
        float r2 = dot(p, p), r = f_sqrt(r2);
        if(r < 1.0f) return {0, 0, 0};               // EVENT HORIZON: light cannot escape
        if(r > 60.0f && dot(p, d) > 0.0f){
            Vec3 du = norm(d);
            if(nc){                                  // objects along the escape path
                float bt = 1e30f, t; Vec3 nn, bn; int bi = -1;
                for(int k = 0; k < nc; k++)
                    if(hitObj(objs[cand[k].i], p, du, bt, t, nn)){ bt = t; bn = nn; bi = cand[k].i; }
                if(bi >= 0) return objColor(objs[bi], bn, du);
            }
            return stars(du);
        }

        float h  = clampf(0.14f*(r - 0.9f), 0.02f, 2.0f);   // adaptive step
        // Schwarzschild null geodesic: a = -(3/2)·h²·p/r⁵
        Vec3 a  = p * (-1.5f*h2 / (r2*r2*r));
        Vec3 pn = p + d*h + a*(0.5f*h*h);
        Vec3 seg = pn - p;

        // object hit in this step segment? (nearest, as fraction)
        float fObj = 2.0f; int oi = -1; Vec3 onrm = {0,0,0}, du = {0,0,0};
        if(nc){
            float L = len(seg);
            if(L > 1e-6f){
                du = seg*(1.0f/L);
                float t; Vec3 nn;
                for(int k = 0; k < nc; k++){
                    const Cand& ck = cand[k];
                    if(s + L < ck.t0 || s > ck.t1) continue;   // outside window
                    const Obj& o = objs[ck.i];
                    Vec3 rel = o.posR - p;            // fast reject: too far from segment?
                    float tc = clampf(dot(rel, du), 0.0f, L);
                    Vec3 mm = rel - du*tc;
                    if(dot(mm, mm) > o.bound*o.bound) continue;
                    if(hitObj(o, p, du, L, t, nn) && t/L < fObj){ fObj = t/L; oi = ck.i; onrm = nn; }
                }
            }
            s += L;
        }

        // crossing thin disk plane?
        float fDisk = 2.0f; Vec3 dhit = {0,0,0}; float rdd = 0.0f;
        float s0 = dot(p, DISK_N), s1 = dot(pn, DISK_N);
        if(s0*s1 < 0.0f){
            float f = s0/(s0 - s1);
            Vec3 hit = p + seg*f;
            float rd = len(hit);
            if(rd > DISK_IN && rd < DISK_OUT){ fDisk = f; dhit = hit; rdd = rd; }
        }
        if(oi >= 0 && fObj < fDisk) return objColor(objs[oi], onrm, du);
        if(fDisk <= 1.0f) return diskColor(dhit, norm(d), rdd);

        d = d + a*h;
        p = pn;
    }
    return {0, 0, 0};                                // stuck orbiting the photon sphere
}

static const float FOV = 0.70020754f;                // tan(70°/2)

static void camBasis(Vec3& fwd, Vec3& rgt, Vec3& up){
    fwd = { f_sin(camYaw)*f_cos(camPitch), f_sin(camPitch), f_cos(camYaw)*f_cos(camPitch) };
    rgt = { f_cos(camYaw), 0.0f, -f_sin(camYaw) };
    up  = cross(fwd, rgt);
}

static void render(){
    Vec3 fwd, rgt, up; camBasis(fwd, rgt, up);
    const float fov = FOV;

    int q = quality;
    for(int y = 0; y < H; y += q){
        for(int x = 0; x < W; x += q){
            float u = (2.0f*(x + 0.5f)/W - 1.0f) * fov;
            float v = (1.0f - 2.0f*(y + 0.5f)/H) * fov;
            u32 c = packRGBA(traceBH(camPos, norm(fwd + rgt*u + up*v)));
            for(int j = 0; j < q && y+j < H; j++)
                for(int i = 0; i < q && x+i < W; i++)
                    fb[(y+j)*W + (x+i)] = c;
        }
    }
}

// =================================================== EXPORTED JS INTERFACE ====
// keys bitmask: 64=look left 128=look right 256=look up 512=look down

EXPORT("fb_ptr")  u32* fb_ptr(){ return fb; }
EXPORT("fb_w")    int  fb_w(){ return W; }
EXPORT("fb_h")    int  fb_h(){ return H; }

EXPORT("set_quality") void set_quality(int q){ quality = q < 1 ? 1 : (q > 4 ? 4 : q); }
EXPORT("get_quality") int  get_quality(){ return quality; }
EXPORT("toggle_anim") int  toggle_anim(){ animate = !animate; return animate; }

EXPORT("drag") void drag(float dx, float dy){        // mouse/touch: look around
    camYaw   += dx*0.004f;
    camPitch  = clampf(camPitch - dy*0.004f, -1.4f, 1.4f);
}

EXPORT("spawn") void spawn(float px, float py){      // launch object toward clicked pixel
    Vec3 fwd, rgt, up; camBasis(fwd, rgt, up);
    float u = (2.0f*(px + 0.5f)/W - 1.0f)*FOV;
    float v = (1.0f - 2.0f*(py + 0.5f)/H)*FOV;
    Vec3 dir = norm(fwd + rgt*u + up*v);

    int s = 0; float oldest = -1.0f;                 // empty slot, or recycle oldest
    for(int i = 0; i < MAX_OBJ; i++){
        if(!objs[i].alive){ s = i; break; }
        if(objs[i].age > oldest){ oldest = objs[i].age; s = i; }
    }
    Obj& o = objs[s];
    o.posR = (camPos + dir*SPAWN_DIST - bhPos)*(1.0f/RS_W);
    o.velR = dir*V0 - bhVel*(1.0f/(RS_W*TIME_SCALE));   // convert to BH frame
    o.col  = PALETTE[spawnCounter++ & 7u];
    o.age  = 0.0f; o.alive = 1;
    o.r = len(o.posR); o.axis = norm(o.posR)*-1.0f;     // cache for first frame
    o.stretch = 1.0f;  o.bound = OBJ_R + 0.02f;
}

EXPORT("obj_alive") int obj_alive(){                 // alive object count (testing/diagnostics)
    int n = 0;
    for(int i = 0; i < MAX_OBJ; i++) n += objs[i].alive ? 1 : 0;
    return n;
}

EXPORT("obj_r") float obj_r(int i){                  // i-th object BH distance, rs (testing/diagnostics)
    return (i >= 0 && i < MAX_OBJ && objs[i].alive) ? objs[i].r : -1.0f;
}

EXPORT("frame") void frame(float dt, int keys){
    dt = clampf(dt, 0.0f, 0.05f);
    if(keys &  64) camYaw   -= 1.7f*dt;
    if(keys & 128) camYaw   += 1.7f*dt;
    if(keys & 256) camPitch += 1.2f*dt;
    if(keys & 512) camPitch -= 1.2f*dt;
    camPitch = clampf(camPitch, -1.4f, 1.4f);
    updateScene(dt);
    updateObjects(dt);                               // objects fly even when animation is off
    render();
}
