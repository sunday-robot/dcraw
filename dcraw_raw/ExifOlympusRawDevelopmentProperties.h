#pragma once
class ExifOlympusRawDevelopmentProperties
{
public:
	char *rawDevVersion;		// 0
	// ExposureBiasValue SRational	// 256
	int whiteBalanceValue;		// 257, u16
	int wbFineAdjustment;		// 258, s16
	int grayPoint;				// 259, s16
	int saturationEmphasis;		// 260, s16
	int memoryColorEmphasisl;	// 261, u16
	int contrastValue;			// 262, s16
	int sharpnessValue;			// 263, s16
	int colorSpace;				// 264, u16
	int engine;					// 265, u16
	int noiseReduction;			// 266, u16
	int editStatus;				// 267, u16
	int setings;				// 268, u16
public:
	ExifOlympusRawDevelopmentProperties();
	~ExifOlympusRawDevelopmentProperties();
};

