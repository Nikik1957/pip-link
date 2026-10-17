"""Build and run the PC unit tests for lib/remote_term (the TERM tab's terminal emulator + Telnet).

    pip install ziglang        (a self-contained C/C++ compiler, once)
    python tools/vt_test.py
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "lib", "remote_term", "src")
exe = os.path.join(tempfile.gettempdir(), "vt_test.exe")
cmd = [sys.executable, "-m", "ziglang", "c++", "-std=c++11", "-O1", "-Wall", "-Wno-nullability-completeness", "-I", SRC,
       os.path.join(ROOT, "test", "vt_test.cpp"), os.path.join(SRC, "vt_screen.cpp"), os.path.join(SRC, "telnet_codec.cpp"),
       "-o", exe]
subprocess.run(cmd, check=True)
sys.exit(subprocess.run([exe]).returncode)
