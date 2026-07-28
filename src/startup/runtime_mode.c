#include "runtime_mode.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mode_error(char* error, size_t error_size, const char* message)
{
	if (error != NULL && error_size != 0)
		snprintf(error, error_size, "%s", message);
	return -1;
}

static int parse_legacy_bool(
	const char* name,
	const char* value,
	bool* present,
	bool* enabled,
	char* error,
	size_t error_size
)
{
	*present = value != NULL;
	*enabled = false;
	if (value == NULL)
		return 0;
	if (strcmp(value, "0") == 0)
		return 0;
	if (strcmp(value, "1") == 0) {
		*enabled = true;
		return 0;
	}

	if (error != NULL && error_size != 0)
		snprintf(error, error_size,
			"%s must be exactly 0 or 1 at the launcher compatibility boundary",
			name);
	return -1;
}

const char* darling_runtime_mode_name(enum darling_runtime_mode mode)
{
	switch (mode) {
	case DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY:
		return "privileged-overlay";
	case DARLING_RUNTIME_MODE_PRIVILEGED_COPY:
		return "privileged-copy";
	case DARLING_RUNTIME_MODE_PRIVILEGED_EUNION:
		return "privileged-eunion";
	case DARLING_RUNTIME_MODE_ROOTLESS_EUNION:
		return "rootless-eunion";
	default:
		return NULL;
	}
}

enum darling_runtime_mode darling_runtime_mode_parse(const char* value)
{
	if (value == NULL)
		return DARLING_RUNTIME_MODE_INVALID;
	if (strcmp(value, "privileged-overlay") == 0)
		return DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY;
	if (strcmp(value, "privileged-copy") == 0)
		return DARLING_RUNTIME_MODE_PRIVILEGED_COPY;
	if (strcmp(value, "privileged-eunion") == 0)
		return DARLING_RUNTIME_MODE_PRIVILEGED_EUNION;
	if (strcmp(value, "rootless-eunion") == 0)
		return DARLING_RUNTIME_MODE_ROOTLESS_EUNION;
	return DARLING_RUNTIME_MODE_INVALID;
}

bool darling_runtime_mode_is_rootless(enum darling_runtime_mode mode)
{
	return mode == DARLING_RUNTIME_MODE_ROOTLESS_EUNION;
}

bool darling_runtime_mode_uses_overlay(enum darling_runtime_mode mode)
{
	return mode == DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY;
}

bool darling_runtime_mode_uses_eunion(enum darling_runtime_mode mode)
{
	return mode == DARLING_RUNTIME_MODE_PRIVILEGED_EUNION ||
		mode == DARLING_RUNTIME_MODE_ROOTLESS_EUNION;
}

enum darling_runtime_mode darling_runtime_mode_default_for_host(void)
{
	/*
	 * Preserve the existing WSL1 no-overlay fallback, but select it once at
	 * the launcher boundary so every downstream process sees the same mode.
	 */
	if (getenv("WSLENV") != NULL && getenv("WSL_INTEROP") == NULL)
		return DARLING_RUNTIME_MODE_PRIVILEGED_COPY;
	return DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY;
}

