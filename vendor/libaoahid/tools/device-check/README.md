# Device check

`capture.sh` records the checks listed in
[`docs/TARGET_MATRIX.md`](../../docs/TARGET_MATRIX.md#checking-a-new-device)
that need `adb`: kernel device capabilities, Android input classification
(`dumpsys input`), and a bounded `getevent` stream. You choose the event node,
duration, and output directory; it does not decide pass or fail.

```sh
tools/device-check/capture.sh \
  <adb-serial> /dev/input/eventN <seconds> <output-directory>
```

Run the app below separately to see what a foreground application receives,
and keep its log with the capture.

## Input test app

`app/` is a standalone Android application. It lists the `InputDevice`
objects visible to the framework and logs key and motion events delivered to
its foreground `Activity`. The on-screen log keeps at most 262,144 characters;
the same records also go to logcat under the tag `AoaHidDeviceCheck`.

CI builds a debug APK as the `aoahid-device-check-debug-apk` artifact. To
build it yourself, use Gradle 9.5.0 and JDK 17, as CI does (Android Gradle
plugin 9.3.0):

```sh
gradle --no-daemon -p tools/device-check/app :app:assembleDebug
adb -s <adb-serial> install -r \
  tools/device-check/app/app/build/outputs/apk/debug/app-debug.apk
adb -s <adb-serial> shell am start -n \
  dev.aoahid.devicecheck/.MainActivity
adb -s <adb-serial> logcat -s 'AoaHidDeviceCheck:I' '*:S'
```

Exercise the keys, axes, contacts, buttons, hover states, and lift states
suggested for the profile in `docs/TARGET_MATRIX.md`. Keep the app log,
device/build identity, `capture.sh` output, and your pass/fail decision
together. Events that Android intercepts before they reach the foreground
activity (for example system or media keys) do not appear in the app.

## Android APIs used by the app

| Official source | Section or symbol | Revision |
| --- | --- | --- |
| [Android Gradle plugin 9.3.0](https://developer.android.com/build/releases/gradle-plugin) | Compatibility: Gradle 9.5.0, JDK 17, maximum API 37 | 9.3.0 (July 2026) |
| [InputManager API reference](https://developer.android.com/reference/android/hardware/input/InputManager) | `getInputDeviceIds`, `getInputDevice`, `registerInputDeviceListener`, `InputDeviceListener` | Versionless platform API page |
| [InputDevice API reference](https://developer.android.com/reference/android/view/InputDevice) | Identity/source accessors, `getMotionRanges`, `MotionRange` accessors | Versionless platform API page |
| [KeyEvent API reference](https://developer.android.com/reference/android/view/KeyEvent) | Device/source, action, key code, scan code, meta state, repeat count | Versionless platform API page |
| [MotionEvent API reference](https://developer.android.com/reference/android/view/MotionEvent) | Action, pointer ID/count, tool type, coordinates, pressure, orientation, axis values | Versionless platform API page |
| [Activity API reference](https://developer.android.com/reference/android/app/Activity) | `dispatchKeyEvent`, `dispatchGenericMotionEvent`, `dispatchTouchEvent` | Versionless platform API page |
