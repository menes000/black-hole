// ============================================================================
//  oda_wasm.cpp — Piksel piksel 3D motor: DAMALI ODA + 3 YANSITICI KÜRE
//
//  render_wasm.cpp'den ayrıldı: burada kara delik YOK. Kapalı damalı oda,
//  içinde salınan üç yansıtıcı küre, Blinn-Phong ışık + gölge + özyinelemeli
//  yansıma (derinlik 2). Kamera sabittir; sürükleyerek etrafına bakılır.
//
//  SIFIR bağımlılık: SDL yok, stdlib yok, libm yok, Emscripten yok.
//  Derleme: ./build.sh  (zig c++ -target wasm32-freestanding + base64 gömme)
// ============================================================================

#define EXPORT(isim) extern "C" __attribute__((export_name(isim), used))

typedef unsigned int  u32;
typedef unsigned char u8;

extern "C" void* memset(void* d, int v, __SIZE_TYPE__ n){
    u8* p = (u8*)d; while(n--) *p++ = (u8)v; return d;
}
extern "C" void* memcpy(void* d, const void* s, __SIZE_TYPE__ n){
    u8* p = (u8*)d; const u8* q = (const u8*)s; while(n--) *p++ = *q++; return d;
}

// ------------------------------------------------- mini matematik (libm yok)
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

static inline float pow48(float x){
    float x2 = x*x, x4 = x2*x2, x8 = x4*x4, x16 = x8*x8, x32 = x16*x16;
    return x32 * x16;
}

// ------------------------------------------------------------ vektör matematiği
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
static inline Vec3  reflect(Vec3 v, Vec3 n){ return v - n*(2.0f*dot(v, n)); }

// -------------------------------------------------------------- framebuffer
static const int W = 480, H = 480;
static u32 fb[W * H];

static inline u32 packRGBA(Vec3 c){
    int r = (int)(c.x*255.0f + 0.5f); r = r < 0 ? 0 : (r > 255 ? 255 : r);
    int g = (int)(c.y*255.0f + 0.5f); g = g < 0 ? 0 : (g > 255 ? 255 : g);
    int b = (int)(c.z*255.0f + 0.5f); b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return 0xFF000000u | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
}

// ------------------------------------------------------------ sahne durumu
static const Vec3 camPos = {0.0f, 0.0f, 0.35f};   // KAMERA SABİT — hareket yok
static float camYaw = 0.0f, camPitch = 0.0f;
static int   animate = 1;
static float animT   = 1.2f;
static int   quality = 2;

struct Sphere { Vec3 c; float r; Vec3 col; float refl; };
static Sphere sph[3];

static const Vec3 roomMin  = {-5.0f, -2.5f, -1.0f};
static const Vec3 roomMax  = { 5.0f,  2.5f, 12.0f};
static const Vec3 lightPos = { 0.0f,  1.9f,  3.0f};

static void updateScene(float dt){
    if(animate) animT += dt;
    sph[0] = {{ 0.5f, 0.25f + 0.25f*f_sin(animT*0.9f),         6.8f}, 1.75f, {0.10f, 0.80f, 0.15f}, 0.25f};
    sph[1] = {{-1.9f + 0.2f*f_sin(animT*0.7f),
               -0.70f + 0.30f*f_sin(animT*1.3f + 2.0f),        4.9f}, 0.95f, {0.85f, 0.05f, 0.05f}, 0.30f};
    sph[2] = {{ 2.4f, -0.15f + 0.30f*f_sin(animT*1.1f + 4.0f), 6.9f}, 1.45f, {0.05f, 0.10f, 0.85f}, 0.30f};
}

// ============================================================ RAYTRACER ====
static bool hitSphere(const Sphere& s, Vec3 o, Vec3 d, float& t){
    Vec3 oc = o - s.c;
    float b = dot(oc, d), c = dot(oc, oc) - s.r*s.r;
    float disc = b*b - c;
    if(disc < 0) return false;
    float q  = f_sqrt(disc);
    float t0 = -b - q, t1 = -b + q;
    t = t0 > 1e-3f ? t0 : t1;
    return t > 1e-3f;
}

static bool hitRoom(Vec3 o, Vec3 d, float& t, int& axis, int& side){
    const float mn[3] = {roomMin.x, roomMin.y, roomMin.z};
    const float mx[3] = {roomMax.x, roomMax.y, roomMax.z};
    const float oo[3] = {o.x, o.y, o.z};
    const float dd[3] = {d.x, d.y, d.z};
    t = 1e30f; axis = -1; side = 0;
    for(int a = 0; a < 3; a++){
        if(dd[a] >  1e-6f){ float tt = (mx[a]-oo[a])/dd[a]; if(tt > 1e-3f && tt < t){ t = tt; axis = a; side = +1; } }
        if(dd[a] < -1e-6f){ float tt = (mn[a]-oo[a])/dd[a]; if(tt > 1e-3f && tt < t){ t = tt; axis = a; side = -1; } }
    }
    return axis >= 0;
}

static inline bool checker(float a, float b){
    return (((int)f_floor(a) + (int)f_floor(b)) & 1) == 0;
}

