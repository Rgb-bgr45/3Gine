// 3gine - tiny stylized low-poly 3D engine + on-device editor.
// C++17, NativeActivity, EGL, GLES 3.0, Lua 5.4, Dear ImGui (editor build only).
// Build flavors: "editor" (GAME_MODE off) and "game" (GAME_MODE on: plays the project, no editor UI).
// Look: flat shading, banded (toon) light, hemisphere ambient, rim, fog. No PBR, no shadows.
#include <android_native_app_glue.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <ctime>
#include <vector>
#include <string>
#include <unistd.h>
#include <dirent.h>
#include <cstdint>
#include <cerrno>
#include <functional>
#include <filesystem>
#include <map>
#include <dlfcn.h>
#include <csignal>
#include <fcntl.h>
#include <algorithm>
#include <cctype>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"
#include "gine_addon.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "3gine", __VA_ARGS__)

// ---------------------------------------------------------------- math
struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static V3 norm(V3 a) { float l = std::sqrt(dot(a, a)); return l > 1e-8f ? a * (1.f / l) : V3{0, 1, 0}; }
#define GINE_V3_DEFINED
#include "threesics.hpp"   // 3sics physics (uses the V3 above)
#include "depot.hpp"       // asset depot (std::filesystem)

struct M4 { float m[16]; };  // column-major
static M4 mul(const M4& a, const M4& b) {
    M4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}
static M4 perspective(float fovy, float aspect, float n, float f) {
    M4 r = {};
    float t = 1.f / std::tan(fovy * 0.5f);
    r.m[0] = t / aspect; r.m[5] = t;
    r.m[10] = (f + n) / (n - f); r.m[11] = -1.f; r.m[14] = 2.f * f * n / (n - f);
    return r;
}
static M4 lookAt(V3 eye, V3 center, V3 up) {
    V3 f = norm(center - eye), s = norm(cross(f, up)), u = cross(s, f);
    M4 r = {};
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -dot(s, eye); r.m[13] = -dot(u, eye); r.m[14] = dot(f, eye);
    r.m[15] = 1.f;
    return r;
}
// Translate * RotY * Scale
static M4 model(V3 p, float rotY, V3 s) {
    float c = std::cos(rotY), sn = std::sin(rotY);
    M4 r = {};
    r.m[0] = c * s.x;  r.m[2] = -sn * s.x;
    r.m[5] = s.y;
    r.m[8] = sn * s.z; r.m[10] = c * s.z;
    r.m[12] = p.x; r.m[13] = p.y; r.m[14] = p.z; r.m[15] = 1.f;
    return r;
}

// ---------------------------------------------------------------- mesh
struct Vert { V3 p, n; };

struct Mesh {
    std::vector<Vert> v;
    GLuint vao = 0, vbo = 0;

    // Flat-shaded triangle; winding is fixed so the normal points away from `center`.
    void tri(V3 a, V3 b, V3 c, V3 center) {
        V3 n = norm(cross(b - a, c - a));
        V3 mid = (a + b + c) * (1.f / 3.f);
        if (dot(n, mid - center) < 0) { V3 t = b; b = c; c = t; n = n * -1.f; }
        v.push_back({a, n}); v.push_back({b, n}); v.push_back({c, n});
    }
    void quad(V3 a, V3 b, V3 c, V3 d, V3 center) { tri(a, b, c, center); tri(a, c, d, center); }

    void upload() {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(Vert), v.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vert), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vert), (void*)sizeof(V3));
        glBindVertexArray(0);
    }
    void draw() const {
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)v.size());
    }
    void destroy() { glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao); vao = vbo = 0; }
};

static Mesh makeCube() {  // unit cube centered at origin
    Mesh m; V3 o = {0, 0, 0}; const float h = 0.5f;
    m.quad({-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h}, o);
    m.quad({ h,-h,-h}, {-h,-h,-h}, {-h, h,-h}, { h, h,-h}, o);
    m.quad({ h,-h, h}, { h,-h,-h}, { h, h,-h}, { h, h, h}, o);
    m.quad({-h,-h,-h}, {-h,-h, h}, {-h, h, h}, {-h, h,-h}, o);
    m.quad({-h, h, h}, { h, h, h}, { h, h,-h}, {-h, h,-h}, o);
    m.quad({-h,-h,-h}, { h,-h,-h}, { h,-h, h}, {-h,-h, h}, o);
    return m;
}
static Mesh makeCone(int sides) {  // base y=0, apex y=1, radius 0.5
    Mesh m; V3 center = {0, 0.35f, 0}; V3 apex = {0, 1, 0}, bc = {0, 0, 0};
    for (int i = 0; i < sides; i++) {
        float a0 = 6.2831853f * i / sides, a1 = 6.2831853f * (i + 1) / sides;
        V3 p0 = {0.5f * std::cos(a0), 0, 0.5f * std::sin(a0)};
        V3 p1 = {0.5f * std::cos(a1), 0, 0.5f * std::sin(a1)};
        m.tri(apex, p0, p1, center);
        m.tri(bc, p0, p1, center);
    }
    return m;
}

static Mesh makeDisc(int sides) {  // flat disc, radius 0.5, facing up
    Mesh m; V3 below = {0, -1, 0};
    for (int i = 0; i < sides; i++) {
        float a0 = 6.2831853f * i / sides, a1 = 6.2831853f * (i + 1) / sides;
        m.tri({0, 0, 0}, {0.5f * std::cos(a0), 0, 0.5f * std::sin(a0)}, {0.5f * std::cos(a1), 0, 0.5f * std::sin(a1)}, below);
    }
    return m;
}

static void addBox(Mesh& m, V3 lo, V3 hi) {
    V3 c = (lo + hi) * 0.5f;
    V3 p[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
               {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    m.quad(p[0], p[1], p[2], p[3], c); m.quad(p[4], p[5], p[6], p[7], c);
    m.quad(p[0], p[1], p[5], p[4], c); m.quad(p[3], p[2], p[6], p[7], c);
    m.quad(p[0], p[3], p[7], p[4], c); m.quad(p[1], p[2], p[6], p[5], c);
}
static Mesh makeConeEx(int sides, float radius, float rot) {  // base y=0, apex y=1
    Mesh m; V3 center = {0, 0.35f, 0}, apex = {0, 1, 0}, bc = {0, 0, 0};
    for (int i = 0; i < sides; i++) {
        float a0 = rot + 6.2831853f * i / sides, a1 = rot + 6.2831853f * (i + 1) / sides;
        V3 p0 = {radius * std::cos(a0), 0, radius * std::sin(a0)};
        V3 p1 = {radius * std::cos(a1), 0, radius * std::sin(a1)};
        m.tri(apex, p0, p1, center);
        m.tri(bc, p0, p1, center);
    }
    return m;
}
static Mesh makeCylinder(int sides, float radius, float rot) {  // base y=0, top y=1
    Mesh m; V3 center = {0, 0.5f, 0};
    for (int i = 0; i < sides; i++) {
        float a0 = rot + 6.2831853f * i / sides, a1 = rot + 6.2831853f * (i + 1) / sides;
        V3 b0 = {radius * std::cos(a0), 0, radius * std::sin(a0)};
        V3 b1 = {radius * std::cos(a1), 0, radius * std::sin(a1)};
        V3 t0 = b0 + V3{0, 1, 0}, t1 = b1 + V3{0, 1, 0};
        m.quad(b0, b1, t1, t0, center);
        m.tri({0, 1, 0}, t0, t1, center);
        m.tri({0, 0, 0}, b0, b1, center);
    }
    return m;
}
static Mesh makeSphere(int rings, int seg) {  // centred, diameter 1
    Mesh m; V3 o = {0, 0, 0};
    auto P = [&](int r, int s) {
        float lat = -1.5707963f + 3.1415927f * r / rings, phi = 6.2831853f * s / seg;
        return V3{0.5f * std::cos(lat) * std::cos(phi), 0.5f * std::sin(lat), 0.5f * std::cos(lat) * std::sin(phi)};
    };
    for (int r = 0; r < rings; r++)
        for (int s = 0; s < seg; s++) {
            V3 a = P(r, s), b = P(r, s + 1), c = P(r + 1, s + 1), d = P(r + 1, s);
            if (r == 0) m.tri(a, c, d, o);
            else if (r == rings - 1) m.tri(a, b, c, o);
            else m.quad(a, b, c, d, o);
        }
    return m;
}
static Mesh makeWedge() {  // ramp, centred unit box
    Mesh m; V3 c = {0, -0.15f, -0.15f};
    V3 b0 = {-.5f, -.5f, -.5f}, b1 = {.5f, -.5f, -.5f}, b2 = {.5f, -.5f, .5f}, b3 = {-.5f, -.5f, .5f};
    V3 t0 = {-.5f, .5f, -.5f}, t1 = {.5f, .5f, -.5f};
    m.quad(b0, b1, b2, b3, c); m.quad(b0, b1, t1, t0, c); m.quad(b3, b2, t1, t0, c);
    m.tri(b0, b3, t0, c); m.tri(b1, b2, t1, c);
    return m;
}
static Mesh makeTorus(int ringSeg, int tubeSeg, float R, float r) {
    Mesh m;
    for (int i = 0; i < ringSeg; i++)
        for (int j = 0; j < tubeSeg; j++) {
            auto P = [&](int ii, int jj) {
                float u = 6.2831853f * ii / ringSeg, v = 6.2831853f * jj / tubeSeg, rr = R + r * std::cos(v);
                return V3{rr * std::cos(u), r * std::sin(v), rr * std::sin(u)};
            };
            float um = 6.2831853f * (i + 0.5f) / ringSeg;
            m.quad(P(i, j), P(i + 1, j), P(i + 1, j + 1), P(i, j + 1), V3{R * std::cos(um), 0, R * std::sin(um)});
        }
    return m;
}
static Mesh makeStairs() {  // 4 steps rising towards +z
    Mesh m;
    for (int k = 0; k < 4; k++)
        addBox(m, {-0.5f, 0.f, -0.5f + 0.25f * k}, {0.5f, 0.25f * (k + 1), -0.5f + 0.25f * (k + 1)});
    return m;
}

// ---------------------------------------------------------------- shader
static const char* VS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
uniform mat4 uVP;
uniform mat4 uModel;
uniform float uTime;
uniform float uSway;
out vec3 vN;
out vec3 vW;
void main(){
    vec3 lp = aPos;
    lp.x += sin(uTime*1.6 + uModel[3].x*0.7 + uModel[3].z*0.5) * lp.y * uSway;
    vec4 w = uModel * vec4(lp, 1.0);
    vW = w.xyz;
    vN = normalize(mat3(uModel) * aNrm);
    gl_Position = uVP * w;
})";

static const char* FS = R"(#version 300 es
precision highp float;
in vec3 vN;
in vec3 vW;
uniform vec3 uColor;
uniform vec3 uCam;
uniform vec3 uSun;     // direction towards the sun
uniform vec3 uFog;
uniform float uAlpha;  // < 1.0: blob shadow
uniform int uForward;  // 0 = Unlit (cheap built-in look), 1 = Forward (dynamic lights + materials)
uniform vec4 uMat;     // specular, shininess, emissive, metal
uniform int uLN;
uniform vec3 uLP[4];
uniform vec3 uLC[4];
uniform float uLR[4];
out vec4 o;
void main(){
    if (uAlpha < 1.0) { o = vec4(uColor, uAlpha); return; }
    vec3 n = normalize(vN);
    vec3 sunL = normalize(uSun);
    float d = max(dot(n, sunL), 0.0);
    float band = d < 0.2 ? 0.0 : (d < 0.55 ? 0.5 : 1.0);
    vec3 amb = mix(vec3(0.30,0.28,0.38), vec3(0.55,0.65,0.80), n.y*0.5+0.5);
    vec3 vd = normalize(uCam - vW);
    vec3 lit;
    if (uForward == 0) {
        lit = uColor * (amb*0.75 + vec3(1.0,0.93,0.78)*band*0.65);
    } else {
        float metal = uMat.w;
        vec3 diffCol = uColor * (1.0 - 0.6*metal);
        vec3 specCol = mix(vec3(1.0), uColor, metal);
        vec3 hs = normalize(sunL + vd);
        float sp = pow(max(dot(n, hs), 0.0), uMat.y) * uMat.x;
        lit = diffCol * (amb*0.75 + vec3(1.0,0.93,0.78)*band*0.65) + specCol*sp*0.6*band + uColor*uMat.z;
        for (int i = 0; i < 4; i++) {
            if (i >= uLN) break;
            vec3 lv = uLP[i] - vW;
            float dist = length(lv);
            float att = clamp(1.0 - dist/uLR[i], 0.0, 1.0);
            att *= att;
            vec3 l = lv / max(dist, 0.0001);
            float nd = max(dot(n, l), 0.0);
            vec3 hh = normalize(l + vd);
            float s2 = pow(max(dot(n, hh), 0.0), uMat.y) * uMat.x;
            lit += uLC[i] * att * (diffCol*nd + specCol*s2);
        }
    }
    float rim = pow(1.0 - max(dot(n, vd), 0.0), 3.0);
    lit += vec3(0.5,0.6,0.8) * rim * 0.18;
    float f = smoothstep(25.0, 85.0, length(uCam - vW));
    o = vec4(mix(lit, uFog, f), 1.0);
})";

static const char* SKY_VS = R"(#version 300 es
out float vY;
void main(){
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vY = p.y;
    gl_Position = vec4(p * 2.0 - 1.0, 0.999, 1.0);
})";
static const char* SKY_FS = R"(#version 300 es
precision mediump float;
in float vY;
out vec4 o;
void main(){
    vec3 horizon = vec3(0.72, 0.82, 0.92);
    vec3 zenith  = vec3(0.30, 0.52, 0.84);
    o = vec4(mix(horizon, zenith, smoothstep(0.35, 1.0, vY)), 1.0);
})";

static std::string g_shaderLog;
static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char buf[512]; glGetShaderInfoLog(s, 512, nullptr, buf); LOG("shader: %s", buf); g_shaderLog = buf; }
    return s;
}
static GLuint makeProgram(const char* vsSrc = VS, const char* fsSrc = FS) {
    GLuint p = glCreateProgram();
    GLuint vs = compile(GL_VERTEX_SHADER, vsSrc), fs = compile(GL_FRAGMENT_SHADER, fsSrc);
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

// ---------------------------------------------------------------- game
enum Mode { MODE_EDIT, MODE_PLAY };
enum Screen { SCREEN_WELCOME, SCREEN_MENU, SCREEN_PROJECT };
// meshId: 0..9 = 3D meshes, 10 camera, 11 light (3D helpers), 12.. = UI elements (pos/size in % of the screen)
enum { MESH_COUNT = 10, KIND_CAMERA = 10, KIND_LIGHT = 11, KIND_BUTTON = 12, KIND_LABEL = 13,
       KIND_JOYSTICK = 14, KIND_IMAGE = 15, KIND_COUNT = 16 };

static const char* MESH_NAMES[KIND_COUNT] = {"cube", "cone", "disc", "cylinder", "sphere", "pyramid", "prism", "wedge",
                                             "torus", "stairs", "camera", "light", "button", "label", "joystick", "image"};
static const char* ADD_NAMES[KIND_COUNT] = {"Cube", "Cone", "Disc", "Cylinder", "Sphere", "Pyramid", "Prism", "Wedge",
                                            "Torus", "Stairs", "Camera", "Light", "Button", "Label", "Joystick", "Image"};
// vertical extent of each mesh in local space (used for placing and picking)
static const float MESH_Y0[MESH_COUNT] = {-0.5f, 0, -0.05f, 0, -0.5f, 0, 0, -0.5f, -0.15f, 0};
static const float MESH_Y1[MESH_COUNT] = {0.5f, 1, 0.05f, 1, 0.5f, 1, 1, 0.5f, 0.15f, 1};
static inline bool isUIKind(int k) { return k >= KIND_BUTTON; }
static inline bool isHelperKind(int k) { return k == KIND_CAMERA || k == KIND_LIGHT; }

static const char* MATERIALS[6] = {"matte", "plastic", "glossy", "metal", "glow", "rubber"};
// out = specular strength, shininess, emissive, metalness (only used in Forward mode)
static void materialParams(const std::string& m, float out[4]) {
    float v[4] = {0.f, 8.f, 0.f, 0.f};
    if (m == "plastic") { v[0] = 0.5f; v[1] = 32.f; }
    else if (m == "glossy") { v[0] = 0.9f; v[1] = 96.f; }
    else if (m == "metal") { v[0] = 1.f; v[1] = 48.f; v[3] = 1.f; }
    else if (m == "glow") { v[2] = 0.9f; }
    else if (m == "rubber") { v[0] = 0.05f; v[1] = 4.f; }
    for (int i = 0; i < 4; i++) out[i] = v[i];
}
static void parseMaterialText(const std::string& txt, float out[4]) {  // .mat: specular= shininess= emissive= metal=
    out[0] = 0; out[1] = 8; out[2] = 0; out[3] = 0;
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t nl = txt.find('\n', pos);
        if (nl == std::string::npos) nl = txt.size();
        std::string line = txt.substr(pos, nl - pos);
        pos = nl + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        float v = (float)atof(line.c_str() + eq + 1);
        if (k == "specular") out[0] = v; else if (k == "shininess") out[1] = v;
        else if (k == "emissive") out[2] = v; else if (k == "metal") out[3] = v;
    }
}
static void kindDefaults(int k, V3& pos, V3& scale, V3& color, std::string& text) {
    pos = {0, 0, 0}; scale = {1, 1, 1}; color = {0.8f, 0.8f, 0.8f}; text.clear();
    switch (k) {
        case KIND_CAMERA: pos = {0, 3, 8}; color = {0.6f, 0.7f, 0.9f}; break;
        case KIND_LIGHT: pos = {0, 3, 0}; color = {1.f, 0.95f, 0.8f}; break;
        case KIND_BUTTON: pos = {50, 50, 0}; scale = {20, 10, 1}; color = {0.25f, 0.45f, 0.85f}; text = "Button"; break;
        case KIND_LABEL: pos = {50, 50, 0}; scale = {20, 6, 1}; color = {1, 1, 1}; text = "Label"; break;
        case KIND_JOYSTICK: pos = {15, 72, 0}; scale = {28, 28, 1}; color = {1, 1, 1}; break;  // scale.x = diameter (% of screen height)
        case KIND_IMAGE: pos = {50, 50, 0}; scale = {20, 20, 1}; color = {1, 1, 1}; break;
        default: if (k == 1 || k == 5) color = {0.3f, 0.6f, 0.35f}; break;
    }
}

struct Entity {
    int id; bool alive; int meshId;
    std::string name, text, script;
    V3 pos, scale; float rotY; V3 color;
    float spin, sway, shadow;  // shadow = blob shadow radius (0 = none)
    bool broken;               // script raised an error
    int parent = 0;            // 0 = no parent. world = parentWorld * local
    std::string folder;        // editor grouping
    bool hidden = false;       // editor only: not drawn while editing
    bool locked = false;       // editor only: cannot be picked or edited
    float intensity = 1.f, range = 10.f, fov = 0.9f;   // light / camera
    std::string material = "matte", img;               // surface preset / UI image file
    V3 look = {0, 1, 0};       // camera: look-at point
    int lookId = 0;            // camera: look at this object instead (0 = use point)
};

