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
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#ifndef GAME_MODE
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"
#endif

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

struct Entity {
    int id; bool alive; int meshId;  // meshId: 0 cube, 1 cone, 2 disc
    std::string name;
    V3 pos, scale; float rotY; V3 color;
    float spin, sway, shadow;  // shadow = blob shadow radius (0 = none)
};
static const char* MESH_NAMES[3] = {"cube", "cone", "disc"};

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

struct Game {
    GLuint prog = 0, skyProg = 0, emptyVao = 0;
    Mesh cube, cone, disc;
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
    // scripting
    lua_State* L = nullptr;
    AAssetManager* assets = nullptr;
    bool updateFailed = false;
    // editor
    Mode mode = MODE_EDIT;
    bool sceneLoaded = false;
    std::vector<Entity> snapshot;
    float snapYaw = 0, snapPitch = 0, snapDist = 0; V3 snapTarget = {0, 0, 0};
    int selected = 0;
    std::string projectDir, fallbackDir, status;

    const Mesh& meshOf(int id) const { return id == 1 ? cone : (id == 2 ? disc : cube); }
    void init();
    void shutdown();
    void update(float dt);
    void render();
    std::string readAsset(const char* path);
    std::string readSource(const char* path);
    bool runFile(const char* path);
    void newLua();
    void loadScene();
    void startPlay();
    void stopPlay();
    bool inViewport(float x, float y) const;
    // editor only
    void setupProjectDir();
    bool saveProject();
    void pick(float sx, float sy);
    int addEntity(int meshId, const char* name);
    void drawUI();
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
static Entity* entArg(lua_State* L, int i) {
    int id = (int)luaL_checkinteger(L, i);
    if (id < 1 || id > (int)G->ents.size() || !G->ents[id - 1].alive) return nullptr;
    return &G->ents[id - 1];
}

// engine.spawn{ name=, mesh="cube"|"cone"|"disc", pos=, scale=, color=, rot=, spin=, sway=, shadow= } -> id
static int l_spawn(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "mesh");
    const char* mn = lua_isstring(L, -1) ? lua_tostring(L, -1) : "cube";
    int meshId = 0;
    if (!strcmp(mn, "cone")) meshId = 1;
    else if (!strcmp(mn, "disc")) meshId = 2;
    lua_pop(L, 1);
    Entity e;
    e.id = (int)G->ents.size() + 1; e.alive = true; e.meshId = meshId;
    lua_getfield(L, 1, "name");
    if (lua_isstring(L, -1)) e.name = lua_tostring(L, -1);
    else e.name = MESH_NAMES[meshId];
    lua_pop(L, 1);
    e.pos = vecField(L, 1, "pos", {0, 0, 0});
    e.scale = vecField(L, 1, "scale", {1, 1, 1});
    e.color = vecField(L, 1, "color", {0.8f, 0.8f, 0.8f});
    e.rotY = numField(L, 1, "rot", 0);
    e.spin = numField(L, 1, "spin", 0);
    e.sway = numField(L, 1, "sway", 0);
    e.shadow = numField(L, 1, "shadow", 0);
    G->ents.push_back(e);
    lua_pushinteger(L, e.id);
    return 1;
}
static int l_find(lua_State* L) {
    const char* n = luaL_checkstring(L, 1);
    for (const auto& e : G->ents)
        if (e.alive && e.name == n) { lua_pushinteger(L, e.id); return 1; }
    return 0;
}
static int l_destroy(lua_State* L) { if (Entity* e = entArg(L, 1)) e->alive = false; return 0; }
static int l_set_pos(lua_State* L) {
    if (Entity* e = entArg(L, 1)) e->pos = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4)};
    return 0;
}
static int l_get_pos(lua_State* L) {
    Entity* e = entArg(L, 1);
    if (!e) return 0;
    lua_pushnumber(L, e->pos.x); lua_pushnumber(L, e->pos.y); lua_pushnumber(L, e->pos.z);
    return 3;
}
static int l_set_scale(lua_State* L) {
    if (Entity* e = entArg(L, 1)) e->scale = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4)};
    return 0;
}
static int l_set_color(lua_State* L) {
    if (Entity* e = entArg(L, 1)) e->color = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4)};
    return 0;
}
static int l_set_rot(lua_State* L) { if (Entity* e = entArg(L, 1)) e->rotY = (float)luaL_checknumber(L, 2); return 0; }
static int l_get_rot(lua_State* L) { Entity* e = entArg(L, 1); if (!e) return 0; lua_pushnumber(L, e->rotY); return 1; }
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
    {"spawn", l_spawn}, {"find", l_find}, {"destroy", l_destroy},
    {"set_pos", l_set_pos}, {"get_pos", l_get_pos}, {"set_scale", l_set_scale},
    {"set_color", l_set_color}, {"set_rot", l_set_rot}, {"get_rot", l_get_rot},
    {"camera", l_camera}, {"set_orbit", l_set_orbit}, {"touch", l_touch},
    {"screen", l_screen}, {"time", l_time}, {"log", l_log}, {"load", l_load},
    {nullptr, nullptr}};

