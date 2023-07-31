#pragma once

#include "ExifOlympusProperties.h"

class ExifProperties
{
public:
	double shutter;	// 33434,float
	double aperture;	// 33437,float
	int exposureProgram;	// 34850,short
	int sensitivityType;	// 34864,short
	int isoSpeed;	// 34855,u16
	ExifOlympusProperties *olympusProperties;	// 37500,u32
	double focalLen;	// 37366,float
	char cfaPattern[4];	// 41730,u16,u16,cfa[4]
public:
	ExifProperties();
	~ExifProperties();
};

