cmake_minimum_required(VERSION 3.13)

SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_SYSTEM_PROCESSOR "x86-64")

# This allows setting the compiler with -DCMAKE_C_COMPILER when configuring.
if (NOT DEFINED CMAKE_C_COMPILER)
	find_program(CMAKE_C_COMPILER NAMES
		"clang"
		"clang-4.0"
		"clang-6.0"
		"clang-7"
		"clang-9"
		"clang-10"
		"clang-11"
		"clang-12"
		"clang-13"
		"clang-14"
		"clang-15"
		"clang-16"
		"clang-17"
		"clang-18"
		"clang-19"
		"clang-20"
		"clang-21"
		"clang-22"
		"clang-23"
)
endif(NOT DEFINED CMAKE_C_COMPILER)

if (NOT DEFINED CMAKE_CXX_COMPILER)
	find_program(CMAKE_CXX_COMPILER NAMES
		"clang++"
		"clang++-4.0"
		"clang++-6.0"
		"clang++-7"
		"clang++-9"
		"clang++-10"
		"clang++-11"
		"clang++-12"
		"clang++-13"
		"clang++-14"
		"clang++-15"
		"clang++-16"
		"clang++-17"
		"clang++-18"
		"clang++-19"
		"clang++-20"
		"clang++-21"
		"clang++-22"
		"clang++-23"
)
endif(NOT DEFINED CMAKE_CXX_COMPILER)

option(DARLING_NO_CCACHE "Disable ccache usage" OFF)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND NOT DARLING_NO_CCACHE)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif()

SET(CMAKE_SKIP_RPATH TRUE)

# technically ignored by CMake when building on non-Apple platforms, but it's already a standard variable for the
# SDK deployment target, so we'll just use it and then add it to the compiler flags manually
set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "The version of macOS we're simulating")

project(darling)

enable_language(ASM)

set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "core")

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(clang_version_check)
include(InstallSymlink)
include(MacroEnsureOutOfSourceBuild)
include(dsym)
include(xcproj)
include(architecture)
include(create_symlink)

set(CLANG_RECOMMENDED_MINIMUM_VERSION 11)
clang_version_check(${CMAKE_C_COMPILER} c ${CLANG_RECOMMENDED_MINIMUM_VERSION})
clang_version_check(${CMAKE_CXX_COMPILER} cpp ${CLANG_RECOMMENDED_MINIMUM_VERSION})

MACRO_ENSURE_OUT_OF_SOURCE_BUILD()

set(DARLING_TOP_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set(DARLING_NO_EXECUTABLES OFF)
set(CMAKE_C_IMPLICIT_LINK_LIBRARIES "")
set(CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "")

if (${CMAKE_HOST_SYSTEM_PROCESSOR} MATCHES "i686|i386")
	message(FATAL_ERROR "This software can only be built on x86-64 systems")
endif (${CMAKE_HOST_SYSTEM_PROCESSOR} MATCHES "i686|i386")

SET(IGNORED_WARNINGS "-Wno-nullability-completeness -Wno-deprecated-declarations -Wno-availability")

if (${CMAKE_C_COMPILER_ID} STREQUAL "Clang" AND NOT ${CMAKE_C_COMPILER_VERSION} VERSION_LESS "3.9")
	SET(IGNORED_WARNINGS "${IGNORED_WARNINGS} -Wno-expansion-to-defined")
endif (${CMAKE_C_COMPILER_ID} STREQUAL "Clang" AND NOT ${CMAKE_C_COMPILER_VERSION} VERSION_LESS "3.9")

if(${CMAKE_C_COMPILER_ID} STREQUAL "Clang" AND ${CMAKE_C_COMPILER_VERSION} VERSION_GREATER_EQUAL "11")
	# newer Clang chokes on the idiomatic way to use CF_ENUM; e.g. like this:
	#     typedef CF_ENUM(int, MyEnum) {
	#       MY_ENUM_THING,
	#       MY_ENUM_OTHER_THING,
	#       # etc...
	#     }
	set(IGNORED_WARNINGS "${IGNORED_WARNINGS} -Wno-elaborated-enum-base -Wno-undef-prefix")
	set(ASM_IGNORED_WARNINGS "${ASM_IGNORED_WARNINGS} -Wno-undef-prefix")
endif()

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${IGNORED_WARNINGS}")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${IGNORED_WARNINGS}")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${ASM_IGNORED_WARNINGS}")
SET(CMAKE_C_FLAGS_DEBUG "-O0 -ggdb")
SET(CMAKE_CXX_FLAGS_DEBUG "-O0 -ggdb")

