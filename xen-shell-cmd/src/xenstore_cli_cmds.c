/*
 * Copyright (c) 2025 TOKITA Hiroshi
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/shell/shell.h>

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <xen/public/io/xs_wire.h>
#include <xenstore_cli.h>
#include <xenstore_common.h>

LOG_MODULE_REGISTER(xenstore_shell, CONFIG_LOG_DEFAULT_LEVEL);

#define OPT_HELP BIT(0)
#define OPT_PREFIX BIT(1)
#define OPT_FULL_PATH BIT(2)
#define OPT_RAW BIT(3)
#define OPT_TIDY BIT(4)
#define OPT_RECURSE BIT(5)
#define OPT_UPTO BIT(6)

#define XENSTORE_LS_P_PADDING_END 60
#define XENSTORE_LS_MAX_INDENT 64
#define XENSTORE_SHELL_REQ_TIMEOUT K_SECONDS(5)
#define XENSTORE_SHELL_WATCH_TIMEOUT K_SECONDS(30)

struct cmd_options {
	/* Parsed OPT_* bits accepted by the current subcommand. */
	uint32_t flags;
	/* First argv index that belongs to the subcommand payload, not options. */
	size_t idx;
	/* Number of watch events requested through -n before watch returns. */
	size_t watch_count;
	/* True when -n was provided; otherwise watch runs until interrupted. */
	bool watch_count_set;
};

struct cmd_xenstore_watcher {
	/* Client-library watcher object registered once and reused by the shell command. */
	struct xs_watcher watcher;
	/* Signals the blocking shell command when enough watch events arrive. */
	struct k_sem sem;
	/* Shell instance used by the callback to print watch notifications. */
	const struct shell *sh;
	/* Remaining events before the current watch command can complete. */
	size_t count;
	/* True after the reusable watcher is registered in the client library. */
	bool registered;
};

/* Return an indentation suffix from a static space buffer for recursive ls output. */
static const char *space_string(size_t len)
{
	static const char spaces[XENSTORE_LS_MAX_INDENT + 1U] =
		"                                                                ";

	if (len >= XENSTORE_LS_MAX_INDENT) {
		return spaces;
	}

	return spaces + (XENSTORE_LS_MAX_INDENT - len);
}

/* Iterate over XenStore's NUL-separated directory response payload. */
static const char *xenstore_next_str(const char *current, const char *buf, size_t len)
{
	ptrdiff_t idx;

	if ((buf == NULL) || (len == 0)) {
		return NULL;
	}

	if (current == NULL) {
		return buf;
	}

	idx = current - buf;
	if ((idx < 0) || ((size_t)idx >= len)) {
		return NULL;
	}

	for (size_t i = ((size_t)idx) + 1U; i < len; i++) {
		if ((buf[i] == '\0') && ((i + 1U) < len)) {
			return &buf[i + 1U];
		}
	}

	return NULL;
}

/*
 * Parse Linux-like short options, handle -h, and verify the required number
 * of positional arguments. When @p handled is true, the command already
 * printed help successfully and should return the helper's return value.
 */
static int parse_command_args(const struct shell *sh, size_t argc, char **argv,
			      const char *allowed, size_t min_args,
			      struct cmd_options *options, bool *handled)
{
	size_t idx = 1;

	*handled = false;
	options->flags = 0;
	options->idx = 1;
	options->watch_count = 0;
	options->watch_count_set = false;

	while (idx < argc) {
		const char *arg = argv[idx];

		if (arg[0] != '-' || arg[1] == '\0') {
			break;
		}
		if (strcmp(arg, "--") == 0) {
			idx++;
			break;
		}

		for (size_t pos = 1; arg[pos] != '\0'; pos++) {
			char opt = arg[pos];

			if (strchr(allowed, opt) == NULL) {
				shell_error(sh, "unknown option: -%c", opt);
				return -EINVAL;
			}

			switch (opt) {
			case 'h':
				options->flags |= OPT_HELP;
				break;
			case 'p':
				options->flags |= OPT_PREFIX;
				break;
			case 'f':
				options->flags |= OPT_FULL_PATH;
				break;
			case 'R':
				options->flags |= OPT_RAW;
				break;
			case 't':
				options->flags |= OPT_TIDY;
				break;
			case 'r':
				options->flags |= OPT_RECURSE;
				break;
			case 'u':
				options->flags |= OPT_UPTO;
				break;
			case 'n':
				options->watch_count_set = true;
				if (arg[pos + 1] != '\0') {
					options->watch_count = strtoul(&arg[pos + 1], NULL, 10);
					pos = strlen(arg) - 1U;
				} else {
					if (++idx == argc) {
						shell_error(sh, "missing argument for -n");
						return -EINVAL;
					}
					options->watch_count = strtoul(argv[idx], NULL, 10);
				}
				break;
			default:
				return -EINVAL;
			}
		}

		idx++;
	}

