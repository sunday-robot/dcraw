#include "load.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "decompressRawData.h"
#include "OrfProperties.h"
#include "ExifProperties.h"
#include "ExifOlympusProperties.h"

static char *createString(FILE *fp, int length) {
	char *str = (char *)malloc(length + 1);
	fread(str, 1, length, fp);
	str[length] = 0;
	return str;
}

static char *createBytes(FILE *fp, int length) {
	char *bytes = (char *)malloc(length);
	fread(bytes, 1, length, fp);
	return bytes;
}


// リトルエンディアン専用
unsigned short get2(FILE *fp)
{
	unsigned char s[2];
	fread(s, 1, 2, fp);
	return s[0] | s[1] << 8;
}

// リトルエンディアン専用
short getS16(FILE *fp)
{
	unsigned char s[2];
	fread(s, 1, 2, fp);
	return (short)(s[0] | s[1] << 8);
}

// リトルエンディアン専用
unsigned get4(FILE *fp)
{
	unsigned char s[4];
	fread(s, 1, 4, fp);
	return s[0] | s[1] << 8 | s[2] << 16 | s[3] << 24;
}

float get_float(FILE *fp) {
	union {
		char c[4];
		float f;
	} v;
	fread(v.c, 1, 4, fp);
	return v.f;
}

int datum_size(int type)
{
	switch (type) {
	case 0:
	case 1:
	case 2:
		return 1;
	case 3:
		return 2;
	case 4:
		return 4;
	case 5:
		return 8;
	case 6:
	case 7:
		return 1;
	case 8:
		return 2;
	case 9:
		return 4;
	case 10:
		return 8;
	case 11:
		return 4;
	case 12:
		return 8;
	case 13:
		return 4;
	default:
		return 1;
	}
}

// TIFFタグのデータ以外の部分を取得し、データの開始位置にファイルポインターを移動させる。(データの読み込みは、呼び出し側で行う。)
void tiff_get(
	FILE *fp,		// TIFFファイル
	unsigned base,	// TIFFデータの基準位置
	unsigned *tag,	// [out] タグの種類(16bit unsigned integer)
	unsigned *type,	// [out] タグに含まれるデータの型(16bit unsigned integer)
	unsigned *len,	// [out] タグに含まれるデータの個数(32bit unsgined integer)
	unsigned *next_tiff_tag_position)	// [out] 次のタグの位置(32bit unsgined integer)
{
	off_t current_position = ftell(fp);
	*tag = get2(fp);
	*type = get2(fp);
	*len = get4(fp);
	*next_tiff_tag_position = current_position + 2 + 2 + 4 + 4;
//	printf("%08x: tag = %04x, type = %04x, len = %8d, next_tiff_tag_position = %08x\n", current_position, *tag, *type, *len, *next_tiff_tag_position);
	// データの総バイト数が4より大きい場合は、次の4バイトにデータの開始位置が記録されているので、これを読み、その位置にfseek()する。
	if (*len * datum_size(*type) > 4)
		fseek(fp, get4(fp) + base, SEEK_SET);
}

//void report_current_position(FILE *fp) {
//	off_t o = ftell(fp);
//	printf("current position = %08x\n", o);
//}

