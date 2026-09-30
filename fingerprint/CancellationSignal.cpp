/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "CancellationSignal.h"

namespace aidl {
namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {

CancellationSignal::CancellationSignal(const std::shared_ptr<Session>& session)
    : mSession(session) {
}

ndk::ScopedAStatus CancellationSignal::cancel() {
    if (auto session = mSession.lock(); session && !session->isClosed()) {
        return session->cancel();
    }
    return ndk::ScopedAStatus::ok();
}

} // namespace fingerprint
} // namespace biometrics
} // namespace hardware
} // namespace android
} // namespace aidl
