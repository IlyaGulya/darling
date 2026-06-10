# Black-box timer-wait reproducer

`timer-wait-stress.c` is a standalone Mach-O client that reproduces the lost
wakeup through public Mach APIs. It does not require changes to DarlingServer.
Compile it once, then run that exact binary against old and fixed, unmodified
DarlingServer builds.

The client creates independent Mach semaphores and has concurrent threads
repeatedly wait for short timeouts. A lost wake strands DarlingServer's shared
timer worker, after which the client cannot finish.

## Build the client once

The following example builds through the Darwin `clang` installed in a Darling
prefix and writes the result to a host path visible from Darling:

```sh
darling shell /bin/bash -lc \
  '/usr/bin/clang -O2 -Wall -Wextra -pthread \
  /Volumes/SystemRoot/path/to/darling/tools/repro-darlingserver-resume-race/timer-wait-stress.c \
  -o /Volumes/SystemRoot/tmp/timer-wait-stress'
```

Do not rebuild or modify `/tmp/timer-wait-stress` between the RED and GREEN
runs.

## Run against unmodified servers

Build the server once at the parent of the fix commit and once at the fix
commit. Do not apply the amplifier patches used by the white-box reproducer.

```sh
ITERATIONS=3 tools/repro-darlingserver-timer-wait.sh \
  /Volumes/SystemRoot/tmp/timer-wait-stress \
  old=/tmp/darlingserver-old \
  fixed=/tmp/darlingserver-fixed
```

Expected result:

```text
old pass=0 timeout=3 failed=0
fixed pass=3 timeout=0 failed=0
```

The default workload is 32 threads, 1000 timed waits per thread, and a 500 us
timeout. Override it with `THREADS`, `WAITS`, and `DELAY_NS` when investigating
scheduler-sensitive behavior.
