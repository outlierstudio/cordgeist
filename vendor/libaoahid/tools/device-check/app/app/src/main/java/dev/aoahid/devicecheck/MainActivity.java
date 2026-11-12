// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * Records Android application-API input-device, key, and motion observations.
 * It classifies nothing and deliberately leaves pass/fail judgment to the evidence record.
 */
package dev.aoahid.devicecheck;

import android.app.Activity;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.Collections;
import java.util.List;
import java.util.Locale;

public final class MainActivity extends Activity implements InputManager.InputDeviceListener {
    private static final String TAG = "AoaHidDeviceCheck";
    private static final int MAXIMUM_DISPLAY_CHARACTERS = 256 * 1024;

    private final StringBuilder displayLog = new StringBuilder();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private InputManager inputManager;
    private ScrollView scrollView;
    private TextView outputView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        inputManager = (InputManager) getSystemService(INPUT_SERVICE);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        LinearLayout controls = new LinearLayout(this);
        controls.setOrientation(LinearLayout.HORIZONTAL);
        Button refresh = new Button(this);
        refresh.setText("Refresh devices");
        refresh.setOnClickListener(view -> recordAllDevices());
        controls.addView(refresh);
        Button clear = new Button(this);
        clear.setText("Clear log");
        clear.setOnClickListener(view -> {
            displayLog.setLength(0);
            outputView.setText("");
        });
        controls.addView(clear);
        root.addView(controls);

        outputView = new TextView(this);
        outputView.setTextIsSelectable(true);
        outputView.setTypeface(android.graphics.Typeface.MONOSPACE);
        scrollView = new ScrollView(this);
        scrollView.addView(outputView);
        root.addView(
                scrollView,
                new LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.MATCH_PARENT,
                        0,
                        1.0f));
        setContentView(root);

        appendRecord("APP created; no support conclusion is implied by this log");
        recordAllDevices();
    }

    @Override
    protected void onResume() {
        super.onResume();
        inputManager.registerInputDeviceListener(this, mainHandler);
    }

    @Override
    protected void onPause() {
        inputManager.unregisterInputDeviceListener(this);
        super.onPause();
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        appendRecord("DEVICE_ADDED id=" + deviceId);
        recordDevice(deviceId);
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        appendRecord("DEVICE_CHANGED id=" + deviceId);
        recordDevice(deviceId);
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        appendRecord("DEVICE_REMOVED id=" + deviceId);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        appendRecord(String.format(
                Locale.ROOT,
                "KEY time=%d action=%d device=%d source=0x%08x keyCode=%d scanCode=%d "
                        + "meta=0x%08x repeat=%d",
                event.getEventTime(),
                event.getAction(),
                event.getDeviceId(),
                event.getSource(),
                event.getKeyCode(),
                event.getScanCode(),
                event.getMetaState(),
                event.getRepeatCount()));
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        recordMotion("GENERIC_MOTION", event);
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        recordMotion("TOUCH", event);
        return super.dispatchTouchEvent(event);
    }

    private void recordAllDevices() {
        int[] identifiers = inputManager.getInputDeviceIds();
        appendRecord("DEVICE_LIST count=" + identifiers.length);
        for (int identifier : identifiers) {
            recordDevice(identifier);
        }
    }

    private void recordDevice(int identifier) {
        InputDevice device = inputManager.getInputDevice(identifier);
        if (device == null) {
            appendRecord("DEVICE id=" + identifier + " unavailable");
            return;
        }
        appendRecord(String.format(
                Locale.ROOT,
                "DEVICE id=%d descriptor=%s name=%s vendor=0x%04x product=0x%04x "
                        + "sources=0x%08x keyboardType=%d virtual=%s",
                device.getId(),
                device.getDescriptor(),
                device.getName(),
                device.getVendorId(),
                device.getProductId(),
                device.getSources(),
                device.getKeyboardType(),
                device.isVirtual()));
        for (InputDevice.MotionRange range : device.getMotionRanges()) {
            appendRecord(String.format(
                    Locale.ROOT,
                    "RANGE device=%d axis=%s(%d) source=0x%08x min=%s max=%s "
                            + "flat=%s fuzz=%s resolution=%s",
                    device.getId(),
                    MotionEvent.axisToString(range.getAxis()),
                    range.getAxis(),
                    range.getSource(),
                    Float.toString(range.getMin()),
                    Float.toString(range.getMax()),
                    Float.toString(range.getFlat()),
                    Float.toString(range.getFuzz()),
                    Float.toString(range.getResolution())));
        }
    }

    private void recordMotion(String kind, MotionEvent event) {
        appendRecord(String.format(
                Locale.ROOT,
                "%s time=%d actionMasked=%d actionIndex=%d device=%d source=0x%08x "
                        + "pointers=%d buttons=0x%08x meta=0x%08x",
                kind,
                event.getEventTime(),
                event.getActionMasked(),
                event.getActionIndex(),
                event.getDeviceId(),
                event.getSource(),
                event.getPointerCount(),
                event.getButtonState(),
                event.getMetaState()));
        InputDevice device = event.getDevice();
        List<InputDevice.MotionRange> ranges =
                device == null ? Collections.emptyList() : device.getMotionRanges();
        for (int pointer = 0; pointer < event.getPointerCount(); ++pointer) {
            StringBuilder record = new StringBuilder();
            record.append(String.format(
                    Locale.ROOT,
                    "POINTER index=%d id=%d toolType=%d x=%s y=%s pressure=%s size=%s "
                            + "orientation=%s",
                    pointer,
                    event.getPointerId(pointer),
                    event.getToolType(pointer),
                    Float.toString(event.getX(pointer)),
                    Float.toString(event.getY(pointer)),
                    Float.toString(event.getPressure(pointer)),
                    Float.toString(event.getSize(pointer)),
                    Float.toString(event.getOrientation(pointer))));
            for (InputDevice.MotionRange range : ranges) {
                record.append(' ')
                        .append(MotionEvent.axisToString(range.getAxis()))
                        .append('=')
                        .append(event.getAxisValue(range.getAxis(), pointer));
            }
            appendRecord(record.toString());
        }
    }

    private void appendRecord(String record) {
        Log.i(TAG, record);
        int required = record.length() + 1;
        int overflow = displayLog.length() + required - MAXIMUM_DISPLAY_CHARACTERS;
        if (overflow > 0) {
            int lineEnd = displayLog.indexOf("\n", overflow);
            displayLog.delete(0, lineEnd < 0 ? Math.min(overflow, displayLog.length()) : lineEnd + 1);
        }
        displayLog.append(record).append('\n');
        if (outputView != null) {
            outputView.setText(displayLog);
            scrollView.post(() -> scrollView.fullScroll(View.FOCUS_DOWN));
        }
    }
}
