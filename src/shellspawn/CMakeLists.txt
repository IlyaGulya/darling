project(shellspawn)

add_definitions(-nostdinc)

add_darling_executable(shellspawn
	shellspawn.c
	duct_signals.c
	../startup/runtime_mode.c
)
target_compile_definitions(shellspawn PRIVATE
	DARLING_RUNTIME_EUNION_CAPABLE=$<BOOL:${DARLING_EUNION}>
)

install(TARGETS shellspawn DESTINATION libexec/darling/usr/libexec)
install(FILES org.darlinghq.shellspawn.plist DESTINATION libexec/darling/System/Library/LaunchDaemons)
