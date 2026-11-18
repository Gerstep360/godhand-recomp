#include "pc_collision.h"
#include "raymath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace godhand::pc
{
    namespace
    {
        constexpr float kBodyRadius = 0.30f;
        constexpr float kStepHeight = 0.28f;
        constexpr float kTimeStep = 1.0f / 120.0f;

        bool rayIntersectsBox(Ray ray, BoundingBox box, float distance)
        {
            const auto origin = Vector3ToFloatV(ray.position);
            const auto direction = Vector3ToFloatV(ray.direction);
            const auto lower = Vector3ToFloatV(box.min);
            const auto upper = Vector3ToFloatV(box.max);
            float first = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float speed = direction.v[axis];
                const float pastMin = lower.v[axis] - origin.v[axis];
                const float pastMax = upper.v[axis] - origin.v[axis];
                if (std::abs(speed) < 1e-7f)
                {
                    if (pastMin > 0 || pastMax < 0)
                        return false;
                    continue;
                }
                const float a = pastMin / speed;
                const float b = pastMax / speed;
                first = std::max(first, std::min(a, b));
                distance = std::min(distance, std::max(a, b));
                if (first > distance)
                    return false;
            }
            return true;
        }

        Vector3 nearestOnTriangle(Vector3 point, Vector3 a, Vector3 b, Vector3 c)
        {
            const auto ab = Vector3Subtract(b, a);
            const auto ac = Vector3Subtract(c, a);
            const auto ap = Vector3Subtract(point, a);
            const float d1 = Vector3DotProduct(ab, ap);
            const float d2 = Vector3DotProduct(ac, ap);
            if (d1 <= 0.0f && d2 <= 0.0f)
                return a;

            const auto bp = Vector3Subtract(point, b);
            const float d3 = Vector3DotProduct(ab, bp);
            const float d4 = Vector3DotProduct(ac, bp);
            if (d3 >= 0.0f && d4 <= d3)
                return b;

            const float vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
                return Vector3Add(a, Vector3Scale(ab, d1 / (d1 - d3)));

            const auto cp = Vector3Subtract(point, c);
            const float d5 = Vector3DotProduct(ab, cp);
            const float d6 = Vector3DotProduct(ac, cp);
            if (d6 >= 0.0f && d5 <= d6)
                return c;

            const float vb = d5 * d2 - d1 * d6;
            if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
                return Vector3Add(a, Vector3Scale(ac, d2 / (d2 - d6)));

            const float va = d3 * d6 - d5 * d4;
            if (va <= 0.0f && d4 >= d3 && d5 >= d6)
                return Vector3Lerp(b, c, (d4 - d3) / ((d4 - d3) + (d5 - d6)));

            return Vector3Add(a, Vector3Scale(Vector3Add(Vector3Scale(ab, vb), Vector3Scale(ac, vc)),
                                              1.0f / (va + vb + vc)));
        }
    }

    void CollisionWorld::add(const Mesh &mesh, Matrix transform)
    {
        if (!mesh.vertices || mesh.triangleCount <= 0)
            return;
        const float max = std::numeric_limits<float>::max();
        Surface surface{{{max, max, max}, {-max, -max, -max}}, {}};
        surface.triangles.reserve(mesh.triangleCount);
        for (int i = 0; i < mesh.triangleCount; ++i)
        {
            std::array<Vector3, 3> point{};
            for (int corner = 0; corner < 3; ++corner)
            {
                const int index = mesh.indices ? mesh.indices[i * 3 + corner] : i * 3 + corner;
                const float *v = mesh.vertices + index * 3;
                point[corner] = Vector3Transform({v[0], v[1], v[2]}, transform);
            }
            const auto normal = Vector3CrossProduct(Vector3Subtract(point[1], point[0]), Vector3Subtract(point[2], point[0]));
            if (Vector3LengthSqr(normal) < 1e-10f)
                continue;
            for (const auto p : point)
            {
                surface.bounds.min = {std::min(surface.bounds.min.x, p.x), std::min(surface.bounds.min.y, p.y), std::min(surface.bounds.min.z, p.z)};
                surface.bounds.max = {std::max(surface.bounds.max.x, p.x), std::max(surface.bounds.max.y, p.y), std::max(surface.bounds.max.z, p.z)};
            }
            surface.triangles.push_back({point[0], point[1], point[2], Vector3Normalize(normal)});
        }
        if (!surface.triangles.empty())
            m_surfaces.push_back(std::move(surface));
    }

    std::optional<float> CollisionWorld::ground(Vector3 feet, float above, float below) const
    {
        const Ray ray{{feet.x, feet.y + above, feet.z}, {0.0f, -1.0f, 0.0f}};
        float nearest = above + below;
        std::optional<float> result;
        for (const auto &surface : m_surfaces)
        {
            const auto &box = surface.bounds;
            if (feet.x < box.min.x || feet.x > box.max.x || feet.z < box.min.z || feet.z > box.max.z ||
                box.min.y > ray.position.y || box.max.y < ray.position.y - nearest)
                continue;
            for (const auto &tri : surface.triangles)
            {
                if (std::abs(tri.normal.y) < 0.62f)
                    continue;
                const auto hit = GetRayCollisionTriangle(ray, tri.a, tri.b, tri.c);
                if (hit.hit && hit.distance <= nearest)
                {
                    nearest = hit.distance;
                    result = hit.point.y;
                }
            }
        }
        return result;
    }

    Vector3 CollisionWorld::slide(Vector3 before, Vector3 feet) const
    {
        // Two sphere contacts cover the body above the step height. Resolve
        // horizontally to slide along walls without climbing into the camera.
        constexpr std::array heights{0.57f, 1.29f};
        for (int pass = 0; pass < 2; ++pass)
        {
            for (const float height : heights)
            {
                Vector3 center{feet.x, feet.y + height, feet.z};
                for (const auto &surface : m_surfaces)
                {
                    if (!CheckCollisionBoxSphere(surface.bounds, center, kBodyRadius))
                        continue;
                    for (const auto &tri : surface.triangles)
                    {
                        if (std::abs(tri.normal.y) >= 0.62f)
                            continue;
                        const auto closest = nearestOnTriangle(center, tri.a, tri.b, tri.c);
                        auto delta = Vector3Subtract(center, closest);
                        const float reachSq = kBodyRadius * kBodyRadius - delta.y * delta.y;
                        const float horizontalSq = delta.x * delta.x + delta.z * delta.z;
                        if (reachSq <= horizontalSq)
                            continue;
                        delta.y = 0;
                        Vector3 direction;
                        if (horizontalSq > 1e-8f)
                            direction = Vector3Scale(delta, 1.0f / std::sqrt(horizontalSq));
                        else
                        {
                            direction = Vector3Normalize({tri.normal.x, 0.0f, tri.normal.z});
                            if (Vector3DotProduct(direction, Vector3Subtract(before, closest)) < 0.0f)
                                direction = Vector3Negate(direction);
                        }
                        const float push = std::sqrt(reachSq) - std::sqrt(horizontalSq) + 0.001f;
                        center = Vector3Add(center, Vector3Scale(direction, push));
                        feet.x = center.x;
                        feet.z = center.z;
                    }
                }
            }
        }
        return feet;
    }

    float CollisionWorld::trace(Vector3 from, Vector3 unitDirection, float maxDistance) const
    {
        const Ray ray{from, unitDirection};
        float nearest = maxDistance;
        for (const auto &surface : m_surfaces)
        {
            if (!rayIntersectsBox(ray, surface.bounds, nearest))
                continue;
            for (const auto &tri : surface.triangles)
            {
                const auto hit = GetRayCollisionTriangle(ray, tri.a, tri.b, tri.c);
                if (hit.hit && hit.distance < nearest)
                    nearest = hit.distance;
            }
        }
        return nearest;
    }

    void CharacterController::update(const CollisionWorld &world, Vector3 direction, float speed, float seconds)
    {
        m_remainder += std::clamp(seconds, 0.0f, 0.067f);
        const auto desired = Vector3Scale(direction, speed);
        while (m_remainder >= kTimeStep)
        {
            step(world, desired, kTimeStep);
            m_remainder -= kTimeStep;
        }
    }

    void CharacterController::jump()
    {
        if (!m_grounded)
            return;
        m_velocity.y = 5.3f;
        m_grounded = false;
    }

    void CharacterController::step(const CollisionWorld &world, Vector3 desired, float dt)
    {
        auto change = Vector3Subtract(desired, {m_velocity.x, 0, m_velocity.z});
        const float limit = (m_grounded ? 22.0f : 7.0f) * dt;
        if (Vector3LengthSqr(change) > limit * limit)
            change = Vector3Scale(Vector3Normalize(change), limit);
        m_velocity.x += change.x;
        m_velocity.z += change.z;
        auto next = world.slide(position, {position.x + m_velocity.x * dt, position.y, position.z + m_velocity.z * dt});
        m_velocity.y = std::max(m_velocity.y - 18.0f * dt, -25.0f);
        const float rise = m_velocity.y * dt;
        std::optional<float> ground;
        if (rise > 0.0f)
        {
            const Vector3 up{0, 1, 0};
            const Vector3 head = Vector3Add(next, {0, 1.85f, 0});
            const float traceLength = rise + 0.06f;
            const float headroom = world.trace(head, up, traceLength);
            next.y += headroom < traceLength ? std::max(0.0f, headroom - 0.06f) : rise;
            if (headroom < traceLength)
                m_velocity.y = 0.0f;
        }
        else
        {
            ground = world.ground(next, m_grounded ? kStepHeight : 0.02f, -rise + (m_grounded ? 0.18f : 0.002f));
            next.y = ground.value_or(next.y + rise);
        }
        if (ground)
            m_velocity.y = 0.0f;
        m_grounded = ground.has_value();
        m_velocity.x = (next.x - position.x) / dt;
        m_velocity.z = (next.z - position.z) / dt;
        m_planarSpeed = std::min(std::hypot(m_velocity.x, m_velocity.z), 7.0f);
        position = next;
        if (position.y < -12.0f)
        {
            position = m_spawn;
            m_velocity = {};
            m_planarSpeed = 0.0f;
        }
    }
}