# prevent object filename conflicts for two source files that differ only in extension (e.g. `object.c` and `object.m`)
set(CMAKE_C_OUTPUT_EXTENSION ".c.o")
set(CMAKE_CXX_OUTPUT_EXTENSION ".cpp.o")
set(CMAKE_OBJC_OUTPUT_EXTENSION ".m.o")

if(CMAKE_POSITION_INDEPENDENT_CODE)
	SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
	SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pie")
endif(CMAKE_POSITION_INDEPENDENT_CODE)

enable_language(ASM-ATT)

option(TARGET_i386 "Enable i386 slices" ON)
option(TARGET_x86_64 "Enable x86_64 slices" ON)
option(DEBIAN_PACKAGING "Packaging for Debian" OFF)
option(ENABLE_TESTS "Install in-prefix unit tests" OFF)
option(REGENERATE_SDK "Regenerate Header Files For Open Source SDK" OFF)

if (DEBIAN_PACKAGING)
	set(COMPONENTS_DEFAULT "all")
else()
	set(COMPONENTS_DEFAULT "stock")
endif()

set(COMPONENTS "${COMPONENTS_DEFAULT}" CACHE STRING "Choose which components of Darling to build")
include(darling_parse_components)
darling_parse_components("${COMPONENTS}")

set(COMPILE_PY2_BYTECODE AUTO CACHE STRING "Pre-compile bytecode for Python 2 packages")
set_property(CACHE COMPILE_PY2_BYTECODE PROPERTY STRINGS AUTO ON OFF)

string(TOLOWER "${COMPILE_PY2_BYTECODE}" COMPILE_PY2_BYTECODE)

if (COMPILE_PY2_BYTECODE STREQUAL "auto")
	find_package(Python2 QUIET COMPONENTS Interpreter)

	if (Python2_Interpreter_FOUND)
		set(COMPILE_PY2_BYTECODE ON)
		message("Found Python 2; enabling pre-compilation of Python bytecode")
	else()
		set(COMPILE_PY2_BYTECODE OFF)
		message("Python 2 not available; bytecode compilation is disabled")
	endif (Python2_Interpreter_FOUND)
endif (COMPILE_PY2_BYTECODE STREQUAL "auto")

if (COMPILE_PY2_BYTECODE)
	find_package(Python2 REQUIRED COMPONENTS Interpreter)
endif (COMPILE_PY2_BYTECODE)

set(ENABLE_METAL AUTO CACHE STRING "Build Darling with Metal support")
set_property(CACHE ENABLE_METAL PROPERTY STRINGS AUTO ON OFF)

string(TOLOWER "${ENABLE_METAL}" BUILD_METAL)

if(BUILD_METAL STREQUAL "auto")
	# check if Vulkan and LLVM are available
	find_program(LLVM_CONFIG_PROGRAM llvm-config)
	find_package(Vulkan)

	if(LLVM_CONFIG_PROGRAM AND Vulkan_FOUND)
		set(BUILD_METAL ON)
		message("Found required libraries; building with Metal support")
	else()
		set(BUILD_METAL OFF)
		message("Did not find required libraries (Vulkan and LLVM); building without Metal support")
	endif()
endif()

FindDsymutil()
find_package(Setcap REQUIRED)

# Missing CMakeLists.txt must trigger an error
cmake_policy(SET CMP0014 NEW)

generate_architecture()

# --- dar-gax: UNCONDITIONAL patchset-drift gate ------------------------------
# TWICE a fix registered in patches.yml was missing from the committed
# integration/homebrew tip the binary was built from, silently regressing it
# (dar-gwn.6 xnu ulock, dar-gwn.1.9 darlingserver assert-wait). A gate that
# relies on someone remembering to run `west patch status` is no gate. So wire
# it into the build itself: it cannot be bypassed by a plain `ninja <target>`.
#
# Opt out only for packaging/CI that builds outside the patched workspace:
#   -DDARLING_SKIP_DRIFT_GATE=ON
option(DARLING_SKIP_DRIFT_GATE "Skip the patchset-drift gate (packaging/CI only)" OFF)
set(DARLING_PATCH_PROFILE "" CACHE STRING
	"Patch profile expected by the drift gate (empty: derive integration/<profile> or homebrew)")
