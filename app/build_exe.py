"""Build release/PIP LINK Setup.exe (one file, no Python needed on the target PC).

    python build_exe.py            # uses the package already in assets/package
    python build_exe.py 1.0        # first refresh assets/package from the current firmware + holotape

Needs: pip install pyinstaller "esptool>=4.7,<5" pillow heatshrink2 requests pyserial numpy contourpy osmium
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
SEP = ";" if os.name == "nt" else ":"


def main():
    if len(sys.argv) > 1:                                       # refresh the built-in software package
        subprocess.check_call([sys.executable, os.path.join(REPO, "tools", "make_update.py"), sys.argv[1],
                               "--dir", os.path.join(HERE, "assets", "package")])
    work = os.path.join(REPO, "build", "pyinstaller")
    args = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--windowed",
            "--name", "PIP LINK Setup", "--icon", os.path.join(HERE, "assets", "icon.ico"),
            "--paths", os.path.join(REPO, "tools"),
            "--add-data", f"{os.path.join(HERE, 'assets')}{SEP}assets",
            "--add-data", f"{os.path.join(HERE, 'assets', 'DejaVuSansMono-Bold.ttf')}{SEP}.",
            "--collect-data", "esptool", "--collect-submodules", "esptool",
            "--hidden-import", "contourpy", "--hidden-import", "heatshrink2",
            "--collect-submodules", "osmium", "--collect-binaries", "osmium",
            "--distpath", os.path.join(REPO, "release"), "--workpath", work, "--specpath", work,
            os.path.join(HERE, "piplink_setup.py")]
    subprocess.check_call(args)
    exe = os.path.join(REPO, "release", "PIP LINK Setup.exe")
    print(f"\nbuilt {exe}  ({os.path.getsize(exe) / 1048576:.0f} MB)")


if __name__ == "__main__":
    main()
