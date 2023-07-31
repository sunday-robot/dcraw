#pragma once

#include "BlackLevel.h"
#include "ColorMatrix.h"
#include "WbRbLevels.h"

unsigned short *load(const char *filePath, int *width, int *height, struct BlackLevel *blackLevel, ColorMatrix *colorMatrix, WbRbLevels *wbRbLevels);
