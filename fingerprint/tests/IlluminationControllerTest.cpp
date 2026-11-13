/* SPDX-License-Identifier: Apache-2.0 */
#include "IlluminationController.h"

#include <cassert>
#include <iostream>

using namespace aidl::android::hardware::biometrics::fingerprint;
using namespace aidl::vendor::nothing::hardware::udfps;
using aidl::android::hardware::biometrics::common::DisplayState;
using aidl::android::hardware::biometrics::common::OperationContext;
using aidl::android::hardware::biometrics::common::OperationState;

struct Callback final : ISessionCallback {
    int acquisitions = 0;
    int errors = 0;
    std::function<void()> onRetry;
    ndk::ScopedAStatus onAcquired(AcquiredInfo info, int vendorCode) override {
        assert(info == AcquiredInfo::INSUFFICIENT && vendorCode == 0);
        ++acquisitions;
        if (onRetry) onRetry();
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onError(Error, int) override {
        ++errors;
        return ndk::ScopedAStatus::ok();
    }
};
struct Service final : IIllumination {
    int begins = 0;
    int ends = 0;
    bool beginFails = false;
    bool endFails = false;
    int32_t version = 2;
    bool versionFails = false;
    int legacyBegins = 0;
    int contextualBegins = 0;
    std::vector<std::optional<bool>> ambientRequests;
    std::weak_ptr<BnIlluminationCallback> client;
    ndk::ScopedAStatus begin(const std::shared_ptr<BnIlluminationCallback>& token,
                            int x, int y, int radius) override {
        ++legacyBegins;
        return beginCapture(token, x, y, radius, std::nullopt);
    }
    ndk::ScopedAStatus beginWithContext(const std::shared_ptr<BnIlluminationCallback>& token,
                                       int x, int y, int radius, bool ambient) override {
        ++contextualBegins;
        return beginCapture(token, x, y, radius, ambient);
    }
    ndk::ScopedAStatus getInterfaceVersion(int32_t* result) override {
        *result = version;
        return ndk::ScopedAStatus(!versionFails);
    }
    ndk::ScopedAStatus beginCapture(const std::shared_ptr<BnIlluminationCallback>& token,
                                   int x, int y, int radius, std::optional<bool> ambient) {
        assert(x == 540 && y == 2109 && radius == 93);
        ++begins;
        ambientRequests.push_back(ambient);
        client = token;
        return ndk::ScopedAStatus(!beginFails);
    }
    ndk::ScopedAStatus end(const std::shared_ptr<BnIlluminationCallback>&) override {
        ++ends;
        return ndk::ScopedAStatus(!endFails);
    }
};
struct Fixture {
    std::shared_ptr<Callback> callback = std::make_shared<Callback>();
    std::shared_ptr<Service> service = std::make_shared<Service>();
    IlluminationController controller{callback};
    Fixture() {
        host::service = service;
        host::writes.clear();
        host::linkFails = false;
        controller.startOperation();
    }
    void down() { controller.pointerDown(); }
    void aodDown() { controller.pointerDown(true); }
    void up() { controller.pointerUp(); }
    void vendorDown() { controller.vendorPointerDown(); }
    void vendorUp() { controller.vendorPointerUp(); }
    std::shared_ptr<BnIlluminationCallback> token() {
        auto token = service->client.lock();
        assert(token);
        return token;
    }
    ~Fixture() {
        controller.close();
        assert(callback->errors == 0);
        host::service.reset();
    }
};

static void assertReset() {
    assert(host::writes.size() == 2);
    assert(host::writes[0] == std::make_pair(std::string("/sys/panel_feature/ui_status"),
                                           std::string("0")));
    assert(host::writes[1] == std::make_pair(
            std::string("/sys/devices/platform/soc/1401a000.dsi0/hbm"), std::string("0")));
}

static OperationContext context(DisplayState display, bool ignoreTouches = false) {
    OperationContext result;
    result.displayState = display;
    result.operationState = OperationState::make<OperationState::Tag::fingerprintOperationState>(
            OperationState::FingerprintOperationState{ignoreTouches});
    return result;
}

static void testOperationContext() {
    for (const auto display : {DisplayState::LOCKSCREEN, DisplayState::SCREENSAVER}) {
        // The framework restriction suppresses raw Goodix input, but an actual
        // pointer delivered by SystemUI remains authorization for a new capture.
        Fixture f;
        auto restricted = context(display, true);
        f.controller.startOperation(&restricted);
        f.vendorDown();
        assert(f.service->begins == 0);
        f.down();
        assert(f.service->begins == 1 && f.service->contextualBegins == 1);
        assert(f.service->ambientRequests.back() == false);
        f.up();
        assert(f.service->ends == 1);
        f.vendorDown();
        assert(f.service->begins == 1);
        f.vendorUp();
    }
    {  // A newly restricted UI revokes a vendor-only scan. Becoming permissive
       // must not resurrect the held finger or honor a stale failure callback.
        Fixture f;
        auto allowed = context(DisplayState::LOCKSCREEN);
        auto restricted = context(DisplayState::LOCKSCREEN, true);
        f.controller.startOperation(&allowed);
        f.vendorDown();
        auto stale = f.token();
        f.controller.updateContext(restricted);
        assert(f.service->begins == 1 && f.service->ends == 1);
        host::writes.clear();
        stale->onFailure();
        assert(host::writes.empty() && f.callback->acquisitions == 0);
        f.controller.updateContext(allowed);
        f.vendorDown();
        assert(f.service->begins == 1);
        f.vendorUp();
        f.vendorDown();
        assert(f.service->begins == 2);
        stale->onFailure();
        assert(host::writes.empty() && f.callback->acquisitions == 0);
    }
    for (const auto display : {DisplayState::NO_UI, DisplayState::AOD}) {
        // Screen-off context preserves the physical wake path even while the
        // UI's touch overlay is unavailable and reports ignoreTouches=true.
        Fixture f;
        auto ambient = context(display, true);
        f.controller.startOperation(&ambient);
        f.vendorDown();
        assert(f.service->begins == 1 && f.service->ambientRequests.back() == true);
        auto original = f.token();
        auto waking = context(DisplayState::LOCKSCREEN, true);
        f.controller.updateContext(waking);
        f.controller.updateContext(waking);
        assert(f.service->begins == 1 && f.service->ends == 0 && f.token() == original);
        f.vendorUp();
        assert(f.service->ends == 1);
        f.vendorDown();  // A new raw lockscreen contact is now restricted.
        assert(f.service->begins == 1);
    }
    {  // Older callers may supply the deprecated isAod field alone.
        Fixture f;
        auto ambient = context(DisplayState::UNKNOWN, true);
        ambient.isAod = true;
        f.controller.startOperation(&ambient);
        f.vendorDown();
        assert(f.service->begins == 1 && f.service->ambientRequests.back() == true);
    }
    for (bool synthetic : {false, true}) {
        // Both a real dozing MotionEvent and the synthetic pulse communicate
        // ambient origin; neither must be classified solely from screen state.
        Fixture f;
        f.controller.pointerDown(synthetic, true);
        assert(f.service->begins == 1 && f.service->ambientRequests.back() == true);
        f.up();
        assert(f.service->ends == 1);
    }
    {  // An accepted synthetic pulse itself implies ambient origin.
        Fixture f;
        f.controller.pointerDown(true, false);
        assert(f.service->begins == 1 && f.service->ambientRequests.back() == true);
    }
    {  // A rejected late AOD hint must not bypass a new lockscreen restriction.
        Fixture f;
        f.vendorDown();
        f.vendorUp();
        f.aodDown();
        assert(f.service->begins == 1 && f.service->ends == 1);
        f.controller.updateContext(context(DisplayState::LOCKSCREEN, true));
        f.vendorDown();
        assert(f.service->begins == 1 && f.service->ends == 1);
    }
    {  // Legacy operations clear previous context; updates while idle cannot
       // preconfigure an operation that has not been started yet.
        Fixture f;
        auto restricted = context(DisplayState::LOCKSCREEN, true);
        f.controller.startOperation(&restricted);
        f.vendorDown();
        assert(f.service->begins == 0);
        f.controller.finishOperation();
        f.controller.updateContext(restricted);
        f.controller.startOperation();
        f.vendorDown();
        assert(f.service->begins == 1 && f.service->ambientRequests.back() == false);
        f.controller.finishOperation();
        f.controller.updateContext(context(DisplayState::AOD, true));
        f.controller.startOperation();
        f.vendorDown();
        assert(f.service->begins == 2 && f.service->ambientRequests.back() == false);
    }
    for (int missingPolicy = 0; missingPolicy < 3; ++missingPolicy) {
        // Context hints may be absent, unknown or unrelated to fingerprint.
        // None is authority to disable the physical sensor.
        Fixture f;
        auto permissive = context(DisplayState::LOCKSCREEN, true);
        if (missingPolicy == 0) permissive.operationState.reset();
        if (missingPolicy == 1) permissive.displayState = DisplayState::UNKNOWN;
        if (missingPolicy == 2) {
            permissive.operationState = OperationState::make<OperationState::Tag::faceOperationState>(
                    OperationState::FaceOperationState{});
        }
        f.controller.startOperation(&permissive);
        f.vendorDown();
        auto original = f.token();
        f.controller.updateContext(permissive);
        f.controller.updateContext(permissive);
        assert(f.service->begins == 1 && f.service->ends == 0 && f.token() == original);
    }
    {  // A SystemUI pointer already in flight stays valid under touch policy
       // updates; once it lifts, its remaining vendor copy is revoked.
        Fixture f;
        f.down();
        f.vendorDown();
        auto original = f.token();
        f.controller.updateContext(context(DisplayState::LOCKSCREEN, true));
        assert(f.service->ends == 0 && f.token() == original);
        f.up();
        assert(f.service->ends == 1);
        f.vendorDown();
        assert(f.service->begins == 1);
        f.vendorUp();
    }
    {  // An older renderer remains usable through the frozen V1 entry point.
        Fixture f;
        f.service->version = 1;
        f.aodDown();
        assert(f.service->begins == 1 && f.service->legacyBegins == 1);
        assert(f.service->contextualBegins == 0 && !f.service->ambientRequests.back());
        f.up();
        assert(f.service->ends == 1);
    }
    {  // Failed interface negotiation must not start an unverified transaction.
       // The current contact fails safely and a physical retry can recover.
        Fixture f;
        f.service->versionFails = true;
        f.down();
        assert(f.service->begins == 0 && f.callback->acquisitions == 1);
        assertReset();
        f.service->versionFails = false;
        f.down();
        assert(f.service->begins == 0 && f.callback->acquisitions == 1);
        f.up();
        f.down();
        assert(f.service->begins == 1 && f.service->contextualBegins == 1);
    }
}

int main() {
    testOperationContext();
    for (bool vendorFirst : {false, true}) {
        // AOD's synthetic hint may precede the first Goodix event. Once Goodix
        // reports the physical lift, its delayed SystemUI hint cannot hold HBM.
        Fixture f;
        if (vendorFirst) f.vendorDown(); else f.aodDown();
        auto original = f.token();
        if (vendorFirst) f.aodDown(); else f.vendorDown();
        f.aodDown();
        assert(f.service->begins == 1 && f.token() == original);
        f.vendorUp();
        assert(f.service->ends == 1);
        f.aodDown();  // The proximity check/pulse can finish after physical lift.
        assert(f.service->begins == 1 && f.service->ends == 1);
        f.up();
        assert(f.service->ends == 1);
        f.aodDown();  // Next pulse arrives ahead of the next Goodix down.
        assert(f.service->begins == 1);
        f.vendorDown();
        assert(f.service->begins == 2);
        f.up();  // Synthetic timeout does not release a still-held physical finger.
        assert(f.service->ends == 1);
        f.vendorUp();
        assert(f.service->ends == 2);
    }
    {  // A fully released vendor-only attempt must not be restarted by its late hint.
        Fixture f;
        f.vendorDown();
        auto stale = f.token();
        f.vendorUp();
        f.aodDown();
        f.aodDown();
        assert(f.service->begins == 1 && f.service->ends == 1);
        f.down();  // A real UI gesture still starts immediately, even in Doze.
        assert(f.service->begins == 2);
        f.vendorUp();  // A delayed vendor lift must not release this real UI touch.
        assert(f.service->ends == 1);
        host::writes.clear();
        stale->onFailure();
        assert(host::writes.empty() && f.callback->acquisitions == 0);
        f.up();
        assert(f.service->ends == 2);
    }
    {  // Operation changes cannot make an already-known vendor lift ambiguous again.
        Fixture f;
        f.vendorDown();
        f.aodDown();
        f.controller.finishOperation();
        f.vendorUp();
        f.controller.startOperation();
        f.aodDown();
        assert(f.service->begins == 1 && f.service->ends == 1);
        f.vendorDown();
        assert(f.service->begins == 2);
        f.vendorUp();
        assert(f.service->ends == 2);
        f.controller.finishOperation();
        f.aodDown();
        assert(f.service->begins == 2);
        f.controller.startOperation();
        f.down();
        assert(f.service->begins == 3);
        f.controller.close();
        f.aodDown();
        f.vendorDown();
        assert(f.service->begins == 3 && f.service->ends == 3);
    }
    {  // Without any vendor event, first-use AOD still works and generic up releases it.
        Fixture f;
        f.aodDown();
        assert(f.service->begins == 1);
        f.up();
        assert(f.service->ends == 1);
        f.aodDown();
        assert(f.service->begins == 2);
        f.controller.pointerCancel();
        assert(f.service->ends == 2);
        f.aodDown();  // Delayed hint cannot restart a cancelled first-use scan.
        f.vendorDown();
        assert(f.service->begins == 2 && f.service->ends == 2);
        f.vendorUp();
        f.aodDown();
        assert(f.service->begins == 2);
        f.vendorDown();
        assert(f.service->begins == 3);
        f.vendorUp();
        assert(f.service->ends == 3);
    }
    {  // Failed AOD capture cannot restart until Goodix reports a physical lift.
        Fixture f;
        f.aodDown();
        auto failed = f.token();
        f.vendorDown();
        failed->onFailure();
        assert(f.callback->acquisitions == 1);
        f.up();
        f.aodDown();
        f.vendorDown();
        assert(f.service->begins == 1);
        f.vendorUp();
        f.aodDown();
        assert(f.service->begins == 1);
        f.vendorDown();
        assert(f.service->begins == 2);
        host::writes.clear();
        failed->onFailure();
        assert(host::writes.empty() && f.callback->acquisitions == 1);
    }
    for (bool vendorFirst : {false, true}) {
        // UI and Goodix describe one contact: upgrade does not rebuild the surface,
        // and either source can release first without ending the other source.
        Fixture f;
        if (vendorFirst) f.vendorDown(); else f.down();
        auto original = f.token();
        if (vendorFirst) f.down(); else f.vendorDown();
        f.down();
        f.vendorDown();
        assert(f.service->begins == 1 && f.token() == original);
        if (vendorFirst) f.vendorUp(); else f.up();
        assert(f.service->ends == 0 && f.token() == original);
        if (vendorFirst) f.up(); else f.vendorUp();
        assert(f.service->ends == 1);
        f.up();
        f.vendorUp();
        assert(f.service->ends == 1);
    }
    for (bool vendorFirst : {false, true}) {
        // A second contact must get a fresh ready edge even if the other source
        // has not delivered the preceding release yet.
        Fixture f;
        f.vendorDown();
        f.down();
        auto original = f.token();
        if (vendorFirst) {
            f.vendorUp();
            f.vendorDown();
        } else {
            f.up();
            f.down();
        }
        assert(f.service->begins == 2 && f.service->ends == 1);
        auto current = f.token();
        assert(current != original);
        // Delayed release/down from the other source belongs to this new
        // contact. It must neither terminate nor restart the new illumination.
        if (vendorFirst) {
            f.up();
            f.down();
        } else {
            f.vendorUp();
            f.vendorDown();
        }
        f.down();
        f.vendorDown();
        assert(f.service->begins == 2 && f.service->ends == 1);
        assert(f.token() == current);
        host::writes.clear();
        original->onFailure();
        assert(host::writes.empty() && f.callback->acquisitions == 0);
        f.up();
        assert(f.service->ends == 1);
        f.vendorUp();
        assert(f.service->ends == 2);
    }
    {  // Repeated UI contacts cannot all merge into a delayed vendor contact.
        Fixture f;
        f.vendorDown();
        f.down();
        for (int attempt = 2; attempt <= 4; ++attempt) {
            f.up();
            f.down();
            assert(f.service->begins == attempt && f.service->ends == attempt - 1);
            f.vendorDown();  // A duplicate is not evidence of a new vendor contact.
            assert(f.service->begins == attempt);
        }
        f.vendorUp();
        f.vendorDown();
        assert(f.service->begins == 4 && f.service->ends == 3);
        f.up();
        f.vendorUp();
        assert(f.service->ends == 4);
    }
    {  // A graphics failure stays latched until both sources have reported lift.
        Fixture f;
        f.down();
        f.vendorDown();
        auto failed = f.token();
        failed->onFailure();
        assert(f.callback->acquisitions == 1);
        f.up();
        f.down();
        f.vendorUp();
        f.vendorDown();
        assert(f.service->begins == 1 && f.callback->acquisitions == 1);
        f.up();
        f.vendorUp();
        f.vendorDown();
        assert(f.service->begins == 2);
        host::writes.clear();
        failed->onFailure();
        assert(host::writes.empty() && f.callback->acquisitions == 1);
    }
    {  // Cancellation rejects delayed vendor down until its matching release.
        Fixture f;
        f.down();
        auto stale = f.token();
        f.controller.pointerCancel();
        assert(f.service->ends == 1);
        f.vendorDown();
        assert(f.service->begins == 1);
        f.down();  // A genuinely new UI gesture remains available immediately.
        assert(f.service->begins == 2);
        host::writes.clear();
        stale->onFailure();
        assert(host::writes.empty());
        f.vendorUp();
        assert(f.service->ends == 1);
        f.up();
        assert(f.service->ends == 2);
        f.vendorDown();  // Screen-off/vendor-only attempts work after that release.
        assert(f.service->begins == 3);
        f.controller.finishOperation();
        assert(f.service->ends == 3);
        f.vendorDown();
        f.down();
        assert(f.service->begins == 3);
        f.controller.startOperation();
        f.vendorDown();
        assert(f.service->begins == 4);
        f.controller.close();
        f.vendorUp();
        f.vendorDown();
        assert(f.service->ends == 4 && f.service->begins == 4);
    }
    {  // A bad graphic frame is retryable and duplicate down/failure events are inert.
        Fixture f;
        f.down();
        auto failed = f.token();
        failed->onFailure();
        assertReset();
        assert(f.callback->acquisitions == 1);
        f.down();
        failed->onFailure();
        assert(f.service->begins == 1 && f.callback->acquisitions == 1);
        assertReset();
        f.up();
        f.down();
        assert(f.service->begins == 2);  // No new authenticate/enroll is required.
        host::writes.clear();
        failed->onFailure();
        assert(host::writes.empty() && f.callback->acquisitions == 1);
    }
    {  // Missing graphics process and failed death-link recover after finger lift.
        Fixture f;
        host::service.reset();
        f.down();
        assertReset();
        assert(f.callback->acquisitions == 1);
        host::service = f.service;
        f.down();
        assert(f.service->begins == 0);
        f.up();
        host::linkFails = true;
        f.down();
        assert(f.callback->acquisitions == 2 && f.service->begins == 0);
        host::linkFails = false;
        f.up();
        f.down();
        assert(f.service->begins == 1);
    }
    {  // Binder transaction failure keeps the vendor operation usable.
        Fixture f;
        f.service->beginFails = true;
        f.down();
        assert(f.callback->acquisitions == 1);
        f.service->beginFails = false;
        f.down();
        assert(f.service->begins == 1);
        f.up();
        f.down();
        assert(f.service->begins == 2);
    }
    {  // Renderer death can recover with its replacement on the next contact.
        Fixture f;
        f.down();
        auto oldService = f.service;
        host::die(oldService->asBinder().get());
        assertReset();
        assert(f.callback->acquisitions == 1);
        f.service = std::make_shared<Service>();
        host::service = f.service;
        f.down();
        assert(f.service->begins == 0);
        f.up();
        f.down();
        assert(f.service->begins == 1);
    }
    {  // Cancel/new operation invalidates failures, and resets the retry latch.
        Fixture f;
        f.down();
        auto stale = f.token();
        stale->onFailure();
        f.controller.finishOperation();
        f.down();
        assert(f.service->begins == 1);
        f.controller.startOperation();
        f.down();
        assert(f.service->begins == 2);
        host::writes.clear();
        stale->onFailure();
        assert(host::writes.empty() && f.callback->acquisitions == 1);
        auto current = f.token();
        f.controller.close();
        current->onFailure();
        f.controller.startOperation();
        f.down();
        assert(host::writes.empty() && f.service->begins == 2);
    }
    {  // Framework cancellation may reenter; neither controller mutex may be held.
        Fixture f;
        f.callback->onRetry = [&] { f.controller.finishOperation(); };
        f.service->beginFails = true;
        f.down();
        assert(f.callback->acquisitions == 1 && f.service->ends == 1);
        f.service->beginFails = false;
        f.controller.startOperation();
        f.down();
        f.token()->onFailure();
        assert(f.callback->acquisitions == 2 && f.service->ends == 2);
    }
    {  // Invalid calibration geometry also fails without poisoning the HAL session.
        host::location = "not|valid";
        Fixture f;
        f.down();
        assert(f.callback->acquisitions == 1 && f.service->begins == 0);
        f.down();
        assert(f.callback->acquisitions == 1);
        f.up();
        f.down();
        assert(f.callback->acquisitions == 2);
        host::location = "540|2109|93";
    }
    assert(host::links.empty());
    std::cout << "PASS: recoverable illumination failures, contact retry, stale tokens, "
                 "service restart, UI/vendor overlap, late AOD hints, cancellation, close, "
                 "operation context, touch policy and renderer version compatibility\n";
}