ExifOlympusRawDevelopmentProperties *parse_olympus_raw_development_tags(
	FILE *fp,
	int base)
{
	ExifOlympusRawDevelopmentProperties *p = new ExifOlympusRawDevelopmentProperties();

	int entries = get2(fp);
	while (entries--) {
		unsigned tag, type, len, next_tiff_tag_position;
		tiff_get(fp, base, &tag, &type, &len, &next_tiff_tag_position);
		int v, v2;
		switch (tag) {
		case 0:
			p->rawDevVersion = createBytes(fp, len);
			break;
		case 256:	// ExposureBiasValue {}
			break;
		case 257:
			p->whiteBalanceValue = get2(fp);
			break;
		case 258:
			p->wbFineAdjustment = getS16(fp);
			break;
		case 259:
			p->grayPoint = get2(fp);
			break;
		case 260:
			p->saturationEmphasis = getS16(fp);
			break;
		case 261:
			p->memoryColorEmphasisl = get2(fp);
			break;
		case 262:
			p->contrastValue = getS16(fp);
			break;
		case 263:
			p->sharpnessValue = getS16(fp);
			break;
		case 264:
			p->colorSpace = get2(fp);
			break;
		case 265:
			p->engine = get2(fp);
			break;
		case 266:
			p->noiseReduction = get2(fp);
			break;
		case 267:
			p->editStatus = get2(fp);
			break;
		case 268:
			p->setings = get2(fp);
			break;
		default:
			printf("?%d: ", tag);
			if (len > 100) {
				len = 100;
			}
			for (int i = 0; i < len; i++) {
				switch (type) {
				case 3: {	// u16
					v = get2(fp);
					printf("%d, ", v);
					break;
				}
				case 4: {	// u32
					v = get4(fp);
					printf("%d, ", v);
					break;
				}
				case 5: {	// u rational
					int v1 = get4(fp);
					int v2 = get4(fp);
					printf("%d/%d, ", v1, v2);
					break;
				}
				case 1:
				case 7: {	// undefined
					int v = fgetc(fp);
					printf("%02x, ", v);
					break;
				}
				case 8: {	// s16
					int v = get2(fp);
					printf("%02x, ", v);
					break;
				}
				case 11: {	// float
					double v = get_float(fp);
					printf("%f, ", v);
					break;
				}
				default:
					printf("aaa\n");
				}
			}
			printf("\n");
			//printf("aaa\n");
			;
		}
		fseek(fp, next_tiff_tag_position, SEEK_SET);
	}

	return p;
}

