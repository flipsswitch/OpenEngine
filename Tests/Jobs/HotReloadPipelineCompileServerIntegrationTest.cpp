#include "CompileServerTestHost.h"
#include "Jobs/HotReloadTasks.h"
#include "Jobs/HotReloadTestHooks.h"
#include "Jobs/WorkspaceId.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestEnvVar.h"
#include "TestTempDir.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

using namespace GameEngine;

TEST(HotReloadPipelineCompileServerIntegration, UsesCompileServerWhenFlagSet)
{
    const TestUtils::ScopedTempDir workspace(
        TestUtils::MakeUniqueTempDirectory("GE_Pipeline_CS_Integration"));
    {
        std::ofstream source(workspace.Path() / "Foo.cs");
        source << "public static class Foo { public static int Smoke() => 1; }";
        ASSERT_TRUE(source.good());
    }

    const std::string pipeName = "GE_CompileServer_" + WorkspaceId::Compute(workspace.Path().string());
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName))
        << "CompileServerHost must be built and staged for this integration suite";

    Testing::SetEnvVar("GE_DISABLE_COMPILE_SERVER", "0");
    ResetLastDefaultCompilerSelection();
    const auto assemblyDirectory = workspace.Path() / "ScriptAssemblies";
    std::filesystem::create_directories(assemblyDirectory);
    const auto assemblyPath = assemblyDirectory / "GameEngine.Scripts.dll";

    JobSystem::WorkStealingThreadPool pool(4);
    JobSystem::JobChannel compiles(pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(pool, compiles);
    auto run = pipeline.ExecuteAsync(assemblyPath.string());
    ASSERT_EQ(run.wait_for(std::chrono::seconds(30)), std::future_status::ready)
        << "Pipeline did not complete in time";
    const auto stats = run.get();

    EXPECT_TRUE(WasLastDefaultCompilerServer());
    ASSERT_TRUE(stats.success) << stats.errorMessage;
    EXPECT_EQ(stats.result, HotReloadResult::Success);
    ASSERT_TRUE(std::filesystem::exists(assemblyPath));
    std::ifstream assembly(assemblyPath, std::ios::binary);
    std::array<char, 2> signature{};
    assembly.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    EXPECT_EQ(assembly.gcount(), 2);
    EXPECT_EQ(signature[0], 'M');
    EXPECT_EQ(signature[1], 'Z');
}
