#pragma once
#include "BitBuffer.h"

class HuffBitBuffer : public BitBuffer {
public:
	HuffBitBuffer(const unsigned char *bytes, int size);
	unsigned short getHuffBits();
};
