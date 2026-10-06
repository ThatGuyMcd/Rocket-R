package com.rocketret.rocketr;

import org.libsdl.app.SDLActivity;

import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.app.ActivityManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.ViewGroup;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import java.io.File;

public final class RocketActivity extends SDLActivity {
    private TouchControlsView touchControls;
    private AudioManager audioManager;
    private AudioFocusRequest focusRequest;
    private OnBackInvokedCallback backCallback;
    private int requestedFrameRate = -1;
    private final AudioManager.OnAudioFocusChangeListener focusListener = change -> {
        nativeAudioFocus(change == AudioManager.AUDIOFOCUS_GAIN ? 1.0f :
                         change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK ? 0.2f : 0.0f);
        Log.i("Rocket-R", "[android-audio] focus changed=" + change);
    };

    static native void nativeTouchState(int buttons, float x, float y);
    static native void nativeTouchLook(float x, float y, boolean recenter);
    static native boolean nativeCameraActive();
    static native void nativeToggleOverlay();
    static native boolean nativeOverlayVisible();
    private static native int nativePresentationRate();
    private static native void nativeOutputRate(int rate);
    private static native void nativeAudioFocus(float gain);

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (mBrokenLibraries || mLayout == null) return;
        ActivityManager manager = (ActivityManager)getSystemService(ACTIVITY_SERVICE);
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        if (manager != null) manager.getMemoryInfo(memory);
        Log.i("Rocket-R", "[android-device] model=" + Build.MANUFACTURER + " " + Build.MODEL +
              " sdk=" + Build.VERSION.SDK_INT + " ram_mb=" + memory.totalMem / (1024 * 1024) +
              " low_ram=" + (manager != null && manager.isLowRamDevice()));
        if (mSurface != null) mSurface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) { requestedFrameRate = -1; }
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) { requestedFrameRate = -1; }
            @Override public void surfaceDestroyed(SurfaceHolder holder) { requestedFrameRate = -1; }
        });
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        audioManager = (AudioManager)getSystemService(AUDIO_SERVICE);
        int rate = 48000;
        if (audioManager != null) {
            try {
                int preferred = Integer.parseInt(audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE));
                if (preferred >= 8000 && preferred <= 96000) rate = preferred;
            } catch (NumberFormatException ignored) { }
            if (Build.VERSION.SDK_INT >= 26) {
                focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(new AudioAttributes.Builder()
                        .setLegacyStreamType(AudioManager.STREAM_MUSIC).build())
                    .setOnAudioFocusChangeListener(focusListener).build();
            }
        }
        nativeOutputRate(rate);
        touchControls = new TouchControlsView(this);
        mLayout.addView(touchControls, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        touchControls.requestApplyInsets();
        if (Build.VERSION.SDK_INT >= 33) {
            backCallback = this::openSettings;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }
    }

    @Override protected void onResume() {
        super.onResume();
        if (mBrokenLibraries) return;
        requestedFrameRate = -1;
        if (audioManager != null) {
            int result = Build.VERSION.SDK_INT >= 26
                ? audioManager.requestAudioFocus(focusRequest)
                : audioManager.requestAudioFocus(focusListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN);
            nativeAudioFocus(result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED ? 1.0f : 0.0f);
            Log.i("Rocket-R", "[android-audio] focus=" + result + " media volume=" +
                  audioManager.getStreamVolume(AudioManager.STREAM_MUSIC) + "/" +
                  audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC));
        }
        if (touchControls != null) touchControls.resume();
    }

    @Override protected void onPause() {
        if (touchControls != null) touchControls.pause();
        if (!mBrokenLibraries) nativeAudioFocus(0);
        if (audioManager != null) {
            if (Build.VERSION.SDK_INT >= 26) audioManager.abandonAudioFocusRequest(focusRequest);
            else audioManager.abandonAudioFocus(focusListener);
        }
        super.onPause();
    }

    @Override public void onWindowFocusChanged(boolean focused) {
        if (!focused && touchControls != null) touchControls.release();
        super.onWindowFocusChanged(focused);
    }

    @Override public void onBackPressed() {
        if (mBrokenLibraries) { super.onBackPressed(); return; }
        openSettings();
    }

    private void openSettings() {
        if (touchControls != null) touchControls.release();
        nativeToggleOverlay();
    }

    void updateGameFrameRate(boolean overlayVisible) {
        if (Build.VERSION.SDK_INT < 30 || mSurface == null) return;
        Surface surface = mSurface.getHolder().getSurface();
        if (!surface.isValid()) { requestedFrameRate = -1; return; }
        int rate = overlayVisible ? 60 : nativePresentationRate();
        if (rate == requestedFrameRate) return;
        try {
            // This is a display hint, not a second limiter or a simulation change.
            surface.setFrameRate(rate, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
            requestedFrameRate = rate;
        } catch (IllegalArgumentException | IllegalStateException failure) {
            requestedFrameRate = rate;
            Log.w("Rocket-R", "Display frame-rate hint unavailable", failure);
        }
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        // SDL consumes keyboard-sourced Back before Activity.onBackPressed.
        // Handle this route too, without generating a second SDL shortcut.
        if (!mBrokenLibraries && event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) openSettings();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override protected void onDestroy() {
        if (Build.VERSION.SDK_INT >= 33 && backCallback != null) {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
        }
        if (touchControls != null) touchControls.pause();
        super.onDestroy();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        File rom = new File(getFilesDir(), "rocket.us.z64");
        File config = new File(getFilesDir(), "config");
        if (!config.exists()) config.mkdirs();
        return new String[] { "--rom", rom.getAbsolutePath(), "--config", config.getAbsolutePath() };
    }
}
