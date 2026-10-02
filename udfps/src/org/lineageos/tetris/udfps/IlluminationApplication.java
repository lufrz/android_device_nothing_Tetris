/* SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0 */
package org.lineageos.tetris.udfps;

import android.app.Application;
import android.content.Intent;
import android.content.res.Resources;
import android.graphics.Point;
import android.hardware.display.BrightnessInfo;
import android.hardware.display.DisplayManager;
import android.os.Binder;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Process;
import android.os.PowerManager;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.os.UserHandle;
import android.util.Log;
import android.view.Display;
import android.view.DisplayInfo;

import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

import vendor.nothing.hardware.udfps.IIllumination;
import vendor.nothing.hardware.udfps.IIlluminationCallback;

/** Device-local owner of the compensation surface, fingerprint HBM and UI-ready signal. */
public final class IlluminationApplication extends Application {
    private static final String TAG = "TetrisUdfps";
    private static final String SERVICE = IIllumination.DESCRIPTOR + "/default";
    private static final String HBM = "/sys/devices/platform/soc/1401a000.dsi0/hbm";
    private static final String UI_READY = "/sys/panel_feature/ui_status";
    private static final String HBM_TIMING = HBM + "_timing";
    private static final String AOD_PULSE_ACTION = "com.android.systemui.doze.pulse";
    private static final String SYSTEM_UI_PACKAGE = "com.android.systemui";
    private static final long MAX_SCAN_MS = 10_000;
    private static final long DISPLAY_READY_TIMEOUT_MS = 2_000;
    private static final long WAKE_LOCK_TIMEOUT_MS = MAX_SCAN_MS + DISPLAY_READY_TIMEOUT_MS;

    private static final long PREPARATION_QUIET_MS = 200;
    private static final long PREPARATION_INTERVAL_MS = 1_000;

    private final Object mLock = new Object();
    private Handler mWorker;
    private Handler mPreparationWorker;
    // Scheduling fields are confined to mWorker. The producer only receives a
    // frozen key/token and publishes immutable pixels through JNI.
    private int mSensorX, mSensorY, mSensorRadius;
    private PreparationKey mDesiredPreparation;
    private PreparationKey mFailedPreparation;
    private boolean mPreparationRunning;
    private long mPreparationToken;
    private long mPreparationStableSince;
    private long mLastPreparationStartedAt = -PREPARATION_INTERVAL_MS;
    private volatile String mPreparationState = "waiting for sensor geometry";
    private final Runnable mPrepareWhenStable = this::prepareWhenStable;
    private DisplayManager mDisplayManager;
    private PowerManager mPowerManager;
    private PowerManager.WakeLock mScanWakeLock;
    private Calibration mCalibration;
    // Binder threads invalidate ownership immediately, including while a present fence is pending.
    private Request mOwner;
    private long mGeneration;
    private volatile String mState = "starting";
    private volatile String mLastFailure = "none";
    private volatile float mBrightness = Float.NaN;
    private volatile float mAlpha = Float.NaN;
    // Keep capture diagnostics as values; format them only when dumpsys is requested.
    private volatile long mLastReadyElapsedMs = -1;
    private volatile boolean mLastHidePresented;
    private volatile boolean mLastForcedHbmOff;
    private volatile boolean mLastCleanupSucceeded;
    private volatile String mLastHbmState = "unknown";
    private volatile String mLastScanContext = "none";
    private volatile String mLastAodPulse = "none";
    private long mScanContextToken;
    private int mWidth;
    private int mHeight;
    private int mRotation;

