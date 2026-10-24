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

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "3gine", __VA_ARGS__)

// ---------------------------------------------------------------- math
struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static V3 norm(V3 a) { float l = std::sqrt(dot(a, a)); return l > 1e-8f ? a * (1.f / l) : V3{0, 1, 0}; }

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
precision mediump float;
in vec3 vN;
in vec3 vW;
uniform vec3 uColor;
uniform vec3 uCam;
uniform vec3 uSun;     // direction towards the sun
uniform vec3 uFog;
uniform float uAlpha;
out vec4 o;
void main(){
    if (uAlpha < 1.0) { o = vec4(uColor, uAlpha); return; }
    vec3 n = normalize(vN);
    float d = max(dot(n, normalize(uSun)), 0.0);
    // 3-step toon bands
    float band = d < 0.2 ? 0.0 : (d < 0.55 ? 0.5 : 1.0);
    // warm sky / cool ground ambient
    vec3 amb = mix(vec3(0.30,0.28,0.38), vec3(0.55,0.65,0.80), n.y*0.5+0.5);
    vec3 lit = uColor * (amb*0.75 + vec3(1.0,0.93,0.78)*band*0.65);
    // soft rim light
    vec3 vd = normalize(uCam - vW);
    float rim = pow(1.0 - max(dot(n, vd), 0.0), 3.0);
    lit += vec3(0.5,0.6,0.8) * rim * 0.18;
    // fog blends to the sky colour
    float dist = length(uCam - vW);
    float f = smoothstep(25.0, 85.0, dist);
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

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char buf[512]; glGetShaderInfoLog(s, 512, nullptr, buf); LOG("shader: %s", buf); }
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
enum Screen { SCREEN_MENU, SCREEN_PROJECT };
enum { MESH_COUNT = 10, KIND_BUTTON = 10, KIND_LABEL = 11, KIND_COUNT = 12 };

static const char* MESH_NAMES[KIND_COUNT] = {"cube", "cone", "disc", "cylinder", "sphere", "pyramid",
                                             "prism", "wedge", "torus", "stairs", "button", "label"};
static const char* ADD_NAMES[KIND_COUNT] = {"Cube", "Cone", "Disc", "Cylinder", "Sphere", "Pyramid",
                                            "Prism", "Wedge", "Torus", "Stairs", "Button", "Label"};
// vertical extent of each mesh in local space (used for placing and picking)
static const float MESH_Y0[MESH_COUNT] = {-0.5f, 0, -0.05f, 0, -0.5f, 0, 0, -0.5f, -0.15f, 0};
static const float MESH_Y1[MESH_COUNT] = {0.5f, 1, 0.05f, 1, 0.5f, 1, 1, 0.5f, 0.15f, 1};

// 3D objects use meshId 0..9. UI elements (meshId 10 button, 11 label) use pos/scale as percent of the screen.
struct Entity {
    int id; bool alive; int meshId;
    std::string name, text, script;
    V3 pos, scale; float rotY; V3 color;
    float spin, sway, shadow;  // shadow = blob shadow radius (0 = none)
    bool broken;               // script raised an error
};

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
    bool ui = meshId >= MESH_COUNT;
    Entity e;
    e.id = (int)G->ents.size() + 1; e.alive = true; e.meshId = meshId; e.broken = false;
    e.name = strField(L, 1, "name", MESH_NAMES[meshId]);
    e.text = strField(L, 1, "text", meshId == KIND_BUTTON ? "Button" : (meshId == KIND_LABEL ? "Label" : ""));
    e.script = strField(L, 1, "script", "");
    e.pos = vecField(L, 1, "pos", ui ? V3{50, 50, 0} : V3{0, 0, 0});
    e.scale = vecField(L, 1, "scale", meshId == KIND_BUTTON ? V3{20, 10, 1} : (meshId == KIND_LABEL ? V3{20, 6, 1} : V3{1, 1, 1}));
    e.color = vecField(L, 1, "color", meshId == KIND_BUTTON ? V3{0.25f, 0.45f, 0.85f} : (meshId == KIND_LABEL ? V3{1, 1, 1} : V3{0.8f, 0.8f, 0.8f}));
    e.rotY = numField(L, 1, "rot", 0);
    e.spin = numField(L, 1, "spin", 0);
    e.sway = numField(L, 1, "sway", 0);
    e.shadow = numField(L, 1, "shadow", 0);
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
static int l_destroy(lua_State* L) { if (Entity* e = entArg(L, 1)) e->alive = false; return 0; }
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