	options->idx = idx;

	if (options->flags & OPT_HELP) {
		shell_help(sh);
		*handled = true;
		return 0;
	}

	if ((argc - options->idx) < min_args) {
		shell_help(sh);
		return -EINVAL;
	}

	return 0;
}

/* Validate a user-supplied XenStore path before sending it to the client API. */
static int check_path_arg(const struct shell *sh, const char *path)
{
	if (!xenstore_is_abs_path(path)) {
		shell_error(sh, "path must be absolute: %s", path ? path : "(null)");
		return -EINVAL;
	}
	if (strlen(path) > XENSTORE_ABS_PATH_MAX) {
		shell_error(sh, "path too long: %s", path);
		return -ENAMETOOLONG;
	}

	return 0;
}

/* Join a parent XenStore path and a child name without creating a double slash at root. */
static bool path_join(char *dst, size_t dst_len, const char *parent, const char *child)
{
	int ret;

	if (strcmp(parent, "/") == 0) {
		ret = snprintf(dst, dst_len, "/%s", child);
	} else {
		ret = snprintf(dst, dst_len, "%s/%s", parent, child);
	}

	return (ret >= 0) && ((size_t)ret < dst_len);
}

/* Copy the direct parent path used by rm -t and chmod -u walks. */
static bool get_parent_path(const char *path, char *parent, size_t parent_size)
{
	size_t len;
	size_t last;

	if ((path == NULL) || (parent == NULL) || (parent_size == 0)) {
		return false;
	}

	len = strlen(path);
	while ((len > 1U) && (path[len - 1U] == '/')) {
		len--;
	}
	if (len <= 1U) {
		return false;
	}

	last = len;
	while ((last > 0U) && (path[last - 1U] != '/')) {
		last--;
	}

	if (last == 0U) {
		return false;
	}
	if (last == 1U) {
		if (parent_size < 2U) {
			return false;
		}
		parent[0] = '/';
		parent[1] = '\0';
		return true;
	}
	if (last > parent_size) {
		return false;
	}

	memcpy(parent, path, last - 1U);
	parent[last - 1U] = '\0';

	return true;
}

/* Decode write values from CLI text form into the byte payload sent to XenStore. */
static int hex_nibble(char c)
{
	if ((c >= '0') && (c <= '9')) {
		return c - '0';
	}
	if ((c >= 'a') && (c <= 'f')) {
		return c - 'a' + 10;
	}
	if ((c >= 'A') && (c <= 'F')) {
		return c - 'A' + 10;
	}

	return -EINVAL;
}

/*
 * Decode the default xenstore write argument format.
 *
 * Shell users can pass byte values as \xNN escape sequences when they do not
 * request raw mode with -R. The decoded byte count is returned through out_len
 * so xs_write() can store embedded NUL bytes instead of stopping at the first
 * string terminator.
 */
static int unescape_value(const char *value, char *buf, size_t buf_len, size_t *out_len)
{
	size_t src = 0;
	size_t dst = 0;

	while (value[src] != '\0') {
		if (dst >= buf_len) {
			return -ENOSPC;
		}

		if ((value[src] == '\\') && (value[src + 1U] == 'x')) {
			int hi = hex_nibble(value[src + 2U]);
			int lo = hex_nibble(value[src + 3U]);

			if ((hi < 0) || (lo < 0)) {
				return -EINVAL;
			}

			buf[dst++] = (char)((hi << 4) | lo);
			src += 4U;
		} else {
			buf[dst++] = value[src++];
		}
	}

	*out_len = dst;

	return 0;
}

/* Print XenStore values using Linux-like escaped output unless raw mode is requested. */
static void print_value(const struct shell *sh, const char *buf, size_t len, bool raw)
{
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)buf[i];

		if (raw || isprint(c)) {
			shell_fprintf_normal(sh, "%c", c);
		} else {
			shell_fprintf_normal(sh, "\\x%02x", c);
		}
	}

	shell_fprintf_normal(sh, "\n");
}

