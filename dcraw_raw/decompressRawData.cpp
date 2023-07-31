#include "decompressRawData.h"
#include "HuffBitBuffer.h"
#include "RawDecoder.h"

#define ABS(x) (((int)(x) ^ ((int)(x) >> 31)) - ((int)(x) >> 31))

#define RAW(row,col) \
	raw_image[(row)*width+(col)]


// (row, col)の画素値の予測値を、上(row - 2, col)、左上(row -2, col - 2)、左(row, col -2)の画素値から計算する。
static int get_pred(unsigned short *raw_image, int width, int row, int col) {
	if (row < 2) {
		if (col < 2)
			return 0;	// 左上の2x2の領域については手掛かりがないので0を返す。
		return  RAW(row, col - 2);	// 上の画素の値を返す。
	}
	if (col < 2)
		return RAW(row - 2, col);	// 左の画素の値を返す。
									// 上、左上、左の画素の値から予想画素値を計算する。
	int nw = RAW(row - 2, col - 2);	// north west, 左上
	int n = RAW(row - 2, col);	// north, 上
	int w = RAW(row, col - 2);	// west,左
	int w_minus_nw = w - nw;
	int n_minus_nw = n - nw;
	int abs_w_minus_nw = ABS(w_minus_nw);
	int abs_n_minus_nw = ABS(n_minus_nw);
	if ((w_minus_nw < 0 && n_minus_nw > 0) || (n_minus_nw < 0 && w_minus_nw > 0)) {
		// nwがnとwの中間である場合
		if (abs_w_minus_nw <= 32 && abs_n_minus_nw <= 32)
			return (w + n) / 2;	// 3者が近い場合は、nwは使用せず、nとwの値の平均値を返す。
		return w + n - nw;	// 3者が少し離れている場合は、この値(説明できず…)を返す。
	} else {
		// nwが一番大きいか、一番小さい場合は、nとwのうち、nwの対極にある方を返す。
		if (abs_w_minus_nw > abs_n_minus_nw)
			return  w;
		return n;
	}
}

void decompressRawData(int width, int height, const unsigned char *compressedRawImage, int compressedRawImageSize, unsigned short *decompressedRawImage) {
	HuffBitBuffer huffBitBuffer(compressedRawImage, compressedRawImageSize);
	for (int row = 0; row < height; row++) {
		RawDecoder rawDecoder[2] = { RawDecoder(huffBitBuffer), RawDecoder(huffBitBuffer) };
		for (int col = 0; col < width; col++) {
			int pred = get_pred(decompressedRawImage, width, row, col);	// 左上、上、左の画素値からの予測値
			int difference = rawDecoder[col & 1].getValue();	// 予測値との差異
			decompressedRawImage[row * width + col] = pred + difference;
		}
	}
}

