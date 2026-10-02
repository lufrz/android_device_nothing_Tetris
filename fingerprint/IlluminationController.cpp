/* SPDX-License-Identifier: Apache-2.0 */

#include "IlluminationController.h"

#include <aidl/vendor/nothing/hardware/udfps/BnIlluminationCallback.h>
#include <aidl/vendor/nothing/hardware/udfps/IIllumination.h>
#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android/binder_manager.h>
#include <fingerprint.sysprop.h>
#include <log/log.h>

#include <cstdint>

namespace aidl::android::hardware::biometrics::fingerprint {
namespace {

using ::aidl::vendor::nothing::hardware::udfps::BnIlluminationCallback;
using ::aidl::vendor::nothing::hardware::udfps::IIllumination;
namespace FingerprintHalProperties = ::android::fingerprint::nothing::FingerprintHalProperties;

constexpr char kService[] = "vendor.nothing.hardware.udfps.IIllumination/default";
constexpr char kUiStatus[] = "/sys/panel_feature/ui_status";
constexpr char kHbm[] = "/sys/devices/platform/soc/1401a000.dsi0/hbm";

void reportFailure(const std::shared_ptr<ISessionCallback>& callback) {
    if (callback) {
        // A failed illumination attempt does not invalidate the vendor session.
        // HW_UNAVAILABLE makes the framework discard that still-open session.
        auto status = callback->onAcquired(AcquiredInfo::INSUFFICIENT, 0);
        if (!status.isOk()) ALOGW("Failed to report illumination retry");
    }
}

}  // namespace

struct IlluminationState {
    std::mutex mutex;
    std::weak_ptr<ISessionCallback> callback;
    uint64_t generation = 0;
    bool active = false;
    bool wanted = false;
    bool uiContact = false;
    bool aodContact = false;
    bool vendorContact = false;
    // Unlike an actual touch, SystemUI's AOD hint can arrive after the finger
    // lifted. Remember that Goodix supplies physical contacts across operations.
    bool vendorContactObserved = false;
    bool suppressVendorUntilUp = false;
    bool retryAfterLift = false;
    bool closed = false;
    bool geometryValid = false;
    int x = 0;
    int y = 0;
    int radius = 0;

    // Only the current owner may reset the panel or report an error. In particular,
    // a delayed onFailure from an older attempt cannot disable a newer capture.
    std::shared_ptr<ISessionCallback> fail(uint64_t attempt) {
        std::lock_guard lock(mutex);
        if (generation != attempt || !active || !wanted || closed) return nullptr;
        wanted = false;
        // Keep authentication/enrollment active, but do not repeatedly restart
        // illumination for duplicate touch/vendor events from this contact.
        retryAfterLift = true;
        IlluminationController::forceOff();
        return callback.lock();
    }
};

class IlluminationClient final : public BnIlluminationCallback {
public:
    IlluminationClient(const std::shared_ptr<IlluminationState>& state, uint64_t generation)
        : mState(state), mGeneration(generation) {
        mDeathRecipient = AIBinder_DeathRecipient_new([](void* cookie) {
            auto* owner = static_cast<std::weak_ptr<IlluminationClient>*>(cookie);
            if (auto client = owner->lock()) {
                ALOGE("UDFPS illumination service died");
                reportFailure(client->fail());
            }
        });
        AIBinder_DeathRecipient_setOnUnlinked(mDeathRecipient, [](void* cookie) {
            delete static_cast<std::weak_ptr<IlluminationClient>*>(cookie);
        });
    }

    ~IlluminationClient() override {
        AIBinder_DeathRecipient_delete(mDeathRecipient);
    }

    uint64_t generation() const { return mGeneration; }

    std::shared_ptr<ISessionCallback> begin(int x, int y, int radius) {
        mService = IIllumination::fromBinder(ndk::SpAIBinder(AServiceManager_checkService(kService)));
        if (!mService) {
            ALOGE("UDFPS illumination service is unavailable");
            return fail();
        }
        auto* cookie = new std::weak_ptr<IlluminationClient>(ref<IlluminationClient>());
        if (AIBinder_linkToDeath(mService->asBinder().get(), mDeathRecipient, cookie) != STATUS_OK) {
            // The onUnlinked callback owns cookie even when linkToDeath fails.
            ALOGE("Cannot watch UDFPS illumination service");
            return fail();
        }
        auto status = mService->begin(ref<IlluminationClient>(), x, y, radius);
        if (!status.isOk()) {
            ALOGE("Cannot begin UDFPS illumination: %s", status.getDescription().c_str());
            return fail();
        }
        return nullptr;
    }

    void end() {
        if (!mService) return;
        auto status = mService->end(ref<IlluminationClient>());
        if (!status.isOk()) {
            ALOGE("Cannot end UDFPS illumination: %s", status.getDescription().c_str());
            IlluminationController::forceOff();
        }
    }

    ndk::ScopedAStatus onFailure() override {
        reportFailure(fail());
        return ndk::ScopedAStatus::ok();
    }

private:
    std::shared_ptr<ISessionCallback> fail() {
        if (auto state = mState.lock()) return state->fail(mGeneration);
        return nullptr;
    }