    @Override
    public void onCreate() {
        super.onCreate();
        if (UserHandle.myUserId() != UserHandle.USER_SYSTEM) return;
        System.loadLibrary("tetris_udfps_surface");
        Resources res = getResources();
        mCalibration = new Calibration(
                res.getIntArray(R.array.config_udfpsMtkGhbmAlphaMap),
                res.getInteger(R.integer.config_udfpsMtkGhbmAlphaMapScale),
                res.getInteger(R.integer.config_udfpsMtkGhbmNormalMaxBacklight),
                res.getInteger(R.integer.config_udfpsMtkGhbmMaxBacklight),
                res.getInteger(R.integer.config_udfpsMtkGhbmMinBacklight));
        mDisplayManager = getSystemService(DisplayManager.class);
        HandlerThread thread = new HandlerThread("TetrisUdfpsIllumination");
        thread.start();
        mWorker = new Handler(thread.getLooper());
        HandlerThread preparationThread = new HandlerThread("TetrisUdfpsPrepare",
                Process.THREAD_PRIORITY_BACKGROUND);
        preparationThread.start();
        mPreparationWorker = new Handler(preparationThread.getLooper());
        // Also recover hardware state after a persistent-process restart.
        mWorker.post(this::clearHardware);
        mDisplayManager.registerDisplayListener(new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int id) {
                if (id == Display.DEFAULT_DISPLAY) updatePreparation();
            }
            @Override public void onDisplayRemoved(int id) {
                if (id == Display.DEFAULT_DISPLAY) {
                    cancelPreparation("display removed");
                    abortCurrent("display removed");
                }
            }
            @Override public void onDisplayChanged(int id) {
                if (id != Display.DEFAULT_DISPLAY) return;
                Request owner;
                synchronized (mLock) { owner = mOwner; }
                if (owner == null) {
                    updatePreparation();
                    return;
                }
                Display display = mDisplayManager.getDisplay(id);
                if (owner.waitingForDisplay) {
                    // SystemUI pulses asynchronously. Never block the worker for this listener.
                    if (display != null && isScanReady(display)) {
                        startRendering(owner);
                    }
                    return;
                }
                // A queued display event may arrive before start() begins waiting for the pulse.
                if (!owner.renderingStarted) return;
                Point size = new Point();
                if (display != null) display.getRealSize(size);
                // Both the requested and completed display power states must stay ON.
                if (display == null || !isScanReady(display)
                        || (mWidth != 0 && (size.x != mWidth || size.y != mHeight
                                || display.getRotation() != mRotation))) {
                    fail(owner, "display is not committed ON or geometry changed");
                }
            }
        }, mWorker, DisplayManager.EVENT_TYPE_DISPLAY_ADDED
                | DisplayManager.EVENT_TYPE_DISPLAY_CHANGED
                | DisplayManager.EVENT_TYPE_DISPLAY_REMOVED
                | DisplayManager.EVENT_TYPE_DISPLAY_STATE
                | DisplayManager.EVENT_TYPE_DISPLAY_REFRESH_RATE
                | DisplayManager.EVENT_TYPE_DISPLAY_BRIGHTNESS,
                DisplayManager.PRIVATE_EVENT_TYPE_DISPLAY_COMMITTED_STATE_CHANGED);
        ServiceManager.addService(SERVICE, mService, false);
        Log.i(TAG, "Device illumination service registered");
    }

    private final IIllumination.Stub mService = new IIllumination.Stub() {
        @Override public int getInterfaceVersion() { return IIllumination.VERSION; }
        @Override public String getInterfaceHash() { return IIllumination.HASH; }

        @Override public void begin(IIlluminationCallback client, int x, int y, int radius) {
            enforceCaller();
            if (client == null) throw new IllegalArgumentException("Missing owner callback");
            // Validate the physical rectangle before queueing work or accepting ownership.
            new SensorGeometry(x, y, radius, 1080, 2400, 0);
            synchronized (mLock) {
                if (mOwner != null && mOwner.token.equals(client.asBinder())) return;
                if (mOwner != null) mOwner.unlink();
                nativeSetGeneration(++mGeneration);
                Request request = new Request(client, x, y, radius, mGeneration);
                try {
                    request.token.linkToDeath(request, 0);
                } catch (RemoteException e) {
                    mOwner = null;
                    mWorker.post(IlluminationApplication.this::clearHardware);
                    return;
                }
                mOwner = request;
                mWorker.post(() -> start(request));
                mWorker.postAtTime(() -> fail(request, "scan timeout"),
                        request.startedAt + MAX_SCAN_MS);
            }
        }

        @Override public void end(IIlluminationCallback client) {
            enforceCaller();
            if (client == null) return;
            synchronized (mLock) {
                if (mOwner == null || !mOwner.token.equals(client.asBinder())) return;
                releaseLocked(mOwner);
                mWorker.post(IlluminationApplication.this::clearHardware);
            }
        }

        @Override protected void dump(FileDescriptor fd, PrintWriter out, String[] args) {
            if (checkCallingOrSelfPermission(android.Manifest.permission.DUMP)
                    != android.content.pm.PackageManager.PERMISSION_GRANTED) return;
            out.println("state=" + mState);
            out.println("brightness=" + mBrightness + " alpha=" + mAlpha);
            synchronized (mLock) {
                out.println("generation=" + mGeneration + " owner=" + (mOwner != null));
            }
            out.println("lastFailure=" + mLastFailure);
            out.println("lastReadyElapsedMs=" + mLastReadyElapsedMs);
            out.println("lastCleanup=hide_presented=" + mLastHidePresented
                    + " forced_hbm_off=" + mLastForcedHbmOff + " success=" + mLastCleanupSucceeded);
            out.println("hbmControl=composer-buffer");
            out.println("displayWake=systemui-doze");
            out.println("aodPulse=" + mLastAodPulse);
            out.println("hbmState=" + mLastHbmState);
            Display display = mDisplayManager.getDisplay(Display.DEFAULT_DISPLAY);
            out.println("display=" + (display == null ? "missing"
                    : Display.stateToString(display.getState())));
            out.println("displayCommitted=" + (display == null ? "missing"
                    : Display.stateToString(display.getCommittedState())));
            synchronized (mLock) {
                out.println("powerManager=" + (mPowerManager != null));
                out.println("scanWakeLock=" + (mScanWakeLock != null && mScanWakeLock.isHeld()));
            }
            out.println("scanContext=" + mLastScanContext);
            out.println("preparation=" + mPreparationState);
            out.println("native=" + nativeGetDiagnostics());
            // Optional read-only driver diagnostics; never part of capture readiness.
            try {
                out.println("hbmTiming:\n" + Files.readString(Path.of(HBM_TIMING)).trim());
            } catch (IOException | SecurityException e) {
                out.println("hbmTiming=unavailable (" + e.getClass().getSimpleName() + ")");
            }
        }
    };

    private static void enforceCaller() {
        // SELinux further limits access to the fingerprint HAL domain.
        if (Binder.getCallingUid() != Process.SYSTEM_UID) {
            throw new SecurityException("Fingerprint HAL only");
        }
    }

    private final class Request implements IBinder.DeathRecipient {
        final IIlluminationCallback client;
        final IBinder token;
        final int x, y, radius;
        final long generation;
        final long startedAt = SystemClock.uptimeMillis();
        boolean waitingForDisplay;
        boolean renderingStarted;
        boolean contextClassified;
        boolean interactiveAtStart;
        boolean interactiveStateKnown;
        int initialDisplayState = Display.STATE_UNKNOWN;
        int initialCommittedState = Display.STATE_UNKNOWN;
        boolean aodPulseHandled;
        Request(IIlluminationCallback callback, int x, int y, int radius, long generation) {
            client = callback;
            token = callback.asBinder();
            this.x = x;
            this.y = y;
            this.radius = radius;
            this.generation = generation;
        }
        void unlink() { token.unlinkToDeath(this, 0); }
        @Override public void binderDied() {
            synchronized (mLock) {
                if (mOwner != this) return;
                mOwner = null;
                nativeSetGeneration(++mGeneration);
                mWorker.post(IlluminationApplication.this::clearHardware);
            }
        }
    }

    private boolean current(Request request) {
        synchronized (mLock) { return currentLocked(request); }
    }

    private boolean currentLocked(Request request) {
        return mOwner == request && mGeneration == request.generation;
    }

    private void releaseLocked(Request request) {
        request.unlink();
        mOwner = null;
        nativeSetGeneration(++mGeneration);
    }

    private void start(Request request) {
        if (!current(request)) return;
        cancelPreparation("scan active");
        // Learn the authoritative physical geometry from the validated HAL request.
        // An app restart falls back to on-demand rendering until this is known.
        mSensorX = request.x;
        mSensorY = request.y;
        mSensorRadius = request.radius;
        mLastReadyElapsedMs = -1;
        try {
            if (!clearHardware()) throw new IOException("Unable to reset fingerprint HBM");
            ensureDisplayReady(request);
        } catch (IOException | RuntimeException e) {
            fail(request, e.toString());
        }
    }

    private static boolean isScanReady(Display display) {
        // getState() changes before SurfaceFlinger finishes the display power transition.
        // A committed ON state completes that transition; presentation and HBM checks
        // still follow because this is not an optical readiness measurement.
        DisplayInfo info = new DisplayInfo();
        return display != null && display.getDisplayInfo(info)
                && info.state == Display.STATE_ON && info.committedState == Display.STATE_ON;
    }

    private Boolean isInteractiveForScan() {
        try {
            return mPowerManager == null ? null : mPowerManager.isInteractive();
        } catch (RuntimeException e) {
            Log.e(TAG, "Interactive state unavailable; skipping scan context", e);
            return null;
        }
    }

    private static boolean isDrawable(int state) {
        return state == Display.STATE_ON || state == Display.STATE_DOZE;
    }

    private void ensureDisplayReady(Request request) throws IOException {
        synchronized (mLock) {
            if (!currentLocked(request)) return;
            Display display = mDisplayManager.getDisplay(Display.DEFAULT_DISPLAY);
            if (display == null) throw new IOException("Display is unavailable");
            // A missing framework dependency must fail this request, not crash the
            // persistent process before its diagnostic Binder service is registered.
            if (mPowerManager == null) {
                PowerManager powerManager = getSystemService(PowerManager.class);
                if (powerManager == null) {
                    throw new IOException("PowerManager is unavailable (power/thermalservice)");
                }
                PowerManager.WakeLock wakeLock = powerManager.newWakeLock(
                        PowerManager.PARTIAL_WAKE_LOCK, "TetrisUdfps:scan");
                wakeLock.setReferenceCounted(false);
                mPowerManager = powerManager;
                mScanWakeLock = wakeLock;
            }
            if (!request.contextClassified) {
                try {
                    request.interactiveAtStart = mPowerManager.isInteractive();
                    request.interactiveStateKnown = true;
                } catch (RuntimeException e) {
                    Log.w(TAG, "Initial interactive state unavailable; skipping scan context", e);
                }
                DisplayInfo initialInfo = new DisplayInfo();
                if (display.getDisplayInfo(initialInfo)) {
                    request.initialDisplayState = initialInfo.state;
                    request.initialCommittedState = initialInfo.committedState;
                }
                request.contextClassified = true;
            }
            // Bound the CPU hold independently of Java timeout delivery during suspend.
            mScanWakeLock.acquire(WAKE_LOCK_TIMEOUT_MS);
            if (!isScanReady(display)) {
                request.waitingForDisplay = true;
                mState = "waiting for display";
                long waitStartedAt = SystemClock.uptimeMillis();
                // The FOD wake-up sensor lets SystemUI request the fingerprint doze pulse.
                // A full wake here races its proximity check and can end Doze before the
                // held contact is delivered. Keep this request pending until that pulse.
                mWorker.postAtTime(() -> {
                    if (current(request) && request.waitingForDisplay) {
                        fail(request, "SystemUI doze pulse timeout");
                    }
                }, waitStartedAt + DISPLAY_READY_TIMEOUT_MS);
                // Recheck in case the pulse completed ON while this worker was starting.
                // The queued task still validates the current owner.
                mWorker.post(() -> {
                    if (!current(request) || !request.waitingForDisplay) return;
                    Display readyDisplay = mDisplayManager.getDisplay(Display.DEFAULT_DISPLAY);
                    if (readyDisplay != null && isScanReady(readyDisplay)) {
                        startRendering(request);
                    }
                });
                return;
            }
        }
        startRendering(request);
    }

    private void startRendering(Request request) {
        synchronized (mLock) {
            if (!currentLocked(request) || request.renderingStarted) return;
            request.waitingForDisplay = false;
            request.renderingStarted = true;
        }
        try {
            Display display = mDisplayManager.getDisplay(Display.DEFAULT_DISPLAY);
            if (display == null || !isScanReady(display)) {
                throw new IOException("Display is not ready for fingerprint capture");
            }
            Point size = new Point();
            display.getRealSize(size);
            mWidth = size.x;
            mHeight = size.y;
            mRotation = display.getRotation();
            SensorGeometry geometry = new SensorGeometry(request.x, request.y, request.radius,
                    mWidth, mHeight, mRotation);
            BrightnessInfo info = display.getBrightnessInfo();
            float brightness = info == null ? Float.NaN : info.adjustedBrightness;
            if (!validBrightness(brightness)) {
                brightness = mDisplayManager.getBrightness(Display.DEFAULT_DISPLAY);
            }
            mBrightness = brightness;
            mAlpha = mCalibration.alpha(brightness);
            synchronized (mLock) {
                if (!currentLocked(request)) return;
            }
            publishScanContext(request);
            mState = "waiting for presentation";
            // JNI waits at most 500 ms for the actual present fence, off the main/binder threads.
            boolean presented = nativeShow(mWidth, mHeight, display.getLayerStack(), geometry.x, geometry.y,
                    geometry.radiusX, geometry.radiusY, mAlpha, request.generation);
            if (!presented) {
                throw new IOException("Compensation surface was not presented: "
                        + nativeGetDiagnostics());
            }
            synchronized (mLock) {
                if (!currentLocked(request)) {
                    clearHardware();
                    return;
                }
                Point presentedSize = new Point();
                display.getRealSize(presentedSize);
                if (!isScanReady(display) || display.getRotation() != mRotation
                        || presentedSize.x != mWidth || presentedSize.y != mHeight) {
                    throw new IOException("Display changed during presentation");
                }
                // The marked gralloc buffer makes the composer carry HBM_ENABLE with its
                // pixels. Do not issue a second, unsynchronised sysfs enable after presentation.
                if (!"1".equals(readHbm())) {
                    throw new IOException("Composer did not enable fingerprint HBM");
                }
                mState = "waiting for panel";
            }
            // The present fence covers the compositor frame. Readback is still a driver
            // state, not an optical measurement; retain two refresh periods for panel settling
            // before notifying Goodix, pending measurements on the device.
            float refreshRate = display.getRefreshRate();
            if (!Float.isFinite(refreshRate) || refreshRate <= 0) refreshRate = 60f;
            long settleMs = Math.max(17L, Math.min(67L, (long) Math.ceil(2000f / refreshRate)));
            mWorker.postDelayed(() -> ready(request), settleMs);
        } catch (IOException | RuntimeException e) {
            fail(request, e.toString());
        }
    }

    private void ready(Request request) {
        try {
            synchronized (mLock) {
                if (!currentLocked(request)) return;
                Display display = mDisplayManager.getDisplay(Display.DEFAULT_DISPLAY);
                Point size = new Point();
                if (display != null) display.getRealSize(size);
                if (display == null || !isScanReady(display)
                        || display.getRotation() != mRotation || size.x != mWidth
                        || size.y != mHeight) {
                    throw new IOException("Display changed before sensor readiness");
                }
                if (!"1".equals(readHbm())) {
                    throw new IOException("Fingerprint HBM was revoked");
                }
                writeNode(UI_READY, true);
                mState = "illuminating";
                queueVisibleAodPulseLocked(request);
            }
            mLastReadyElapsedMs = SystemClock.uptimeMillis() - request.startedAt;
        } catch (IOException | RuntimeException e) {
            fail(request, e.toString());
        }
    }

    private void queueVisibleAodPulseLocked(Request request) {
        if (!currentLocked(request) || request.aodPulseHandled) return;
        request.aodPulseHandled = true;
        if (!request.contextClassified || !request.interactiveStateKnown
                || request.interactiveAtStart) {
            mLastAodPulse = "skipped=interactive";
            return;
        }
        if (request.initialDisplayState == Display.STATE_UNKNOWN
                || request.initialCommittedState == Display.STATE_UNKNOWN) {
            mLastAodPulse = "skipped=initial_display_unavailable";
            return;
        }
        if (isDozeState(request.initialDisplayState)
                || isDozeState(request.initialCommittedState)) {
            // The always-on UI is already present. Leave its policy to SystemUI.
            mLastAodPulse = "skipped=already_aod";
            return;
        }
        mLastAodPulse = "queued";
        try {
            // The display pulse, presentation and sensor readiness have already succeeded.
            // Do not put ActivityManager IPC on the capture worker or make capture wait
            // for SystemUI. No setting is changed and no extra wake-up is requested.
            getMainExecutor().execute(() -> dispatchVisibleAodPulse(request));
        } catch (RuntimeException e) {
            mLastAodPulse = "failed=queue " + e.getClass().getSimpleName();
            Log.w(TAG, "Unable to queue the visible AOD pulse", e);
        }
    }

    private static boolean isDozeState(int state) {
        return state == Display.STATE_DOZE || state == Display.STATE_DOZE_SUSPEND;
    }

    private void dispatchVisibleAodPulse(Request request) {
        if (!current(request)) return;
        boolean displayReady;
        boolean interactive;
        try {
            // Sample framework state outside the capture lock: these getters can use Binder.
            Display display = mDisplayManager.getDisplay(Display.DEFAULT_DISPLAY);
            displayReady = isScanReady(display);
            // An unknown power state must not request UI.
            PowerManager powerManager = mPowerManager;
            interactive = powerManager == null || powerManager.isInteractive();
        } catch (RuntimeException e) {
            synchronized (mLock) {
                if (currentLocked(request)) {
                    mLastAodPulse = "skipped=state_unavailable "
                            + e.getClass().getSimpleName();
                }
            }
            return;
        }
        Intent intent = new Intent(AOD_PULSE_ACTION).setPackage(SYSTEM_UI_PACKAGE)
                .addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY | Intent.FLAG_RECEIVER_FOREGROUND);
        synchronized (mLock) {
            if (!currentLocked(request)) return;
            if (!"illuminating".equals(mState) || !displayReady) {
                mLastAodPulse = "skipped=display_not_ready";
                return;
            }
            if (interactive) {
                mLastAodPulse = "skipped=interactive_or_unavailable";
                return;
            }
            mLastAodPulse = "dispatching";
        }
        // Ownership is checked at the dispatch decision. An in-flight broadcast may
        // finish after finger-up, but must never hold up HAL cancellation or cleanup.
        String result;
        try {
            // This existing receiver is registered by the system-user SystemUI process.
            // "requested" is not an acknowledgement that its UI has been presented.
            sendBroadcastAsUser(intent, UserHandle.SYSTEM);
            result = "requested";
        } catch (RuntimeException e) {
            result = "failed=broadcast " + e.getClass().getSimpleName();
            Log.w(TAG, "Unable to request the visible AOD pulse", e);
        }
        synchronized (mLock) {
            if (currentLocked(request)) {
                mLastAodPulse = result;
            }
        }
    }

    private static boolean validBrightness(float value) {
        return Float.isFinite(value) && value >= 0f && value <= 1f;
    }

    private static void writeNode(String path, boolean enabled) throws IOException {
        try (FileOutputStream stream = new FileOutputStream(path)) {
            stream.write((enabled ? "1" : "0").getBytes(StandardCharsets.US_ASCII));
        }
    }

    private static void writeScanContext(String command) throws IOException {
        try (FileOutputStream stream = new FileOutputStream(HBM)) {
            stream.write(command.getBytes(StandardCharsets.US_ASCII));
        }
    }

    private void publishScanContext(Request request) throws IOException {
        Boolean interactiveNow = isInteractiveForScan();
        if (!current(request)) return;
        if (!request.contextClassified || !request.interactiveStateKnown || interactiveNow == null) {
            // An unknown power state must not opt into either synchronized scan path.
            // The worker serializes token cleanup before any newer owner can publish.
            endScanContext();
            mLastScanContext = "skipped=power_state_unavailable";
            return;
        }
        // A pulse may already be ON while non-interactive. Never upgrade its origin
        // to interactive if unlocking wakes the phone before rendering.
        String mode = request.interactiveAtStart && interactiveNow ? "interactive" : "ambient";
        try {
            // The versioned prefix is rejected by older kernels, without toggling HBM.
            writeScanContext("scan_v1 begin " + request.generation + " " + mode);
            mScanContextToken = request.generation;
            mLastScanContext = mode;
        } catch (IOException | SecurityException e) {
            // Even a close failure can follow an accepted write. Revoke any context
            // before falling back to the previous kernel path, with no surface yet.
            mLastScanContext = "unavailable: " + e.getClass().getSimpleName();
            writeNode(HBM, false);
            if (!"0".equals(readHbm())) {
                throw new IOException("Cannot revoke fingerprint scan context", e);
            }
            mScanContextToken = 0;
            Log.w(TAG, "Scan context unavailable; using previous kernel path", e);
        }
    }

    private void endScanContext() {
        long token = mScanContextToken;
        if (token == 0) return;
        // clearHardware has already confirmed HBM off. Legacy HBM=0 also revokes
        // the context; the token-specific end is idempotent and cannot clear a newer one.
        try {
            writeScanContext("scan_v1 end " + token);
        } catch (IOException | SecurityException e) {
            Log.e(TAG, "Cannot finish scan context after HBM reset", e);
        }
        mScanContextToken = 0;
    }

    private String readHbm() throws IOException {
        String state = Files.readString(Path.of(HBM)).trim();
        if (!"0".equals(state) && !"1".equals(state)) {
            throw new IOException("Invalid HBM state: " + state);
        }
        mLastHbmState = state;
        return state;
    }

    private boolean clearHardware() {
        boolean cleared = true;
        boolean forcedOff = false;
        // Revoke capture first. Removing the marked buffer lets the composer lower HBM
        // alongside the unmasked frame, rather than darkening a still-compensated frame.
        try { writeNode(UI_READY, false); }
        catch (IOException e) { cleared = false; Log.e(TAG, "Cannot clear UI-ready", e); }
        boolean hidden = nativeHide();
        try {
            forcedOff = !"0".equals(readHbm());
            // Always release a stale sysfs override, including while the display is OFF:
            // readback can be zero while the old request remains latched. After a normal
            // composer removal the driver is already OFF and this sends no panel command.
            writeNode(HBM, false);
            if (!"0".equals(readHbm())) throw new IOException("Panel did not leave HBM");
            endScanContext();
            Display display = mDisplayManager.getDisplay(Display.DEFAULT_DISPLAY);
            if (!hidden && display != null && isDrawable(display.getState())) {
                throw new IOException("Compensation removal was not presented");
            }
        } catch (IOException e) { cleared = false; Log.e(TAG, "Cannot clear illumination", e); }
        mLastHidePresented = hidden;
        mLastForcedHbmOff = forcedOff;
        mLastCleanupSucceeded = cleared;
        if (mScanWakeLock != null && mScanWakeLock.isHeld()) mScanWakeLock.release();
        mWidth = mHeight = 0;
        mState = cleared ? "idle" : "hardware reset failed";
        // Ownership is released before cleanup is queued. Only now may idle
        // preparation resume, after the surface and HBM have been cleared.
        updatePreparation();
        return cleared;
    }

    private static final class PreparationKey {
        final int width, height, rotation;
        final float x, y, radiusX, radiusY, alpha;

        PreparationKey(int width, int height, int rotation, SensorGeometry geometry, float alpha) {
            this.width = width;
            this.height = height;
            this.rotation = rotation;
            x = geometry.x;
            y = geometry.y;
            radiusX = geometry.radiusX;
            radiusY = geometry.radiusY;
            this.alpha = alpha;
        }

        boolean matches(PreparationKey other) {
            return other != null && width == other.width && height == other.height
                    && rotation == other.rotation && x == other.x && y == other.y
                    && radiusX == other.radiusX && radiusY == other.radiusY && alpha == other.alpha;
        }

        boolean available() {
            return nativeHasBuffer(width, height, x, y, radiusX, radiusY, alpha);
        }
    }

    private PreparationKey readPreparationKey() {
        synchronized (mLock) {
            if (mOwner != null || !"idle".equals(mState) || mSensorRadius == 0) return null;
        }
        try {
            Display display = mDisplayManager.getDisplay(Display.DEFAULT_DISPLAY);
            if (display == null || !isScanReady(display)
                    || !Boolean.TRUE.equals(isInteractiveForScan())) return null;
            Point size = new Point();
            display.getRealSize(size);
            int rotation = display.getRotation();
            SensorGeometry geometry = new SensorGeometry(mSensorX, mSensorY, mSensorRadius,
                    size.x, size.y, rotation);
            BrightnessInfo info = display.getBrightnessInfo();
            float brightness = info == null ? Float.NaN : info.adjustedBrightness;
            if (!validBrightness(brightness)) {
                brightness = mDisplayManager.getBrightness(Display.DEFAULT_DISPLAY);
            }
            if (!validBrightness(brightness)) return null;
            return new PreparationKey(size.x, size.y, rotation, geometry,
                    mCalibration.alpha(brightness));
        } catch (RuntimeException e) {
            // Speculation is optional; a display/service transition must not affect scans.
            return null;
        }
    }

    private void cancelPreparation(String reason) {
        mWorker.removeCallbacks(mPrepareWhenStable);
        mDesiredPreparation = null;
        mFailedPreparation = null;
        mPreparationToken = nativeCancelPreparation();
        mPreparationState = reason;
        // Do not remove a producer task: its completion is what clears the
        // in-flight flag. The native token cancels it before/during rendering.
    }

    private void updatePreparation() {
        PreparationKey key = readPreparationKey();
        if (key == null) {
            if (mDesiredPreparation != null || mPreparationRunning) {
                cancelPreparation("not idle and interactive");
            }
            return;
        }
        if (key.available()) {
            if (mDesiredPreparation != null || mPreparationRunning) {
                cancelPreparation("ready");
            }
            mPreparationState = "ready";
            return;
        }
        long now = SystemClock.uptimeMillis();
        if (!key.matches(mDesiredPreparation)) {
            mWorker.removeCallbacks(mPrepareWhenStable);
            mPreparationToken = nativeCancelPreparation();
            mDesiredPreparation = key;
            mFailedPreparation = null;
            mPreparationStableSince = now;
        }
        if (key.matches(mFailedPreparation)) return;
        if (mPreparationRunning) {
            mPreparationState = "waiting for cancelled preparation";
            return;
        }
        mWorker.removeCallbacks(mPrepareWhenStable);
        mWorker.postAtTime(mPrepareWhenStable, Math.max(mPreparationStableSince + PREPARATION_QUIET_MS,
                mLastPreparationStartedAt + PREPARATION_INTERVAL_MS));
        mPreparationState = "waiting for stable brightness";
    }

    private void prepareWhenStable() {
        PreparationKey key = readPreparationKey();
        if (key == null || !key.matches(mDesiredPreparation) || key.available()) {
            updatePreparation();
            return;
        }
        long now = SystemClock.uptimeMillis();
        long at = Math.max(mPreparationStableSince + PREPARATION_QUIET_MS,
                mLastPreparationStartedAt + PREPARATION_INTERVAL_MS);
        if (now < at) {
            mWorker.postAtTime(mPrepareWhenStable, at);
            return;
        }
        if (mPreparationRunning || key.matches(mFailedPreparation)) return;
        final long token;
        synchronized (mLock) {
            // A Binder begin may race the display snapshot above. Never let
            // speculation start behind an already accepted scan request.
            if (mOwner != null || !"idle".equals(mState)) {
                cancelPreparation("scan active");
                return;
            }
            token = nativeCancelPreparation();
            mPreparationToken = token;
            mPreparationRunning = true;
            mLastPreparationStartedAt = now;
            mPreparationState = "preparing";
        }
        mPreparationWorker.post(() -> {
            long startedAt = SystemClock.uptimeMillis();
            boolean prepared = false;
            try {
                prepared = nativePrepareBuffer(key.width, key.height, key.x, key.y,
                        key.radiusX, key.radiusY, key.alpha, token);
            } catch (RuntimeException e) {
                Log.e(TAG, "Optional illumination preparation failed", e);
            }
            final boolean completed = prepared;
            mWorker.post(() -> preparationFinished(key, token, completed, startedAt));
        });
    }

    private void preparationFinished(PreparationKey key, long token, boolean prepared,
            long startedAt) {
        // A background thread may start well after dispatch. Rate-limit the
        // actual work too, including when a stale job yields to the latest key.
        mLastPreparationStartedAt = Math.max(mLastPreparationStartedAt, startedAt);
        mPreparationRunning = false;
        boolean latest = token == mPreparationToken && key.matches(mDesiredPreparation);
        if (latest && !prepared) {
            // No retry loop for memory/mapper failures at an unchanged brightness.
            // Normal scans retain the synchronous rendering path.
            mFailedPreparation = key;
            mPreparationState = "failed; on-demand rendering available";
            return;
        }
        updatePreparation();
    }

    private void abortCurrent(String reason) {
        Request request;
        synchronized (mLock) { request = mOwner; }
        if (request != null) fail(request, reason);
    }

    private void fail(Request request, String reason) {
        synchronized (mLock) {
            if (!currentLocked(request)) return;
            releaseLocked(request);
        }
        mLastFailure = "generation=" + request.generation + " state=" + mState
                + " elapsed_ms=" + (SystemClock.uptimeMillis() - request.startedAt)
                + " reason=" + reason;
        clearHardware();
        Log.e(TAG, "Illumination aborted: " + mLastFailure);
        try { request.client.onFailure(); }
        catch (RemoteException ignored) { /* Dead HAL is already detached. */ }
    }

    private static native boolean nativeShow(int width, int height, int layerStack, float x,
            float y, float radiusX, float radiusY, float alpha, long generation);
    private static native void nativeSetGeneration(long generation);
    private static native boolean nativeHide();
    private static native long nativeCancelPreparation();
    private static native boolean nativeHasBuffer(int width, int height, float x, float y,
            float radiusX, float radiusY, float alpha);
    private static native boolean nativePrepareBuffer(int width, int height, float x, float y,
            float radiusX, float radiusY, float alpha, long token);
    private static native String nativeGetDiagnostics();
}
