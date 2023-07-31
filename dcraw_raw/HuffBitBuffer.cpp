#include "HuffBitBuffer.h"

static unsigned char huff_nbits[4096] = { 0 };
static unsigned char huff_value[4096];

static void make_huff_table()
{
	huff_nbits[0] = 0x0c;
	huff_value[0] = 0x0c;
	int n = 1;
	int nbits = 0x0c;
	for (int cnt = 1; cnt < 4096; cnt <<= 1) {
		for (int i = 0; i < cnt; i++) {
			huff_nbits[n] = nbits;
			huff_value[n] = nbits - 1;
			n++;
		}
		nbits--;
	}
}

HuffBitBuffer::HuffBitBuffer(const unsigned char *bytes, int size) : BitBuffer(bytes, size)
{
	if (huff_nbits[0] == 0) {
		make_huff_table();
	}
}

unsigned short HuffBitBuffer::getHuffBits()
{
	const int huffIndex = peekBits12();
	const int huffBits = huff_nbits[huffIndex];
	progress(huffBits);
	return huff_value[huffIndex];
}
