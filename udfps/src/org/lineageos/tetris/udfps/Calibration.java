/* SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0 */
package org.lineageos.tetris.udfps;

/** Frozen per-read compensation, derived from the stock Tetris transmission curve. */
final class Calibration {
    private final int[] mTransmission;
    private final int mScale;
    private final float mNormalRatio;
    private final float mMinimum;

    Calibration(int[] transmission, int scale, int normalMax, int hbmMax, int minimum) {
        if (transmission.length != 256 || scale <= 0 || hbmMax <= 0 || normalMax <= 0
                || normalMax > hbmMax || minimum < 0 || minimum > normalMax) {
            throw new IllegalArgumentException("Invalid panel calibration");
        }
        for (int sample : transmission) {
            if (sample < 0 || sample > scale) {
                throw new IllegalArgumentException("Invalid transmission sample");
            }
        }
        mTransmission = transmission.clone();
        mScale = scale;
        mNormalRatio = normalMax / (float) hbmMax;
        mMinimum = minimum / (float) hbmMax;
    }

    float alpha(float brightness) {
        if (!Float.isFinite(brightness) || brightness < 0 || brightness > 1) {
            throw new IllegalArgumentException("Display brightness unavailable");
        }
        int legacy = Math.round(1f + 254f * Math.max(brightness, mMinimum));
        int index = Math.min(255, Math.max(0, (int) (legacy / mNormalRatio)));
        return 1f - mTransmission[index] / (float) mScale;
    }
}
