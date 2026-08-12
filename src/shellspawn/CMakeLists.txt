project(shellspawn)

add_definitions(-nostdinc)

set(shellspawn_sources shellspawn.c duct_signals.c)
if(DARLING_LIFECYCLE_COHORT_V1)
	list(APPEND shellspawn_sources ../lifecycle/lifecycle_cohort_client.c)
endif()

add_darling_executable(shellspawn ${shellspawn_sources})
if(DARLING_LIFECYCLE_COHORT_V1)
	target_compile_definitions(shellspawn PRIVATE DARLING_LIFECYCLE_COHORT_V1=1)
endif()

install(TARGETS shellspawn DESTINATION libexec/darling/usr/libexec)
install(FILES org.darlinghq.shellspawn.plist DESTINATION libexec/darling/System/Library/LaunchDaemons)
