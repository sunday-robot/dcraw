#pragma once
#include "BlackLevel.h"
#include "ColorMatrix.h"

void convertRawToRgbImage(
	unsigned short *rawImage,
	int width,
	int height,
	const struct BlackLevel *blackLevel,
	const ColorMatrix *colorMatrix,
	unsigned short(**rgbImage)[3]);