ExifOlympusImageProcessingProperties *parse_olympus_image_processing_tags(
	FILE *fp,
	int base)
{
	ExifOlympusImageProcessingProperties *p = new ExifOlympusImageProcessingProperties();

	int entries = get2(fp);
	while (entries--) {
		unsigned tag, type, len, next_tiff_tag_position;
		tiff_get(fp, base, &tag, &type, &len, &next_tiff_tag_position);
		int v, v2;
		switch (tag) {
		case 0:		// ImageProcessingVersion {"0112"}
			printf("ImageProcessingVersion: ");
			for (int i = 0; i < len; i++) {
				v = fgetc(fp);
				printf("%02x, ", v);
			}
			printf("\n");
			break;
		case 256:	// WB_RBLevelsUsed {478, 486, 256, 256}
			p->wbRbLevels = new WbRbLevels(get2(fp), get2(fp));
			// 残りの二つは使用しないので読まない。
			break;
		case 257:	// WB_RBLevels3000K {512, 464, 256, 256}
		case 258:
		case 259:
		case 260:
		case 261:
		case 262:
		case 263:
		case 264:
		case 265:
		case 266:
		case 267:
		case 268:
		case 269:
		case 270:
		case 271:
		case 272:
		case 273:
			printf("WB_RBLevels[%d]: ", tag - 257);
			for (int i = 0; i < len; i++) {
				v = get2(fp);
				printf("%d, ", v);
			}
			printf("\n");
			break;
		case 288:	// ? {438, 548, 256, 256}
		case 289:	// ? {544, 414, 256, 256}
			printf("?288[%d]: ", tag - 288);
			for (int i = 0; i < len; i++) {
				v = get2(fp);
				printf("%d, ", v);
			}
			printf("\n");
			break;
		case 290:	// ? {2, 100, -2, 100, -2, 100, -2, 100}
			printf("?290: ");
			for (int i = 0; i < len; i++) {
				v = getS16(fp);
				printf("%d, ", v);
			}
			printf("\n");
			break;
		case 291:	// ? {1011, 1009, 1008, 1010, 2976, 2976, 2976, 2976}
			printf("?291: ");
			for (int i = 0; i < len; i++) {
				v = get2(fp);
				printf("%d, ", v);
			}
			printf("\n");
			break;
		case 292:	// ? {48381}
			v = get2(fp);
			printf("?292: %d\n", v);
			break;
		case 293:	// ? {512, 464, 256, 256}
		case 294:
			printf("?293[%d]: ", tag - 293);
			for (int i = 0; i < len; i++) {
				v = get2(fp);
				printf("%d, ", v);
			}
			printf("\n");
			break;
		case 512:	// Exif.OlympusIp.ColorMatrix
		{
			int e00 = getS16(fp);
			int e01 = getS16(fp);
			int e02 = getS16(fp);
			int e10 = getS16(fp);
			int e11 = getS16(fp);
			int e12 = getS16(fp);
			int e20 = getS16(fp);
			int e21 = getS16(fp);
			int e22 = getS16(fp);
			p->colorMatrix = new ColorMatrix(e00, e01, e02, e10, e11, e12, e20, e21, e22);
			break;
		}
		case 848:	// ? {0, 0, 9, 16383, 4095, 9, 16333, 4095, 0, 16383, 4095, 0}
			printf("?848: ");
			for (int i = 0; i < len; i++) {
				v = get2(fp);
				printf("%d, ", v);
			}
			printf("\n");
			break;
		case 1536:	// Exif.OlympusIp.BlackLevel
			p->blackLevel = new BlackLevel();
			p->blackLevel->red = get2(fp);
			p->blackLevel->green = get2(fp);
			p->blackLevel->green2 = get2(fp);
			p->blackLevel->blue = get2(fp);
			break;
		case 1552:	// GainBase -> 256
			v = get2(fp);
			printf("%d, ", v);
			break;
		case 1553:	// Exif.OlympusIp.ValidBits -> {12, 0}
			v = get2(fp);
			v2 = get2(fp);
			printf("ValidBits = {%d, %d}\n", v, v2);
			break;
		case 1554:	// CropLeft -> {8, 0}
			v = get2(fp);
			v2 = get2(fp);
			printf("CropLeft = {%d, %d}\n", v, v2);
			break;
		case 1555:	// Exif.OlympusIp.CropTop -> {8, 0}
			v = get2(fp);
			v2 = get2(fp);
			printf("CropTop = {%d, %d}\n", v, v2);
			break;
		case 1556:	// CropWidth {4608}
			v = get4(fp);
			printf("CropWidth = %d\n", v);
			break;
		case 1557:	// CropHeight {3456}
			v = get4(fp);
			printf("CropHeight = %d\n", v);
			break;
		case 1559:	// ? -> {65535}
			v = get2(fp);
			printf("?1559 = %d\n", v);
			break;
		case 1568:	// ? -> {3}
			v = get2(fp);
			printf("?1568 = %d\n", v);
			break;
		case 1584:	// ? -> {3}
			v = get2(fp);
			printf("?1584 = %d\n", v);
			break;
		case 1589:		// ? [5764]{}
			printf("?1589: ");
			//for (int i = 0; i < len; i++) {
			//	v = fgetc(fp);
			//	printf("%02x, ", v);
			//}
			printf("\n");
			break;
		case 1590:		// ? [76608]{}
			printf("?1590: ");
			//for (int i = 0; i < len; i++) {
			//	v = fgetc(fp);
			//	printf("%02x, ", v);
			//}
			printf("\n");
			break;
		case 1591:	// ? {2303, 4607, 1727, 3455}
			printf("?1591: ");
			for (int i = 0; i < len; i++) {
				v = get2(fp);
				printf("%d, ", v);
			}
			printf("\n");
			break;
		case 1592:	// ? -> {4590}
			v = get4(fp);
			printf("?1592 = %d\n", v);
			break;
		case 1600:	// ? -> {0}
			v = get2(fp);
			printf("?1600 = %d\n", v);
			break;
		case 1601:	// ? -> {5}
		case 1602:	// ? -> {0}
		case 1603:	// ? -> {2}
		case 1604:	// ? -> {5}
		case 1605:	// ? -> {4}
		case 1606:	// ? -> {3}
		case 1607:	// ? -> {2}
		case 1608:	// ? -> {3}
		case 1609:	// ? -> {4}
			v = get2(fp);
			printf("?1601[%d] = %d\n", tag - 1601, v);
			break;
		case 1616:	// ? -> {7}
		case 1617:	// ? -> {15}
		case 1618:	// ? -> {12}
		case 1619:	// ? -> {5}
		case 1620:	// ? -> {80}
			v = get2(fp);
			printf("?1616[%d] = %d\n", tag - 1616, v);
			break;
		case 2048:	// ? -> {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}
			printf("?2048: ");
			for (int i = 0; i < len; i++) {
				double v = get_float(fp);
				printf("%f, ", v);
			}
			printf("\n");
			break;
		case 2049:	// ? {8196, 8226, 8234, 8413, 8546, 8756, 8906, 9186, 9485, 9746, 10202, 10801, 13275, 16031, 20150}
			printf("?2049: ");
			for (int i = 0; i < len; i++) {
				v = get2(fp);
				printf("%d, ", v);
			}
			printf("\n");
			break;
		case 2050:	// ? -> 11140000/59200
			v = get4(fp);
			v2 = get4(fp);
			printf("?2050: %d/%d\n", v, v2);
			break;
		case 2052:	// ? {1, 341}
			printf("?2052: ");
			for (int i = 0; i < len; i++) {
				v = get2(fp);
				printf("%d, ", v);
			}
			printf("\n");
			break;
		case 2053:	// ? -> {4051, 256}
			printf("?2053: ");
			for (int i = 0; i < len; i++) {
				v = getS16(fp);
				printf("%d, ", v);
			}
			printf("\n");
			break;
		case 4099:	// ? 8
		case 4100:	// ? 1536
			v = get2(fp);
			printf("?4099[%d] = %d\n", tag - 4099, v);
			break;
		case 4112:	// NoiseReduction 0
			v = get2(fp);
			printf("NoiseReduction = %d\n", v);
			break;
		case 4113:	// DistortionCorrection 0
			v = get2(fp);
			printf("DistortionCorrection = %d\n", v);
			break;
		case 4114:	// ShadingCompensation 0
			v = get2(fp);
			printf("ShadingCompensation = %d\n", v);
			break;
		case 4123:	// ? {0, 0}
			printf("?4123: ");
			for (int i = 0; i < len; i++) {
				v = getS16(fp);
				printf("%d, ", v);
			}
			printf("\n");
			break;
		case 4124:	// MultipleExposureMode {0, 1}
			printf("MultipleExposureMode: ");
			for (int i = 0; i < len; i++) {
				v = getS16(fp);
				printf("%d, ", v);
			}
			printf("\n");
			break;
		case 4126:	// ? {0, 0}
			printf("?4126: ");
			for (int i = 0; i < len; i++) {
				v = get2(fp);
				printf("%d, ", v);
			}
			printf("\n");
			break;
		case 4370:	// AspectRatio {1, 1}
		case 4371:	// AspectFrame {0, 0, 4607, 3455}
		case 4608:	// FaceDetect
		case 4609:	// FaceDetectArea
		default:
			printf("?%d: ", tag);
			if (len > 100) {
				len = 100;
			}
			for (int i = 0; i < len; i++) {
				switch (type) {
				case 3: {	// u16
					v = get2(fp);
					printf("%d, ", v);
					break;
				}
				case 4: {	// u32
					v = get4(fp);
					printf("%d, ", v);
					break;
				}
				case 5: {	// u rational
					int v1 = get4(fp);
					int v2 = get4(fp);
					printf("%d/%d, ", v1, v2);
					break;
				}
				case 1:
				case 7: {	// undefined
					int v = fgetc(fp);
					printf("%02x, ", v);
					break;
				}
				case 8: {	// s16
					int v = get2(fp);
					printf("%02x, ", v);
					break;
				}
				case 11: {	// float
					double v = get_float(fp);
					printf("%f, ", v);
					break;
				}
				default:
					printf("aaa\n");
				}
			}
			printf("\n");
			//printf("aaa\n");
			;
		}
		fseek(fp, next_tiff_tag_position, SEEK_SET);
	}

	return p;
}

