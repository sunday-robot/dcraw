#include "BitBuffer.h"

BitBuffer::BitBuffer(
	const unsigned char *bytes,
	int size)
	:
	bytes(bytes),
	size(size)
{
	this->bitPosition = 0;
	this->bytePosition = 0;
}

// 指定されたビット数のデータを取得し、返す。
unsigned int BitBuffer::peekBits(	// 0...65535
	int bitCount) const				// ビット数(1...16)
{
	int x = bitPosition + bitCount;

	if (x <= 8) {
		// 現在のバイト位置に収まる場合
		unsigned char b0 = bytes[bytePosition] & (0xff >> bitPosition);
		return b0 >> (8 - x);
	}
	if (x <= 16) {
		// 2バイトにまたがる場合
		unsigned char b0 = bytes[bytePosition] & (0xff >> bitPosition);
		unsigned char b1 = bytes[bytePosition + 1];
		return ((b0 << 8) | b1) >> (16 - x);
	}
	// 3バイトにまたがる場合
	unsigned char b0 = bytes[bytePosition] & (0xff >> bitPosition);
	unsigned char b1 = bytes[bytePosition + 1];
	unsigned char b2 = bytes[bytePosition + 2];
	return ((b0 << 16) | (b1 << 8) | b2) >> (24 - x);
}

// 1ビットのデータを取り出し、ビット位置を進める。
unsigned int BitBuffer::getBit()
{
	unsigned int r = (bytes[bytePosition] >> (7 - bitPosition)) & 1;
	bytePosition += (bitPosition + 1) / 8;
	bitPosition = (bitPosition + 1) & 7;
	return r;
}

// 2ビットのデータを取り出し、ビット位置を進める。
unsigned int BitBuffer::getBits2()
{
	unsigned int r;
	if (bitPosition <= 6) {
		// 現在のバイト位置に収まる場合
		r = (bytes[bytePosition] >> (6 - bitPosition)) & 3;
	} else {
		// 2バイトにまたがる場合
		r = (((bytes[bytePosition] << 8) | bytes[bytePosition + 1]) >> 7) & 3;
	}
	bytePosition += (bitPosition + 2) / 8;
	bitPosition = (bitPosition + 2) & 7;
	return r;
}

// 3ビットのデータを取り出し、ビット位置を進める。
unsigned int BitBuffer::getBits3()
{
	unsigned int r;
	if (bitPosition <= 5) {
		// 現在のバイト位置に収まる場合
		r = (bytes[bytePosition] >> (5 - bitPosition)) & 0b111;
	} else {
		// 2バイトにまたがる場合
		r = (((bytes[bytePosition] << 8) | bytes[bytePosition + 1]) >> (13 - bitPosition)) & 0b111;
	}
	bytePosition += (bitPosition + 3) / 8;
	bitPosition = (bitPosition + 3) & 7;
	return r;
}

// 12ビットのデータを取り出し、ビット位置を進める。
unsigned int BitBuffer::peekBits12() const
{
	int x = bitPosition + 12;

	if (x <= 16) {
		// 2バイトにまたがる場合
		unsigned char b0 = bytes[bytePosition];
		unsigned char b1 = bytes[bytePosition + 1];
		return (((b0 << 8) | b1) >> (16 - x)) & 0xfff;
	}
	// 3バイトにまたがる場合
	unsigned char b0 = bytes[bytePosition];
	unsigned char b1 = bytes[bytePosition + 1];
	unsigned char b2 = bytes[bytePosition + 2];
	return (((b0 << 16) | (b1 << 8) | b2) >> (24 - x)) & 0xfff;
}

// 指定されたビット数のデータを取り出し、ビット位置を進める。
unsigned int BitBuffer::getBits(
	int bitCount)	// ビット数(1...16)
{
	const int bits = peekBits(bitCount);
	progress(bitCount);
	return bits;
}
