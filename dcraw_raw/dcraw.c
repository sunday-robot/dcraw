#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "load.h"
#include "raw_to_rgb.h"
#include "save.h"
#include "gamma.h"

int histogram[4096];

void init_histogram() {
	memset(histogram, 0, sizeof(histogram));
}

void increment(int index) {
	histogram[index]++;
}

void write_histogram(const char *filePath) {
	FILE *fp = fopen(filePath, "w");
	for (int i = 0; i < 4096; i++) {
		fprintf(fp, "%d\n", histogram[i]);
	}
	fclose(fp);
}

void write_pgm_raw_r_channel(const unsigned short *image, int width, int height, FILE *ofp)
{
	init_histogram();
	int curve[4096];
	//	gamma_curve(1 / 2.2, 255, curve, 4096);
	createGammaTable(1, 255, curve, 4096);
	unsigned char *ppm = (unsigned char *)malloc(width / 2);
	fprintf(ofp, "P5\n");
	fprintf(ofp, "%d %d\n", width / 2, height / 2);
	fprintf(ofp, "255\n");
	for (int row = 0; row < height; row += 2) {
		for (int col = 0; col < width; col += 2) {
			ppm[col / 2] = curve[image[row * width + col]];
			increment(image[row * width + col]);
		}
		fwrite(ppm, 1, width / 2, ofp);
	}
	free(ppm);
	write_histogram("histogram_red.csv");
}

void write_pgm_raw_g_channel(const unsigned short *image, int width, int height, FILE *ofp)
{
	init_histogram();
	int curve[4096];
	//	gamma_curve(1 / 2.2, 255, curve, 4096);
	createGammaTable(1, 255, curve, 4096);
	unsigned char *ppm0 = (unsigned char *)malloc(width);
	unsigned char *ppm1 = (unsigned char *)malloc(width);
	fprintf(ofp, "P5\n");
	fprintf(ofp, "%d %d\n", width, height);
	fprintf(ofp, "255\n");
	for (int row = 0; row < height; row += 2) {
		for (int col = 0; col < width; col += 2) {
			int g0 = curve[image[row * width + col + 1]];
			int g1 = curve[image[(row + 1) * width + col]];
			increment(image[row * width + col + 1]);
			increment(image[(row + 1) * width + col]);
			ppm0[col] = g0;
			ppm0[col + 1] = g0;
			ppm1[col] = g1;
			ppm1[col + 1] = g1;
		}
		fwrite(ppm0, 1, width, ofp);
		fwrite(ppm1, 1, width, ofp);
	}
	free(ppm0);
	free(ppm1);
	write_histogram("histogram_green.csv");
}

void write_pgm_raw_b_channel(const unsigned short *image, int width, int height, FILE *ofp)
{
	init_histogram();
	int curve[4096];
	//	gamma_curve(1 / 2.2, 255, curve, 4096);
	createGammaTable(1, 255, curve, 4096);
	unsigned char *ppm = (unsigned char *)malloc(width / 2);
	fprintf(ofp, "P5\n");
	fprintf(ofp, "%d %d\n", width / 2, height / 2);
	fprintf(ofp, "255\n");
	for (int row = 1; row < height; row += 2) {
		for (int col = 0; col < width; col += 2) {
			ppm[col / 2] = curve[image[row * width + col]];
			increment(image[row * width + col]);
		}
		fwrite(ppm, 1, width / 2, ofp);
	}
	free(ppm);
	write_histogram("histogram_blue.csv");
}

char *createOutputFilePath(const char *inputFilePath) {
	char *ofname = (char *)malloc(strlen(inputFilePath) + 64);
	strcpy(ofname, inputFilePath);
	char *cp;
	if ((cp = strrchr(ofname, '.')))
		*cp = 0;
	strcat(ofname, ".ppm");
	return ofname;
}

int main(int argc, const char **argv)
{
	for (int i = 1; i < argc; i++) {
		unsigned short *raw_image;
		int width;
		int height;
		struct BlackLevel blackLevel;
		ColorMatrix colorMatrix;
		WbRbLevels wbRbLevels;
		raw_image = load(argv[i], &width, &height, &blackLevel, &colorMatrix, &wbRbLevels);
		if (raw_image == NULL)
			continue;

		{
			FILE *ofp;
			ofp = fopen("raw_red.pgm", "wb");
			write_pgm_raw_r_channel(raw_image, width, height, ofp);
			fclose(ofp);
			ofp = fopen("raw_green.pgm", "wb");
			write_pgm_raw_g_channel(raw_image, width, height, ofp);
			fclose(ofp);
			ofp = fopen("raw_blue.pgm", "wb");
			write_pgm_raw_b_channel(raw_image, width, height, ofp);
			fclose(ofp);
		}

		unsigned short(*image)[3];
		convertRawToRgbImage(raw_image, width, height, &blackLevel, &colorMatrix, &image);
		free(raw_image);

		char *outputFilePath = createOutputFilePath(argv[i]);
		save(outputFilePath, image, width, height);
		free(outputFilePath);
		free(image);
	}
	return 0;
}
