#!/bin/sh
# Darling stub for macOS sandbox-exec(1).
#
# The macOS sandbox (Seatbelt / libsystem_sandbox) provides no isolation under
# Darling's emulation -- sandbox_init() and friends are no-ops. This stub keeps
# the command-line interface compatible (so callers such as Homebrew, which wrap
# build steps in `sandbox-exec -f profile.sb <command>`, work unchanged) by
# discarding the profile-selection options and executing the command directly.
#
# Recognised options that take an argument: -f file, -p string, -n name,
# -D key=value. Everything after the options (or after a `--`) is the command.

while [ $# -gt 0 ]; do
	case "$1" in
		-f|-p|-n|-D)
			# These options consume the following argument.
			shift 2
			;;
		--)
			shift
			break
			;;
		-*)
			# Any other flag with no argument.
			shift
			;;
		*)
			break
			;;
	esac
done

exec "$@"
