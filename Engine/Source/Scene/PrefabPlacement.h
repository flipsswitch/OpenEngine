#pragma once

// Places prefab instances into a world: each prefab file read once, its entities placed under the
// instance's path, the instance's override lines applied. The scene loader and the prefab override
// writer's templates use it. Private to the scene IO sources.

#include "Scene/SceneDoc.h"
#include "Types/FlatMap.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Scene
{

// A prefab file read once for one load or save, with the tables its lines resolve against.
struct ParsedPrefab
{
    std::filesystem::path path;
    SceneDoc doc;
    SceneLoadContext ctx;
};

// What one load (or one save's template placement) shares across the prefab instances it places.
struct PrefabPlacement
{
    ECS::World& world;
    const LoadOptions& options;
    SceneLoadDegradation* degradation = nullptr;

    // Each prefab file parsed once, by its absolute path. A scene places thousands of instances
    // of a few hundred prefabs.
    FlatMap<std::string, std::unique_ptr<ParsedPrefab>> parsedByPath;

    // The prefab files being placed, outermost first: a file already on the chain closes a cycle.
    std::vector<std::filesystem::path> chain;

    // Every hashed scene identity the load assigned, with its path; two paths with one identity
    // are refused. A path that fits the tag is its own identity and cannot collide.
    FlatMap<std::string, std::string> pathByHashedTag;
};

// The entities of one placed prefab instance with their addresses under its root, in template
// order. The root is at the empty address.
struct PlacedPrefab
{
    ECS::EntityHandle root{};
    std::vector<std::pair<std::string, ECS::EntityHandle>> members;
};

// Places the prefab `section` names at `instancePath`; the section's override lines are the caller's.
bool PlaceInstanceSection(PrefabPlacement& placement, const SceneLoadContext& ownerCtx,
                          const std::filesystem::path& ownerFile, const SceneDoc& ownerDoc,
                          const SceneDoc::PrefabInstanceSection& section, const std::string& instancePath,
                          PlacedPrefab& out, const LoadFailure& failure);

// Places every [prefab] section of `doc` and applies its override lines.
bool PlaceSceneInstances(PrefabPlacement& placement, SceneLoadContext& sceneCtx,
                         const std::filesystem::path& sceneFilePath, const SceneDoc& doc, const std::string& idPrefix,
                         Instantiated& out, const LoadFailure& failure);

} // namespace GameEngine::Scene
