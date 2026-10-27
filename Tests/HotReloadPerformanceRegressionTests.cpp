#include "Jobs/HotReloadTasks.h"
#include "Jobs/MockHotReloadDependencies.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "TestTempDir.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

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

TEST_F(HotReloadPerformanceRegressionTest, ZeroMainThreadBlockingRegression) {
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


TEST_F(HotReloadPerformanceRegressionTest, PipelineSubmissionPerformance) {
    const int kMaxSubmissionMs = 10;
    const int kOptimalSubmissionMs = 6; // Warn if above optimal
    
    Logger::Log::SetLogLevel(Logger::LogLevel::Error);

    auto threadPool = std::make_shared<JobSystem::WorkStealingThreadPool>(std::thread::hardware_concurrency());
    JobSystem::JobChannel compiles(*threadPool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeline(*threadPool, compiles);
    ConfigurePipeline(pipeline);
    
    // Measure multiple submissions for statistical accuracy
    std::vector<std::chrono::milliseconds> submissionTimes;
    const int kNumTests = 10;
    
    for (int runIndex = 0; runIndex < kNumTests; ++runIndex) {
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
    auto averageSubmission = std::accumulate(submissionTimes.begin(), submissionTimes.end(),
                                        std::chrono::milliseconds(0)) / kNumTests;
    auto maximumSubmission = *std::max_element(submissionTimes.begin(), submissionTimes.end());
    
    Logger::Log::Info("🚀 Pipeline Submission Performance ({} tests):", kNumTests);
    Logger::Log::Info("   Average submission: {}ms (target: <{}ms)", 
                 averageSubmission.count(), kMaxSubmissionMs);
    Logger::Log::Info("   Maximum submission: {}ms", maximumSubmission.count());
    
    // Regression detection
    EXPECT_LT(averageSubmission.count(), kMaxSubmissionMs)
        << "Pipeline submission performance regression detected";
    
    EXPECT_LT(maximumSubmission.count(), kMaxSubmissionMs * 2)
        << "Pipeline submission worst-case performance regression detected";
    
    // Performance warnings
    if (averageSubmission.count() > kOptimalSubmissionMs) {
        Logger::Log::Warning("Submission time: {}ms (preferred: <{}ms)",
                            averageSubmission.count(), kOptimalSubmissionMs);
    }
}

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
    for (int runIndex = 0; runIndex < kNumConcurrentOperations; ++runIndex) {
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
    for (size_t runIndex = 0; runIndex < results.size(); ++runIndex) {
        EXPECT_TRUE(results[runIndex].success || results[runIndex].result == HotReloadResult::CancelledByNewOperation)
            << results[runIndex].errorMessage;
        if (results[runIndex].success) {
            EXPECT_LE(results[runIndex].swapDuration.count(), 1)
                << "Operation " << runIndex << " had excessive main thread blocking";
        }
    }

    EXPECT_LT(totalConcurrentTime.count(), kMaxConcurrentCompletionMs)
        << "Overlapping submissions took pathologically long (hang or serialized cancellations)";
}
