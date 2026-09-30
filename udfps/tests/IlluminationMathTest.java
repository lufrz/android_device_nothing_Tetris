/* SPDX-License-Identifier: Apache-2.0 */
package org.lineageos.tetris.udfps;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/** Golden observations from the Tetris brightness diagnostic, plus geometry/error cases. */
public final class IlluminationMathTest {
    private static void equal(float expected, float actual) {
        if (Math.abs(expected - actual) > 0.000001f) {
            throw new AssertionError("Expected " + expected + ", got " + actual);
        }
    }

    private static void rejects(Runnable action) {
        try {
            action.run();
        } catch (IllegalArgumentException expected) {
            return;
        }
        throw new AssertionError("Invalid input accepted");
    }

    private static void geometry(int width, int height, int rotation, float x, float y, float r) {
        SensorGeometry g = new SensorGeometry(540, 2109, 93, width, height, rotation);
        equal(x, g.x);
        equal(y, g.y);
        equal(r, g.radiusX);
        equal(r, g.radiusY);
    }

    public static void main(String[] args) throws Exception {
        int[] table = Files.lines(Path.of(args[0])).mapToInt(Integer::parseInt).toArray();
        Calibration c = new Calibration(table, 256, 2680, 4095, 4);
        // Captured low/medium/high display targets, evaluated against the audited stock curve.
        equal(0.734375f, c.alpha(0.0517464f));
        equal(0.4296875f, c.alpha(0.29569238f));
        equal(0.19140625f, c.alpha(0.6f));
        equal(0.14453125f, c.alpha(1f));
        equal(c.alpha(4f / 4095f), c.alpha(0f));
        float previous = 1f;
        for (int level = 0; level <= 4095; ++level) {
            float alpha = c.alpha(level / 4095f);
            if (alpha < 0 || alpha > previous) {
                throw new AssertionError("Invalid/non-monotonic compensation at " + level);
            }
            previous = alpha;
        }
        Arrays.fill(table, 0);
        equal(0.19140625f, c.alpha(0.6f)); // Caller cannot mutate the active calibration.
        rejects(() -> c.alpha(Float.NaN));
        rejects(() -> c.alpha(Float.POSITIVE_INFINITY));
        rejects(() -> c.alpha(-0.1f));
        rejects(() -> c.alpha(1.1f));
        rejects(() -> new Calibration(new int[255], 256, 2680, 4095, 4));
        rejects(() -> new Calibration(new int[256], 0, 2680, 4095, 4));
        rejects(() -> new Calibration(new int[256], 256, 4096, 4095, 4));
        rejects(() -> new Calibration(new int[256], 256, 2680, 4095, -1));
        table[0] = 257;
        rejects(() -> new Calibration(table, 256, 2680, 4095, 4));

        // Android RotationUtils physical-to-logical transforms, plus half-resolution rendering.
        geometry(1080, 2400, 0, 540, 2109, 93);
        geometry(2400, 1080, 1, 2109, 540, 93);
        geometry(1080, 2400, 2, 540, 291, 93);
        geometry(2400, 1080, 3, 291, 540, 93);
        geometry(540, 1200, 0, 270, 1054.5f, 46.5f);
        geometry(1200, 540, 1, 1054.5f, 270, 46.5f);
        rejects(() -> new SensorGeometry(540, 2109, 93, 0, 2400, 0));
        rejects(() -> new SensorGeometry(540, 2109, 93, 1080, 2400, 4));
        rejects(() -> new SensorGeometry(10, 10, 93, 1080, 2400, 0));
        rejects(() -> new SensorGeometry(540, 2109, 0, 1080, 2400, 0));
        System.out.println("PASS: brightness fixtures, calibration bounds, rotations and scaling");
    }
}
