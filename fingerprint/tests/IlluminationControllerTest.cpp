/* SPDX-License-Identifier: Apache-2.0 */
#include "IlluminationController.h"

#include <cassert>
#include <iostream>

using namespace aidl::android::hardware::biometrics::fingerprint;
using namespace aidl::vendor::nothing::hardware::udfps;

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
    std::weak_ptr<BnIlluminationCallback> client;
    ndk::ScopedAStatus begin(const std::shared_ptr<BnIlluminationCallback>& token,
                            int x, int y, int radius) override {
        assert(x == 540 && y == 2109 && radius == 93);
        ++begins;
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

int main() {
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
    {  // Rapid taps from the diagnostic: old Goodix up follows a newer UI down.
        Fixture f;
        f.vendorDown();
        f.down();
        auto original = f.token();
        f.up();
        f.down();
        // Acquisition quality / a negative auth result do not call a lift API.
        f.vendorUp();
        f.vendorDown();
        assert(f.service->begins == 1 && f.service->ends == 0);
        assert(f.token() == original);
        f.up();
        assert(f.service->ends == 0);
        f.vendorUp();
        assert(f.service->ends == 1);
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
                 "service restart, UI/vendor overlap, late AOD hints, cancellation and close\n";
}
