# Node 26 for Linux SPARC64: release candidate

This is an out-of-tree port. The package retains the upstream Node version and
uses an `rc1` suffix in its archive name. It is not an official Node.js release.

## Install and remove

Extract the binary archive into a directory you own, then put its `bin` directory
first in PATH. For example, from a directory containing the archive:

```sh
mkdir -p "$HOME/opt"
tar -xzf node-v26.12.1-linux-sparc64-rc1.tar.gz -C "$HOME/opt"
export PATH="$HOME/opt/node-v26.12.1-linux-sparc64-rc1/bin:$PATH"
node --version
npm --version
npx --version
```

The entire extracted directory can be moved. npm and npx use relative symlinks
and PATH to select node. Keep the installation intact; do not copy just npm's
launch scripts. To remove it, remove that directory and its PATH entry. npm's
user cache and any separately configured global installation prefix are separate.
No system installation or replacement of distribution packages is needed.

## Runtime and hardware

This is a 64-bit, big-endian Linux SPARC V9 build. It uses shared OpenSSL 3 and
ICU 78, glibc, libstdc++ and libgcc. `BUILD-PROVENANCE.json` records the actual
ELF dependencies and symbol versions; its `ldd` paths describe the build host,
not a required installation prefix. The initial validation machines use glibc
2.44 and GCC 16 runtime libraries. Older runtime compatibility is not claimed.
Use `ldd bin/node` to check the libraries on another machine.

T7 (M7) and T4 application-validation reports are supplied alongside the release
artifacts. Check their `passed` status and binary SHA-256 before treating a
hardware configuration as validated. The C/C++ build targets V9; no M7-specific
compiler tuning is requested. V8 currently probes FPU support and cache-line
sizes; the installed execution tests establish compatibility on each CPU.

Sparkplug and TurboFan generate native JavaScript code. Wasm has Liftoff and
optimizing compilation, including SIMD, GC and JSPI. SIMD operations currently
use scalar SPARC instructions; SIMD support does not imply VIS acceleration.
Native RegExp, workers, snapshots, TypeScript stripping, Temporal, and Node FFI
are enabled together. Maglev is disabled. Heap verification is compiled in and
can be requested with `--verify-heap`; normal runs do not enable stress flags.

npm packages containing portable JavaScript can be installed normally. Packages
with native code may lack SPARC prebuilt binaries and need a C/C++ toolchain,
Python and compatible source. The installed validation compiles a Node-API addon
using only the shipped headers and bundled npm/node-gyp. Each third-party native
addon still needs its own validation.

Upstream tests occasionally assume little-endian host TypedArrays. The original
Wasm `instance-gc` fixture and several FFI guards/byte expectations are recorded
as upstream fixture limitations in the parent evidence, not counted as unchanged
passes. A stale FFI arity expectation is also retained there. Derived endian-safe
fixtures were validated separately. This release does not claim that every
upstream test passes unchanged or that every npm application is supported.

## Build from the source archive

Extract the source archive and enter its root. The full port is already applied.
The separate binary patch applies to the base Git commit recorded in
`SOURCE-MANIFEST.json`; do not apply it again to this archive.

The native build uses Python 3, GNU Make, GCC/G++, GNU binutils, pkg-config,
OpenSSL 3 development files, ICU 78 development files, and Rust/Cargo with a
working `sparc64-unknown-linux-gnu` standard library. The validated build uses
GCC 16.2.1, binutils 2.47, Rust/Cargo 1.99.0. Record versions when rebuilding.
Cargo dependencies are vendored in the source tree. Allow at least 15 GiB free
space for build output, with additional space for archives and validation.

```sh
sh tools/sparc64/configure-node-features.sh all --prefix=/usr/local
CCACHE_DISABLE=1 make -j96
out/Release/node --version
python3 tools/install.py install --dest-dir="$PWD/out/install-stage" --prefix=/
```

`-j96` is the measured T7 setting; use a suitable lower parallelism elsewhere.
Do not edit sources during the build. `--v8-enable-verify-heap` persists across
GYP regeneration. Rust's copied archive is processed by
`deps/crates/isolate_libm.py` to avoid weak Rust libm definitions shadowing the
platform library; the raw Cargo archive is retained. Do not replace this with
manual archive edits.

For a fresh Git checkout, `tools/sparc64/build-release-candidate.py` records the
source inventory and clean build. The packaging helper requires that recorded
build and a Git checkout; the ordinary configure/make/install commands above
also work from the source archive without Git metadata.

## Validate the installed package

Python 3, gcc/g++, OpenSSL's command-line tool and network access to the npm
registry are needed for validation. Runtime use itself does not require them.
Choose a new output directory outside the installation:

```sh
python3 "$PREFIX/share/doc/node-sparc64/validation/validate-installed.py" \
  --prefix "$PREFIX" --output "$PWD/installed-validation"
```

The runner checks the package manifest, installed paths, npm/npx lifecycle,
headers and native addon ABI, direct TypeScript, Temporal DST/workers, built-in
fetch/WebSocket, both Wasm tiers including SIMD and GC across JSPI suspension,
and actual optimized native FFI calls. Express/secure WebSocket applications use
lockfile-pinned dependencies and locally generated, explicitly trusted test TLS
certificates. A two-minute HTTPS/worker/WebSocket soak records throughput, heap,
RSS, and executed TurboFan code. No equal performance between T4 and T7 is
assumed. Individual commands have deadlines and preserve logs, including failures.
