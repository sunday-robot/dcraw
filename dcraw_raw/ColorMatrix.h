#pragma once

class ColorMatrix
{
	int e[3][3];
public:
	ColorMatrix() {}
	ColorMatrix(
		int e00, int e01, int e02,
		int e10, int e11, int e12,
		int e20, int e21, int e22);
	~ColorMatrix();

	void transForm(int r, int g, int b, unsigned short *r2, unsigned short *g2, unsigned short *b2) const;

	void transForm(unsigned short rgb[]) const {
		transForm(rgb[0], rgb[1], rgb[2], rgb, rgb + 1, rgb + 2);
	}
};

