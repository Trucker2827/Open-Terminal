// AppleSpeechTranscriber.mm — Objective-C++ implementation.
// Compiled only on Apple (guarded in CMakeLists.txt).
// Built with -fobjc-arc (set via set_source_files_properties in CMake).

#include "web/AppleSpeechTranscriber.h"

#import <Speech/Speech.h>
#import <Foundation/Foundation.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QString>

#include <functional>

namespace {

// ── ffmpeg location ──────────────────────────────────────────────────────────

NSString* find_ffmpeg() {
    NSArray<NSString*>* candidates = @[
        @"/opt/homebrew/bin/ffmpeg",
        @"/usr/local/bin/ffmpeg",
    ];
    for (NSString* p in candidates) {
        if ([[NSFileManager defaultManager] isExecutableFileAtPath:p])
            return p;
    }
    // Fall back to whatever is in PATH
    return @"ffmpeg";
}

// ── Marshal done() to Qt main thread ─────────────────────────────────────────

void invoke_done(std::function<void(bool, QString, QString)> done, bool ok,
                 QString transcript, QString error) {
    // Capture by value so everything outlives the Qt event dispatch.
    QMetaObject::invokeMethod(
        qApp,
        [done = std::move(done), ok, transcript = std::move(transcript),
         error = std::move(error)]() mutable { done(ok, std::move(transcript), std::move(error)); },
        Qt::QueuedConnection);
}

// ── Per-chunk state held on the heap so ARC keeps it alive ───────────────────

struct ChunkState {
    SFSpeechRecognizer*    recognizer;
    NSMutableArray<NSURL*>* chunks;      // sorted chunk URLs to process
    NSInteger               index;       // next chunk to process
    NSMutableString*        result;      // accumulated transcript
    NSString*               tmpDir;      // directory to delete on finish
    std::function<void(bool, QString, QString)> done;
};

// ── Forward declaration ───────────────────────────────────────────────────────

void transcribe_next_chunk(std::shared_ptr<ChunkState> state);

// ── Transcribe one chunk then recurse ────────────────────────────────────────

void transcribe_chunk(std::shared_ptr<ChunkState> state, NSURL* url) {
    SFSpeechURLRecognitionRequest* req =
        [[SFSpeechURLRecognitionRequest alloc] initWithURL:url];

    if (state->recognizer.supportsOnDeviceRecognition)
        req.requiresOnDeviceRecognition = YES;
    req.shouldReportPartialResults = NO;

    // Per-chunk timeout: 60 s (generous for a 50-s chunk).
    __block BOOL did_finish = NO;
    __block SFSpeechRecognitionTask* task = nil;

    dispatch_block_t timeout_block = dispatch_block_create(DISPATCH_BLOCK_INHERIT_QOS_CLASS, ^{
        if (!did_finish) {
            [task cancel];
            // Treat timeout as empty chunk — keep going.
            did_finish = YES;
            transcribe_next_chunk(state);
        }
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(60 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), timeout_block);

    task = [state->recognizer recognitionTaskWithRequest:req
        resultHandler:^(SFSpeechRecognitionResult* _Nullable result,
                        NSError* _Nullable error) {
            if (did_finish) return;

            const BOOL is_final = (error != nil) || (result != nil && result.isFinal);
            if (!is_final) return;

            // Cancel the timeout block.
            dispatch_block_cancel(timeout_block);
            did_finish = YES;

            if (result && result.bestTranscription.formattedString.length > 0) {
                if (state->result.length > 0)
                    [state->result appendString:@" "];
                [state->result appendString:result.bestTranscription.formattedString];
            }
            transcribe_next_chunk(state);
        }];
}

// ── Advance to the next chunk or finish ──────────────────────────────────────

void transcribe_next_chunk(std::shared_ptr<ChunkState> state) {
    if (state->index >= (NSInteger)state->chunks.count) {
        // All chunks done — clean up and return result.
        [[NSFileManager defaultManager]
            removeItemAtPath:state->tmpDir error:nil];

        QString transcript = QString::fromNSString(state->result);
        if (transcript.trimmed().isEmpty()) {
            invoke_done(std::move(state->done), false, {},
                        QStringLiteral("Speech recognition produced no text"));
        } else {
            invoke_done(std::move(state->done), true, std::move(transcript), {});
        }
        return;
    }

    NSURL* url = state->chunks[state->index];
    state->index += 1;
    transcribe_chunk(state, url);
}

// ── Segment audio with ffmpeg ─────────────────────────────────────────────────

// Returns sorted array of chunk URLs, or nil on failure.
// The chunks are written into a fresh temp dir; caller owns cleanup.
NSString* segment_audio(NSString* inputPath, NSMutableArray<NSURL*>* outChunks) {
    NSString* tmpDir = [NSTemporaryDirectory()
        stringByAppendingPathComponent:
            [[NSUUID UUID] UUIDString]];
    [[NSFileManager defaultManager] createDirectoryAtPath:tmpDir
        withIntermediateDirectories:YES attributes:nil error:nil];

    NSString* pattern = [tmpDir stringByAppendingPathComponent:@"chunk%03d.wav"];
    NSString* ffmpeg  = find_ffmpeg();

    NSTask* task = [[NSTask alloc] init];
    task.launchPath = ffmpeg;
    // -c copy works only for same-codec pcm; re-encode to ensure correct format.
    task.arguments = @[
        @"-y", @"-i", inputPath,
        @"-ar", @"16000", @"-ac", @"1",
        @"-f", @"segment", @"-segment_time", @"50",
        @"-c:a", @"pcm_s16le",
        pattern,
    ];
    task.standardOutput = [NSPipe pipe];
    task.standardError  = [NSPipe pipe];

    @try {
        [task launch];
        [task waitUntilExit];
    } @catch (NSException* e) {
        // ffmpeg not found/executable.
        return nil;
    }

    if (task.terminationStatus != 0)
        return nil;

    // Enumerate sorted chunks.
    NSArray<NSString*>* contents =
        [[NSFileManager defaultManager] contentsOfDirectoryAtPath:tmpDir error:nil];
    NSArray<NSString*>* sorted =
        [contents sortedArrayUsingSelector:@selector(compare:)];
    for (NSString* name in sorted) {
        if ([name hasPrefix:@"chunk"] && [name hasSuffix:@".wav"])
            [outChunks addObject:[NSURL fileURLWithPath:
                [tmpDir stringByAppendingPathComponent:name]]];
    }

    if (outChunks.count == 0)
        return nil;

    return tmpDir;
}

} // anonymous namespace

// ── Public C++ entry point ───────────────────────────────────────────────────

namespace openmarketterminal::web {

void AppleSpeechTranscriber::transcribe(
    const QString& audioFilePath,
    std::function<void(bool ok, QString transcript, QString error)> done)
{
    // Step 1: Request authorization — completion fires on an arbitrary queue.
    [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus status) {
        if (status != SFSpeechRecognizerAuthorizationStatusAuthorized) {
            invoke_done(done, false, {},
                QStringLiteral("Speech Recognition permission denied — enable it in "
                               "System Settings → Privacy & Security → Speech Recognition"));
            return;
        }

        // Step 2: Build recognizer.
        NSLocale* locale = [[NSLocale alloc] initWithLocaleIdentifier:@"en-US"];
        SFSpeechRecognizer* recognizer = [[SFSpeechRecognizer alloc] initWithLocale:locale];
        if (!recognizer || !recognizer.available) {
            invoke_done(done, false, {},
                QStringLiteral("Speech recognizer not available for en-US on this device"));
            return;
        }

        // Step 3: Segment audio.
        NSString* inputPath = audioFilePath.toNSString();
        NSMutableArray<NSURL*>* chunks = [NSMutableArray array];
        NSString* tmpDir = segment_audio(inputPath, chunks);
        if (!tmpDir || chunks.count == 0) {
            invoke_done(done, false, {},
                QStringLiteral("Failed to segment audio with ffmpeg — check that "
                               "/opt/homebrew/bin/ffmpeg is installed"));
            return;
        }

        // Step 4: Start sequential chunk transcription.
        auto state = std::make_shared<ChunkState>();
        state->recognizer = recognizer;
        state->chunks     = chunks;
        state->index      = 0;
        state->result     = [NSMutableString string];
        state->tmpDir     = tmpDir;
        state->done       = std::move(done);

        // SFSpeechRecognizer callbacks run on the main thread by default,
        // but let's be explicit and dispatch to main to start the chain.
        dispatch_async(dispatch_get_main_queue(), ^{
            transcribe_next_chunk(state);
        });
    }];
}

} // namespace openmarketterminal::web