/* Convert a typed permission into the character form printed by Linux xenstore-ls. */
static int permission_to_char(enum xs_permission perm, char *ch)
{
	if (ch == NULL) {
		return -EINVAL;
	}

	switch (perm) {
	case XS_PERMISSION_NONE:
		*ch = 'n';
		return 0;
	case XS_PERMISSION_READ:
		*ch = 'r';
		return 0;
	case XS_PERMISSION_WRITE:
		*ch = 'w';
		return 0;
	case XS_PERMISSION_READ_WRITE:
		*ch = 'b';
		return 0;
	default:
		return -EINVAL;
	}
}

/* Parse the first character from a Linux-style chmod mode such as b1 or r0. */
static int permission_from_char(char ch, enum xs_permission *perm)
{
	if (perm == NULL) {
		return -EINVAL;
	}

	switch (ch) {
	case 'n':
		*perm = XS_PERMISSION_NONE;
		return 0;
	case 'r':
		*perm = XS_PERMISSION_READ;
		return 0;
	case 'w':
		*perm = XS_PERMISSION_WRITE;
		return 0;
	case 'b':
		*perm = XS_PERMISSION_READ_WRITE;
		return 0;
	default:
		return -EINVAL;
	}
}

/* Parse one chmod mode string such as b1, r0, w2, or n3. */
static int parse_permission(const char *text, struct xs_permission_entry *entry)
{
	char *end;
	int ret;

	if ((text == NULL) || (text[0] == '\0') || (text[1] == '\0')) {
		return -EINVAL;
	}

	ret = permission_from_char(text[0], &entry->perm);
	if (ret < 0) {
		return ret;
	}

	entry->domid = strtoul(&text[1], &end, 10);
	if (*end != '\0') {
		return -EINVAL;
	}

	return 0;
}

/* Format XenStore permission entries into the comma-separated form printed by ls -p. */
static int get_perms(const struct shell *sh, const char *path, char *perm_buffer, size_t len)
{
	struct xs_permission_entry perms[XS_SET_PERMS_MAX_ENTRIES];
	ssize_t ret;
	size_t off = 0;

	ret = xs_get_permissions_timeout(path, perms, ARRAY_SIZE(perms), XS_TRANSACTION_NONE,
					 XENSTORE_SHELL_REQ_TIMEOUT);
	if (ret < 0) {
		shell_warn(sh, "get_perms %s: %ld", path, ret);
		return (int)ret;
	}

	for (ssize_t i = 0; i < ret; i++) {
		char wire_perm = '?';
		int written;

		(void)permission_to_char(perms[i].perm, &wire_perm);
		written = snprintf(&perm_buffer[off], len - off, "%s%c%u", (i == 0) ? "" : ",",
				   wire_perm, perms[i].domid);
		if ((written < 0) || ((size_t)written >= (len - off))) {
			return -ENOSPC;
		}
		off += written;
	}

	return 0;
}

/* Initialize the DomU XenStore client transport from the shell. */
static int cmd_xenstore_init(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return xs_init();
}

/* Implement xenstore list by printing direct children of each requested path. */
static int cmd_xenstore_list(const struct shell *sh, size_t argc, char **argv)
{
	struct cmd_options options;
	char *buffer;
	bool handled;
	int rc = 0;
	int ret;

	ret = parse_command_args(sh, argc, argv, "hp", 1, &options, &handled);
	if ((ret < 0) || handled) {
		return ret;
	}

	buffer = k_malloc(XENSTORE_PAYLOAD_MAX + 1U);
	if (buffer == NULL) {
		shell_error(sh, "failed to allocate buffer");
		return -ENOMEM;
	}

	for (size_t idx = options.idx; idx < argc; idx++) {
		const char *path = argv[idx];
		const char *prefix;
		const char *ptr = NULL;
		ssize_t resp_len;

		ret = check_path_arg(sh, path);
		if (ret < 0) {
			rc = (rc == 0) ? ret : rc;
			continue;
		}

		prefix = (strcmp(path, "/") == 0) ? "" : path;
		resp_len = xs_directory_timeout(path, buffer, XENSTORE_PAYLOAD_MAX,
						XS_TRANSACTION_NONE,
						XENSTORE_SHELL_REQ_TIMEOUT);
		if (resp_len < 0) {
			shell_error(sh, "xs_directory: %ld: %s", resp_len, path);
			rc = (rc == 0) ? (int)resp_len : rc;
			continue;
		}

		buffer[resp_len] = '\0';
		while ((ptr = xenstore_next_str(ptr, buffer, resp_len))) {
			if (options.flags & OPT_PREFIX) {
				shell_print(sh, "%s/%s", prefix, ptr);
			} else {
				shell_print(sh, "%s", ptr);
			}
		}
	}

	k_free(buffer);

	return rc;
}