// ---- project files ----
std::string Game::readAsset(const char* path) {
    std::string out;
    if (!assets) return out;
    AAsset* a = AAssetManager_open(assets, path, AASSET_MODE_BUFFER);
    if (!a) return out;
    out.assign((const char*)AAsset_getBuffer(a), AAsset_getLength(a));
    AAsset_close(a);
    return out;
}
// Editor: project folder on shared storage first, bundled asset as fallback. Game build: assets only.
std::string Game::readSource(const char* path) {
    std::string out;
    if (!projectDir.empty() && readFile(projectDir + path, out)) return out;
    return readAsset(path);
}
bool Game::runFile(const char* path) {
    std::string src = readSource(path);
    if (src.empty()) { LOG("script not found: %s", path); return false; }
    if (luaL_loadbufferx(L, src.data(), src.size(), path, "t") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        LOG("lua error in %s: %s", path, lua_tostring(L, -1));
        status = std::string("Lua error in ") + path + ": " + lua_tostring(L, -1);
        lua_pop(L, 1);
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
    updateFailed = false;
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
    touchDown = false;
    newLua();
    if (runFile("scripts/main.lua")) {
        lua_getglobal(L, "start");
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                LOG("lua start(): %s", lua_tostring(L, -1));
                status = std::string("start(): ") + lua_tostring(L, -1);
                lua_pop(L, 1);
            }
        } else lua_pop(L, 1);
    }
}
void Game::stopPlay() {
    ents = snapshot;
    yaw = snapYaw; pitch = snapPitch; dist = snapDist; target = snapTarget;
    if (L) { lua_close(L); L = nullptr; }
    mode = MODE_EDIT;
    orbit = true;
    touchDown = false;
    selected = 0;
}
bool Game::inViewport(float x, float y) const {
#ifdef GAME_MODE
    (void)x; (void)y; return true;
#else
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
    cube = makeCube(); cube.upload();
    cone = makeCone(7); cone.upload();
    disc = makeDisc(14); disc.upload();
    if (!sceneLoaded) {  // only the first time: the scene survives the app going to background
#ifdef GAME_MODE
        loadScene();
        startPlay();
#else
        setupProjectDir();
        loadScene();
#endif
        sceneLoaded = true;
    }
}
void Game::shutdown() {  // frees GL objects only; scene and Lua state are kept
    if (!prog) return;
    cube.destroy(); cone.destroy(); disc.destroy();
    glDeleteProgram(prog); glDeleteProgram(skyProg); glDeleteVertexArrays(1, &emptyVao);
    prog = 0;
}
void Game::update(float dt) {
    t += dt;
    if (mode == MODE_PLAY) {
        for (auto& e : ents) if (e.alive) e.rotY += e.spin * dt;
        if (L && !updateFailed) {
            lua_getglobal(L, "update");
            if (lua_isfunction(L, -1)) {
                lua_pushnumber(L, dt);
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                    LOG("lua update(): %s", lua_tostring(L, -1));
                    status = std::string("update(): ") + lua_tostring(L, -1);
                    lua_pop(L, 1);
                    updateFailed = true;
                }
            } else lua_pop(L, 1);
        }
    }
    touchDX = touchDY = 0;
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
            if (!e.alive) continue;
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
                disc.draw();
            }
        }
    }
    glDisable(GL_BLEND); glDepthMask(GL_TRUE);
}

