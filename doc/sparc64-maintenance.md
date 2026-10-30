# Maintaining Node 26 on Linux SPARC64

The development branch is `shalseth/node:sparc64`. Merge from
`nodejs/node:v26.x`, not `main`. GitHub compares forks against upstream's default
branch in its banner; thousands of commits relative to `main` do not measure
how far this port is behind Node 26. Keep updates as merges so published port
history remains usable. Do not force-push a rewritten development branch.

This procedure builds from Git/source using C/C++ and Rust. An installed Node
is not required. Gentoo's source-only `net-libs/nodejs-9999` is in
[shalseth/gentoo-sparc64](https://github.com/shalseth/gentoo-sparc64#installing-nodejs).
The Gentoo README documents installation, dependencies and npm configuration.

## Upstream merge

Use a clean working tree and preserve existing source/build/test checkpoints.
Do not merge or edit compiled sources during a live Make. From a development
checkout:

```sh
git remote -v
# Add this once if the remote is absent:
git remote add upstream https://github.com/nodejs/node.git
git fetch origin sparc64
git fetch upstream refs/heads/v26.x:refs/remotes/upstream/v26.x
git switch -c sparc64-update-YYYYMMDD origin/sparc64
git branch backup/sparc64-before-YYYYMMDD HEAD
git rev-list --left-right --count HEAD...upstream/v26.x
git log --oneline HEAD..upstream/v26.x
git merge --no-ff --no-commit upstream/v26.x
```

Review conflicts against the new upstream implementation and sibling
architectures. In particular check GYP/V8 build flags and architecture dispatch,
register/stack conventions, builtins and safepoints, compiler selection and
instruction lowering, deoptimization, RegExp, Wasm ABI/debugger code, Rust/Cargo
integration, libffi and the FFI entry paths. Retain platform libm isolation for
Rust's weak math exports. Preserve SPARC stack bias, register windows, delay
slots, endianness and native stack-argument accounting.

Keep upstream tests intact. Do not convert an unexpected failure into a skip or
expected failure merely to turn a gate green. Review changed upstream test data,
status files, feature flags and test-count assumptions explicitly. Stage the
resolved merge, inspect it, and commit locally using the repository's author,
signoff and assistance conventions in AGENTS.md. A local commit is needed for
the clean-build source inventory; defer pushing until the appropriate gates
have been assessed.

```sh
git diff --check
git diff --cached --stat
git status
git commit -s
git rev-parse HEAD
```

If the merge is unsuitable, `git merge --abort` is available before committing.
Keep the backup ref and failed attempt; do not overwrite old logs on retry.

## Fresh native build

Use GCC/G++, GNU binutils, Python 3, Make, pkg-config, OpenSSL 3 development
files, ICU 78+ development files, and a working native SPARC64 Rust/Cargo
installation. Initial validation used GCC 16.2.1 and Rust 1.99.0. Cargo's
vendored dependencies are in the source tree and ordinary builds run offline.

Allow at least 22 GiB free build space and 8 GiB memory, with extra space for
retaining multiple trees and test inputs. Check `df -h . /var/tmp` before and
during long work. On T7, use `-j96`, the measured balance for its 32 cores and
256 hardware threads. Do not infer that `-j256` is faster.

The following commands run on native SPARC. Use a separate detached worktree
for every candidate so earlier outputs remain reviewable:

```sh
git worktree add --detach ../node-sparc64-build-YYYYMMDD HEAD
cd ../node-sparc64-build-YYYYMMDD
python3 tools/sparc64/build-port.py --jobs 96 --prefix=/usr
```

The helper requires committed, clean sources and no prior `config.gypi` or
`out/Release`. It records all tracked files with NUL-delimited names, including
Unicode paths and symlinks, records configure/Make outcomes and executable
hashes, checks that sources and HEAD stayed unchanged, probes the enabled
runtime features, and writes V8 test metadata from the actual compiler flags.
Its record is `out/sparc64-builds/<timestamp>/build.json`. Preserve failed records.

For a source archive without Git metadata, use the ordinary build directly:

```sh
sh tools/sparc64/configure-node-features.sh all \
  --prefix=/usr --use-prefix-to-find-headers
CARGO_NET_OFFLINE=true CCACHE_DISABLE=1 make -j96
python3 tools/sparc64/write-test-config.py --allow-turbofan --allow-webassembly
```

`--v8-enable-verify-heap` must persist across GYP regeneration. Keep Maglev off
until its backend is implemented and validated. The build makes its own native
snapshot/compiler tools; no existing Node binary is used as a bootstrap.

The historical `build-node-features.py`, `build-release-candidate.py` and
checkpoint audit-chain drivers authenticate earlier uncommitted worktrees and
local parent manifests. Use them to reproduce those checkpoints with their
original inputs. Do not invent parent manifests or copy passing reports to make
those historical audits accept a new committed checkout.

## Regression gates

Keep distinct logs and fresh labels for every candidate. Run full C++ suites
sequentially: inspector fixtures bind a fixed port and collide if full suites
share a host concurrently. The known intermittent Abseil futex EINVAL remains
an unresolved failure, not an accepted retry or skip.

From the build root:

```sh
mkdir -p out/maintenance-validation
out/Release/cctest --gtest_output=json:out/maintenance-validation/cctest.json \
  > out/maintenance-validation/cctest.log 2>&1
python3 tools/test.py --mode=release --arch=sparc64 -j16 --flaky-tests=run \
  message parallel sequential > out/maintenance-validation/node.log 2>&1
sparc_build_root=$PWD
(cd deps/v8 && python3 tools/run-tests.py \
  --outdir="$sparc_build_root/out/Release" --variants=default --no-presubmit \
  --rerun-failures-count=0 --exit-after-n-failures=0 -j16 \
  --json-test-results="$sparc_build_root/out/maintenance-validation/mjsunit.json" \
  mjsunit) > out/maintenance-validation/mjsunit.log 2>&1
```

Inspect each exit code before continuing. Generic runners can exclude tests by
architecture or feature flags; a successful generic run is not proof that Wasm,
Temporal, Amaro or FFI executed. Explicit feature/tier proofs are required.
The bounded source-ebuild `USE=test` selection is useful during development but
does not replace this full validation matrix.

Run the unchanged feature suites and independent parser/Temporal checks:

```sh
make -j96 build-ffi-tests
python3 tools/sparc64/test-node-feature-upstream.py --feature amaro --label YYYYMMDD
python3 tools/sparc64/test-node-feature-upstream.py --feature temporal --label YYYYMMDD
python3 tools/sparc64/test-node-feature-upstream.py --feature ffi --label YYYYMMDD
python3 tools/sparc64/test-amaro-parser-native.py --label YYYYMMDD
python3 tools/sparc64/prepare-node-features-test262.py
python3 tools/sparc64/test-temporal-test262.py --label YYYYMMDD
```

The FFI unchanged-suite report can retain the documented big-endian skips and
obsolete arity expectation. Inspect that outcome and run its independent
native-call/callback/memory controls; do not report it as an unchanged full pass.
Test262's revision/count assertions deliberately require review when upstream
changes the pinned revision or selection.

Fetch pinned Wasm inputs and run all production specification groups in both
native tiers and normal tiering:

```sh
python3 tools/sparc64/prepare-wasm-suites.py
for tier in liftoff optimized default; do
  python3 tools/sparc64/test-wasm-production-spec.py \
    --feature production --tier "$tier" --label YYYYMMDD
done
for tier in liftoff optimized; do
  for suite in simd-core gc-core simd-abi; do
    python3 tools/sparc64/test-wasm-production-upstream.py \
      --tier "$tier" --suite "$suite" --label "YYYYMMDD-$tier-$suite"
  done
done
for suite in deopt-all inlining-core tiering-core; do
  python3 tools/sparc64/test-wasm-production-upstream.py \
    --tier default --suite "$suite" --label "YYYYMMDD-$suite"
done
python3 tools/sparc64/build-wasm-production-inspector.py --label YYYYMMDD
python3 tools/sparc64/test-wasm-debug-simd.py --label YYYYMMDD
```

The production spec baseline has 258 scripts, with upstream expectations
retained; experimental Wasm extensions are outside that selection. When new
upstream inputs change the inventory, review the new tests and outcomes before
updating fixed count/revision assertions. Retain the original endian-dependent
`instance-gc` failure separately from the explicit little-endian derived control.

Also repeat the standalone installed-workflow fixtures in
`tools/sparc64/release-validation`: actual native compilation of JavaScript and
Wasm, SIMD/llhttp, GC across JSPI suspension, eight-argument/native FFI and its
libffi fallback, RegExp, custom snapshots, cold/warm TypeScript caches, Temporal
DST/workers, npm/npx, native addon builds and real HTTPS/WebSocket applications.
Keep independent arithmetic/byte-order oracles and compiler-tier assertions.
Run bounded application soaks and record throughput, retained heap/RSS and host
load on T7 and T4. Longer stability testing remains separate work.

## Portage preparation and installation

Keep the overlay's README, `net-libs/nodejs/nodejs-9999.ebuild` and installed
notes current. Check live ebuild fetching, dependency resolution and configure
under Portage's environment. For a full validation run, enable `USE=test`, retain
the fetched Git revision and run the source build and staged image checks before
merging into the host. `contrib/validate-nodejs-image.py` in the overlay tests the
image without installing it into the running system. Its native addon check
uses npm's system configuration and shipped headers without explicit `--nodedir`.

Source installations use shared ICU/OpenSSL and Gentoo's toolchain flags;
validate the actual rebuilt artifact rather than assuming it matches a former
binary hash. A system library ABI upgrade can require rebuilding Node.

When a user requests to perform the final `emerge`, leave that step to them.
Provide the exact keyword, repository-sync and source-emerge commands from the
overlay README. No prebuilt Node release is needed or used by this ebuild.

## Publishing source updates

After reviewing validation and preserving every unexpected outcome, push the
candidate as a normal fast-forward update of the fork's `sparc64` branch:

```sh
git push origin HEAD:sparc64
```

Push overlay documentation/ebuild changes to `shalseth/gentoo-sparc64` after its
Portage checks. Sync `/var/db/repos/sparc64` on T7 and T4 only when their working
trees are clean. Never overwrite local edits. No action here authorizes an
upstream Node/V8 PR or submission; upstreaming remains a separate decision.
