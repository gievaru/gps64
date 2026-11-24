GTA:SA GPS - ARM64 port (libGTASA.so 2.10), milestone 1

Build (GitHub, no NDK needed):
 1. Create a repo on github.com, upload ALL files of this folder (keep the .github folder).
 2. Open the Actions tab -> "Build GPS arm64" -> wait for the green check.
 3. Open the finished run -> Artifacts -> GPS64 -> libGPS64.so
 4. Put libGPS64.so where your other 64-bit AML mods go, start the game,
    set a waypoint on the map, get into a car.
 5. Send me the AML/logcat lines that contain "GPS".

Not ported yet: node-limit / streaming patches, line on the pause map.
