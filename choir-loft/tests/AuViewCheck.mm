// Opens the installed AU's real window through the system, the way a host does, without showing it on screen. Repeats, with and without audio.
// Build: clang++ -std=c++17 -fobjc-arc -framework AudioToolbox -framework AudioUnit -framework AppKit -framework CoreAudioKit tests/AuViewCheck.mm -o build/AuViewCheck
// Run: build/AuViewCheck aumf SHrm Luke
#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static OSType code (const char* s) { return ((OSType) s[0] << 24) | ((OSType) s[1] << 16) | ((OSType) s[2] << 8) | (OSType) s[3]; }

static OSStatus silence (void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList* io)
{
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
        std::fill_n ((float*) io->mBuffers[b].mData, io->mBuffers[b].mDataByteSize / sizeof (float), 0.1f);
    return noErr;
}

int main (int, char** v)
{
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        AudioComponentDescription d {};
        d.componentType = code (v[1]); d.componentSubType = code (v[2]); d.componentManufacturer = code (v[3]);
        AudioComponent c = AudioComponentFindNext (nullptr, &d);
        if (c == nullptr) { puts ("FAIL not found"); return 1; }

        int failures = 0;
        for (const bool withAudio : { false, true })
        {
            AudioUnit au = nullptr;
            if (AudioComponentInstanceNew (c, &au) != noErr) { puts ("FAIL open"); return 1; }

            AudioStreamBasicDescription f {};
            f.mSampleRate = 48000; f.mFormatID = kAudioFormatLinearPCM; f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
            f.mBytesPerPacket = 4; f.mFramesPerPacket = 1; f.mBytesPerFrame = 4; f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32;
            AudioUnitSetProperty (au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
            AudioUnitSetProperty (au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f);
            AURenderCallbackStruct cb { silence, nullptr };
            AudioUnitSetProperty (au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
            AudioUnitInitialize (au);

            std::atomic<bool> running { withAudio };
            std::thread audio ([&]
            {
                std::vector<float> l (256), r (256);
                AudioTimeStamp ts {}; ts.mFlags = kAudioTimeStampSampleTimeValid;
                while (running.load())
                {
                    struct { UInt32 n; AudioBuffer b[2]; } list { 2, { { 1, 1024, l.data() }, { 1, 1024, r.data() } } };
                    AudioUnitRenderActionFlags flags = 0;
                    AudioUnitRender (au, &flags, &ts, 0, 256, (AudioBufferList*) &list);
                    ts.mSampleTime += 256;
                    std::this_thread::sleep_for (std::chrono::milliseconds (5));
                }
            });

            UInt32 size = 0;
            Boolean writable = false;
            if (AudioUnitGetPropertyInfo (au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size, &writable) != noErr) { puts ("FAIL no Cocoa view"); return 1; }
            std::vector<char> info (size);
            AudioUnitGetProperty (au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, info.data(), &size);
            auto* cocoa = (AudioUnitCocoaViewInfo*) info.data();
            NSBundle* bundle = [NSBundle bundleWithURL: (__bridge NSURL*) cocoa->mCocoaAUViewBundleLocation];
            Class viewClass = [bundle classNamed: (__bridge NSString*) cocoa->mCocoaAUViewClass[0]];
            id<AUCocoaUIBase> factory = [[viewClass alloc] init];

            NSSize shown = NSZeroSize;
            for (int i = 0; i < 40; ++i)
            {
                @autoreleasepool
                {
                    NSView* view = [factory uiViewForAudioUnit: au withSize: NSMakeSize (0, 0)];
                    if (view == nil) { ++failures; continue; }
                    shown = view.frame.size;
                    [[NSRunLoop currentRunLoop] runUntilDate: [NSDate dateWithTimeIntervalSinceNow: 0.03]];
                    [view removeFromSuperview];
                }
                [[NSRunLoop currentRunLoop] runUntilDate: [NSDate dateWithTimeIntervalSinceNow: 0.005]];
            }
            printf ("%s  40 opens through the system, %s audio, view %.0f x %.0f\n", failures == 0 ? "PASS" : "FAIL", withAudio ? "with" : "no", shown.width, shown.height);

            running = false;
            audio.join();
            AudioUnitUninitialize (au);
            AudioComponentInstanceDispose (au);
        }
        return failures == 0 ? 0 : 1;
    }
}
