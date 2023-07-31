#pragma once
#include "HuffBitBuffer.h"

class RawDecoder {
private:
	HuffBitBuffer &huffBitBuffer;
	unsigned int carry0;
	int carry1;
	int carry2;
public:
	RawDecoder(HuffBitBuffer &huffBitBuffer);
	int getValue();
};
