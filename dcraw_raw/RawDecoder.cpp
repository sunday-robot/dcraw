#include "RawDecoder.h"

RawDecoder::RawDecoder(HuffBitBuffer &huffBitBuffer) : huffBitBuffer(huffBitBuffer) {
	carry0 = 0;
	carry1 = 0;
	carry2 = 0;
}

static int getNBits(unsigned int carry0, int carry2) {
	int nbits;
	unsigned short int c;
	if (carry2 < 3) {
		nbits = 4;
		c = carry0 >> 2;
	} else {
		nbits = 2;
		c = carry0;
	}
	for (; (c >> nbits) != 0; nbits++)
		;
	return nbits;
}

int RawDecoder::getValue() {
	int nbits = getNBits(carry0, carry2);
	int bits3 = huffBitBuffer.getBits3();
	int sign = ((bits3 & 0b100) != 0) ? -1 : 0;
	int low = bits3 & 0b011;
	int tmp1 = huffBitBuffer.getHuffBits();
	int tmp2 = (tmp1 == 12) ? (huffBitBuffer.getBits(16 - nbits) >> 1) : tmp1;
	int new_carry0 = (tmp2 << nbits) | huffBitBuffer.getBits(nbits);
	int value = (((new_carry0 ^ sign) + carry1) << 2) | low;

	carry0 = new_carry0;
	carry1 = ((new_carry0 ^ sign) * 3 + carry1 * 4) >> 5;
	carry2 = new_carry0 > 16 ? 0 : carry2 + 1;

	return value;
}
