"""Where the installer finds what it runs: the one place that maps the release folder and a repository checkout.

Release (tools/make_release.py), root = the unpacked folder:

    MTF-Setup.exe           starts python\\pythonw.exe -m installer.gui
    python/python.exe       runs every tool; its ._pth puts .., ../tools, ../tools/shaders and ../recomp on sys.path
    llvm/                   llvm-mingw
    cmake/bin/cmake.exe     builds the game (the sources of CMakeLists.txt and runtime/, with the player's lifted code)
    bin/ninja.exe
    bin/xlate/dmsh_xlate.exe  the shader translator, self-contained
    deps/                   include/, lib/ and bin/ of SDL3, Vulkan, VMA, Opus, libvpx, zstd
    CMakeLists.txt, runtime/, recomp/, tools/, profiles/
    VERSION                 the release version, one line
    temp/                   the default temp folder (created)

Dev mode, root = a repository checkout: .venv's Python, the toolchain of docs/building.md under build/toolchain, the
version from VERSION with -dev and the commit, build/generated/installer as the temp folder.
"""
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Layout:
    root: Path
    dev: bool
    release: str
    python: Path
    llvm: Path            # llvm-mingw tree
    cmake: Path
    ninja: Path
    deps: Path            # include/, lib/, bin/
    xlate: Path           # the shader translator: an .exe in a release, a .dll for dotnet in a checkout
    temp: Path

    @property
    def runtime(self):
        return self.root / 'runtime'

    def missing(self):
        """Paths the steps need that do not exist."""
        need = [self.python, self.llvm / 'bin' / 'clang.exe', self.cmake, self.ninja, self.deps / 'include' / 'SDL3' / 'SDL.h',
                self.deps / 'lib', self.xlate, self.root / 'CMakeLists.txt', self.runtime / 'main.c',
                self.root / 'recomp' / 'lift.py', self.root / 'recomp' / 'glue.py', self.root / 'tools' / 'nup_extract.py',
                self.root / 'tools' / 'shaders' / 'translate.py']
        return [str(p) for p in need if not p.exists()]


def release_layout(root):
    version = root / 'VERSION'
    return Layout(root=root, dev=False,
                  release=version.read_text(encoding='utf-8').strip() if version.is_file() else 'unknown',
                  python=root / 'python' / 'python.exe', llvm=root / 'llvm', cmake=root / 'cmake' / 'bin' / 'cmake.exe',
                  ninja=root / 'bin' / 'ninja.exe', deps=root / 'deps', xlate=root / 'bin' / 'xlate' / 'dmsh_xlate.exe',
                  temp=root / 'temp')


def dev_layout(root):
    commit = subprocess.run(['git', '-C', str(root), 'rev-parse', '--short', 'HEAD'], capture_output=True,
                            text=True).stdout.strip()
    semver = (root / 'VERSION').read_text(encoding='utf-8').strip()
    toolchain = root / 'build' / 'toolchain'
    manifest = toolchain / 'toolchain-manifest.json'
    tools = json.loads(manifest.read_text(encoding='utf-8-sig')) if manifest.is_file() else {}
    trees = sorted(p.parent.parent for p in toolchain.glob('llvm-mingw-*/bin/clang.exe'))
    venv = root / '.venv' / 'Scripts' / 'python.exe'
    return Layout(root=root, dev=True, release=f'{semver}-dev+{commit or "unknown"}',
                  python=venv if venv.is_file() else Path(sys.executable),
                  llvm=trees[-1] if trees else toolchain / 'llvm-mingw-missing',
                  cmake=Path(tools.get('cmake_path', toolchain / 'cmake-missing')),
                  ninja=Path(tools.get('ninja_path', toolchain / 'ninja.exe')), deps=toolchain / 'deps',
                  xlate=toolchain / 'dmsh_xlate-sbuf' / 'dmsh_xlate.dll', temp=root / 'build' / 'generated' / 'installer')


def is_release(root):
    return (root / 'python' / 'python.exe').is_file()


def find(root=None):
    """The layout of root (default: the folder above this package)."""
    root = Path(root or Path(__file__).resolve().parent.parent).resolve()
    return release_layout(root) if is_release(root) else dev_layout(root)
