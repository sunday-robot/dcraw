#include "gamma.h"
#include <math.h>

void createGammaTable(double pwr, int maximumOutputIntensity, int table[], int tableSize)
{
	double k = 1.0 / (tableSize - 1);
	for (int i = 0; i < tableSize; i++) {
		double r = i * k;
		table[i] = (int)(maximumOutputIntensity * pow(r, pwr));
	}
}
