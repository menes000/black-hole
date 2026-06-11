// ============================================================================
//  render_wasm.cpp — KARA DELİK + CİSİM FIRLATMA (WebAssembly)
//
//  Kamera uzayda sabittir, başlangıçta kara deliğe bakar; sürükleyerek
//  etrafına bakılır, tıklayınca o yönde cisim fırlatılır. (Damalı oda ve üç
//  yansıtıcı küre ayrı demoya taşındı: oda_wasm.cpp / oda.html.)
//  Gerçek fizikle render edilen bir Schwarzschild kara deliği:
//
//   • Işık ışınları kara deliğin yakınında DÜZ GİTMEZ: genel göreliliğin
//     null jeodezik denklemi adım adım entegre edilir:
//         a = -(3/2)·h²·r̂/r⁴      (h = korunan açısal momentum, rs = 1 birim)
//     Bu denklem d²u/dφ² + u = (3/2)·rs·u² Schwarzschild yörüngesinin
//     birebir vektör hâlidir — foton küresi (r = 1.5·rs) ve kritik vurma
//     parametresi b = (3√3/2)·rs kendiliğinden ortaya çıkar.
//   • Olay ufku (r < rs): ışık kaçamaz → mutlak siyah.
//   • Yığılma diski: iç kenarı ISCO'da (r = 3·rs). Parlaklık profili
//     Shakura–Sunyaev (~r^-2.5), Kepler hızı v = √(rs/2r) ile:
//       - Doppler ışıması  δ³  (yaklaşan taraf parlak/mavimsi)
//       - kütleçekimsel kızıla kayma √(1 - rs/r)
//   • Uzak ışınlar (b > 9·rs) için birinci dereceden kesin sapma α = 2·rs/b
//     analitik uygulanır (hız için); yakın ışınlar tam entegre edilir.
//   • Arka plan yıldızları merceklenmeyle yay biçiminde bükülür.
//
//  Yaklaştırma: disk geometrik olarak ince ve opaktır.
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