// ---- languages (first-run Welcome screen, saved in config.json) ----
enum Str { S_PLAY, S_STOP, S_SAVE, S_PREPARE, S_ADD, S_SCENES, S_MODS, S_MENU, S_CREATE, S_OPEN, S_REFRESH, S_CLOSE, S_OK, S_CANCEL, S_DELETE, S_DUPLICATE, S_NEW, S_PICK, S_EDIT, S_CLEAR, S_SCENE_LIST, S_INSPECTOR, S_PROJECTS, S_NAME, S_FOLDER, S_PARENT, S_SET, S_POS, S_SIZE, S_ROT, S_COLOR, S_HIDE, S_LOCK, S_SCRIPT, S_DEPOT, S_IMPORT, S_BACKUP, S_SNAP, S_TAP_HINT, S_NO_PROJECTS, S_SAVE_CLOSE, S_NEW_SCENE, S_MORE, S_OFF, S_MATERIAL, S_INTENSITY, S_RANGE, S_LOOK_AT, S_FOV, S_IMAGE, S_ADD_FILE, S_LANGUAGE, S_COUNT };
static const char* LANG_CODES[6] = {"en", "tr", "de", "es", "fr", "ru"};
static const char* LANG_NAMES[6] = {"English", "Türkçe", "Deutsch", "Español", "Français", "Русский"};
static const char* STR[6][S_COUNT] = {
    {"Play", "Stop", "Save", "Prepare for APK", "+ Add", "Scenes", "Mods", "Menu", "Create", "Open", "Refresh", "Close", "OK", "Cancel", "Delete", "Duplicate", "New", "Pick", "Edit", "Clear", "Scene", "Inspector", "Projects", "Name", "Folder", "Parent", "Set", "Pos", "Size", "Rot", "Color", "Hide", "Lock", "Script", "Depot", "Import .3ne", "Backup .3ne", "Snap", "Tap an object in the scene or in the list.", "No projects yet. Press Create to make one.", "Save & Close", "+ New scene", "More", "Off", "Material", "Intensity", "Range", "Look at", "FOV", "Image", "+ Add file", "Language"},
    {"Bitince Çalıştır", "Durdur", "Kaydet", "Apk olacak şekilde düzenle", "+ Ekle", "Sahneler", "Modlar", "Menü", "Oluştur", "Aç", "Yenile", "Kapat", "Tamam", "İptal", "Sil", "Çoğalt", "Yeni", "Seç", "Düzenle", "Temizle", "Sahne", "Özellikler", "Projeler", "İsim", "Klasör", "Ebeveyn", "Ayarla", "Konum", "Boyut", "Dönüş", "Renk", "Gizle", "Kilitle", "Script", "Depo", ".3ne içe aktar", ".3ne yedekle", "Mıknatıs", "Sahnede veya listeden bir nesneye dokun.", "Henüz proje yok. Oluştur'a bas.", "Kaydet ve Kapat", "+ Yeni sahne", "Daha fazla", "Kapalı", "Materyal", "Yoğunluk", "Menzil", "Bakış noktası", "FOV", "Resim", "+ Dosya ekle", "Dil"},
    {"Abspielen", "Stopp", "Speichern", "Für APK vorbereiten", "+ Hinzufügen", "Szenen", "Mods", "Menü", "Erstellen", "Öffnen", "Aktualisieren", "Schließen", "OK", "Abbrechen", "Löschen", "Duplizieren", "Neu", "Wählen", "Bearbeiten", "Leeren", "Szene", "Inspektor", "Projekte", "Name", "Ordner", "Eltern", "Setzen", "Pos", "Größe", "Rot", "Farbe", "Verbergen", "Sperren", "Skript", "Depot", ".3ne importieren", ".3ne sichern", "Raster", "Tippe auf ein Objekt in der Szene oder Liste.", "Noch keine Projekte. Tippe auf Erstellen.", "Speichern & Schließen", "+ Neue Szene", "Mehr", "Aus", "Material", "Intensität", "Reichweite", "Blick auf", "FOV", "Bild", "+ Datei hinzufügen", "Sprache"},
    {"Jugar", "Detener", "Guardar", "Preparar para APK", "+ Añadir", "Escenas", "Mods", "Menú", "Crear", "Abrir", "Actualizar", "Cerrar", "OK", "Cancelar", "Eliminar", "Duplicar", "Nuevo", "Elegir", "Editar", "Limpiar", "Escena", "Inspector", "Proyectos", "Nombre", "Carpeta", "Padre", "Fijar", "Pos", "Tamaño", "Rot", "Color", "Ocultar", "Bloquear", "Script", "Depósito", "Importar .3ne", "Respaldar .3ne", "Ajuste", "Toca un objeto en la escena o en la lista.", "Aún no hay proyectos. Pulsa Crear.", "Guardar y cerrar", "+ Nueva escena", "Más", "No", "Material", "Intensidad", "Alcance", "Mirar a", "FOV", "Imagen", "+ Añadir archivo", "Idioma"},
    {"Jouer", "Arrêter", "Enregistrer", "Préparer pour APK", "+ Ajouter", "Scènes", "Mods", "Menu", "Créer", "Ouvrir", "Actualiser", "Fermer", "OK", "Annuler", "Supprimer", "Dupliquer", "Nouveau", "Choisir", "Modifier", "Effacer", "Scène", "Inspecteur", "Projets", "Nom", "Dossier", "Parent", "Définir", "Pos", "Taille", "Rot", "Couleur", "Masquer", "Verrouiller", "Script", "Dépôt", "Importer .3ne", "Sauvegarder .3ne", "Grille", "Touchez un objet dans la scène ou la liste.", "Aucun projet. Appuyez sur Créer.", "Enregistrer et fermer", "+ Nouvelle scène", "Plus", "Désactivé", "Matériau", "Intensité", "Portée", "Regarder", "FOV", "Image", "+ Ajouter un fichier", "Langue"},
    {"Запуск", "Стоп", "Сохранить", "Подготовить для APK", "+ Добавить", "Сцены", "Моды", "Меню", "Создать", "Открыть", "Обновить", "Закрыть", "ОК", "Отмена", "Удалить", "Дублировать", "Новый", "Выбрать", "Правка", "Очистить", "Сцена", "Инспектор", "Проекты", "Имя", "Папка", "Родитель", "Задать", "Поз", "Размер", "Вращ", "Цвет", "Скрыть", "Блок", "Скрипт", "Склад", "Импорт .3ne", "Резерв .3ne", "Привязка", "Коснитесь объекта в сцене или списке.", "Проектов нет. Нажмите Создать.", "Сохранить и закрыть", "+ Новая сцена", "Ещё", "Выкл", "Материал", "Яркость", "Радиус", "Смотреть на", "FOV", "Изображение", "+ Добавить файл", "Язык"},
};

// sound is implemented in the platform section (JNI MediaPlayer)
static bool playSoundFile(const std::string& path, bool loop);
static void stopAllSounds();

static void mkdirs(const std::string& path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0775);
    mkdir(path.c_str(), 0775);
}
static bool readFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096]; size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}
static bool writeFile(const std::string& path, const std::string& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}
static bool fileExists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }
static std::vector<std::string> listDir(const std::string& dir, bool wantDirs) {  // dir must end with '/'
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* en = readdir(d)) {
        std::string n = en->d_name;
        if (n.empty() || n[0] == '.') continue;
        struct stat st;
        if (stat((dir + n).c_str(), &st) != 0) continue;
        if (wantDirs ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode)) out.push_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}
static std::string cleanName(const std::string& s) {
    std::string o;
    for (char c : s) if (isalnum((unsigned char)c) || c == '_' || c == '-') o += c;
    return o;
}
static std::string esc(std::string s) {
    for (auto& ch : s) if (ch == '"' || ch == '\\' || ch == '\n' || ch == '\r') ch = '\'';
    return s;
}

// ---- crash log: errors and native crashes are written to crash.log with the mod that was running ----
static std::string g_crashPath;
static char g_currentMod[64] = "";
static void setMod(const char* s) { snprintf(g_currentMod, sizeof g_currentMod, "%s", s ? s : ""); }
static void logCrash(const std::string& msg) {
    if (g_crashPath.empty()) return;
    FILE* f = fopen(g_crashPath.c_str(), "ab");
    if (!f) return;
    fprintf(f, "[%ld] mod=%s : %s\n", (long)time(nullptr), g_currentMod[0] ? g_currentMod : "(none)", msg.c_str());
    fclose(f);
}
static void crashHandler(int sig) {
    if (!g_crashPath.empty()) {
        int fd = open(g_crashPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0664);
        if (fd >= 0) {
            char buf[200];
            int n = snprintf(buf, sizeof buf, "[%ld] CRASH signal %d, mod=%s\n", (long)time(nullptr), sig, g_currentMod[0] ? g_currentMod : "(none)");
            if (n > 0) { ssize_t r = write(fd, buf, (size_t)n); (void)r; }
            close(fd);
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
static void installCrashHandler() {
    int sigs[] = {SIGSEGV, SIGABRT, SIGFPE, SIGBUS, SIGILL};
    for (int sg : sigs) signal(sg, crashHandler);
}

struct Addon { std::string id, file, dir, ext; bool enabled = true, supported = false, builtin = false; };
struct NativeCb { std::string id; void (*fn)(float); };

struct Game {
    GLuint prog = 0, skyProg = 0, emptyVao = 0;
    Mesh meshes[MESH_COUNT];
    std::vector<Entity> ents;
    int w = 1, h = 1;
    // camera
    float yaw = 0.6f, pitch = 0.45f, dist = 22.f;
    V3 target = {0, 1.5f, 0};
    bool orbit = true;
    float t = 0;
    // input (filled by the platform layer, read by scripts)
    bool touchDown = false;
    float touchX = 0, touchY = 0, touchDX = 0, touchDY = 0;
    int uiDownId = 0;  // UI button the current touch started on
    // scripting
    lua_State* L = nullptr;
    AAssetManager* assets = nullptr;
    bool updateFailed = false;
    // project / editor state
    Mode mode = MODE_EDIT;
    Screen screen = SCREEN_PROJECT;
    bool sceneLoaded = false;
    std::vector<Entity> snapshot;
    float snapYaw = 0, snapPitch = 0, snapDist = 0; V3 snapTarget = {0, 0, 0};
    int selected = 0;
    std::string baseDir, fallbackDir, projectName, projectDir, status;
    std::vector<std::string> projects;
    int menuSel = -1;
    char nameBuf[64] = {0};
    // script editor
    bool codeOpen = false;
    std::string codeName, pendingInsert;
    std::vector<char> code;
    bool refocusCode = false;
    // hierarchy, scenes, addons
    float lastDt = 0;
    std::string sceneName = "main", snapScene, pendingScene;
    std::string rootDir, addonsDir, internalDir, modError, settingsDir;
    std::vector<std::string> reqs;      // addons the project requires (project.json)
    std::vector<Addon> addons;
    std::vector<NativeCb> nativeCbs;
    std::string blockMsg;               // game build: set when required mods are missing
    bool showAddons = false;
    char sceneBuf[64] = {0};
    // rendering / camera / lights / shaders
    GLuint customProg = 0;
    std::string customShader;
    bool forwardMode = false;
    M4 lastVP = {};
    V3 lastEye = {0, 0, 0};
    std::map<std::string, GLuint> texCache;
    bool customFont = false;
    // joystick
    float joyX = 0, joyY = 0;
    bool joyActive = false;
    // gizmo / snap
    int snapIdx = 0, gizmoAxis = 0;
    V3 gizmoStart = {0, 0, 0};
    float gizmoAccum = 0, gizmoPxPerUnit = 1, gizmoLen = 1;
    ImVec2 gizmoDir = ImVec2(1, 0);
    // language, depot, file browser
    int lang = 0;
    std::string settingsPath, depotDir, browseDir, browseKind;
    bool showDepot = false, showBrowser = false;
    // 3sics physics (active while the "3sics" addon is enabled) and the asset depot
    Physics3 physics;
    bool physicsOn = false;
    std::vector<std::pair<int, int>> pendingHits;
    Depot depot;

    const Mesh& meshOf(int id) const { return meshes[(id >= 0 && id < MESH_COUNT) ? id : 0]; }
    void init();
    void shutdown();
    void update(float dt);
    void render();
    void drawUI();
    std::string readAsset(const char* path);
    std::string readSource(const char* path);
    bool runFile(const char* path);
    void newLua();
    void loadScene();
    void startPlay();
    void stopPlay();
    bool inViewport(float x, float y) const;
    // scripts
    bool pushEnvFn(int id, const char* fn);
    void attachScript(int id);
    void clickUI(int id);
    void luaErr(const char* where, int id);
    // UI elements
    void uiRect(const Entity& e, ImVec2& a, ImVec2& b) const;
    int uiAt(float x, float y, bool includeLabels) const;
    void drawEntityUI();
    // editor
    void setupBaseDir();
    void refreshProjects();
    bool createProject(const std::string& name);
    bool openProject(const std::string& name);
    bool saveProject();
    void pick(float sx, float sy);
    int addEntity(int kind);
    std::vector<std::string> listScripts() const;
    void newScriptFor(int id);
    void openCode(const std::string& name);
    void saveCode();
    void drawEditorUI();
    void drawMenu(ImGuiWindowFlags F);
    void drawAddons(ImGuiWindowFlags F);
    // hierarchy
    M4 worldOf(int id, int depth = 0) const;
    V3 worldPos(const Entity& e) const;
    V3 worldScale(const Entity& e) const;
    bool setParent(int id, int pid);
    void destroyEntity(int id);
    int raycast(V3 o, V3 d, float& tHit, bool forPick) const;
    int spawnBasic(const char* mesh, V3 p);
    // scenes
    std::string sceneFile(const std::string& name) const;
    void runtimeInit();
    void switchScene(const std::string& name);
    std::vector<std::string> listScenes() const;
    // addons
    void loadRequires();
    void saveRequires();
    std::vector<Addon> scanAddons() const;
    bool toggleAddon(int idx);
    std::string checkRequires();
    void loadAddons();
    void loadLuaAddon(const std::string& id, const std::string& src);
    void loadNativeAddon(const Addon& a);
    void callMods(float dt);
    void modErr(const std::string& id);
    // rendering
    bool loadShader(const std::string& file);
    GLuint getTexture(const std::string& name);
    bool sceneCamera(V3& eye, V3& tgt, float& fov) const;
    std::string soundPath(const std::string& name);
    const char* tr(int k) const { return STR[lang][k]; }
    void updateJoystick(float x, float y);
    // gizmo
    bool project(V3 p, ImVec2& out) const;
    float snapVal() const { return snapIdx == 1 ? 0.5f : (snapIdx == 2 ? 1.f : 0.f); }
    bool gizmoGeom(V3& c, ImVec2 sc[4]);
    bool gizmoBegin(float x, float y);
    void gizmoMove(float dx, float dy);
    void gizmoEnd() { gizmoAxis = 0; }
    void drawGizmo();
    // editor: welcome, depot, browser, archives
    void loadConfig();
    void saveConfig();
    void drawWelcome(ImGuiWindowFlags F);
    void drawDepot(ImGuiWindowFlags F);
    void drawBrowser(ImGuiWindowFlags F);
    void browsePick(const std::string& path);
    bool exportBackup(std::string& outPath);
    bool importBackup(const std::string& path);
    bool prepareApk(std::string& outPath);
    void stepPhysics(float dt);
    void matParams(const std::string& m, float out[4]);
};
static Game* G = nullptr;

// ---- Lua bindings: the "engine" table ----
static float numField(lua_State* L, int t, const char* k, float d) {
    lua_getfield(L, t, k);
    float v = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : d;
    lua_pop(L, 1);
    return v;
}
static V3 vecField(lua_State* L, int t, const char* k, V3 d) {
    lua_getfield(L, t, k);
    if (lua_istable(L, -1)) {
        float a[3] = {d.x, d.y, d.z};
        for (int i = 0; i < 3; i++) {
            lua_rawgeti(L, -1, i + 1);
            if (lua_isnumber(L, -1)) a[i] = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        d = {a[0], a[1], a[2]};
    }
    lua_pop(L, 1);
    return d;
}
static std::string strField(lua_State* L, int t, const char* k, const std::string& d) {
    lua_getfield(L, t, k);
    std::string r = lua_isstring(L, -1) ? lua_tostring(L, -1) : d;
    lua_pop(L, 1);
    return r;
}
static bool boolField(lua_State* L, int t, const char* k, bool d) {
    lua_getfield(L, t, k);
    bool v = lua_isnil(L, -1) ? d : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}
static Entity* entArg(lua_State* L, int i) {
    int id = (int)luaL_checkinteger(L, i);
    if (id < 1 || id > (int)G->ents.size() || !G->ents[id - 1].alive) return nullptr;
    return &G->ents[id - 1];
}

// engine.spawn{ name=, mesh=, pos=, scale=, color=, rot=, spin=, sway=, shadow=, text=, script= } -> id
// mesh: cube cone disc cylinder sphere pyramid prism wedge torus stairs | button label (UI: pos/scale are % of screen)
static int l_spawn(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string mn = strField(L, 1, "mesh", "cube");
    int meshId = 0;
    for (int i = 0; i < KIND_COUNT; i++) if (mn == MESH_NAMES[i]) meshId = i;
    Entity e;
    V3 dp, ds, dc; std::string dt;
    kindDefaults(meshId, dp, ds, dc, dt);
    e.id = (int)G->ents.size() + 1; e.alive = true; e.meshId = meshId; e.broken = false;
    e.name = strField(L, 1, "name", MESH_NAMES[meshId]);
    e.text = strField(L, 1, "text", dt);
    e.script = strField(L, 1, "script", "");
    e.pos = vecField(L, 1, "pos", dp);
    e.scale = vecField(L, 1, "scale", ds);
    e.color = vecField(L, 1, "color", dc);
    e.rotY = numField(L, 1, "rot", 0);
    e.spin = numField(L, 1, "spin", 0);
    e.sway = numField(L, 1, "sway", 0);
    e.shadow = numField(L, 1, "shadow", 0);
    e.parent = (int)numField(L, 1, "parent", 0);
    e.folder = strField(L, 1, "folder", "");
    e.hidden = boolField(L, 1, "hidden", false);
    e.locked = boolField(L, 1, "locked", false);
    e.intensity = numField(L, 1, "intensity", 1.f);
    e.range = numField(L, 1, "range", 10.f);
    e.fov = numField(L, 1, "fov", 0.9f);
    e.material = strField(L, 1, "material", "matte");
    e.img = strField(L, 1, "img", "");
    e.look = vecField(L, 1, "look", {0, 1, 0});
    e.lookId = (int)numField(L, 1, "lookid", 0);
    G->ents.push_back(e);
    if (G->mode == MODE_PLAY && !e.script.empty()) G->attachScript(e.id);  // objects spawned during play get their script
    lua_pushinteger(L, e.id);
    return 1;
}
static int l_find(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    for (const auto& e : G->ents)
        if (e.alive && e.name == n) { lua_pushinteger(L, e.id); return 1; }
    return 0;
}
// engine.find_all(name) -> table of ids (all objects when name is omitted)
static int l_find_all(lua_State* L) {
    const char* n = luaL_optstring(L, 1, nullptr);
    lua_newtable(L);
    int k = 1;
    for (const auto& e : G->ents)
        if (e.alive && (!n || e.name == n)) { lua_pushinteger(L, e.id); lua_rawseti(L, -2, k++); }
    return 1;
}
static int l_destroy(lua_State* L) { if (Entity* e = entArg(L, 1)) G->destroyEntity(e->id); return 0; }
static int l_set_pos(lua_State* L) {
    if (Entity* e = entArg(L, 1)) e->pos = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_optnumber(L, 4, e->pos.z)};
    return 0;
}
static int l_move(lua_State* L) {
    if (Entity* e = entArg(L, 1)) {
        e->pos.x += (float)luaL_optnumber(L, 2, 0); e->pos.y += (float)luaL_optnumber(L, 3, 0); e->pos.z += (float)luaL_optnumber(L, 4, 0);
    }
    return 0;
}
static int l_get_pos(lua_State* L) {
    Entity* e = entArg(L, 1);
    if (!e) return 0;
    lua_pushnumber(L, e->pos.x); lua_pushnumber(L, e->pos.y); lua_pushnumber(L, e->pos.z);
    return 3;
}
static int l_set_scale(lua_State* L) {
    if (Entity* e = entArg(L, 1)) e->scale = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_optnumber(L, 4, e->scale.z)};
    return 0;
}
static int l_get_scale(lua_State* L) {
    Entity* e = entArg(L, 1);
    if (!e) return 0;
    lua_pushnumber(L, e->scale.x); lua_pushnumber(L, e->scale.y); lua_pushnumber(L, e->scale.z);
    return 3;
}
static int l_set_color(lua_State* L) {
    if (Entity* e = entArg(L, 1)) e->color = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4)};
    return 0;
}
static int l_set_rot(lua_State* L) { if (Entity* e = entArg(L, 1)) e->rotY = (float)luaL_checknumber(L, 2); return 0; }
static int l_get_rot(lua_State* L) { Entity* e = entArg(L, 1); if (!e) return 0; lua_pushnumber(L, e->rotY); return 1; }
static int l_set_text(lua_State* L) { if (Entity* e = entArg(L, 1)) e->text = luaL_tolstring(L, 2, nullptr); return 0; }
static int l_get_text(lua_State* L) { Entity* e = entArg(L, 1); if (!e) return 0; lua_pushstring(L, e->text.c_str()); return 1; }
static int l_camera(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    G->yaw = numField(L, 1, "yaw", G->yaw);
    G->pitch = numField(L, 1, "pitch", G->pitch);
    G->dist = numField(L, 1, "dist", G->dist);
    G->target = vecField(L, 1, "target", G->target);
    return 0;
}
static int l_set_orbit(lua_State* L) { G->orbit = lua_toboolean(L, 1) != 0; return 0; }
static int l_touch(lua_State* L) {
    lua_pushboolean(L, G->touchDown);
    lua_pushnumber(L, G->touchX); lua_pushnumber(L, G->touchY);
    lua_pushnumber(L, G->touchDX); lua_pushnumber(L, G->touchDY);
    return 5;
}
static int l_screen(lua_State* L) { lua_pushinteger(L, G->w); lua_pushinteger(L, G->h); return 2; }
static int l_time(lua_State* L) { lua_pushnumber(L, G->t); return 1; }
static int l_log(lua_State* L) { LOG("[lua] %s", luaL_tolstring(L, 1, nullptr)); return 0; }
static int l_load(lua_State* L) { lua_pushboolean(L, G->runFile(luaL_checkstring(L, 1))); return 1; }

