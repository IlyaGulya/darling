# Extend the toolchain component only for the explicit Homebrew product fixture.
# Targets and guest paths follow the source repositories' normal install rules.
foreach(_homebrew_command IN ITEMS
	"cat|/bin/cat" "chmod|/bin/chmod" "cp|/bin/cp" "date|/bin/date"
	"echo|/bin/echo" "expr|/bin/expr" "kill|/bin/kill" "ln|/bin/ln"
	"ls|/bin/ls" "mkdir|/bin/mkdir" "mv|/bin/mv" "pwd|/bin/pwd"
	"rm|/bin/rm" "rmdir|/bin/rmdir" "sleep|/bin/sleep" "stat|/bin/stat"
	"stat|/bin/readlink" "test-bin|/bin/test" "grep|/bin/grep"
	"basename|/usr/bin/basename" "dirname|/usr/bin/dirname" "env|/usr/bin/env"
	"find|/usr/bin/find" "gzip|/usr/bin/gzip" "head|/usr/bin/head"
	"id|/usr/bin/id" "install-bin|/usr/bin/install" "locale|/usr/bin/locale"
	"mktemp|/usr/bin/mktemp" "printenv|/usr/bin/printenv" "printf|/usr/bin/printf"
	"sed|/usr/bin/sed" "sort|/usr/bin/sort" "tail|/usr/bin/tail" "tee|/usr/bin/tee"
	"touch|/usr/bin/touch" "tr|/usr/bin/tr" "uname|/usr/bin/uname"
	"uniq|/usr/bin/uniq" "wc|/usr/bin/wc" "which|/usr/bin/which" "xargs|/usr/bin/xargs"
	"awkexe|/usr/bin/awk" "cmp|/usr/bin/cmp" "diff|/usr/bin/diff"
	"bsdtar|/usr/bin/tar" "curlexe|/usr/bin/curl" "sw_vers|/usr/bin/sw_vers"
	"sysctl|/usr/sbin/sysctl" "xcrun|/usr/bin/xcrun" "xcode-select|/usr/bin/xcode-select"
	"git_shim|/usr/bin/git" "make_shim|/usr/bin/make" "ar_shim|/usr/bin/ar"
	"clang_shim|/usr/bin/clang" "clang++_shim|/usr/bin/clang++"
	"lipo_shim|/usr/bin/lipo" "otool_shim|/usr/bin/otool" "nm_shim|/usr/bin/nm"
	"strip_shim|/usr/bin/strip" "install_name_tool_shim|/usr/bin/install_name_tool"
	"ffi|/usr/lib/libffi.dylib"
	"ranlib_shim|/usr/bin/ranlib" "dsymutil_shim|/usr/bin/dsymutil")
	string(REPLACE "|" ";" _homebrew_parts "${_homebrew_command}")
	list(GET _homebrew_parts 0 _homebrew_target)
	list(GET _homebrew_parts 1 _homebrew_guest_path)
	if(NOT TARGET ${_homebrew_target})
		message(FATAL_ERROR "rootless Homebrew target is unavailable: ${_homebrew_target}")
	endif()
	add_dependencies(rootless_toolchain ${_homebrew_target})
	string(APPEND _rootless_toolchain_extra_entrypoints
		",\n    {\"target\": \"${_homebrew_target}\", \"guest_path\": \"${_homebrew_guest_path}\", \"host_path\": \"$<TARGET_FILE:${_homebrew_target}>\"}")
endforeach()

set(_homebrew_resources
	"src/frameworks/CoreServices/SystemVersion.plist|/System/Library/CoreServices/SystemVersion.plist"
	"src/frameworks/CoreServices/SystemVersionCompat.plist|/System/Library/CoreServices/SystemVersionCompat.plist"
	"src/sandbox/sandbox-exec.sh|/usr/bin/sandbox-exec"
	"src/external/libressl-2.8.3/apps/openssl/cert.pem|/private/etc/ssl/cert.pem")
set(_homebrew_locale_source "src/external/libc/darling/assets/locale")
foreach(_homebrew_category IN ITEMS
	LC_COLLATE LC_CTYPE LC_MESSAGES/LC_MESSAGES LC_MONETARY LC_NUMERIC LC_TIME)
	list(APPEND _homebrew_resources
		"${_homebrew_locale_source}/en_US.UTF-8/${_homebrew_category}|/usr/share/locale/en_US.UTF-8/${_homebrew_category}")
endforeach()
foreach(_homebrew_resource IN LISTS _homebrew_resources)
	string(REPLACE "|" ";" _homebrew_parts "${_homebrew_resource}")
	list(GET _homebrew_parts 0 _homebrew_source)
	list(GET _homebrew_parts 1 _homebrew_guest_path)
	set(_homebrew_host_path "${_rootless_toolchain_resource_dir}${_homebrew_guest_path}")
	get_filename_component(_homebrew_parent "${_homebrew_host_path}" DIRECTORY)
	file(MAKE_DIRECTORY "${_homebrew_parent}")
	configure_file("${CMAKE_SOURCE_DIR}/${_homebrew_source}" "${_homebrew_host_path}" COPYONLY)
	string(APPEND _rootless_toolchain_extra_resources
		",\n    {\"target\": \"${_homebrew_source}\", \"guest_path\": \"${_homebrew_guest_path}\", \"host_path\": \"${_homebrew_host_path}\"}")
endforeach()
