/*-------------------------------------------------------------------------
 * memory_pressure.c -- parse bounded cgroup v2 metadata
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include "postmaster/memory_protector.h"
#include "storage/fd.h"

bool
MPParseBytes(const char *text, uint64 *value)
{
	const unsigned char *p = (const unsigned char *) text;
	uint64		v = 0;

	*value = 0;
	if (*p < '0' || *p > '9')
		return false;
	for (; *p >= '0' && *p <= '9'; p++)
	{
		unsigned int digit = *p - '0';

		if (v > (UINT64_MAX - digit) / 10)
			return false;
		v = v * 10 + digit;
	}
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p != '\0')
		return false;
	*value = v;
	return true;
}

/* Do not normalize traversal components or accept truncated kernel paths. */
static bool
valid_path(const char *path)
{
	const char *p;

	if (path[0] != '/')
		return false;
	for (p = path; *p; p++)
	{
		if ((unsigned char) *p < 32)
			return false;
		if (*p == '/' &&
			((p[1] == '.' && (p[2] == '/' || p[2] == '\0')) ||
			 (p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == '\0'))))
			return false;
	}
	return true;
}

bool
MPParseCgroup(const char *text, char *path, Size capacity)
{
	const char *line = text;
	bool		found = false;

	if (capacity == 0)
		return false;
	path[0] = '\0';
	while (*line)
	{
		const char *end = strchr(line, '\n');
		Size		len = end ? (Size) (end - line) : strlen(line);

		if (len >= 3 && strncmp(line, "0::", 3) == 0)
		{
			if (found || len - 3 >= capacity)
				goto fail;
			memcpy(path, line + 3, len - 3);
			path[len - 3] = '\0';
			if (!valid_path(path))
				goto fail;
			found = true;
		}
		if (!end)
			break;
		line = end + 1;
	}
	return found;
fail:
	path[0] = '\0';
	return false;
}

static bool
decode_mount_path(const char *input, char *out, Size capacity)
{
	Size		used = 0;

	while (*input)
	{
		unsigned char c = *input++;

		if (c == '\\')
		{
			/* Linux mountinfo escapes space, tab, newline and backslash. */
			if (strncmp(input, "040", 3) == 0)
				c = ' ';
			else if (strncmp(input, "134", 3) == 0)
				c = '\\';
			else
				return false;
			input += 3;
		}
		if (used + 1 >= capacity)
			return false;
		out[used++] = c;
	}
	if (!capacity)
		return false;
	out[used] = '\0';
	return valid_path(out);
}

bool
MPResolveMount(const char *text, const char *membership, char *path, Size capacity)
{
	char	   *copy;
	char	   *line;
	char	   *linesave;
	bool		found = false;

	if (!capacity)
		return false;
	path[0] = '\0';
	if (!valid_path(membership))
		return false;
	copy = pstrdup(text);
	for (line = strtok_r(copy, "\n", &linesave); line;
		 line = strtok_r(NULL, "\n", &linesave))
	{
		char	   *separator = strstr(line, " - cgroup2 ");
		char	   *field;
		char	   *save;
		char		root[MAXPGPATH];
		char		mount[MAXPGPATH];
		const char *suffix;
		int			n = 0;
		Size		rootlen;

		if (!separator)
			continue;
		*separator = '\0';
		root[0] = mount[0] = '\0';
		for (field = strtok_r(line, " ", &save); field;
			 field = strtok_r(NULL, " ", &save))
		{
			n++;
			if (n == 4 && !decode_mount_path(field, root, sizeof(root)))
				break;
			if (n == 5 && !decode_mount_path(field, mount, sizeof(mount)))
				break;
		}
		if (n < 6 || root[0] == '\0' || mount[0] == '\0')
			continue;
		rootlen = strlen(root);
		if (strcmp(root, "/") == 0)
			suffix = strcmp(membership, "/") == 0 ? "" : membership;
		else if (strncmp(root, membership, rootlen) == 0 &&
				 (membership[rootlen] == '/' || membership[rootlen] == '\0'))
			suffix = membership + rootlen;
		else
			continue;
		/* Ambiguous mount mappings require platform discovery, not a guess. */
		if (found || strlen(mount) + strlen(suffix) >= capacity)
		{
			found = false;
			break;
		}
		snprintf(path, capacity, "%s%s", mount, suffix);
		found = true;
	}
	pfree(copy);
	if (!found)
		path[0] = '\0';
	return found;
}

#ifdef __linux__
/* Bounded reads; incomplete input must never become a valid pressure sample. */
static bool
read_text(const char *path, char *buf, Size capacity)
{
	FILE	   *file;
	Size		n;
	bool		ok;

	file = AllocateFile(path, "r");
	if (file == NULL)
		return false;
	n = fread(buf, 1, capacity - 1, file);
	ok = !ferror(file) && feof(file);
	buf[n] = '\0';
	FreeFile(file);
	return ok;
}

static bool
read_bytes_at(const char *dir, const char *name, uint64 *value)
{
	char		path[MAXPGPATH];
	char text[	64];

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= sizeof(path))
		return false;
	return read_text(path, text, sizeof(text)) && MPParseBytes(text, value);
}
#endif


#ifdef __linux__
static bool
native_boundary(const char *directory, const char *root)
{
	char		path[MAXPGPATH];
	char		file[MAXPGPATH];
	char		value[64];
	struct stat st;
	char	   *slash;
	Size		rootlen = strlen(root);

	if (strcmp(directory, root) == 0 || strncmp(directory, root, rootlen) != 0 ||
		directory[rootlen] != '/')
		return false;
	if (snprintf(file, sizeof(file), "%s/memory.max", root) >= sizeof(file))
		return false;

	/*
	 * A mounted/namespace subtree root has memory.max; the true root does
	 * not.
	 */
	if (stat(file, &st) == 0 || errno != ENOENT)
		return false;
	strlcpy(path, directory, sizeof(path));
	while ((slash = strrchr(path, '/')) != NULL)
	{
		*slash = '\0';
		if (strcmp(path, root) == 0)
			return true;
		if (strlen(path) <= rootlen ||
			snprintf(file, sizeof(file), "%s/memory.max", path) >= sizeof(file) ||
			!read_text(file, value, sizeof(value)) || strcmp(value, "max\n") != 0)
			return false;
	}
	return false;
}
#endif