int darling_runtime_mode_parse_cli(
	int argc,
	char* const argv[],
	struct darling_runtime_cli* cli,
	char* error,
	size_t error_size
)
{
	if (cli == NULL)
		return mode_error(error, error_size, "launcher CLI output is missing");

	memset(cli, 0, sizeof(*cli));
	cli->command_index = -1;

	for (int index = 1; index < argc; ++index) {
		if (strcmp(argv[index], "--") == 0) {
			cli->command_index = index + 1;
			break;
		}
		if (argv[index][0] != '-') {
			cli->command_index = index;
			break;
		}
		if (strcmp(argv[index], "--rootless") == 0) {
			if (cli->rootless)
				return mode_error(error, error_size,
					"--rootless was specified more than once");
			cli->rootless = true;
			continue;
		}
		if (strcmp(argv[index], "--help") == 0) {
			cli->show_help = true;
			continue;
		}
		if (strcmp(argv[index], "--version") == 0) {
			cli->show_version = true;
			continue;
		}
		if (error != NULL && error_size != 0)
			snprintf(error, error_size,
				"unknown launcher option (long options must be exact): %s",
				argv[index]);
		return -1;
	}
	return 0;
}

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
)
{
	bool rootless_present;
	bool nooverlay_present;
	bool eunion_present;
	bool rootless;
	bool nooverlay;
	bool eunion;
	enum darling_runtime_mode canonical_mode = DARLING_RUNTIME_MODE_INVALID;
	enum darling_runtime_mode legacy_mode = default_mode;
	bool legacy_present;

	if (mode == NULL)
		return mode_error(error, error_size, "runtime mode output is missing");
	*mode = DARLING_RUNTIME_MODE_INVALID;

	if (darling_runtime_mode_name(default_mode) == NULL)
		return mode_error(error, error_size, "default runtime mode is invalid");

	if (canonical != NULL) {
		canonical_mode = darling_runtime_mode_parse(canonical);
		if (canonical_mode == DARLING_RUNTIME_MODE_INVALID)
			return mode_error(error, error_size,
				"DARLING_RUNTIME_MODE contains an unknown runtime mode");
	}

	if (parse_legacy_bool("DARLING_ROOTLESS", legacy_rootless,
			&rootless_present, &rootless, error, error_size) != 0 ||
		parse_legacy_bool("DARLING_NOOVERLAYFS", legacy_nooverlay,
			&nooverlay_present, &nooverlay, error, error_size) != 0 ||
		parse_legacy_bool("DARLING_EUNION", legacy_eunion,
			&eunion_present, &eunion, error, error_size) != 0)
		return -1;

	legacy_present = rootless_present || nooverlay_present || eunion_present;
	if (legacy_present) {
		if (rootless) {
			if (!nooverlay || !eunion)
				return mode_error(error, error_size,
					"legacy rootless mode requires DARLING_ROOTLESS=1, "
					"DARLING_NOOVERLAYFS=1 and DARLING_EUNION=1");
			legacy_mode = DARLING_RUNTIME_MODE_ROOTLESS_EUNION;
		} else if (eunion && !nooverlay) {
			return mode_error(error, error_size,
				"DARLING_EUNION=1 conflicts with overlay selection");
		} else if (nooverlay && eunion) {
			legacy_mode = DARLING_RUNTIME_MODE_PRIVILEGED_EUNION;
		} else if (nooverlay) {
			legacy_mode = DARLING_RUNTIME_MODE_PRIVILEGED_COPY;
		} else {
			legacy_mode = DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY;
		}
	}

	if (cli_rootless) {
		if (canonical != NULL &&
			canonical_mode != DARLING_RUNTIME_MODE_ROOTLESS_EUNION)
			return mode_error(error, error_size,
				"--rootless conflicts with inherited DARLING_RUNTIME_MODE");
		if (legacy_present &&
			legacy_mode != DARLING_RUNTIME_MODE_ROOTLESS_EUNION)
			return mode_error(error, error_size,
				"--rootless conflicts with inherited legacy runtime flags");
		*mode = DARLING_RUNTIME_MODE_ROOTLESS_EUNION;
	} else if (canonical != NULL) {
		if (legacy_present && canonical_mode != legacy_mode)
			return mode_error(error, error_size,
				"DARLING_RUNTIME_MODE conflicts with inherited legacy runtime flags");
		*mode = canonical_mode;
	} else {
		*mode = legacy_present ? legacy_mode : default_mode;
	}

	if (darling_runtime_mode_uses_eunion(*mode) && !eunion_capable) {
		*mode = DARLING_RUNTIME_MODE_INVALID;
		return mode_error(error, error_size,
			"selected runtime mode requires an E-UNION-capable build");
	}
	return 0;
}

int darling_runtime_mode_select_process(
	const struct darling_runtime_cli* cli,
	bool eunion_capable,
	enum darling_runtime_mode* mode,
	char* error,
	size_t error_size
)
{
	if (cli == NULL)
		return mode_error(error, error_size, "parsed launcher CLI is missing");
	return darling_runtime_mode_resolve(
		cli->rootless,
		getenv(DARLING_RUNTIME_MODE_ENV),
		getenv("DARLING_ROOTLESS"),
		getenv("DARLING_NOOVERLAYFS"),
		getenv("DARLING_EUNION"),
		darling_runtime_mode_default_for_host(),
		eunion_capable,
		mode,
		error,
		error_size
	);
}

int darling_runtime_mode_publish(
	enum darling_runtime_mode mode,
	char* error,
	size_t error_size
)
{
	const char* name = darling_runtime_mode_name(mode);
	if (name == NULL)
		return mode_error(error, error_size, "cannot publish an invalid runtime mode");
	if (setenv(DARLING_RUNTIME_MODE_ENV, name, 1) != 0 ||
		unsetenv("DARLING_ROOTLESS") != 0 ||
		unsetenv("DARLING_NOOVERLAYFS") != 0 ||
		unsetenv("DARLING_EUNION") != 0) {
		if (error != NULL && error_size != 0)
			snprintf(error, error_size,
				"cannot publish canonical runtime mode: %s", strerror(errno));
		return -1;
	}
	return 0;
}

int darling_runtime_mode_require_canonical_process(
	bool eunion_capable,
	enum darling_runtime_mode* mode,
	char* error,
	size_t error_size
)
{
	if (getenv("DARLING_ROOTLESS") != NULL ||
		getenv("DARLING_NOOVERLAYFS") != NULL ||
		getenv("DARLING_EUNION") != NULL)
		return mode_error(error, error_size,
			"legacy runtime flags are forbidden after launcher normalization");

	const char* canonical = getenv(DARLING_RUNTIME_MODE_ENV);
	enum darling_runtime_mode parsed = darling_runtime_mode_parse(canonical);
	if (parsed == DARLING_RUNTIME_MODE_INVALID)
		return mode_error(error, error_size,
			"canonical DARLING_RUNTIME_MODE is missing or invalid");
	if (darling_runtime_mode_uses_eunion(parsed) && !eunion_capable)
		return mode_error(error, error_size,
			"canonical runtime mode requires an E-UNION-capable build");
	if (mode != NULL)
		*mode = parsed;
	return 0;
}
