/* SPDX-License-Identifier: Apache-2.0 */
package vendor.nothing.hardware.udfps;

import vendor.nothing.hardware.udfps.IIlluminationCallback;

/** Owns the display changes for one optical fingerprint capture. */
@VintfStability
interface IIllumination {
    /**
     * Starts an asynchronous capture preparation using a fresh owner token.
     * Geometry is in the display's natural orientation, in physical pixels.
     * The service owns HBM/UI readiness and must clean up when the owner dies.
     */
    void begin(in IIlluminationCallback client, int x, int y, int radius);

    /** Cancels pending work and restores the display for this owner only. */
    void end(in IIlluminationCallback client);
}
