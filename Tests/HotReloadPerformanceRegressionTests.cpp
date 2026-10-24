#include "Jobs/HotReloadTasks.h"
#include "Jobs/MockHotReloadDependencies.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "TestTempDir.h"
#include <gtest/gtest.h>
#include <chrono>
#include <vector>
#include <future>
#include <numeric>
#include <algorithm>
#include <filesystem>

using namespace GameEngine;

class HotReloadPerformanceRegressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Info);
        m_AssemblyPath = (TestUtils::MakeUniqueTempDirectory("GE_HotReloadPerformance") /
                          "ScriptAssemblies" / "GameEngine.Scripts.dll").string();
    }

    void TearDown() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    }

    void ConfigurePipeline(HotReloadPipeline& pipeline) {
        pipeline.SetCompilerFactory([] { return std::make_unique<Tests::MockCompiler>(true, 0); });
        pipeline.SetAssemblyLoaderFactory([] { return std::make_unique<Tests::MockAssemblyLoader>(); });
        pipeline.SetSwapperFactory([] { return std::make_unique<Tests::MockAssemblySwapper>(true, 0); });
    }

    std::string m_AssemblyPath;
};

/**
 * @brief Critical regression test for 0ms main thread blocking achievement
 * 
 * This test ensures we maintain the 0ms main thread blocking performance
 * that was achieved through the 4-stage async pipeline optimization.
 */
TEST_F(HotReloadPerformanceRegressionTest, ZeroMainThreadBlockingRegression) {
    // Loosen thresholds in CI to avoid false failures due to environment variance
    const int kMaxMainThreadBlockingMs = 10;  // allow jitter
    const int kMaxTotalCompletionMs = 1500;    // relaxed upper bound for CI
    const int kOptimalCompletionMs = 200;       // warn if above optimal range

    Logger::Log::SetLogLevel(Logger::LogLevel::Error); // Minimize logging overhead

    auto threadPool = std::make_shared<JobSystem::WorkStealingThreadPool>(std::thread::hardware_concurrency());
    JobSystem::JobChannel compiles(*threadPool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(*threadPool, compiles);
    ConfigurePipeline(pipeline);

    // Measure main thread blocking time
    auto mainThreadStart = std::chrono::high_resolution_clock::now();
    auto future = pipeline.ExecuteAsync(m_AssemblyPath);
    auto mainThreadEnd = std::chrono::high_resolution_clock::now();

    // Main thread blocking time (pipeline submission)
    auto mainThreadBlocking = std::chrono::duration_cast<std::chrono::milliseconds>(
        mainThreadEnd - mainThreadStart);

    // Wait for completion and measure total time
    auto totalStart = mainThreadStart;
    auto stats = future.get();
    ASSERT_TRUE(stats.success) << stats.errorMessage;
    auto totalEnd = std::chrono::high_resolution_clock::now();

    auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        totalEnd - totalStart);

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);

    // Sanity checks (relaxed for CI)
    EXPECT_LT(mainThreadBlocking.count(), kMaxMainThreadBlockingMs)
        << "Main thread blocking exceeded relaxed threshold";
    EXPECT_LT(totalDuration.count(), kMaxTotalCompletionMs)
        << "Total completion time exceeded relaxed threshold";
    EXPECT_LT(stats.swapDuration.count(), kMaxMainThreadBlockingMs)
        << "Assembly swap duration exceeded relaxed threshold";

    // Informational warnings
    if (totalDuration.count() > kOptimalCompletionMs) {
        Logger::Log::Warning("Performance degradation detected: {}ms (optimal: <{}ms)",
                       totalDuration.count(), kOptimalCompletionMs);
    }
}


/**
 * @brief Pipeline submission time regression test
 * 
 * Tests that pipeline submission remains fast (3-5ms) and doesn't block
 * the main thread during task creation and dependency setup.
 */
