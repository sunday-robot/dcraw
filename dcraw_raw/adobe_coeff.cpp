#include "adobe_coeff.h"
#include <string.h>

const double XYZ_FROM_RGB[3][3] = {			/* XYZ from RGB */
	{ 0.412453, 0.357580, 0.180423 },
	{ 0.212671, 0.715160, 0.072169 },
	{ 0.019334, 0.119193, 0.950227 } };

void pseudoinverse(const double in[3][3], double out[3][3])
{
	double work[3][6], num;
	int i, j, k;

	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++)
			for (k = 0; k < 3; k++)
				work[i][j] += in[k][i] * in[k][j];
		for (j = 3; j < 6; j++)
			work[i][j] = j == i + 3;
	}
	for (i = 0; i < 3; i++) {
		num = work[i][i];
		for (j = 0; j < 6; j++)
			work[i][j] /= num;
		for (k = 0; k < 3; k++) {
			if (k == i)
				continue;
			num = work[k][i];
			for (j = 0; j < 6; j++)
				work[k][j] -= work[i][j] * num;
		}
	}
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			for (out[i][j] = k = 0; k < 3; k++)
				out[i][j] += work[j][k + 3] * in[i][k];
}

// 行列の乗算
// A * B -> C
void matrix_multiply(const double a[3][3], const double b[3][3], double c[3][3]) {
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			double v = 0;
			for (int k = 0; k < 3; k++) {
				v += a[i][k] * b[k][j];
			}
			c[i][j] = v;
		}
	}
}

// 各行の合計値がそれぞれ1となるように、変換する
// m[i][j] /= m[i][0] + m[i][1] + m[i][2]
void matrix_normalize(const double a[3][3], double b[3][3]) {
	for (int i = 0; i < 3; i++) {
		double v = 0;
		for (int j = 0; j < 3; j++)
			v += a[i][j];
		for (int j = 0; j < 3; j++)
			b[i][j] = a[i][j] / v;
	}
}

// 行列の転置
// transpose(A) -> B
void matrix_transpose(const double a[3][3], double b[3][3]) {
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			b[i][j] = a[j][i];
}

double sum(double elements[], int elementCount) {
	double sum = 0;
	for (int i = 0; i < elementCount; i++) {
		sum += elements[i];
	}
	return sum;
}

void cam_xyz_coeff(const double adobe_cam_xyz[3][3], struct RgbCoefficient *rgbCoefficient, double rgb_cam[3][3])
{
	double cam_rgb[3][3];
	matrix_multiply(adobe_cam_xyz, XYZ_FROM_RGB, cam_rgb);

	rgbCoefficient->r = 1 / sum(cam_rgb[0], 3);
	rgbCoefficient->g = 1 / sum(cam_rgb[1], 3);
	rgbCoefficient->b = 1 / sum(cam_rgb[2], 3);

	matrix_normalize(cam_rgb, cam_rgb);

	double inverse[3][3];
	pseudoinverse(cam_rgb, inverse);
	matrix_transpose(inverse, rgb_cam);
}