static int l_get_name(lua_State* L) { Entity* e = entArg(L, 1); if (!e) return 0; lua_pushstring(L, e->name.c_str()); return 1; }
// engine.set_parent(id, parentId)  (0 = no parent) -> true / false (false = would create a loop)
static int l_set_parent(lua_State* L) {
    Entity* e = entArg(L, 1);
    if (!e) return 0;
    lua_pushboolean(L, G->setParent(e->id, (int)luaL_optinteger(L, 2, 0)));
    return 1;
}
static int l_get_parent(lua_State* L) { Entity* e = entArg(L, 1); if (!e) return 0; lua_pushinteger(L, e->parent); return 1; }
static int l_get_world_pos(lua_State* L) {
    Entity* e = entArg(L, 1);
    if (!e) return 0;
    V3 p = G->worldPos(*e);
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y); lua_pushnumber(L, p.z);
    return 3;
}
static int l_get_distance(lua_State* L) {
    Entity* a = entArg(L, 1); Entity* b = entArg(L, 2);
    if (!a || !b) return 0;
    V3 d = G->worldPos(*a) - G->worldPos(*b);
    lua_pushnumber(L, std::sqrt(dot(d, d)));
    return 1;
}
// engine.move_towards(id, tx, ty, tz, speed) -> true when the target is reached (speed in units per second)
static int l_move_towards(lua_State* L) {
    Entity* e = entArg(L, 1);
    if (!e) return 0;
    V3 t = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4)};
    float step = (float)luaL_checknumber(L, 5) * G->lastDt;
    V3 d = t - e->pos;
    float len = std::sqrt(dot(d, d));
    if (len <= step || len < 1e-6f) { e->pos = t; lua_pushboolean(L, 1); }
    else { e->pos = e->pos + d * (step / len); lua_pushboolean(L, 0); }
    return 1;
}
// engine.check_raycast(x,y,z, dx,dy,dz) -> id, distance of the nearest 3D object hit (nothing if no hit)
static int l_check_raycast(lua_State* L) {
    V3 o = {(float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)};
    V3 d = norm({(float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5), (float)luaL_checknumber(L, 6)});
    float t = 0;
    int id = G->raycast(o, d, t, false);
    if (!id) return 0;
    lua_pushinteger(L, id); lua_pushnumber(L, t);
    return 2;
}
static int l_load_scene(lua_State* L) { G->pendingScene = luaL_checkstring(L, 1); return 0; }  // switches on the next frame

// engine.get_bounds(id) -> minx,miny,minz,maxx,maxy,maxz  (world box, rotation ignored)
static int l_get_bounds(lua_State* L) {
    Entity* e = entArg(L, 1);
    if (!e || isUIKind(e->meshId)) return 0;
    V3 p = G->worldPos(*e), sc = G->worldScale(*e);
    float y0 = -0.4f, y1 = 0.4f, hx = sc.x * 0.5f, hz = sc.z * 0.5f;
    if (e->meshId < MESH_COUNT) { y0 = MESH_Y0[e->meshId] * sc.y; y1 = MESH_Y1[e->meshId] * sc.y; }
    else { hx = hz = 0.4f; }
    lua_pushnumber(L, p.x - hx); lua_pushnumber(L, p.y + y0); lua_pushnumber(L, p.z - hz);
    lua_pushnumber(L, p.x + hx); lua_pushnumber(L, p.y + y1); lua_pushnumber(L, p.z + hz);
    return 6;
}
// engine.set_camera_look_at(cameraId, x, y, z)   or   (cameraId, targetObjectId)
static int l_set_camera_look_at(lua_State* L) {
    Entity* e = entArg(L, 1);
    if (!e || e->meshId != KIND_CAMERA) return 0;
    if (lua_gettop(L) == 2) e->lookId = (int)luaL_checkinteger(L, 2);
    else { e->look = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4)}; e->lookId = 0; }
    return 0;
}
static int l_set_camera_fov(lua_State* L) { Entity* e = entArg(L, 1); if (e && e->meshId == KIND_CAMERA) e->fov = (float)luaL_checknumber(L, 2); return 0; }
// engine.set_light_properties(id, r, g, b, intensity, range)
static int l_set_light_properties(lua_State* L) {
    Entity* e = entArg(L, 1);
    if (!e || e->meshId != KIND_LIGHT) return 0;
    e->color = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4)};
    e->intensity = (float)luaL_optnumber(L, 5, e->intensity);
    e->range = (float)luaL_optnumber(L, 6, e->range);
    return 0;
}
// engine.set_material(id, "matte"|"plastic"|"glossy"|"metal"|"glow"|"rubber")  (visible in Forward mode)
static int l_set_material(lua_State* L) { if (Entity* e = entArg(L, 1)) e->material = luaL_checkstring(L, 2); return 0; }
static int l_set_image(lua_State* L) { if (Entity* e = entArg(L, 1)) e->img = luaL_checkstring(L, 2); return 0; }
// engine.load_shader("file.frag") loads <project>/shaders/file.frag and switches to Forward mode; "default" resets
static int l_load_shader(lua_State* L) { lua_pushboolean(L, G->loadShader(luaL_checkstring(L, 1))); return 1; }
static int l_play_sound(lua_State* L) {
    std::string path = G->soundPath(luaL_checkstring(L, 1));
    lua_pushboolean(L, !path.empty() && playSoundFile(path, lua_toboolean(L, 2) != 0));
    return 1;
}
static int l_stop_sounds(lua_State* L) { (void)L; stopAllSounds(); return 0; }

// ---- 3sics physics (needs the "3sics" addon to be enabled) ----
// engine.physics_add_body(id, {shape="box"|"sphere", mass=1, restitution=0.3, static=false})
static int l_physics_add_body(lua_State* L) {
    Entity* e = entArg(L, 1);
    if (!e || isUIKind(e->meshId) || isHelperKind(e->meshId)) return 0;
    if (!G->physicsOn) { LOG("physics_add_body: the 3sics addon is not enabled"); lua_pushboolean(L, 0); return 1; }
    V3 p = G->worldPos(*e), sc = G->worldScale(*e);
    bool sphere = strField(L, 2, "shape", "box") == "sphere";
    V3 half = sphere ? V3{sc.x * 0.5f, sc.x * 0.5f, sc.x * 0.5f} : V3{sc.x * 0.5f, sc.y * 0.5f, sc.z * 0.5f};
    p.y += (MESH_Y0[e->meshId] + MESH_Y1[e->meshId]) * 0.5f * sc.y;   // box centre
    G->physics.add(e->id, p, half, sphere, numField(L, 2, "mass", 1.f), numField(L, 2, "restitution", 0.3f), boolField(L, 2, "static", false));
    lua_pushboolean(L, 1);
    return 1;
}
static int l_physics_remove_body(lua_State* L) { G->physics.remove((int)luaL_checkinteger(L, 1)); return 0; }
static int l_physics_clear(lua_State* L) { (void)L; G->physics.clear(); return 0; }
static int l_physics_set_gravity(lua_State* L) {
    G->physics.gravity = {(float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)};
    return 0;
}
static int l_physics_add_impulse(lua_State* L) {
    G->physics.impulse((int)luaL_checkinteger(L, 1), {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4)});
    return 0;
}
static int l_physics_set_velocity(lua_State* L) {
    if (PBody* b = G->physics.get((int)luaL_checkinteger(L, 1)))
        b->vel = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4)};
    return 0;
}
static int l_physics_get_velocity(lua_State* L) {
    PBody* b = G->physics.get((int)luaL_checkinteger(L, 1));
    if (!b) return 0;
    lua_pushnumber(L, b->vel.x); lua_pushnumber(L, b->vel.y); lua_pushnumber(L, b->vel.z);
    return 3;
}

static const luaL_Reg ENGINE_FUNCS[] = {
    {"physics_add_body", l_physics_add_body}, {"physics_remove_body", l_physics_remove_body},
    {"physics_clear", l_physics_clear}, {"physics_set_gravity", l_physics_set_gravity},
    {"physics_add_impulse", l_physics_add_impulse}, {"physics_set_velocity", l_physics_set_velocity},
    {"physics_get_velocity", l_physics_get_velocity},
    {"spawn", l_spawn}, {"find", l_find}, {"find_all", l_find_all}, {"destroy", l_destroy},
    {"set_pos", l_set_pos}, {"get_pos", l_get_pos}, {"move", l_move},
    {"set_scale", l_set_scale}, {"get_scale", l_get_scale},
    {"set_color", l_set_color}, {"set_rot", l_set_rot}, {"get_rot", l_get_rot},
    {"set_text", l_set_text}, {"get_text", l_get_text}, {"get_name", l_get_name},
    {"set_parent", l_set_parent}, {"get_parent", l_get_parent}, {"get_world_pos", l_get_world_pos},
    {"get_distance", l_get_distance}, {"move_towards", l_move_towards}, {"check_raycast", l_check_raycast},
    {"get_bounds", l_get_bounds}, {"load_scene", l_load_scene},
    {"set_camera_look_at", l_set_camera_look_at}, {"set_camera_fov", l_set_camera_fov},
    {"set_light_properties", l_set_light_properties}, {"set_material", l_set_material},
    {"load_shader", l_load_shader}, {"set_image", l_set_image},
    {"play_sound", l_play_sound}, {"stop_sounds", l_stop_sounds},
    {"camera", l_camera}, {"set_orbit", l_set_orbit}, {"touch", l_touch},
    {"screen", l_screen}, {"time", l_time}, {"log", l_log}, {"load", l_load},
    {nullptr, nullptr}};

// ---- project files / scripts ----
std::string Game::readAsset(const char* path) {
    std::string out;
    if (!assets) return out;
    AAsset* a = AAssetManager_open(assets, path, AASSET_MODE_BUFFER);
    if (!a) return out;
    out.assign((const char*)AAsset_getBuffer(a), AAsset_getLength(a));
    AAsset_close(a);
    return out;
}
// Editor: the open project's folder on shared storage. Game build: the project bundled as assets.
std::string Game::readSource(const char* path) {
    std::string out;
    if (!projectDir.empty()) { readFile(projectDir + path, out); return out; }
    return readAsset(path);
}
void Game::luaErr(const char* where, int id) {
    status = std::string(where) + ": " + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "error");
    LOG("lua %s: %s", where, status.c_str());
    logCrash(status);
    lua_pop(L, 1);
    if (id > 0 && id <= (int)ents.size()) ents[id - 1].broken = true;
}
bool Game::runFile(const char* path) {
    std::string src = readSource(path);
    if (src.empty()) { LOG("script not found: %s", path); status = std::string("Not found: ") + path; return false; }
    if (luaL_loadbufferx(L, src.data(), src.size(), path, "t") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        luaErr(path, 0);
        return false;
    }
    return true;
}
void Game::newLua() {
    if (L) lua_close(L);
    L = luaL_newstate();
    luaL_openlibs(L);
    luaL_newlib(L, ENGINE_FUNCS);
    lua_setglobal(L, "engine");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "envs");  // per-object script environments, keyed by object id
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "mods");  // loaded Lua addons
    updateFailed = false;
}
// Pushes env[fn] (raw lookup) of the script attached to object `id`. Leaves the stack unchanged if absent.
bool Game::pushEnvFn(int id, const char* fn) {
    if (!L) return false;
    lua_getfield(L, LUA_REGISTRYINDEX, "envs");
    lua_rawgeti(L, -1, id);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return false; }
    lua_pushstring(L, fn);
    lua_rawget(L, -2);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 3); return false; }
    lua_remove(L, -2);
    lua_remove(L, -2);
    return true;
}
// Loads scripts/<script>.lua into its own environment for this object, then calls start(self).
void Game::attachScript(int id) {
    if (!L || id < 1 || id > (int)ents.size()) return;
    std::string file = "scripts/" + ents[id - 1].script + ".lua";
    std::string src = readSource(file.c_str());
    if (src.empty()) { status = "Script not found: " + file; ents[id - 1].broken = true; return; }
    if (luaL_loadbufferx(L, src.data(), src.size(), file.c_str(), "t") != LUA_OK) { luaErr(file.c_str(), id); return; }
    lua_newtable(L);                    // env
    lua_newtable(L);                    // metatable: env falls back to the globals (engine, math, ...)
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_getfield(L, LUA_REGISTRYINDEX, "envs");
    lua_pushvalue(L, -2);
    lua_rawseti(L, -2, id);
    lua_pop(L, 1);
    lua_setupvalue(L, -2, 1);           // chunk's _ENV = env (pops env)
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) { luaErr(file.c_str(), id); return; }
    if (pushEnvFn(id, "start")) {
        lua_pushinteger(L, id);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) luaErr("start", id);
    }
}
void Game::clickUI(int id) {
    if (!L || id < 1 || id > (int)ents.size()) return;
    if (!ents[id - 1].alive || ents[id - 1].broken) return;
    if (pushEnvFn(id, "on_click")) {
        lua_pushinteger(L, id);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) luaErr("on_click", id);
    }
}

std::string Game::sceneFile(const std::string& name) const {
    return name == "main" ? std::string("scene.lua") : "scenes/" + cleanName(name) + ".lua";
}
void Game::loadScene() {
    ents.clear();
    selected = 0;
    mode = MODE_EDIT;
    orbit = true;
    newLua();
    runFile(sceneFile(sceneName).c_str());
}
// Runs after the scene objects exist: addons, the optional global script, then the per-object scripts.
void Game::runtimeInit() {
    physics.clear();
    physicsOn = false;  // the "3sics" addon switches it on in loadAddons()
    size_t n0 = ents.size();  // objects spawned later (by scripts) attach their own scripts in spawn()
    loadAddons();
    if (!readSource("scripts/main.lua").empty() && runFile("scripts/main.lua")) {
        lua_getglobal(L, "start");
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) luaErr("start()", 0);
        } else lua_pop(L, 1);
    }
    for (size_t i = 0; i < n0; i++)
        if (ents[i].alive && !ents[i].script.empty()) attachScript((int)i + 1);
}
void Game::startPlay() {
    snapshot = ents;
    snapScene = sceneName;
    snapYaw = yaw; snapPitch = pitch; snapDist = dist; snapTarget = target;
    mode = MODE_PLAY;
    orbit = true;
    touchDown = false; uiDownId = 0;
    status.clear();
    for (auto& e : ents) e.broken = false;
    newLua();
    runtimeInit();
}
// engine.load_scene("name") while playing
void Game::switchScene(const std::string& name) {
    sceneName = cleanName(name);
    if (sceneName.empty()) sceneName = "main";
    ents.clear();
    selected = 0;
    touchDown = false; uiDownId = 0;
    newLua();
    Mode m = mode;
    mode = MODE_EDIT;  // spawn() must not attach scripts yet: runtimeInit() does that once
    bool found = runFile(sceneFile(sceneName).c_str());
    mode = m;
    if (!found) status = "Scene not found: " + sceneName;
    runtimeInit();
}
void Game::stopPlay() {
    stopAllSounds();
    physics.clear(); physicsOn = false;
    joyX = joyY = 0; joyActive = false;
    ents = snapshot;
    sceneName = snapScene;
    yaw = snapYaw; pitch = snapPitch; dist = snapDist; target = snapTarget;
    if (L) { lua_close(L); L = nullptr; }
    mode = MODE_EDIT;
    orbit = true;
    touchDown = false; uiDownId = 0;
    selected = 0;
}
bool Game::inViewport(float x, float y) const {
#ifdef GAME_MODE
    (void)x; (void)y; return true;
#else
    if (screen != SCREEN_PROJECT || codeOpen || showAddons || showDepot || showBrowser) return false;
    float W = (float)w, H = (float)h;
    if (mode == MODE_PLAY) return !(x > 0.86f * W && y < 0.12f * H);
    return x > 0.25f * W && x < 0.70f * W && y > 0.12f * H;
#endif
}

