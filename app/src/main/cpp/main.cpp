// 3gine - tiny stylized low-poly 3D engine: C++17, NativeActivity, EGL, GLES 3.0
// Look: flat shading, banded (toon) light, hemisphere ambient, rim, fog. No PBR, no shadows.
#include <android_native_app_glue.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cmath>
#include <ctime>
#include <vector>

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
struct Entity { const Mesh* mesh; V3 pos; V3 scale; float rotY; V3 color; float spin = 0; float sway = 0; int kind = 0; };  // kind 1 = blob shadow

struct Game {
    // GL resources (recreated whenever the context is recreated)
    GLuint prog = 0, skyProg = 0, emptyVao = 0;
    Mesh cube, cone, disc;
    std::vector<Entity> ents;
    int w = 1, h = 1;
    // camera
    float yaw = 0.6f, pitch = 0.45f, dist = 22.f;
    float t = 0;

    void init() {
        prog = makeProgram();
        skyProg = makeProgram(SKY_VS, SKY_FS);
        glGenVertexArrays(1, &emptyVao);
        cube = makeCube(); cube.upload();
        cone = makeCone(7); cone.upload();
        disc = makeDisc(14); disc.upload();
        ents.clear();

        ents.push_back({&cube, {0, -0.5f, 0}, {160, 1, 160}, 0, {0.42f, 0.66f, 0.38f}, 0});

        unsigned seed = 12345;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.f; };
        for (int i = 0; i < 70; i++) {
            float x = (rnd() - 0.5f) * 90, z = (rnd() - 0.5f) * 90;
            if (std::fabs(x) < 5 && std::fabs(z) < 5) continue;
            float s = 0.8f + rnd() * 1.2f;
            ents.push_back({&cube, {x, 0.6f * s, z}, {0.35f * s, 1.2f * s, 0.35f * s}, 0, {0.45f, 0.30f, 0.20f}, 0});
            float g = 0.85f + rnd() * 0.3f;
            ents.push_back({&cone, {x, 1.0f * s, z}, {2.2f * s, 2.2f * s, 2.2f * s}, rnd() * 6.f, {0.22f * g, 0.50f * g, 0.30f * g}, 0, 0.35f});
            ents.push_back({&cone, {x, 2.1f * s, z}, {1.6f * s, 1.8f * s, 1.6f * s}, rnd() * 6.f, {0.26f * g, 0.58f * g, 0.34f * g}, 0, 0.45f});
            ents.push_back({&disc, {x, 0.02f, z}, {2.6f * s, 1, 2.6f * s}, 0, {0.05f, 0.12f, 0.08f}, 0, 0, 1});
        }
        for (int i = 0; i < 25; i++) {
            float x = (rnd() - 0.5f) * 80, z = (rnd() - 0.5f) * 80;
            float s = 0.4f + rnd() * 0.9f;
            ents.push_back({&cube, {x, 0.3f * s, z}, {s, 0.6f * s, s * 0.8f}, rnd() * 6.f, {0.55f, 0.56f, 0.60f}, 0});
            ents.push_back({&disc, {x, 0.02f, z}, {s * 1.6f, 1, s * 1.5f}, 0, {0.05f, 0.12f, 0.08f}, 0, 0, 1});
        }
        // centrepiece
        ents.push_back({&cube, {0, 1.6f, 0}, {1.8f, 1.8f, 1.8f}, 0, {0.95f, 0.75f, 0.25f}, 0.8f});
        ents.push_back({&cone, {0, 0.0f, 0}, {3.5f, 1.2f, 3.5f}, 0, {0.80f, 0.80f, 0.85f}, 0});
        ents.push_back({&disc, {0, 0.02f, 0}, {4.2f, 1, 4.2f}, 0, {0.05f, 0.12f, 0.08f}, 0, 0, 1});
    }
    void shutdown() {
        if (!prog) return;
        cube.destroy(); cone.destroy(); disc.destroy();
        glDeleteProgram(skyProg); glDeleteVertexArrays(1, &emptyVao);
        glDeleteProgram(prog); prog = 0;
    }

    void update(float dt) {
        t += dt;
        for (auto& e : ents) e.rotY += e.spin * dt;
    }

    void render() {
        const V3 fog = {0.72f, 0.82f, 0.92f};
        glViewport(0, 0, w, h);
        glClearColor(fog.x, fog.y, fog.z, 1);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // sky gradient
        glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
        glUseProgram(skyProg);
        glBindVertexArray(emptyVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glEnable(GL_DEPTH_TEST); glEnable(GL_CULL_FACE);
        V3 target = {0, 1.5f, 0};
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

        for (int pass = 0; pass < 2; pass++) {  // pass 0: solid, pass 1: blob shadows
            if (pass == 1) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE); }
            glUniform1f(uAlpha, pass == 1 ? 0.35f : 1.0f);
            for (const auto& e : ents) {
                if (e.kind != pass) continue;
                M4 m = model(e.pos, e.rotY, e.scale);
                glUniformMatrix4fv(uModel, 1, GL_FALSE, m.m);
                glUniform3f(uColor, e.color.x, e.color.y, e.color.z);
                glUniform1f(uSway, e.sway);
                e.mesh->draw();
            }
        }
        glDisable(GL_BLEND); glDepthMask(GL_TRUE);
    }
};

// ---------------------------------------------------------------- platform
struct Platform {
    android_app* app;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
    Game game;
    bool ready = false;
    float lastX = 0, lastY = 0, lastPinch = 0;
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
    return true;
}
static void termGL(Platform& p) {
    if (p.dpy == EGL_NO_DISPLAY) return;
    eglMakeCurrent(p.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    p.game.shutdown();
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
    int act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    size_t cnt = AMotionEvent_getPointerCount(e);
    float x = AMotionEvent_getX(e, 0), y = AMotionEvent_getY(e, 0);
    if (cnt >= 2) {  // pinch = zoom
        float dx = AMotionEvent_getX(e, 1) - x, dy = AMotionEvent_getY(e, 1) - y;
        float d = std::sqrt(dx * dx + dy * dy);
        if (act == AMOTION_EVENT_ACTION_MOVE && p->lastPinch > 0) {
            g.dist -= (d - p->lastPinch) * 0.03f;
            g.dist = g.dist < 6.f ? 6.f : (g.dist > 60.f ? 60.f : g.dist);
        }
        p->lastPinch = d;
    } else {
        p->lastPinch = 0;
        if (act == AMOTION_EVENT_ACTION_MOVE) {
            g.yaw -= (x - p->lastX) * 0.005f;
            g.pitch += (y - p->lastY) * 0.005f;
            g.pitch = g.pitch < 0.1f ? 0.1f : (g.pitch > 1.4f ? 1.4f : g.pitch);
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
        eglSwapBuffers(p.dpy, p.surf);
    }
    termGL(p);
}