static inline u32 wang(u32 h){               // tamsayı karma (yıldızlar için)
    h = (h ^ 61u) ^ (h >> 16); h *= 9u; h ^= h >> 4;
    h *= 0x27d4eb2du; h ^= h >> 15; return h;
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

// ------------------------------------------------------------ sahne durumu
static const Vec3 camPos = {0.0f, 0.0f, 0.35f};   // KAMERA SABİT — hareket yok
static float camYaw = PI, camPitch = 0.0f;        // başlangıçta kara deliğe bak (-z)
static int   animate = 1;
static float animT   = 1.2f;
static int   quality = 2;

// ---- kara delik: rs = 1 birim olacak şekilde yerel ölçek ----
static const float RS_W    = 4.0f;                  // Schwarzschild yarıçapı (dünya birimi) — 2x
// Kara delik sabit durmaz: ikili sistemdeki gibi, eğik bir düzlemde yavaş
// dairesel yörüngede dolanır (kamera, eş kütlenin çerçevesindedir).
static Vec3 bhPos = {0.0f, 0.0f, -40.0f};           // anlık konum (updateScene günceller)
static const Vec3  BH_CTR  = {0.0f, 0.0f, -40.0f};  // yörünge merkezi
static const Vec3  ORB_E1  = {1.0f, 0.0f, 0.0f};    // yörünge düzlemi eksenleri
static const Vec3  ORB_E2  = {0.0f, 1.0f, 0.0f};    // tam dikey (kameraya bakan simetrik çember)
static const float ORB_R   = 8.0f;                 // yörünge yarıçapı
static const float ORB_W   = 0.15f;                 // açısal hız (tam tur ≈ 42 sn)
static const float DISK_IN = 3.0f;                  // ISCO: kararlı son yörünge = 3·rs
static const float DISK_OUT= 7.0f;
static const Vec3  DISK_N  = {0.0995f, 0.9950f, 0.0f};   // disk normali (z-eğimi yok: alt/üst eşit görünür)
static const Vec3  DISK_E1 = {0.9952f, -0.0978f, 0.0f};    // disk içi eksen (doku için)

static Vec3 bhVel = {0.0f, 0.0f, 0.0f};            // dünya birimi/sn (cisim fırlatmada çerçeve düzeltmesi)

static void updateScene(float dt){
    if(animate) animT += dt;
    // kara delik yörüngesi: yana + yukarı-aşağı yavaş döngü
    float a = animT * ORB_W;
    bhPos = BH_CTR + ORB_E1*(ORB_R*f_cos(a)) + ORB_E2*(ORB_R*f_sin(a));
    bhVel = animate ? (ORB_E1*(-f_sin(a)) + ORB_E2*f_cos(a))*(ORB_R*ORB_W) : Vec3{0,0,0};
}

// ===================================== FIRLATILAN CİSİMLER (tıkla & izle) ====
// Ekrana tıklayınca o pikselin ışını yönünde kameradan cisim fırlatılır.
// Cisimler kara delik çerçevesinde (rs=1, c=1) kütleli parçacık jeodeziğiyle
// hareket eder, gelgit kuvvetiyle spagettileşir, ufka düşünce sönüp yok olur.

static const int   MAX_OBJ    = 20;
static const float OBJ_R      = 0.09f;   // taban yarıçap (rs birimi; 0.36 dünya b.)
static const float V0         = 0.28f;   // fırlatma hızı (c); köşe tıklamada
                                         // h = r·v_perp ≈ 10·0.28·sin(~60°) ≈ 2.4 > 2 → yörünge
                                         // merkez tıklamada v_perp≈0, h<2 → dalış
static const float SPAWN_DIST = 2.0f;    // kameranın ne kadar önünden çıkar (dünya b.)
static const float TIME_SCALE = 8.0f;    // fizik zamanı (rs/c) / duvar saati saniyesi
static const float KILL_R     = 1.03f;   // bu yarıçapta cisim silinir (rs)
static const float K_DOP      = 2.0f;    // Doppler ton kazancı

struct Obj {
    Vec3  posR, velR;          // KD-merkezli konum (rs) ve hız (c) — bhPos hareketli!
    Vec3  col;                 // taban renk
    float age;
    int   alive;
    float r, stretch, bound;   // kare başı önbellek (updateObjects doldurur)
    Vec3  axis;                // KD'ye bakan birim uzun eksen
};
static Obj objs[MAX_OBJ];
static int nAlive = 0;
static u32 spawnCounter = 0;

static const Vec3 PALETTE[8] = {
    {1.00f,0.85f,0.20f},{0.30f,1.00f,0.50f},{1.00f,0.45f,0.85f},{0.40f,0.80f,1.00f},
    {1.00f,0.55f,0.15f},{0.75f,0.55f,1.00f},{0.55f,1.00f,0.95f},{1.00f,0.95f,0.80f},
};

// Schwarzschild kütleli parçacık ivmesi (rs=1, c=1):
//   a = -(1/(2r²) + (3/2)·h²/r⁴)·r̂ ,  h = |p×v|
// ISCO=3rs, foton küresi davranışı ve h<2'de yakalanma kendiliğinden çıkar.
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
        float t = 0.0f;                               // velocity-Verlet, uyarlanır alt adım
        for(int s = 0; s < 48 && t < T; s++){
            float r = len(o.posR);
            if(r < KILL_R || r > 60.0f){ o.alive = 0; break; }
            float h = f_min(0.25f, 0.06f*(r - 1.0f));
            if(h > T - t) h = T - t;
            Vec3 a0 = objAccel(o.posR, o.velR);
            o.posR = o.posR + o.velR*h + a0*(0.5f*h*h);
            Vec3 a1 = objAccel(o.posR, o.velR);
            o.velR = o.velR + (a0 + a1)*(0.5f*h);
            t += h;
        }
        if(!o.alive) continue;
        float r = len(o.posR);
        if(r < KILL_R || r > 60.0f){ o.alive = 0; continue; }
        o.r    = r;
        o.axis = norm(o.posR)*-1.0f;
        // gelgit uzaması: r≥5rs'te küre, r≈1.3rs'te ~5x spagetti
        float k   = clampf((5.0f - r)/3.7f, 0.0f, 1.0f);
        o.stretch = 1.0f + 4.0f*k*k;
        o.bound   = OBJ_R*o.stretch + 0.02f;
        nAlive++;
    }
}