void Game::init() {
    G = this;
    prog = makeProgram();
    skyProg = makeProgram(SKY_VS, SKY_FS);
    glGenVertexArrays(1, &emptyVao);
    meshes[0] = makeCube();
    meshes[1] = makeConeEx(7, 0.5f, 0.f);
    meshes[2] = makeDisc(14);
    meshes[3] = makeCylinder(10, 0.5f, 0.f);
    meshes[4] = makeSphere(6, 10);
    meshes[5] = makeConeEx(4, 0.70710678f, 0.78539816f);
    meshes[6] = makeCylinder(3, 0.5f, 0.f);
    meshes[7] = makeWedge();
    meshes[8] = makeTorus(12, 6, 0.35f, 0.15f);
    meshes[9] = makeStairs();
    for (auto& m : meshes) m.upload();
    if (!customShader.empty()) { std::string f = customShader; loadShader(f); }  // after the GL context was recreated
    if (!sceneLoaded) {  // only the first time: the open project survives the app going to background
#ifdef GAME_MODE
        screen = SCREEN_PROJECT;
        g_crashPath = fallbackDir + "crash.log";
        installCrashHandler();
        loadRequires();
        blockMsg = checkRequires();
        loadScene();
        if (blockMsg.empty()) startPlay();
#else
        settingsPath = fallbackDir + "setting/config.json";
        settingsDir = fallbackDir + "setting/";
        mkdirs(settingsDir);
        depotDir = fallbackDir;
        if (!depotDir.empty()) depot.init(depotDir);  // creates img/ sound/ material/
        setupBaseDir();
        g_crashPath = rootDir + "crash.log";
        installCrashHandler();
        loadConfig();  // first run: Welcome screen; afterwards the saved language and the project menu
        refreshProjects();
#endif
        sceneLoaded = true;
    }
}
void Game::shutdown() {  // frees GL objects only; project and Lua state are kept
    if (!prog) return;
    for (auto& m : meshes) m.destroy();
    for (auto& kv : texCache) if (kv.second) glDeleteTextures(1, &kv.second);
    texCache.clear();
    if (customProg) { glDeleteProgram(customProg); customProg = 0; }
    glDeleteProgram(prog); glDeleteProgram(skyProg); glDeleteVertexArrays(1, &emptyVao);
    prog = 0;
}

bool Game::loadShader(const std::string& file) {
    if (file.empty() || file == "default") {
        if (customProg) glDeleteProgram(customProg);
        customProg = 0; customShader.clear();
        return true;
    }
    std::string src = readSource(("shaders/" + file).c_str());
    if (src.empty()) { status = "Shader not found: " + file; logCrash(status); return false; }
    g_shaderLog.clear();
    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, src.c_str());
    GLuint pr = glCreateProgram();
    glAttachShader(pr, vs); glAttachShader(pr, fs);
    glLinkProgram(pr);
    GLint ok = 0;
    glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!ok) {
        status = "Shader '" + file + "': " + (g_shaderLog.empty() ? std::string("link error") : g_shaderLog);
        logCrash(status);
        glDeleteProgram(pr);
        return false;
    }
    if (customProg) glDeleteProgram(customProg);
    customProg = pr; customShader = file;  // a custom shader switches the renderer to Forward mode
    return true;
}
GLuint Game::getTexture(const std::string& name) {
    if (name.empty()) return 0;
    auto it = texCache.find(name);
    if (it != texCache.end()) return it->second;
    std::string data;
    if (!depotDir.empty()) depot.read("img", name, data);
    if (data.empty()) data = readSource(("img/" + name).c_str());
    GLuint tex = 0;
    if (!data.empty()) {
        int iw = 0, ih = 0, n = 0;
        unsigned char* px = stbi_load_from_memory((const unsigned char*)data.data(), (int)data.size(), &iw, &ih, &n, 4);
        if (px) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            stbi_image_free(px);
        }
    }
    texCache[name] = tex;  // 0 = failed, do not retry every frame
    return tex;
}
std::string Game::soundPath(const std::string& name) {
    if (!depotDir.empty() && fs::exists(depot.pathOf("sound", name))) return depot.pathOf("sound", name).string();
    std::string data = readSource(("sound/" + name).c_str());  // bundled with the project (game APK)
    if (data.empty() || internalDir.empty()) return "";
    std::string dir = internalDir + "/cache/";
    mkdirs(dir);
    std::string flat = name;
    for (auto& ch : flat) if (ch == '/') ch = '_';
    if (!writeFile(dir + flat, data)) return "";
    return dir + flat;
}
bool Game::sceneCamera(V3& eye, V3& tgt, float& fov) const {  // first Camera object in the scene
    for (const auto& e : ents) {
        if (!e.alive || e.meshId != KIND_CAMERA) continue;
        eye = worldPos(e);
        tgt = (e.lookId >= 1 && e.lookId <= (int)ents.size() && ents[e.lookId - 1].alive) ? worldPos(ents[e.lookId - 1]) : e.look;
        fov = e.fov;
        return true;
    }
    return false;
}
void Game::matParams(const std::string& m, float out[4]) {
    if (m.size() > 4 && m.compare(m.size() - 4, 4, ".mat") == 0) {  // material file from the depot (editor) or the project (game)
        std::string txt;
        if (!depotDir.empty()) depot.read("material", m, txt);
        if (txt.empty()) txt = readSource(("material/" + m).c_str());
        if (!txt.empty()) { parseMaterialText(txt, out); return; }
    }
    materialParams(m, out);
}
// 3sics: bodies read their position from the objects every frame (scripts may move them), then the world
// is stepped and the movement is written back. Collision events reach Lua as a global on_collision(a, b).
void Game::stepPhysics(float dt) {
    for (auto& b : physics.bodies()) {
        if (b.id < 1 || b.id > (int)ents.size() || !ents[b.id - 1].alive) continue;
        const Entity& e = ents[b.id - 1];
        V3 p = worldPos(e), sc = worldScale(e);
        b.pos = b.prev = {p.x, p.y + (MESH_Y0[e.meshId] + MESH_Y1[e.meshId]) * 0.5f * sc.y, p.z};
    }
    physics.onCollision = [this](int a, int b) { pendingHits.push_back({a, b}); };
    physics.step(dt);
    for (auto& b : physics.bodies()) {
        if (b.isStatic || b.id < 1 || b.id > (int)ents.size() || !ents[b.id - 1].alive) continue;
        ents[b.id - 1].pos = ents[b.id - 1].pos + (b.pos - b.prev);
    }
    std::vector<std::pair<int, int>> hits;
    hits.swap(pendingHits);
    for (auto& h : hits) {
        if (!L) break;
        lua_getglobal(L, "on_collision");
        if (lua_isfunction(L, -1)) {
            lua_pushinteger(L, h.first); lua_pushinteger(L, h.second);
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) luaErr("on_collision", 0);
        } else { lua_pop(L, 1); break; }
    }
}
void Game::updateJoystick(float x, float y) {
    if (uiDownId < 1 || uiDownId > (int)ents.size()) return;
    ImVec2 a, b; uiRect(ents[uiDownId - 1], a, b);
    float r = (b.y - a.y) * 0.5f;
    if (r < 1.f) return;
    float dx = (x - (a.x + b.x) * 0.5f) / r, dy = (y - (a.y + b.y) * 0.5f) / r;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len > 1.f) { dx /= len; dy /= len; }
    joyX = dx; joyY = -dy;  // y up is positive
}

void Game::update(float dt) {
    t += dt;
    lastDt = dt;
    if (!pendingScene.empty() && mode == MODE_PLAY) {
        std::string n = pendingScene;
        pendingScene.clear();
        switchScene(n);
    }
    if (screen == SCREEN_PROJECT && mode == MODE_PLAY) {
        for (auto& e : ents) if (e.alive) e.rotY += e.spin * dt;
        if (L && !updateFailed) {
            lua_getglobal(L, "update");
            if (lua_isfunction(L, -1)) {
                lua_pushnumber(L, dt);
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) { luaErr("update()", 0); updateFailed = true; }
            } else lua_pop(L, 1);
        }
        if (L) {  // virtual joystick values for scripts
            lua_pushnumber(L, joyX); lua_setglobal(L, "joystick_x");
            lua_pushnumber(L, joyY); lua_setglobal(L, "joystick_y");
        }
        callMods(dt);
        for (size_t i = 0; i < ents.size(); i++) {  // per-object update(self, dt)
            if (!ents[i].alive || ents[i].script.empty() || ents[i].broken) continue;
            int id = ents[i].id;
            if (pushEnvFn(id, "update")) {
                lua_pushinteger(L, id); lua_pushnumber(L, dt);
                if (lua_pcall(L, 2, 0, 0) != LUA_OK) luaErr("update", id);
            }
        }
    }
    if (screen == SCREEN_PROJECT && mode == MODE_PLAY && physicsOn && L) stepPhysics(dt);
    touchDX = touchDY = 0;
}

// ---- UI elements (buttons / labels): drawn with the ImGui draw list in both editor and game ----
void Game::uiRect(const Entity& e, ImVec2& a, ImVec2& b) const {
    float cx = e.pos.x / 100.f * w, cy = e.pos.y / 100.f * h, hw, hh;
    if (e.meshId == KIND_JOYSTICK) hw = hh = e.scale.x / 100.f * h * 0.5f;  // circle: size = % of screen height
    else { hw = e.scale.x / 100.f * w * 0.5f; hh = e.scale.y / 100.f * h * 0.5f; }
    a = ImVec2(cx - hw, cy - hh);
    b = ImVec2(cx + hw, cy + hh);
}
// all = every UI element (editor selection); otherwise only the interactive ones (buttons, joysticks)
int Game::uiAt(float x, float y, bool all) const {
    for (int i = (int)ents.size() - 1; i >= 0; i--) {
        const Entity& e = ents[i];
        if (!e.alive || !isUIKind(e.meshId)) continue;
        if (!all && e.meshId != KIND_BUTTON && e.meshId != KIND_JOYSTICK) continue;
        if (mode == MODE_EDIT && e.hidden) continue;
        ImVec2 a, b; uiRect(e, a, b);
        if (x >= a.x && x <= b.x && y >= a.y && y <= b.y) return e.id;
    }
    return 0;
}

// ---- hierarchy: world = parentWorld * local (matrix product, so scale/rotation of the parent carry over) ----
M4 Game::worldOf(int id, int depth) const {
    const Entity& e = ents[id - 1];
    M4 L = model(e.pos, e.rotY, e.scale);
    if (e.parent > 0 && e.parent <= (int)ents.size() && depth < 32 && !isUIKind(e.meshId)) {
        const Entity& p = ents[e.parent - 1];
        if (p.alive && !isUIKind(p.meshId)) return mul(worldOf(e.parent, depth + 1), L);
    }
    return L;
}
V3 Game::worldPos(const Entity& e) const {
    if (isUIKind(e.meshId)) return e.pos;
    M4 w = worldOf(e.id);
    return {w.m[12], w.m[13], w.m[14]};
}
V3 Game::worldScale(const Entity& e) const {
    M4 w = worldOf(e.id);
    return {std::sqrt(w.m[0] * w.m[0] + w.m[1] * w.m[1] + w.m[2] * w.m[2]),
            std::sqrt(w.m[4] * w.m[4] + w.m[5] * w.m[5] + w.m[6] * w.m[6]),
            std::sqrt(w.m[8] * w.m[8] + w.m[9] * w.m[9] + w.m[10] * w.m[10])};
}
bool Game::setParent(int id, int pid) {
    if (id < 1 || id > (int)ents.size() || !ents[id - 1].alive) return false;
    if (pid != 0) {
        if (pid < 1 || pid > (int)ents.size() || !ents[pid - 1].alive || pid == id) return false;
        for (int c = pid, guard = 0; c != 0 && guard < 64; guard++) {  // refuse loops
            if (c == id) return false;
            c = ents[c - 1].parent;
            if (c < 1 || c > (int)ents.size()) break;
        }
    }
    ents[id - 1].parent = pid;
    return true;
}
void Game::destroyEntity(int id) {  // children go with their parent
    if (id < 1 || id > (int)ents.size() || !ents[id - 1].alive) return;
    ents[id - 1].alive = false;
    for (size_t i = 0; i < ents.size(); i++)
        if (ents[i].alive && ents[i].parent == id) destroyEntity((int)i + 1);
}
int Game::spawnBasic(const char* mesh, V3 p) {
    Entity e;
    e.id = (int)ents.size() + 1; e.alive = true; e.broken = false; e.meshId = 0;
    for (int i = 0; i < KIND_COUNT; i++) if (!strcmp(mesh, MESH_NAMES[i])) e.meshId = i;
    e.name = MESH_NAMES[e.meshId];
    V3 dp, ds, dc; std::string dt;
    kindDefaults(e.meshId, dp, ds, dc, dt);
    e.pos = p; e.scale = ds; e.color = dc; e.text = dt;
    e.rotY = 0; e.spin = 0; e.sway = 0; e.shadow = 0;
    ents.push_back(e);
    return e.id;
}
// Ray vs. the (world-space) box of every 3D object. forPick = editor picking (skips locked / hidden, includes camera/light helpers).
int Game::raycast(V3 eye, V3 d, float& best, bool forPick) const {
    best = 1e30f; int hit = 0;
    for (const auto& e : ents) {
        if (!e.alive || isUIKind(e.meshId)) continue;
        if (isHelperKind(e.meshId) && !forPick) continue;
        if (forPick && (e.locked || (mode == MODE_EDIT && e.hidden))) continue;
        V3 p = worldPos(e), sc = worldScale(e);
        float hx, hz, y0, y1;
        if (isHelperKind(e.meshId)) { hx = hz = 0.45f; y0 = p.y - 0.45f; y1 = p.y + 0.45f; }
        else {
            hx = sc.x * 0.5f; hz = sc.z * 0.5f;
            if (std::fabs(e.rotY) > 1e-3f) hx = hz = 0.5f * std::sqrt(sc.x * sc.x + sc.z * sc.z);
            y0 = p.y + MESH_Y0[e.meshId] * sc.y; y1 = p.y + MESH_Y1[e.meshId] * sc.y;
        }
        float lo[3] = {p.x - hx, y0, p.z - hz}, hi[3] = {p.x + hx, y1, p.z + hz};
        float o[3] = {eye.x, eye.y, eye.z}, dv[3] = {d.x, d.y, d.z};
        float tmin = 0, tmax = 1e30f; bool ok = true;
        for (int i = 0; i < 3 && ok; i++) {
            if (std::fabs(dv[i]) < 1e-8f) { if (o[i] < lo[i] || o[i] > hi[i]) ok = false; }
            else {
                float t0 = (lo[i] - o[i]) / dv[i], t1 = (hi[i] - o[i]) / dv[i];
                if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }
                if (t0 > tmin) tmin = t0;
                if (t1 < tmax) tmax = t1;
                if (tmin > tmax) ok = false;
            }
        }
        if (ok && tmin < best) { best = tmin; hit = e.id; }
    }
    return hit;
}

// ---- addons (mods): project.json "requires", loading, per-frame callbacks ----
void Game::loadRequires() {
    reqs.clear();
    std::string s = readSource("project.json");
    size_t k = s.find("\"requires\"");
    if (k == std::string::npos) return;
    size_t b = s.find('[', k), e = s.find(']', b == std::string::npos ? k : b);
    if (b == std::string::npos || e == std::string::npos) return;
    for (size_t i = b; i < e;) {
        size_t q1 = s.find('"', i);
        if (q1 == std::string::npos || q1 >= e) break;
        size_t q2 = s.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > e) break;
        reqs.push_back(s.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
}
std::string Game::checkRequires() {  // returns the names of required addons that are missing or disabled
    std::string miss;
#ifdef GAME_MODE
    for (const auto& id : reqs)
        if (id != "3sics" && readAsset(("addons/" + id + ".lua").c_str()).empty()) miss += id + " ";  // 3sics is built in
#else
    addons = scanAddons();
    for (const auto& id : reqs) {
        bool ok = false;
        for (const auto& a : addons) if (a.id == id && a.enabled && a.supported) { ok = true; break; }
        if (!ok) miss += id + " ";
    }
#endif
    return miss;
}
void Game::modErr(const std::string& id) {
    std::string msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : "error";
    status = "Mod '" + id + "': " + msg;
    logCrash(status);
    lua_pop(L, 1);
}
void Game::loadLuaAddon(const std::string& id, const std::string& src) {
    setMod(id.c_str());
    if (luaL_loadbufferx(L, src.data(), src.size(), id.c_str(), "t") != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
        modErr(id);
        setMod("");
        return;
    }
    if (lua_istable(L, -1)) {  // an addon may return { start=function() end, update=function(dt) end }
        lua_pushstring(L, id.c_str());
        lua_setfield(L, -2, "__id");
        lua_getfield(L, LUA_REGISTRYINDEX, "mods");
        lua_pushvalue(L, -2);
        lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "start");
        if (lua_isfunction(L, -1)) { if (lua_pcall(L, 0, 0, 0) != LUA_OK) modErr(id); }
        else lua_pop(L, 1);
    }
    lua_pop(L, 1);
    setMod("");
}
// ---- native addon API (see gine_addon.h) ----
static void apiLog(const char* s) { LOG("[mod %s] %s", g_currentMod, s); }
static int apiSpawn(const char* mesh, float x, float y, float z) { return G->spawnBasic(mesh, {x, y, z}); }
static void apiGetPos(int id, float* x, float* y, float* z) {
    if (id < 1 || id > (int)G->ents.size()) return;
    const Entity& e = G->ents[id - 1];
    if (x) *x = e.pos.x;
    if (y) *y = e.pos.y;
    if (z) *z = e.pos.z;
}
static void apiSetPos(int id, float x, float y, float z) {
    if (id < 1 || id > (int)G->ents.size()) return;
    G->ents[id - 1].pos = {x, y, z};
}
static int apiFind(const char* n) {
    for (const auto& e : G->ents) if (e.alive && e.name == n) return e.id;
    return 0;
}
static void apiOnUpdate(void (*fn)(float)) { if (fn) G->nativeCbs.push_back({g_currentMod, fn}); }

