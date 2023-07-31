#include "save.h"
#include <stdio.h>
#include <stdlib.h>
#include "gamma.h"

void writeHeader(int mode, int width, int height, int maxIntensity, FILE *fp) {
	fprintf(fp, "P%d\n", mode);
	fprintf(fp, "%d %d\n", width, height);
	fprintf(fp, "%d\n", maxIntensity);
}

// グレイスケール画像をPGM形式で出力する。
void writePgm(
	int width,
	int height,
	int maxIntensity,
	const unsigned short *data,
	FILE *fp)
{
	writeHeader(5, width, height, maxIntensity, fp);	// 5は、グレースケールで、輝度値がバイナリ形式であることを意味する
	unsigned char *row = (unsigned char *)malloc(width * 2);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			unsigned short v = *data++;
			row[x * 2] = v >> 8;
			row[x * 2 + 1] = v & 0xff;
		}
		fwrite(row, 2, width, fp);
	}
}

void writePpm(
	int width,
	int height,
	const unsigned short(*image)[3],
	FILE *fp)
{
	writeHeader(6, width, height, 255, fp);	// 6は、カラーで、輝度値がバイナリ形式であることを意味する

	int curve[0x10000];
	createGammaTable(1 / 2.2, 255, curve, 0x10000);
	unsigned char *ppm = (unsigned char *)malloc(width * 3);
	int soff = 0;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			ppm[x * 3] = curve[image[soff][0]];
			ppm[x * 3 + 1] = curve[image[soff][1]];
			ppm[x * 3 + 2] = curve[image[soff][2]];
			soff++;
		}
		fwrite(ppm, 3, width, fp);
	}
	free(ppm);
}

void save(const char *filePath, const unsigned short(*image)[3], int width, int height) {
	FILE *fp = fopen(filePath, "wb");
	writePpm(width, height, image, fp);
	fclose(fp);
}
