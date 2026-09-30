/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

// Minimal host implementations of Android transport and sysfs dependencies.
// The tests compile the actual IlluminationController.cpp against these seams.
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#define ALOGE(...) ((void)0)
#define ALOGW(...) ((void)0)
#define ALOGI(...) ((void)0)
constexpr int STATUS_OK = 0;
struct AIBinder {};
struct AIBinder_DeathRecipient {
    void (*died)(void*);
    void (*unlinked)(void*) = nullptr;
};
namespace host {
struct DeathLink {
    AIBinder* binder;
    AIBinder_DeathRecipient* recipient;
    void* cookie;
};
inline std::vector<DeathLink> links;
inline bool linkFails = false;
inline std::string location = "540|2109|93";
inline std::vector<std::pair<std::string, std::string>> writes;
inline void die(AIBinder* binder) {
    const auto snapshot = links;
    for (const auto& link : snapshot) {
        if (link.binder == binder) link.recipient->died(link.cookie);
    }
}
}  // namespace host
inline auto AIBinder_DeathRecipient_new(void (*died)(void*)) {
    return new AIBinder_DeathRecipient{died};
}
inline void AIBinder_DeathRecipient_setOnUnlinked(
        AIBinder_DeathRecipient* recipient, void (*callback)(void*)) {
    recipient->unlinked = callback;
}
inline void AIBinder_DeathRecipient_delete(AIBinder_DeathRecipient* recipient) {
    auto it = host::links.begin();
    while (it != host::links.end()) {
        if (it->recipient == recipient) {
            recipient->unlinked(it->cookie);
            it = host::links.erase(it);
        } else {
            ++it;
        }
    }
    delete recipient;
}
inline int AIBinder_linkToDeath(
        AIBinder* binder, AIBinder_DeathRecipient* recipient, void* cookie) {
    if (host::linkFails) {
        recipient->unlinked(cookie);
        return -1;
    }
    host::links.push_back({binder, recipient, cookie});
    return STATUS_OK;
}
namespace ndk {
class ScopedAStatus {
public:
    explicit ScopedAStatus(bool success = true) : mSuccess(success) {}
    static ScopedAStatus ok() { return ScopedAStatus(); }
    bool isOk() const { return mSuccess; }
    std::string getDescription() const { return mSuccess ? "OK" : "transport failure"; }
private:
    bool mSuccess;
};
class SpAIBinder {
public:
    explicit SpAIBinder(AIBinder* binder = nullptr) : mBinder(binder) {}
    AIBinder* get() const { return mBinder; }
private:
    AIBinder* mBinder;
};
class SharedRefBase : public std::enable_shared_from_this<SharedRefBase> {
public:
    virtual ~SharedRefBase() = default;
    template <class T, class... Args>
    static auto make(Args&&... args) {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
    template <class T> auto ref() { return std::dynamic_pointer_cast<T>(shared_from_this()); }
};
}  // namespace ndk
namespace aidl::android::hardware::biometrics::fingerprint {
enum class AcquiredInfo { INSUFFICIENT };
enum class Error { HW_UNAVAILABLE };
class ISessionCallback {
public:
    virtual ~ISessionCallback() = default;
    virtual ndk::ScopedAStatus onAcquired(AcquiredInfo, int) = 0;
    virtual ndk::ScopedAStatus onError(Error, int) = 0;
};
}  // namespace aidl::android::hardware::biometrics::fingerprint
namespace aidl::vendor::nothing::hardware::udfps {
class BnIlluminationCallback : public ndk::SharedRefBase {
public:
    virtual ndk::ScopedAStatus onFailure() = 0;
};
class IIllumination {
public:
    virtual ~IIllumination() = default;
    virtual ndk::ScopedAStatus begin(
            const std::shared_ptr<BnIlluminationCallback>&, int, int, int) = 0;
    virtual ndk::ScopedAStatus end(const std::shared_ptr<BnIlluminationCallback>&) = 0;
    ndk::SpAIBinder asBinder() { return ndk::SpAIBinder(&mBinder); }
    static std::shared_ptr<IIllumination> fromBinder(ndk::SpAIBinder binder);
private:
    AIBinder mBinder;
};
}  // namespace aidl::vendor::nothing::hardware::udfps
namespace host {
inline std::shared_ptr<aidl::vendor::nothing::hardware::udfps::IIllumination> service;
}
inline auto AServiceManager_checkService(const char*) {
    return host::service ? host::service->asBinder().get() : nullptr;
}
inline std::shared_ptr<aidl::vendor::nothing::hardware::udfps::IIllumination>
aidl::vendor::nothing::hardware::udfps::IIllumination::fromBinder(ndk::SpAIBinder binder) {
    return binder.get() ? host::service : nullptr;
}
namespace android::base {
inline bool WriteStringToFile(const std::string& value, const std::string& path) {
    host::writes.emplace_back(path, value);
    return true;
}
inline std::vector<std::string> Split(const std::string& input, const std::string&) {
    std::istringstream stream(input);
    std::vector<std::string> result;
    std::string piece;
    while (std::getline(stream, piece, '|')) result.push_back(piece);
    return result;
}
inline bool ParseInt(const std::string& input, int* result) {
    try {
        size_t consumed;
        *result = std::stoi(input, &consumed);
        return consumed == input.size();
    } catch (...) {
        return false;
    }
}
}  // namespace android::base
namespace android::fingerprint::nothing::FingerprintHalProperties {
inline std::optional<std::string> sensor_location() { return host::location; }
}
