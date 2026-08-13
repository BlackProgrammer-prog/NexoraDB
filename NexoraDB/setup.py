import shutil
import subprocess
from pathlib import Path

from setuptools import Distribution, setup
from setuptools.command.build_py import build_py
from setuptools.command.bdist_wheel import bdist_wheel


class BinaryDistribution(Distribution):
    def has_ext_modules(self) -> bool:
        return True


class PlatformWheel(bdist_wheel):
    def finalize_options(self) -> None:
        super().finalize_options()
        self.root_is_pure = False


class BuildPyWithStrippedNativeLibrary(build_py):
    """Strip only the copy staged for the wheel, never the developer's .so."""

    def run(self) -> None:
        super().run()

        native_dir = Path(self.build_lib) / "nexoradb" / "native"
        native_libraries = sorted(native_dir.glob("nexoradb*.so"))
        if not native_libraries:
            raise RuntimeError(
                "The NexoraDB native library is missing. Build the CMake target "
                "'nexoradb_py' and copy its .so into src/nexoradb/native first."
            )

        strip = shutil.which("strip")
        if strip is None:
            raise RuntimeError("GNU strip is required to build the binary wheel.")

        for library in native_libraries:
            subprocess.run([strip, "--strip-unneeded", str(library)], check=True)


setup(
    distclass=BinaryDistribution,
    cmdclass={
        "bdist_wheel": PlatformWheel,
        "build_py": BuildPyWithStrippedNativeLibrary,
    },
)
