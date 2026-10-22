#pragma once

#include <cstdint>
#include <span>

namespace GameEngine
{
class UIManager;

/// A CSS transform function, shared by native styles and the generated script API.
/// Axis-specific CSS functions parse to the two-axis form, with the other axis at its identity.
// @ge-script
enum class TransformFunction : std::uint8_t
{
    Translate,
    Scale,
    Rotate,
    Skew,
    Matrix
};
} // namespace GameEngine

namespace GameEngine::UI::Script
{

/// A length in pixels or percent of the element's border box (100 means 100%).
// @ge-script
struct UiLength
{
    float Value = 0.0f;
    bool IsPercent = false;
};

/// One transform in CSS list order. Translate uses A/B as x/y lengths; Scale uses A/B;
/// Rotate uses A in degrees; Skew uses A/B in degrees; Matrix uses A through F.
/// XIsPercent/YIsPercent apply only to Translate. All numeric fields must be finite.
// @ge-script
struct UiTransformOperation
{
    TransformFunction Function = TransformFunction::Translate;
    float A = 0.0f;
    float B = 0.0f;
    float C = 0.0f;
    float D = 0.0f;
    float E = 0.0f;
    float F = 0.0f;
    bool XIsPercent = false;
    bool YIsPercent = false;
};

/// Replace the live element's transform override, copying the operations before returning.
/// An empty span overrides authored transforms with none. Returns false for an unreachable
/// element or invalid operation, preserving the previous override. Main thread only.
// @ge-script @ge-context
bool SetTransform(UIManager& ui, std::uint64_t instanceId, std::span<const UiTransformOperation> operations);

/// Remove the transform override so authored styles apply again. Main thread only.
/// Returns false if the element is no longer reachable from this UIManager's root.
// @ge-script @ge-context
bool ClearTransform(UIManager& ui, std::uint64_t instanceId);

/// Override the pivot in border-box pixels or percent. Main thread only.
/// Returns false for an unreachable element or non-finite length, preserving the old origin.
// @ge-script @ge-context
bool SetTransformOrigin(UIManager& ui, std::uint64_t instanceId, UiLength x, UiLength y);

/// Remove the origin override so authored styles apply again. Main thread only.
/// Returns false if the element is no longer reachable from this UIManager's root.
// @ge-script @ge-context
bool ClearTransformOrigin(UIManager& ui, std::uint64_t instanceId);

} // namespace GameEngine::UI::Script
