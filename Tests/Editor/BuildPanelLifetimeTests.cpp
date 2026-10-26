#include "Panels/BuildPanel.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UiDispatcher.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace GameEngine
{

struct BuildPanelTestAccess
{
    static void ReportProgress(BuildPanel& panel, const BuildProgress& progress)
    {
        panel.OnBuildProgress(progress);
    }

    static std::string StatusText(const BuildPanel& panel)
    {
        return panel.m_StatusLabel->GetText();
    }
};

} // namespace GameEngine

namespace
{

using namespace GameEngine;

TEST(BuildPanelLifetime, QueuedProgressUpdatesTheLivePanel)
{
    UIManager manager(nullptr);
    auto panel = std::make_unique<BuildPanel>();
    BuildPanel* pointer = panel.get();
    manager.SetRoot(std::move(panel));

    BuildProgress progress;
    progress.progress = 0.5f;
    progress.statusMessage = "Compiling scripts";
    BuildPanelTestAccess::ReportProgress(*pointer, progress);
    ASSERT_GT(manager.GetDispatcher()->PendingCount(), 0u);
    manager.DrainDeferredActionsOnce();

    EXPECT_EQ(BuildPanelTestAccess::StatusText(*pointer), progress.statusMessage);
}

TEST(BuildPanelLifetime, QueuedProgressDoesNotOutliveThePanel)
{
    UIManager manager(nullptr);
    auto root = std::make_unique<UIElement>();
    UIElement* rootPointer = root.get();
    manager.SetRoot(std::move(root));
    auto panel = std::make_unique<BuildPanel>();
    BuildPanel* pointer = panel.get();
    rootPointer->AddChild(std::move(panel));

    BuildProgress progress;
    progress.cancelled = true;
    progress.statusMessage = "Build cancelled";
    BuildPanelTestAccess::ReportProgress(*pointer, progress);
    ASSERT_GT(manager.GetDispatcher()->PendingCount(), 0u);

    // The editor destroys its panels before the owning UIManager's final drain.
    rootPointer->RemoveChild(pointer);
    EXPECT_NO_THROW(manager.DrainDeferredActionsOnce());
    EXPECT_EQ(manager.GetDispatcher()->PendingCount(), 0u);
}

} // namespace