if(NOT DARLING_SKIP_DRIFT_GATE)
	# Locate the workspace that owns patches.yml (manifest repo two levels up:
	# <ws>/darling-dev/darling -> <ws>/darling-dev/darling-workspace).
	find_program(WEST_PROGRAM west)
	set(_drift_ws "${CMAKE_CURRENT_SOURCE_DIR}/../darling-workspace")
	set(_drift_profile "${DARLING_PATCH_PROFILE}")
	if(NOT _drift_profile)
		execute_process(
			COMMAND git branch --show-current
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			RESULT_VARIABLE _drift_branch_rc
			OUTPUT_VARIABLE _drift_branch
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET)
		if(_drift_branch_rc EQUAL 0 AND _drift_branch MATCHES "^integration/([A-Za-z0-9_.-]+)$")
			set(_drift_profile "${CMAKE_MATCH_1}")
		else()
			set(_drift_profile "homebrew")
		endif()
	endif()
	set(_drift_profile_manifest "${_drift_ws}/patches/${_drift_profile}/patches.yml")
	if(WEST_PROGRAM AND EXISTS "${_drift_profile_manifest}")
		# (1) configure-time check: fail `cmake`/reconfigure on drift.
		execute_process(
			COMMAND ${WEST_PROGRAM} patch status --profile ${_drift_profile} --strict
			WORKING_DIRECTORY "${_drift_ws}"
			RESULT_VARIABLE _drift_rc
			OUTPUT_VARIABLE _drift_out
			ERROR_VARIABLE  _drift_out)
		if(NOT _drift_rc EQUAL 0)
			message(FATAL_ERROR
				"patchset DRIFT in profile ${_drift_profile}: a patch is MISSING from the build tree.\n"
				"${_drift_out}\n"
				"Regenerate first: cd ${_drift_ws} && west patch verify --profile ${_drift_profile} && west patch apply --profile ${_drift_profile} --roll-back\n"
				"(stash manual worktree edits first). Override (packaging/CI): -DDARLING_SKIP_DRIFT_GATE=ON")
		endif()
		# (2) every-build check: an always-out-of-date target in ALL so a plain
		#     `ninja` re-runs the gate even when CMake does not reconfigure.
		add_custom_target(drift_gate ALL
			COMMAND ${WEST_PROGRAM} patch status --profile ${_drift_profile} --strict
			WORKING_DIRECTORY "${_drift_ws}"
			COMMENT "dar-gax: verifying ${_drift_profile} patchset is not drifted"
			VERBATIM)
	else()
		message(WARNING
			"DARLING_SKIP_DRIFT_GATE is OFF but the drift gate could not run "
			"(west=${WEST_PROGRAM}, profile=${_drift_profile}, ws=${_drift_ws}); proceeding WITHOUT drift protection.")
	endif()
endif()
# --- end drift gate ----------------------------------------------------------

add_subdirectory(src)

# --- dar-gax: wire the drift gate to the deployable targets ------------------
# Make the binaries we actually build+deploy DEPEND on the always-out-of-date
# drift_gate, so a TARGETED `ninja darlingserver` / `ninja system_kernel` runs
# the gate first and fails on drift -- not only a bare `ninja`. Placed after
# add_subdirectory(src) so the targets exist; TARGET-guarded for component
# selections that exclude them.
if(TARGET drift_gate)
	foreach(_dg_target darlingserver system_kernel)
		if(TARGET ${_dg_target})
			add_dependencies(${_dg_target} drift_gate)
		endif()
	endforeach()
endif()

# Rootless bootstrap is a source-owned component, not a list maintained by
# external test runners. Keep each guest entrypoint paired with the CMake target
# that produces it; the runtime runner consumes the generated manifest and
# resolves its Mach-O library closure from the resulting build tree.
set(DARLING_ROOTLESS_BOOTSTRAP_TARGETS
	darling
	mldr
	launchd
	launchctl
	shellspawn
	vchroot
	system_loader
	darlingserver
	system_kernel
	objc
	resolv-darwin
	CoreFoundation
)
foreach(_bootstrap_target IN LISTS DARLING_ROOTLESS_BOOTSTRAP_TARGETS)
	if(NOT TARGET ${_bootstrap_target})
		message(FATAL_ERROR "rootless bootstrap target is unavailable: ${_bootstrap_target}")
	endif()
