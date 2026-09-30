/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <aidl/android/hardware/biometrics/fingerprint/ISessionCallback.h>

#include <memory>
#include <mutex>

namespace aidl::android::hardware::biometrics::fingerprint {

struct IlluminationState;
class IlluminationClient;

/** Serializes capture ownership without holding state locks across Binder calls. */
class IlluminationController {
public:
    explicit IlluminationController(const std::shared_ptr<ISessionCallback>& callback);
    ~IlluminationController();

    void startOperation();
    void finishOperation();
    void pointerDown(bool isSyntheticAod = false);
    void pointerUp();
    void pointerCancel();
    void vendorPointerDown();
    void vendorPointerUp();
    void close();

    static void forceOff();

private:
    enum class ContactSource { Ui, Aod, Vendor };
    void updateContact(ContactSource source, bool down);
    void reconcile();

    std::shared_ptr<IlluminationState> mState;
    // Failure/death callbacks never acquire this mutex or call the remote service.
    std::mutex mRequestMutex;
    std::shared_ptr<IlluminationClient> mClient;
};

}  // namespace aidl::android::hardware::biometrics::fingerprint
