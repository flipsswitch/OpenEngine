#include "PhysicsECS/PhysicsScriptApi.h"

#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/PhysicsEntityEventCallbacks.h"
#include "PhysicsECS/PhysicsWorldService.h"

#include "ECS/World.h"
#include "Physics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace GameEngine::PhysicsECS::Script
{
namespace
{

// The live body of the entity's PhysicsBody and the world it lives in, or null for either when there
// is none (no body, a body the physics systems have not created yet, or no world outside play).
struct BodyRef
{
    Physics::PhysicsWorld* World = nullptr;
    Physics::BodyHandle Body{};
};

BodyRef FindBody(const ECS::World& world, ECS::EntityHandle entity)
{
    Physics::PhysicsWorld* physics = PhysicsWorldService::TryGet();
    const Components::PhysicsBody* body = physics != nullptr ? world.GetComponent<Components::PhysicsBody>(entity) : nullptr;
    if (body == nullptr || !body->initialized || !physics->IsBodyValid(body->body))
        return {};
    return BodyRef{physics, body->body};
}

ContactEvent ToScript(const EntityContactEvent& e)
{
    ContactEvent result;
    result.self = e.self;
    result.other = e.other;
    result.point = e.point;
    result.normal = e.normal;
    return result;
}

TriggerEvent ToScript(const EntityTriggerEvent& e)
{
    TriggerEvent result;
    result.self = e.self;
    result.other = e.other;
    result.selfIsTrigger = e.selfIsTrigger;
    return result;
}

// The native subscription's callbacks for a script callback: each event converted to the script
// payload, and the callback's Ended when the subscription ends.
template <class Native, class Payload>
GameEngine::Function<void(const Native&)> Relay(ScriptCallback<Payload> callback)
{
    return [callback](const Native& e)
    {
        const Payload payload = ToScript(e);
        callback.Invoke(&payload, callback.User);
    };
}

template <class Payload>
PhysicsEntityEventCallbacks::EndedCallback EndedRelay(ScriptCallback<Payload> callback)
{
    if (callback.Ended == nullptr)
        return {};
    return [callback]
    { callback.Ended(callback.User); };
}

} // namespace

bool RayCast(Physics::PhysicsWorld& physics, Mathematics::Vector3 origin, Mathematics::Vector3 direction, RayHit& hit,
             float maxDistance, std::uint32_t layerMask, bool ignoreTriggers)
{
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (!(length > 0.0f) || !(maxDistance > 0.0f))
        return false;

    Physics::RayCastQuery query;
    query.ray.origin = origin;
    query.ray.direction = direction / length;
    query.maxDistance = maxDistance;
    query.filter.layerMask = layerMask;
    query.filter.ignoreSensors = ignoreTriggers;
    Physics::RayCastResult result;
    if (!physics.RayCast(query, result))
        return false;

    // PhysicsECS stores the body's entity id as its user data.
    hit.entity = ECS::EntityHandle(static_cast<std::uint32_t>(result.userData));
    hit.point = result.hitPoint;
    hit.normal = result.hitNormal;
    hit.distance = result.distance;
    return true;
}

std::int32_t RayCastAll(Physics::PhysicsWorld& physics, Mathematics::Vector3 origin, Mathematics::Vector3 direction,
                        std::span<RayHit> results, float maxDistance, std::uint32_t layerMask, bool ignoreTriggers)
{
    const float lengthSquared = direction.LengthSquared();
    if (results.empty() || !(maxDistance > 0.0f) || !std::isfinite(maxDistance) ||
        !(lengthSquared > 0.0f) || !std::isfinite(lengthSquared) ||
        !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z))
        return 0;

    Physics::RayCastQuery query;
    query.ray.origin = origin;
    query.ray.direction = direction;
    query.maxDistance = maxDistance;
    query.filter.layerMask = layerMask;
    query.filter.ignoreSensors = ignoreTriggers;
    const auto capacity = static_cast<std::uint32_t>(
        std::min<std::size_t>(results.size(), std::numeric_limits<std::int32_t>::max()));
    std::vector<Physics::RayCastResult> hits;
    physics.RayCastAll(query, hits, capacity);
    for (std::size_t i = 0; i < hits.size(); ++i)
    {
        results[i].entity = ECS::EntityHandle(static_cast<std::uint32_t>(hits[i].userData));
        results[i].point = hits[i].hitPoint;
        results[i].normal = hits[i].hitNormal;
        results[i].distance = hits[i].distance;
    }
    return static_cast<std::int32_t>(hits.size());
}

