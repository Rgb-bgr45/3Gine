// 3sics - mid-level physics for 3gine: gravity, AABB + sphere collisions, restitution, mass / impulse.
#pragma once
#include <vector>
#include <functional>
#include <cmath>
#include <algorithm>

#ifndef GINE_V3_DEFINED
struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
#endif

struct PBody {
    int id = 0;                 // engine object id
    bool isStatic = false;      // static bodies never move (floors, walls)
    bool sphere = false;        // false = box (AABB), true = sphere
    float mass = 1.f, invMass = 1.f, restitution = 0.3f;
    V3 pos = {0, 0, 0}, prev = {0, 0, 0}, vel = {0, 0, 0};
    V3 half = {0.5f, 0.5f, 0.5f};   // box half extents (sphere: radius = half.x)
    float radius() const { return half.x; }
};

class Physics3 {
public:
    V3 gravity = {0, -9.8f, 0};
    int substeps = 2;
    std::function<void(int, int)> onCollision;      // called after the step with the two body ids

    PBody* add(int id, V3 center, V3 half, bool sphere, float mass, float restitution, bool isStatic) {
        remove(id);
        PBody b;
        b.id = id; b.pos = b.prev = center; b.half = half; b.sphere = sphere;
        b.restitution = restitution;
        b.isStatic = isStatic || mass <= 0.f;
        b.mass = b.isStatic ? 0.f : mass;
        b.invMass = b.isStatic ? 0.f : 1.f / mass;
        bodies_.push_back(b);
        return &bodies_.back();
    }
    void remove(int id) { bodies_.erase(std::remove_if(bodies_.begin(), bodies_.end(), [id](const PBody& b) { return b.id == id; }), bodies_.end()); }
    void clear() { bodies_.clear(); events_.clear(); touching_.clear(); lastTouching_.clear(); }
    PBody* get(int id) { for (auto& b : bodies_) if (b.id == id) return &b; return nullptr; }
    std::vector<PBody>& bodies() { return bodies_; }

    void impulse(int id, V3 j) {                    // instant push: v += j / mass
        if (PBody* b = get(id)) if (!b->isStatic) b->vel = b->vel + j * b->invMass;
    }

    void step(float dt) {
        if (dt > 0.05f) dt = 0.05f;
        for (auto& b : bodies_) b.prev = b.pos;
        float h = dt / (float)substeps;
        for (int s = 0; s < substeps; s++) {
            for (auto& b : bodies_) {
                if (b.isStatic) continue;
                b.vel = b.vel + gravity * h;        // gravity
                b.pos = b.pos + b.vel * h;          // integrate
            }
            for (size_t i = 0; i + 1 < bodies_.size(); i++)
                for (size_t j = i + 1; j < bodies_.size(); j++) {
                    PBody& a = bodies_[i]; PBody& b = bodies_[j];
                    if (a.isStatic && b.isStatic) continue;
                    V3 n; float depth;
                    if (collide(a, b, n, depth)) {
                        resolve(a, b, n, depth);
                        std::pair<int, int> key = {std::min(a.id, b.id), std::max(a.id, b.id)};
                        touching_.push_back(key);
                        if (std::find(lastTouching_.begin(), lastTouching_.end(), key) == lastTouching_.end() &&
                            std::find(events_.begin(), events_.end(), key) == events_.end())
                            events_.push_back(key);                 // only NEW contacts raise an event
                    }
                }
        }
        lastTouching_.swap(touching_);
        touching_.clear();
        if (onCollision) for (auto& e : events_) onCollision(e.first, e.second);
        events_.clear();
    }

private:
    std::vector<PBody> bodies_;
    std::vector<std::pair<int, int>> events_, touching_, lastTouching_;

    // true when a and b overlap; n points from a to b, depth = penetration
    static bool collide(const PBody& a, const PBody& b, V3& n, float& depth) {
        V3 d = b.pos - a.pos;
        if (a.sphere && b.sphere) {
            float rr = a.radius() + b.radius(), len = std::sqrt(dot(d, d));
            if (len >= rr) return false;
            n = len > 1e-6f ? d * (1.f / len) : V3{0, 1, 0};
            depth = rr - len;
            return true;
        }
        if (a.sphere || b.sphere) {
            const PBody& s = a.sphere ? a : b;
            const PBody& bx = a.sphere ? b : a;
            V3 c = {std::max(bx.pos.x - bx.half.x, std::min(s.pos.x, bx.pos.x + bx.half.x)),
                    std::max(bx.pos.y - bx.half.y, std::min(s.pos.y, bx.pos.y + bx.half.y)),
                    std::max(bx.pos.z - bx.half.z, std::min(s.pos.z, bx.pos.z + bx.half.z))};
            V3 v = s.pos - c;                      // box -> sphere
            float d2 = dot(v, v), r = s.radius();
            if (d2 > r * r) return false;
            V3 nb;                                 // normal box -> sphere
            if (d2 > 1e-10f) { float len = std::sqrt(d2); nb = v * (1.f / len); depth = r - len; }
            else {                                 // centre inside the box: leave through the nearest face
                float ox = bx.half.x - std::fabs(s.pos.x - bx.pos.x), oy = bx.half.y - std::fabs(s.pos.y - bx.pos.y),
                      oz = bx.half.z - std::fabs(s.pos.z - bx.pos.z);
                if (ox < oy && ox < oz) { nb = {s.pos.x >= bx.pos.x ? 1.f : -1.f, 0, 0}; depth = ox + r; }
                else if (oy < oz) { nb = {0, s.pos.y >= bx.pos.y ? 1.f : -1.f, 0}; depth = oy + r; }
                else { nb = {0, 0, s.pos.z >= bx.pos.z ? 1.f : -1.f}; depth = oz + r; }
            }
            n = a.sphere ? nb * -1.f : nb;         // we want a -> b
            return true;
        }
        float ox = (a.half.x + b.half.x) - std::fabs(d.x), oy = (a.half.y + b.half.y) - std::fabs(d.y),
              oz = (a.half.z + b.half.z) - std::fabs(d.z);
        if (ox <= 0 || oy <= 0 || oz <= 0) return false;
        if (ox < oy && ox < oz) { n = {d.x >= 0 ? 1.f : -1.f, 0, 0}; depth = ox; }
        else if (oy < oz) { n = {0, d.y >= 0 ? 1.f : -1.f, 0}; depth = oy; }
        else { n = {0, 0, d.z >= 0 ? 1.f : -1.f}; depth = oz; }
        return true;
    }

    static void resolve(PBody& a, PBody& b, V3 n, float depth) {
        float sum = a.invMass + b.invMass;
        if (sum <= 0.f) return;
        float corr = std::max(depth - 0.005f, 0.f) / sum * 0.8f;     // push apart (with a small slop)
        a.pos = a.pos - n * (corr * a.invMass);
        b.pos = b.pos + n * (corr * b.invMass);
        float vn = dot(b.vel - a.vel, n);
        if (vn > 0.f) return;                                         // already separating
        float e = std::min(a.restitution, b.restitution);
        if (-vn < 1.0f) e = 0.f;                                      // resting contact: no jitter
        float j = -(1.f + e) * vn / sum;                              // impulse magnitude
        a.vel = a.vel - n * (j * a.invMass);
        b.vel = b.vel + n * (j * b.invMass);
        if (std::fabs(n.y) > 0.5f) {                                  // light ground friction
            if (!a.isStatic) { a.vel.x *= 0.98f; a.vel.z *= 0.98f; }
            if (!b.isStatic) { b.vel.x *= 0.98f; b.vel.z *= 0.98f; }
        }
    }
};