void Game::loadNativeAddon(const Addon& a) {
    setMod(a.id.c_str());
    // shared storage cannot be executed from: copy the library into the app's private folder first
    std::string path = a.dir + a.file;
    if (!internalDir.empty()) {
        std::string data;
        mkdirs(internalDir + "/addons_native/");
        std::string copy = internalDir + "/addons_native/" + a.file;
        if (readFile(path, data) && writeFile(copy, data)) path = copy;
    }
    void* h = dlopen(path.c_str(), RTLD_NOW);
    if (!h) { status = "Mod '" + a.id + "': " + std::string(dlerror()); logCrash(status); setMod(""); return; }
    GineAddonInit init = (GineAddonInit)dlsym(h, "gine_addon_init");
    if (!init) { status = "Mod '" + a.id + "': gine_addon_init not found"; logCrash(status); dlclose(h); setMod(""); return; }
    static GineAPI api = {1, apiLog, apiSpawn, apiGetPos, apiSetPos, apiFind, apiOnUpdate};
    init(&api);
    setMod("");
}
void Game::loadAddons() {
    nativeCbs.clear();
#ifdef GAME_MODE
    physicsOn = true;  // 3sics is part of the engine (C++); with no bodies it costs nothing
    for (const auto& id : reqs) {
        if (id == "3sics") continue;
        std::string src = readAsset(("addons/" + id + ".lua").c_str());
        if (!src.empty()) loadLuaAddon(id, src);
    }
#else
    addons = scanAddons();
    for (const auto& a : addons) {
        if (a.builtin) { if (a.id == "3sics") physicsOn = a.enabled; continue; }   // built-in 3sics: toggled from the Mods menu
        if (!a.enabled || !a.supported) continue;                                  // ".disable" files are ignored
        if (a.ext == ".lua") { std::string src; if (readFile(a.dir + a.file, src)) loadLuaAddon(a.id, src); }
        else loadNativeAddon(a);
    }
#endif
}
void Game::callMods(float dt) {
    if (!L) return;
    lua_getfield(L, LUA_REGISTRYINDEX, "mods");
    int n = (int)lua_rawlen(L, -1);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);
        lua_getfield(L, -1, "__id");
        setMod(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
        lua_pop(L, 1);
        lua_getfield(L, -1, "update");
        if (lua_isfunction(L, -1)) {
            lua_pushnumber(L, dt);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                modErr(g_currentMod);
                lua_pushnil(L);
                lua_setfield(L, -2, "update");  // a failing mod stops updating, the game keeps running
            }
        } else lua_pop(L, 1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    for (auto& c : nativeCbs) { setMod(c.id.c_str()); c.fn(dt); }
    setMod("");
}

void Game::drawEntityUI() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    for (const auto& e : ents) {
        if (!e.alive || !isUIKind(e.meshId)) continue;
        if (mode == MODE_EDIT && e.hidden) continue;
        ImVec2 a, b; uiRect(e, a, b);
        float bh = b.y - a.y;
        int cr = (int)(e.color.x * 255), cg = (int)(e.color.y * 255), cb = (int)(e.color.z * 255);
        if (e.meshId == KIND_BUTTON) {
            bool pressed = mode == MODE_PLAY && uiDownId == e.id && touchDown;
            float k = pressed ? 0.7f : 1.f;
            dl->AddRectFilled(a, b, IM_COL32((int)(cr * k), (int)(cg * k), (int)(cb * k), 255), bh * 0.2f);
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 90), bh * 0.2f, 0, 2.f);
            float fp = bh * 0.45f;
            ImVec2 ts = font->CalcTextSizeA(fp, 100000.f, 0.f, e.text.c_str());
            dl->AddText(font, fp, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), e.text.c_str());
        } else if (e.meshId == KIND_LABEL) {
            float fp = bh;
            ImVec2 ts = font->CalcTextSizeA(fp, 100000.f, 0.f, e.text.c_str());
            dl->AddText(font, fp, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), IM_COL32(cr, cg, cb, 255), e.text.c_str());
        } else if (e.meshId == KIND_JOYSTICK) {
            ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            float r = bh * 0.5f;
            dl->AddCircleFilled(c, r, IM_COL32(cr, cg, cb, 60), 48);
            dl->AddCircle(c, r, IM_COL32(255, 255, 255, 150), 48, 3.f);
            ImVec2 kn = c;
            if (mode == MODE_PLAY && joyActive && uiDownId == e.id) { kn.x += joyX * r * 0.6f; kn.y -= joyY * r * 0.6f; }
            dl->AddCircleFilled(kn, r * 0.35f, IM_COL32(255, 255, 255, 190), 32);
        } else {  // image from the depot
            GLuint tex = getTexture(e.img);
            if (tex) dl->AddImage((ImTextureID)(intptr_t)tex, a, b);
            else dl->AddRect(a, b, IM_COL32(255, 255, 255, 120), 0.f, 0, 2.f);
        }
        if (mode == MODE_EDIT && e.id == selected) dl->AddRect(a, b, IM_COL32(255, 220, 60, 255), 6.f, 0, 4.f);
    }
}

void Game::render() {
    const V3 fog = {0.72f, 0.82f, 0.92f};
    glViewport(0, 0, w, h);
    glClearColor(fog.x, fog.y, fog.z, 1);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glUseProgram(skyProg);
    glBindVertexArray(emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (screen != SCREEN_PROJECT) return;

    glEnable(GL_DEPTH_TEST); glEnable(GL_CULL_FACE);
    V3 eye = target + V3{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)} * dist;
    V3 look = target;
    float fov = 0.9f;
    if (mode == MODE_PLAY) {  // a Camera object in the scene takes over while playing
        V3 ce, ct; float cf;
        if (sceneCamera(ce, ct, cf)) { eye = ce; look = ct; fov = cf; }
    }
    M4 vp = mul(perspective(fov, (float)w / h, 0.3f, 200.f), lookAt(eye, look, {0, 1, 0}));
    lastVP = vp; lastEye = eye;

    float lp[12] = {0}, lc[12] = {0}, lr[4] = {1, 1, 1, 1};
    int nl = 0;
    for (const auto& e : ents) {
        if (nl >= 4) break;
        if (!e.alive || e.meshId != KIND_LIGHT) continue;
        if (mode == MODE_EDIT && e.hidden) continue;
        V3 wp = worldPos(e);
        lp[nl * 3] = wp.x; lp[nl * 3 + 1] = wp.y; lp[nl * 3 + 2] = wp.z;
        lc[nl * 3] = e.color.x * e.intensity; lc[nl * 3 + 1] = e.color.y * e.intensity; lc[nl * 3 + 2] = e.color.z * e.intensity;
        lr[nl] = e.range > 0.1f ? e.range : 0.1f;
        nl++;
    }
    forwardMode = nl > 0 || customProg != 0;  // Unlit until the first light or custom shader appears

    GLuint P = customProg ? customProg : prog;
    glUseProgram(P);
    glUniformMatrix4fv(glGetUniformLocation(P, "uVP"), 1, GL_FALSE, vp.m);
    glUniform3f(glGetUniformLocation(P, "uCam"), eye.x, eye.y, eye.z);
    glUniform3f(glGetUniformLocation(P, "uSun"), 0.5f, 0.8f, 0.3f);
    glUniform3f(glGetUniformLocation(P, "uFog"), fog.x, fog.y, fog.z);
    glUniform1f(glGetUniformLocation(P, "uTime"), t);
    glUniform1i(glGetUniformLocation(P, "uForward"), forwardMode ? 1 : 0);
    glUniform1i(glGetUniformLocation(P, "uLN"), nl);
    glUniform3fv(glGetUniformLocation(P, "uLP"), 4, lp);
    glUniform3fv(glGetUniformLocation(P, "uLC"), 4, lc);
    glUniform1fv(glGetUniformLocation(P, "uLR"), 4, lr);
    GLint uModel = glGetUniformLocation(P, "uModel"), uColor = glGetUniformLocation(P, "uColor");
    GLint uSway = glGetUniformLocation(P, "uSway"), uAlpha = glGetUniformLocation(P, "uAlpha");
    GLint uMat = glGetUniformLocation(P, "uMat");

    for (int pass = 0; pass < 2; pass++) {  // 0: solid, 1: blob shadows
        if (pass == 1) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE); }
        glUniform1f(uAlpha, pass == 1 ? 0.35f : 1.0f);
        for (const auto& e : ents) {
            if (!e.alive || isUIKind(e.meshId)) continue;
            if (mode == MODE_EDIT && e.hidden) continue;
            bool helper = isHelperKind(e.meshId);
            if (helper && (mode != MODE_EDIT || pass == 1)) continue;  // camera / light markers: editor only
            if (pass == 0) {
                V3 c = e.color;
                if (mode == MODE_EDIT && e.id == selected) {  // selection highlight
                    float k = 0.30f + 0.15f * std::sin(t * 6.f);
                    c = c * (1.f - k) + V3{1, 1, 1} * k;
                }
                M4 m;
                const Mesh* mesh;
                float mat[4];
                if (helper) {
                    m = model(worldPos(e), 0, {0.8f, 0.8f, 0.8f});
                    mesh = e.meshId == KIND_CAMERA ? &meshes[5] : &meshes[4];
                    materialParams("glow", mat);
                } else {
                    m = worldOf(e.id);
                    mesh = &meshOf(e.meshId);
                    matParams(e.material, mat);
                }
                glUniform3f(uColor, c.x, c.y, c.z);
                glUniform1f(uSway, helper ? 0.f : e.sway);
                glUniform4f(uMat, mat[0], mat[1], mat[2], mat[3]);
                glUniformMatrix4fv(uModel, 1, GL_FALSE, m.m);
                mesh->draw();
            } else if (e.shadow > 0) {
                V3 wp = worldPos(e);
                M4 m = model({wp.x, 0.02f, wp.z}, 0, {e.shadow, 1, e.shadow});
                glUniform3f(uColor, 0.05f, 0.12f, 0.08f);
                glUniform1f(uSway, 0);
                glUniformMatrix4fv(uModel, 1, GL_FALSE, m.m);
                meshes[2].draw();
            }
        }
    }
    glDisable(GL_BLEND); glDepthMask(GL_TRUE);
}

void Game::drawUI() {
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = customFont ? h / 1080.f : h / 520.f;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
    if (screen == SCREEN_PROJECT) drawEntityUI();
#ifndef GAME_MODE
    drawEditorUI();
#else
    if (!blockMsg.empty()) {
        std::string m = "Cannot start: missing mods: " + blockMsg;
        ImGui::GetForegroundDrawList()->AddText(ImGui::GetFont(), h * 0.05f, ImVec2(20, h * 0.4f), IM_COL32(255, 90, 90, 255), m.c_str());
    } else if (!status.empty() && mode == MODE_PLAY)  // show script errors on screen
        ImGui::GetForegroundDrawList()->AddText(ImVec2(10, 10), IM_COL32(255, 90, 90, 255), status.c_str());
#endif
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

#ifndef GAME_MODE
// ---------------------------------------------------------------- editor
static const char* DEFAULT_SCENE =
    "-- 3gine scene. Generated by the editor (Save). Safe to hand-edit.\n"
    "engine.camera{ yaw=0.6, pitch=0.45, dist=16, target={0,1,0} }\n"
    "engine.spawn{ name=\"Ground\", mesh=\"cube\", pos={0,-0.5,0}, scale={60,1,60}, color={0.42,0.66,0.38}, rot=0, spin=0, sway=0, shadow=0 }\n"
    "engine.spawn{ name=\"Cube\", mesh=\"cube\", pos={0,0.5,0}, scale={1,1,1}, color={0.95,0.75,0.25}, rot=0, spin=0, sway=0, shadow=1.4 }\n";



// ---- zip archives (stored, no compression): used for .3ne backups and the "Prepare for APK" package ----
static uint32_t crc32buf(const std::string& d) {
    static uint32_t T[256]; static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1); T[i] = c; }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : d) c = T[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void put16(std::string& s, unsigned v) { s.push_back((char)(v & 0xFF)); s.push_back((char)((v >> 8) & 0xFF)); }
static void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back((char)((v >> (8 * i)) & 0xFF)); }
static uint32_t get16(const std::string& s, size_t o) { return (uint32_t)(unsigned char)s[o] | ((uint32_t)(unsigned char)s[o + 1] << 8); }
static uint32_t get32(const std::string& s, size_t o) { return get16(s, o) | (get16(s, o + 2) << 16); }
struct ZipEntry { std::string name, data; };
static bool writeZip(const std::string& path, const std::vector<ZipEntry>& files) {
    std::string out, cd;
    for (const auto& f : files) {
        uint32_t crc = crc32buf(f.data), sz = (uint32_t)f.data.size(), off = (uint32_t)out.size();
        put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0x21);
        put32(out, crc); put32(out, sz); put32(out, sz); put16(out, (unsigned)f.name.size()); put16(out, 0);
        out += f.name; out += f.data;
        put32(cd, 0x02014b50); put16(cd, 20); put16(cd, 20); put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0x21);
        put32(cd, crc); put32(cd, sz); put32(cd, sz); put16(cd, (unsigned)f.name.size());
        put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, off);
        cd += f.name;
    }
    uint32_t cdOff = (uint32_t)out.size();
    out += cd;
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, (unsigned)files.size()); put16(out, (unsigned)files.size());
    put32(out, (uint32_t)cd.size()); put32(out, cdOff); put16(out, 0);
    return writeFile(path, out);
}
static bool readZip(const std::string& path, std::vector<ZipEntry>& out) {  // reads archives written by writeZip (stored entries)
    std::string z;
    if (!readFile(path, z) || z.size() < 22) return false;
    size_t eocd = std::string::npos;
    for (size_t i = z.size() - 22;; i--) {
        if (get32(z, i) == 0x06054b50) { eocd = i; break; }
        if (i == 0 || z.size() - i > 70000) break;
    }
    if (eocd == std::string::npos) return false;
    uint32_t count = get16(z, eocd + 10);
    size_t p = get32(z, eocd + 16);
    for (uint32_t k = 0; k < count; k++) {
        if (p + 46 > z.size() || get32(z, p) != 0x02014b50) return false;
        uint32_t method = get16(z, p + 10), csize = get32(z, p + 20), nlen = get16(z, p + 28);
        uint32_t elen = get16(z, p + 30), clen = get16(z, p + 32), loff = get32(z, p + 42);
        std::string name = z.substr(p + 46, nlen);
        p += 46 + nlen + elen + clen;
        if (method != 0) return false;
        if ((size_t)loff + 30 > z.size() || get32(z, loff) != 0x04034b50) return false;
        size_t ds = (size_t)loff + 30 + get16(z, loff + 26) + get16(z, loff + 28);
        if (ds + csize > z.size()) return false;
        if (name.empty() || name.back() == '/') continue;
        out.push_back({name, z.substr(ds, csize)});
    }
    return true;
}
static void listFilesRec(const std::string& dir, const std::string& rel, std::vector<std::string>& out, int depth) {
    for (const auto& f : listDir(dir + rel, false)) out.push_back(rel + f);
    if (depth >= 8) return;
    for (const auto& d : listDir(dir + rel, true)) listFilesRec(dir, rel + d + "/", out, depth + 1);
}
static std::string lowerExt(const std::string& n) {
    size_t dot = n.rfind('.');
    if (dot == std::string::npos) return "";
    std::string e = n.substr(dot + 1);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    return e;
}
static std::string baseName(const std::string& path) {
    size_t sl = path.rfind('/');
    return sl == std::string::npos ? path : path.substr(sl + 1);
}
static std::string lab(const char* text, const char* id) { return std::string(text) + "###" + id; }
static void openBrowser(Game& g, const char* kind) {
    g.browseKind = kind;
    if (g.browseDir.empty()) g.browseDir = "/sdcard/Download/";
    if (listDir(g.browseDir, true).empty() && listDir(g.browseDir, false).empty()) g.browseDir = "/sdcard/";
    g.showBrowser = true;
}

void Game::loadConfig() {
    std::string cfg;
    if (readFile(settingsPath, cfg)) {
        size_t k = cfg.find("\"language\"");
        if (k != std::string::npos) {
            size_t c = cfg.find(':', k), q1 = c == std::string::npos ? c : cfg.find('"', c);
            size_t q2 = q1 == std::string::npos ? q1 : cfg.find('"', q1 + 1);
            if (q2 != std::string::npos) {
                std::string code = cfg.substr(q1 + 1, q2 - q1 - 1);
                for (int i = 0; i < 6; i++) if (code == LANG_CODES[i]) lang = i;
            }
        }
        screen = SCREEN_MENU;
    } else { lang = 0; screen = SCREEN_WELCOME; }  // first launch: ask for the language once
}
void Game::saveConfig() {
    size_t sl = settingsPath.rfind('/');
    if (sl != std::string::npos) mkdirs(settingsPath.substr(0, sl + 1));
    writeFile(settingsPath, std::string("{\n  \"language\": \"") + LANG_CODES[lang] + "\"\n}\n");
}
void Game::drawWelcome(ImGuiWindowFlags F) {
    float W = (float)w, H = (float)h;
    ImGui::SetNextWindowPos(ImVec2(0.2f * W, 0.08f * H));
    ImGui::SetNextWindowSize(ImVec2(0.6f * W, 0.84f * H));
    ImGui::Begin("Welcome", nullptr, F);
    ImGui::SetWindowFontScale(2.2f);
    ImGui::TextUnformatted("Welcome");
    ImGui::SetWindowFontScale(1.f);
    ImGui::Separator();
    ImGui::TextUnformatted("Choose your language:");
    for (int i = 0; i < 6; i++) if (ImGui::Selectable(LANG_NAMES[i], lang == i)) lang = i;
    ImGui::Separator();
    if (ImGui::Button("OK", ImVec2(-1, 0))) { saveConfig(); screen = SCREEN_MENU; }
    ImGui::End();
}

// ---- depot (asset store): <files>/img and <files>/sound, plus a simple file browser ----
void Game::browsePick(const std::string& path) {
    if (browseKind == "3ne") { importBackup(path); showBrowser = false; return; }
    std::string err;
    status = depot.addFile(path, browseKind, &err) ? "Added: " + baseName(path) : "Could not add: " + err;
    showBrowser = false;
}
void Game::drawBrowser(ImGuiWindowFlags F) {
    float W = (float)w, H = (float)h;
    ImGui::SetNextWindowPos(ImVec2(0.05f * W, 0.05f * H));
    ImGui::SetNextWindowSize(ImVec2(0.9f * W, 0.9f * H));
    ImGui::Begin("File browser###browser", nullptr, F);
    if (ImGui::Button(tr(S_CLOSE))) showBrowser = false;
    ImGui::SameLine();
    if (ImGui::Button("..") && browseDir.size() > 1) {
        size_t sl = browseDir.rfind('/', browseDir.size() - 2);
        browseDir = sl == std::string::npos ? "/" : browseDir.substr(0, sl + 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("/sdcard")) browseDir = "/sdcard/";
    ImGui::SameLine();
    if (ImGui::Button("Download")) browseDir = "/sdcard/Download/";
    ImGui::TextWrapped("%s", browseDir.c_str());
    ImGui::Separator();
    if (ImGui::BeginChild("blist", ImVec2(0, 0), 0)) {
        std::string next;
        for (const auto& d : listDir(browseDir, true)) {
            std::string l = "[" + d + "]###d_" + d;
            if (ImGui::Selectable(l.c_str())) next = browseDir + d + "/";
        }
        std::string pick;
        for (const auto& f : listDir(browseDir, false)) {
            std::string e = lowerExt(f);
            bool ok = (browseKind == "img" && (e == "png" || e == "jpg" || e == "jpeg")) ||
                      (browseKind == "sound" && (e == "mp3" || e == "ogg" || e == "wav")) ||
                      (browseKind == "material" && e == "mat") ||
                      (browseKind == "3ne" && e == "3ne");
            if (ok && ImGui::Selectable((f + "###f_" + f).c_str())) pick = browseDir + f;
        }
        if (!next.empty()) browseDir = next;
        if (!pick.empty()) browsePick(pick);
    }
    ImGui::EndChild();
    ImGui::End();
}
void Game::drawDepot(ImGuiWindowFlags F) {
    float W = (float)w, H = (float)h;
    ImGui::SetNextWindowPos(ImVec2(0.08f * W, 0.08f * H));
    ImGui::SetNextWindowSize(ImVec2(0.84f * W, 0.84f * H));
    ImGui::Begin(lab(tr(S_DEPOT), "depot").c_str(), nullptr, F);
    if (ImGui::Button(tr(S_CLOSE))) showDepot = false;
    ImGui::TextWrapped("%s", depotDir.c_str());
    ImGui::TextWrapped("img/ (png, jpg), sound/ (mp3, ogg, wav), material/ (.mat). Files dropped in with a file manager show up here too.");
    int uid = 0;
    for (const char* kind : {"img", "sound", "material"}) {
        ImGui::Separator();
        ImGui::TextUnformatted(kind);
        std::vector<std::string> names = depot.list(kind);
        for (const auto& n : names) {
            ImGui::PushID(uid++);
            ImGui::TextUnformatted(n.c_str());
            if (!strcmp(kind, "sound")) { ImGui::SameLine(); if (ImGui::SmallButton("Play")) playSoundFile(depot.pathOf("sound", n).string(), false); }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(S_DELETE))) depot.remove(kind, n);
            ImGui::PopID();
        }
        if (ImGui::Button(lab(tr(S_ADD_FILE), (std::string("add_") + kind).c_str()).c_str())) openBrowser(*this, kind);
    }
    ImGui::Separator();
    if (ImGui::Button("Stop sounds")) stopAllSounds();
    ImGui::End();
}

