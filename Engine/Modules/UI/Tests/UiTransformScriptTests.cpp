#include "IsolatedUIFixture.h"

#include "Engine/GameUI/GameUIHost.h"
#include "Engine/GameUI/GameplayUI.h"
#include "Scripting/ScriptingABI.h"
#include "UI/StyleOverrides.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UiTransformsScriptApi.h"

#include <array>
#include <limits>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{
void GE_CDECL CapturePointer(const GE_UIEventData* event, void* user)
{
    *static_cast<GE_UIEventData*>(user) = *event;
}
} // namespace

TEST(UiTransformScript, PointerPayloadPreservesLayoutAndWindowCoordinates)
{
    GameUIHost host(nullptr, nullptr, nullptr);
    GameUI::SetHost(&host);
    UIElement* element = host.GetUIManager()->GetRootElement();
    GE_UIEventData received{};
    uint64_t token = 0;
    ASSERT_EQ(GE_UIElement_RegisterEvent(element->GetInstanceId(), "UI.MouseMove", CapturePointer, &received, &token),
              GE_Result_Ok);
    ASSERT_NE(token, 0u);

    UIEvent event{};
    event.Id = kEventMouseMove;
    event.Target = element;
    event.CurrentTarget = element;
    event.X = 25.0f;
    event.Y = 40.0f;
    event.WindowX = 125.0f;
    event.WindowY = 240.0f;
    event.Mods = 3;
    element->DispatchEvent(event);

    EXPECT_EQ(received.elementInstanceId, element->GetInstanceId());
    EXPECT_FLOAT_EQ(received.x, 25.0f);
    EXPECT_FLOAT_EQ(received.y, 40.0f);
    EXPECT_FLOAT_EQ(received.windowX, 125.0f);
    EXPECT_FLOAT_EQ(received.windowY, 240.0f);
    EXPECT_EQ(received.mods, 3);
    EXPECT_EQ(GE_UIElement_UnregisterEvent(element->GetInstanceId(), "UI.MouseMove", token), GE_Result_Ok);
}

TEST(UiTransformScript, OrderedOperationsKeepTheirArgumentsAndUnits)
{
    using namespace UI::Script;
    UIManager ui(nullptr);
    ui.SetRoot(std::make_unique<UIElement>());
    UIElement& element = *ui.GetRootElement();
    const std::array operations = {
        UiTransformOperation{TransformFunction::Translate, -50.0f, 12.0f, 0, 0, 0, 0, true, false},
        UiTransformOperation{TransformFunction::Scale, 2.0f, 3.0f},
        UiTransformOperation{TransformFunction::Rotate, 45.0f},
        UiTransformOperation{TransformFunction::Skew, 10.0f, 20.0f},
        UiTransformOperation{TransformFunction::Matrix, 1, 2, 3, 4, 5, 6}};

    ASSERT_TRUE(SetTransform(ui, element.GetInstanceId(), operations));
    const auto transform = element.Overrides().Get(Style::Transform);
    ASSERT_TRUE(transform.has_value());
    const auto expected = TransformValue{}.Translate(StyleLength::Percent(-50), StyleLength::Px(12)).Scale(2, 3).Rotate(45).Skew(10, 20).Matrix(1, 2, 3, 4, 5, 6);
    EXPECT_EQ(*transform, expected);
    EXPECT_FALSE(element.Overrides().IsLayoutDirty());
    EXPECT_TRUE(element.Overrides().IsVisualDirty());

    ASSERT_TRUE(SetTransformOrigin(ui, element.GetInstanceId(), UiLength{50, true}, UiLength{12, false}));
    const auto origin = element.Overrides().Get(Style::TransformOrigin);
    ASSERT_TRUE(origin.has_value());
    EXPECT_EQ(origin->X, StyleLength::Percent(50));
    EXPECT_EQ(origin->Y, StyleLength::Px(12));
}

