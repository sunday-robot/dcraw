#include "ColorMatrix.h"

ColorMatrix::ColorMatrix(int e00, int e01, int e02, int e10, int e11, int e12, int e20, int e21, int e22)
{
	e[0][0] = e00;
	e[0][1] = e01;
	e[0][2] = e02;
	e[1][0] = e10;
	e[1][1] = e11;
	e[1][2] = e12;
	e[2][0] = e20;
	e[2][1] = e21;
	e[2][2] = e22;
}


ColorMatrix::~ColorMatrix()
{
}

static int x(int v) {
	if (v > 65535)
		return 65535;
	if (v < 0)
		return 0;
	return v;
}

void ColorMatrix::transForm(
	int r,
	int g,
	int b,
	unsigned short *r2,
	unsigned short *g2,
	unsigned short *b2) const {
	*r2 = x((e[0][0] * r + e[0][1] * g + e[0][2] * b) >> 8);
	*g2 = x((e[1][0] * r + e[1][1] * g + e[1][2] * b) >> 8);
	*b2 = x((e[2][0] * r + e[2][1] * g + e[2][2] * b) >> 8);
}
