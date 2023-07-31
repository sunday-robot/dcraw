#pragma once

#include "BlackLevel.h"
#include "ColorMatrix.h"
#include "WbRbLevels.h"

class ExifOlympusImageProcessingProperties
{
public:
	ColorMatrix *colorMatrix;
	BlackLevel *blackLevel;
	WbRbLevels *wbRbLevels;
public:
	ExifOlympusImageProcessingProperties();
	~ExifOlympusImageProcessingProperties();
};

