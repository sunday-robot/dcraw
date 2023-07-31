// dcraw.cpp : コンソール アプリケーションのエントリ ポイントを定義します。
//

#include "stdafx.h"
#include <string.h>

#define RAW(row,col) \
	raw_image[(row) * width + (col)]
#define ABS(x) (((int)(x) ^ ((int)(x) >> 31)) - ((int)(x) >> 31))

typedef signed __int64 INT64;

extern unsigned short *raw_image;

static const char *ifname;
static unsigned data_error;

static void derror(FILE *ifp) {
	if (!data_error) {
		fprintf(stderr, "%s: ", ifname);
		if (feof(ifp))
			fprintf(stderr, "Unexpected end of file\n");
		else
			fprintf(stderr, "Corrupt data near 0x%llx\n", (INT64)ftell(ifp));
	}
	data_error++;
}

class BitHuff {
private:
	FILE *fp;
	unsigned short int huffTable[4096];
	unsigned bitBuffer = 0;	// 32個のbitのバッファ。
	int bitBufferLength = 0;// bitBufferの長さ	

	void initialize_huff() {
		huffTable[0] = 0xc0c;
		int n = 1;
		for (int i = 12; i--;)
			for (int c = 0; c < 2048 >> i; c++)
				huffTable[n++] = (i + 1) << 8 | i;
	}

	void cacheBits(int bitLength) {
		while (bitBufferLength < bitLength) {
			int c = fgetc(fp);	// EOFチェックしていないことに注意!
			bitBuffer = (bitBuffer << 8) | (c & 0xff);
			bitBufferLength += 8;
		}
	}

	int fetchBits(int bitCount) {
		cacheBits(bitCount);
		unsigned c = bitBuffer << (32 - bitBufferLength) >> (32 - bitCount);
		return c;
	}

public:
	BitHuff(FILE *fp) {
		this->fp = fp;
		initialize_huff();
	}

	// bitLengthビットのデータをキャッシュあるいはファイルから取得し、返す
	unsigned getBits(int bitCount) {
		unsigned c = fetchBits(bitCount);
		bitBufferLength -= bitCount;
		return c;
	}

	unsigned getbithuff(int bitCount) {
		unsigned c = fetchBits(bitCount);
		bitBufferLength -= huffTable[c] >> 8;
		c = (unsigned char)huffTable[c];
		return c;
	}

	// sign...0 or 1
	// low...0～3
	void getSignAndLow(int *sign, int *low) {
		int b3 = getBits(3);
		*sign = b3 >> 2;
		*low = b3 & 3;
	}
};

#if 0
int get_nbits(int zero_or_two, int c) {
	int nbits;
	for (nbits = 2 + zero_or_two; (unsigned short)c >> (nbits + zero_or_two); nbits++)
		;
	return nbits;
}
#else
static int getHighestTrueBitPosition(unsigned int c) {
	int bitPosition = 0;
	for (; c != 0; c >>= 1)
		bitPosition++;
	return bitPosition;
}

#endif

static int getPred(int width, int height, int col, int row) {
	if (row < 2) {
		if (col < 2)
			return 0;
		return RAW(row, col - 2);
	}
	if (col < 2)
		return RAW(row - 2, col);

	int w = RAW(row, col - 2);	// west
	int n = RAW(row - 2, col);	// north
	int nw = RAW(row - 2, col - 2);	// north west

	if ((w < nw && nw < n) || (n < nw && nw < w)) {
		// nwが最大でも最小でもない場合
		if (ABS(w - nw) > 32 || ABS(n - nw) > 32)
			return w + n - nw;
		else
			return (w + n) >> 1;
	} else {
		// nwが最小あるいは最大の場合、wとnのうち、nwとの差異が大きいほうを返す。
		return ABS(w - nw) > ABS(n - nw) ? w : n;
	}
}

class Carry {
public:
	int c0;
	int c1;
	int c2;

	Carry() {
		c0 = 0;
		c1 = 0;
		c2 = 0;
	}
};

extern "C" void olympus_load_raw_internal(int width, int height, FILE *ifp) {
	fseek(ifp, 7, SEEK_CUR);	// STRIP_OFFSETが、現在位置なので、これよりも7バイト後に移動する?
	BitHuff bithuff(ifp);
	for (int row = 0; row < height; row++) {
		Carry acarry[] = { Carry(), Carry() };
		for (int col = 0; col < width; col++) {
			Carry *carry = &acarry[col & 1];

			int sign;
			int low;
			bithuff.getSignAndLow(&sign, &low);

			int nbits;
			if (carry->c2 < 3) {
				nbits = 4 + getHighestTrueBitPosition((carry->c0 & 0xffff) >> 6);
			} else {
				nbits = 2 + getHighestTrueBitPosition((carry->c0 & 0xffff) >> 2);
			}

			int high = bithuff.getbithuff(12);
			if (high == 12) {
				high = bithuff.getBits(16 - nbits) >> 1;
			}
			int newC0 = (high << nbits) | bithuff.getbithuff(nbits);
			int diff = (newC0 ^ sign) + carry->c1;

			int pred = getPred(width, height, col, row);

			RAW(row, col) = pred + ((diff << 2) | low);

			carry->c0 = newC0;
			carry->c1 = (diff * 3 + carry->c1) >> 5;
			if (newC0 > 16)
				carry->c2 = 0;
			else
				carry->c2++;
		}
	}
}
