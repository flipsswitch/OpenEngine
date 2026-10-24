#pragma once

#import <AVFoundation/AVFoundation.h>

#include <cmath>

namespace GameEngine::Platform::Detail
{
inline bool InstallSpeechAudioTap(AVAudioNode* node, AVAudioFormat* format,
                                 AVAudioNodeTapBlock block, NSError** error)
{
    // Audio taps support 100 to 400 ms requests; use the shortest duration at the device's rate.
    constexpr double kBufferDurationSeconds = 0.1;
    const auto frames = static_cast<AVAudioFrameCount>(std::ceil(format.sampleRate * kBufferDurationSeconds));
#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 270000
    return [node installTapOnBus:0 bufferSize:frames format:format error:error block:block];
#else
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
    if (@available(macOS 27.0, *))
        return [node installTapOnBus:0 bufferSize:frames format:format error:error block:block];
    else
#endif
    {
        [node installTapOnBus:0 bufferSize:frames format:format block:block];
        return true;
    }
#endif
}
}