#ifndef GAME_MODE
// ---------------------------------------------------------------- editor
void Game::setupProjectDir() {
    std::string cands[2] = {"/sdcard/Documents/3gine/game/", fallbackDir};
    for (const auto& c : cands) {
        if (c.empty()) continue;
        mkdirs(c);
        std::string probe = c + ".probe";
        if (writeFile(probe, "x")) { remove(probe.c_str()); projectDir = c; return; }
    }
    status = "No writable project folder. Grant 'All files access' to 3gine Editor in Android settings.";
}

bool Game::saveProject() {
    if (projectDir.empty()) setupProjectDir();
    if (projectDir.empty()) return false;
    mkdirs(projectDir + "scripts/");
    char buf[640];
    std::string out = "-- 3gine scene. Generated by the editor (Save). Safe to hand-edit.\n";
    snprintf(buf, sizeof buf, "engine.camera{ yaw=%.3f, pitch=%.3f, dist=%.3f, target={%.3f,%.3f,%.3f} }\n",
             yaw, pitch, dist, target.x, target.y, target.z);
    out += buf;
    for (const auto& e : ents) {
        if (!e.alive) continue;
        std::string nm = e.name;
        for (auto& ch : nm) if (ch == '"' || ch == '\\' || ch == '\n') ch = '\'';
        snprintf(buf, sizeof buf,
                 "engine.spawn{ name=\"%s\", mesh=\"%s\", pos={%.3f,%.3f,%.3f}, scale={%.3f,%.3f,%.3f}, "
                 "color={%.3f,%.3f,%.3f}, rot=%.3f, spin=%.3f, sway=%.3f, shadow=%.3f }\n",
                 nm.c_str(), MESH_NAMES[e.meshId], e.pos.x, e.pos.y, e.pos.z, e.scale.x, e.scale.y, e.scale.z,
                 e.color.x, e.color.y, e.color.z, e.rotY, e.spin, e.sway, e.shadow);
        out += buf;
    }
    if (!writeFile(projectDir + "scene.lua", out)) { status = "Save failed: " + projectDir; return false; }
    if (access((projectDir + "scripts/main.lua").c_str(), F_OK) != 0) {
        std::string def = readAsset("scripts/main.lua");
        if (!def.empty()) writeFile(projectDir + "scripts/main.lua", def);
    }
    status = "Saved";
    return true;
}

int Game::addEntity(int meshId, const char* name) {
    Entity e;
    e.id = (int)ents.size() + 1; e.alive = true; e.meshId = meshId;
    char nm[64]; snprintf(nm, sizeof nm, "%s %d", name, e.id);
    e.name = nm;
    float y = meshId == 0 ? 0.5f : (meshId == 1 ? 0.f : 0.02f);
    e.pos = {target.x, y, target.z};
    e.scale = {1, 1, 1};
    e.color = meshId == 1 ? V3{0.3f, 0.6f, 0.35f} : V3{0.8f, 0.8f, 0.8f};
    e.rotY = 0; e.spin = 0; e.sway = 0; e.shadow = 0;
    ents.push_back(e);
    return e.id;
}