static const luaL_Reg ENGINE_FUNCS[] = {
    {"spawn", l_spawn}, {"find", l_find}, {"find_all", l_find_all}, {"destroy", l_destroy},
    {"set_pos", l_set_pos}, {"get_pos", l_get_pos}, {"move", l_move},
    {"set_scale", l_set_scale}, {"get_scale", l_get_scale},
    {"set_color", l_set_color}, {"set_rot", l_set_rot}, {"get_rot", l_get_rot},
    {"set_text", l_set_text}, {"get_text", l_get_text},
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

void Game::loadScene() {
    ents.clear();
    selected = 0;
    mode = MODE_EDIT;
    orbit = true;
    newLua();
    runFile("scene.lua");
}
void Game::startPlay() {
    snapshot = ents;
    snapYaw = yaw; snapPitch = pitch; snapDist = dist; snapTarget = target;
    mode = MODE_PLAY;
    orbit = true;
    touchDown = false; uiDownId = 0;
    status.clear();
    for (auto& e : ents) e.broken = false;
    size_t n0 = ents.size();  // objects spawned by main.lua attach their own scripts in spawn()
    newLua();
    if (!readSource("scripts/main.lua").empty() && runFile("scripts/main.lua")) {  // optional global script
        lua_getglobal(L, "start");
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) luaErr("start()", 0);
        } else lua_pop(L, 1);
    }
    for (size_t i = 0; i < n0; i++)  // scripts attached to objects / UI elements
        if (ents[i].alive && !ents[i].script.empty()) attachScript((int)i + 1);
}
void Game::stopPlay() {
    ents = snapshot;
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
    if (screen == SCREEN_MENU || codeOpen) return false;
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
    if (!sceneLoaded) {  // only the first time: the open project survives the app going to background
#ifdef GAME_MODE
        screen = SCREEN_PROJECT;
        loadScene();
        startPlay();
#else
        setupBaseDir();
        refreshProjects();
        screen = SCREEN_MENU;
#endif
        sceneLoaded = true;
    }
}
void Game::shutdown() {  // frees GL objects only; project and Lua state are kept
    if (!prog) return;
    for (auto& m : meshes) m.destroy();
    glDeleteProgram(prog); glDeleteProgram(skyProg); glDeleteVertexArrays(1, &emptyVao);
    prog = 0;
}
void Game::update(float dt) {
    t += dt;
    if (screen == SCREEN_PROJECT && mode == MODE_PLAY) {
        for (auto& e : ents) if (e.alive) e.rotY += e.spin * dt;
        if (L && !updateFailed) {
            lua_getglobal(L, "update");
            if (lua_isfunction(L, -1)) {
                lua_pushnumber(L, dt);
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) { luaErr("update()", 0); updateFailed = true; }
            } else lua_pop(L, 1);
        }
        for (size_t i = 0; i < ents.size(); i++) {  // per-object update(self, dt)
            if (!ents[i].alive || ents[i].script.empty() || ents[i].broken) continue;
            int id = ents[i].id;
            if (pushEnvFn(id, "update")) {
                lua_pushinteger(L, id); lua_pushnumber(L, dt);
                if (lua_pcall(L, 2, 0, 0) != LUA_OK) luaErr("update", id);
            }
        }
    }
    touchDX = touchDY = 0;
}

