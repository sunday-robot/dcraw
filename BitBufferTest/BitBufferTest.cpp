// BitBufferTest.cpp : コンソール アプリケーションのエントリ ポイントを定義します。
//

#include "../dcraw_raw/BitBuffer.h"
#include <stdio.h>

void testPeekBits(BitBuffer &bitBuffer, int bitCount) {
	unsigned int v = bitBuffer.peekBits(bitCount);
	printf("peekBits(%d) => %u(%x)\n", bitCount, v, v);
}

void testPeekBitsSuit() {
	unsigned char data[] = { 0b11111111, 0b11111111, 0b11111111, 0b11111111 };
	BitBuffer bb(data, sizeof(data));
	for (int i = 1; i <= 16; i++) {
		testPeekBits(bb, i);
	}
}

int main()
{
	testPeekBitsSuit();
	return 0;
}

