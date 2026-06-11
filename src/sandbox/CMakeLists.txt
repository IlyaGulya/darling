project(libsandbox)

set(DYLIB_INSTALL_NAME "/usr/lib/system/libsystem_sandbox.dylib")
add_circular(system_sandbox FAT
	SOURCES
	sandbox.c

	SIBLINGS
		system_dyld
	DEPENDENCIES
		system_c
		system_kernel
		system_malloc
)

install(TARGETS system_sandbox DESTINATION libexec/darling/usr/lib/system)

# macOS ships the sandbox-exec(1) command alongside the sandbox library. Under
# Darling the sandbox is a no-op, so install a thin pass-through that keeps the
# CLI compatible (see sandbox-exec.sh) for callers like Homebrew.
install(PROGRAMS sandbox-exec.sh DESTINATION libexec/darling/usr/bin RENAME sandbox-exec)