ExifOlympusProperties *parse_olympus_maker_note_tags(
	FILE *fp)
{
	ExifOlympusProperties *p = new ExifOlympusProperties();
	int base = ftell(fp);
	fseek(fp, 8, SEEK_CUR);
	int byteOrder = get2(fp);	// byte order
	int x = get2(fp);	// version?
	int entries = get2(fp);
	while (entries--) {
		unsigned tag, type, len, next_tiff_tag_position;
		tiff_get(fp, base, &tag, &type, &len, &next_tiff_tag_position);

		unsigned int sub_tags_offset;
		switch (tag) {
		case 0x0100:	// Exif.Olympus.ThumbnailImage
			p->thumbnailImage = createBytes(fp, len);
			break;
		case 0x0200:	// Exif.Olympus.SpecialMode
			p->specialMode = get4(fp);
			break;
		case 0x0209:	// Exif.Olympus.CameraID
			p->cameraId = createBytes(fp, len);
			break;
		case 0x2010:	// Exif.Olympus.Equipment
			p->equipment = get4(fp);
			break;
		case 0x2020:	// Exif.Olympus.CameraSettings
			p->cameraSettings = get4(fp);
			break;
		case 0x2030:	// Exif.Olympus.RawDevelopment
			sub_tags_offset = get4(fp);
			fseek(fp, base + sub_tags_offset, SEEK_SET);
			p->rawDevelopment = parse_olympus_raw_development_tags(fp, base);
			break;
		case 0x2040:	// Exif.Olympus.ImageProcessing
			sub_tags_offset = get4(fp);	// TIFFタグのデータ部分(4バイト以下なので、ポインターではなく値ということらしい。ただしデータ型は13で、TIFFデータ型としては定義されていないものと思われる)
			fseek(fp, sub_tags_offset + base, SEEK_SET);
			//report_current_position(fp);
			p->imageProcessingProperties = parse_olympus_image_processing_tags(fp, base);
			break;
		case 0x2050:	// Exif.Olympus.FocusInfo
			p->focusInfo = get4(fp);
			break;
		default:
			printf("%08x\n", tag);
		}
		fseek(fp, next_tiff_tag_position, SEEK_SET);
	}
	return p;
}