bool
MPReadPressure(MPPressureSample *out, char *reason, Size capacity)
{
	/*
	 * Leave no old measurement behind on error. Sequence/time belong to
	 * caller.
	 */
	out->valid = false;
	out->boundary_trusted = false;
	out->limit_bytes = 0;
	out->current_bytes = 0;
#ifdef __linux__
	{
		char	   *text = palloc(65536);
		char		membership[MAXPGPATH];
		char		confirmed[MAXPGPATH];
		char		directory[MAXPGPATH];
		char		root[MAXPGPATH];
		bool		root_visible;
		uint64		limit_after;
		bool		ok = false;

		strlcpy(reason, "cgroup_unavailable", capacity);
		if (!read_text("/proc/self/cgroup", text, 65536) ||
			!MPParseCgroup(text, membership, sizeof(membership)) ||
			!read_text("/proc/self/mountinfo", text, 65536) ||
			!MPResolveMount(text, membership, directory, sizeof(directory)))
			goto done;
		root_visible = MPResolveMount(text, "/", root, sizeof(root));
		strlcpy(reason, "local_limit_unavailable", capacity);
		if (!read_bytes_at(directory, "memory.max", &out->limit_bytes) ||
			out->limit_bytes == 0 ||
			!read_bytes_at(directory, "memory.current", &out->current_bytes))
			goto done;
		strlcpy(reason, "cgroup_changed", capacity);
		if (!read_text("/proc/self/cgroup", text, 65536) ||
			!MPParseCgroup(text, confirmed, sizeof(confirmed)) ||
			strcmp(membership, confirmed) != 0 ||
			!read_bytes_at(directory, "memory.max", &limit_after) ||
			limit_after != out->limit_bytes)
			goto done;
		/* A namespace-hidden or constrained ancestor prevents enforcement. */
		out->boundary_trusted = root_visible && native_boundary(directory, root);
		strlcpy(reason, out->boundary_trusted ? "native_instance_boundary" : "local_cgroup_only", capacity);
		out->valid = true;
		ok = true;
done:
		pfree(text);
		return ok;
	}
#else
	strlcpy(reason, "unsupported_platform", capacity);
	return false;
#endif
}

/* Parse smaps_rollup without treating missing or overflowing fields as zero. */
bool
MPParsePrivateBytes(const char *text, uint64 *bytes)
{
	char	   *copy = pstrdup(text);
	char	   *line;
	char	   *save;
	unsigned	mask = 0;
	uint64		total = 0;
	bool		ok = false;

	*bytes = 0;
	for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
	{
		char	   *colon = strchr(line, ':');
		char	   *number;
		char	   *end;
		unsigned	bit;
		uint64		kb;

		if (!colon)
			continue;
		*colon = '\0';
		if (strcmp(line, "Private_Clean") == 0)
			bit = 1;
		else if (strcmp(line, "Private_Dirty") == 0)
			bit = 2;
		else if (strcmp(line, "Private_Hugetlb") == 0)
			bit = 4;
		else
			continue;
		if (mask & bit)
			goto done;
		number = colon + 1;
		while (*number == ' ' || *number == '\t')
			number++;
		end = number;
		while (*end >= '0' && *end <= '9')
			end++;
		if (*end != ' ' && *end != '\t')
			goto done;
		*end++ = '\0';
		while (*end == ' ' || *end == '\t')
			end++;
		if (strcmp(end, "kB") != 0 || !MPParseBytes(number, &kb) ||
			kb > (UINT64_MAX - total) / 1024)
			goto done;
		total += kb * 1024;
		mask |= bit;
	}
	if ((mask & 3) == 3)
	{
		*bytes = total;
		ok = true;
	}
done:
	pfree(copy);
	return ok;
}

bool
MPReadProcessStart(pid_t pid, uint64 *ticks)
{
	*ticks = 0;
#ifdef __linux__
	{
		char		path[64];
		char		buffer[4096];
		char	   *p;
		char	   *save;
		char	   *field;
		int			number = 3;

		snprintf(path, sizeof(path), "/proc/%d/stat", (int) pid);
		if (!read_text(path, buffer, sizeof(buffer)))
			return false;
		p = strrchr(buffer, ')');
		if (!p || p[1] != ' ')
			return false;
		for (field = strtok_r(p + 2, " ", &save); field;
			 field = strtok_r(NULL, " ", &save), number++)
			if (number == 22)
				return MPParseBytes(field, ticks);
	}
#endif
	return false;
}

bool
MPReadPrivateBytes(pid_t pid, uint64 ticks, uint64 *bytes)
{
	*bytes = 0;
#ifdef __linux__
	{
		char		path[64];
		char		buffer[8192];
		uint64		actual;

		if (!ticks || !MPReadProcessStart(pid, &actual) || ticks != actual)
			return false;
		snprintf(path, sizeof(path), "/proc/%d/smaps_rollup", (int) pid);
		if (!read_text(path, buffer, sizeof(buffer)) || !MPParsePrivateBytes(buffer, bytes))
			return false;
		if (!MPReadProcessStart(pid, &actual) || ticks != actual)
		{
			*bytes = 0;
			return false;
		}
		return true;
	}
#else
	return false;
#endif
}
