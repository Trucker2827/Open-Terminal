// TouchIdAuth.mm — Objective-C++ implementation.
// Compiled only on Apple (guarded in CMakeLists.txt).
// Built with -fobjc-arc (set via set_source_files_properties in CMake).

#include "auth/TouchIdAuth.h"

#import <LocalAuthentication/LocalAuthentication.h>
#import <Foundation/Foundation.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QString>

#include <functional>

namespace openmarketterminal::auth {

bool TouchIdAuth::is_available() {
    LAContext* ctx = [[LAContext alloc] init];
    NSError* err = nil;
    return [ctx canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics
                            error:&err];
}

void TouchIdAuth::authenticate(const QString& reason,
                               std::function<void(bool ok, QString error)> done) {
    NSString* ns_reason = reason.toNSString();
    LAContext* ctx = [[LAContext alloc] init];

    // Copy the std::function so the ObjC block retains its own instance.
    // The block may be invoked on a background thread; QMetaObject::invokeMethod
    // marshals the result to the Qt main thread before calling the user callback.
    auto callback = done;

    [ctx evaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics
        localizedReason:ns_reason
                  reply:^(BOOL success, NSError* err) {
        const bool ok = success;
        QString err_str;
        if (!ok) {
            err_str = err
                ? QString::fromNSString(err.localizedDescription)
                : QStringLiteral("Touch ID failed");
        }
        // Capture by value — everything must outlive the Qt event dispatch.
        QMetaObject::invokeMethod(qApp, [callback, ok, err_str]() {
            callback(ok, err_str);
        }, Qt::QueuedConnection);
    }];
}

} // namespace openmarketterminal::auth
