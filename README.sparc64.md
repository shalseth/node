# Node.js for Linux SPARC64

This is the out-of-tree 64-bit big-endian SPARC port of Node 26 and its bundled
V8 engine. Development is on the `sparc64` branch, tracking upstream `v26.x`.
The initial source checkpoint is Node v26.12.1 / V8 14.6.202.34-node.40.

Build from source with native GCC/G++, Python and Rust/Cargo. **An existing
Node.js installation is not needed.** On Gentoo, use the source-only
[`net-libs/nodejs-9999` ebuild](https://github.com/shalseth/gentoo-sparc64#installing-nodejs).
It fetches this Git branch; it does not download a prebuilt Node package.

Native execution supports Sparkplug, TurboFan, native RegExp, Liftoff and
optimizing Wasm, including production SIMD, GC and JSPI. npm/npx, native addons,
workers, TypeScript stripping, Temporal, FFI, snapshots and code caches are
enabled together. Maglev is disabled. SIMD uses scalar SPARC lowering rather
than VIS acceleration. Other FFI signatures use libffi when no fast path applies.

Validated hardware is SPARC T4 and M7/T7 on glibc Linux. Initial native validation
used GCC 16.2.1, Rust/Cargo 1.99.0, binutils 2.47, glibc 2.44, ICU 78.3 and
OpenSSL 3.6.5. More hardware/toolchain combinations require validation.

**The port is experimental.** One intermittent Abseil futex EINVAL in the
native C++ suite remains unresolved. It has not reproduced in subsequent normal
and kernel-traced runs or targeted waiter/signal tests; this does not establish
that it is fixed. Preserve the first failure and its logs. Some upstream
Wasm/FFI fixtures have little-endian assumptions, architecture skips or stale
expectations. Passing derived controls is reported separately from unchanged
test outcomes. Experimental Wasm extensions outside the production scope are
not covered by the validation claim.

The baseline passed broad Node/V8 regressions, Temporal Test262, native ABI/FFI,
Wasm specifications in three modes, inspector/deoptimization, npm workflows and
bounded real-application soaks. These are evidence for that recorded baseline;
they do not certify every future live-branch commit or every npm package.

For native builds, upstream merges and validation, follow
[doc/sparc64-maintenance.md](doc/sparc64-maintenance.md). Historical bring-up
notes and checkpoint-specific drivers remain in
[tools/sparc64/README](tools/sparc64/README); their hardcoded checkpoint state
must not be mistaken for a fresh-checkout build procedure.
