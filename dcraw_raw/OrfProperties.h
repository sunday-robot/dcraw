#pragma once

#include "ExifProperties.h"

struct Rational {
	int numerator;
	int denominator;
};

class OrfProperties
{
public:
	int imageWidth = -1;	// 256,u32
	int imageHeight = -1;	// 257,u32
	int bitPerSample = -1;	// 258,u16
	int compression = -1;	// 259,u16
	int photometricInterpretation = -1;	// 262,u16
	char *imageDescription = 0;	// 270,string
	char *maker = 0;	// 271,string
	char *model = 0;	// 272,string
	int stripOffset = -1;	// 273,u32
	int orientation = -1;	// 274,u16
	int samplesPerPixel = -1;	// 277,u16
	int _278 = -1;	// 278,u32
	int stripByteCounts = -1;	// 279,u32
	Rational _282;	// 282,u32/u32
	Rational _283;	// 283,u32/u32
	int _284 = -1;	// 284,u16
	int _296 = -1;	// 296,u16
	char *_305 = 0;	// 305,string
	char *dateTime = 0;	// 306,string
	char *artist = 0;	// 315,string
	char *_33432 = 0;	// 33432,string
	ExifProperties *exif;	// 34665,exif
	char *_50341;	// 50341,undefined
public:
	OrfProperties();
	~OrfProperties();
};

