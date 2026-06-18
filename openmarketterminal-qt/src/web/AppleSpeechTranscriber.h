#pragma once
// AppleSpeechTranscriber — on-device Apple Speech Recognition fallback.
// This header is pure C++ (no ObjC types) so it can be included by
// Qt/C++ translation units on all platforms.
//
// On non-Apple platforms the .mm is not compiled; callers must guard
// invocations with #ifdef Q_OS_MACOS.

#include <QString>
#include <functional>

namespace openmarketterminal::web {

/// Transcribes a local audio file using Apple's on-device SFSpeechRecognizer.
///
/// The file is segmented into ~50-second chunks via ffmpeg, transcribed
/// sequentially, then results are concatenated and returned.
///
/// The `done` callback is always invoked on the Qt main thread.
class AppleSpeechTranscriber {
  public:
    /// Transcribe `audioFilePath` (WAV/AIFF/M4A) on-device.
    /// `done` is called exactly once with:
    ///   ok=true  + transcript text on success
    ///   ok=false + error message on any failure (permission, no recognizer,
    ///              ffmpeg missing, task timeout, etc.)
    static void transcribe(const QString& audioFilePath,
                           std::function<void(bool ok, QString transcript, QString error)> done);
};

} // namespace openmarketterminal::web
