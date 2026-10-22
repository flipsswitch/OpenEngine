#include "UI/UiTransformsScriptApi.h"

#include "UI/StyleOverrides.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"

#include <cmath>
#include <utility>

namespace GameEngine::UI::Script
{
namespace
{

StyleLength ToStyleLength(float value, bool isPercent)
{
    return isPercent ? StyleLength::Percent(value) : StyleLength::Px(value);
}

bool ConvertOperation(const UiTransformOperation& source, TransformOp& destination)
{
    if (!std::isfinite(source.A) || !std::isfinite(source.B) || !std::isfinite(source.C) ||
        !std::isfinite(source.D) || !std::isfinite(source.E) || !std::isfinite(source.F))
        return false;

    destination.Function = source.Function;
    switch (source.Function)
    {
    case TransformFunction::Translate:
        destination.TranslateX = ToStyleLength(source.A, source.XIsPercent);
        destination.TranslateY = ToStyleLength(source.B, source.YIsPercent);
        return true;
    case TransformFunction::Scale:
        destination = {source.Function, {}, {}, {source.A, source.B, 0, 0, 0, 0}};
        return true;
    case TransformFunction::Rotate:
        destination = {source.Function, {}, {}, {source.A, 0, 0, 0, 0, 0}};
        return true;
    case TransformFunction::Skew:
        destination = {source.Function, {}, {}, {source.A, source.B, 0, 0, 0, 0}};
        return true;
    case TransformFunction::Matrix:
        destination = {source.Function, {}, {}, {source.A, source.B, source.C, source.D, source.E, source.F}};
        return true;
    default:
        return false;
    }
}

} // namespace

bool SetTransform(UIManager& ui, std::uint64_t instanceId, std::span<const UiTransformOperation> operations)
{
    UIElement* element = ui.FindElementByInstanceId(instanceId);
    if (!element)
        return false;

    TransformOpList nativeOperations;
    nativeOperations.reserve(operations.size());
    for (const UiTransformOperation& operation : operations)
    {
        TransformOp nativeOperation;
        if (!ConvertOperation(operation, nativeOperation))
            return false;
        nativeOperations.push_back(nativeOperation);
    }
    element->Overrides().Set(Style::Transform, TransformValue::FromOps(std::move(nativeOperations)));
    return true;
}

bool ClearTransform(UIManager& ui, std::uint64_t instanceId)
{
    UIElement* element = ui.FindElementByInstanceId(instanceId);
    if (!element)
        return false;
    element->Overrides().Reset(Style::Transform);
    return true;
}

bool SetTransformOrigin(UIManager& ui, std::uint64_t instanceId, UiLength x, UiLength y)
{
    UIElement* element = ui.FindElementByInstanceId(instanceId);
    if (!element || !std::isfinite(x.Value) || !std::isfinite(y.Value))
        return false;
    element->Overrides().Set(Style::TransformOrigin,
                             TransformOriginValue{ToStyleLength(x.Value, x.IsPercent),
                                                  ToStyleLength(y.Value, y.IsPercent)});
    return true;
}

bool ClearTransformOrigin(UIManager& ui, std::uint64_t instanceId)
{
    UIElement* element = ui.FindElementByInstanceId(instanceId);
    if (!element)
        return false;
    element->Overrides().Reset(Style::TransformOrigin);
    return true;
}

} // namespace GameEngine::UI::Script
