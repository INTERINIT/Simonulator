package org.interinit.simonulator;

import android.Manifest;
import android.content.Context;
import android.content.BroadcastReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.os.BatteryManager;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;
import java.util.Arrays;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

public class SimonActivity extends SDLActivity {
    private static native void setHostBatteryLow(boolean low);
    private static native void setHostRecordingAllowed(boolean allowed);
    private BroadcastReceiver batteryReceiver;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (mBrokenLibraries) return;
        batteryReceiver = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                switch (intent.getAction()) {
                    case Intent.ACTION_BATTERY_LOW: setHostBatteryLow(true); break;
                    case Intent.ACTION_BATTERY_OKAY: setHostBatteryLow(false); break;
                    case Intent.ACTION_BATTERY_CHANGED: updateBatteryLevel(intent); break;
                }
            }
        };
        IntentFilter filter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        filter.addAction(Intent.ACTION_BATTERY_LOW);
        filter.addAction(Intent.ACTION_BATTERY_OKAY);
        Intent current = registerReceiver(batteryReceiver, filter);
        if (current != null) updateBatteryLevel(current);
    }

    private void updateBatteryLevel(Intent battery) {
        if (battery.hasExtra(BatteryManager.EXTRA_BATTERY_LOW)) {
            setHostBatteryLow(battery.getBooleanExtra(BatteryManager.EXTRA_BATTERY_LOW, false));
            return;
        }
        // Older Android releases publish the percentage and plug state only.
        int threshold = getResources().getIdentifier("config_lowBatteryWarningLevel", "integer", "android");
        int level = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
        int scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, 0);
        if (threshold != 0 && level >= 0 && scale > 0) {
            setHostBatteryLow(battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) == 0
                    && 100L * level <= (long) scale * getResources().getInteger(threshold));
        }
    }

    @Override protected void onDestroy() {
        if (batteryReceiver != null) unregisterReceiver(batteryReceiver);
        super.onDestroy();
    }

    @Override protected void onResume() {
        super.onResume();
        if (!mBrokenLibraries) setHostRecordingAllowed(
                checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED);
    }

    @Override protected void onPause() {
        if (!mBrokenLibraries) setHostRecordingAllowed(false);
        super.onPause();
    }

    @Override protected String[] getArguments() {
        String dir = getFilesDir().getAbsolutePath();
        ArrayList<String> args = new ArrayList<>(Arrays.asList("ibmsimon",
                "-rompath", dir + "/roms", "-cfg_directory", dir + "/cfg",
                "-nvram_directory", dir + "/nvram", "-state_directory", dir + "/sta",
                "-skip_gameinfo", "-noui_active", "-keepaspect", "-unevenstretch", "-nofilter",
                "-view", "Simon Front", "-video", "soft", "-sound", "sdl", "-mouse", "-lightgun_device", "lightgun"));
        String host = getIntent().getStringExtra("host");
        if (host != null && !host.isEmpty()) {
            args.add("-bitb");
            args.add("socket." + host + ":" + getIntent().getStringExtra("port"));
        }
        return args.toArray(new String[0]);
    }

    @Override protected SDLSurface createSDLSurface(Context context) {
        return new SDLSurface(context) {
            private int finger = -1;
            private float penX, penY;

            @Override public boolean onTouch(View view, MotionEvent event) {
                // SDL's lightgun normalizes coordinates against the whole window.
                // Map the LCD inside the centered Simon Front view to that range.
                float width = getWidth(), height = getHeight();
                float scale = Math.min(width / 220f, height / 684f);
                if (scale == 0) return false;
                int action = event.getActionMasked();
                int index = event.getActionIndex();
                if (action == MotionEvent.ACTION_CANCEL) {
                    if (finger >= 0) onNativeMouse(0, MotionEvent.ACTION_UP, penX, penY, false);
                    finger = -1;
                    return true;
                }
                if (action == MotionEvent.ACTION_MOVE) {
                    index = event.findPointerIndex(finger);
                    if (index < 0) return true;
                }
                int id = event.getPointerId(index);
                float x = ((event.getX(index) - (width - 220 * scale) / 2) / scale - 10) / 200;
                float y = ((event.getY(index) - (height - 684 * scale) / 2) / scale - 34) / 640;
                if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                    if (finger >= 0 || x < 0 || x > 1 || y < 0 || y > 1) return true;
                    finger = id;
                    action = MotionEvent.ACTION_DOWN;
                } else if (id != finger) return true;
                if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
                    action = MotionEvent.ACTION_UP;
                    finger = -1;
                }
                penX = Math.max(0, Math.min(1, x)) * (width - 1);
                penY = Math.max(0, Math.min(1, y)) * (height - 1);
                // SDL determines the changed button from the mask after the event.
                onNativeMouse(action == MotionEvent.ACTION_UP ? 0 : MotionEvent.BUTTON_PRIMARY,
                        action, penX, penY, false);
                return true;
            }
        };
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        if (code == KeyEvent.KEYCODE_VOLUME_UP || code == KeyEvent.KEYCODE_VOLUME_DOWN) {
            int side = code == KeyEvent.KEYCODE_VOLUME_UP ? KeyEvent.KEYCODE_F8 : KeyEvent.KEYCODE_F7;
            if (event.getAction() == KeyEvent.ACTION_UP) onNativeKeyUp(side);
            else if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) onNativeKeyDown(side);
            return true;
        }
        return super.dispatchKeyEvent(event);
    }
}