/* Recursively render xenstore ls output, optionally including full paths and permissions. */
static int cmd_xenstore_ls_recur(const struct shell *sh, size_t level, const char *path,
				 bool show_path, bool show_perms)
{
	char *buffer = NULL;
	char *path_buf = NULL;
	char *read_buffer = NULL;
	char *perm_buffer = NULL;
	const char *ptr = NULL;
	ssize_t resp_len;
	int ret = 0;

	buffer = k_malloc(XENSTORE_PAYLOAD_MAX + 1U);
	path_buf = k_malloc(XENSTORE_ABS_PATH_MAX + 1U);
	read_buffer = k_malloc(XENSTORE_PAYLOAD_MAX + 1U);

	if ((buffer == NULL) || (path_buf == NULL) || (read_buffer == NULL)) {
		shell_error(sh, "alloc buffer");
		ret = -ENOMEM;
		goto cleanup;
	}

	resp_len = xs_directory_timeout(path, buffer, XENSTORE_PAYLOAD_MAX, XS_TRANSACTION_NONE,
					XENSTORE_SHELL_REQ_TIMEOUT);
	if (resp_len < 0) {
		if (level != 0U) {
			ret = 0;
		} else {
			shell_error(sh, "xs_directory: %ld: %s", resp_len, path);
			ret = (int)resp_len;
		}
		goto cleanup;
	}

	buffer[resp_len] = '\0';
	while ((ptr = xenstore_next_str(ptr, buffer, resp_len))) {
		ssize_t read_len;
		const char *perms_display = NULL;
		int child_ret;

		if (!path_join(path_buf, XENSTORE_ABS_PATH_MAX + 1U, path, ptr)) {
			shell_warn(sh, "path truncated: %s/%s", path, ptr);
			continue;
		}

		read_len = xs_read_timeout(path_buf, read_buffer, XENSTORE_PAYLOAD_MAX,
					   XS_TRANSACTION_NONE, XENSTORE_SHELL_REQ_TIMEOUT);
		if (read_len < 0) {
			read_buffer[0] = '\0';
		} else {
			read_buffer[read_len] = '\0';
		}

		if (show_perms) {
			if (perm_buffer == NULL) {
				perm_buffer = k_malloc(XENSTORE_PAYLOAD_MAX + 1U);
				if (perm_buffer == NULL) {
					shell_error(sh, "unable to allocate permissions buffer");
					ret = -ENOMEM;
					goto cleanup;
				}
			}

			ret = get_perms(sh, path_buf, perm_buffer, XENSTORE_PAYLOAD_MAX + 1U);
			if (ret < 0) {
				perm_buffer[0] = '\0';
			}
			perms_display = perm_buffer;
		}

		if (show_path) {
			if (show_perms) {
				shell_print(sh, "%s = \"%s\"   (%s)", path_buf, read_buffer,
					    perms_display);
			} else {
				shell_print(sh, "%s = \"%s\"", path_buf, read_buffer);
			}
		} else if (show_perms) {
			const char *indent = space_string(level);
			size_t wlen = strlen(indent) + strlen(ptr) + strlen(read_buffer) + 4U;

			shell_fprintf_normal(sh, "%s%s = \"%s\"", indent, ptr, read_buffer);
			if (wlen < XENSTORE_LS_P_PADDING_END) {
				if (wlen % 2U) {
					shell_fprintf_normal(sh, "%s", " ");
					wlen += 1U;
				}
				while ((XENSTORE_LS_P_PADDING_END - wlen) > 0U) {
					shell_fprintf_normal(sh, "%s", " .");
					wlen += 2U;
				}
			}
			shell_fprintf_normal(sh, "  (%s)\n", perms_display);
		} else {
			shell_print(sh, "%s%s = \"%s\"", space_string(level), ptr, read_buffer);
		}

		child_ret = cmd_xenstore_ls_recur(sh, level + 1U, path_buf, show_path, show_perms);
		if ((child_ret < 0) && (ret == 0)) {
			ret = child_ret;
		}
	}

cleanup:
	k_free(perm_buffer);
	k_free(read_buffer);
	k_free(path_buf);
	k_free(buffer);

	return ret;
}

