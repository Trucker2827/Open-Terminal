#include "services/stt/AppleSpeechProvider.h"

#include "core/logging/Logger.h"
#include "services/stt/SpeechService.h"

#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>

#include <QMetaObject>
#include <QTimer>

#include <atomic>

namespace openmarketterminal::services {

class AppleSpeechProvider final : public SttProvider {
  public:
    AppleSpeechProvider() {
        finish_timer_.setSingleShot(true);
        finish_timer_.setInterval(900);
        connect(&finish_timer_, &QTimer::timeout, this, [this]() {
            const QString text = pending_text_.trimmed();
            if (!text.isEmpty()) {
                pending_text_.clear();
                emit transcription(text);
                stop();
            }
        });
    }
    ~AppleSpeechProvider() override { stop(); }

    QString name() const override { return QStringLiteral("apple"); }
    bool is_active() const noexcept override { return active_.load(); }

    void start() override {
        if (active_.load())
            return;

        auto begin = [this]() {
            if (SFSpeechRecognizer.authorizationStatus !=
                SFSpeechRecognizerAuthorizationStatusAuthorized) {
                emit error(QStringLiteral(
                    "Allow Speech Recognition in System Settings → Privacy & Security."));
                return;
            }
            if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] !=
                AVAuthorizationStatusAuthorized) {
                emit error(QStringLiteral(
                    "Allow Microphone access in System Settings → Privacy & Security."));
                return;
            }
            start_engine();
        };

        const auto request_speech = [begin]() {
            if (SFSpeechRecognizer.authorizationStatus ==
                SFSpeechRecognizerAuthorizationStatusNotDetermined) {
                [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus) {
                    dispatch_async(dispatch_get_main_queue(), begin);
                }];
            } else {
                begin();
            }
        };

        if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] ==
            AVAuthorizationStatusNotDetermined) {
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                    completionHandler:^(BOOL) {
                dispatch_async(dispatch_get_main_queue(), request_speech);
            }];
        } else {
            request_speech();
        }
    }

    void stop() override {
        finish_timer_.stop();
        pending_text_.clear();
        if (engine_) {
            if (engine_.isRunning)
                [engine_ stop];
            @try {
                [engine_.inputNode removeTapOnBus:0];
            } @catch (NSException*) {
            }
        }
        [request_ endAudio];
        [task_ cancel];
        task_ = nil;
        request_ = nil;
        engine_ = nil;
        recognizer_ = nil;
        if (active_.exchange(false))
            emit active_changed(false);
    }

  private:
    void start_engine() {
        stop();
        finish_timer_.setInterval(900);

        recognizer_ = [[SFSpeechRecognizer alloc]
            initWithLocale:[[NSLocale alloc] initWithLocaleIdentifier:@"en-AU"]];
        if (!recognizer_ || !recognizer_.available) {
            emit error(QStringLiteral("Apple Speech Recognition is unavailable."));
            return;
        }

        request_ = [[SFSpeechAudioBufferRecognitionRequest alloc] init];
        request_.shouldReportPartialResults = YES;
        if (@available(macOS 13.0, *))
            request_.addsPunctuation = YES;

        engine_ = [[AVAudioEngine alloc] init];
        AVAudioInputNode* input = engine_.inputNode;
        AVAudioFormat* format = [input outputFormatForBus:0];
        if (!format || format.sampleRate <= 0 || format.channelCount == 0) {
            emit error(QStringLiteral("No usable microphone input is available."));
            stop();
            return;
        }

        AppleSpeechProvider* provider = this;
        [input installTapOnBus:0 bufferSize:1024 format:format
                         block:^(AVAudioPCMBuffer* buffer, AVAudioTime*) {
            [provider->request_ appendAudioPCMBuffer:buffer];
        }];

        NSError* engine_error = nil;
        [engine_ prepare];
        if (![engine_ startAndReturnError:&engine_error]) {
            emit error(QStringLiteral("Could not start the microphone: ") +
                       QString::fromNSString(engine_error.localizedDescription));
            stop();
            return;
        }

        active_.store(true);
        emit active_changed(true);
        LOG_INFO("AppleSpeechProvider", "Native Apple Speech is listening");

        task_ = [recognizer_ recognitionTaskWithRequest:request_
            resultHandler:^(SFSpeechRecognitionResult* result, NSError* task_error) {
                if (result) {
                    const QString text =
                        QString::fromNSString(result.bestTranscription.formattedString).trimmed();
                    if (!text.isEmpty()) {
                        QMetaObject::invokeMethod(provider, [provider, text]() {
                            provider->pending_text_ = text;
                            provider->finish_timer_.start();
                        }, Qt::QueuedConnection);
                    }
                    if (result.isFinal && !text.isEmpty()) {
                        QMetaObject::invokeMethod(provider, [provider]() {
                            provider->finish_timer_.setInterval(50);
                            provider->finish_timer_.start();
                        }, Qt::QueuedConnection);
                    }
                }
                if (task_error && task_error.code != 1 && task_error.code != 203) {
                    const QString detail =
                        QString::fromNSString(task_error.localizedDescription);
                    if (!detail.contains(QStringLiteral("No speech detected"),
                                         Qt::CaseInsensitive)) {
                        QMetaObject::invokeMethod(provider, [provider, detail]() {
                            emit provider->error(QStringLiteral("Apple Dictation: ") + detail);
                            provider->stop();
                        }, Qt::QueuedConnection);
                    }
                }
            }];
    }

    SFSpeechRecognizer* recognizer_ = nil;
    SFSpeechAudioBufferRecognitionRequest* request_ = nil;
    SFSpeechRecognitionTask* task_ = nil;
    AVAudioEngine* engine_ = nil;
    std::atomic<bool> active_{false};
    QTimer finish_timer_;
    QString pending_text_;
};

std::unique_ptr<SttProvider> create_apple_speech_provider() {
    return std::make_unique<AppleSpeechProvider>();
}

} // namespace openmarketterminal::services
