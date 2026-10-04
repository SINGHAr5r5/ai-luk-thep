# PlatformIO pre-build script: version.txt is baked into the app descriptor by CMake, but editing
# that file alone does not make PlatformIO re-run CMake. When the version changed since the last
# build, touch CMakeLists.txt so the project is reconfigured and the new version really lands in the image.
Import("env")
import os

root = env.subst("$PROJECT_DIR")
ver = open(os.path.join(root, "version.txt")).read().strip()
stamp = os.path.join(env.subst("$BUILD_DIR"), ".version_stamp")
old = open(stamp).read().strip() if os.path.exists(stamp) else None
if old != ver:
    os.utime(os.path.join(root, "CMakeLists.txt"))
    os.makedirs(os.path.dirname(stamp), exist_ok=True)
    open(stamp, "w").write(ver)
    print(f"version_stamp: {old} -> {ver}, forcing CMake reconfigure")
