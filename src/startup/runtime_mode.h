#ifndef DARLING_RUNTIME_MODE_H
#define DARLING_RUNTIME_MODE_H

#include <stdbool.h>
#include <stddef.h>

#define DARLING_RUNTIME_MODE_ENV "DARLING_RUNTIME_MODE"
#define DARLING_RUNTIME_MODE_MARKER_NAME ".darling-runtime-mode-v1"
#define DARLING_RUNTIME_MODE_MARKER_VERSION 1

#ifndef DARLING_RUNTIME_EUNION_CAPABLE
#define DARLING_RUNTIME_EUNION_CAPABLE 0
#endif

enum darling_runtime_mode {
	DARLING_RUNTIME_MODE_INVALID = 0,
	DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY,
	DARLING_RUNTIME_MODE_PRIVILEGED_COPY,
	DARLING_RUNTIME_MODE_PRIVILEGED_EUNION,
	DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
};

struct darling_runtime_cli {
	bool rootless;
	bool show_help;
	bool show_version;
	bool confirm_prefix_recreate;
	int command_index;
};

const char* darling_runtime_mode_name(enum darling_runtime_mode mode);
enum darling_runtime_mode darling_runtime_mode_parse(const char* value);
bool darling_runtime_mode_is_rootless(enum darling_runtime_mode mode);
bool darling_runtime_mode_uses_overlay(enum darling_runtime_mode mode);
bool darling_runtime_mode_uses_eunion(enum darling_runtime_mode mode);
enum darling_runtime_mode darling_runtime_mode_default_for_host(void);

int darling_runtime_mode_parse_cli(
	int argc,
	char* const argv[],
	struct darling_runtime_cli* cli,
	char* error,
	size_t error_size
);

int darling_runtime_mode_resolve(
	bool cli_rootless,
	const char* canonical,
	const char* legacy_rootless,
	const char* legacy_nooverlay,
	const char* legacy_eunion,
	enum darling_runtime_mode default_mode,
	bool eunion_capable,
	enum darling_runtime_mode* mode,
	char* error,
	size_t error_size
);

int darling_runtime_mode_select_process(
	const struct darling_runtime_cli* cli,
	bool eunion_capable,
	enum darling_runtime_mode* mode,
	char* error,
	size_t error_size
);

int darling_runtime_mode_publish(
	enum darling_runtime_mode mode,
	char* error,
	size_t error_size
);

int darling_runtime_mode_require_canonical_process(
	bool eunion_capable,
	enum darling_runtime_mode* mode,
	char* error,
	size_t error_size
);

#endif