    std::weak_ptr<IlluminationState> mState;
    const uint64_t mGeneration;
    std::shared_ptr<IIllumination> mService;
    AIBinder_DeathRecipient* mDeathRecipient;
};

IlluminationController::IlluminationController(const std::shared_ptr<ISessionCallback>& callback)
    : mState(std::make_shared<IlluminationState>()) {
    mState->callback = callback;
    const auto location = ::android::base::Split(
            FingerprintHalProperties::sensor_location().value_or(""), "|");
    mState->geometryValid = location.size() >= 3 && location.size() <= 4 &&
            ::android::base::ParseInt(location[0], &mState->x) &&
            ::android::base::ParseInt(location[1], &mState->y) &&
            ::android::base::ParseInt(location[2], &mState->radius) &&
            mState->radius > 0 && mState->x >= mState->radius && mState->y >= mState->radius;
}

IlluminationController::~IlluminationController() {
    close();
}

void IlluminationController::forceOff() {
    // The graphics service owns normal writes. These are only the failure/reset path.
    if (!::android::base::WriteStringToFile("0", kUiStatus)) {
        ALOGE("Cannot reset UDFPS UI readiness");
    }
    if (!::android::base::WriteStringToFile("0", kHbm)) {
        ALOGE("Cannot reset UDFPS HBM");
    }
}

void IlluminationController::startOperation() {
    {
        std::lock_guard lock(mState->mutex);
        if (mState->closed) return;
        ++mState->generation;
        mState->active = true;
        mState->wanted = false;
        mState->uiContact = false;
        mState->aodContact = false;
        mState->vendorContact = false;
        mState->suppressVendorUntilUp = false;
        mState->retryAfterLift = false;
    }
    reconcile();
}

void IlluminationController::finishOperation() {
    {
        std::lock_guard lock(mState->mutex);
        ++mState->generation;
        mState->active = false;
        mState->wanted = false;
        mState->uiContact = false;
        mState->aodContact = false;
        mState->vendorContact = false;
        mState->suppressVendorUntilUp = false;
        mState->retryAfterLift = false;
    }
    reconcile();
}

void IlluminationController::pointerDown(bool isSyntheticAod) {
    updateContact(isSyntheticAod ? ContactSource::Aod : ContactSource::Ui, true);
}

void IlluminationController::pointerUp() {
    updateContact(ContactSource::Ui, false);
}

void IlluminationController::vendorPointerDown() {
    updateContact(ContactSource::Vendor, true);
}

void IlluminationController::vendorPointerUp() {
    updateContact(ContactSource::Vendor, false);
}

void IlluminationController::updateContact(ContactSource source, bool down) {
    {
        std::lock_guard lock(mState->mutex);
        if (mState->closed || (down && !mState->active)) return;
        const bool vendor = source == ContactSource::Vendor;
        if (vendor && mState->suppressVendorUntilUp) {
            if (down) return;
            mState->suppressVendorUntilUp = false;
        }
        if (vendor) {
            mState->vendorContactObserved = true;
            mState->vendorContact = down;
            if (!down) {
                mState->aodContact = false;
            }
        } else if (source == ContactSource::Aod) {
            mState->aodContact = down;
        } else {
            mState->uiContact = down;
            // SystemUI also uses this release for AOD timeout and overlay hide.
            if (!down) mState->aodContact = false;
        }
        // Permit the first AOD hint before Goodix has reported any contact.
        // Afterwards a delayed hint cannot resurrect a released finger, or keep
        // illumination on after its physical lift. A fresh vendor down still
        // starts the next scan, including when its AOD hint arrived first.
        const bool touching = mState->uiContact || mState->vendorContact ||
                (mState->aodContact && !mState->vendorContactObserved &&
                        !mState->suppressVendorUntilUp);
        if (!touching) mState->retryAfterLift = false;
        const bool wanted = mState->active && touching && !mState->retryAfterLift;
        // Real UI and vendor contacts may overlap or arrive late. A vendor lift
        // must not end a newer physical UI touch.
        if (mState->wanted != wanted) {
            ++mState->generation;
            mState->wanted = wanted;
        }
    }
    reconcile();
}

void IlluminationController::pointerCancel() {
    {
        std::lock_guard lock(mState->mutex);
        if (mState->closed) return;
        ++mState->generation;
        mState->wanted = false;
        mState->uiContact = false;
        mState->aodContact = false;
        mState->vendorContact = false;
        mState->retryAfterLift = false;
        // A cancelled UI gesture must stop immediately. Ignore the vendor copy
        // of that contact if it arrives afterwards; a fresh UI gesture still works.
        mState->suppressVendorUntilUp = true;
    }
    reconcile();
}

void IlluminationController::close() {
    {
        std::lock_guard lock(mState->mutex);
        ++mState->generation;
        mState->active = false;
        mState->wanted = false;
        mState->uiContact = false;
        mState->aodContact = false;
        mState->vendorContact = false;
        mState->suppressVendorUntilUp = false;
        mState->retryAfterLift = false;
        mState->closed = true;
    }
    reconcile();
}

void IlluminationController::reconcile() {
    std::shared_ptr<ISessionCallback> failure;
    {
        // Only outgoing begin/end calls use this lock. onFailure and binder death
        // take the separate state lock and never make calls to the graphics service.
        std::lock_guard requestLock(mRequestMutex);
        uint64_t generation;
        bool wanted;
        {
            std::lock_guard lock(mState->mutex);
            generation = mState->generation;
            wanted = mState->active && mState->wanted && !mState->closed;
        }
        if (mClient && (!wanted || mClient->generation() != generation)) {
            mClient->end();
            mClient.reset();
        }
        {
            std::lock_guard lock(mState->mutex);
            generation = mState->generation;
            wanted = mState->active && mState->wanted && !mState->closed;
        }
        if (wanted && !mClient) {
            if (!mState->geometryValid) {
                ALOGE("Invalid UDFPS sensor geometry");
                failure = mState->fail(generation);
            } else {
                mClient = ndk::SharedRefBase::make<IlluminationClient>(mState, generation);
                failure = mClient->begin(mState->x, mState->y, mState->radius);
            }
        }
    }
    // Framework callbacks may trigger cancellation. Never issue one under either lock.
    reportFailure(failure);
}

}  // namespace aidl::android::hardware::biometrics::fingerprint