/* Implement xenstore ls; without an explicit path it mirrors Linux and starts at root. */
static int cmd_xenstore_ls(const struct shell *sh, size_t argc, char **argv)
{
	struct cmd_options options;
	bool handled;
	int rc = 0;
	int ret;

	ret = parse_command_args(sh, argc, argv, "hfp", 0, &options, &handled);
	if ((ret < 0) || handled) {
		return ret;
	}

	if (options.idx == argc) {
		return cmd_xenstore_ls_recur(sh, 0, "/", options.flags & OPT_FULL_PATH,
					     options.flags & OPT_PREFIX);
	}

	for (size_t idx = options.idx; idx < argc; idx++) {
		int ret = check_path_arg(sh, argv[idx]);

		if (ret == 0) {
			ret = cmd_xenstore_ls_recur(sh, 0, argv[idx], options.flags & OPT_FULL_PATH,
						   options.flags & OPT_PREFIX);
		}

		if ((ret < 0) && (rc == 0)) {
			rc = ret;
		}
	}

	return rc;
}

/* Implement xenstore read for one or more paths with optional path prefixes and raw bytes. */
static int cmd_xenstore_read(const struct shell *sh, size_t argc, char **argv)
{
	struct cmd_options options;
	char *buffer;
	bool handled;
	int rc = 0;
	int ret;

	ret = parse_command_args(sh, argc, argv, "hpR", 1, &options, &handled);
	if ((ret < 0) || handled) {
		return ret;
	}

	buffer = k_malloc(XENSTORE_PAYLOAD_MAX + 1U);
	if (buffer == NULL) {
		shell_error(sh, "failed to allocate buffer");
		return -ENOMEM;
	}

	for (size_t idx = options.idx; idx < argc; idx++) {
		const char *path = argv[idx];
		ssize_t resp_len;

		ret = check_path_arg(sh, path);
		if (ret < 0) {
			rc = (rc == 0) ? ret : rc;
			continue;
		}

		resp_len = xs_read_timeout(path, buffer, XENSTORE_PAYLOAD_MAX, XS_TRANSACTION_NONE,
					   XENSTORE_SHELL_REQ_TIMEOUT);
		if (resp_len < 0) {
			shell_error(sh, "xs_read: %s: %ld", path, resp_len);
			rc = (rc == 0) ? (int)resp_len : rc;
			continue;
		}

		if (options.flags & OPT_PREFIX) {
			shell_fprintf_normal(sh, "%s: ", path);
		}
		print_value(sh, buffer, resp_len, options.flags & OPT_RAW);
	}

	k_free(buffer);

	return rc;
}

/* Implement xenstore write for key/value pairs using escaped values by default. */
static int cmd_xenstore_write(const struct shell *sh, size_t argc, char **argv)
{
	struct cmd_options options;
	char *response;
	char *value_buf;
	bool handled;
	int rc = 0;
	int ret;

	ret = parse_command_args(sh, argc, argv, "hR", 2, &options, &handled);
	if ((ret < 0) || handled) {
		return ret;
	}
	if (((argc - options.idx) % 2U) != 0U) {
		shell_error(sh, "invalid argument pair");
		return -EINVAL;
	}

	response = k_malloc(XENSTORE_PAYLOAD_MAX + 1U);
	value_buf = k_malloc(XENSTORE_PAYLOAD_MAX + 1U);
	if ((response == NULL) || (value_buf == NULL)) {
		k_free(value_buf);
		k_free(response);
		shell_error(sh, "failed to allocate buffer");
		return -ENOMEM;
	}

	for (size_t idx = options.idx; idx < argc; idx += 2U) {
		const char *path = argv[idx];
		const char *value = argv[idx + 1U];
		const char *write_value = value;
		size_t write_len = strlen(value);
		ssize_t resp_len;

		ret = check_path_arg(sh, path);
		if (ret < 0) {
			rc = (rc == 0) ? ret : rc;
			continue;
		}

		if (!(options.flags & OPT_RAW)) {
			int ret = unescape_value(value, value_buf, XENSTORE_PAYLOAD_MAX, &write_len);

			if (ret < 0) {
				shell_error(sh, "invalid escaped value: %s", value);
				rc = (rc == 0) ? ret : rc;
				continue;
			}
			write_value = value_buf;
		}

		resp_len = xs_write_timeout(path, write_value, write_len, response,
					    XENSTORE_PAYLOAD_MAX, XS_TRANSACTION_NONE,
					    XENSTORE_SHELL_REQ_TIMEOUT);
		if (resp_len < 0) {
			shell_error(sh, "xs_write: %s: %ld", path, resp_len);
			rc = (rc == 0) ? (int)resp_len : rc;
		}
	}

	k_free(value_buf);
	k_free(response);

	return rc;
}

