#include "Scene/PrefabPlacement.h"

#include "Components/Hierarchy.h"
#include "Components/PrefabInstance.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Types/FlatSet.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <optional>
#include <string_view>

namespace GameEngine::Scene
{

static ECS::EntityHandle FindMember(const PlacedPrefab& placed, std::string_view address)
{
    for (const auto& [memberAddress, h] : placed.members)
    {
        if (memberAddress == address)
            return h;
    }
    return {};
}

// Records the hashed identity `tag` of `path`. False, with both paths and the fix, when another path
// already has it.
static bool ClaimHashedTag(PrefabPlacement& placement, const std::string& tag, const std::string& path,
                           const std::filesystem::path& file, int line, const LoadFailure& failure)
{
    if (tag == path)
        return true;
    const std::string* claimed = placement.pathByHashedTag.Find(tag);
    if (!claimed)
    {
        placement.pathByHashedTag.InsertOrAssign(tag, path);
        return true;
    }
    if (*claimed == path)
        return true;
    return failure.Fail(file, line,
                        "Two entities get the scene identity \"" + tag + "\": \"" + *claimed + "\" and \"" + path +
                            "\". Shorten an id in one of the chains.");
}

static bool PlacePrefab(PrefabPlacement& placement, ParsedPrefab& prefab, const std::string& instancePath,
                        PlacedPrefab& out, const LoadFailure& failure);

// The prefab the [prefab] section `section` of `ownerFile` places, read and checked once per load.
// Null, with the failure recorded, when the file cannot be read, does not parse, or is not a prefab.
static ParsedPrefab* ReadPrefab(PrefabPlacement& placement, const SceneLoadContext& ownerCtx,
                                const std::filesystem::path& ownerFile, const SceneDoc& ownerDoc,
                                const SceneDoc::PrefabInstanceSection& section, const LoadFailure& failure)
{
    const SceneDoc::Resource& resource = ownerDoc.resources.at(section.sourceResourceId);
    const std::filesystem::path path = ResolveResourcePath(ownerCtx, ownerFile, resource).lexically_normal();
    const std::string key = path.generic_string();
    if (auto* found = placement.parsedByPath.Find(key))
        return found->get();

    std::string text;
    if (path.empty() || !ReadTextFile(path, text))
    {
        ReportUnreadableResource("Prefab", resource, path, placement.options.projectRoot, ownerFile,
                                 section.headerLine, failure.message, failure.file, failure.line);
        return nullptr;
    }
    auto parsed = std::make_unique<ParsedPrefab>();
    parsed->path = path;
    int errorLine = 0;
    std::string error;
    if (!ParseDocument(path, text, parsed->doc, &error, &errorLine) ||
        !ValidateDocument(parsed->doc, &error, &errorLine))
    {
        failure.Fail(path, errorLine, std::move(error));
        return nullptr;
    }
    if (!parsed->doc.isPrefab)
    {
        failure.Fail(ownerFile, section.headerLine,
                     "The prefab instance \"" + section.id + "\" on line " + std::to_string(section.headerLine) +
                         " uses resource \"" + section.sourceResourceId + "\", which is a scene, not a prefab. Point "
                         "the resource at a .prefab file, or place the scene with [subscene].");
        return nullptr;
    }
    parsed->ctx = MakeDocumentLoadContext(placement.world, parsed->path, parsed->doc, placement.options);
    return placement.parsedByPath.InsertOrAssign(key, std::move(parsed)).get();
}

// Places the prefab the section names at `instancePath` and marks its root with the source. The
// section's own override lines are the caller's to apply, against the caller's entity ids.
bool PlaceInstanceSection(PrefabPlacement& placement, const SceneLoadContext& ownerCtx,
                          const std::filesystem::path& ownerFile, const SceneDoc& ownerDoc,
                          const SceneDoc::PrefabInstanceSection& section, const std::string& instancePath,
                          PlacedPrefab& out, const LoadFailure& failure)
{
    ParsedPrefab* prefab = ReadPrefab(placement, ownerCtx, ownerFile, ownerDoc, section, failure);
    if (!prefab)
        return false;

    if (std::find(placement.chain.begin(), placement.chain.end(), prefab->path) != placement.chain.end())
    {
        std::string loop;
        for (const auto& file : placement.chain)
            loop += file.filename().string() + " places ";
        loop += prefab->path.filename().string();
        return failure.Fail(ownerFile, section.headerLine,
                            "Prefab cycle: " + loop + ". Remove the prefab instance on line " +
                                std::to_string(section.headerLine) + ", which closes the loop.");
    }

    placement.chain.push_back(prefab->path);
    const bool placed = PlacePrefab(placement, *prefab, instancePath, out, failure);
    placement.chain.pop_back();
    if (!placed)
        return false;

    Components::PrefabInstance marker{};
    AssetReference source;
    if (TryResolveResourceIdToAssetReference(ownerCtx, section.sourceResourceId, source, nullptr))
        marker.Source = source.guid;
    placement.world.AddComponentImmediate(out.root, marker);
    return true;
}

// The load-report row for an override line whose address names no entity of the prefab: an
// orphaned override. The load goes on; the line is not kept by a save.
static void RecordOrphanedOverride(SceneLoadDegradation* degradation, const std::string& instancePath,
                                   std::string_view address, std::string component, std::string_view line,
                                   const std::filesystem::path& ownerFile, int lineNo, ECS::EntityHandle root)
{
    if (!degradation)
        return;
    degradation->skips.push_back(SceneLoadSkip{
        JoinPath(instancePath, address), std::move(component), {},
        "'" + std::string(line) + "' names \"" + std::string(address) +
            "\", which the prefab does not have. Delete the line, or put the entity back in the prefab.",
        ownerFile, lineNo, /*preserved=*/false, root});
}

// True when the section changes a field of `component` at its address.
static bool HasFieldLinesFor(const SceneDoc::PrefabInstanceSection& section,
                             const SceneDoc::PrefabInstanceSection::AddressedComponent& component)
{
    for (const auto& fields : section.changedFields)
    {
        if (fields.address != component.address)
            continue;
        for (const auto& changed : fields.components)
        {
            if (changed.name == component.name)
                return true;
        }
    }
    return false;
}

// True when `memberAddress` is `address` or the address of an entity under it.
static bool IsAtOrUnder(std::string_view memberAddress, std::string_view address)
{
    return memberAddress.starts_with(address) &&
           (memberAddress.size() == address.size() || memberAddress[address.size()] == '.');
}

// Destroys the member of `placed` at `address` and every member under it, and moves them from
// `placed` to `removed`.
static void RemoveMemberAndItsChildren(ECS::World& world, PlacedPrefab& placed, std::string_view address,
                                       std::vector<std::pair<std::string, ECS::EntityHandle>>& removed)
{
    const auto first =
        std::stable_partition(placed.members.begin(), placed.members.end(),
                              [address](const auto& member) { return !IsAtOrUnder(member.first, address); });
    for (auto it = first; it != placed.members.end(); ++it)
    {
        world.DestroyEntityImmediate(it->second);
        removed.push_back(*it);
    }
    placed.members.erase(first, placed.members.end());
}

// Applies one [prefab] section's override lines to the instance `placed`, whose root is at
// `instancePath`: removed entities, removed components, added components, then changed fields. A
// line whose address names no entity, a removal of a component no build registers and a removal of a
// component the entity does not have are load-report rows, never a failure. The entities a "-entity"
// line removes go to `removed`.
static bool ApplyInstanceOverrides(ECS::World& world, const SceneLoadContext& ctx,
                                   const std::filesystem::path& ownerFile,
                                   const SceneDoc::PrefabInstanceSection& section, const std::string& instancePath,
                                   PlacedPrefab& placed,
                                   std::vector<std::pair<std::string, ECS::EntityHandle>>& removed,
                                   SceneLoadDegradation* degradation, const LoadFailure& failure)
{
    const std::size_t firstRow = degradation ? degradation->skips.size() : 0;

    for (const auto& removedEntity : section.removedEntities)
    {
        if (!FindMember(placed, removedEntity.address).IsValid())
        {
            RecordOrphanedOverride(degradation, instancePath, removedEntity.address, {},
                                   "-entity " + removedEntity.address, ownerFile, removedEntity.line, placed.root);
            continue;
        }
        RemoveMemberAndItsChildren(world, placed, removedEntity.address, removed);
    }

    for (const auto& component : section.removedComponents)
    {
        const ECS::EntityHandle h = FindMember(placed, component.address);
        const std::string text = "-" + Addressed(component.address, component.authoredName);
        if (!h.IsValid())
        {
            RecordOrphanedOverride(degradation, instancePath, component.address, component.authoredName, text,
                                   ownerFile, component.line, placed.root);
            continue;
        }
        const auto* schema = SceneSchemaRegistry::Find(component.name);
        if (!schema)
        {
            if (degradation)
                degradation->skips.push_back(SceneLoadSkip{
                    JoinPath(instancePath, component.address), component.authoredName, {},
                    "'" + text + "' removes a component no loaded module registers. Delete the line, or load the "
                                 "module that registers " + component.authoredName + ".",
                    ownerFile, component.line, /*preserved=*/false, h});
            continue;
        }
        if (!schema->IsPresent(world, h))
        {
            if (degradation)
                degradation->skips.push_back(SceneLoadSkip{
                    JoinPath(instancePath, component.address), component.authoredName, {},
                    "'" + text + "' removes a component the entity does not have. Delete the line.", ownerFile,
                    component.line, /*preserved=*/false, h});
            continue;
        }
        if (!schema->Remove(world, h))
            return failure.Fail(ownerFile, component.line,
                                "Component '" + component.authoredName + "' does not support removal");
    }

    for (const auto& component : section.addedComponents)
    {
        const ECS::EntityHandle h = FindMember(placed, component.address);
        if (!h.IsValid())
        {
            RecordOrphanedOverride(degradation, instancePath, component.address, component.authoredName,
                                   "+" + Addressed(component.address, component.authoredName),
                                   ownerFile, component.line, placed.root);
            continue;
        }
        // The component's field lines add it with their values below.
        if (HasFieldLinesFor(section, component))
            continue;
        const SceneDoc::ComponentProps block{component.name, component.authoredName, {}};
        const std::string rowId = JoinPath(instancePath, component.address);
        if (const auto* schema = SceneSchemaRegistry::Find(component.name))
            ApplyComponentBlock(world, ctx, *schema, h, block, rowId, ownerFile, component.line, degradation);
        else
            PreserveOrDropUnhandledComponent(world, h, block, rowId, ownerFile);
    }

    for (const auto& fields : section.changedFields)
    {
        const ECS::EntityHandle h = FindMember(placed, fields.address);
        const std::string rowId = JoinPath(instancePath, fields.address);
        for (const auto& comp : fields.components)
        {
            const int line = comp.props.empty() ? section.headerLine : comp.props.front().second.line;
            if (!h.IsValid())
            {
                RecordOrphanedOverride(degradation, instancePath, fields.address, comp.originalName,
                                       Addressed(fields.address, comp.originalName + "." +
                                                                     (comp.props.empty() ? std::string()
                                                                                         : comp.props.front().first)),
                                       ownerFile, line, placed.root);
                continue;
            }
            if (const auto* schema = SceneSchemaRegistry::Find(comp.name))
                ApplyComponentBlock(world, ctx, *schema, h, comp, rowId, ownerFile, line, degradation);
            else
                PreserveOrDropUnhandledComponent(world, h, comp, rowId, ownerFile);
        }
    }

    RecordUnreadLines(section.unreadLines, instancePath, ownerFile, placed.root, firstRow, degradation);
    return true;
}

// The address under a prefab's root of the entity `identity` names in the prefab's file: an entity
// id, an instance section's id, or an instance id, '.', and an address inside that instance. False
// when the name resolves to nothing in the file, or its parent chain loops.
static bool AddressOfIdentity(const SceneDoc& doc, std::string_view identity,
                              FlatMap<std::string, std::string>& addressByIdentity, FlatSet<std::string>& resolving,
                              std::string& outAddress)
{
    const std::string key(identity);
    if (const std::string* known = addressByIdentity.Find(key))
    {
        outAddress = *known;
        return true;
    }
    if (!resolving.Insert(key))
        return false;

    const std::optional<std::string>* parent = nullptr;
    std::string_view ownId;
    std::string_view rest;
    if (auto e = std::find_if(doc.entities.begin(), doc.entities.end(),
                              [identity](const auto& entity) { return entity.id == identity; });
        e != doc.entities.end())
    {
        parent = &e->parent;
        ownId = e->id;
    }
    else
    {
        const std::size_t dot = identity.find('.');
        const std::string_view sectionId = identity.substr(0, dot);
        auto b = std::find_if(doc.prefabInstances.begin(), doc.prefabInstances.end(),
                              [sectionId](const auto& instance) { return instance.id == sectionId; });
        if (b == doc.prefabInstances.end())
            return false;
        parent = &b->parent;
        ownId = b->id;
        if (dot != std::string_view::npos)
            rest = identity.substr(dot + 1);
    }

    std::string address;
    if (parent->has_value())
    {
        std::string parentAddress;
        if (!AddressOfIdentity(doc, **parent, addressByIdentity, resolving, parentAddress))
            return false;
        address = JoinPath(parentAddress, ownId);
    }
    outAddress = JoinPath(address, rest);
    addressByIdentity.InsertOrAssign(key, outAddress);
    resolving.Erase(key);
    return true;
}

// Places `prefab` at `instancePath` into the placement's world, in layers, a later one winning per
// field: a variant's base; the prefab's own entities; its nested instances with their override
// lines; the variant's override lines. Every entity gets its scene identity from its path.
static bool PlacePrefab(PrefabPlacement& placement, ParsedPrefab& prefab, const std::string& instancePath,
                        PlacedPrefab& out, const LoadFailure& failure)
{
    ECS::World& world = placement.world;
    const SceneDoc& doc = prefab.doc;

    // The file's own names for its entities: entity ids, instance ids, and "instance.address".
    std::unordered_map<std::string, ECS::EntityHandle> entityByIdentity;
    FlatMap<std::string, std::string> addressByIdentity;
    FlatSet<std::string> resolving;

    struct NestedInstance
    {
        const SceneDoc::PrefabInstanceSection* section = nullptr;
        std::string path;
        PlacedPrefab placed;
    };
    std::vector<NestedInstance> nested;
    nested.reserve(doc.prefabInstances.size());
    const SceneDoc::PrefabInstanceSection* variantRoot = nullptr;

    for (const auto& section : doc.prefabInstances)
    {
        std::string address;
        if (!AddressOfIdentity(doc, section.id, addressByIdentity, resolving, address))
            return failure.Fail(prefab.path, section.headerLine,
                                "Missing parent '" + section.parent.value_or("") + "' for prefab instance '" +
                                    section.id + "'");
        NestedInstance instance{&section, JoinPath(instancePath, address), {}};
        if (!PlaceInstanceSection(placement, prefab.ctx, prefab.path, doc, section, instance.path, instance.placed,
                                  failure))
            return false;
        if (!section.parent)
        {
            variantRoot = &section;
            out.root = instance.placed.root;
        }
        for (const auto& [memberAddress, h] : instance.placed.members)
        {
            out.members.emplace_back(JoinPath(address, memberAddress), h);
            entityByIdentity[JoinPath(section.id, memberAddress)] = h;
        }
        nested.push_back(std::move(instance));
    }

    std::vector<ECS::EntityHandle> created;
    created.reserve(doc.entities.size());
    std::vector<std::string> paths;
    paths.reserve(doc.entities.size());
    for (const auto& e : doc.entities)
    {
        std::string address;
        if (!AddressOfIdentity(doc, e.id, addressByIdentity, resolving, address))
            return failure.Fail(prefab.path, e.headerLine,
                                "Missing parent '" + e.parent.value_or("") + "' for entity '" + e.id + "'");
        const ECS::EntityHandle h = world.CreateEntity();
        if (!h.IsValid())
            return failure.Fail(prefab.path, e.headerLine, "Failed to create entity");
        std::string path = JoinPath(instancePath, address);
        const std::string tag = SceneTagForPath(path);
        if (!ClaimHashedTag(placement, tag, path, prefab.path, e.headerLine, failure))
            return false;
        world.AddComponentImmediate(h, MakeTag(tag));
        if (address.empty())
            out.root = h;
        out.members.emplace_back(std::move(address), h);
        entityByIdentity[e.id] = h;
        created.push_back(h);
        paths.push_back(std::move(path));
    }
    std::stable_partition(out.members.begin(), out.members.end(),
                          [](const auto& member) { return member.first.empty(); });

    // Parent links, now that every entity of the file and of its nested instances exists.
    for (std::size_t entityIndex = 0; entityIndex < doc.entities.size(); ++entityIndex)
    {
        const auto& parent = doc.entities[entityIndex].parent;
        if (!parent)
            continue;
        auto it = entityByIdentity.find(*parent);
        if (it == entityByIdentity.end())
            return failure.Fail(prefab.path, doc.entities[entityIndex].headerLine,
                                "Missing parent '" + *parent + "' for entity '" + doc.entities[entityIndex].id + "'");
        world.AddComponentImmediate(created[entityIndex], Components::Parent{it->second});
    }
    for (const auto& instance : nested)
    {
        if (!instance.section->parent)
            continue;
        auto it = entityByIdentity.find(*instance.section->parent);
        if (it == entityByIdentity.end())
            return failure.Fail(prefab.path, instance.section->headerLine,
                                "Missing parent '" + *instance.section->parent + "' for prefab instance '" +
                                    instance.section->id + "'");
        world.AddComponentImmediate(instance.placed.root, Components::Parent{it->second});
    }

    // The file's lines resolve entity references against the file's own names.
    prefab.ctx.EntityIdMap = &entityByIdentity;
    prefab.ctx.EntityIdPrefix.clear();
    for (std::size_t entityIndex = 0; entityIndex < doc.entities.size(); ++entityIndex)
    {
        const SceneDoc::Entity& e = doc.entities[entityIndex];
        // A member's fallback name is its id; the root's is the instance's own id.
        const std::string_view fallbackName = e.parent ? std::string_view(e.id) : LastIdOf(instancePath);
        ApplyEntityComponents(world, prefab.ctx, prefab.path, created[entityIndex], e, paths[entityIndex], fallbackName,
                              placement.degradation);
    }

    std::vector<std::pair<std::string, ECS::EntityHandle>> removed;
    bool applied = true;
    for (auto& instance : nested)
    {
        if (instance.section == variantRoot)
            continue;
        applied = ApplyInstanceOverrides(world, prefab.ctx, prefab.path, *instance.section, instance.path,
                                         instance.placed, removed, placement.degradation, failure);
        if (!applied)
            break;
    }
    if (applied && variantRoot)
        applied = ApplyInstanceOverrides(world, prefab.ctx, prefab.path, *variantRoot, instancePath, out, removed,
                                         placement.degradation, failure);
    prefab.ctx.EntityIdMap = nullptr;
    if (!applied)
        return false;

    std::erase_if(out.members, [&world](const auto& member) { return !world.IsValid(member.second); });
    return true;
}

// Places every [prefab] section of a scene (or of a prefab file loaded on its own) under the ids
// the scene's own lines use, applies each section's override lines, and links each instance root
// and each entity that waits on a member to its parent.
bool PlaceSceneInstances(PrefabPlacement& placement, SceneLoadContext& sceneCtx,
                         const std::filesystem::path& sceneFilePath, const SceneDoc& doc,
                         const std::string& idPrefix, Instantiated& out, const LoadFailure& failure)
{
    ECS::World& world = placement.world;
    sceneCtx.EntityIdMap = &out.idToEntity;
    sceneCtx.EntityIdPrefix = idPrefix;

    std::vector<std::pair<ECS::EntityHandle, const SceneDoc::PrefabInstanceSection*>> roots;
    roots.reserve(doc.prefabInstances.size());
    for (const auto& section : doc.prefabInstances)
    {
        const std::string instancePath = idPrefix + section.id;
        PlacedPrefab placed;
        if (!PlaceInstanceSection(placement, sceneCtx, sceneFilePath, doc, section, instancePath, placed, failure))
            return false;
        for (const auto& [address, h] : placed.members)
        {
            const std::string path = JoinPath(instancePath, address);
            const std::string tag = SceneTagForPath(path);
            if (tag != path)
                out.idToEntity[tag] = h;
            out.idToEntity[path] = h;
        }

        std::vector<std::pair<std::string, ECS::EntityHandle>> removed;
        if (!ApplyInstanceOverrides(world, sceneCtx, sceneFilePath, section, instancePath, placed, removed,
                                    placement.degradation, failure))
            return false;
        for (const auto& [address, h] : removed)
        {
            const std::string path = JoinPath(instancePath, address);
            out.idToEntity.erase(SceneTagForPath(path));
            out.idToEntity.erase(path);
        }
        for (const auto& member : placed.members)
            out.created.push_back(member.second);
        roots.emplace_back(placed.root, &section);
    }

    for (const auto& [root, section] : roots)
    {
        if (!section->parent)
            continue;
        out.pendingParents.push_back({root, idPrefix + *section->parent, sceneFilePath, section->headerLine});
    }
    for (const auto& pending : out.pendingParents)
    {
        auto it = out.idToEntity.find(pending.parentId);
        if (it == out.idToEntity.end())
            return failure.Fail(pending.file, pending.line, "Missing parent '" + pending.parentId + "'");
        world.AddComponentImmediate(pending.entity, Components::Parent{it->second});
    }
    out.pendingParents.clear();
    return true;
}

} // namespace GameEngine::Scene