void Game::pick(float sx, float sy) {
    V3 eye = target + V3{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)} * dist;
    V3 f = norm(target - eye), s = norm(cross(f, {0, 1, 0})), u = cross(s, f);
    float th = std::tan(0.45f), asp = (float)w / h;
    float nx = 2.f * sx / w - 1.f, ny = 1.f - 2.f * sy / h;
    V3 d = norm(f + s * (nx * th * asp) + u * (ny * th));
    float best = 1e30f; int hit = 0;
    for (const auto& e : ents) {
        if (!e.alive) continue;
        float hx = e.scale.x * 0.5f, hz = e.scale.z * 0.5f;
        if (std::fabs(e.rotY) > 1e-3f) hx = hz = 0.5f * std::sqrt(e.scale.x * e.scale.x + e.scale.z * e.scale.z);
        float y0, y1;
        if (e.meshId == 0) { y0 = e.pos.y - e.scale.y * 0.5f; y1 = e.pos.y + e.scale.y * 0.5f; }
        else if (e.meshId == 1) { y0 = e.pos.y; y1 = e.pos.y + e.scale.y; }
        else { y0 = e.pos.y - 0.05f; y1 = e.pos.y + 0.05f; }
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

void Game::drawUI() {
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = h / 520.f;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    const ImGuiWindowFlags F = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    float W = (float)w, H = (float)h, topH = 0.12f * H;
    bool doPlay = false, doBuild = false, doDup = false;
    int doAdd = -1;

    if (mode == MODE_PLAY) {
        ImGui::SetNextWindowPos(ImVec2(0.86f * W, 0));
        ImGui::SetNextWindowSize(ImVec2(0.14f * W, topH));
        ImGui::Begin("##play", nullptr, F | ImGuiWindowFlags_NoTitleBar);
        if (ImGui::Button("Stop", ImVec2(-1, -1))) stopPlay();
        ImGui::End();
    } else {
        // top bar
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(W, topH));
        ImGui::Begin("##top", nullptr, F | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
        if (ImGui::Button("Play")) doPlay = true;
        ImGui::SameLine(); if (ImGui::Button("Save")) saveProject();
        ImGui::SameLine(); if (ImGui::Button("Build APK")) doBuild = true;
        ImGui::SameLine(); ImGui::TextUnformatted("|");
        ImGui::SameLine(); if (ImGui::Button("+Cube")) doAdd = 0;
        ImGui::SameLine(); if (ImGui::Button("+Cone")) doAdd = 1;
        ImGui::SameLine(); if (ImGui::Button("+Disc")) doAdd = 2;
        ImGui::SameLine(); ImGui::TextUnformatted(status.c_str());
        ImGui::End();

        // hierarchy
        ImGui::SetNextWindowPos(ImVec2(0, topH));
        ImGui::SetNextWindowSize(ImVec2(0.25f * W, H - topH));
        ImGui::Begin("Scene", nullptr, F);
        for (const auto& e : ents) {
            if (!e.alive) continue;
            char label[96]; snprintf(label, sizeof label, "%s###e%d", e.name.c_str(), e.id);
            if (ImGui::Selectable(label, selected == e.id)) selected = e.id;
        }
        ImGui::End();

        // inspector
        ImGui::SetNextWindowPos(ImVec2(0.70f * W, topH));
        ImGui::SetNextWindowSize(ImVec2(0.30f * W, H - topH));
        ImGui::Begin("Inspector", nullptr, F);
        Entity* se = (selected >= 1 && selected <= (int)ents.size() && ents[selected - 1].alive) ? &ents[selected - 1] : nullptr;
        if (!se) ImGui::TextWrapped("Tap an object in the scene or in the list.");
        else {
            ImGui::TextUnformatted(se->name.c_str());
            ImGui::Separator();
            ImGui::DragFloat3("Pos", &se->pos.x, 0.05f);
            ImGui::DragFloat3("Size", &se->scale.x, 0.05f, 0.01f, 500.f);
            float deg = se->rotY * 57.29578f;
            if (ImGui::SliderFloat("Rot", &deg, -180.f, 180.f)) se->rotY = deg / 57.29578f;
            ImGui::ColorEdit3("Color", &se->color.x);
            ImGui::SliderFloat("Spin", &se->spin, -5.f, 5.f);
            ImGui::SliderFloat("Sway", &se->sway, 0.f, 1.f);
            ImGui::SliderFloat("Shadow", &se->shadow, 0.f, 10.f);
            if (ImGui::Button("Duplicate")) doDup = true;
            ImGui::SameLine();
            if (ImGui::Button("Delete")) { se->alive = false; selected = 0; }
        }
        ImGui::End();
    }

    if (doBuild) { saveProject(); ImGui::OpenPopup("Build APK"); }
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(W * 0.7f, 0));
    if (ImGui::BeginPopupModal("Build APK", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::TextWrapped("Project folder:\n%s", projectDir.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("To get the game APK: open Termux in your repo and run  sh publish.sh  "
                           "GitHub Actions then compiles the game and the APK appears on the Actions page "
                           "(artifact: 3gine-game-apk).");
        ImGui::TextWrapped("An APK cannot be compiled on the phone itself (it needs the Android SDK/NDK toolchain), "
                           "so GitHub does the compiling.");
        if (ImGui::Button("OK", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // deferred actions (never change the entity list while iterating it)
    if (doAdd >= 0) { static const char* N[3] = {"Cube", "Cone", "Disc"}; selected = addEntity(doAdd, N[doAdd]); }
    if (doDup && selected >= 1 && selected <= (int)ents.size()) {
        Entity c = ents[selected - 1];
        c.id = (int)ents.size() + 1; c.name += " copy"; c.pos.x += 1.f;
        ents.push_back(c); selected = c.id;
    }
    if (doPlay) startPlay();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
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
    float lastX = 0, lastY = 0, lastPinch = 0, dragDist = 0;
    bool touchOnUI = false, imgui = false;
};

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
    p.game.init();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    float sc = (float)p.game.h / 430.f; sc = sc < 1.8f ? 1.8f : (sc > 4.5f ? 4.5f : sc);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(sc);
    ImGui::GetStyle().WindowRounding = 8.f;
    ImGui::GetStyle().Alpha = 0.95f;
    io.FontGlobalScale = sc;
    ImGui_ImplAndroid_Init(p.app->window);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    p.imgui = true;
    return true;
}
static void termGL(Platform& p) {
    if (p.dpy == EGL_NO_DISPLAY) return;
    if (p.imgui) {
        ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplAndroid_Shutdown(); ImGui::DestroyContext();
        p.imgui = false;
    }
    p.game.shutdown();
    eglMakeCurrent(p.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (p.ctx != EGL_NO_CONTEXT) eglDestroyContext(p.dpy, p.ctx);
    if (p.surf != EGL_NO_SURFACE) eglDestroySurface(p.dpy, p.surf);
    eglTerminate(p.dpy);
    p.dpy = EGL_NO_DISPLAY; p.ctx = EGL_NO_CONTEXT; p.surf = EGL_NO_SURFACE;
}

static void onCmd(android_app* app, int32_t cmd) {
    auto* p = (Platform*)app->userData;
    if (cmd == APP_CMD_INIT_WINDOW) { if (app->window) p->ready = initGL(*p); }
    else if (cmd == APP_CMD_TERM_WINDOW) { p->ready = false; termGL(*p); }
}

static int32_t onInput(android_app* app, AInputEvent* e) {
    if (AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) return 0;
    auto* p = (Platform*)app->userData;
    Game& g = p->game;
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

    if (act == AMOTION_EVENT_ACTION_DOWN) { g.touchDown = true; g.touchX = x; g.touchY = y; }
    else if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_CANCEL) {
        g.touchDown = false;
#ifndef GAME_MODE
        if (act == AMOTION_EVENT_ACTION_UP && g.mode == MODE_EDIT && p->moved < 25.f) g.pick(x, y);
#endif
    } else if (act == AMOTION_EVENT_ACTION_MOVE) {
        g.touchDX += x - p->lastX; g.touchDY += y - p->lastY; g.touchX = x; g.touchY = y;
        p->moved += std::fabs(x - p->lastX) + std::fabs(y - p->lastY);
    }

    if (g.orbit) {
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
    p.game.internalDir = app->activity->internalDataPath;
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
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplAndroid_NewFrame();
        ImGui::NewFrame();
        p.game.update(dt);
        p.game.ui();
        ImGui::Render();
        p.game.render();
#ifndef GAME_MODE
        p.game.drawUI();
#endif
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(p.dpy, p.surf);
    }
    termGL(p);
}