// ---- .3ne backup, import and "Prepare for APK" ----
bool Game::exportBackup(std::string& outPath) {
    if (projectDir.empty()) return false;
    saveProject();
    std::vector<ZipEntry> files;
    std::vector<std::string> rels;
    listFilesRec(projectDir, "", rels, 0);
    for (const auto& rel : rels) { std::string d; if (readFile(projectDir + rel, d)) files.push_back({"project/" + rel, d}); }
    for (const char* kind : {"img", "sound", "material"})
        for (const auto& f : listDir(depotDir + kind + "/", false)) { std::string d; if (readFile(depotDir + kind + "/" + f, d)) files.push_back({std::string("depot/") + kind + "/" + f, d}); }
    std::string dir = "/sdcard/Download/3Gine-project/" + projectName + "/";
    mkdirs(dir);
    outPath = dir + projectName + ".3ne";
    return writeZip(outPath, files);
}
bool Game::importBackup(const std::string& path) {
    std::vector<ZipEntry> files;
    if (!readZip(path, files)) { status = "Not a valid .3ne file"; return false; }
    std::string name = baseName(path);
    if (name.size() > 4 && lowerExt(name) == "3ne") name = name.substr(0, name.size() - 4);
    name = cleanName(name);
    if (name.empty()) name = "imported";
    std::string fin = name;
    for (int n = 2; fileExists(baseDir + fin + "/scene.lua"); n++) fin = name + "_" + std::to_string(n);
    for (const auto& f : files) {
        if (f.name.find("..") != std::string::npos || f.name[0] == '/') continue;
        std::string dst;
        if (f.name.compare(0, 8, "project/") == 0) dst = baseDir + fin + "/" + f.name.substr(8);
        else if (f.name.compare(0, 6, "depot/") == 0 && !depotDir.empty()) dst = depotDir + f.name.substr(6);
        else continue;
        mkdirs(dst.substr(0, dst.rfind('/') + 1));
        writeFile(dst, f.data);
    }
    refreshProjects();
    status = "Imported: " + fin;
    return true;
}
// The editor cannot compile an APK on the phone, so it packs everything needed to build one: the engine source
// (bundled in this app), the project and the depot files. Output: Download/3Gine-project/<name>/<name>.zip
bool Game::prepareApk(std::string& outPath) {
    std::string man = readAsset("engine_src/manifest.txt");
    if (man.empty()) { status = "Engine sources are not bundled in this APK (build it from the repo with Gradle)."; return false; }
    std::vector<ZipEntry> files;
    size_t pos = 0;
    while (pos < man.size()) {
        size_t nl = man.find('\n', pos);
        if (nl == std::string::npos) nl = man.size();
        std::string line = man.substr(pos, nl - pos);
        pos = nl + 1;
        size_t bar = line.find('|');
        if (bar == std::string::npos) continue;
        std::string data = readAsset(("engine_src/" + line.substr(0, bar)).c_str());
        files.push_back({line.substr(bar + 1), data});
    }
    std::vector<std::string> rels;
    listFilesRec(projectDir, "", rels, 0);
    for (const auto& rel : rels) { std::string d; if (readFile(projectDir + rel, d)) files.push_back({"game/" + projectName + "/" + rel, d}); }
    for (const char* kind : {"img", "sound", "material"})
        for (const auto& f : listDir(depotDir + kind + "/", false)) {
            std::string d;
            if (readFile(depotDir + kind + "/" + f, d)) files.push_back({"game/" + projectName + "/" + kind + "/" + f, d});
        }
    files.push_back({"game/startup.txt", projectName + "\n"});
    files.push_back({"README_BUILD.txt",
        "3gine - project prepared for APK\n\n"
        "1. Extract this zip and upload everything to a GitHub repository (Termux: git add -A, commit, push).\n"
        "2. GitHub Actions builds both APKs. Download the artifact '3gine-game-apk' - that is your game.\n"
        "3. Or open the folder in Android Studio and run: gradle assembleGameDebug\n\n"
        "The game inside is the project in game/" + projectName + "/ (game/startup.txt names it).\n"});
    std::string dir = "/sdcard/Download/3Gine-project/" + projectName + "/";
    mkdirs(dir);
    outPath = dir + projectName + ".zip";
    return writeZip(outPath, files);
}

// ---- move gizmo with snap ----
bool Game::project(V3 p, ImVec2& out) const {
    const float* m = lastVP.m;
    float x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
    float y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
    float wv = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (wv < 0.05f) return false;
    out = ImVec2((x / wv * 0.5f + 0.5f) * w, (1.f - (y / wv * 0.5f + 0.5f)) * h);
    return true;
}
bool Game::gizmoGeom(V3& c, ImVec2 sc[4]) {
    if (mode != MODE_EDIT || screen != SCREEN_PROJECT || codeOpen || selected < 1 || selected > (int)ents.size()) return false;
    const Entity& e = ents[selected - 1];
    if (!e.alive || isUIKind(e.meshId) || e.locked) return false;
    c = worldPos(e);
    V3 dd = c - lastEye;
    gizmoLen = std::sqrt(dot(dd, dd)) * 0.15f;
    if (gizmoLen < 0.2f) gizmoLen = 0.2f;
    V3 tips[3] = {c + V3{gizmoLen, 0, 0}, c + V3{0, gizmoLen, 0}, c + V3{0, 0, gizmoLen}};
    if (!project(c, sc[0])) return false;
    for (int i = 0; i < 3; i++) if (!project(tips[i], sc[i + 1])) return false;
    return true;
}
bool Game::gizmoBegin(float x, float y) {
    V3 c; ImVec2 sc[4];
    if (!gizmoGeom(c, sc)) return false;
    float best = 0.045f * h; int ax = 0;
    for (int i = 1; i <= 3; i++) {
        float dx = sc[i].x - sc[0].x, dy = sc[i].y - sc[0].y, l2 = dx * dx + dy * dy;
        float t = l2 > 0 ? ((x - sc[0].x) * dx + (y - sc[0].y) * dy) / l2 : 0.f;
        t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
        float px = sc[0].x + t * dx - x, py = sc[0].y + t * dy - y;
        float dist = std::sqrt(px * px + py * py);
        if (dist < best) { best = dist; ax = i; }
    }
    if (!ax) return false;
    float dx = sc[ax].x - sc[0].x, dy = sc[ax].y - sc[0].y, pl = std::sqrt(dx * dx + dy * dy);
    if (pl < 4.f) return false;
    gizmoAxis = ax;
    gizmoStart = ents[selected - 1].pos;
    gizmoAccum = 0;
    gizmoPxPerUnit = pl / gizmoLen;
    gizmoDir = ImVec2(dx / pl, dy / pl);
    return true;
}
void Game::gizmoMove(float dx, float dy) {
    if (!gizmoAxis || selected < 1 || selected > (int)ents.size()) return;
    gizmoAccum += (dx * gizmoDir.x + dy * gizmoDir.y) / gizmoPxPerUnit;
    float v = (gizmoAxis == 1 ? gizmoStart.x : (gizmoAxis == 2 ? gizmoStart.y : gizmoStart.z)) + gizmoAccum;
    float sv = snapVal();
    if (sv > 0.f) v = std::round(v / sv) * sv;  // snap to the 0.5 / 1.0 grid
    Entity& e = ents[selected - 1];
    if (gizmoAxis == 1) e.pos.x = v; else if (gizmoAxis == 2) e.pos.y = v; else e.pos.z = v;
}
void Game::drawGizmo() {
    V3 c; ImVec2 sc[4];
    if (!gizmoGeom(c, sc)) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float u = h / 1080.f;
    ImU32 cols[3] = {IM_COL32(255, 70, 70, 255), IM_COL32(80, 220, 90, 255), IM_COL32(80, 140, 255, 255)};
    const char* names[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; i++) {
        bool act = gizmoAxis == i + 1;
        dl->AddLine(sc[0], sc[i + 1], cols[i], (act ? 9.f : 6.f) * u);
        dl->AddCircleFilled(sc[i + 1], (act ? 24.f : 17.f) * u, cols[i], 20);
        dl->AddText(ImVec2(sc[i + 1].x + 16.f * u, sc[i + 1].y - 14.f * u), IM_COL32(255, 255, 255, 255), names[i]);
    }
}

void Game::setupBaseDir() {
    std::string cands[2] = {"/sdcard/Documents/3gine/", fallbackDir};
    for (const auto& c : cands) {
        if (c.empty()) continue;
        mkdirs(c + "game/");
        std::string probe = c + "game/.probe";
        if (writeFile(probe, "x")) {
            remove(probe.c_str());
            rootDir = c; baseDir = c + "game/"; addonsDir = c + "addons/";
            mkdirs(addonsDir);
            // older versions installed a 3sics.lua marker: physics is built into the engine now, remove that file
            for (const char* old : {"3sics.lua", "3sics.lua.disable"}) {
                std::string txt;
                if (readFile(addonsDir + old, txt) && txt.compare(0, 8, "-- 3sics") == 0) remove((addonsDir + old).c_str());
            }
            return;
        }
    }
    status = "No writable folder. Give 3gine Editor 'All files access' in Android settings, then restart.";
}
void Game::refreshProjects() {
    projects.clear();
    if (baseDir.empty()) return;
    for (const auto& d : listDir(baseDir, true))
        if (fileExists(baseDir + d + "/scene.lua")) projects.push_back(d);
    if (menuSel >= (int)projects.size()) menuSel = -1;
}
bool Game::createProject(const std::string& rawName) {
    std::string name = cleanName(rawName);
    if (baseDir.empty()) { status = "No writable folder (see settings)."; return false; }
    if (name.empty()) { status = "Type a project name (letters, digits, _ or -)."; return false; }
    if (fileExists(baseDir + name + "/scene.lua")) { status = "A project named '" + name + "' already exists."; return false; }
    mkdirs(baseDir + name + "/scripts/");
    if (!writeFile(baseDir + name + "/scene.lua", DEFAULT_SCENE)) { status = "Could not write " + baseDir + name; return false; }
    writeFile(baseDir + name + "/project.json", "{\n  \"name\": \"" + name + "\",\n  \"requires\": []\n}\n");
    return openProject(name);
}
bool Game::openProject(const std::string& name) {
    projectName = name;
    projectDir = baseDir + name + "/";
    codeOpen = false;
    showAddons = false;
    status.clear();
    sceneName = "main";
    loadRequires();
    addons = scanAddons();
    loadScene();
    screen = SCREEN_PROJECT;
    return true;
}
bool Game::saveProject() {
    if (projectDir.empty()) return false;
    mkdirs(projectDir + "scripts/");
    mkdirs(projectDir + "scenes/");
    std::vector<int> remap(ents.size() + 1, 0);  // ids are re-numbered in file order; parents / targets follow
    int n = 0;
    for (const auto& e : ents) if (e.alive) remap[e.id] = ++n;
    char buf[2048];
    std::string out = "-- 3gine scene. Generated by the editor (Save). Safe to hand-edit.\n";
    snprintf(buf, sizeof buf, "engine.camera{ yaw=%.3f, pitch=%.3f, dist=%.3f, target={%.3f,%.3f,%.3f} }\n",
             yaw, pitch, dist, target.x, target.y, target.z);
    out += buf;
    for (const auto& e : ents) {
        if (!e.alive) continue;
        int par = (e.parent > 0 && e.parent <= (int)ents.size()) ? remap[e.parent] : 0;
        int lk = (e.lookId > 0 && e.lookId <= (int)ents.size()) ? remap[e.lookId] : 0;
        snprintf(buf, sizeof buf,
                 "engine.spawn{ name=\"%s\", mesh=\"%s\", pos={%.3f,%.3f,%.3f}, scale={%.3f,%.3f,%.3f}, "
                 "color={%.3f,%.3f,%.3f}, rot=%.3f, spin=%.3f, sway=%.3f, shadow=%.3f, text=\"%s\", script=\"%s\", "
                 "parent=%d, folder=\"%s\", hidden=%s, locked=%s, intensity=%.3f, range=%.3f, fov=%.3f, "
                 "material=\"%s\", img=\"%s\", look={%.3f,%.3f,%.3f}, lookid=%d }\n",
                 esc(e.name).c_str(), MESH_NAMES[e.meshId], e.pos.x, e.pos.y, e.pos.z, e.scale.x, e.scale.y, e.scale.z,
                 e.color.x, e.color.y, e.color.z, e.rotY, e.spin, e.sway, e.shadow, esc(e.text).c_str(), esc(e.script).c_str(),
                 par, esc(e.folder).c_str(), e.hidden ? "true" : "false", e.locked ? "true" : "false",
                 e.intensity, e.range, e.fov, esc(e.material).c_str(), esc(e.img).c_str(), e.look.x, e.look.y, e.look.z, lk);
        out += buf;
    }
    if (!writeFile(projectDir + sceneFile(sceneName), out)) { status = "Save failed: " + projectDir; return false; }
    saveRequires();
    status = "Saved";
    return true;
}
void Game::saveRequires() {
    std::string j = "{\n  \"name\": \"" + esc(projectName) + "\",\n  \"requires\": [";
    for (size_t i = 0; i < reqs.size(); i++) j += std::string(i ? ", " : "") + "\"" + esc(reqs[i]) + "\"";
    j += "]\n}\n";
    writeFile(projectDir + "project.json", j);
}
std::vector<std::string> Game::listScenes() const {
    std::vector<std::string> out = {"main"};
    for (const auto& f : listDir(projectDir + "scenes/", false))
        if (f.size() > 4 && f.compare(f.size() - 4, 4, ".lua") == 0) out.push_back(f.substr(0, f.size() - 4));
    return out;
}
std::vector<Addon> Game::scanAddons() const {
    std::vector<Addon> out;
    {   // 3sics physics is built into the engine; the switch is a flag file in the app's settings folder
        Addon b; b.id = "3sics"; b.file = "(built-in)"; b.ext = ""; b.builtin = true; b.supported = true;
        b.enabled = settingsDir.empty() || !fileExists(settingsDir + "3sics.off");
        out.push_back(b);
    }
    if (addonsDir.empty()) return out;
    for (const auto& f : listDir(addonsDir, false)) {
        Addon a; a.file = f; a.dir = addonsDir; a.enabled = true;
        std::string n = f;
        if (n.size() > 8 && n.compare(n.size() - 8, 8, ".disable") == 0) { a.enabled = false; n = n.substr(0, n.size() - 8); }
        size_t dot = n.rfind('.');
        if (dot == std::string::npos) continue;
        a.ext = n.substr(dot); a.id = n.substr(0, dot);
        if (a.ext != ".lua" && a.ext != ".so" && a.ext != ".gpp" && a.ext != ".dll") continue;
        a.supported = (a.ext == ".lua" || a.ext == ".so");  // .dll / .gpp cannot run on Android
        out.push_back(a);
    }
    return out;
}
bool Game::toggleAddon(int idx) {  // enable/disable = built-in flag, or add / remove the ".disable" suffix
    if (idx < 0 || idx >= (int)addons.size()) return false;
    const Addon& a = addons[idx];
    if (a.builtin) {
        std::string flag = settingsDir + "3sics.off";
        mkdirs(settingsDir);
        bool ok = a.enabled ? writeFile(flag, "off\n") : (remove(flag.c_str()) == 0 || !fileExists(flag));
        if (!ok) status = "Could not change " + flag;
        return ok;
    }
    std::string from = a.dir + a.file;
    std::string to = a.dir + (a.enabled ? a.file + ".disable" : a.file.substr(0, a.file.size() - 8));
    if (rename(from.c_str(), to.c_str()) == 0) return true;
    std::string why = strerror(errno), data;     // rename refused: copy the file under the new name instead
    if (readFile(from, data) && writeFile(to, data) && remove(from.c_str()) == 0) return true;
    status = "Could not rename " + a.file + ": " + why;
    return false;
}
int Game::addEntity(int kind) {
    Entity e;
    e.id = (int)ents.size() + 1; e.alive = true; e.meshId = kind; e.broken = false;
    e.name = ADD_NAMES[kind];
    V3 dp, ds, dc; std::string dt;
    kindDefaults(kind, dp, ds, dc, dt);
    e.scale = ds; e.color = dc; e.text = dt;
    e.rotY = 0; e.spin = 0; e.sway = 0; e.shadow = 0;
    if (isUIKind(kind)) e.pos = dp;
    else if (isHelperKind(kind)) e.pos = {target.x, target.y + 2.f, target.z + (kind == KIND_CAMERA ? 6.f : 0.f)};
    else e.pos = {target.x, -MESH_Y0[kind], target.z};
    if (kind == KIND_CAMERA) e.look = target;
    ents.push_back(e);
    return e.id;
}

std::vector<std::string> Game::listScripts() const {
    std::vector<std::string> out;
    for (const auto& f : listDir(projectDir + "scripts/", false))
        if (f.size() > 4 && f.compare(f.size() - 4, 4, ".lua") == 0) out.push_back(f.substr(0, f.size() - 4));
    return out;
}
void Game::newScriptFor(int id) {
    if (id < 1 || id > (int)ents.size()) return;
    Entity& e = ents[id - 1];
    std::string base = cleanName(e.name);
    if (base.empty()) base = "script";
    std::string name = base;
    for (int n = 2; fileExists(projectDir + "scripts/" + name + ".lua"); n++) name = base + "_" + std::to_string(n);
    mkdirs(projectDir + "scripts/");
    std::string src;
    if (e.meshId == KIND_BUTTON)
        src = "-- Script for the button \"" + esc(e.name) + "\". self = this button's id.\n"
              "function on_click(self)\n"
              "  -- example: move every object named \"Cube\" 10 units along x\n"
              "  for _, id in ipairs(engine.find_all(\"Cube\")) do\n"
              "    engine.move(id, 10, 0, 0)\n"
              "  end\n"
              "end\n";
    else
        src = "-- Script for \"" + esc(e.name) + "\". self = this object's id.\n"
              "function start(self)\n"
              "end\n\n"
              "function update(self, dt)\n"
              "  -- runs every frame while playing\n"
              "end\n";
    writeFile(projectDir + "scripts/" + name + ".lua", src);
    e.script = name;
    openCode(name);
}
void Game::openCode(const std::string& name) {
    std::string src;
    readFile(projectDir + "scripts/" + name + ".lua", src);
    code.assign(32768, 0);
    memcpy(code.data(), src.data(), std::min(src.size(), code.size() - 1));
    codeName = name;
    codeOpen = true;
    refocusCode = false;
}
void Game::saveCode() {
    if (!codeOpen) return;
    mkdirs(projectDir + "scripts/");
    if (writeFile(projectDir + "scripts/" + codeName + ".lua", std::string(code.data()))) status = "Script saved";
    else status = "Script save failed";
}

