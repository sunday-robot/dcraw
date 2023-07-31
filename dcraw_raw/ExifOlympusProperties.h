#pragma once

#include "ExifOlympusRawDevelopmentProperties.h"
#include "ExifOlympusImageProcessingProperties.h"

class ExifOlympusProperties
{
public:
	char *thumbnailImage;	// 256, undefined
	int specialMode;		// 512, u32
	char *cameraId;			// 521, undefined
	int equipment;			// 8208, Long Camera equipment sub-IFD
	int cameraSettings;		// 8224, Long Camera Settings sub-IFD
	ExifOlympusRawDevelopmentProperties *rawDevelopment;		// 8240, Long Raw development sub-IFD
	ExifOlympusImageProcessingProperties *imageProcessingProperties;	// 8256, Long Image processing sub-IFD
	int focusInfo;			// 8272, Long Focus sub-IFD
public:
	ExifOlympusProperties();
	~ExifOlympusProperties();
};

