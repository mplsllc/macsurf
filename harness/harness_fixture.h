/* Host-harness fixture paths.  The Makefile supplies this absolute root so
 * tests never depend on the process working directory. */
#ifndef HARNESS_FIXTURE_H
#define HARNESS_FIXTURE_H

#include <stdio.h>
#include <string.h>

#ifndef HARNESS_FIXTURE_ROOT
#error HARNESS_FIXTURE_ROOT must name the harness fixture directory
#endif

#define HARNESS_FIXTURE_PATH_MAX 4096

static int
harness_fixture_path(const char *name, char *path, size_t path_size)
{
	size_t root_len;
	size_t name_len;

	if (name == NULL || path == NULL || name[0] == '/')
		return 0;
	root_len = strlen(HARNESS_FIXTURE_ROOT);
	name_len = strlen(name);
	if (root_len + 1 + name_len + 1 > path_size)
		return 0;
	memcpy(path, HARNESS_FIXTURE_ROOT, root_len);
	path[root_len] = '/';
	memcpy(path + root_len + 1, name, name_len + 1);
	return 1;
}

static FILE *
harness_fixture_open(const char *name, const char *mode)
{
	char path[HARNESS_FIXTURE_PATH_MAX];

	if (!harness_fixture_path(name, path, sizeof(path)))
		return NULL;
	return fopen(path, mode);
}

#endif
