/* SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0 */
package org.lineageos.tetris.udfps;

/** Maps the physical, portrait sensor coordinates into the current logical display. */
final class SensorGeometry {
    static final int NATURAL_WIDTH = 1080;
    static final int NATURAL_HEIGHT = 2400;
    final float x;
    final float y;
    final float radiusX;
    final float radiusY;

    SensorGeometry(int sensorX, int sensorY, int radius, int width, int height, int rotation) {
        if (width <= 0 || height <= 0 || radius <= 0 || radius > 200
                || sensorX < radius || sensorX > NATURAL_WIDTH - radius
                || sensorY < radius || sensorY > NATURAL_HEIGHT - radius
                || rotation < 0 || rotation > 3) {
            throw new IllegalArgumentException("Invalid sensor geometry");
        }
        float rotatedX;
        float rotatedY;
        switch (rotation) {
            case 1: rotatedX = sensorY; rotatedY = NATURAL_WIDTH - sensorX; break;
            case 2: rotatedX = NATURAL_WIDTH - sensorX;
                    rotatedY = NATURAL_HEIGHT - sensorY; break;
            case 3: rotatedX = NATURAL_HEIGHT - sensorY; rotatedY = sensorX; break;
            default: rotatedX = sensorX; rotatedY = sensorY;
        }
        float sx = width / (float) ((rotation & 1) == 0 ? NATURAL_WIDTH : NATURAL_HEIGHT);
        float sy = height / (float) ((rotation & 1) == 0 ? NATURAL_HEIGHT : NATURAL_WIDTH);
        x = rotatedX * sx;
        y = rotatedY * sy;
        radiusX = radius * sx;
        radiusY = radius * sy;
    }
}
