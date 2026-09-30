/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>

#include "Session.h"

using ::aidl::android::hardware::biometrics::common::BnCancellationSignal;

namespace aidl {
namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {

class CancellationSignal : public BnCancellationSignal {
public:
    explicit CancellationSignal(const std::shared_ptr<Session>& session);
    ndk::ScopedAStatus cancel() override;

private:
    std::weak_ptr<Session> mSession;
};

} // namespace fingerprint
} // namespace biometrics
} // namespace hardware
} // namespace android
} // namespace aidl
