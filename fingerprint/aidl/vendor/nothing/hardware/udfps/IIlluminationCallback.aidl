/* SPDX-License-Identifier: Apache-2.0 */
package vendor.nothing.hardware.udfps;

/** Binder lifetime is the ownership token for one capture attempt. */
@VintfStability
oneway interface IIlluminationCallback {
    /** The service has restored the display and cannot finish this attempt. */
    void onFailure();
}
