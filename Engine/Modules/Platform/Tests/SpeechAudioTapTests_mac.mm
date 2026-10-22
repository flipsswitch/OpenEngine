#include "SpeechAudioTap_mac.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>

namespace
{
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
struct CapturedAudio
{
    std::atomic<unsigned> Frames{0};
    std::atomic<bool> SamplesMatch{true};
};

class API_AVAILABLE(macos(27.0)) OfflineSpeechAudio
{
public:
    ~OfflineSpeechAudio() { RemoveTap(); }

    bool Initialize(double sampleRate, NSError** error)
    {
        m_Format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:sampleRate channels:1];
        m_FramesPerRender = static_cast<AVAudioFrameCount>(sampleRate / 10);
        m_Engine = [[AVAudioEngine alloc] init];
        AVAudioSourceNode* source = [[AVAudioSourceNode alloc] initWithFormat:m_Format
            renderBlock:^OSStatus(BOOL* silence, const AudioTimeStamp*, AVAudioFrameCount frames, AudioBufferList* output)
        {
            *silence = NO;
            for (unsigned channel = 0; channel < output->mNumberBuffers; ++channel)
                std::fill_n(static_cast<float*>(output->mBuffers[channel].mData), frames, 0.25f);
            return noErr;
        }];
        [m_Engine attachNode:source];
        return [m_Engine connect:source to:m_Engine.mainMixerNode format:m_Format error:error]
            && [m_Engine enableManualRenderingMode:AVAudioEngineManualRenderingModeOffline format:m_Format
                                maximumFrameCount:m_FramesPerRender error:error];
    }

    bool InstallTap(NSError** error)
    {
        m_Captured = std::make_shared<CapturedAudio>();
        auto captured = m_Captured;
        m_Tapped = GameEngine::Platform::Detail::InstallSpeechAudioTap(m_Engine.mainMixerNode, m_Format,
            ^(AVAudioPCMBuffer* buffer, AVAudioTime*)
        {
            for (AVAudioFrameCount frame = 0; frame < buffer.frameLength; ++frame)
            {
                if (buffer.floatChannelData[0][frame] != 0.25f)
                    captured->SamplesMatch = false;
            }
            captured->Frames += buffer.frameLength;
        }, error);
        return m_Tapped;
    }

    bool InstallDuplicate(NSError** error)
    {
        return GameEngine::Platform::Detail::InstallSpeechAudioTap(m_Engine.mainMixerNode, m_Format,
            ^(AVAudioPCMBuffer*, AVAudioTime*) {}, error);
    }

    bool Render(NSError** error)
    {
        AVAudioPCMBuffer* output = [[AVAudioPCMBuffer alloc] initWithPCMFormat:m_Format frameCapacity:m_FramesPerRender];
        if (![m_Engine startAndReturnError:error])
            return false;
        for (unsigned buffer = 0; buffer < 10; ++buffer)
        {
            if ([m_Engine renderOffline:m_FramesPerRender toBuffer:output error:error] != AVAudioEngineManualRenderingStatusSuccess)
                return false;
        }
        [m_Engine stop];
        return true;
    }

    void RemoveTap()
    {
        [m_Engine stop];
        if (m_Tapped)
            [m_Engine.mainMixerNode removeTapOnBus:0];
        m_Tapped = false;
    }

    const CapturedAudio& Captured() const { return *m_Captured; }

private:
    AVAudioEngine* m_Engine = nil;
    AVAudioFormat* m_Format = nil;
    AVAudioFrameCount m_FramesPerRender = 0;
    bool m_Tapped = false;
    std::shared_ptr<CapturedAudio> m_Captured;
};
#endif

TEST(SpeechAudioTap, CapturesPcmAcrossSampleRatesAndRepeatedInstallation)
{
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
    if (@available(macOS 27.0, *))
    {
        @autoreleasepool
        {
            for (double rate : {8000.0, 44100.0, 48000.0, 96000.0})
            {
                SCOPED_TRACE(rate);
                OfflineSpeechAudio audio;
                NSError* error = nil;
                ASSERT_TRUE(audio.Initialize(rate, &error)) << error.description.UTF8String;
                for (unsigned pass = 0; pass < 3; ++pass)
                {
                    SCOPED_TRACE(pass);
                    ASSERT_TRUE(audio.InstallTap(&error)) << error.description.UTF8String;
                    ASSERT_TRUE(audio.Render(&error)) << error.description.UTF8String;
                    EXPECT_GT(audio.Captured().Frames.load(), 0u);
                    EXPECT_TRUE(audio.Captured().SamplesMatch.load());
                    audio.RemoveTap();
                    const unsigned frames = audio.Captured().Frames;
                    ASSERT_TRUE(audio.Render(&error)) << error.description.UTF8String;
                    EXPECT_EQ(audio.Captured().Frames.load(), frames);
                }
            }
        }
        return;
    }
#endif
    GTEST_SKIP() << "The error-returning audio tap requires the macOS 27 SDK and runtime.";
}

TEST(SpeechAudioTap, ReportsInstallationFailureWithoutReplacingTheExistingTap)
{
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
    if (@available(macOS 27.0, *))
    {
        @autoreleasepool
        {
            OfflineSpeechAudio audio;
            NSError* error = nil;
            ASSERT_TRUE(audio.Initialize(48000, &error)) << error.description.UTF8String;
            ASSERT_TRUE(audio.InstallTap(&error)) << error.description.UTF8String;
            @try
            {
                EXPECT_FALSE(audio.InstallDuplicate(&error));
                EXPECT_NE(error, nil);
            }
            @catch (NSException* exception)
            {
                ADD_FAILURE() << "Tap installation threw instead of returning an error: " << exception.reason.UTF8String;
            }
            error = nil;
            ASSERT_TRUE(audio.Render(&error)) << error.description.UTF8String;
            EXPECT_GT(audio.Captured().Frames.load(), 0u);
            EXPECT_TRUE(audio.Captured().SamplesMatch.load());
            audio.RemoveTap();
            ASSERT_TRUE(audio.InstallTap(&error)) << error.description.UTF8String;
            ASSERT_TRUE(audio.Render(&error)) << error.description.UTF8String;
            EXPECT_GT(audio.Captured().Frames.load(), 0u);
        }
        return;
    }
#endif
    GTEST_SKIP() << "The error-returning audio tap requires the macOS 27 SDK and runtime.";
}
}
