#pragma once

class BitBuffer {
public:
	BitBuffer(const unsigned char *bytes, int size);
	unsigned int getBit();
	unsigned int getBits2();
	unsigned int getBits3();
	unsigned int getBits(int bitCount);
	unsigned int peekBits12() const;
	unsigned int peekBits(int bitCount) const;
	inline void progress(int bitCount) {
		int tmp = bitPosition + bitCount;
		bitPosition = tmp % 8;
		bytePosition += tmp / 8;
	}
private:
	const unsigned char *bytes;
	const int size;
	int bitPosition;	// 0...7
	int bytePosition;	// 0...size-1
};