ExifProperties *parse_exif(
	FILE *fp)
{
	ExifProperties *p = new ExifProperties();
	int entries = get2(fp);
	while (entries--) {
		int v;
		int v2;
		char *s;
		char *b;
		unsigned tag, type, len, next_tiff_tag_position;
		tiff_get(fp, 0, &tag, &type, &len, &next_tiff_tag_position);
		switch (tag) {
		case 33434:
			p->shutter = get_float(fp);
			break;
		case 33437:
			p->aperture = get_float(fp);
			break;
		case 34850:
			p->exposureProgram  = get2(fp);
			break;
		case 34864:	// ?1
			p->sensitivityType = get2(fp);
			break;
		case 34855:
			p->isoSpeed = get2(fp);
			break;
		case 36864:	// ?"0230" byte[4];
			b = createBytes(fp, len);
			break;
		case 36867:	// timestamp
			s = createString(fp, len);
			break;
		case 36868:	// timestamp
			s = createString(fp, len);
			break;
		case 37386:
			p->focalLen = get_float(fp);
			break;
		case 37380:	// ?0/10
			v = get4(fp);
			v2 = get4(fp);
			break;
		case 37381:	// ?925/256
			v = get4(fp);
			v2 = get4(fp);
			break;
		case 37383:	// ?5
			v = get2(fp);
			break;
		case 37384:	// ?9
			v = get2(fp);
			break;
		case 37385:	// ?24
			v = get2(fp);
			break;
		case 37500:	// MakerNote
			p->olympusProperties = parse_olympus_maker_note_tags(fp);
			break;
		case 37510:	// ? byte[125];
			b = createBytes(fp, len);
			break;
		case 40960:	// ?"0100" byte[4];
			b = createBytes(fp, len);
			break;
		case 40961:	// ?1
			v = get2(fp);
			break;
		case 41728:	// ? byte[1];
			b = createBytes(fp, len);
			break;
		case 41730:	// CFAPattern bytes[8]
			get2(fp);	// CFAの幅(==2のはず)
			get2(fp);	// CFAの高さ(==2のはず)
			p->cfaPattern[0] = fgetc(fp);	// (0,0)のチャンネル(Red=0)
			p->cfaPattern[1] = fgetc(fp);	// (0,1)のチャンネル(Green=1)
			p->cfaPattern[2] = fgetc(fp);	// (1,0)のチャンネル(Green=1)
			p->cfaPattern[3] = fgetc(fp);	// (1,1)のチャンネル(Blue=2)
			break;
		case 41985:	// ?0
			v = get2(fp);
			break;
		case 41986:	// ?0
			v = get2(fp);
			break;
		case 41987:	// ?1
			v = get2(fp);
			break;
		case 41988:	// ?100/100
			v = get4(fp);
			v2 = get4(fp);
			break;
		case 41990:	// ?0
			v = get2(fp);
			break;
		case 41991:	// ?2
			v = get2(fp);
			break;
		case 41992:	// ?0
			v = get2(fp);
			break;
		case 41993:	// ?0
			v = get2(fp);
			break;
		case 41994:	// ?0
			v = get2(fp);
			break;
		case 42034:	// ?14/1
			v = get4(fp);
			v2 = get4(fp);
			break;
		case 42036:	// レンズの名前"OLYMPUS M.14-42mm F3.5-5.6 EZ"
			s = createString(fp, len);
			break;
		default:
			printf("%d, %d, %d, %x\n", tag, type, len, next_tiff_tag_position);
		}
		fseek(fp, next_tiff_tag_position, SEEK_SET);
	}
	return p;
}

