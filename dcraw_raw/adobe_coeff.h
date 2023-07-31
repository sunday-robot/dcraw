#pragma once

#include "RgbCoefficient.h"

void adobe_coeff(
	const char *model,	// カメラの製品名
	struct RgbCoefficient *rgbCoefficient,	// [out] ?
	double rgb_cam[][3]);	// [out] ?