// Işın/segment – elipsoid kesişimi (KD-merkezli rs uzayı, d birim olmalı).
// Uzun eksen KD'yi gösterir; yarı eksenler (R·s, R/√s, R/√s) → hacim ~korunur.
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
    Vec3 lp = lo + ld*t;                              // birim küre üstündeki nokta
    n = norm(u*(lp.x/sL) + w1*(lp.y/sT) + w2*(lp.z/sT));
    return true;
}

// Cisim rengi: taban renk + üç katmanlı etki
//  1) Doppler (istek gereği: KD'ye doğru → mavi, uzaklaşırken → kırmızı)
//  2) kütleçekimsel kızıla kayma √(1−1/r) ile sönme
//  3) ufka yaklaşırken tam kararma (KILL_R'de ~0 → pat diye yok olmaz)
static Vec3 objColor(const Obj& o, Vec3 n, Vec3 d){
    float vr = dot(o.velR, norm(o.posR));             // >0: KD'den uzaklaşıyor
    Vec3 c = o.col;
    c = lerp3(c, {0.45f,0.65f,1.00f}, clampf(-vr*K_DOP, 0.0f, 0.7f));
    c = lerp3(c, {1.00f,0.40f,0.25f}, clampf( vr*K_DOP, 0.0f, 0.7f));
    float gz   = f_sqrt(f_max(0.0f, 1.0f - 1.0f/o.r));
    float fade = clampf((o.r - KILL_R)/(1.45f - KILL_R), 0.0f, 1.0f);
    float lum  = 0.35f + 0.65f*f_max(0.0f, -dot(n, d));
    return c * (lum * gz * fade);
}

// Işın başına aday ön filtresi: düz çizgiye uzak cisimler elenir ki 320 adımlık
// jeodezik yürüyüşü cisim başına test yapmasın. margin, küçük vurma parametreli
// (çok bükülen) ışınlar için pay bırakır; [t0,t1] penceresi yürüyüş sırasında
// yalnız cisme yakın yol diliminde test yapılmasını sağlar. Dönüş: aday sayısı.
struct Cand { int i; float t0, t1; };

static int objCandidates(Vec3 p, Vec3 d, float b, Cand* out){
    int nc = 0;
    float bend = 2.0f/f_max(b, 0.7f);                 // toplam Einstein sapması (rad)
    for(int i = 0; i < MAX_OBJ; i++){
        if(!objs[i].alive) continue;
        Vec3 rel = objs[i].posR - p;
        float tca = dot(rel, d);
        if(tca < -(objs[i].bound + 0.5f)) continue;   // tamamen geride
        float m2 = dot(rel, rel) - tca*tca;
        // KRİTİK: ışın en yakın geçişten sonra KD'ye doğru kayar; düz çizgiden
        // sapma ≈ bükülme·yol olur. Pay buna göre ölçeklenmezse uçuştaki cisim
        // "çizgiden uzak" diye elenir ve ekranda bir anda görünmez olur.
        float lim = objs[i].bound + 0.5f + bend*f_max(tca, 0.0f);
        if(m2 < lim*lim){
            float slack = 2.0f + 0.6f*bend*f_max(tca, 0.0f);
            out[nc++] = { i, tca - objs[i].bound - slack, tca + objs[i].bound + slack };
        }
    }
    return nc;
}

// ============================================================ YILDIZLAR ====
// Yön vektörü hücrelere bölünür; her hücrede karma ile seyrek yıldız üretilir.
// Merceklenmiş ışınlar bükülmüş yönle örneklediği için yıldızlar kara deliğin
// çevresinde yay biçiminde uzar.

static Vec3 stars(Vec3 d){
    Vec3 c = {0, 0, 0};
    float scl = 24.0f;
    for(int katman = 0; katman < 2; katman++, scl *= 2.3f){
        float gx = d.x*scl, gy = d.y*scl, gz = d.z*scl;
        int ix = (int)f_floor(gx), iy = (int)f_floor(gy), iz = (int)f_floor(gz);
        u32 h = wang((u32)ix*73856093u ^ (u32)iy*19349663u ^ (u32)iz*83492791u ^ (u32)katman*2654435769u);
        if((h & 7u) == 0u){                          // her 8 hücreden birinde yıldız
            float sx = ((h >>  8) & 255u) / 255.0f;  // hücre içi rastgele konum
            float sy = ((h >> 16) & 255u) / 255.0f;
            float sz = ((h >> 24) & 255u) / 255.0f;
            float fx = gx - ix - sx, fy = gy - iy - sy, fz = gz - iz - sz;
            float d2 = fx*fx + fy*fy + fz*fz;
            float br = 1.35f*(0.35f + 0.65f*((h >> 5) & 7u) / 7.0f) * f_max(0.0f, 1.0f - d2*55.0f);
            float t  = ((h >> 11) & 3u) / 3.0f;      // hafif renk sıcaklığı çeşidi
            c = c + Vec3{br*(0.75f + 0.25f*t), br*0.85f, br*(1.0f - 0.25f*t)};
        }
    }
    return c;
}