endforeach()
add_custom_target(rootless_bootstrap DEPENDS ${DARLING_ROOTLESS_BOOTSTRAP_TARGETS})
set(_rootless_bootstrap_resource_dir "${CMAKE_BINARY_DIR}/rootless-bootstrap-resources")
file(MAKE_DIRECTORY
	"${_rootless_bootstrap_resource_dir}/System/Library/LaunchDaemons"
)
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shellspawn/org.darlinghq.shellspawn.plist"
	"${_rootless_bootstrap_resource_dir}/System/Library/LaunchDaemons/org.darlinghq.shellspawn.plist"
	COPYONLY
)
file(GENERATE
	OUTPUT "${CMAKE_BINARY_DIR}/darling-rootless-bootstrap.json"
	CONTENT
"{
  \"schema\": 1,
  \"entrypoints\": [
    {\"target\": \"darling\", \"guest_path\": \"/bin/darling\", \"host_path\": \"$<TARGET_FILE:darling>\"},
    {\"target\": \"mldr\", \"guest_path\": \"/usr/libexec/darling/mldr\", \"host_path\": \"$<TARGET_FILE:mldr>\"},
    {\"target\": \"launchd\", \"guest_path\": \"/sbin/launchd\", \"host_path\": \"$<TARGET_FILE:launchd>\"},
    {\"target\": \"launchctl\", \"guest_path\": \"/bin/launchctl\", \"host_path\": \"$<TARGET_FILE:launchctl>\"},
    {\"target\": \"shellspawn\", \"guest_path\": \"/usr/libexec/shellspawn\", \"host_path\": \"$<TARGET_FILE:shellspawn>\"},
    {\"target\": \"vchroot\", \"guest_path\": \"/usr/libexec/darling/vchroot\", \"host_path\": \"$<TARGET_FILE:vchroot>\"},
    {\"target\": \"system_loader\", \"guest_path\": \"/usr/lib/dyld\", \"host_path\": \"$<TARGET_FILE:system_loader>\"},
    {\"target\": \"darlingserver\", \"guest_path\": \"/bin/darlingserver\", \"host_path\": \"$<TARGET_FILE:darlingserver>\"}
  ],
  \"resources\": [
    {\"target\": \"shellspawn_launchd_plist\", \"guest_path\": \"/System/Library/LaunchDaemons/org.darlinghq.shellspawn.plist\", \"host_path\": \"${_rootless_bootstrap_resource_dir}/System/Library/LaunchDaemons/org.darlinghq.shellspawn.plist\"}
  ]
}
"
)

install(DIRECTORY DESTINATION libexec/darling/private)
install(DIRECTORY DESTINATION libexec/darling/private/etc)
install(DIRECTORY DESTINATION libexec/darling/private/var)
install(DIRECTORY DESTINATION libexec/darling/private/tmp)
InstallSymlink(private/etc ${CMAKE_INSTALL_PREFIX}/libexec/darling/etc)
InstallSymlink(private/var ${CMAKE_INSTALL_PREFIX}/libexec/darling/var)

install(FILES etc/resolv.conf
	DESTINATION libexec/darling/private/etc)
InstallSymlink(/Volumes/SystemRoot/etc/machine-id ${CMAKE_INSTALL_PREFIX}/libexec/darling/private/etc/machine-id)
InstallSymlink(/Volumes/SystemRoot/etc/nsswitch.conf ${CMAKE_INSTALL_PREFIX}/libexec/darling/private/etc/nsswitch.conf)

install(DIRECTORY DESTINATION libexec/darling/Volumes)
install(DIRECTORY DESTINATION libexec/darling/Volumes/SystemRoot)
InstallSymlink(/ ${CMAKE_INSTALL_PREFIX}/libexec/darling/Volumes/DarlingEmulatedDrive)

install(DIRECTORY DESTINATION libexec/darling/proc)

install(DIRECTORY DESTINATION libexec/darling/var/root)
install(DIRECTORY DESTINATION libexec/darling/var/run)
InstallSymlink(/dev/log ${CMAKE_INSTALL_PREFIX}/libexec/darling/var/run/syslog)

install(DIRECTORY DESTINATION libexec/darling/usr)
install(DIRECTORY DESTINATION libexec/darling/usr/local)
install(DIRECTORY DESTINATION libexec/darling/usr/local/share)

InstallSymlink(/Volumes/SystemRoot/dev ${CMAKE_INSTALL_PREFIX}/libexec/darling/dev)
InstallSymlink(private/tmp ${CMAKE_INSTALL_PREFIX}/libexec/darling/tmp)

InstallSymlink(/proc/self/mounts ${CMAKE_INSTALL_PREFIX}/libexec/darling/private/etc/mtab)
#InstallSymlink(/Volumes/SystemRoot/etc/passwd ${CMAKE_INSTALL_PREFIX}/libexec/darling/private/etc/passwd)
#InstallSymlink(/Volumes/SystemRoot/etc/group ${CMAKE_INSTALL_PREFIX}/libexec/darling/private/etc/group)
InstallSymlink(/Volumes/SystemRoot/etc/localtime ${CMAKE_INSTALL_PREFIX}/libexec/darling/private/etc/localtime)

InstallSymlink(/Volumes/SystemRoot/usr/share/zoneinfo ${CMAKE_INSTALL_PREFIX}/libexec/darling/usr/share/zoneinfo)

install(DIRECTORY DESTINATION libexec/darling/Library/Preferences)

if(NOT DEBIAN_PACKAGING)
	install(CODE "execute_process(COMMAND bash ${DARLING_TOP_DIRECTORY}/tools/shutdown-user.sh)")
endif(NOT DEBIAN_PACKAGING)

add_custom_target(uninstall
	COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/uninstall
	COMMENT "Uninstall Darling and kernel module")