// 
OrfProperties *parse_tiff(
	FILE *fp)						// RAWファイル
{
	int exifTag;
	OrfProperties *orfProperties = new OrfProperties();

	fseek(fp, 4, SEEK_SET);	// バイトオーダーとバージョン(どちらも2バイト)は無視
	int firstIfdPointer = get4(fp);
	fseek(fp, firstIfdPointer, SEEK_SET);
	int entries = get2(fp);

	while (entries--) {
		unsigned tag, type, len, next_tiff_tag_position;
		tiff_get(fp, 0, &tag, &type, &len, &next_tiff_tag_position);

		switch (tag) {
		case 256:	/* ImageWidth */
			orfProperties->imageWidth = get4(fp);
			break;
		case 257:	/* ImageHeight */
			orfProperties->imageHeight = get4(fp);
			break;
		case 258:	/* BitsPerSample */
			orfProperties->bitPerSample = get2(fp);
			break;
		case 259:				/* Compression */
			orfProperties->compression = get2(fp);
			break;
		case 262:				/* PhotometricInterpretation */
			orfProperties->photometricInterpretation = get2(fp);
			break;
		case 270:				/* ImageDescription */
			orfProperties->imageDescription = createString(fp, len);
			break;
		case 271:				/* Maker */
			orfProperties->maker = createString(fp, len);
			break;
		case 272:				/* Model */
			orfProperties->model = createString(fp, len);
			break;
		case 273:				/* StripOffset */
			orfProperties->stripOffset = get4(fp);
			break;
		case 274:				/* Orientation */
			orfProperties->orientation = get2(fp);
			break;
		case 277:				/* SamplesPerPixel */
			orfProperties->samplesPerPixel = get2(fp);
			break;
		case 278:	// ???
			orfProperties->_278 = get4(fp);
			break;
		case 279:				/* 0x0117 StripByteCounts */
			orfProperties->stripByteCounts = get4(fp);
			break;
		case 282:	// ???
			orfProperties->_282.numerator = get4(fp);
			orfProperties->_282.denominator = get4(fp);
			break;
		case 283:	// ???
			orfProperties->_283.numerator = get4(fp);
			orfProperties->_283.denominator = get4(fp);
			break;
		case 284:	// 0x011c
			orfProperties->_284 = get2(fp);
			break;
		case 296:	// 0x0128
			orfProperties->_296 = get2(fp);
			break;
		case 305:	// 0x0131
			orfProperties->_305 = createString(fp, len);
			break;
		case 306:				/* DateTime */
			orfProperties->dateTime = createString(fp, len);
			break;
		case 315:				/* Artist */
			orfProperties->artist = createString(fp, len);
			break;
		case 33432:	// 0x8298
			orfProperties->_33432 = createString(fp, len);
			break;
		case 34665:			/* EXIF tag (0x8769) */
			exifTag = get4(fp);
			fseek(fp, exifTag, SEEK_SET);
			orfProperties->exif = parse_exif(fp);
			break;
		case 50341:	// 0xC4A5
			orfProperties->_50341 = createBytes(fp, len);
			break;
		default:
			printf("aaa");
		}
		fseek(fp, next_tiff_tag_position, SEEK_SET);
	}
	return orfProperties;
}