// ====================================================== YIĞILMA DİSKİ ====
static Vec3 diskColor(Vec3 p, Vec3 d, float rd){     // p: kesişim (rs biriminde), d: foton yönü
    // Kepler dolanım hızı (c=1, GM=rs/2): v = sqrt(1/(2r))
    float v   = f_sqrt(0.5f/rd);
    Vec3 tang = norm(cross(DISK_N, p));              // dolanım yönü (teğet)
    float gam = 1.0f / f_sqrt(1.0f - v*v);
    // Gözlemciye Doppler çarpanı: foton kameraya -d yönünde gider
    float dop = 1.0f / (gam * (1.0f + v*dot(tang, d)));
    float gz  = f_sqrt(1.0f - 1.0f/rd);              // kütleçekimsel kızıla kayma
    // Shakura–Sunyaev tarzı parlaklık profili: (r_in/r)^2.5
    float xr  = DISK_IN/rd;
    float prof = xr*xr*f_sqrt(xr);
    // içe doğru süzülen halka dokusu (diferansiyel dönme hissi)
    float dalga = 0.82f + 0.13f*f_sin(rd*6.0f - animT*1.6f) + 0.05f*f_sin(rd*19.0f + animT*0.7f);
    // kenar yumuşatma
    float kenar = f_min(1.0f, (rd - DISK_IN)*2.2f) * f_min(1.0f, (DISK_OUT - rd)*0.9f);
    float I = 4.2f * prof * dop*dop*dop * gz*gz*gz * dalga * kenar;
    // sıcaklık gradyanı: içte akkor beyaz → dışta turuncu-kızıl
    Vec3 renk = lerp3({1.00f, 0.97f, 0.92f}, {1.00f, 0.42f, 0.10f},
                      clampf((rd - DISK_IN)/(DISK_OUT - DISK_IN), 0.0f, 1.0f));
    // maviye kayma: yaklaşan taraf hafif beyaz-mavi
    renk = lerp3(renk, {0.75f, 0.88f, 1.0f}, clampf((dop - 1.0f)*0.9f, 0.0f, 0.55f));
    return renk * I;
}

