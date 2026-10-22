// Real UI state for Managed/BindingTests. The dotnet host has no frame loop or renderer,
// so the fixture supplies layout boxes and applies authored/runtime overrides with the
// engine style applier. UITests exercises the full cascade and frame lifecycle.
#include "Engine/GameUI/GameUIHost.h"
#include "Engine/GameUI/GameplayUI.h"
#include "Mathematics/Rect.h"
#include "Mathematics/Vector2.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/StyleOverrides.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include "../../Engine/Modules/UI/Source/StyleApplier.h"

#include <cstdint>
#include <memory>

#if defined(_WIN32)
#define GE_TEST_UI_EXPORT extern "C" __declspec(dllexport)
#else
#define GE_TEST_UI_EXPORT extern "C" __attribute__((visibility("default")))
#endif

using namespace GameEngine;

namespace
{
std::unique_ptr<GameUIHost> s_Host;
UIElement* s_Element = nullptr;

void ResolveStyles()
{
    ResolvedStyle& style = s_Element->GetMutableResolvedStyle();
    style = ResolvedStyle{};
    ApplyOverridesToResolvedStyle(style, s_Element->InlineOverrides());
    ApplyOverridesToResolvedStyle(style, s_Element->Overrides());
}
} // namespace

GE_TEST_UI_EXPORT std::uint64_t GE_TestUI_Open()
{
    s_Host = std::make_unique<GameUIHost>(nullptr, nullptr, nullptr);
    UIManager& ui = *s_Host->GetUIManager();
    auto child = std::make_unique<UIElement>();
    s_Element = child.get();
    child->InlineOverrides().Set(Style::Transform, TransformValue{}.Translate(StyleLength::Px(10), StyleLength::Px(20)));
    ui.GetRootElement()->AddChild(std::move(child));
    ResolveStyles();
    UILayoutAccess::SetLastLayoutRect(*ui.GetRootElement(), 0, 0, 400, 300);
    UILayoutAccess::SetLastLayoutRect(*s_Element, 40, 50, 100, 40);
    GameUI::SetHost(s_Host.get());
    return s_Element->GetInstanceId();
}

GE_TEST_UI_EXPORT void GE_TestUI_Close()
{
    s_Element = nullptr;
    s_Host.reset();
}

GE_TEST_UI_EXPORT void GE_TestUI_ReadWindowRect(float* x, float* y, float* width, float* height)
{
    ResolveStyles();
    const Mathematics::Rect rect = s_Element->GetWindowRect();
    *x = rect.X;
    *y = rect.Y;
    *width = rect.Width;
    *height = rect.Height;
}

// Update requires a rendering device. Drive the native coordinate mapping and handler
// dispatch directly here; UITests covers the UIManager hit-test/frame path with a device.
GE_TEST_UI_EXPORT void GE_TestUI_DispatchPointer(float x, float y)
{
    Mathematics::Vector2 layout;
    if (!s_Element->WindowToLayout(Mathematics::Vector2{x, y}, layout))
        return;
    UIEvent event{};
    event.Id = kEventMouseMove;
    event.Target = s_Element;
    event.CurrentTarget = s_Element;
    event.X = layout.x;
    event.Y = layout.y;
    event.WindowX = x;
    event.WindowY = y;
    s_Element->DispatchEvent(event);
}