/*
All matrices are from Adobe DNG Converter unless otherwise noted.
*/
void adobe_coeff(
	const char *model,				// カメラの製品名
	struct RgbCoefficient *rgbCoefficient,	// [out] カメラのRGBの各輝度値の係数
	double rgb_cam[][3])			// [out] ?
{
	static const struct {
		const char *model;	// カメラの製品名(キー)
		short black;		// ?使っていない
		short maximum;		// ?使っていない
		short trans[3 * 3];	// ?
	} table[] = {
		{ "C5050", 0, 0,
		{ 10508,-3124,-1273,-6079,14294,1901,-1653,2306,6237 } },
		{ "C5060", 0, 0,
		{ 10445,-3362,-1307,-7662,15690,2058,-1135,1176,7602 } },
		{ "C7070", 0, 0,
		{ 10252,-3531,-1095,-7114,14850,2436,-1451,1723,6365 } },
		{ "C70", 0, 0,
		{ 10793,-3791,-1146,-7498,15177,2488,-1390,1577,7321 } },
		{ "C80", 0, 0,
		{ 8606,-2509,-1014,-8238,15714,2703,-942,979,7760 } },
		{ "E-10", 0, 0xffc,
		{ 12745,-4500,-1416,-6062,14542,1580,-1934,2256,6603 } },
		{ "E-1", 0, 0,
		{ 11846,-4767,-945,-7027,15878,1089,-2699,4122,8311 } },
		{ "E-20", 0, 0xffc,
		{ 13173,-4732,-1499,-5807,14036,1895,-2045,2452,7142 } },
		{ "E-300", 0, 0,
		{ 7828,-1761,-348,-5788,14071,1830,-2853,4518,6557 } },
		{ "E-330", 0, 0,
		{ 8961,-2473,-1084,-7979,15990,2067,-2319,3035,8249 } },
		{ "E-30", 0, 0xfbc,
		{ 8144,-1861,-1111,-7763,15894,1929,-1865,2542,7607 } },
		{ "E-3", 0, 0xf99,
		{ 9487,-2875,-1115,-7533,15606,2010,-1618,2100,7389 } },
		{ "E-400", 0, 0,
		{ 6169,-1483,-21,-7107,14761,2536,-2904,3580,8568 } },
		{ "E-410", 0, 0xf6a,
		{ 8856,-2582,-1026,-7761,15766,2082,-2009,2575,7469 } },
		{ "E-420", 0, 0xfd7,
		{ 8746,-2425,-1095,-7594,15612,2073,-1780,2309,7416 } },
		{ "E-450", 0, 0xfd2,
		{ 8745,-2425,-1095,-7594,15613,2073,-1780,2309,7416 } },
		{ "E-500", 0, 0,
		{ 8136,-1968,-299,-5481,13742,1871,-2556,4205,6630 } },
		{ "E-510", 0, 0xf6a,
		{ 8785,-2529,-1033,-7639,15624,2112,-1783,2300,7817 } },
		{ "E-520", 0, 0xfd2,
		{ 8344,-2322,-1020,-7596,15635,2048,-1748,2269,7287 } },
		{ "E-5", 0, 0xeec,
		{ 11200,-3783,-1325,-4576,12593,2206,-695,1742,7504 } },
		{ "E-600", 0, 0xfaf,
		{ 8453,-2198,-1092,-7609,15681,2008,-1725,2337,7824 } },
		{ "E-620", 0, 0xfaf,
		{ 8453,-2198,-1092,-7609,15681,2008,-1725,2337,7824 } },
		{ "E-P1", 0, 0xffd,
		{ 8343,-2050,-1021,-7715,15705,2103,-1831,2380,8235 } },
		{ "E-P2", 0, 0xffd,
		{ 8343,-2050,-1021,-7715,15705,2103,-1831,2380,8235 } },
		{ "E-P3", 0, 0,
		{ 7575,-2159,-571,-3722,11341,2725,-1434,2819,6271 } },
		{ "E-P5", 0, 0,
		{ 8380,-2630,-639,-2887,10725,2496,-627,1427,5438 } },
		{ "E-PL1s", 0, 0,
		{ 11409,-3872,-1393,-4572,12757,2003,-709,1810,7415 } },
		{ "E-PL1", 0, 0,
		{ 11408,-4289,-1215,-4286,12385,2118,-387,1467,7787 } },
		{ "E-PL2", 0, 0xcf3,
		{ 15030,-5552,-1806,-3987,12387,1767,-592,1670,7023 } },
		{ "E-PL3", 0, 0,
		{ 7575,-2159,-571,-3722,11341,2725,-1434,2819,6271 } },
		{ "E-PL5", 0, 0xfcb,
		{ 8380,-2630,-639,-2887,10725,2496,-627,1427,5438 } },
		{ "E-PL6", 0, 0,
		{ 8380,-2630,-639,-2887,10725,2496,-627,1427,5438 } },
		{ "E-PL7", 0, 0,
		{ 9197,-3190,-659,-2606,10830,2039,-458,1250,5458 } },
		{ "E-PM1", 0, 0,
		{ 7575,-2159,-571,-3722,11341,2725,-1434,2819,6271 } },
		{ "E-PM2", 0, 0,
		{ 8380,-2630,-639,-2887,10725,2496,-627,1427,5438 } },
		{ "E-M10", 0, 0,
		{ 8380,-2630,-639,-2887,10725,2496,-627,1427,5438 } },
		{ "E-M1", 0, 0,
		{ 7687,-1984,-606,-4327,11928,2721,-1381,2339,6452 } },
		{ "E-M5MarkII", 0, 0,
		{ 9422,-3258,-711,-2655,10898,2015,-512,1354,5512 } },
		{ "E-M5", 0, 0xfe1,
		{ 8380,-2630,-639,-2887,10725,2496,-627,1427,5438 } },
		{ "SH-2", 0, 0,
		{ 10156,-3425,-1077,-2611,11177,1624,-385,1592,5080 } },
		{ "SP350", 0, 0,
		{ 12078,-4836,-1069,-6671,14306,2578,-786,939,7418 } },
		{ "SP3", 0, 0,
		{ 11766,-4445,-1067,-6901,14421,2707,-1029,1217,7572 } },
		{ "SP500UZ", 0, 0xfff,
		{ 9493,-3415,-666,-5211,12334,3260,-1548,2262,6482 } },
		{ "SP510UZ", 0, 0xffe,
		{ 10593,-3607,-1010,-5881,13127,3084,-1200,1805,6721 } },
		{ "SP550UZ", 0, 0xffe,
		{ 11597,-4006,-1049,-5432,12799,2957,-1029,1750,6516 } },
		{ "SP560UZ", 0, 0xff9,
		{ 10915,-3677,-982,-5587,12986,2911,-1168,1968,6223 } },
		{ "SP570UZ", 0, 0,
		{ 11522,-4044,-1146,-4736,12172,2904,-988,1829,6039 } },
		{ "STYLUS1", 0, 0,
		{ 8360,-2420,-880,-3928,12353,1739,-1381,2416,5173 } },
		{ "TG-4", 0, 0,
		{ 11426,-4159,-1126,-2066,10678,1593,-120,1327,4998 } },
		{ "XZ-10", 0, 0,
		{ 9777,-3483,-925,-2886,11297,1800,-602,1663,5134 } },
		{ "XZ-1", 0, 0,
		{ 10901,-4095,-1074,-1141,9208,2293,-62,1417,5158 } },
		{ "XZ-2", 0, 0,
		{ 9777,-3483,-925,-2886,11297,1800,-602,1663,5134 } },
	};

	for (int i = 0; i < sizeof table / sizeof *table; i++)
		if (strncmp(model, table[i].model, strlen(table[i].model)) == 0) {
			double adobe_cam_xyz[3][3];
			for (int j = 0; j < 3 * 3; j++)
				adobe_cam_xyz[j / 3][j % 3] = table[i].trans[j] / 10000.0;
			cam_xyz_coeff(adobe_cam_xyz, rgbCoefficient, rgb_cam);
			return;
		}
}