// ================================================== KARA DELİK İZLEME ====
static Vec3 traceBH(Vec3 oW, Vec3 dW){
    Vec3 p = (oW - bhPos) * (1.0f/RS_W);            // rs = 1 birimine geç
    Vec3 d = dW;
    float b = len(cross(p, d));                      // vurma parametresi (ışının uzaklığı)

    Cand cand[MAX_OBJ]; int nc = 0;                  // fırlatılan cisim adayları
    if(nAlive) nc = objCandidates(p, d, b, cand);

    if(b > 9.0f && nc == 0){                         // ZAYIF ALAN: analitik sapma
        float tca = -dot(p, d);                      // (aday cisim varsa kısayol atlanır:
        if(tca > 0.0f){                              //  yürüyüş cismi bükülmüş yolda yakalar)
            Vec3 m = p + d*tca;                      // en yakın geçiş noktası (|m| = b)
            d = norm(d - m*(2.0f/(b*b)));            // Einstein sapması: α = 2·rs/b
        }
        return stars(d);                             // b > disk yarıçapı → disk vurulamaz
    }

    float h2 = b*b;                                  // korunan açısal momentum karesi
    float s  = 0.0f;                                 // kat edilen yol (pencere kırpması için)
    for(int i = 0; i < 320; i++){
        float r2 = dot(p, p), r = f_sqrt(r2);
        if(r < 1.0f) return {0, 0, 0};               // OLAY UFKU: ışık kaçamaz
        if(r > 60.0f && dot(p, d) > 0.0f){
            Vec3 du = norm(d);
            if(nc){                                  // kaçarken yan yoldaki cisimler
                float bt = 1e30f, t; Vec3 nn, bn; int bi = -1;
                for(int k = 0; k < nc; k++)
                    if(hitObj(objs[cand[k].i], p, du, bt, t, nn)){ bt = t; bn = nn; bi = cand[k].i; }
                if(bi >= 0) return objColor(objs[bi], bn, du);
            }
            return stars(du);
        }

        float h  = clampf(0.14f*(r - 0.9f), 0.02f, 2.0f);   // uyarlanır adım
        // Schwarzschild null jeodeziği: a = -(3/2)·h²·p/r⁵
        Vec3 a  = p * (-1.5f*h2 / (r2*r2*r));
        Vec3 pn = p + d*h + a*(0.5f*h*h);
        Vec3 seg = pn - p;

        // bu adım segmentinde cisim vuruşu? (kesir olarak en yakını)
        float fObj = 2.0f; int oi = -1; Vec3 onrm = {0,0,0}, du = {0,0,0};
        if(nc){
            float L = len(seg);
            if(L > 1e-6f){
                du = seg*(1.0f/L);
                float t; Vec3 nn;
                for(int k = 0; k < nc; k++){
                    const Cand& ck = cand[k];
                    if(s + L < ck.t0 || s > ck.t1) continue;   // pencere dışı
                    const Obj& o = objs[ck.i];
                    Vec3 rel = o.posR - p;            // hızlı ret: segmente uzak mı?
                    float tc = clampf(dot(rel, du), 0.0f, L);
                    Vec3 mm = rel - du*tc;
                    if(dot(mm, mm) > o.bound*o.bound) continue;
                    if(hitObj(o, p, du, L, t, nn) && t/L < fObj){ fObj = t/L; oi = ck.i; onrm = nn; }
                }
            }
            s += L;
        }

        // ince disk düzleminden geçiş?
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
    return {0, 0, 0};                                // foton küresinde dolanıp kaldı
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

EXPORT("spawn") void spawn(float px, float py){      // tıklanan piksele cisim fırlat
    Vec3 fwd, rgt, up; camBasis(fwd, rgt, up);
    float u = (2.0f*(px + 0.5f)/W - 1.0f)*FOV;
    float v = (1.0f - 2.0f*(py + 0.5f)/H)*FOV;
    Vec3 dir = norm(fwd + rgt*u + up*v);

    int s = 0; float oldest = -1.0f;                 // boş slot, yoksa en yaşlıyı geri dönüştür
    for(int i = 0; i < MAX_OBJ; i++){
        if(!objs[i].alive){ s = i; break; }
        if(objs[i].age > oldest){ oldest = objs[i].age; s = i; }
    }
    Obj& o = objs[s];
    o.posR = (camPos + dir*SPAWN_DIST - bhPos)*(1.0f/RS_W);
    o.velR = dir*V0 - bhVel*(1.0f/(RS_W*TIME_SCALE));   // KD çerçevesine geçiş
    o.col  = PALETTE[spawnCounter++ & 7u];
    o.age  = 0.0f; o.alive = 1;
    o.r = len(o.posR); o.axis = norm(o.posR)*-1.0f;     // ilk kare için önbellek
    o.stretch = 1.0f;  o.bound = OBJ_R + 0.02f;
}

EXPORT("obj_alive") int obj_alive(){                 // canlı cisim sayısı (test/teşhis)
    int n = 0;
    for(int i = 0; i < MAX_OBJ; i++) n += objs[i].alive ? 1 : 0;
    return n;
}

EXPORT("frame") void frame(float dt, int keys){
    dt = clampf(dt, 0.0f, 0.05f);
    if(keys &  64) camYaw   -= 1.7f*dt;
    if(keys & 128) camYaw   += 1.7f*dt;
    if(keys & 256) camPitch += 1.2f*dt;
    if(keys & 512) camPitch -= 1.2f*dt;
    camPitch = clampf(camPitch, -1.4f, 1.4f);
    updateScene(dt);
    updateObjects(dt);                               // cisimler animasyon kapalıyken de uçar
    render();
}
