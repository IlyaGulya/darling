# DarlingServer resume-before-suspend reproducer

For the preferred black-box regression reproducer, which runs one unchanged
Mach-O client against unmodified server binaries, see [BLACKBOX.md](BLACKBOX.md).
This document describes the white-box amplifier used to deterministically prove
the exact resume-before-suspend interleaving.

This harness deterministically widens the race between an XNU semaphore wake
and `DarlingServer::Thread::suspend()`.

Without the resume-permit fix, the semaphore wake can arrive before the
microthread marks itself suspended. `Thread::resume()` drops that wake and the
microthread then sleeps forever.

The harness does not require any prebuilt binaries from the original
investigation. Build the RED and GREEN binaries from source as described below.

## Prerequisites

- A built Darling checkout.
- A disposable Darling prefix containing Homebrew portable Ruby.
- Rust/Cargo for `tools/darling-debug-runner`.

Build the runner:

```sh
cargo build --release --manifest-path tools/darling-debug-runner/Cargo.toml
```

The defaults expect:

```text
Darling executable:  ../darling-prefix/bin/darling
Disposable prefix:   ../darling-prefix-homebrew-test
Portable Ruby:       /usr/local/Homebrew/Library/Homebrew/vendor/portable-ruby/current/bin/ruby
```

Override them with `DARLING` and `DPREFIX`.

## Build the RED binary

Use a worktree or temporary branch at the commit immediately before the
resume-permit fix. The old timer-specific workaround must also be removed so
timer delivery exercises the shared `Thread::kernelAsync` worker.

If using separate worktrees, configure a separate top-level Darling build
directory for each worktree. A build directory remains tied to the source
worktree used when CMake configured it.

From the top-level Darling checkout:

```sh
git -C src/external/darlingserver apply --check ../../../tools/repro-darlingserver-resume-race/restore-shared-timer.patch
git -C src/external/darlingserver apply --check ../../../tools/repro-darlingserver-resume-race/amplify-suspend-window.patch
git -C src/external/darlingserver apply ../../../tools/repro-darlingserver-resume-race/restore-shared-timer.patch
git -C src/external/darlingserver apply ../../../tools/repro-darlingserver-resume-race/amplify-suspend-window.patch
ninja -C ../darling-build src/external/darlingserver/darlingserver
cp ../darling-build/src/external/darlingserver/darlingserver /tmp/darlingserver-resume-race-red
```

## Build the GREEN binary

At the commit containing the resume-permit fix, only apply the amplifier:

```sh
git -C src/external/darlingserver apply --check ../../../tools/repro-darlingserver-resume-race/amplify-suspend-window.patch
git -C src/external/darlingserver apply ../../../tools/repro-darlingserver-resume-race/amplify-suspend-window.patch
ninja -C ../darling-build src/external/darlingserver/darlingserver
cp ../darling-build/src/external/darlingserver/darlingserver /tmp/darlingserver-resume-race-green
```

Do not commit the amplifier. It intentionally inserts a 20 ms delay into every
duct-taped wait.

## Run

```sh
ITERATIONS=3 tools/repro-darlingserver-resume-race.sh \
  red=/tmp/darlingserver-resume-race-red \
  green=/tmp/darlingserver-resume-race-green
```

Expected result:

```text
red pass=0 timeout=3 failed=0
green pass=3 timeout=0 failed=0
```

Each invocation installs the selected `darlingserver` into the disposable
prefix, runs the same Ruby timed-wait workload, and gently shuts Darling down.