// RAWファイルを読み、展開済みのRAW画像データ(各ピクセルがRGBを持つのではなく、どれか一つのチャンネルの値だけを持つ)と、
// カラー画像化に必要な情報を取得する
unsigned short *load(
	const char *filePath,	// RAWファイルのパス名
	int *width,				// out 画像の幅
	int *height,			// out 画像の高さ
	struct BlackLevel *blackLevel,	// out 最低輝度値
	ColorMatrix *colorMatrix, 	// out 色返還行列
	WbRbLevels *wbRbLevels) {	// out ホワイトバランスRB
	FILE *fp;
	if (!(fp = fopen(filePath, "rb"))) {
		perror(filePath);
		return NULL;
	}
	OrfProperties *orfProperties = parse_tiff(fp);
	fseek(fp, orfProperties->stripOffset + 7, SEEK_SET);	// StripOffsetで示される開始位置から、7バイト後へシーク?
	unsigned char *compressed_raw_image = (unsigned char *)malloc(orfProperties->stripByteCounts - 7);
	fread(compressed_raw_image, 1, orfProperties->stripByteCounts, fp);
	fclose(fp);

	unsigned short *raw_image = (unsigned short *)malloc(orfProperties->imageWidth * orfProperties->imageHeight * sizeof(*raw_image));
	decompressRawData(orfProperties->imageWidth, orfProperties->imageHeight, compressed_raw_image, orfProperties->stripByteCounts - 7, raw_image);

	*width = orfProperties->imageWidth;
	*height = orfProperties->imageHeight;
	*blackLevel = *orfProperties->exif->olympusProperties->imageProcessingProperties->blackLevel;
	*colorMatrix = *orfProperties->exif->olympusProperties->imageProcessingProperties->colorMatrix;
	*wbRbLevels = *orfProperties->exif->olympusProperties->imageProcessingProperties->wbRbLevels;

	return raw_image;
}