TEST(UiTransformScript, EmptyTransformOverridesTheCascadeAndClearRestoresIt)
{
    using namespace UI::Script;
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f, R"(<uielement id="root"/>)",
                                     "#root { width: 100px; height: 40px; transform: rotate(30deg); transform-origin: 12px 18px; }");
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fixture.Diagnostic();
    UIManager& ui = fixture.Manager();
    UIElement& element = *fixture.Element("root");
    const TransformValue authored = element.GetResolvedStyle().Visual.Transform;
    ASSERT_EQ(authored.Ops().size(), 1u);
    ASSERT_EQ(authored.Ops()[0].Function, TransformFunction::Rotate);
    ASSERT_FLOAT_EQ(authored.Ops()[0].Args[0], 30.0f);

    ASSERT_TRUE(SetTransform(ui, element.GetInstanceId(), {}));
    ASSERT_TRUE(SetTransformOrigin(ui, element.GetInstanceId(), UiLength{50, true}, UiLength{100, true}));
    fixture.Settle();
    EXPECT_TRUE(element.GetResolvedStyle().Visual.Transform.IsNone());
    EXPECT_EQ(element.GetResolvedStyle().Visual.TransformOrigin.Y, StyleLength::Percent(100));

    ASSERT_TRUE(ClearTransform(ui, element.GetInstanceId()));
    ASSERT_TRUE(ClearTransformOrigin(ui, element.GetInstanceId()));
    fixture.Settle();
    EXPECT_EQ(element.GetResolvedStyle().Visual.Transform, authored);
    EXPECT_EQ(element.GetResolvedStyle().Visual.TransformOrigin.X, StyleLength::Px(12));
    EXPECT_EQ(element.GetResolvedStyle().Visual.TransformOrigin.Y, StyleLength::Px(18));
    EXPECT_TRUE(ClearTransform(ui, element.GetInstanceId()));
    EXPECT_TRUE(ClearTransformOrigin(ui, element.GetInstanceId()));
}

TEST(UiTransformScript, InvalidValuesLeaveTheEntireOverrideUnchanged)
{
    using namespace UI::Script;
    UIManager ui(nullptr);
    ui.SetRoot(std::make_unique<UIElement>());
    UIElement& element = *ui.GetRootElement();
    element.Overrides().Set(Style::Transform, TransformValue{}.Rotate(30));
    element.Overrides().Set(Style::TransformOrigin, TransformOriginValue{});
    std::array operations = {UiTransformOperation{TransformFunction::Scale, 2, 2},
                             UiTransformOperation{static_cast<TransformFunction>(255)}};

    EXPECT_FALSE(SetTransform(ui, element.GetInstanceId(), operations));
    EXPECT_EQ(*element.Overrides().Get(Style::Transform), TransformValue{}.Rotate(30));
    operations[1] = UiTransformOperation{TransformFunction::Rotate, std::numeric_limits<float>::quiet_NaN()};
    EXPECT_FALSE(SetTransform(ui, element.GetInstanceId(), operations));
    operations[1].A = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(SetTransform(ui, element.GetInstanceId(), operations));
    EXPECT_EQ(*element.Overrides().Get(Style::Transform), TransformValue{}.Rotate(30));
    EXPECT_FALSE(SetTransformOrigin(ui, element.GetInstanceId(), UiLength{0, false},
                                    UiLength{std::numeric_limits<float>::infinity(), true}));
    EXPECT_EQ(*element.Overrides().Get(Style::TransformOrigin), TransformOriginValue{});
}

TEST(UiTransformScript, DetachedDestroyedAndForeignElementsAreRefused)
{
    using namespace UI::Script;
    UIManager ui(nullptr);
    UIManager other(nullptr);
    ui.SetRoot(std::make_unique<UIElement>());
    other.SetRoot(std::make_unique<UIElement>());
    auto child = std::make_unique<UIElement>();
    UIElement* childPointer = child.get();
    const uint64_t id = child->GetInstanceId();
    ui.GetRootElement()->AddChild(std::move(child));
    ASSERT_TRUE(SetTransform(ui, id, {}));
    EXPECT_FALSE(SetTransform(other, id, {}));
    EXPECT_FALSE(ClearTransformOrigin(other, id));

    auto detached = ui.GetRootElement()->TakeChild(childPointer);
    EXPECT_FALSE(SetTransform(ui, id, {}));
    EXPECT_FALSE(SetTransformOrigin(ui, id, {}, {}));
    EXPECT_FALSE(ClearTransform(ui, id));
    EXPECT_FALSE(ClearTransformOrigin(ui, id));
    detached.reset();
    EXPECT_FALSE(SetTransform(ui, id, {}));
    EXPECT_FALSE(ClearTransform(ui, 0));
}
