# PlatformIO pre-build script: keep the build machine's user folder (and so its user name) out of the firmware.
# __FILE__ paths end up in asserts and log strings; map the home folder to "Byrd" in both slash styles.
import os

Import("env")  # noqa: F821 - provided by PlatformIO

home = os.path.expanduser("~")
for h in {home, home.replace("\\", "/")}:
    env.Append(CCFLAGS=[f"-fmacro-prefix-map={h}=Byrd"])  # noqa: F821