/* Remove one key and, for -t, remove newly-empty parent directories up to root. */
static int cmd_xenstore_rm_one(const struct shell *sh, const char *path, bool tidy)
{
	char *current_path = NULL;
	char *parent_path = NULL;
	char *buffer;
	size_t path_len;
	ssize_t ret;

	ret = check_path_arg(sh, path);
	if (ret < 0) {
		return ret;
	}

	buffer = k_malloc(XENSTORE_PAYLOAD_MAX + 1U);
	if (buffer == NULL) {
		shell_error(sh, "failed to allocate buffer");
		return -ENOMEM;
	}

	ret = xs_rm_timeout(path, buffer, XENSTORE_PAYLOAD_MAX, XS_TRANSACTION_NONE,
			    XENSTORE_SHELL_REQ_TIMEOUT);
	if ((ret < 0) || !tidy) {
		if (ret < 0) {
			shell_error(sh, "xs_rm: %ld %s", ret, path);
		}
		k_free(buffer);
		return (int)ret;
	}

	path_len = strlen(path) + 1U;
	current_path = k_malloc(path_len);
	parent_path = k_malloc(path_len);
	if ((current_path == NULL) || (parent_path == NULL)) {
		shell_error(sh, "failed to allocate path buffer");
		ret = -ENOMEM;
		goto end;
	}

	memcpy(current_path, path, path_len);
	while (get_parent_path(current_path, parent_path, path_len)) {
		const ssize_t resp_len = xs_directory_timeout(parent_path, buffer,
							      XENSTORE_PAYLOAD_MAX,
							      XS_TRANSACTION_NONE,
							      XENSTORE_SHELL_REQ_TIMEOUT);

		if ((resp_len < 0) || (resp_len != 0)) {
			break;
		}

		ret = xs_rm_timeout(parent_path, buffer, XENSTORE_PAYLOAD_MAX,
				    XS_TRANSACTION_NONE, XENSTORE_SHELL_REQ_TIMEOUT);
		if (ret < 0) {
			break;
		}

		memcpy(current_path, parent_path, path_len);
	}

end:
	k_free(current_path);
	k_free(parent_path);
	k_free(buffer);

	return (int)ret;
}

/* Implement xenstore rm across one or more requested paths. */
static int cmd_xenstore_rm(const struct shell *sh, size_t argc, char **argv)
{
	struct cmd_options options;
	bool handled;
	int rc = 0;
	int ret;

	ret = parse_command_args(sh, argc, argv, "ht", 1, &options, &handled);
	if ((ret < 0) || handled) {
		return ret;
	}

	for (size_t idx = options.idx; idx < argc; idx++) {
		int ret = cmd_xenstore_rm_one(sh, argv[idx], options.flags & OPT_TIDY);

		if ((ret < 0) && (rc == 0)) {
			rc = ret;
		}
	}

	return rc;
}

/* Apply chmod to a path and, for -r, recursively to all readable children. */
static ssize_t cmd_xenstore_chmod_recur(const struct shell *sh, char *path_buf,
					const struct xs_permission_entry *perms, size_t perms_num,
					bool recursive)
{
	char *buffer;
	char *child_path;
	const char *ptr = NULL;
	ssize_t resp_len;
	int ret;

	ret = check_path_arg(sh, path_buf);
	if (ret < 0) {
		return ret;
	}

	ret = xs_set_permissions_timeout(path_buf, perms, perms_num, XS_TRANSACTION_NONE,
					 XENSTORE_SHELL_REQ_TIMEOUT);
	if (ret < 0) {
		shell_error(sh, "xs_set_permissions %d %s", ret, path_buf);
		return ret;
	}

	if (!recursive) {
		return 0;
	}

	buffer = k_malloc(XENSTORE_PAYLOAD_MAX + 1U);
	child_path = k_malloc(XENSTORE_ABS_PATH_MAX + 1U);
	if ((buffer == NULL) || (child_path == NULL)) {
		shell_error(sh, "alloc buffer");
		k_free(child_path);
		k_free(buffer);
		return -ENOMEM;
	}

	resp_len = xs_directory_timeout(path_buf, buffer, XENSTORE_PAYLOAD_MAX,
					XS_TRANSACTION_NONE, XENSTORE_SHELL_REQ_TIMEOUT);
	if (resp_len < 0) {
		k_free(buffer);
		return 0;
	}

	buffer[resp_len] = '\0';
	while ((ptr = xenstore_next_str(ptr, buffer, resp_len))) {
		if (!path_join(child_path, XENSTORE_ABS_PATH_MAX + 1U, path_buf, ptr)) {
			shell_warn(sh, "path truncated");
			continue;
		}

		ret = cmd_xenstore_chmod_recur(sh, child_path, perms, perms_num, recursive);
		if (ret < 0) {
			break;
		}
	}

	k_free(child_path);
	k_free(buffer);

	return ret;
}

