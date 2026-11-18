#pragma once

#include "raylib.h"

#include <optional>
#include <vector>

namespace godhand::pc
{
    class CollisionWorld
    {
    public:
        void add(const Mesh &mesh, Matrix transform);
        std::optional<float> ground(Vector3 feet, float above, float below) const;
        Vector3 slide(Vector3 before, Vector3 feet) const;
        float trace(Vector3 from, Vector3 unitDirection, float maxDistance) const;

    private:
        struct Triangle
        {
            Vector3 a, b, c, normal;
        };
        struct Surface
        {
            BoundingBox bounds;
            std::vector<Triangle> triangles;
        };
        std::vector<Surface> m_surfaces;
    };

    class CharacterController
    {
    public:
        explicit CharacterController(Vector3 spawn) : position(spawn), m_spawn(spawn) {}
        void update(const CollisionWorld &world, Vector3 direction, float speed, float seconds);
        void jump();
        bool grounded() const { return m_grounded; }
        float planarSpeed() const { return m_planarSpeed; }
        Vector3 position;

    private:
        void step(const CollisionWorld &world, Vector3 desired, float dt);
        Vector3 m_spawn;
        Vector3 m_velocity{};
        float m_planarSpeed = 0.0f;
        float m_remainder = 0.0f;
        bool m_grounded = false;
    };
}
