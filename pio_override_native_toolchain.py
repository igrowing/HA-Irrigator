import os
from pathlib import Path

# Absolute path to MSYS2 UCRT64 bin
MSYS2_UCRT64_BIN = r"C:\msys64\ucrt64\bin"

def before_env_configure(env):
    # Prepend MSYS2 UCRT64 to PATH for this environment only
    old_path = env.get("ENV", {}).get("PATH", "")
    env.Replace(
        ENV={
            "PATH": MSYS2_UCRT64_BIN + os.pathsep + old_path,
            **{k: v for k, v in env.get("ENV", {}).items() if k != "PATH"}
        }
    )
    # Optionally force specific tools
    env.Replace(
        CC="g++",
        CXX="g++",
        LINK="g++",
    )

# PlatformIO hook
Import("env")
env.AddPreAction("$BUILD_DIR", before_env_configure)
