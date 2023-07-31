#include <stdio.h>

static unsigned short raw_width;
static unsigned short *raw_image;
static unsigned short width;
static unsigned short height;
static int raw_height = 2504;

typedef signed __int64 INT64;


#define RAW(row,col) \
	raw_image[(row)*raw_width+(col)]

static const char *ifname;
//unsigned zero_after_ff;
static unsigned data_error;
static int top_margin = 0;
static int left_margin = 0;
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



class BitBuffer {
private:
	FILE *fp;
	unsigned bitBuffer = 0;	// 32個のbitのバッファ。
	int bitBufferLength = 0;// bitBufferの長さ	

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
	BitBuffer(FILE *fp) {
		this->fp = fp;
	}

	// bitLengthビットのデータをキャッシュあるいはファイルから取得し、返す
	unsigned getBits(int bitCount) {
		unsigned c = fetchBits(bitCount);
		bitBufferLength -= bitCount;
		return c;
	}

};

void packed_load_raw(FILE *ifp) {
	BitBuffer bitBuffer(ifp);
	//	int vbits = 0;

	unsigned __int64 bitbuf = 0;


	//	int bwide = raw_width * 12 /*tiff_bps*/ / 8;
	//	bwide += bwide & 0x51/*load_flags*/ >> 7;
	int rbits = (raw_width * 12 / 8) * 8 - raw_width * 12/*tiff_bps*/;
#if 0
	//	if (0x51/*load_flags*/ & 1)
	//		bwide = bwide * 16 / 15;
	//	int bite = 8 + 16;// (0x51/*load_flags*/ & 0x18);
	//	int half = (raw_height + 1) >> 1;
	printf("current position = %08x\n", ftell(ifp));
#endif
	for (int irow = 0; irow < raw_height; irow++) {
#if 0
		int row = irow;
		if ((0x51/*load_flags*/ & 2) != 0) {
			row = irow % half * 2 + irow / half;
			if ((row == 1)
				&& ((load_flags & 4) != 0)) {
				if (vbits = 0, tiff_compress)
					fseek(ifp, data_offset - (-half*bwide & -2048), SEEK_SET);
				else {
					fseek(ifp, 0, SEEK_END);
					fseek(ifp, ftell(ifp) >> 3 << 2, SEEK_SET);
				}
			}
		}
#endif
		for (int col = 0; col < raw_width; col++) {
#if 0
			for (vbits -= 12/*tiff_bps*/; vbits < 0; vbits += 24/*bite*/) {
#if 0
				bitbuf <<= 24/*bite*/;
				for (int i = 0; i < 24/*bite*/; i += 8)
					bitbuf |= (unsigned)(fgetc(ifp) << i);
#endif
				unsigned char b0 = (unsigned char)fgetc(ifp);
				unsigned char b1 = (unsigned char)fgetc(ifp);
				unsigned char b2 = (unsigned char)fgetc(ifp);
				bitbuf = b0 | (b1 << 8) | (b2 << 16) | (bitbuf << 24);
			}
			//			int val = bitbuf << (64 - 12/*tiff_bps*/ - vbits) >> (64 - 12/*tiff_bps*/);
			int val = bitbuf << (52 - vbits) >> 52;
#endif
			int val = bitBuffer.getBits(12);
			RAW(irow, col ^ (0x51/*load_flags*/ >> 6 & 1)) = val;
			if (0x51/*load_flags*/ & 1
				&& (col % 10) == 9
				&& fgetc(ifp)
				&& irow < height + top_margin && col < width + left_margin)
				derror(ifp);
		}
		//		vbits -= rbits;
		bitBuffer.getBits(rbits);
	}
}