/* Apply chmod -u by walking upward from the target path to its parents. */
static int cmd_xenstore_chmod_upto(const struct shell *sh, const char *path,
				   const struct xs_permission_entry *perms, size_t perms_num)
{
	char *current;
	char *parent;
	int ret = 0;

	ret = check_path_arg(sh, path);
	if (ret < 0) {
		return ret;
	}

	current = k_malloc(XENSTORE_ABS_PATH_MAX + 1U);
	parent = k_malloc(XENSTORE_ABS_PATH_MAX + 1U);
	if ((current == NULL) || (parent == NULL)) {
		k_free(current);
		k_free(parent);
		return -ENOMEM;
	}

	memcpy(current, path, strlen(path) + 1U);
	while (get_parent_path(current, parent, XENSTORE_ABS_PATH_MAX + 1U)) {
		ret = xs_set_permissions_timeout(parent, perms, perms_num, XS_TRANSACTION_NONE,
						 XENSTORE_SHELL_REQ_TIMEOUT);
		if (ret < 0) {
			shell_error(sh, "xs_set_permissions %d %s", ret, parent);
			break;
		}
		memcpy(current, parent, strlen(parent) + 1U);
	}

	k_free(current);
	k_free(parent);

	return ret;
}

/* Parse chmod modes and apply them to the requested path, optionally recursively/upward. */
static int cmd_xenstore_chmod(const struct shell *sh, size_t argc, char **argv)
{
	struct xs_permission_entry perms[XS_SET_PERMS_MAX_ENTRIES];
	struct cmd_options options;
	char *path_buf;
	size_t perms_num;
	size_t path_len;
	bool handled;
	int ret;

	ret = parse_command_args(sh, argc, argv, "hru", 2, &options, &handled);
	if ((ret < 0) || handled) {
		return ret;
	}

	ret = check_path_arg(sh, argv[options.idx]);
	if (ret < 0) {
		return ret;
	}

	perms_num = argc - options.idx - 1U;
	if (perms_num > ARRAY_SIZE(perms)) {
		shell_error(sh, "too many permission entries");
		return -E2BIG;
	}

	for (size_t i = 0; i < perms_num; i++) {
		ret = parse_permission(argv[options.idx + 1U + i], &perms[i]);
		if (ret < 0) {
			shell_error(sh, "invalid permission: %s", argv[options.idx + 1U + i]);
			return ret;
		}
	}

	path_len = strlen(argv[options.idx]);

	path_buf = k_malloc(XENSTORE_ABS_PATH_MAX + 1U);
	if (path_buf == NULL) {
		shell_error(sh, "alloc buffer");
		return -ENOMEM;
	}

	memcpy(path_buf, argv[options.idx], path_len + 1U);
	ret = cmd_xenstore_chmod_recur(sh, path_buf, perms, perms_num, options.flags & OPT_RECURSE);
	k_free(path_buf);
	if (ret < 0) {
		return ret;
	}

	if (options.flags & OPT_UPTO) {
		ret = cmd_xenstore_chmod_upto(sh, argv[options.idx], perms, perms_num);
	}

	return ret;
}

/* Print watch events and wake the command once the requested event count is reached. */
static void watcher_callback(const char *path, const char *token, void *param)
{
	struct cmd_xenstore_watcher *w = param;

	ARG_UNUSED(token);

	shell_print(w->sh, "%s", path);

	if (w->count == 0U) {
		return;
	}

	if (w->count > 0U) {
		w->count--;
	}

	if (w->count == 0U) {
		k_sem_give(&w->sem);
	}
}