// ---- UI elements (buttons / labels): drawn with the ImGui draw list in both editor and game ----
void Game::uiRect(const Entity& e, ImVec2& a, ImVec2& b) const {
    float cx = e.pos.x / 100.f * w, cy = e.pos.y / 100.f * h;
    float hw = e.scale.x / 100.f * w * 0.5f, hh = e.scale.y / 100.f * h * 0.5f;
    a = ImVec2(cx - hw, cy - hh);
    b = ImVec2(cx + hw, cy + hh);
}
int Game::uiAt(float x, float y, bool includeLabels) const {
    for (int i = (int)ents.size() - 1; i >= 0; i--) {
        const Entity& e = ents[i];
        if (!e.alive || e.meshId < MESH_COUNT) continue;
        if (e.meshId == KIND_LABEL && !includeLabels) continue;
        ImVec2 a, b; uiRect(e, a, b);
        if (x >= a.x && x <= b.x && y >= a.y && y <= b.y) return e.id;
    }
    return 0;
}
void Game::drawEntityUI() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    for (const auto& e : ents) {
        if (!e.alive || e.meshId < MESH_COUNT) continue;
        ImVec2 a, b; uiRect(e, a, b);
        float bh = b.y - a.y;
        if (e.meshId == KIND_BUTTON) {
            bool pressed = mode == MODE_PLAY && uiDownId == e.id && touchDown;
            float k = pressed ? 0.7f : 1.f;
            ImU32 fill = IM_COL32((int)(e.color.x * 255 * k), (int)(e.color.y * 255 * k), (int)(e.color.z * 255 * k), 255);
            dl->AddRectFilled(a, b, fill, bh * 0.2f);
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 90), bh * 0.2f, 0, 2.f);
            float fp = bh * 0.45f;
            ImVec2 ts = font->CalcTextSizeA(fp, 100000.f, 0.f, e.text.c_str());
            dl->AddText(font, fp, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), e.text.c_str());
        } else {
            float fp = bh;
            ImVec2 ts = font->CalcTextSizeA(fp, 100000.f, 0.f, e.text.c_str());
            ImU32 col = IM_COL32((int)(e.color.x * 255), (int)(e.color.y * 255), (int)(e.color.z * 255), 255);
            dl->AddText(font, fp, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), col, e.text.c_str());
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
    if (screen == SCREEN_MENU) return;

    glEnable(GL_DEPTH_TEST); glEnable(GL_CULL_FACE);
    V3 eye = target + V3{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)} * dist;
    M4 vp = mul(perspective(0.9f, (float)w / h, 0.3f, 200.f), lookAt(eye, target, {0, 1, 0}));

    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uVP"), 1, GL_FALSE, vp.m);
    glUniform3f(glGetUniformLocation(prog, "uCam"), eye.x, eye.y, eye.z);
    glUniform3f(glGetUniformLocation(prog, "uSun"), 0.5f, 0.8f, 0.3f);
    glUniform3f(glGetUniformLocation(prog, "uFog"), fog.x, fog.y, fog.z);
    glUniform1f(glGetUniformLocation(prog, "uTime"), t);
    GLint uModel = glGetUniformLocation(prog, "uModel"), uColor = glGetUniformLocation(prog, "uColor");
    GLint uSway = glGetUniformLocation(prog, "uSway"), uAlpha = glGetUniformLocation(prog, "uAlpha");

    for (int pass = 0; pass < 2; pass++) {  // 0: solid, 1: blob shadows
        if (pass == 1) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE); }
        glUniform1f(uAlpha, pass == 1 ? 0.35f : 1.0f);
        for (const auto& e : ents) {
            if (!e.alive || e.meshId >= MESH_COUNT) continue;
            if (pass == 0) {
                V3 c = e.color;
                if (mode == MODE_EDIT && e.id == selected) {  // selection highlight
                    float k = 0.30f + 0.15f * std::sin(t * 6.f);
                    c = c * (1.f - k) + V3{1, 1, 1} * k;
                }
                M4 m = model(e.pos, e.rotY, e.scale);
                glUniform3f(uColor, c.x, c.y, c.z);
                glUniform1f(uSway, e.sway);
                glUniformMatrix4fv(uModel, 1, GL_FALSE, m.m);
                meshOf(e.meshId).draw();
            } else if (e.shadow > 0) {
                M4 m = model({e.pos.x, 0.02f, e.pos.z}, 0, {e.shadow, 1, e.shadow});
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
    io.FontGlobalScale = h / 520.f;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
    if (screen == SCREEN_PROJECT) drawEntityUI();
#ifndef GAME_MODE
    drawEditorUI();
#else
    if (!status.empty() && mode == MODE_PLAY)  // show script errors on screen while testing
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

void Game::setupBaseDir() {
    std::string cands[2] = {"/sdcard/Documents/3gine/game/", fallbackDir};
    for (const auto& c : cands) {
        if (c.empty()) continue;
        mkdirs(c);
        std::string probe = c + ".probe";
        if (writeFile(probe, "x")) { remove(probe.c_str()); baseDir = c; return; }
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
    return openProject(name);
}
bool Game::openProject(const std::string& name) {
    projectName = name;
    projectDir = baseDir + name + "/";
    codeOpen = false;
    status.clear();
    loadScene();
    screen = SCREEN_PROJECT;
    return true;
}
bool Game::saveProject() {
    if (projectDir.empty()) return false;
    mkdirs(projectDir + "scripts/");
    char buf[1024];
    std::string out = "-- 3gine scene. Generated by the editor (Save). Safe to hand-edit.\n";
    snprintf(buf, sizeof buf, "engine.camera{ yaw=%.3f, pitch=%.3f, dist=%.3f, target={%.3f,%.3f,%.3f} }\n",
             yaw, pitch, dist, target.x, target.y, target.z);
    out += buf;
    for (const auto& e : ents) {
        if (!e.alive) continue;
        snprintf(buf, sizeof buf,
                 "engine.spawn{ name=\"%s\", mesh=\"%s\", pos={%.3f,%.3f,%.3f}, scale={%.3f,%.3f,%.3f}, "
                 "color={%.3f,%.3f,%.3f}, rot=%.3f, spin=%.3f, sway=%.3f, shadow=%.3f, text=\"%s\", script=\"%s\" }\n",
                 esc(e.name).c_str(), MESH_NAMES[e.meshId], e.pos.x, e.pos.y, e.pos.z, e.scale.x, e.scale.y, e.scale.z,
                 e.color.x, e.color.y, e.color.z, e.rotY, e.spin, e.sway, e.shadow, esc(e.text).c_str(), esc(e.script).c_str());
        out += buf;
    }
    if (!writeFile(projectDir + "scene.lua", out)) { status = "Save failed: " + projectDir; return false; }
    status = "Saved";
    return true;
}

int Game::addEntity(int kind) {
    Entity e;
    e.id = (int)ents.size() + 1; e.alive = true; e.meshId = kind; e.broken = false;
    e.name = ADD_NAMES[kind];
    e.rotY = 0; e.spin = 0; e.sway = 0; e.shadow = 0;
    if (kind >= MESH_COUNT) {
        e.pos = {50, 50, 0};
        e.scale = kind == KIND_BUTTON ? V3{20, 10, 1} : V3{20, 6, 1};
        e.color = kind == KIND_BUTTON ? V3{0.25f, 0.45f, 0.85f} : V3{1, 1, 1};
        e.text = kind == KIND_BUTTON ? "Button" : "Label";
    } else {
        e.pos = {target.x, -MESH_Y0[kind], target.z};
        e.scale = {1, 1, 1};
        e.color = (kind == 1 || kind == 5) ? V3{0.3f, 0.6f, 0.35f} : V3{0.8f, 0.8f, 0.8f};
    }
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
    if (u) { selected = u; return; }
    V3 eye = target + V3{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)} * dist;
    V3 f = norm(target - eye), s = norm(cross(f, {0, 1, 0})), up = cross(s, f);
    float th = std::tan(0.45f), asp = (float)w / h;
    float nx = 2.f * sx / w - 1.f, ny = 1.f - 2.f * sy / h;
    V3 d = norm(f + s * (nx * th * asp) + up * (ny * th));
    float best = 1e30f; int hit = 0;
    for (const auto& e : ents) {
        if (!e.alive || e.meshId >= MESH_COUNT) continue;
        float hx = e.scale.x * 0.5f, hz = e.scale.z * 0.5f;
        if (std::fabs(e.rotY) > 1e-3f) hx = hz = 0.5f * std::sqrt(e.scale.x * e.scale.x + e.scale.z * e.scale.z);
        float y0 = e.pos.y + MESH_Y0[e.meshId] * e.scale.y, y1 = e.pos.y + MESH_Y1[e.meshId] * e.scale.y;
        float lo[3] = {e.pos.x - hx, y0, e.pos.z - hz}, hi[3] = {e.pos.x + hx, y1, e.pos.z + hz};
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
    selected = hit;
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
    ImGui::Begin("3gine - Projects", nullptr, F);
    ImGui::TextWrapped("Projects folder: %s", baseDir.empty() ? "(none)" : baseDir.c_str());
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
    ImGui::Separator();
    if (ImGui::BeginChild("plist", ImVec2(0, H * 0.5f), 0)) {
        if (projects.empty()) ImGui::TextWrapped("No projects yet. Press Create to make one.");
        for (int i = 0; i < (int)projects.size(); i++)
            if (ImGui::Selectable(projects[i].c_str(), menuSel == i)) menuSel = i;
    }
    ImGui::EndChild();
    if (ImGui::Button("Create")) doCreate = true;
    ImGui::SameLine();
    if (ImGui::Button("Open") && menuSel >= 0 && menuSel < (int)projects.size()) openProject(projects[menuSel]);
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refreshProjects();
    ImGui::End();

    if (doCreate) { nameBuf[0] = 0; ImGui::OpenPopup("New project"); }
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.03f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(W * 0.7f, 0));
    if (ImGui::BeginPopupModal("New project", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextUnformatted("Project name:");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##pname", nameBuf, sizeof nameBuf, ImGuiInputTextFlags_CharsNoBlank);
        ImGui::TextWrapped("Saved in: %s%s/", baseDir.c_str(), cleanName(nameBuf).c_str());
        if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
        if (ImGui::Button("Create")) {
            if (createProject(nameBuf)) ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { status.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void Game::drawEditorUI() {
    const ImGuiWindowFlags F = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    float W = (float)w, H = (float)h, topH = 0.12f * H;

    if (screen == SCREEN_MENU) { drawMenu(F); return; }

    if (mode == MODE_PLAY) {
        ImGui::SetNextWindowPos(ImVec2(0.86f * W, 0));
        ImGui::SetNextWindowSize(ImVec2(0.14f * W, topH));
        ImGui::Begin("##play", nullptr, F | ImGuiWindowFlags_NoTitleBar);
        if (ImGui::Button("Stop", ImVec2(-1, -1))) stopPlay();
        ImGui::End();
        if (!status.empty()) ImGui::GetForegroundDrawList()->AddText(ImVec2(10, topH), IM_COL32(255, 90, 90, 255), status.c_str());
        return;
    }

    bool doPlay = false, doBuild = false, doDup = false, doMenu = false, doNewScript = false, doEditScript = false;
    int doAdd = -1;

    // ---- top bar ----
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, topH));
    ImGui::Begin("##top", nullptr, F | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
    if (ImGui::Button("Menu")) doMenu = true;
    ImGui::SameLine(); if (ImGui::Button("Play")) doPlay = true;
    ImGui::SameLine(); if (ImGui::Button("Save")) saveProject();
    ImGui::SameLine(); if (ImGui::Button("Build APK")) doBuild = true;
    ImGui::SameLine(); if (ImGui::Button("+ Add")) ImGui::OpenPopup("addpop");
    if (ImGui::BeginPopup("addpop")) {
        for (int i = 0; i < KIND_COUNT; i++) if (ImGui::Selectable(ADD_NAMES[i])) doAdd = i;
        ImGui::EndPopup();
    }
    ImGui::SameLine(); ImGui::TextUnformatted(projectName.c_str());
    ImGui::SameLine(); ImGui::TextUnformatted(status.c_str());
    ImGui::End();

    // ---- scene list ----
    ImGui::SetNextWindowPos(ImVec2(0, topH));
    ImGui::SetNextWindowSize(ImVec2(0.25f * W, H - topH));
    ImGui::Begin("Scene", nullptr, F);
    for (const auto& e : ents) {
        if (!e.alive) continue;
        char label[160]; snprintf(label, sizeof label, "%s###e%d", e.name.c_str(), e.id);
        if (ImGui::Selectable(label, selected == e.id)) selected = e.id;
    }
    ImGui::End();

    // ---- inspector ----
    ImGui::SetNextWindowPos(ImVec2(0.70f * W, topH));
    ImGui::SetNextWindowSize(ImVec2(0.30f * W, H - topH));
    ImGui::Begin("Inspector", nullptr, F);
    Entity* se = (selected >= 1 && selected <= (int)ents.size() && ents[selected - 1].alive) ? &ents[selected - 1] : nullptr;
    if (!se) ImGui::TextWrapped("Tap an object in the scene or in the list.");
    else {
        char tb[128];
        snprintf(tb, sizeof tb, "%s", se->name.c_str());
        if (ImGui::InputText("Name", tb, sizeof tb)) se->name = tb;
        bool isUI = se->meshId >= MESH_COUNT;
        if (isUI) {
            ImGui::DragFloat2("Pos %", &se->pos.x, 0.2f, -20.f, 120.f);
            ImGui::DragFloat2("Size %", &se->scale.x, 0.2f, 1.f, 100.f);
            snprintf(tb, sizeof tb, "%s", se->text.c_str());
            if (ImGui::InputText("Text", tb, sizeof tb)) se->text = tb;
            ImGui::ColorEdit3("Color", &se->color.x);
        } else {
            ImGui::DragFloat3("Pos", &se->pos.x, 0.05f);
            ImGui::DragFloat3("Size", &se->scale.x, 0.05f, 0.01f, 500.f);
            float deg = se->rotY * 57.29578f;
            if (ImGui::SliderFloat("Rot", &deg, -180.f, 180.f)) se->rotY = deg / 57.29578f;
            ImGui::ColorEdit3("Color", &se->color.x);
            ImGui::SliderFloat("Spin", &se->spin, -5.f, 5.f);
            ImGui::SliderFloat("Sway", &se->sway, 0.f, 1.f);
            ImGui::SliderFloat("Shadow", &se->shadow, 0.f, 10.f);
        }
        ImGui::Separator();
        ImGui::Text("Script: %s", se->script.empty() ? "(none)" : se->script.c_str());
        if (ImGui::Button("New")) doNewScript = true;
        ImGui::SameLine(); if (ImGui::Button("Pick")) ImGui::OpenPopup("pickscript");
        if (!se->script.empty()) {
            ImGui::SameLine(); if (ImGui::Button("Edit")) doEditScript = true;
            ImGui::SameLine(); if (ImGui::Button("Clear")) se->script.clear();
        }
        if (ImGui::BeginPopup("pickscript")) {
            std::vector<std::string> sc = listScripts();
            if (sc.empty()) ImGui::TextUnformatted("No scripts yet - press New.");
            for (const auto& n : sc) if (ImGui::Selectable(n.c_str())) se->script = n;
            ImGui::EndPopup();
        }
        ImGui::Separator();
        if (ImGui::Button("Duplicate")) doDup = true;
        ImGui::SameLine();
        if (ImGui::Button("Delete")) { se->alive = false; selected = 0; }
    }
    ImGui::End();

    // ---- script editor (top half; the keyboard covers the lower half) ----
    if (codeOpen) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(W, 0.5f * H));
        char title[160]; snprintf(title, sizeof title, "Script: %s.lua###code", codeName.c_str());
        ImGui::Begin(title, nullptr, F);
        if (ImGui::Button("Save")) saveCode();
        ImGui::SameLine();
        if (ImGui::Button("Save & Close")) { saveCode(); codeOpen = false; }
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

    // ---- build popup ----
    if (doBuild) {
        saveProject();
        writeFile(baseDir + "startup.txt", projectName + "\n");  // tells CI which project to bundle in the game APK
        ImGui::OpenPopup("Build APK");
    }
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(W * 0.7f, 0));
    if (ImGui::BeginPopupModal("Build APK", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::TextWrapped("Project '%s' saved in:\n%s", projectName.c_str(), projectDir.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("To get the game APK: open Termux in your repo and run  sh publish.sh  "
                           "GitHub Actions then compiles it; download the artifact 3gine-game-apk from the Actions page.");
        ImGui::TextWrapped("An APK cannot be compiled on the phone itself (it needs the Android SDK/NDK toolchain), "
                           "so GitHub does the compiling.");
        if (ImGui::Button("OK", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- deferred actions (never change the entity list while iterating it) ----
    if (doAdd >= 0) selected = addEntity(doAdd);
    if (doDup && selected >= 1 && selected <= (int)ents.size()) {
        Entity c = ents[selected - 1];
        c.id = (int)ents.size() + 1;
        if (c.meshId >= MESH_COUNT) c.pos.y += 5.f; else c.pos.x += 1.f;
        ents.push_back(c); selected = c.id;
    }
    if (doNewScript && selected >= 1) newScriptFor(selected);
    if (doEditScript && selected >= 1 && selected <= (int)ents.size()) openCode(ents[selected - 1].script);
    if (doMenu) { saveProject(); codeOpen = false; refreshProjects(); screen = SCREEN_MENU; status.clear(); }
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
    ImGui::GetIO().IniFilename = nullptr;
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
    }
    if (!p->gameTouch) { p->lastX = x; p->lastY = y; return 1; }

    if (act == AMOTION_EVENT_ACTION_DOWN) {
        g.touchDown = true; g.touchX = x; g.touchY = y;
        g.uiDownId = (g.mode == MODE_PLAY) ? g.uiAt(x, y, false) : 0;
    } else if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_CANCEL) {
        g.touchDown = false;
        if (act == AMOTION_EVENT_ACTION_UP && g.mode == MODE_PLAY && g.uiDownId && g.uiAt(x, y, false) == g.uiDownId)
            g.clickUI(g.uiDownId);  // a tap on a UI button runs its on_click()
#ifndef GAME_MODE
        if (act == AMOTION_EVENT_ACTION_UP && g.mode == MODE_EDIT && p->moved < 25.f) g.pick(x, y);
#endif
        g.uiDownId = 0;
    } else if (act == AMOTION_EVENT_ACTION_MOVE) {
        g.touchDX += x - p->lastX; g.touchDY += y - p->lastY; g.touchX = x; g.touchY = y;
        p->moved += std::fabs(x - p->lastX) + std::fabs(y - p->lastY);
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
    p.game.assets = app->activity->assetManager;
    if (app->activity->externalDataPath) p.game.fallbackDir = std::string(app->activity->externalDataPath) + "/game/";
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
