// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
extern "C" int vendorDownmix(void*, void*, size_t)
        asm("_ZN7android28AudioALSAPlaybackHandlerBase30doStereoToMonoConversionIfNeedEPvm");
extern "C" int vendorDownmix(void*, void* buffer, size_t bytes) {
    if (bytes) *static_cast<unsigned char*>(buffer) = 0x5a;
    return 37;
}
extern "C" int invokeVendor(void* buffer, size_t bytes) {
    return vendorDownmix(nullptr, buffer, bytes);
}

extern "C" int vendorMixer(void*, unsigned int, void*)
        asm("_ZN7android28AudioALSAPlaybackHandlerBase16wrapSwMixerWriteEPvjS1_");
extern "C" int vendorMixer(void* buffer, unsigned int bytes, void* handler) {
    return vendorDownmix(handler, buffer, bytes);
}
extern "C" int invokeMixer(void* buffer, unsigned int bytes, void* handler) {
    return vendorMixer(buffer, bytes, handler);
}