static Vec3 wallColor(Vec3 p, int axis, int side){
    if(axis == 1)
        return checker(p.x*0.9f, p.z*0.9f) ? Vec3{0.92f,0.92f,0.97f} : Vec3{0.06f,0.15f,0.85f};
    if(axis == 0){
        Vec3 tint = side < 0 ? Vec3{0.95f,0.15f,0.95f} : Vec3{0.10f,0.85f,0.80f};
        return checker(p.y*0.9f, p.z*0.9f) ? tint : tint*0.45f;
    }
    // z duvarları: oda kapalı — arka koyu, ön (kameranın arkası) hafif damalı gri
    if(side > 0) return {0.02f, 0.02f, 0.02f};
    return checker(p.x*0.9f, p.y*0.9f) ? Vec3{0.30f,0.30f,0.34f} : Vec3{0.12f,0.12f,0.15f};
}

static Vec3 shade(Vec3 o, Vec3 d, int depth){
    float tS = 1e30f; int id = -1;
    for(int i = 0; i < 3; i++){ float t; if(hitSphere(sph[i], o, d, t) && t < tS){ tS = t; id = i; } }
    float tR; int axis, side;
    bool room = hitRoom(o, d, tR, axis, side);

    Vec3 p, n, alb; float refl = 0.0f, ks;
    if(id >= 0 && (!room || tS < tR)){
        p = o + d*tS; n = norm(p - sph[id].c);
        alb = sph[id].col; refl = sph[id].refl; ks = 0.50f;
    } else if(room){
        p = o + d*tR;
        n = {0,0,0};
        if(axis == 0) n.x = (float)-side; else if(axis == 1) n.y = (float)-side; else n.z = (float)-side;
        alb = wallColor(p, axis, side); ks = 0.06f;
    } else return {0, 0, 0};

    Vec3 L = lightPos - p; float dl = len(L); L = L*(1.0f/dl);
    float diff = f_max(0.0f, dot(n, L));

    float shadow = 1.0f;
    for(int i = 0; i < 3; i++){
        if(i == id) continue;
        float t; if(hitSphere(sph[i], p + n*1e-3f, L, t) && t < dl){ shadow = 0.35f; break; }
    }

    float atten = 1.0f / (1.0f + 0.012f*dl*dl);
    float lum   = 0.27f + 0.95f*diff*shadow*atten;
    Vec3 col = {alb.x*lum, alb.y*lum, alb.z*lum};

    if(diff > 0.0f && shadow > 0.9f){
        Vec3 hv = norm(L - d);
        float sp = ks * pow48(f_max(0.0f, dot(n, hv))) * atten;
        col = col + Vec3{sp, sp, sp};
    }
    if(refl > 0.0f && depth < 2)
        col = col*(1.0f - refl) + shade(p + n*1e-3f, norm(reflect(d, n)), depth + 1)*refl;
    return col;
}

static void render(){
    Vec3 fwd = { f_sin(camYaw)*f_cos(camPitch), f_sin(camPitch), f_cos(camYaw)*f_cos(camPitch) };
    Vec3 rgt = { f_cos(camYaw), 0.0f, -f_sin(camYaw) };
    Vec3 up  = cross(fwd, rgt);
    const float fov = 0.70020754f;                   // tan(70°/2)

    int q = quality;
    for(int y = 0; y < H; y += q){
        for(int x = 0; x < W; x += q){
            float u = (2.0f*(x + 0.5f)/W - 1.0f) * fov;
            float v = (1.0f - 2.0f*(y + 0.5f)/H) * fov;
            u32 c = packRGBA(shade(camPos, norm(fwd + rgt*u + up*v), 0));
            for(int j = 0; j < q && y+j < H; j++)
                for(int i = 0; i < q && x+i < W; i++)
                    fb[(y+j)*W + (x+i)] = c;
        }
    }
}

// =================================================== JS'E AÇILAN ARAYÜZ ====
// keys bit maskesi: 64=sol bak 128=sağ bak 256=yukarı bak 512=aşağı bak

EXPORT("fb_ptr")  u32* fb_ptr(){ return fb; }
EXPORT("fb_w")    int  fb_w(){ return W; }
EXPORT("fb_h")    int  fb_h(){ return H; }

EXPORT("set_quality") void set_quality(int q){ quality = q < 1 ? 1 : (q > 4 ? 4 : q); }
EXPORT("get_quality") int  get_quality(){ return quality; }
EXPORT("toggle_anim") int  toggle_anim(){ animate = !animate; return animate; }

EXPORT("drag") void drag(float dx, float dy){        // fare/parmak: etrafına bak
    camYaw   += dx*0.004f;
    camPitch  = clampf(camPitch - dy*0.004f, -1.4f, 1.4f);
}

EXPORT("frame") void frame(float dt, int keys){
    dt = clampf(dt, 0.0f, 0.05f);
    if(keys &  64) camYaw   -= 1.7f*dt;
    if(keys & 128) camYaw   += 1.7f*dt;
    if(keys & 256) camPitch += 1.2f*dt;
    if(keys & 512) camPitch -= 1.2f*dt;
    camPitch = clampf(camPitch, -1.4f, 1.4f);
    updateScene(dt);
    render();
}