void Game::pick(float sx, float sy) {
    int u = uiAt(sx, sy, true);
    if (u && !ents[u - 1].locked) { selected = u; return; }
    V3 eye = target + V3{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)} * dist;
    V3 f = norm(target - eye), s = norm(cross(f, {0, 1, 0})), up = cross(s, f);
    float th = std::tan(0.45f), asp = (float)w / h;
    float nx = 2.f * sx / w - 1.f, ny = 1.f - 2.f * sy / h;
    V3 d = norm(f + s * (nx * th * asp) + up * (ny * th));
    float t = 0;
    selected = raycast(eye, d, t, true);
}

static int codeCallback(ImGuiInputTextCallbackData* d) {
    if (G && !G->pendingInsert.empty()) {  // symbol-bar buttons insert at the cursor
        d->InsertChars(d->CursorPos, G->pendingInsert.c_str());
        G->pendingInsert.clear();
    }
    return 0;
}

void Game::drawMenu(ImGuiWindowFlags F) {
    float W = (float)w, H = (float)h;
    bool doCreate = false;
    ImGui::SetNextWindowPos(ImVec2(0.12f * W, 0.04f * H));
    ImGui::SetNextWindowSize(ImVec2(0.76f * W, 0.92f * H));
    ImGui::Begin(lab(tr(S_PROJECTS), "projects").c_str(), nullptr, F);
    ImGui::TextWrapped("%s", baseDir.empty() ? "(none)" : baseDir.c_str());
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
    ImGui::Separator();
    if (ImGui::BeginChild("plist", ImVec2(0, H * 0.5f), 0)) {
        if (projects.empty()) ImGui::TextWrapped("%s", tr(S_NO_PROJECTS));
        for (int i = 0; i < (int)projects.size(); i++)
            if (ImGui::Selectable(projects[i].c_str(), menuSel == i)) menuSel = i;
    }
    ImGui::EndChild();
    if (ImGui::Button(tr(S_CREATE))) doCreate = true;
    ImGui::SameLine();
    if (ImGui::Button(tr(S_OPEN)) && menuSel >= 0 && menuSel < (int)projects.size()) openProject(projects[menuSel]);
    ImGui::SameLine();
    if (ImGui::Button(tr(S_REFRESH))) refreshProjects();
    ImGui::SameLine();
    if (ImGui::Button(tr(S_IMPORT))) openBrowser(*this, "3ne");
    ImGui::SameLine();
    if (ImGui::Button(tr(S_DEPOT))) showDepot = true;
    ImGui::End();

    if (doCreate) { nameBuf[0] = 0; status.clear(); ImGui::OpenPopup("New project"); }
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.03f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(W * 0.7f, 0));
    if (ImGui::BeginPopupModal("New project", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextUnformatted(tr(S_NAME));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##pname", nameBuf, sizeof nameBuf, ImGuiInputTextFlags_CharsNoBlank);
        ImGui::TextWrapped("%s%s/", baseDir.c_str(), cleanName(nameBuf).c_str());
        if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
        if (ImGui::Button(tr(S_CREATE))) {
            if (createProject(nameBuf)) ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(S_CANCEL))) { status.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void Game::drawAddons(ImGuiWindowFlags F) {
    float W = (float)w, H = (float)h;
    ImGui::SetNextWindowPos(ImVec2(0.08f * W, 0.10f * H));
    ImGui::SetNextWindowSize(ImVec2(0.84f * W, 0.82f * H));
    ImGui::Begin("Addons (mods)", nullptr, F);
    if (ImGui::Button("Close")) showAddons = false;
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) addons = scanAddons();
    ImGui::TextWrapped("Folder: %s", addonsDir.c_str());
    ImGui::TextWrapped("3sics (physics) is built into the engine: just switch it on or off here. .lua and .so addons from the folder are loaded too. .dll / .gpp are PC formats and cannot run on Android. "
                       "A disabled addon has .disable added to its file name and is ignored. "
                       "'Required' lists it in project.json: the game will not start without it.");
    ImGui::Separator();
    if (addons.empty()) ImGui::TextUnformatted("No addons found. Put files into the folder above.");
    int toggle = -1;
    for (int i = 0; i < (int)addons.size(); i++) {
        Addon& a = addons[i];
        ImGui::PushID(i);
        if (a.builtin) ImGui::Text("%s  (built-in, C++)", a.id.c_str());
        else ImGui::Text("%s%s", a.id.c_str(), a.ext.c_str());
        ImGui::SameLine();
        if (!a.supported) ImGui::TextUnformatted("(not supported on Android)");
        else if (ImGui::Button(a.enabled ? "Disable" : "Enable")) toggle = i;
        ImGui::SameLine();
        bool req = false;
        for (const auto& r : reqs) if (r == a.id) req = true;
        if (ImGui::Checkbox("Required", &req)) {
            if (req) reqs.push_back(a.id);
            else for (size_t k = 0; k < reqs.size(); k++) if (reqs[k] == a.id) { reqs.erase(reqs.begin() + k); break; }
            saveRequires();
        }
        ImGui::PopID();
    }
    if (toggle >= 0) { toggleAddon(toggle); addons = scanAddons(); }
    ImGui::End();
}

void Game::drawEditorUI() {
    const ImGuiWindowFlags F = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    float W = (float)w, H = (float)h, topH = 0.12f * H;

    if (screen == SCREEN_WELCOME) { drawWelcome(F); return; }
    if (screen == SCREEN_MENU) {
        drawMenu(F);
        if (showDepot) drawDepot(F);
        if (showBrowser) drawBrowser(F);
        return;
    }

    if (mode == MODE_PLAY) {
        ImGui::SetNextWindowPos(ImVec2(0.86f * W, 0));
        ImGui::SetNextWindowSize(ImVec2(0.14f * W, topH));
        ImGui::Begin("##play", nullptr, F | ImGuiWindowFlags_NoTitleBar);
        if (ImGui::Button(tr(S_STOP), ImVec2(-1, -1))) stopPlay();
        ImGui::End();
        if (!status.empty()) ImGui::GetForegroundDrawList()->AddText(ImVec2(10, topH), IM_COL32(255, 90, 90, 255), status.c_str());
        return;
    }

    bool doPlay = false, doBuild = false, doBackup = false, doDup = false, doMenu = false, doNewScript = false, doEditScript = false;
    int doAdd = -1;
    std::string goScene, newScene;

    // ---- top bar ----
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, topH));
    ImGui::Begin("##top", nullptr, F | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
    if (ImGui::Button(tr(S_MENU))) doMenu = true;
    ImGui::SameLine(); if (ImGui::Button(tr(S_PLAY))) doPlay = true;
    ImGui::SameLine(); if (ImGui::Button(tr(S_PREPARE))) doBuild = true;
    ImGui::SameLine(); if (ImGui::Button(tr(S_SAVE))) saveProject();
    ImGui::SameLine(); if (ImGui::Button(tr(S_ADD))) ImGui::OpenPopup("addpop");
    if (ImGui::BeginPopup("addpop")) {
        for (int i = 0; i < KIND_COUNT; i++) if (ImGui::Selectable(ADD_NAMES[i])) doAdd = i;
        ImGui::EndPopup();
    }
    ImGui::SameLine(); if (ImGui::Button(tr(S_SCENES))) ImGui::OpenPopup("scenespop");
    if (ImGui::BeginPopup("scenespop")) {
        for (const auto& sn : listScenes()) {
            std::string l = (sn == sceneName ? "> " : "") + sn + "###sc_" + sn;
            if (ImGui::Selectable(l.c_str())) goScene = sn;
        }
        ImGui::Separator();
        ImGui::InputText("##newscene", sceneBuf, sizeof sceneBuf, ImGuiInputTextFlags_CharsNoBlank);
        if (ImGui::Button(lab(tr(S_NEW_SCENE), "newscene").c_str()) && !cleanName(sceneBuf).empty()) { newScene = cleanName(sceneBuf); sceneBuf[0] = 0; }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    {
        std::string sl = std::string(tr(S_SNAP)) + ": " + (snapIdx == 0 ? tr(S_OFF) : (snapIdx == 1 ? "0.5" : "1.0")) + "###snap";
        if (ImGui::Button(sl.c_str())) snapIdx = (snapIdx + 1) % 3;
    }
    ImGui::SameLine(); if (ImGui::Button(tr(S_MORE))) ImGui::OpenPopup("morepop");
    if (ImGui::BeginPopup("morepop")) {
        if (ImGui::Selectable(tr(S_MODS))) { addons = scanAddons(); showAddons = true; }
        if (ImGui::Selectable(tr(S_DEPOT))) showDepot = true;
        if (ImGui::Selectable(tr(S_BACKUP))) doBackup = true;
        ImGui::EndPopup();
    }
    ImGui::Text("%s / %s  [%s]", projectName.c_str(), sceneName.c_str(), forwardMode ? "Forward" : "Unlit");
    ImGui::SameLine(); ImGui::TextUnformatted(status.c_str());
    ImGui::End();

    // ---- scene list (grouped by folder, children indented under their parent) ----
    ImGui::SetNextWindowPos(ImVec2(0, topH));
    ImGui::SetNextWindowSize(ImVec2(0.25f * W, H - topH));
    ImGui::Begin(lab(tr(S_SCENE_LIST), "scenelist").c_str(), nullptr, F);
    {
        auto row = [&](const Entity& e) {
            int depth = 0;
            for (int c = e.parent; c > 0 && c <= (int)ents.size() && depth < 8 && !isUIKind(e.meshId); c = ents[c - 1].parent) depth++;
            if (depth) ImGui::Indent(depth * 18.f);
            char label[200];
            snprintf(label, sizeof label, "%s%s%s###e%d", e.hidden ? "(H) " : "", e.locked ? "(L) " : "", e.name.c_str(), e.id);
            if (ImGui::Selectable(label, selected == e.id)) selected = e.id;
            if (depth) ImGui::Unindent(depth * 18.f);
        };
        std::vector<std::string> folders;
        for (const auto& e : ents)
            if (e.alive && !e.folder.empty() && std::find(folders.begin(), folders.end(), e.folder) == folders.end()) folders.push_back(e.folder);
        for (const auto& e : ents) if (e.alive && e.folder.empty()) row(e);
        for (const auto& f : folders) {
            if (ImGui::TreeNode(f.c_str())) {
                for (const auto& e : ents) if (e.alive && e.folder == f) row(e);
                ImGui::TreePop();
            }
        }
    }
    ImGui::End();

    // ---- inspector ----
    ImGui::SetNextWindowPos(ImVec2(0.70f * W, topH));
    ImGui::SetNextWindowSize(ImVec2(0.30f * W, H - topH));
    ImGui::Begin(lab(tr(S_INSPECTOR), "inspector").c_str(), nullptr, F);
    Entity* se = (selected >= 1 && selected <= (int)ents.size() && ents[selected - 1].alive) ? &ents[selected - 1] : nullptr;
    if (!se) ImGui::TextWrapped("%s", tr(S_TAP_HINT));
    else {
        ImGui::Checkbox(lab(tr(S_HIDE), "hide").c_str(), &se->hidden);
        ImGui::SameLine();
        ImGui::Checkbox(lab(tr(S_LOCK), "lock").c_str(), &se->locked);
        if (se->locked) ImGui::BeginDisabled();
        char tb[128];
        snprintf(tb, sizeof tb, "%s", se->name.c_str());
        if (ImGui::InputText(lab(tr(S_NAME), "name").c_str(), tb, sizeof tb)) se->name = tb;
        snprintf(tb, sizeof tb, "%s", se->folder.c_str());
        if (ImGui::InputText(lab(tr(S_FOLDER), "folder").c_str(), tb, sizeof tb)) se->folder = tb;
        int k = se->meshId;
        if (k == KIND_BUTTON || k == KIND_LABEL) {
            ImGui::DragFloat2(lab(tr(S_POS), "p2").c_str(), &se->pos.x, 0.2f, -20.f, 120.f);
            ImGui::DragFloat2(lab(tr(S_SIZE), "s2").c_str(), &se->scale.x, 0.2f, 1.f, 100.f);
            snprintf(tb, sizeof tb, "%s", se->text.c_str());
            if (ImGui::InputText("Text", tb, sizeof tb)) se->text = tb;
            ImGui::ColorEdit3(lab(tr(S_COLOR), "col").c_str(), &se->color.x);
        } else if (k == KIND_JOYSTICK) {
            ImGui::DragFloat2(lab(tr(S_POS), "p2").c_str(), &se->pos.x, 0.2f, -20.f, 120.f);
            ImGui::DragFloat(lab(tr(S_SIZE), "s1").c_str(), &se->scale.x, 0.2f, 8.f, 80.f);
            ImGui::ColorEdit3(lab(tr(S_COLOR), "col").c_str(), &se->color.x);
            ImGui::TextWrapped("Lua: joystick_x, joystick_y (-1..1)");
        } else if (k == KIND_IMAGE) {
            ImGui::DragFloat2(lab(tr(S_POS), "p2").c_str(), &se->pos.x, 0.2f, -20.f, 120.f);
            ImGui::DragFloat2(lab(tr(S_SIZE), "s2").c_str(), &se->scale.x, 0.2f, 1.f, 100.f);
            if (ImGui::BeginCombo(lab(tr(S_IMAGE), "img").c_str(), se->img.empty() ? "(none)" : se->img.c_str())) {
                for (const auto& f : listDir(depotDir + "img/", false)) if (ImGui::Selectable(f.c_str(), se->img == f)) se->img = f;
                ImGui::EndCombo();
            }
        } else if (k == KIND_CAMERA) {
            ImGui::DragFloat3(lab(tr(S_POS), "p3").c_str(), &se->pos.x, 0.05f);
            std::string tn = "(point)";
            if (se->lookId >= 1 && se->lookId <= (int)ents.size() && ents[se->lookId - 1].alive) tn = ents[se->lookId - 1].name;
            ImGui::DragFloat3(lab(tr(S_LOOK_AT), "look").c_str(), &se->look.x, 0.05f);
            ImGui::Text("-> %s", tn.c_str());
            ImGui::SameLine();
            if (ImGui::Button(lab(tr(S_SET), "lookset").c_str())) ImGui::OpenPopup("lookpop");
            if (ImGui::BeginPopup("lookpop")) {
                if (ImGui::Selectable("(point)")) se->lookId = 0;
                for (const auto& o : ents) {
                    if (!o.alive || o.id == se->id || isUIKind(o.meshId)) continue;
                    char l[200]; snprintf(l, sizeof l, "%s###lk%d", o.name.c_str(), o.id);
                    if (ImGui::Selectable(l)) se->lookId = o.id;
                }
                ImGui::EndPopup();
            }
            ImGui::SliderFloat(lab(tr(S_FOV), "fov").c_str(), &se->fov, 0.3f, 1.6f);
            ImGui::TextWrapped("While playing, the first Camera object is the view.");
        } else if (k == KIND_LIGHT) {
            ImGui::DragFloat3(lab(tr(S_POS), "p3").c_str(), &se->pos.x, 0.05f);
            ImGui::ColorEdit3(lab(tr(S_COLOR), "col").c_str(), &se->color.x);
            ImGui::SliderFloat(lab(tr(S_INTENSITY), "int").c_str(), &se->intensity, 0.f, 5.f);
            ImGui::SliderFloat(lab(tr(S_RANGE), "rng").c_str(), &se->range, 1.f, 50.f);
            ImGui::TextWrapped("The first light switches the renderer from Unlit to Forward.");
        } else {
            std::string pn = "(none)";
            if (se->parent >= 1 && se->parent <= (int)ents.size() && ents[se->parent - 1].alive) pn = ents[se->parent - 1].name;
            ImGui::Text("%s: %s", tr(S_PARENT), pn.c_str());
            ImGui::SameLine();
            if (ImGui::Button(lab(tr(S_SET), "parset").c_str())) ImGui::OpenPopup("parentpop");
            if (ImGui::BeginPopup("parentpop")) {
                if (ImGui::Selectable("(none)")) setParent(se->id, 0);
                for (const auto& o : ents) {
                    if (!o.alive || o.id == se->id || isUIKind(o.meshId)) continue;
                    char l[200]; snprintf(l, sizeof l, "%s###par%d", o.name.c_str(), o.id);
                    if (ImGui::Selectable(l)) { if (!setParent(se->id, o.id)) status = "That would make a loop."; }
                }
                ImGui::EndPopup();
            }
            ImGui::DragFloat3(lab(tr(S_POS), "p3").c_str(), &se->pos.x, 0.05f);
            ImGui::DragFloat3(lab(tr(S_SIZE), "s3").c_str(), &se->scale.x, 0.05f, 0.01f, 500.f);
            float deg = se->rotY * 57.29578f;
            if (ImGui::SliderFloat(lab(tr(S_ROT), "rot").c_str(), &deg, -180.f, 180.f)) se->rotY = deg / 57.29578f;
            ImGui::ColorEdit3(lab(tr(S_COLOR), "col").c_str(), &se->color.x);
            if (ImGui::BeginCombo(lab(tr(S_MATERIAL), "mat").c_str(), se->material.c_str())) {
                for (const char* m : MATERIALS) if (ImGui::Selectable(m, se->material == m)) se->material = m;
                for (const auto& f : depot.list("material")) if (ImGui::Selectable(f.c_str(), se->material == f)) se->material = f;
                ImGui::EndCombo();
            }
            ImGui::SliderFloat("Spin", &se->spin, -5.f, 5.f);
            ImGui::SliderFloat("Sway", &se->sway, 0.f, 1.f);
            ImGui::SliderFloat("Shadow", &se->shadow, 0.f, 10.f);
        }
        ImGui::Separator();
        ImGui::Text("%s: %s", tr(S_SCRIPT), se->script.empty() ? "(none)" : se->script.c_str());
        if (ImGui::Button(tr(S_NEW))) doNewScript = true;
        ImGui::SameLine(); if (ImGui::Button(tr(S_PICK))) ImGui::OpenPopup("pickscript");
        if (!se->script.empty()) {
            ImGui::SameLine(); if (ImGui::Button(tr(S_EDIT))) doEditScript = true;
            ImGui::SameLine(); if (ImGui::Button(tr(S_CLEAR))) se->script.clear();
        }
        if (ImGui::BeginPopup("pickscript")) {
            std::vector<std::string> sc = listScripts();
            if (sc.empty()) ImGui::TextUnformatted("-");
            for (const auto& n : sc) if (ImGui::Selectable(n.c_str())) se->script = n;
            ImGui::EndPopup();
        }
        ImGui::Separator();
        if (ImGui::Button(tr(S_DUPLICATE))) doDup = true;
        ImGui::SameLine();
        if (ImGui::Button(tr(S_DELETE))) { destroyEntity(se->id); selected = 0; }
        if (se->locked) ImGui::EndDisabled();
    }
    ImGui::End();

    if (showAddons) drawAddons(F);
    if (showDepot) drawDepot(F);
    if (showBrowser) drawBrowser(F);

    // ---- script editor (top half; the keyboard covers the lower half) ----
    if (codeOpen) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(W, 0.5f * H));
        char title[160]; snprintf(title, sizeof title, "Script: %s.lua###code", codeName.c_str());
        ImGui::Begin(title, nullptr, F);
        if (ImGui::Button(tr(S_SAVE))) saveCode();
        ImGui::SameLine();
        if (ImGui::Button(lab(tr(S_SAVE_CLOSE), "savecl").c_str())) { saveCode(); codeOpen = false; }
        static const char* SYM[] = {"(", ")", "{", "}", "[", "]", "\"", "'", "=", ",", ".", ":", "_", "#", "+", "-", "*", "/", "<", ">", "~"};
        for (int i = 0; i < (int)(sizeof SYM / sizeof SYM[0]); i++) {
            if (i != 0) ImGui::SameLine();
            if (ImGui::Button(SYM[i])) { pendingInsert = SYM[i]; refocusCode = true; }
        }
        if (codeOpen) {
            if (refocusCode) { ImGui::SetKeyboardFocusHere(); refocusCode = false; }
            ImGui::InputTextMultiline("##code", code.data(), code.size(), ImVec2(-1, -1),
                                      ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackAlways, codeCallback);
        }
        ImGui::End();
    }

    // ---- move gizmo ----
    if (!codeOpen && !showAddons && !showDepot && !showBrowser) drawGizmo();

    // ---- prepare for APK / backup ----
    if (doBuild) {
        saveProject();
        writeFile(baseDir + "startup.txt", projectName + "\n");  // names the project bundled in the game APK
        mkdirs(projectDir + "addons/");  // required Lua addons travel with the game
        addons = scanAddons();
        for (const auto& id : reqs)
            for (const auto& a : addons)
                if (a.id == id && a.enabled && a.ext == ".lua") {
                    std::string src;
                    if (readFile(a.dir + a.file, src)) writeFile(projectDir + "addons/" + id + ".lua", src);
                }
        std::string outp;
        status = prepareApk(outp) ? "Ready: " + outp : status;
        ImGui::OpenPopup("Prepare for APK");
    }
    if (doBackup) {
        std::string outp;
        status = exportBackup(outp) ? "Backup: " + outp : "Backup failed";
    }
    if (doPlay) {
        std::string miss = checkRequires();
        if (!miss.empty()) { modError = miss; doPlay = false; ImGui::OpenPopup("Mods missing"); }
    }
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(W * 0.7f, 0));
    if (ImGui::BeginPopupModal("Prepare for APK", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("Extract that zip into a GitHub repository (or run  sh publish.sh  in your repo). "
                           "GitHub Actions compiles it; download the artifact 3gine-game-apk from the Actions page.");
        ImGui::TextWrapped("An APK cannot be compiled on the phone itself (it needs the Android SDK/NDK toolchain), "
                           "so the project is prepared here and GitHub does the compiling. Required .lua addons are included; .so addons are not.");
        if (ImGui::Button(tr(S_OK), ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(W * 0.7f, 0));
    if (ImGui::BeginPopupModal("Mods missing", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("The game was not started. This project requires mods that are missing or disabled:");
        ImGui::TextWrapped("%s", modError.c_str());
        ImGui::TextWrapped("Open More > Mods to enable them, or copy the files into %s", addonsDir.c_str());
        if (ImGui::Button(tr(S_OK), ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- deferred actions (never change the entity list while iterating it) ----
    if (!newScene.empty()) {
        mkdirs(projectDir + "scenes/");
        if (!fileExists(projectDir + sceneFile(newScene))) writeFile(projectDir + sceneFile(newScene), DEFAULT_SCENE);
        goScene = newScene;
    }
    if (!goScene.empty() && goScene != sceneName) {
        saveProject();
        sceneName = goScene;
        selected = 0;
        loadScene();
        status = "Scene: " + sceneName;
    }
    if (doAdd >= 0) selected = addEntity(doAdd);
    if (doDup && selected >= 1 && selected <= (int)ents.size()) {
        Entity c = ents[selected - 1];
        c.id = (int)ents.size() + 1;
        if (isUIKind(c.meshId)) c.pos.y += 5.f; else c.pos.x += 1.f;
        ents.push_back(c); selected = c.id;
    }
    if (doNewScript && selected >= 1) newScriptFor(selected);
    if (doEditScript && selected >= 1 && selected <= (int)ents.size()) openCode(ents[selected - 1].script);
    if (doMenu) { saveProject(); codeOpen = false; showAddons = false; refreshProjects(); screen = SCREEN_MENU; status.clear(); }
    if (doPlay) { saveCode(); startPlay(); }
}
#endif

// ---------------------------------------------------------------- platform
struct Platform {
    android_app* app;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
    Game game;
    bool ready = false;
    float lastX = 0, lastY = 0, lastPinch = 0;
    float downX = 0, downY = 0, moved = 0;
    bool gameTouch = true;
    bool kbd = false;
    bool gizmo = false;
};

// Shows / hides the Android soft keyboard through JNI (no Java code needed).
static void showKeyboard(android_app* app, bool show) {
    JavaVM* vm = app->activity->vm;
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    jobject activity = app->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jmethodID getSS = env->GetMethodID(actCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring svc = env->NewStringUTF("input_method");
    jobject ims = env->CallObjectMethod(activity, getSS, svc);
    jmethodID getWin = env->GetMethodID(actCls, "getWindow", "()Landroid/view/Window;");
    jobject win = env->CallObjectMethod(activity, getWin);
    jclass winCls = env->GetObjectClass(win);
    jmethodID getDecor = env->GetMethodID(winCls, "getDecorView", "()Landroid/view/View;");
    jobject decor = env->CallObjectMethod(win, getDecor);
    jclass imsCls = env->GetObjectClass(ims);
    if (show) {
        jmethodID m = env->GetMethodID(imsCls, "showSoftInput", "(Landroid/view/View;I)Z");
        env->CallBooleanMethod(ims, m, decor, 0);
    } else {
        jclass viewCls = env->GetObjectClass(decor);
        jmethodID getTok = env->GetMethodID(viewCls, "getWindowToken", "()Landroid/os/IBinder;");
        jobject tok = env->CallObjectMethod(decor, getTok);
        jmethodID m = env->GetMethodID(imsCls, "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z");
        env->CallBooleanMethod(ims, m, tok, 0);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    vm->DetachCurrentThread();
}

// Soft-keyboard key events -> characters (ASCII layout; the symbol bar in the script editor covers the rest).
static int keyToChar(int code, bool shift) {
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) { char c = (char)('a' + (code - AKEYCODE_A)); return shift ? c - 32 : c; }
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9) {
        static const char* SH = ")!@#$%^&*(";
        return shift ? SH[code - AKEYCODE_0] : '0' + (code - AKEYCODE_0);
    }
    switch (code) {
        case AKEYCODE_SPACE: return ' ';
        case AKEYCODE_COMMA: return shift ? '<' : ',';
        case AKEYCODE_PERIOD: return shift ? '>' : '.';
        case AKEYCODE_MINUS: return shift ? '_' : '-';
        case AKEYCODE_EQUALS: return shift ? '+' : '=';
        case AKEYCODE_LEFT_BRACKET: return shift ? '{' : '[';
        case AKEYCODE_RIGHT_BRACKET: return shift ? '}' : ']';
        case AKEYCODE_BACKSLASH: return shift ? '|' : '\\';
        case AKEYCODE_SEMICOLON: return shift ? ':' : ';';
        case AKEYCODE_APOSTROPHE: return shift ? '"' : '\'';
        case AKEYCODE_SLASH: return shift ? '?' : '/';
        case AKEYCODE_GRAVE: return shift ? '~' : '`';
        case AKEYCODE_AT: return '@';
        case AKEYCODE_POUND: return '#';
        case AKEYCODE_STAR: return '*';
        case AKEYCODE_PLUS: return '+';
    }
    return 0;
}

// ---- sound: android.media.MediaPlayer through JNI (mp3 / ogg / wav) ----
static JavaVM* g_vm = nullptr;
static std::vector<jobject> g_players;
static void releasePlayer(JNIEnv* env, jobject mp) {
    jclass cls = env->GetObjectClass(mp);
    jmethodID stop = env->GetMethodID(cls, "stop", "()V");
    jmethodID rel = env->GetMethodID(cls, "release", "()V");
    if (stop) env->CallVoidMethod(mp, stop);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (rel) env->CallVoidMethod(mp, rel);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteGlobalRef(mp);
}
static bool playSoundFile(const std::string& path, bool loop) {
    if (!g_vm) return false;
    JNIEnv* env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;
    bool ok = false;
    jclass cls = env->FindClass("android/media/MediaPlayer");
    if (cls) {
        jmethodID ctor = env->GetMethodID(cls, "<init>", "()V");
        jobject mp = env->NewObject(cls, ctor);
        if (mp && !env->ExceptionCheck()) {
            jmethodID setDS = env->GetMethodID(cls, "setDataSource", "(Ljava/lang/String;)V");
            jmethodID prep = env->GetMethodID(cls, "prepare", "()V");
            jmethodID setLoop = env->GetMethodID(cls, "setLooping", "(Z)V");
            jmethodID start = env->GetMethodID(cls, "start", "()V");
            jstring js = env->NewStringUTF(path.c_str());
            env->CallVoidMethod(mp, setDS, js);
            if (!env->ExceptionCheck()) env->CallVoidMethod(mp, prep);
            if (!env->ExceptionCheck()) { env->CallVoidMethod(mp, setLoop, (int)(loop ? 1 : 0)); env->CallVoidMethod(mp, start); }
            if (!env->ExceptionCheck()) { g_players.push_back(env->NewGlobalRef(mp)); ok = true; }
            else env->ExceptionClear();
        } else if (env->ExceptionCheck()) env->ExceptionClear();
    } else if (env->ExceptionCheck()) env->ExceptionClear();
    while (g_players.size() > 12) { releasePlayer(env, g_players.front()); g_players.erase(g_players.begin()); }
    g_vm->DetachCurrentThread();
    return ok;
}
static void stopAllSounds() {
    if (!g_vm || g_players.empty()) return;
    JNIEnv* env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    for (jobject mp : g_players) releasePlayer(env, mp);
    g_players.clear();
    g_vm->DetachCurrentThread();
}

static bool initGL(Platform& p) {
    p.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(p.dpy, nullptr, nullptr);
    const EGLint cfgAttr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    const EGLint msaaAttr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24,
                               EGL_SAMPLE_BUFFERS, 1, EGL_SAMPLES, 4, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(p.dpy, msaaAttr, &cfg, 1, &n) || n < 1) {
        if (!eglChooseConfig(p.dpy, cfgAttr, &cfg, 1, &n) || n < 1) { LOG("no EGL config"); return false; }
    }
    p.surf = eglCreateWindowSurface(p.dpy, cfg, p.app->window, nullptr);
    const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    p.ctx = eglCreateContext(p.dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (!eglMakeCurrent(p.dpy, p.surf, p.surf, p.ctx)) { LOG("makeCurrent failed"); return false; }
    eglQuerySurface(p.dpy, p.surf, EGL_WIDTH, &p.game.w);
    eglQuerySurface(p.dpy, p.surf, EGL_HEIGHT, &p.game.h);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    {  // a system font with Latin Extended + Cyrillic glyphs (Turkish, German, Russian ... text)
        static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x0400, 0x04FF, 0};
        const char* fonts[] = {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf",
                               "/system/fonts/NotoSans-Regular.ttf", "/system/fonts/DroidSans.ttf"};
        p.game.customFont = false;
        for (const char* f : fonts)
            if (access(f, R_OK) == 0 && io.Fonts->AddFontFromFileTTF(f, 28.0f, nullptr, ranges)) { p.game.customFont = true; break; }
    }
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(2.0f);
    ImGui_ImplAndroid_Init(p.app->window);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    p.game.init();
    return true;
}
static void termGL(Platform& p) {
    if (p.dpy == EGL_NO_DISPLAY) return;
    p.game.shutdown();
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();
    }
    eglMakeCurrent(p.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (p.ctx != EGL_NO_CONTEXT) eglDestroyContext(p.dpy, p.ctx);
    if (p.surf != EGL_NO_SURFACE) eglDestroySurface(p.dpy, p.surf);
    eglTerminate(p.dpy);
    p.dpy = EGL_NO_DISPLAY; p.ctx = EGL_NO_CONTEXT; p.surf = EGL_NO_SURFACE;
    p.kbd = false;
}

static void onCmd(android_app* app, int32_t cmd) {
    auto* p = (Platform*)app->userData;
    if (cmd == APP_CMD_INIT_WINDOW) { if (app->window) p->ready = initGL(*p); }
    else if (cmd == APP_CMD_TERM_WINDOW) { p->ready = false; termGL(*p); }
}

static int32_t onInput(android_app* app, AInputEvent* e) {
    auto* p = (Platform*)app->userData;
    Game& g = p->game;

    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
#ifdef GAME_MODE
        return 0;
#else
        int code = AKeyEvent_getKeyCode(e);
        if (code == AKEYCODE_BACK || !ImGui::GetCurrentContext()) return 0;
        ImGui_ImplAndroid_HandleInputEvent(e);
        if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN) {
            bool shift = (AKeyEvent_getMetaState(e) & AMETA_SHIFT_ON) != 0;
            int ch = keyToChar(code, shift);
            if (ch) ImGui::GetIO().AddInputCharacter((unsigned int)ch);
        }
        return 1;
#endif
    }
    if (AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) return 0;
#ifndef GAME_MODE
    if (ImGui::GetCurrentContext()) ImGui_ImplAndroid_HandleInputEvent(e);
#endif
    int act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    size_t cnt = AMotionEvent_getPointerCount(e);
    float x = AMotionEvent_getX(e, 0), y = AMotionEvent_getY(e, 0);

    if (act == AMOTION_EVENT_ACTION_DOWN) {
        p->gameTouch = g.inViewport(x, y);  // touches that start on a UI panel never reach the game
        p->downX = x; p->downY = y; p->moved = 0; p->lastX = x; p->lastY = y; p->lastPinch = 0;
#ifndef GAME_MODE
        p->gizmo = p->gameTouch && g.mode == MODE_EDIT && g.screen == SCREEN_PROJECT && g.gizmoBegin(x, y);  // grabbed an axis arrow?
#endif
    }
    if (!p->gameTouch) { p->lastX = x; p->lastY = y; return 1; }
#ifndef GAME_MODE
    if (p->gizmo) {  // dragging an axis arrow: move the selected object (snapped), never orbit or pick
        if (act == AMOTION_EVENT_ACTION_MOVE) g.gizmoMove(x - p->lastX, y - p->lastY);
        else if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_CANCEL) { g.gizmoEnd(); p->gizmo = false; }
        p->lastX = x; p->lastY = y;
        return 1;
    }
#endif

    if (act == AMOTION_EVENT_ACTION_DOWN) {
        g.touchDown = true; g.touchX = x; g.touchY = y;
        g.uiDownId = (g.mode == MODE_PLAY) ? g.uiAt(x, y, false) : 0;
        g.joyActive = g.uiDownId && g.ents[g.uiDownId - 1].meshId == KIND_JOYSTICK;
        if (g.joyActive) g.updateJoystick(x, y);
    } else if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_CANCEL) {
        g.touchDown = false;
        if (act == AMOTION_EVENT_ACTION_UP && g.mode == MODE_PLAY && g.uiDownId && g.ents[g.uiDownId - 1].meshId == KIND_BUTTON && g.uiAt(x, y, false) == g.uiDownId)
            g.clickUI(g.uiDownId);  // a tap on a UI button runs its on_click()
#ifndef GAME_MODE
        if (act == AMOTION_EVENT_ACTION_UP && g.mode == MODE_EDIT && p->moved < 25.f) g.pick(x, y);
#endif
        g.joyActive = false; g.joyX = g.joyY = 0;
        g.uiDownId = 0;
    } else if (act == AMOTION_EVENT_ACTION_MOVE) {
        g.touchDX += x - p->lastX; g.touchDY += y - p->lastY; g.touchX = x; g.touchY = y;
        p->moved += std::fabs(x - p->lastX) + std::fabs(y - p->lastY);
        if (g.joyActive) g.updateJoystick(x, y);
    }

    if (g.orbit && !g.uiDownId) {
        if (cnt >= 2) {  // pinch = zoom
            float dx = AMotionEvent_getX(e, 1) - x, dy = AMotionEvent_getY(e, 1) - y;
            float d = std::sqrt(dx * dx + dy * dy);
            if (act == AMOTION_EVENT_ACTION_MOVE && p->lastPinch > 0) {
                g.dist -= (d - p->lastPinch) * 0.03f;
                g.dist = g.dist < 3.f ? 3.f : (g.dist > 80.f ? 80.f : g.dist);
            }
            p->lastPinch = d;
            p->moved += 100.f;  // a pinch is never a tap
        } else {
            p->lastPinch = 0;
            if (act == AMOTION_EVENT_ACTION_MOVE) {
                g.yaw -= (x - p->lastX) * 0.005f;
                g.pitch += (y - p->lastY) * 0.005f;
                g.pitch = g.pitch < 0.1f ? 0.1f : (g.pitch > 1.4f ? 1.4f : g.pitch);
            }
        }
    }
    p->lastX = x; p->lastY = y;
    return 1;
}

static double nowSec() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void android_main(android_app* app) {
    Platform p; p.app = app;
    g_vm = app->activity->vm;
    p.game.assets = app->activity->assetManager;
    if (app->activity->externalDataPath) p.game.fallbackDir = std::string(app->activity->externalDataPath) + "/";
    if (app->activity->internalDataPath) p.game.internalDir = app->activity->internalDataPath;
    app->userData = &p;
    app->onAppCmd = onCmd;
    app->onInputEvent = onInput;
    double last = nowSec();

    while (!app->destroyRequested) {
        android_poll_source* src;
        int timeout = p.ready ? 0 : -1;
        while (ALooper_pollOnce(timeout, nullptr, nullptr, (void**)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) { termGL(p); return; }
            timeout = p.ready ? 0 : -1;
        }
        if (!p.ready) continue;
        double now = nowSec();
        float dt = (float)(now - last); last = now;
        if (dt > 0.1f) dt = 0.1f;
        p.game.update(dt);
        p.game.render();
        p.game.drawUI();
#ifndef GAME_MODE
        bool want = ImGui::GetIO().WantTextInput;  // show the soft keyboard while a text field is active
        if (want != p.kbd) { showKeyboard(app, want); p.kbd = want; }
#endif
        eglSwapBuffers(p.dpy, p.surf);
    }
    termGL(p);
}