std::int32_t BoxOverlap(Physics::PhysicsWorld& physics, Mathematics::Vector3 center, Mathematics::Vector3 halfExtents,
                        Mathematics::Quaternion rotation, std::span<ECS::EntityHandle> results,
                        std::uint32_t layerMask, bool ignoreTriggers)
{
    if (results.empty())
        return 0;

    Physics::BoxOverlapQuery query;
    query.center = center;
    query.halfExtents = halfExtents;
    query.rotation = rotation;
    query.filter.layerMask = layerMask;
    query.filter.ignoreSensors = ignoreTriggers;
    const auto capacity = static_cast<std::uint32_t>(
        std::min<std::size_t>(results.size(), std::numeric_limits<std::int32_t>::max()));
    std::vector<Physics::OverlapResult> hits;
    physics.BoxOverlap(query, hits, capacity);
    for (std::size_t i = 0; i < hits.size(); ++i)
        results[i] = ECS::EntityHandle(static_cast<std::uint32_t>(hits[i].userData));
    return static_cast<std::int32_t>(hits.size());
}

Mathematics::Vector3 GetLinearVelocity(ECS::World& world, ECS::EntityHandle entity)
{
    const BodyRef body = FindBody(world, entity);
    return body.World != nullptr ? body.World->GetLinearVelocity(body.Body) : Mathematics::Vector3(0.0f, 0.0f, 0.0f);
}

void SetLinearVelocity(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 velocity)
{
    if (const BodyRef body = FindBody(world, entity); body.World != nullptr)
        body.World->SetLinearVelocity(body.Body, velocity);
}

Mathematics::Vector3 GetAngularVelocity(ECS::World& world, ECS::EntityHandle entity)
{
    const BodyRef body = FindBody(world, entity);
    return body.World != nullptr ? body.World->GetAngularVelocity(body.Body) : Mathematics::Vector3(0.0f, 0.0f, 0.0f);
}

void SetAngularVelocity(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 velocity)
{
    if (const BodyRef body = FindBody(world, entity); body.World != nullptr)
        body.World->SetAngularVelocity(body.Body, velocity);
}

void AddImpulse(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 impulse)
{
    if (const BodyRef body = FindBody(world, entity); body.World != nullptr)
        body.World->AddImpulse(body.Body, impulse);
}

void AddImpulseAtPosition(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 impulse,
                          Mathematics::Vector3 position)
{
    if (const BodyRef body = FindBody(world, entity); body.World != nullptr)
        body.World->AddImpulseAtPosition(body.Body, impulse, position);
}

Mathematics::Vector3 GetDesiredVelocity(ECS::World& world, ECS::EntityHandle entity)
{
    const Components::CharacterController* controller = world.GetComponent<Components::CharacterController>(entity);
    if (controller == nullptr)
        return Mathematics::Vector3(0.0f, 0.0f, 0.0f);
    return Mathematics::Vector3(controller->desiredVelocityX, controller->desiredVelocityY, controller->desiredVelocityZ);
}

void SetDesiredVelocity(ECS::World& world, ECS::EntityHandle entity, Mathematics::Vector3 velocity)
{
    Components::CharacterController* controller = world.GetComponentForWrite<Components::CharacterController>(entity);
    if (controller == nullptr)
        return;
    controller->desiredVelocityX = velocity.x;
    controller->desiredVelocityY = velocity.y;
    controller->desiredVelocityZ = velocity.z;
}

bool IsGrounded(ECS::World& world, ECS::EntityHandle entity)
{
    const Components::CharacterController* controller = world.GetComponent<Components::CharacterController>(entity);
    return controller != nullptr && controller->grounded;
}

SubscriptionId SubscribeContactBegin(ECS::EntityHandle scope, ScriptCallback<ContactEvent> callback)
{
    return PhysicsEntityEventCallbacks::SubscribeContactBegin(scope, Relay<EntityContactEvent>(callback), EndedRelay(callback));
}

void UnsubscribeContactBegin(SubscriptionId subscription)
{
    PhysicsEntityEventCallbacks::Unsubscribe(subscription);
}

SubscriptionId SubscribeContactEnd(ECS::EntityHandle scope, ScriptCallback<ContactEvent> callback)
{
    return PhysicsEntityEventCallbacks::SubscribeContactEnd(scope, Relay<EntityContactEvent>(callback), EndedRelay(callback));
}

void UnsubscribeContactEnd(SubscriptionId subscription)
{
    PhysicsEntityEventCallbacks::Unsubscribe(subscription);
}

SubscriptionId SubscribeTriggerEnter(ECS::EntityHandle scope, ScriptCallback<TriggerEvent> callback)
{
    return PhysicsEntityEventCallbacks::SubscribeTriggerEnter(scope, Relay<EntityTriggerEvent>(callback), EndedRelay(callback));
}

void UnsubscribeTriggerEnter(SubscriptionId subscription)
{
    PhysicsEntityEventCallbacks::Unsubscribe(subscription);
}

SubscriptionId SubscribeTriggerExit(ECS::EntityHandle scope, ScriptCallback<TriggerEvent> callback)
{
    return PhysicsEntityEventCallbacks::SubscribeTriggerExit(scope, Relay<EntityTriggerEvent>(callback), EndedRelay(callback));
}

void UnsubscribeTriggerExit(SubscriptionId subscription)
{
    PhysicsEntityEventCallbacks::Unsubscribe(subscription);
}

} // namespace GameEngine::PhysicsECS::Script