/* Register a XenStore watch and block until -n notifications have been observed. */
static int cmd_xenstore_watch(const struct shell *sh, size_t argc, char **argv)
{
	static struct cmd_xenstore_watcher watcher;
	struct cmd_options options;
	char *buffer;
	const char *path;
	bool handled;
	ssize_t ret;

	ret = parse_command_args(sh, argc, argv, "hn", 1, &options, &handled);
	if ((ret < 0) || handled) {
		return ret;
	}
	if (options.watch_count_set && options.watch_count == 0U) {
		shell_error(sh, "watch count must be greater than zero");
		return -EINVAL;
	}
	if (!options.watch_count_set) {
		shell_error(sh, "watch requires -n in Zephyr shell");
		return -EINVAL;
	}

	path = argv[options.idx];
	ret = check_path_arg(sh, path);
	if (ret < 0) {
		return ret;
	}

	if (!watcher.registered) {
		xs_watcher_init(&watcher.watcher, watcher_callback, &watcher);
		ret = xs_watcher_register(&watcher.watcher);
		if (ret < 0) {
			shell_error(sh, "xs_watcher_register %ld", ret);
			return (int)ret;
		}
		watcher.registered = true;
	}

	watcher.sh = sh;
	watcher.count = options.watch_count;
	k_sem_init(&watcher.sem, 0, 1);

	buffer = k_malloc(XENSTORE_PAYLOAD_MAX + 1U);
	if (buffer == NULL) {
		shell_error(sh, "alloc buffer");
		return -ENOMEM;
	}

	ret = xs_watch_timeout(path, path, buffer, XENSTORE_PAYLOAD_MAX, XS_TRANSACTION_NONE,
			       XENSTORE_SHELL_REQ_TIMEOUT);
	if (ret < 0) {
		shell_error(sh, "xs_watch: %ld", ret);
		goto end;
	}

	ret = k_sem_take(&watcher.sem, XENSTORE_SHELL_WATCH_TIMEOUT);
	if (ret < 0) {
		shell_error(sh, "watch timed out: %ld", ret);
	}

	{
		ssize_t unwatch_ret = xs_unwatch_timeout(path, path, buffer, XENSTORE_PAYLOAD_MAX,
							 XS_TRANSACTION_NONE,
							 XENSTORE_SHELL_REQ_TIMEOUT);

		if (unwatch_ret < 0) {
			shell_error(sh, "xs_unwatch: %ld", unwatch_ret);
			if (ret == 0) {
				ret = unwatch_ret;
			}
		}
	}

end:
	k_free(buffer);

	return (ret < 0) ? (int)ret : 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_xenstore_cmds,
	SHELL_CMD_ARG(init, NULL, "Usage: xenstore init", cmd_xenstore_init, 0, 0),
	SHELL_CMD_ARG(list, NULL, "Usage: xenstore list [-h] [-p] key [...]", cmd_xenstore_list, 1,
		      UINT8_MAX),
	SHELL_CMD_ARG(ls, NULL, "Usage: xenstore ls [-h] [-f] [-p] [path [...]]", cmd_xenstore_ls,
		      0, UINT8_MAX),
	SHELL_CMD_ARG(read, NULL, "Usage: xenstore read [-h] [-p] [-R] path [...]",
		      cmd_xenstore_read, 1, UINT8_MAX),
	SHELL_CMD_ARG(write, NULL, "Usage: xenstore write [-h] [-R] key value [...]",
		      cmd_xenstore_write, 2, UINT8_MAX),
	SHELL_CMD_ARG(rm, NULL, "Usage: xenstore rm [-h] [-t] key [...]", cmd_xenstore_rm, 1,
		      UINT8_MAX),
	SHELL_CMD_ARG(chmod, NULL, "Usage: xenstore chmod [-h] [-u] [-r] key mode [modes...]",
		      cmd_xenstore_chmod, 1, UINT8_MAX),
	SHELL_CMD_ARG(watch, NULL, "Usage: xenstore watch [-h] -n NR key", cmd_xenstore_watch, 1,
		      UINT8_MAX),
	SHELL_SUBCMD_SET_END /* Array terminated. */
);

SHELL_CMD_REGISTER(xenstore, &sub_xenstore_cmds, "XenStore client commands", NULL);