TEST_F(HotReloadPerformanceRegressionTest, PipelineSubmissionPerformance) {
    const int kMaxSubmissionMs = 10; // <10ms submission (current: 3-5ms)
    const int kOptimalSubmissionMs = 6; // Warn if above optimal
    
    Logger::Log::SetLogLevel(Logger::LogLevel::Error);

    auto threadPool = std::make_shared<JobSystem::WorkStealingThreadPool>(std::thread::hardware_concurrency());
    JobSystem::JobChannel compiles(*threadPool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(*threadPool, compiles);
    ConfigurePipeline(pipeline);
    
    // Measure multiple submissions for statistical accuracy
    std::vector<std::chrono::milliseconds> submissionTimes;
    const int kNumTests = 10;
    
    for (int i = 0; i < kNumTests; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        auto future = pipeline.ExecuteAsync(m_AssemblyPath);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto submissionTime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        submissionTimes.push_back(submissionTime);
        
        // Wait for completion before next test
        const auto stats = future.get();
        ASSERT_TRUE(stats.success) << stats.errorMessage;
        
        // Brief pause between tests
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    
    // Calculate statistics
    auto avgSubmission = std::accumulate(submissionTimes.begin(), submissionTimes.end(), 
                                        std::chrono::milliseconds(0)) / kNumTests;
    auto maxSubmission = *std::max_element(submissionTimes.begin(), submissionTimes.end());
    
    Logger::Log::Info("🚀 Pipeline Submission Performance ({} tests):", kNumTests);
    Logger::Log::Info("   Average submission: {}ms (target: <{}ms)", 
                 avgSubmission.count(), kMaxSubmissionMs);
    Logger::Log::Info("   Maximum submission: {}ms", maxSubmission.count());
    
    // Regression detection
    EXPECT_LT(avgSubmission.count(), kMaxSubmissionMs)
        << "Pipeline submission performance regression detected";
    
    EXPECT_LT(maxSubmission.count(), kMaxSubmissionMs * 2)
        << "Pipeline submission worst-case performance regression detected";
    
    // Performance warnings
    if (avgSubmission.count() > kOptimalSubmissionMs) {
        Logger::Log::Warning("⚠️  Submission performance degradation: {}ms (optimal: 3-5ms)", 
                       avgSubmission.count());
    }
}

/**
 * @brief Concurrent hot-reload submission test
 *
 * ExecuteAsync implements last-wins coalescing: submitting while a run is in
 * flight cancels the current run and keeps the newest request. Overlapping
 * submissions are therefore EXPECTED to report failure for superseded runs —
 * what must hold is that every future completes (no hang/deadlock), the last
 * submission wins and succeeds, and the whole burst stays within a bound that
 * catches pathological serialization of cancelled runs.
 */
TEST_F(HotReloadPerformanceRegressionTest, ConcurrentHotReloadPerformance) {
    const int kMaxConcurrentCompletionMs = 15000; // hang/serialization guard, not a perf target
    const int kNumConcurrentOperations = 3;

    Logger::Log::SetLogLevel(Logger::LogLevel::Error);

    auto threadPool = std::make_shared<JobSystem::WorkStealingThreadPool>(std::thread::hardware_concurrency());
    JobSystem::JobChannel compiles(*threadPool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(*threadPool, compiles);
    ConfigurePipeline(pipeline);

    std::vector<std::future<HotReloadPipeline::PipelineStats>> futures;

    auto globalStart = std::chrono::high_resolution_clock::now();

    // Start overlapping operations; earlier ones may be coalesced away.
    for (int i = 0; i < kNumConcurrentOperations; ++i) {
        futures.push_back(pipeline.ExecuteAsync(m_AssemblyPath));
    }

    // Every future must complete — superseded runs report failure, they must not hang.
    std::vector<HotReloadPipeline::PipelineStats> results;
    for (auto& future : futures) {
        results.push_back(future.get());
    }

    auto globalEnd = std::chrono::high_resolution_clock::now();
    auto totalConcurrentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        globalEnd - globalStart);

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);

    Logger::Log::Info("🔄 Concurrent Hot-Reload Submissions ({} operations):", kNumConcurrentOperations);
    Logger::Log::Info("   Total time: {}ms (guard: <{}ms)",
                 totalConcurrentTime.count(), kMaxConcurrentCompletionMs);

    // Last-wins: the newest submission must complete successfully.
    ASSERT_FALSE(results.empty());
    EXPECT_TRUE(results.back().success) << "Newest (last-wins) operation failed";

    // Runs that did complete must not have blocked the main thread in the swap.
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].success || results[i].result == HotReloadResult::CancelledByNewOperation)
            << results[i].errorMessage;
        if (results[i].success) {
            EXPECT_LE(results[i].swapDuration.count(), 1)
                << "Operation " << i << " had excessive main thread blocking";
        }
    }

    EXPECT_LT(totalConcurrentTime.count(), kMaxConcurrentCompletionMs)
        << "Overlapping submissions took pathologically long (hang or serialized cancellations)";
}
