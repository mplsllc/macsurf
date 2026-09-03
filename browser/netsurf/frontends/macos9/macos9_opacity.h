/* Mac OS 9 opacity contract shared by the QuickDraw plotter and host tests. */
#ifndef MACOS9_OPACITY_H
#define MACOS9_OPACITY_H

#include <stdint.h>

#define MACOS9_OPACITY_SCALE 1024

enum macos9_opacity_bucket {
	MACOS9_OPACITY_SKIP = 0,
	MACOS9_OPACITY_SPARSE,
	MACOS9_OPACITY_HALF,
	MACOS9_OPACITY_DENSE,
	MACOS9_OPACITY_SOLID
};

static int32_t macos9_opacity_resolve(int32_t opacity, int opacity_set)
{
	return opacity_set ? opacity : MACOS9_OPACITY_SCALE;
}

static enum macos9_opacity_bucket macos9_opacity_bucket_for(int32_t opacity)
{
	if (opacity < MACOS9_OPACITY_SCALE / 20) return MACOS9_OPACITY_SKIP;
	if (opacity < (MACOS9_OPACITY_SCALE * 35) / 100) return MACOS9_OPACITY_SPARSE;
	if (opacity < (MACOS9_OPACITY_SCALE * 60) / 100) return MACOS9_OPACITY_HALF;
	if (opacity < (MACOS9_OPACITY_SCALE * 85) / 100) return MACOS9_OPACITY_DENSE;
	return MACOS9_OPACITY_SOLID;
}

#endif
