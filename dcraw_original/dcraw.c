#include <io.h>
#include <direct.h>
/*
   dcraw.c -- Dave Coffin's raw photo decoder
   Copyright 1997-2016 by Dave Coffin, dcoffin a cybercom o net

   This is a command-line ANSI C program to convert raw photos from
   any digital camera on any computer running any operating system.

   No license is required to download and use dcraw.c.  However,
   to lawfully redistribute dcraw, you must either (a) offer, at
   no extra charge, full source code* for all executable files
   containing RESTRICTED functions, (b) distribute this code under
   the GPL Version 2 or later, (c) remove all RESTRICTED functions,
   re-implement them, or copy them from an earlier, unrestricted
   Revision of dcraw.c, or (d) purchase a license from the author.

   The functions that process Foveon images have been RESTRICTED
   since Revision 1.237.  All other code remains free for all uses.

   *If you have not modified dcraw.c in any way, a link to my
   homepage qualifies as "full source code".

   $Revision: 1.477 $
   $Date: 2016/05/10 21:30:43 $
 */

#define DCRAW_VERSION "9.27"

#define _GNU_SOURCE
#define _USE_MATH_DEFINES
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

#define fseeko fseek
#define ftello ftell
#include <sys/utime.h>
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#define snprintf _snprintf
#define strcasecmp stricmp
#define strncasecmp strnicmp
typedef __int64 INT64;
typedef unsigned __int64 UINT64;

#define _(String) (String)

#define uchar unsigned char
#define ushort unsigned short

/*
   All global variables are defined here, and all functions that
   access them are prefixed with "CLASS".  Note that a thread-safe
   C++ class cannot have non-const static local variables.
 */
FILE *ifp, *ofp;
short order;
const char *ifname;
char *meta_data, xtrans[6][6], xtrans_abs[6][6];
char cdesc[5], desc[512], model[64], model2[64], artist[64];
float flash_used, canon_ev, iso_speed, shutter, aperture, focal_len;
time_t timestamp;
off_t strip_offset, data_offset;
off_t thumb_offset, meta_offset, profile_offset;
unsigned shot_order, exif_cfa, unique_id;
unsigned thumb_length, meta_length, profile_length;
unsigned thumb_misc, *oprof, shot_select = 0, multi_out = 0;
unsigned tiff_nifds, tiff_samples, tiff_bps, tiff_compress;
unsigned black, maximum, mix_green, raw_color, zero_is_bad;
unsigned zero_after_ff, is_raw, dng_version, is_foveon, data_error;
unsigned tile_width, tile_length, gpsdata[32], load_flags;
unsigned flip, tiff_flip, filters, colors;
ushort raw_height, raw_width, height, width, top_margin, left_margin;
ushort shrink, iheight, iwidth, thumb_width, thumb_height;
ushort *raw_image, (*image)[4], cblack[4102];
ushort white[8][8], curve[0x10000], cr2_slice[3], sraw_mul[4];
double pixel_aspect, aber[4] = { 1,1,1,1 }, gamm[6] = { 0.45,4.5,0,0,0,0 };
float bright = 1, user_mul[4] = { 0,0,0,0 }, threshold = 0;
int mask[8][4];
int half_size = 0, four_color_rgb = 0, document_mode = 0, highlight = 0;
int verbose = 0, use_auto_wb = 0, use_camera_wb = 0, use_camera_matrix = 1;
int output_color = 1, output_bps = 8, output_tiff = 0, med_passes = 0;
int no_auto_bright = 0;
unsigned greybox[4] = { 0, 0, UINT_MAX, UINT_MAX };
float cam_mul[4], pre_mul[4], cmatrix[3][4], rgb_cam[3][4];
const double xyz_rgb[3][3] = {			/* XYZ from RGB */
  { 0.412453, 0.357580, 0.180423 },
  { 0.212671, 0.715160, 0.072169 },
  { 0.019334, 0.119193, 0.950227 } };
const float d65_white[3] = { 0.950456, 1, 1.088754 };
int histogram[4][0x2000];
//void(*write_thumb)();
void(*write_fun)();
void(*load_raw)(), (*thumb_load_raw)();
jmp_buf failure;

struct decode {
	struct decode *branch[2];
	int leaf;
} first_decode[2048], *second_decode, *free_decode;

struct tiff_ifd {
	int width, height, bps, comp, phint, offset, flip, samples, bytes;
	int tile_width, tile_length;
	float shutter;
} tiff_ifd[10];

struct ph1 {
	int format, key_off, tag_21a;
	int black, split_col, black_col, split_row, black_row;
	float tag_210;
} ph1;

#define CLASS

#define FORC(cnt) for (c=0; c < cnt; c++)
#define FORC3 FORC(3)
#define FORC4 FORC(4)
#define FORCC FORC(colors)

#define SQR(x) ((x)*(x))
#define ABS(x) (((int)(x) ^ ((int)(x) >> 31)) - ((int)(x) >> 31))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define LIM(x,min,max) MAX(min,MIN(x,max))
#define ULIM(x,y,z) ((y) < (z) ? LIM(x,y,z) : LIM(x,z,y))
#define CLIP(x) LIM((int)(x),0,65535)
#define SWAP(a,b) { a=a+b; b=a-b; a=a-b; }

/*
   In order to inline this calculation, I make the risky
   assumption that all filter patterns can be described
   by a repeating pattern of eight rows and two columns

   Do not use the FC or BAYER macros with the Leaf CatchLight,
   because its pattern is 16x16, not 2x8.

   Return values are either 0/1/2/3 = G/M/C/Y or 0/1/2/3 = R/G1/B/G2

	PowerShot 600	PowerShot A50	PowerShot Pro70	Pro90 & G1
	0xe1e4e1e4:	0x1b4e4b1e:	0x1e4b4e1b:	0xb4b4b4b4:

	  0 1 2 3 4 5	  0 1 2 3 4 5	  0 1 2 3 4 5	  0 1 2 3 4 5
	0 G M G M G M	0 C Y C Y C Y	0 Y C Y C Y C	0 G M G M G M
	1 C Y C Y C Y	1 M G M G M G	1 M G M G M G	1 Y C Y C Y C
	2 M G M G M G	2 Y C Y C Y C	2 C Y C Y C Y
	3 C Y C Y C Y	3 G M G M G M	3 G M G M G M
			4 C Y C Y C Y	4 Y C Y C Y C
	PowerShot A5	5 G M G M G M	5 G M G M G M
	0x1e4e1e4e:	6 Y C Y C Y C	6 C Y C Y C Y
			7 M G M G M G	7 M G M G M G
	  0 1 2 3 4 5
	0 C Y C Y C Y
	1 G M G M G M
	2 C Y C Y C Y
	3 M G M G M G

   All RGB cameras use one of these Bayer grids:

	0x16161616:	0x61616161:	0x49494949:	0x94949494:

	  0 1 2 3 4 5	  0 1 2 3 4 5	  0 1 2 3 4 5	  0 1 2 3 4 5
	0 B G B G B G	0 G R G R G R	0 G B G B G B	0 R G R G R G
	1 G R G R G R	1 B G B G B G	1 R G R G R G	1 G B G B G B
	2 B G B G B G	2 G R G R G R	2 G B G B G B	2 R G R G R G
	3 G R G R G R	3 B G B G B G	3 R G R G R G	3 G B G B G B
 */

#define RAW(row,col) \
	raw_image[(row)*raw_width+(col)]

#define FC(row,col) \
	(filters >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3)

#define BAYER(row,col) \
	image[((row) >> shrink)*iwidth + ((col) >> shrink)][FC(row,col)]

#define BAYER2(row,col) \
	image[((row) >> shrink)*iwidth + ((col) >> shrink)][fcol(row,col)]

int CLASS fcol(int row, int col)
{
	static const char filter[16][16] =
	{ { 2,1,1,3,2,3,2,0,3,2,3,0,1,2,1,0 },
	  { 0,3,0,2,0,1,3,1,0,1,1,2,0,3,3,2 },
	  { 2,3,3,2,3,1,1,3,3,1,2,1,2,0,0,3 },
	  { 0,1,0,1,0,2,0,2,2,0,3,0,1,3,2,1 },
	  { 3,1,1,2,0,1,0,2,1,3,1,3,0,1,3,0 },
	  { 2,0,0,3,3,2,3,1,2,0,2,0,3,2,2,1 },
	  { 2,3,3,1,2,1,2,1,2,1,1,2,3,0,0,1 },
	  { 1,0,0,2,3,0,0,3,0,3,0,3,2,1,2,3 },
	  { 2,3,3,1,1,2,1,0,3,2,3,0,2,3,1,3 },
	  { 1,0,2,0,3,0,3,2,0,1,1,2,0,1,0,2 },
	  { 0,1,1,3,3,2,2,1,1,3,3,0,2,1,3,2 },
	  { 2,3,2,0,0,1,3,0,2,0,1,2,3,0,1,0 },
	  { 1,3,1,2,3,2,3,2,0,2,0,1,1,0,3,0 },
	  { 0,2,0,3,1,0,0,1,1,3,3,2,3,2,2,1 },
	  { 2,1,3,2,3,1,2,1,0,3,0,2,0,2,0,2 },
	  { 0,3,1,0,0,2,0,3,2,1,3,1,1,3,1,3 } };

	if (filters == 1) return filter[(row + top_margin) & 15][(col + left_margin) & 15];
	if (filters == 9) return xtrans[(row + 6) % 6][(col + 6) % 6];
	return FC(row, col);
}

char *my_memmem(char *haystack, size_t haystacklen,
	char *needle, size_t needlelen)
{
	char *c;
	for (c = haystack; c <= haystack + haystacklen - needlelen; c++)
		if (!memcmp(c, needle, needlelen))
			return c;
	return 0;
}
#define memmem my_memmem
char *my_strcasestr(char *haystack, const char *needle)
{
	char *c;
	for (c = haystack; *c; c++)
		if (!strncasecmp(c, needle, strlen(needle)))
			return c;
	return 0;
}
#define strcasestr my_strcasestr

void CLASS merror(void *ptr, const char *where)
{
	if (ptr) return;
	fprintf(stderr, _("%s: Out of memory in %s\n"), ifname, where);
	longjmp(failure, 1);
}

void CLASS derror()
{
	if (!data_error) {
		fprintf(stderr, "%s: ", ifname);
		if (feof(ifp))
			fprintf(stderr, _("Unexpected end of file\n"));
		else
			fprintf(stderr, _("Corrupt data near 0x%llx\n"), (INT64)ftello(ifp));
	}
	data_error++;
}

ushort CLASS sget2(uchar *s)
{
	if (order == 0x4949)		/* "II" means little-endian */
		return s[0] | s[1] << 8;
	else				/* "MM" means big-endian */
		return s[0] << 8 | s[1];
}

ushort CLASS get2()
{
	uchar str[2] = { 0xff,0xff };
	fread(str, 1, 2, ifp);
	return sget2(str);
}

unsigned CLASS sget4(uchar *s)
{
	if (order == 0x4949)
		return s[0] | s[1] << 8 | s[2] << 16 | s[3] << 24;
	else
		return s[0] << 24 | s[1] << 16 | s[2] << 8 | s[3];
}
#define sget4(s) sget4((uchar *)s)

unsigned CLASS get4()
{
	uchar str[4] = { 0xff,0xff,0xff,0xff };
	fread(str, 1, 4, ifp);
	return sget4(str);
}

unsigned CLASS getint(int type)
{
	return type == 3 ? get2() : get4();
}

float CLASS int_to_float(int i)
{
	union { int i; float f; } u;
	u.i = i;
	return u.f;
}

double CLASS getreal(int type)
{
	union { char c[8]; double d; } u;
	int i, rev;

	switch (type) {
	case 3: return (unsigned short)get2();
	case 4: return (unsigned int)get4();
	case 5:  u.d = (unsigned int)get4();
		return u.d / (unsigned int)get4();
	case 8: return (signed short)get2();
	case 9: return (signed int)get4();
	case 10: u.d = (signed int)get4();
		return u.d / (signed int)get4();
	case 11: return int_to_float(get4());
	case 12:
		rev = 7 * ((order == 0x4949) == (ntohs(0x1234) == 0x1234));
		for (i = 0; i < 8; i++)
			u.c[i ^ rev] = fgetc(ifp);
		return u.d;
	default: return fgetc(ifp);
	}
}

void CLASS read_shorts(ushort *pixel, int count)
{
	if (fread(pixel, 2, count, ifp) < count) derror();
	if ((order == 0x4949) == (ntohs(0x1234) == 0x1234))
		swab((char *)pixel, (char *)pixel, count * 2);
}

void CLASS cubic_spline(const int *x_, const int *y_, const int len)
{
	float **A, *b, *c, *d, *x, *y;
	int i, j;

	A = (float **)calloc(((2 * len + 4) * sizeof **A + sizeof *A), 2 * len);
	if (!A) return;
	A[0] = (float *)(A + 2 * len);
	for (i = 1; i < 2 * len; i++)
		A[i] = A[0] + 2 * len*i;
	y = len + (x = i + (d = i + (c = i + (b = A[0] + i*i))));
	for (i = 0; i < len; i++) {
		x[i] = x_[i] / 65535.0;
		y[i] = y_[i] / 65535.0;
	}
	for (i = len - 1; i > 0; i--) {
		b[i] = (y[i] - y[i - 1]) / (x[i] - x[i - 1]);
		d[i - 1] = x[i] - x[i - 1];
	}
	for (i = 1; i < len - 1; i++) {
		A[i][i] = 2 * (d[i - 1] + d[i]);
		if (i > 1) {
			A[i][i - 1] = d[i - 1];
			A[i - 1][i] = d[i - 1];
		}
		A[i][len - 1] = 6 * (b[i + 1] - b[i]);
	}
	for (i = 1; i < len - 2; i++) {
		float v = A[i + 1][i] / A[i][i];
		for (j = 1; j <= len - 1; j++)
			A[i + 1][j] -= v * A[i][j];
	}
	for (i = len - 2; i > 0; i--) {
		float acc = 0;
		for (j = i; j <= len - 2; j++)
			acc += A[i][j] * c[j];
		c[i] = (A[i][len - 1] - acc) / A[i][i];
	}
	for (i = 0; i < 0x10000; i++) {
		float x_out = (float)(i / 65535.0);
		float y_out = 0;
		for (j = 0; j < len - 1; j++) {
			if (x[j] <= x_out && x_out <= x[j + 1]) {
				float v = x_out - x[j];
				y_out = y[j] +
					((y[j + 1] - y[j]) / d[j] - (2 * d[j] * c[j] + c[j + 1] * d[j]) / 6) * v
					+ (c[j] * 0.5) * v*v + ((c[j + 1] - c[j]) / (6 * d[j])) * v*v*v;
			}
		}
		curve[i] = y_out < 0.0 ? 0 : (y_out >= 1.0 ? 65535 :
			(ushort)(y_out * 65535.0 + 0.5));
	}
	free(A);
}

unsigned CLASS getbithuff(int nbits, ushort *huff)
{
	static unsigned bitbuf = 0;
	static int vbits = 0, reset = 0;
	unsigned c;

	if (nbits > 25) return 0;
	if (nbits < 0)
		return bitbuf = vbits = reset = 0;
	if (nbits == 0 || vbits < 0) return 0;
	while (!reset && vbits < nbits && (c = fgetc(ifp)) != EOF &&
		!(reset = zero_after_ff && c == 0xff && fgetc(ifp))) {
		bitbuf = (bitbuf << 8) + (uchar)c;
		vbits += 8;
	}
	c = bitbuf << (32 - vbits) >> (32 - nbits);
	if (huff) {
		vbits -= huff[c] >> 8;
		c = (uchar)huff[c];
	} else
		vbits -= nbits;
	if (vbits < 0) derror();
	return c;
}

#define getbits(n) getbithuff(n,0)
#define gethuff(h) getbithuff(*h,h+1)

/*
   Construct a decode tree according the specification in *source.
   The first 16 bytes specify how many codes should be 1-bit, 2-bit
   3-bit, etc.  Bytes after that are the leaf values.

   For example, if the source is

	{ 0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0,
	  0x04,0x03,0x05,0x06,0x02,0x07,0x01,0x08,0x09,0x00,0x0a,0x0b,0xff  },

   then the code is

	00		0x04
	010		0x03
	011		0x05
	100		0x06
	101		0x02
	1100		0x07
	1101		0x01
	11100		0x08
	11101		0x09
	11110		0x00
	111110		0x0a
	1111110		0x0b
	1111111		0xff
 */
ushort * CLASS make_decoder_ref(const uchar **source)
{
	int max, len, h, i, j;
	const uchar *count;
	ushort *huff;

	count = (*source += 16) - 17;
	for (max = 16; max && !count[max]; max--);
	huff = (ushort *)calloc(1 + (1 << max), sizeof *huff);
	merror(huff, "make_decoder()");
	huff[0] = max;
	for (h = len = 1; len <= max; len++)
		for (i = 0; i < count[len]; i++, ++*source)
			for (j = 0; j < 1 << (max - len); j++)
				if (h <= 1 << max)
					huff[h++] = len << 8 | **source;
	return huff;
}

ushort * CLASS make_decoder(const uchar *source)
{
	return make_decoder_ref(&source);
}

void CLASS crw_init_tables(unsigned table, ushort *huff[2])
{
	static const uchar first_tree[3][29] = {
	  { 0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0,
		0x04,0x03,0x05,0x06,0x02,0x07,0x01,0x08,0x09,0x00,0x0a,0x0b,0xff  },
	  { 0,2,2,3,1,1,1,1,2,0,0,0,0,0,0,0,
		0x03,0x02,0x04,0x01,0x05,0x00,0x06,0x07,0x09,0x08,0x0a,0x0b,0xff  },
	  { 0,0,6,3,1,1,2,0,0,0,0,0,0,0,0,0,
		0x06,0x05,0x07,0x04,0x08,0x03,0x09,0x02,0x00,0x0a,0x01,0x0b,0xff  },
	};
	static const uchar second_tree[3][180] = {
	  { 0,2,2,2,1,4,2,1,2,5,1,1,0,0,0,139,
		0x03,0x04,0x02,0x05,0x01,0x06,0x07,0x08,
		0x12,0x13,0x11,0x14,0x09,0x15,0x22,0x00,0x21,0x16,0x0a,0xf0,
		0x23,0x17,0x24,0x31,0x32,0x18,0x19,0x33,0x25,0x41,0x34,0x42,
		0x35,0x51,0x36,0x37,0x38,0x29,0x79,0x26,0x1a,0x39,0x56,0x57,
		0x28,0x27,0x52,0x55,0x58,0x43,0x76,0x59,0x77,0x54,0x61,0xf9,
		0x71,0x78,0x75,0x96,0x97,0x49,0xb7,0x53,0xd7,0x74,0xb6,0x98,
		0x47,0x48,0x95,0x69,0x99,0x91,0xfa,0xb8,0x68,0xb5,0xb9,0xd6,
		0xf7,0xd8,0x67,0x46,0x45,0x94,0x89,0xf8,0x81,0xd5,0xf6,0xb4,
		0x88,0xb1,0x2a,0x44,0x72,0xd9,0x87,0x66,0xd4,0xf5,0x3a,0xa7,
		0x73,0xa9,0xa8,0x86,0x62,0xc7,0x65,0xc8,0xc9,0xa1,0xf4,0xd1,
		0xe9,0x5a,0x92,0x85,0xa6,0xe7,0x93,0xe8,0xc1,0xc6,0x7a,0x64,
		0xe1,0x4a,0x6a,0xe6,0xb3,0xf1,0xd3,0xa5,0x8a,0xb2,0x9a,0xba,
		0x84,0xa4,0x63,0xe5,0xc5,0xf3,0xd2,0xc4,0x82,0xaa,0xda,0xe4,
		0xf2,0xca,0x83,0xa3,0xa2,0xc3,0xea,0xc2,0xe2,0xe3,0xff,0xff  },
	  { 0,2,2,1,4,1,4,1,3,3,1,0,0,0,0,140,
		0x02,0x03,0x01,0x04,0x05,0x12,0x11,0x06,
		0x13,0x07,0x08,0x14,0x22,0x09,0x21,0x00,0x23,0x15,0x31,0x32,
		0x0a,0x16,0xf0,0x24,0x33,0x41,0x42,0x19,0x17,0x25,0x18,0x51,
		0x34,0x43,0x52,0x29,0x35,0x61,0x39,0x71,0x62,0x36,0x53,0x26,
		0x38,0x1a,0x37,0x81,0x27,0x91,0x79,0x55,0x45,0x28,0x72,0x59,
		0xa1,0xb1,0x44,0x69,0x54,0x58,0xd1,0xfa,0x57,0xe1,0xf1,0xb9,
		0x49,0x47,0x63,0x6a,0xf9,0x56,0x46,0xa8,0x2a,0x4a,0x78,0x99,
		0x3a,0x75,0x74,0x86,0x65,0xc1,0x76,0xb6,0x96,0xd6,0x89,0x85,
		0xc9,0xf5,0x95,0xb4,0xc7,0xf7,0x8a,0x97,0xb8,0x73,0xb7,0xd8,
		0xd9,0x87,0xa7,0x7a,0x48,0x82,0x84,0xea,0xf4,0xa6,0xc5,0x5a,
		0x94,0xa4,0xc6,0x92,0xc3,0x68,0xb5,0xc8,0xe4,0xe5,0xe6,0xe9,
		0xa2,0xa3,0xe3,0xc2,0x66,0x67,0x93,0xaa,0xd4,0xd5,0xe7,0xf8,
		0x88,0x9a,0xd7,0x77,0xc4,0x64,0xe2,0x98,0xa5,0xca,0xda,0xe8,
		0xf3,0xf6,0xa9,0xb2,0xb3,0xf2,0xd2,0x83,0xba,0xd3,0xff,0xff  },
	  { 0,0,6,2,1,3,3,2,5,1,2,2,8,10,0,117,
		0x04,0x05,0x03,0x06,0x02,0x07,0x01,0x08,
		0x09,0x12,0x13,0x14,0x11,0x15,0x0a,0x16,0x17,0xf0,0x00,0x22,
		0x21,0x18,0x23,0x19,0x24,0x32,0x31,0x25,0x33,0x38,0x37,0x34,
		0x35,0x36,0x39,0x79,0x57,0x58,0x59,0x28,0x56,0x78,0x27,0x41,
		0x29,0x77,0x26,0x42,0x76,0x99,0x1a,0x55,0x98,0x97,0xf9,0x48,
		0x54,0x96,0x89,0x47,0xb7,0x49,0xfa,0x75,0x68,0xb6,0x67,0x69,
		0xb9,0xb8,0xd8,0x52,0xd7,0x88,0xb5,0x74,0x51,0x46,0xd9,0xf8,
		0x3a,0xd6,0x87,0x45,0x7a,0x95,0xd5,0xf6,0x86,0xb4,0xa9,0x94,
		0x53,0x2a,0xa8,0x43,0xf5,0xf7,0xd4,0x66,0xa7,0x5a,0x44,0x8a,
		0xc9,0xe8,0xc8,0xe7,0x9a,0x6a,0x73,0x4a,0x61,0xc7,0xf4,0xc6,
		0x65,0xe9,0x72,0xe6,0x71,0x91,0x93,0xa6,0xda,0x92,0x85,0x62,
		0xf3,0xc5,0xb2,0xa4,0x84,0xba,0x64,0xa5,0xb3,0xd2,0x81,0xe5,
		0xd3,0xaa,0xc4,0xca,0xf2,0xb1,0xe4,0xd1,0x83,0x63,0xea,0xc3,
		0xe2,0x82,0xf1,0xa3,0xc2,0xa1,0xc1,0xe3,0xa2,0xe1,0xff,0xff  }
	};
	if (table > 2) table = 2;
	huff[0] = make_decoder(first_tree[table]);
	huff[1] = make_decoder(second_tree[table]);
}

struct jhead {
	int algo, bits, high, wide, clrs, sraw, psv, restart, vpred[6];
	ushort quant[64], idct[64], *huff[20], *free[20], *row;
};

int CLASS ljpeg_start(struct jhead *jh, int info_only)
{
	ushort c, tag, len;
	uchar data[0x10000];
	const uchar *dp;

	memset(jh, 0, sizeof *jh);
	jh->restart = INT_MAX;
	if ((fgetc(ifp), fgetc(ifp)) != 0xd8) return 0;
	do {
		if (!fread(data, 2, 2, ifp)) return 0;
		tag = data[0] << 8 | data[1];
		len = (data[2] << 8 | data[3]) - 2;
		if (tag <= 0xff00) return 0;
		fread(data, 1, len, ifp);
		switch (tag) {
		case 0xffc3:
			jh->sraw = ((data[7] >> 4) * (data[7] & 15) - 1) & 3;
		case 0xffc1:
		case 0xffc0:
			jh->algo = tag & 0xff;
			jh->bits = data[0];
			jh->high = data[1] << 8 | data[2];
			jh->wide = data[3] << 8 | data[4];
			jh->clrs = data[5] + jh->sraw;
			if (len == 9 && !dng_version) getc(ifp);
			break;
		case 0xffc4:
			if (info_only) break;
			for (dp = data; dp < data + len && !((c = *dp++) & -20); )
				jh->free[c] = jh->huff[c] = make_decoder_ref(&dp);
			break;
		case 0xffda:
			jh->psv = data[1 + data[0] * 2];
			jh->bits -= data[3 + data[0] * 2] & 15;
			break;
		case 0xffdb:
			FORC(64) jh->quant[c] = data[c * 2 + 1] << 8 | data[c * 2 + 2];
			break;
		case 0xffdd:
			jh->restart = data[0] << 8 | data[1];
		}
	} while (tag != 0xffda);
	if (jh->bits > 16 || jh->clrs > 6 ||
		!jh->bits || !jh->high || !jh->wide || !jh->clrs) return 0;
	if (info_only) return 1;
	if (!jh->huff[0]) return 0;
	FORC(19) if (!jh->huff[c + 1]) jh->huff[c + 1] = jh->huff[c];
	if (jh->sraw) {
		FORC(4)        jh->huff[2 + c] = jh->huff[1];
		FORC(jh->sraw) jh->huff[1 + c] = jh->huff[0];
	}
	jh->row = (ushort *)calloc(jh->wide*jh->clrs, 4);
	merror(jh->row, "ljpeg_start()");
	return zero_after_ff = 1;
}

void CLASS ljpeg_end(struct jhead *jh)
{
	int c;
	FORC4 if (jh->free[c]) free(jh->free[c]);
	free(jh->row);
}

int CLASS ljpeg_diff(ushort *huff)
{
	int len, diff;

	len = gethuff(huff);
	if (len == 16 && (!dng_version || dng_version >= 0x1010000))
		return -32768;
	diff = getbits(len);
	if ((diff & (1 << (len - 1))) == 0)
		diff -= (1 << len) - 1;
	return diff;
}

ushort * CLASS ljpeg_row(int jrow, struct jhead *jh)
{
	int col, c, diff, pred, spred = 0;
	ushort mark = 0, *row[3];

	if (jrow * jh->wide % jh->restart == 0) {
		FORC(6) jh->vpred[c] = 1 << (jh->bits - 1);
		if (jrow) {
			fseek(ifp, -2, SEEK_CUR);
			do mark = (mark << 8) + (c = fgetc(ifp));
			while (c != EOF && mark >> 4 != 0xffd);
		}
		getbits(-1);
	}
	FORC3 row[c] = jh->row + jh->wide*jh->clrs*((jrow + c) & 1);
	for (col = 0; col < jh->wide; col++)
		FORC(jh->clrs) {
		diff = ljpeg_diff(jh->huff[c]);
		if (jh->sraw && c <= jh->sraw && (col | c))
			pred = spred;
		else if (col) pred = row[0][-jh->clrs];
		else	    pred = (jh->vpred[c] += diff) - diff;
		if (jrow && col) switch (jh->psv) {
		case 1:	break;
		case 2: pred = row[1][0];					break;
		case 3: pred = row[1][-jh->clrs];				break;
		case 4: pred = pred + row[1][0] - row[1][-jh->clrs];		break;
		case 5: pred = pred + ((row[1][0] - row[1][-jh->clrs]) >> 1);	break;
		case 6: pred = row[1][0] + ((pred - row[1][-jh->clrs]) >> 1);	break;
		case 7: pred = (pred + row[1][0]) >> 1;				break;
		default: pred = 0;
		}
		if ((**row = pred + diff) >> jh->bits) derror();
		if (c <= jh->sraw) spred = **row;
		row[0]++; row[1]++;
	}
	return row[2];
}

void CLASS lossless_jpeg_load_raw()
{
	int jwide, jrow, jcol, val, jidx, i, j, row = 0, col = 0;
	struct jhead jh;
	ushort *rp;

	if (!ljpeg_start(&jh, 0)) return;
	jwide = jh.wide * jh.clrs;

	for (jrow = 0; jrow < jh.high; jrow++) {
		rp = ljpeg_row(jrow, &jh);
		if (load_flags & 1)
			row = jrow & 1 ? height - 1 - jrow / 2 : jrow / 2;
		for (jcol = 0; jcol < jwide; jcol++) {
			val = curve[*rp++];
			if (cr2_slice[0]) {
				jidx = jrow*jwide + jcol;
				i = jidx / (cr2_slice[1] * raw_height);
				if ((j = i >= cr2_slice[0]))
					i = cr2_slice[0];
				jidx -= i * (cr2_slice[1] * raw_height);
				row = jidx / cr2_slice[1 + j];
				col = jidx % cr2_slice[1 + j] + i*cr2_slice[1];
			}
			if (raw_width == 3984 && (col -= 2) < 0)
				col += (row--, raw_width);
			if ((unsigned)row < raw_height) RAW(row, col) = val;
			if (++col >= raw_width)
				col = (row++, 0);
		}
	}
	ljpeg_end(&jh);
}

void CLASS adobe_copy_pixel(unsigned row, unsigned col, ushort **rp)
{
	int c;

	if (tiff_samples == 2 && shot_select) (*rp)++;
	if (raw_image) {
		if (row < raw_height && col < raw_width)
			RAW(row, col) = curve[**rp];
		*rp += tiff_samples;
	} else {
		if (row < height && col < width)
			FORC(tiff_samples)
			image[row*width + col][c] = curve[(*rp)[c]];
		*rp += tiff_samples;
	}
	if (tiff_samples == 2 && shot_select) (*rp)--;
}

void CLASS ljpeg_idct(struct jhead *jh)
{
	int c, i, j, len, skip, coef;
	float work[3][8][8];
	static float cs[106] = { 0 };
	static const uchar zigzag[80] =
	{ 0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,12,19,26,33,
	  40,48,41,34,27,20,13, 6, 7,14,21,28,35,42,49,56,57,50,43,36,
	  29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,
	  47,55,62,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63 };

	if (!cs[0])
		FORC(106) cs[c] = cos((c & 31)*M_PI / 16) / 2;
	memset(work, 0, sizeof work);
	work[0][0][0] = jh->vpred[0] += ljpeg_diff(jh->huff[0]) * jh->quant[0];
	for (i = 1; i < 64; i++) {
		len = gethuff(jh->huff[16]);
		i += skip = len >> 4;
		if (!(len &= 15) && skip < 15) break;
		coef = getbits(len);
		if ((coef & (1 << (len - 1))) == 0)
			coef -= (1 << len) - 1;
		((float *)work)[zigzag[i]] = coef * jh->quant[i];
	}
	FORC(8) work[0][0][c] *= M_SQRT1_2;
	FORC(8) work[0][c][0] *= M_SQRT1_2;
	for (i = 0; i < 8; i++)
		for (j = 0; j < 8; j++)
			FORC(8) work[1][i][j] += work[0][i][c] * cs[(j * 2 + 1)*c];
	for (i = 0; i < 8; i++)
		for (j = 0; j < 8; j++)
			FORC(8) work[2][i][j] += work[1][c][j] * cs[(i * 2 + 1)*c];

	FORC(64) jh->idct[c] = CLIP(((float *)work[2])[c] + 0.5);
}

void CLASS lossless_dng_load_raw()
{
	unsigned save, trow = 0, tcol = 0, jwide, jrow, jcol, row, col, i, j;
	struct jhead jh;
	ushort *rp;

	while (trow < raw_height) {
		save = ftell(ifp);
		if (tile_length < INT_MAX)
			fseek(ifp, get4(), SEEK_SET);
		if (!ljpeg_start(&jh, 0)) break;
		jwide = jh.wide;
		if (filters) jwide *= jh.clrs;
		jwide /= MIN(is_raw, tiff_samples);
		switch (jh.algo) {
		case 0xc1:
			jh.vpred[0] = 16384;
			getbits(-1);
			for (jrow = 0; jrow + 7 < jh.high; jrow += 8) {
				for (jcol = 0; jcol + 7 < jh.wide; jcol += 8) {
					ljpeg_idct(&jh);
					rp = jh.idct;
					row = trow + jcol / tile_width + jrow * 2;
					col = tcol + jcol%tile_width;
					for (i = 0; i < 16; i += 2)
						for (j = 0; j < 8; j++)
							adobe_copy_pixel(row + i, col + j, &rp);
				}
			}
			break;
		case 0xc3:
			for (row = col = jrow = 0; jrow < jh.high; jrow++) {
				rp = ljpeg_row(jrow, &jh);
				for (jcol = 0; jcol < jwide; jcol++) {
					adobe_copy_pixel(trow + row, tcol + col, &rp);
					if (++col >= tile_width || col >= raw_width)
						row += 1 + (col = 0);
				}
			}
		}
		fseek(ifp, save + 4, SEEK_SET);
		if ((tcol += tile_width) >= raw_width)
			trow += tile_length + (tcol = 0);
		ljpeg_end(&jh);
	}
}

void CLASS packed_dng_load_raw()
{
	ushort *pixel, *rp;
	int row, col;

	pixel = (ushort *)calloc(raw_width, tiff_samples * sizeof *pixel);
	merror(pixel, "packed_dng_load_raw()");
	for (row = 0; row < raw_height; row++) {
		if (tiff_bps == 16)
			read_shorts(pixel, raw_width * tiff_samples);
		else {
			getbits(-1);
			for (col = 0; col < raw_width * tiff_samples; col++)
				pixel[col] = getbits(tiff_bps);
		}
		for (rp = pixel, col = 0; col < raw_width; col++)
			adobe_copy_pixel(row, col, &rp);
	}
	free(pixel);
}

void CLASS pentax_load_raw()
{
	ushort bit[2][15], huff[4097];
	int dep, row, col, diff, c, i;
	ushort vpred[2][2] = { {0,0},{0,0} }, hpred[2];

	fseek(ifp, meta_offset, SEEK_SET);
	dep = (get2() + 12) & 15;
	fseek(ifp, 12, SEEK_CUR);
	FORC(dep) bit[0][c] = get2();
	FORC(dep) bit[1][c] = fgetc(ifp);
	FORC(dep)
		for (i = bit[0][c]; i <= ((bit[0][c] + (4096 >> bit[1][c]) - 1) & 4095); )
			huff[++i] = bit[1][c] << 8 | c;
	huff[0] = 12;
	fseek(ifp, data_offset, SEEK_SET);
	getbits(-1);
	for (row = 0; row < raw_height; row++)
		for (col = 0; col < raw_width; col++) {
			diff = ljpeg_diff(huff);
			if (col < 2) hpred[col] = vpred[row & 1][col] += diff;
			else	   hpred[col & 1] += diff;
			RAW(row, col) = hpred[col & 1];
			if (hpred[col & 1] >> tiff_bps) derror();
		}
}

void CLASS ppm_thumb()
{
	char *thumb;
	thumb_length = thumb_width*thumb_height * 3;
	thumb = (char *)malloc(thumb_length);
	merror(thumb, "ppm_thumb()");
	fprintf(ofp, "P6\n%d %d\n255\n", thumb_width, thumb_height);
	fread(thumb, 1, thumb_length, ifp);
	fwrite(thumb, 1, thumb_length, ofp);
	free(thumb);
}

void CLASS ppm16_thumb()
{
	int i;
	char *thumb;
	thumb_length = thumb_width*thumb_height * 3;
	thumb = (char *)calloc(thumb_length, 2);
	merror(thumb, "ppm16_thumb()");
	read_shorts((ushort *)thumb, thumb_length);
	for (i = 0; i < thumb_length; i++)
		thumb[i] = ((ushort *)thumb)[i] >> 8;
	fprintf(ofp, "P6\n%d %d\n255\n", thumb_width, thumb_height);
	fwrite(thumb, 1, thumb_length, ofp);
	free(thumb);
}

void CLASS layer_thumb()
{
	int i, c;
	char *thumb, map[][4] = { "012","102" };

	colors = thumb_misc >> 5 & 7;
	thumb_length = thumb_width*thumb_height;
	thumb = (char *)calloc(colors, thumb_length);
	merror(thumb, "layer_thumb()");
	fprintf(ofp, "P%d\n%d %d\n255\n",
		5 + (colors >> 1), thumb_width, thumb_height);
	fread(thumb, thumb_length, colors, ifp);
	for (i = 0; i < thumb_length; i++)
		FORCC putc(thumb[i + thumb_length*(map[thumb_misc >> 8][c] - '0')], ofp);
	free(thumb);
}

void CLASS rollei_thumb()
{
	unsigned i;
	ushort *thumb;

	thumb_length = thumb_width * thumb_height;
	thumb = (ushort *)calloc(thumb_length, 2);
	merror(thumb, "rollei_thumb()");
	fprintf(ofp, "P6\n%d %d\n255\n", thumb_width, thumb_height);
	read_shorts(thumb, thumb_length);
	for (i = 0; i < thumb_length; i++) {
		putc(thumb[i] << 3, ofp);
		putc(thumb[i] >> 5 << 2, ofp);
		putc(thumb[i] >> 11 << 3, ofp);
	}
	free(thumb);
}

void CLASS rollei_load_raw()
{
	uchar pixel[10];
	unsigned iten = 0, isix, i, buffer = 0, todo[16];

	isix = raw_width * raw_height * 5 / 8;
	while (fread(pixel, 1, 10, ifp) == 10) {
		for (i = 0; i < 10; i += 2) {
			todo[i] = iten++;
			todo[i + 1] = pixel[i] << 8 | pixel[i + 1];
			buffer = pixel[i] >> 2 | buffer << 6;
		}
		for (; i < 16; i += 2) {
			todo[i] = isix++;
			todo[i + 1] = buffer >> (14 - i) * 5;
		}
		for (i = 0; i < 16; i += 2)
			raw_image[todo[i]] = (todo[i + 1] & 0x3ff);
	}
	maximum = 0x3ff;
}

int CLASS raw(unsigned row, unsigned col)
{
	return (row < raw_height && col < raw_width) ? RAW(row, col) : 0;
}

void CLASS phase_one_flat_field(int is_float, int nc)
{
	ushort head[8];
	unsigned wide, high, y, x, c, rend, cend, row, col;
	float *mrow, num, mult[4];

	read_shorts(head, 8);
	if (head[2] * head[3] * head[4] * head[5] == 0) return;
	wide = head[2] / head[4] + (head[2] % head[4] != 0);
	high = head[3] / head[5] + (head[3] % head[5] != 0);
	mrow = (float *)calloc(nc*wide, sizeof *mrow);
	merror(mrow, "phase_one_flat_field()");
	for (y = 0; y < high; y++) {
		for (x = 0; x < wide; x++)
			for (c = 0; c < nc; c += 2) {
				num = is_float ? getreal(11) : get2() / 32768.0;
				if (y == 0) mrow[c*wide + x] = num;
				else mrow[(c + 1)*wide + x] = (num - mrow[c*wide + x]) / head[5];
			}
		if (y == 0) continue;
		rend = head[1] + y*head[5];
		for (row = rend - head[5];
			row < raw_height && row < rend &&
			row < head[1] + head[3] - head[5]; row++) {
			for (x = 1; x < wide; x++) {
				for (c = 0; c < nc; c += 2) {
					mult[c] = mrow[c*wide + x - 1];
					mult[c + 1] = (mrow[c*wide + x] - mult[c]) / head[4];
				}
				cend = head[0] + x*head[4];
				for (col = cend - head[4];
					col < raw_width &&
					col < cend && col < head[0] + head[2] - head[4]; col++) {
					c = nc > 2 ? FC(row - top_margin, col - left_margin) : 0;
					if (!(c & 1)) {
						c = RAW(row, col) * mult[c];
						RAW(row, col) = LIM(c, 0, 65535);
					}
					for (c = 0; c < nc; c += 2)
						mult[c] += mult[c + 1];
				}
			}
			for (x = 0; x < wide; x++)
				for (c = 0; c < nc; c += 2)
					mrow[c*wide + x] += mrow[(c + 1)*wide + x];
		}
	}
	free(mrow);
}

void CLASS phase_one_correct()
{
	unsigned entries, tag, data, save, col, row, type;
	int len, i, j, k, cip, val[4], dev[4], sum, max;
	int head[9], diff, mindiff = INT_MAX, off_412 = 0;
	static const signed char dir[12][2] =
	{ {-1,-1}, {-1,1}, {1,-1}, {1,1}, {-2,0}, {0,-2}, {0,2}, {2,0},
	  {-2,-2}, {-2,2}, {2,-2}, {2,2} };
	float poly[8], num, cfrac, frac, mult[2], *yval[2];
	ushort *xval[2];
	int qmult_applied = 0, qlin_applied = 0;

	if (half_size || !meta_length) return;
	if (verbose) fprintf(stderr, _("Phase One correction...\n"));
	fseek(ifp, meta_offset, SEEK_SET);
	order = get2();
	fseek(ifp, 6, SEEK_CUR);
	fseek(ifp, meta_offset + get4(), SEEK_SET);
	entries = get4();  get4();
	while (entries--) {
		tag = get4();
		len = get4();
		data = get4();
		save = ftell(ifp);
		fseek(ifp, meta_offset + data, SEEK_SET);
		if (tag == 0x419) {				/* Polynomial curve */
			for (get4(), i = 0; i < 8; i++)
				poly[i] = getreal(11);
			poly[3] += (ph1.tag_210 - poly[7]) * poly[6] + 1;
			for (i = 0; i < 0x10000; i++) {
				num = (poly[5] * i + poly[3])*i + poly[1];
				curve[i] = LIM(num, 0, 65535);
			} goto apply;				/* apply to right half */
		} else if (tag == 0x41a) {			/* Polynomial curve */
			for (i = 0; i < 4; i++)
				poly[i] = getreal(11);
			for (i = 0; i < 0x10000; i++) {
				for (num = 0, j = 4; j--; )
					num = num * i + poly[j];
				curve[i] = LIM(num + i, 0, 65535);
			} apply:					/* apply to whole image */
			for (row = 0; row < raw_height; row++)
				for (col = (tag & 1)*ph1.split_col; col < raw_width; col++)
					RAW(row, col) = curve[RAW(row, col)];
		} else if (tag == 0x400) {			/* Sensor defects */
			while ((len -= 8) >= 0) {
				col = get2();
				row = get2();
				type = get2(); get2();
				if (col >= raw_width) continue;
				if (type == 131 || type == 137)		/* Bad column */
					for (row = 0; row < raw_height; row++)
						if (FC(row - top_margin, col - left_margin) == 1) {
							for (sum = i = 0; i < 4; i++)
								sum += val[i] = raw(row + dir[i][0], col + dir[i][1]);
							for (max = i = 0; i < 4; i++) {
								dev[i] = abs((val[i] << 2) - sum);
								if (dev[max] < dev[i]) max = i;
							}
							RAW(row, col) = (sum - val[max]) / 3.0 + 0.5;
						} else {
							for (sum = 0, i = 8; i < 12; i++)
								sum += raw(row + dir[i][0], col + dir[i][1]);
							RAW(row, col) = 0.5 + sum * 0.0732233 +
								(raw(row, col - 2) + raw(row, col + 2)) * 0.3535534;
						} else if (type == 129) {			/* Bad pixel */
							if (row >= raw_height) continue;
							j = (FC(row - top_margin, col - left_margin) != 1) * 4;
							for (sum = 0, i = j; i < j + 8; i++)
								sum += raw(row + dir[i][0], col + dir[i][1]);
							RAW(row, col) = (sum + 4) >> 3;
						}
			}
		} else if (tag == 0x401) {			/* All-color flat fields */
			phase_one_flat_field(1, 2);
		} else if (tag == 0x416 || tag == 0x410) {
			phase_one_flat_field(0, 2);
		} else if (tag == 0x40b) {			/* Red+blue flat field */
			phase_one_flat_field(0, 4);
		} else if (tag == 0x412) {
			fseek(ifp, 36, SEEK_CUR);
			diff = abs(get2() - ph1.tag_21a);
			if (mindiff > diff) {
				mindiff = diff;
				off_412 = ftell(ifp) - 38;
			}
		} else if (tag == 0x41f && !qlin_applied) { /* Quadrant linearization */
			ushort lc[2][2][16], ref[16];
			int qr, qc;
			for (qr = 0; qr < 2; qr++)
				for (qc = 0; qc < 2; qc++)
					for (i = 0; i < 16; i++)
						lc[qr][qc][i] = get4();
			for (i = 0; i < 16; i++) {
				int v = 0;
				for (qr = 0; qr < 2; qr++)
					for (qc = 0; qc < 2; qc++)
						v += lc[qr][qc][i];
				ref[i] = (v + 2) >> 2;
			}
			for (qr = 0; qr < 2; qr++) {
				for (qc = 0; qc < 2; qc++) {
					int cx[19], cf[19];
					for (i = 0; i < 16; i++) {
						cx[1 + i] = lc[qr][qc][i];
						cf[1 + i] = ref[i];
					}
					cx[0] = cf[0] = 0;
					cx[17] = cf[17] = ((unsigned)ref[15] * 65535) / lc[qr][qc][15];
					cx[18] = cf[18] = 65535;
					cubic_spline(cx, cf, 19);
					for (row = (qr ? ph1.split_row : 0);
						row < (qr ? raw_height : ph1.split_row); row++)
						for (col = (qc ? ph1.split_col : 0);
							col < (qc ? raw_width : ph1.split_col); col++)
							RAW(row, col) = curve[RAW(row, col)];
				}
			}
			qlin_applied = 1;
		} else if (tag == 0x41e && !qmult_applied) { /* Quadrant multipliers */
			float qmult[2][2] = { { 1, 1 }, { 1, 1 } };
			get4(); get4(); get4(); get4();
			qmult[0][0] = 1.0 + getreal(11);
			get4(); get4(); get4(); get4(); get4();
			qmult[0][1] = 1.0 + getreal(11);
			get4(); get4(); get4();
			qmult[1][0] = 1.0 + getreal(11);
			get4(); get4(); get4();
			qmult[1][1] = 1.0 + getreal(11);
			for (row = 0; row < raw_height; row++)
				for (col = 0; col < raw_width; col++) {
					i = qmult[row >= ph1.split_row][col >= ph1.split_col] * RAW(row, col);
					RAW(row, col) = LIM(i, 0, 65535);
				}
			qmult_applied = 1;
		} else if (tag == 0x431 && !qmult_applied) { /* Quadrant combined */
			ushort lc[2][2][7], ref[7];
			int qr, qc;
			for (i = 0; i < 7; i++)
				ref[i] = get4();
			for (qr = 0; qr < 2; qr++)
				for (qc = 0; qc < 2; qc++)
					for (i = 0; i < 7; i++)
						lc[qr][qc][i] = get4();
			for (qr = 0; qr < 2; qr++) {
				for (qc = 0; qc < 2; qc++) {
					int cx[9], cf[9];
					for (i = 0; i < 7; i++) {
						cx[1 + i] = ref[i];
						cf[1 + i] = ((unsigned)ref[i] * lc[qr][qc][i]) / 10000;
					}
					cx[0] = cf[0] = 0;
					cx[8] = cf[8] = 65535;
					cubic_spline(cx, cf, 9);
					for (row = (qr ? ph1.split_row : 0);
						row < (qr ? raw_height : ph1.split_row); row++)
						for (col = (qc ? ph1.split_col : 0);
							col < (qc ? raw_width : ph1.split_col); col++)
							RAW(row, col) = curve[RAW(row, col)];
				}
			}
			qmult_applied = 1;
			qlin_applied = 1;
		}
		fseek(ifp, save, SEEK_SET);
	}
	if (off_412) {
		fseek(ifp, off_412, SEEK_SET);
		for (i = 0; i < 9; i++) head[i] = get4() & 0x7fff;
		yval[0] = (float *)calloc(head[1] * head[3] + head[2] * head[4], 6);
		merror(yval[0], "phase_one_correct()");
		yval[1] = (float  *)(yval[0] + head[1] * head[3]);
		xval[0] = (ushort *)(yval[1] + head[2] * head[4]);
		xval[1] = (ushort *)(xval[0] + head[1] * head[3]);
		get2();
		for (i = 0; i < 2; i++)
			for (j = 0; j < head[i + 1] * head[i + 3]; j++)
				yval[i][j] = getreal(11);
		for (i = 0; i < 2; i++)
			for (j = 0; j < head[i + 1] * head[i + 3]; j++)
				xval[i][j] = get2();
		for (row = 0; row < raw_height; row++)
			for (col = 0; col < raw_width; col++) {
				cfrac = (float)col * head[3] / raw_width;
				cfrac -= cip = cfrac;
				num = RAW(row, col) * 0.5;
				for (i = cip; i < cip + 2; i++) {
					for (k = j = 0; j < head[1]; j++)
						if (num < xval[0][k = head[1] * i + j]) break;
					frac = (j == 0 || j == head[1]) ? 0 :
						(xval[0][k] - num) / (xval[0][k] - xval[0][k - 1]);
					mult[i - cip] = yval[0][k - 1] * frac + yval[0][k] * (1 - frac);
				}
				i = ((mult[0] * (1 - cfrac) + mult[1] * cfrac) * row + num) * 2;
				RAW(row, col) = LIM(i, 0, 65535);
			}
		free(yval[0]);
	}
}

void CLASS phase_one_load_raw()
{
	int a, b, i;
	ushort akey, bkey, mask;

	fseek(ifp, ph1.key_off, SEEK_SET);
	akey = get2();
	bkey = get2();
	mask = ph1.format == 1 ? 0x5555 : 0x1354;
	fseek(ifp, data_offset, SEEK_SET);
	read_shorts(raw_image, raw_width*raw_height);
	if (ph1.format)
		for (i = 0; i < raw_width*raw_height; i += 2) {
			a = raw_image[i + 0] ^ akey;
			b = raw_image[i + 1] ^ bkey;
			raw_image[i + 0] = (a & mask) | (b & ~mask);
			raw_image[i + 1] = (b & mask) | (a & ~mask);
		}
}

unsigned CLASS ph1_bithuff(int nbits, ushort *huff)
{
	static UINT64 bitbuf = 0;
	static int vbits = 0;
	unsigned c;

	if (nbits == -1)
		return bitbuf = vbits = 0;
	if (nbits == 0) return 0;
	if (vbits < nbits) {
		bitbuf = bitbuf << 32 | get4();
		vbits += 32;
	}
	c = bitbuf << (64 - vbits) >> (64 - nbits);
	if (huff) {
		vbits -= huff[c] >> 8;
		return (uchar)huff[c];
	}
	vbits -= nbits;
	return c;
}
#define ph1_bits(n) ph1_bithuff(n,0)
#define ph1_huff(h) ph1_bithuff(*h,h+1)

void CLASS phase_one_load_raw_c()
{
	static const int length[] = { 8,7,6,9,11,10,5,12,14,13 };
	int *offset, len[2], pred[2], row, col, i, j;
	ushort *pixel;
	short(*cblack)[2], (*rblack)[2];

	pixel = (ushort *)calloc(raw_width * 3 + raw_height * 4, 2);
	merror(pixel, "phase_one_load_raw_c()");
	offset = (int *)(pixel + raw_width);
	fseek(ifp, strip_offset, SEEK_SET);
	for (row = 0; row < raw_height; row++)
		offset[row] = get4();
	cblack = (short(*)[2]) (offset + raw_height);
	fseek(ifp, ph1.black_col, SEEK_SET);
	if (ph1.black_col)
		read_shorts((ushort *)cblack[0], raw_height * 2);
	rblack = cblack + raw_height;
	fseek(ifp, ph1.black_row, SEEK_SET);
	if (ph1.black_row)
		read_shorts((ushort *)rblack[0], raw_width * 2);
	for (i = 0; i < 256; i++)
		curve[i] = i*i / 3.969 + 0.5;
	for (row = 0; row < raw_height; row++) {
		fseek(ifp, data_offset + offset[row], SEEK_SET);
		ph1_bits(-1);
		pred[0] = pred[1] = 0;
		for (col = 0; col < raw_width; col++) {
			if (col >= (raw_width & -8))
				len[0] = len[1] = 14;
			else if ((col & 7) == 0)
				for (i = 0; i < 2; i++) {
					for (j = 0; j < 5 && !ph1_bits(1); j++);
					if (j--) len[i] = length[j * 2 + ph1_bits(1)];
				}
			if ((i = len[col & 1]) == 14)
				pixel[col] = pred[col & 1] = ph1_bits(16);
			else
				pixel[col] = pred[col & 1] += ph1_bits(i) + 1 - (1 << (i - 1));
			if (pred[col & 1] >> 16) derror();
			if (ph1.format == 5 && pixel[col] < 256)
				pixel[col] = curve[pixel[col]];
		}
		for (col = 0; col < raw_width; col++) {
			i = (pixel[col] << 2 * (ph1.format != 8)) - ph1.black
				+ cblack[row][col >= ph1.split_col]
				+ rblack[col][row >= ph1.split_row];
			if (i > 0) RAW(row, col) = i;
		}
	}
	free(pixel);
	maximum = 0xfffc - ph1.black;
}

void CLASS hasselblad_load_raw()
{
	struct jhead jh;
	int shot, row, col, *back[5], len[2], diff[12], pred, sh, f, s, c;
	unsigned upix, urow, ucol;
	ushort *ip;

	if (!ljpeg_start(&jh, 0)) return;
	order = 0x4949;
	ph1_bits(-1);
	back[4] = (int *)calloc(raw_width, 3 * sizeof **back);
	merror(back[4], "hasselblad_load_raw()");
	FORC3 back[c] = back[4] + c*raw_width;
	cblack[6] >>= sh = tiff_samples > 1;
	shot = LIM(shot_select, 1, tiff_samples) - 1;
	for (row = 0; row < raw_height; row++) {
		FORC4 back[(c + 3) & 3] = back[c];
		for (col = 0; col < raw_width; col += 2) {
			for (s = 0; s < tiff_samples * 2; s += 2) {
				FORC(2) len[c] = ph1_huff(jh.huff[0]);
				FORC(2) {
					diff[s + c] = ph1_bits(len[c]);
					if ((diff[s + c] & (1 << (len[c] - 1))) == 0)
						diff[s + c] -= (1 << len[c]) - 1;
					if (diff[s + c] == 65535) diff[s + c] = -32768;
				}
			}
			for (s = col; s < col + 2; s++) {
				pred = 0x8000 + load_flags;
				if (col) pred = back[2][s - 2];
				if (col && row > 1) switch (jh.psv) {
				case 11: pred += back[0][s] / 2 - back[0][s - 2] / 2;  break;
				}
				f = (row & 1) * 3 ^ ((col + s) & 1);
				FORC(tiff_samples) {
					pred += diff[(s & 1)*tiff_samples + c];
					upix = pred >> sh & 0xffff;
					if (raw_image && c == shot)
						RAW(row, s) = upix;
					if (image) {
						urow = row - top_margin + (c & 1);
						ucol = col - left_margin - ((c >> 1) & 1);
						ip = &image[urow*width + ucol][f];
						if (urow < height && ucol < width)
							*ip = c < 4 ? upix : (*ip + upix) >> 1;
					}
				}
				back[2][s] = pred;
			}
		}
	}
	free(back[4]);
	ljpeg_end(&jh);
	if (image) mix_green = 1;
}

void CLASS leaf_hdr_load_raw()
{
	ushort *pixel = 0;
	unsigned tile = 0, r, c, row, col;

	if (!filters) {
		pixel = (ushort *)calloc(raw_width, sizeof *pixel);
		merror(pixel, "leaf_hdr_load_raw()");
	}
	FORC(tiff_samples)
		for (r = 0; r < raw_height; r++) {
			if (r % tile_length == 0) {
				fseek(ifp, data_offset + 4 * tile++, SEEK_SET);
				fseek(ifp, get4(), SEEK_SET);
			}
			if (filters && c != shot_select) continue;
			if (filters) pixel = raw_image + r*raw_width;
			read_shorts(pixel, raw_width);
			if (!filters && (row = r - top_margin) < height)
				for (col = 0; col < width; col++)
					image[row*width + col][c] = pixel[col + left_margin];
		}
	if (!filters) {
		maximum = 0xffff;
		raw_color = 1;
		free(pixel);
	}
}

void CLASS unpacked_load_raw()
{
	int row, col, bits = 0;

	while (1 << ++bits < maximum);
	read_shorts(raw_image, raw_width*raw_height);
	for (row = 0; row < raw_height; row++)
		for (col = 0; col < raw_width; col++)
			if ((RAW(row, col) >>= load_flags) >> bits
				&& (unsigned)(row - top_margin) < height
				&& (unsigned)(col - left_margin) < width) derror();
}

void CLASS sinar_4shot_load_raw()
{
	ushort *pixel;
	unsigned shot, row, col, r, c;

	if (raw_image) {
		shot = LIM(shot_select, 1, 4) - 1;
		fseek(ifp, data_offset + shot * 4, SEEK_SET);
		fseek(ifp, get4(), SEEK_SET);
		unpacked_load_raw();
		return;
	}
	pixel = (ushort *)calloc(raw_width, sizeof *pixel);
	merror(pixel, "sinar_4shot_load_raw()");
	for (shot = 0; shot < 4; shot++) {
		fseek(ifp, data_offset + shot * 4, SEEK_SET);
		fseek(ifp, get4(), SEEK_SET);
		for (row = 0; row < raw_height; row++) {
			read_shorts(pixel, raw_width);
			if ((r = row - top_margin - (shot >> 1 & 1)) >= height) continue;
			for (col = 0; col < raw_width; col++) {
				if ((c = col - left_margin - (shot & 1)) >= width) continue;
				image[r*width + c][(row & 1) * 3 ^ (~col & 1)] = pixel[col];
			}
		}
	}
	free(pixel);
	mix_green = 1;
}

void CLASS imacon_full_load_raw()
{
	int row, col;

	if (!image) return;
	for (row = 0; row < height; row++)
		for (col = 0; col < width; col++)
			read_shorts(image[row*width + col], 3);
}

void CLASS packed_load_raw()
{
	int vbits = 0, bwide, rbits, bite, half, irow, row, col, val, i;
	UINT64 bitbuf = 0;

	bwide = raw_width * tiff_bps / 8;
	bwide += bwide & load_flags >> 7;
	rbits = bwide * 8 - raw_width * tiff_bps;
	if (load_flags & 1) bwide = bwide * 16 / 15;
	bite = 8 + (load_flags & 24);
	half = (raw_height + 1) >> 1;
	for (irow = 0; irow < raw_height; irow++) {
		row = irow;
		if (load_flags & 2 &&
			(row = irow % half * 2 + irow / half) == 1 &&
			load_flags & 4) {
			if (vbits = 0, tiff_compress)
				fseek(ifp, data_offset - (-half*bwide & -2048), SEEK_SET);
			else {
				fseek(ifp, 0, SEEK_END);
				fseek(ifp, ftell(ifp) >> 3 << 2, SEEK_SET);
			}
		}
		for (col = 0; col < raw_width; col++) {
			for (vbits -= tiff_bps; vbits < 0; vbits += bite) {
				bitbuf <<= bite;
				for (i = 0; i < bite; i += 8)
					bitbuf |= (unsigned)(fgetc(ifp) << i);
			}
			val = bitbuf << (64 - tiff_bps - vbits) >> (64 - tiff_bps);
			RAW(row, col ^ (load_flags >> 6 & 1)) = val;
			if (load_flags & 1 && (col % 10) == 9 && fgetc(ifp) &&
				row < height + top_margin && col < width + left_margin) derror();
		}
		vbits -= rbits;
	}
}

void CLASS nokia_load_raw()
{
	uchar  *data, *dp;
	int rev, dwide, row, col, c;
	double sum[] = { 0,0 };

	rev = 3 * (order == 0x4949);
	dwide = (raw_width * 5 + 1) / 4;
	data = (uchar *)malloc(dwide * 2);
	merror(data, "nokia_load_raw()");
	for (row = 0; row < raw_height; row++) {
		if (fread(data + dwide, 1, dwide, ifp) < dwide) derror();
		FORC(dwide) data[c] = data[dwide + (c ^ rev)];
		for (dp = data, col = 0; col < raw_width; dp += 5, col += 4)
			FORC4 RAW(row, col + c) = (dp[c] << 2) | (dp[4] >> (c << 1) & 3);
	}
	free(data);
	maximum = 0x3ff;
	row = raw_height / 2;
	FORC(width - 1) {
		sum[c & 1] += SQR(RAW(row, c) - RAW(row + 1, c + 1));
		sum[~c & 1] += SQR(RAW(row + 1, c) - RAW(row, c + 1));
	}
	if (sum[1] > sum[0]) filters = 0x4b4b4b4b;
}

void CLASS olympus_load_raw()
{
	ushort huff[4096];
	int row, col, nbits, sign, low, high, i, c, w, n, nw;
	int acarry[2][3], *carry, pred, diff;

	huff[n = 0] = 0xc0c;
	for (i = 12; i--; )
		FORC(2048 >> i) huff[++n] = (i + 1) << 8 | i;
	fseek(ifp, 7, SEEK_CUR);
	getbits(-1);
	for (row = 0; row < height; row++) {
		memset(acarry, 0, sizeof acarry);
		for (col = 0; col < raw_width; col++) {
			carry = acarry[col & 1];
			i = 2 * (carry[2] < 3);
			for (nbits = 2 + i; (ushort)carry[0] >> (nbits + i); nbits++);
			low = (sign = getbits(3)) & 3;
			sign = sign << 29 >> 31;
			if ((high = getbithuff(12, huff)) == 12)
				high = getbits(16 - nbits) >> 1;
			carry[0] = (high << nbits) | getbits(nbits);
			diff = (carry[0] ^ sign) + carry[1];
			carry[1] = (diff * 3 + carry[1]) >> 5;
			carry[2] = carry[0] > 16 ? 0 : carry[2] + 1;
			if (col >= width) continue;
			if (row < 2 && col < 2) pred = 0;
			else if (row < 2) pred = RAW(row, col - 2);
			else if (col < 2) pred = RAW(row - 2, col);
			else {
				w = RAW(row, col - 2);
				n = RAW(row - 2, col);
				nw = RAW(row - 2, col - 2);
				if ((w < nw && nw < n) || (n < nw && nw < w)) {
					if (ABS(w - nw) > 32 || ABS(n - nw) > 32)
						pred = w + n - nw;
					else pred = (w + n) >> 1;
				} else pred = ABS(w - nw) > ABS(n - nw) ? w : n;
			}
			if ((RAW(row, col) = pred + ((diff << 2) | low)) >> 12) derror();
		}
	}
}

void CLASS quicktake_100_load_raw()
{
	uchar pixel[484][644];
	static const short gstep[16] =
	{ -89,-60,-44,-32,-22,-15,-8,-2,2,8,15,22,32,44,60,89 };
	static const short rstep[6][4] =
	{ {  -3,-1,1,3  }, {  -5,-1,1,5  }, {  -8,-2,2,8  },
	  { -13,-3,3,13 }, { -19,-4,4,19 }, { -28,-6,6,28 } };
	static const short curve[256] =
	{ 0,1,2,3,4,5,6,7,8,9,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,
	  28,29,30,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,53,
	  54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,74,75,76,77,78,
	  79,80,81,82,83,84,86,88,90,92,94,97,99,101,103,105,107,110,112,114,116,
	  118,120,123,125,127,129,131,134,136,138,140,142,144,147,149,151,153,155,
	  158,160,162,164,166,168,171,173,175,177,179,181,184,186,188,190,192,195,
	  197,199,201,203,205,208,210,212,214,216,218,221,223,226,230,235,239,244,
	  248,252,257,261,265,270,274,278,283,287,291,296,300,305,309,313,318,322,
	  326,331,335,339,344,348,352,357,361,365,370,374,379,383,387,392,396,400,
	  405,409,413,418,422,426,431,435,440,444,448,453,457,461,466,470,474,479,
	  483,487,492,496,500,508,519,531,542,553,564,575,587,598,609,620,631,643,
	  654,665,676,687,698,710,721,732,743,754,766,777,788,799,810,822,833,844,
	  855,866,878,889,900,911,922,933,945,956,967,978,989,1001,1012,1023 };
	int rb, row, col, sharp, val = 0;

	getbits(-1);
	memset(pixel, 0x80, sizeof pixel);
	for (row = 2; row < height + 2; row++) {
		for (col = 2 + (row & 1); col < width + 2; col += 2) {
			val = ((pixel[row - 1][col - 1] + 2 * pixel[row - 1][col + 1] +
				pixel[row][col - 2]) >> 2) + gstep[getbits(4)];
			pixel[row][col] = val = LIM(val, 0, 255);
			if (col < 4)
				pixel[row][col - 2] = pixel[row + 1][~row & 1] = val;
			if (row == 2)
				pixel[row - 1][col + 1] = pixel[row - 1][col + 3] = val;
		}
		pixel[row][col] = val;
	}
	for (rb = 0; rb < 2; rb++)
		for (row = 2 + rb; row < height + 2; row += 2)
			for (col = 3 - (row & 1); col < width + 2; col += 2) {
				if (row < 4 || col < 4) sharp = 2;
				else {
					val = ABS(pixel[row - 2][col] - pixel[row][col - 2])
						+ ABS(pixel[row - 2][col] - pixel[row - 2][col - 2])
						+ ABS(pixel[row][col - 2] - pixel[row - 2][col - 2]);
					sharp = val < 4 ? 0 : val < 8 ? 1 : val < 16 ? 2 :
						val < 32 ? 3 : val < 48 ? 4 : 5;
				}
				val = ((pixel[row - 2][col] + pixel[row][col - 2]) >> 1)
					+ rstep[sharp][getbits(2)];
				pixel[row][col] = val = LIM(val, 0, 255);
				if (row < 4) pixel[row - 2][col + 2] = val;
				if (col < 4) pixel[row + 2][col - 2] = val;
			}
	for (row = 2; row < height + 2; row++)
		for (col = 3 - (row & 1); col < width + 2; col += 2) {
			val = ((pixel[row][col - 1] + (pixel[row][col] << 2) +
				pixel[row][col + 1]) >> 1) - 0x100;
			pixel[row][col] = LIM(val, 0, 255);
		}
	for (row = 0; row < height; row++)
		for (col = 0; col < width; col++)
			RAW(row, col) = curve[pixel[row + 2][col + 2]];
	maximum = 0x3ff;
}

#define radc_token(tree) ((signed char) getbithuff(8,huff[tree]))

#define FORYX for (y=1; y < 3; y++) for (x=col+1; x >= col; x--)

#define PREDICTOR (c ? (buf[c][y-1][x] + buf[c][y][x+1]) / 2 \
: (buf[c][y-1][x+1] + 2*buf[c][y-1][x] + buf[c][y][x+1]) / 4)

#undef FORYX
#undef PREDICTOR

void CLASS lossy_dng_load_raw() {}

void CLASS eight_bit_load_raw()
{
	uchar *pixel;
	unsigned row, col;

	pixel = (uchar *)calloc(raw_width, sizeof *pixel);
	merror(pixel, "eight_bit_load_raw()");
	for (row = 0; row < raw_height; row++) {
		if (fread(pixel, 1, raw_width, ifp) < raw_width) derror();
		for (col = 0; col < raw_width; col++)
			RAW(row, col) = curve[pixel[col]];
	}
	free(pixel);
	maximum = curve[0xff];
}

void CLASS samsung_load_raw()
{
	int row, col, c, i, dir, op[4], len[4];

	order = 0x4949;
	for (row = 0; row < raw_height; row++) {
		fseek(ifp, strip_offset + row * 4, SEEK_SET);
		fseek(ifp, data_offset + get4(), SEEK_SET);
		ph1_bits(-1);
		FORC4 len[c] = row < 2 ? 7 : 4;
		for (col = 0; col < raw_width; col += 16) {
			dir = ph1_bits(1);
			FORC4 op[c] = ph1_bits(2);
			FORC4 switch (op[c]) {
			case 3: len[c] = ph1_bits(4);	break;
			case 2: len[c]--;		break;
			case 1: len[c]++;
			}
			for (c = 0; c < 16; c += 2) {
				i = len[((c & 1) << 1) | (c >> 3)];
				RAW(row, col + c) = ((signed)ph1_bits(i) << (32 - i) >> (32 - i)) +
					(dir ? RAW(row + (~c | -2), col + c) : col ? RAW(row, col + (c | -2)) : 128);
				if (c == 14) c = -1;
			}
		}
	}
	for (row = 0; row < raw_height - 1; row += 2)
		for (col = 0; col < raw_width - 1; col += 2)
			SWAP(RAW(row, col + 1), RAW(row + 1, col));
}

void CLASS samsung2_load_raw()
{
	static const ushort tab[14] =
	{ 0x304,0x307,0x206,0x205,0x403,0x600,0x709,
	  0x80a,0x90b,0xa0c,0xa0d,0x501,0x408,0x402 };
	ushort huff[1026], vpred[2][2] = { {0,0},{0,0} }, hpred[2];
	int i, c, n, row, col, diff;

	huff[0] = 10;
	for (n = i = 0; i < 14; i++)
		FORC(1024 >> (tab[i] >> 8)) huff[++n] = tab[i];
	getbits(-1);
	for (row = 0; row < raw_height; row++)
		for (col = 0; col < raw_width; col++) {
			diff = ljpeg_diff(huff);
			if (col < 2) hpred[col] = vpred[row & 1][col] += diff;
			else	   hpred[col & 1] += diff;
			RAW(row, col) = hpred[col & 1];
			if (hpred[col & 1] >> tiff_bps) derror();
		}
}

void CLASS samsung3_load_raw()
{
	int opt, init, mag, pmode, row, tab, col, pred, diff, i, c;
	ushort lent[3][2], len[4], *prow[2];

	order = 0x4949;
	fseek(ifp, 9, SEEK_CUR);
	opt = fgetc(ifp);
	init = (get2(), get2());
	for (row = 0; row < raw_height; row++) {
		fseek(ifp, (data_offset - ftell(ifp)) & 15, SEEK_CUR);
		ph1_bits(-1);
		mag = 0; pmode = 7;
		FORC(6) ((ushort *)lent)[c] = row < 2 ? 7 : 4;
		prow[row & 1] = &RAW(row - 1, 1 - ((row & 1) << 1));	// green
		prow[~row & 1] = &RAW(row - 2, 0);			// red and blue
		for (tab = 0; tab + 15 < raw_width; tab += 16) {
			if (~opt & 4 && !(tab & 63)) {
				i = ph1_bits(2);
				mag = i < 3 ? mag - '2' + "204"[i] : ph1_bits(12);
			}
			if (opt & 2)
				pmode = 7 - 4 * ph1_bits(1);
			else if (!ph1_bits(1))
				pmode = ph1_bits(3);
			if (opt & 1 || !ph1_bits(1)) {
				FORC4 len[c] = ph1_bits(2);
				FORC4{
				  i = ((row & 1) << 1 | (c & 1)) % 3;
				  len[c] = len[c] < 3 ? lent[i][0] - '1' + "120"[len[c]] : ph1_bits(4);
				  lent[i][0] = lent[i][1];
				  lent[i][1] = len[c];
				}
			}
			FORC(16) {
				col = tab + (((c & 7) << 1) ^ (c >> 3) ^ (row & 1));
				pred = (pmode == 7 || row < 2)
					? (tab ? RAW(row, tab - 2 + (col & 1)) : init)
					: (prow[col & 1][col - '4' + "0224468"[pmode]] +
						prow[col & 1][col - '4' + "0244668"[pmode]] + 1) >> 1;
				diff = ph1_bits(i = len[c >> 2]);
				if (diff >> (i - 1)) diff -= 1 << i;
				diff = diff * (mag * 2 + 1) + mag;
				RAW(row, col) = pred + diff;
			}
		}
	}
}

#define HOLE(row) ((holes >> (((row) - raw_height) & 7)) & 1)

/* Kudos to Rich Taylor for figuring out SMaL's compression algorithm. */
void CLASS smal_decode_segment(unsigned seg[2][2], int holes)
{
	uchar hist[3][13] = {
	  { 7, 7, 0, 0, 63, 55, 47, 39, 31, 23, 15, 7, 0 },
	  { 7, 7, 0, 0, 63, 55, 47, 39, 31, 23, 15, 7, 0 },
	  { 3, 3, 0, 0, 63,     47,     31,     15,    0 } };
	int low, high = 0xff, carry = 0, nbits = 8;
	int pix, s, count, bin, next, i, sym[3];
	uchar diff, pred[] = { 0,0 };
	ushort data = 0, range = 0;

	fseek(ifp, seg[0][1] + 1, SEEK_SET);
	getbits(-1);
	if (seg[1][0] > raw_width*raw_height)
		seg[1][0] = raw_width*raw_height;
	for (pix = seg[0][0]; pix < seg[1][0]; pix++) {
		for (s = 0; s < 3; s++) {
			data = data << nbits | getbits(nbits);
			if (carry < 0)
				carry = (nbits += carry + 1) < 1 ? nbits - 1 : 0;
			while (--nbits >= 0)
				if ((data >> nbits & 0xff) == 0xff) break;
			if (nbits > 0)
				data = ((data & ((1 << (nbits - 1)) - 1)) << 1) |
				((data + (((data & (1 << (nbits - 1)))) << 1)) & (-1 << nbits));
			if (nbits >= 0) {
				data += getbits(1);
				carry = nbits - 8;
			}
			count = ((((data - range + 1) & 0xffff) << 2) - 1) / (high >> 4);
			for (bin = 0; hist[s][bin + 5] > count; bin++);
			low = hist[s][bin + 5] * (high >> 4) >> 2;
			if (bin) high = hist[s][bin + 4] * (high >> 4) >> 2;
			high -= low;
			for (nbits = 0; high << nbits < 128; nbits++);
			range = (range + low) << nbits;
			high <<= nbits;
			next = hist[s][1];
			if (++hist[s][2] > hist[s][3]) {
				next = (next + 1) & hist[s][0];
				hist[s][3] = (hist[s][next + 4] - hist[s][next + 5]) >> 2;
				hist[s][2] = 1;
			}
			if (hist[s][hist[s][1] + 4] - hist[s][hist[s][1] + 5] > 1) {
				if (bin < hist[s][1])
					for (i = bin; i < hist[s][1]; i++) hist[s][i + 5]--;
				else if (next <= bin)
					for (i = hist[s][1]; i < bin; i++) hist[s][i + 5]++;
			}
			hist[s][1] = next;
			sym[s] = bin;
		}
		diff = sym[2] << 5 | sym[1] << 2 | (sym[0] & 3);
		if (sym[0] & 4)
			diff = diff ? -diff : 0x80;
		if (ftell(ifp) + 12 >= seg[1][1])
			diff = 0;
		raw_image[pix] = pred[pix & 1] += diff;
		if (!(pix & 1) && HOLE(pix / raw_width)) pix += 2;
	}
	maximum = 0xff;
}

void CLASS smal_v6_load_raw()
{
	unsigned seg[2][2];

	fseek(ifp, 16, SEEK_SET);
	seg[0][0] = 0;
	seg[0][1] = get2();
	seg[1][0] = raw_width * raw_height;
	seg[1][1] = INT_MAX;
	smal_decode_segment(seg, 0);
}

int CLASS median4(int *p)
{
	int min, max, sum, i;

	min = max = sum = p[0];
	for (i = 1; i < 4; i++) {
		sum += p[i];
		if (min > p[i]) min = p[i];
		if (max < p[i]) max = p[i];
	}
	return (sum - min - max) >> 1;
}

void CLASS fill_holes(int holes)
{
	int row, col, val[4];

	for (row = 2; row < height - 2; row++) {
		if (!HOLE(row)) continue;
		for (col = 1; col < width - 1; col += 4) {
			val[0] = RAW(row - 1, col - 1);
			val[1] = RAW(row - 1, col + 1);
			val[2] = RAW(row + 1, col - 1);
			val[3] = RAW(row + 1, col + 1);
			RAW(row, col) = median4(val);
		}
		for (col = 2; col < width - 2; col += 4)
			if (HOLE(row - 2) || HOLE(row + 2))
				RAW(row, col) = (RAW(row, col - 2) + RAW(row, col + 2)) >> 1;
			else {
				val[0] = RAW(row, col - 2);
				val[1] = RAW(row, col + 2);
				val[2] = RAW(row - 2, col);
				val[3] = RAW(row + 2, col);
				RAW(row, col) = median4(val);
			}
	}
}

void CLASS smal_v9_load_raw()
{
	unsigned seg[256][2], offset, nseg, holes, i;

	fseek(ifp, 67, SEEK_SET);
	offset = get4();
	nseg = (uchar)fgetc(ifp);
	fseek(ifp, offset, SEEK_SET);
	for (i = 0; i < nseg * 2; i++)
		((unsigned *)seg)[i] = get4() + data_offset*(i & 1);
	fseek(ifp, 78, SEEK_SET);
	holes = fgetc(ifp);
	fseek(ifp, 88, SEEK_SET);
	seg[nseg][0] = raw_height * raw_width;
	seg[nseg][1] = get4() + data_offset;
	for (i = 0; i < nseg; i++)
		smal_decode_segment(seg + i, holes);
	if (holes) fill_holes(holes);
}

void CLASS redcine_load_raw()
{
}

/* RESTRICTED code starts here */

void CLASS foveon_decoder(unsigned size, unsigned code)
{
	static unsigned huff[1024];
	struct decode *cur;
	int i, len;

	if (!code) {
		for (i = 0; i < size; i++)
			huff[i] = get4();
		memset(first_decode, 0, sizeof first_decode);
		free_decode = first_decode;
	}
	cur = free_decode++;
	if (free_decode > first_decode + 2048) {
		fprintf(stderr, _("%s: decoder table overflow\n"), ifname);
		longjmp(failure, 2);
	}
	if (code)
		for (i = 0; i < size; i++)
			if (huff[i] == code) {
				cur->leaf = i;
				return;
			}
	if ((len = code >> 27) > 26) return;
	code = (len + 1) << 27 | (code & 0x3ffffff) << 1;

	cur->branch[0] = free_decode;
	foveon_decoder(size, code);
	cur->branch[1] = free_decode;
	foveon_decoder(size, code + 1);
}

void CLASS foveon_thumb()
{
	unsigned bwide, row, col, bitbuf = 0, bit = 1, c, i;
	char *buf;
	struct decode *dindex;
	short pred[3];

	bwide = get4();
	fprintf(ofp, "P6\n%d %d\n255\n", thumb_width, thumb_height);
	if (bwide > 0) {
		if (bwide < thumb_width * 3) return;
		buf = (char *)malloc(bwide);
		merror(buf, "foveon_thumb()");
		for (row = 0; row < thumb_height; row++) {
			fread(buf, 1, bwide, ifp);
			fwrite(buf, 3, thumb_width, ofp);
		}
		free(buf);
		return;
	}
	foveon_decoder(256, 0);

	for (row = 0; row < thumb_height; row++) {
		memset(pred, 0, sizeof pred);
		if (!bit) get4();
		for (bit = col = 0; col < thumb_width; col++)
			FORC3{
		  for (dindex = first_decode; dindex->branch[0]; ) {
			if ((bit = (bit - 1) & 31) == 31)
			  for (i = 0; i < 4; i++)
				bitbuf = (bitbuf << 8) + fgetc(ifp);
			dindex = dindex->branch[bitbuf >> bit & 1];
		  }
		  pred[c] += dindex->leaf;
		  fputc(pred[c], ofp);
		}
	}
}

void CLASS foveon_sd_load_raw()
{
	struct decode *dindex;
	short diff[1024];
	unsigned bitbuf = 0;
	int pred[3], row, col, bit = -1, c, i;

	read_shorts((ushort *)diff, 1024);
	if (!load_flags) foveon_decoder(1024, 0);

	for (row = 0; row < height; row++) {
		memset(pred, 0, sizeof pred);
		if (!bit && !load_flags && atoi(model + 2) < 14) get4();
		for (col = bit = 0; col < width; col++) {
			if (load_flags) {
				bitbuf = get4();
				FORC3 pred[2 - c] += diff[bitbuf >> c * 10 & 0x3ff];
			} else FORC3{
		  for (dindex = first_decode; dindex->branch[0]; ) {
			if ((bit = (bit - 1) & 31) == 31)
			  for (i = 0; i < 4; i++)
				bitbuf = (bitbuf << 8) + fgetc(ifp);
			dindex = dindex->branch[bitbuf >> bit & 1];
		  }
		  pred[c] += diff[dindex->leaf];
		  if (pred[c] >> 16 && ~pred[c] >> 16) derror();
			}
			FORC3 image[row*width + col][c] = pred[c];
		}
	}
}

void CLASS foveon_huff(ushort *huff)
{
	int i, j, clen, code;

	huff[0] = 8;
	for (i = 0; i < 13; i++) {
		clen = getc(ifp);
		code = getc(ifp);
		for (j = 0; j < 256 >> clen; )
			huff[code + ++j] = clen << 8 | i;
	}
	get2();
}

void CLASS foveon_dp_load_raw()
{
	unsigned c, roff[4], row, col, diff;
	ushort huff[512], vpred[2][2], hpred[2];

	fseek(ifp, 8, SEEK_CUR);
	foveon_huff(huff);
	roff[0] = 48;
	FORC3 roff[c + 1] = -(-(roff[c] + get4()) & -16);
	FORC3{
	  fseek(ifp, data_offset + roff[c], SEEK_SET);
	  getbits(-1);
	  vpred[0][0] = vpred[0][1] = vpred[1][0] = vpred[1][1] = 512;
	  for (row = 0; row < height; row++) {
		for (col = 0; col < width; col++) {
	  diff = ljpeg_diff(huff);
	  if (col < 2) hpred[col] = vpred[row & 1][col] += diff;
	  else hpred[col & 1] += diff;
	  image[row*width + col][c] = hpred[col & 1];
		}
	  }
	}
}

void CLASS foveon_load_camf()
{
	unsigned type, wide, high, i, j, row, col, diff;
	ushort huff[258], vpred[2][2] = { {512,512},{512,512} }, hpred[2];

	fseek(ifp, meta_offset, SEEK_SET);
	type = get4();  get4();  get4();
	wide = get4();
	high = get4();
	if (type == 2) {
		fread(meta_data, 1, meta_length, ifp);
		for (i = 0; i < meta_length; i++) {
			high = (high * 1597 + 51749) % 244944;
			wide = high * (INT64)301593171 >> 24;
			meta_data[i] ^= ((((high << 8) - wide) >> 1) + wide) >> 17;
		}
	} else if (type == 4) {
		free(meta_data);
		meta_data = (char *)malloc(meta_length = wide*high * 3 / 2);
		merror(meta_data, "foveon_load_camf()");
		foveon_huff(huff);
		get4();
		getbits(-1);
		for (j = row = 0; row < high; row++) {
			for (col = 0; col < wide; col++) {
				diff = ljpeg_diff(huff);
				if (col < 2) hpred[col] = vpred[row & 1][col] += diff;
				else         hpred[col & 1] += diff;
				if (col & 1) {
					meta_data[j++] = hpred[0] >> 4;
					meta_data[j++] = hpred[0] << 4 | hpred[1] >> 8;
					meta_data[j++] = hpred[1];
				}
			}
		}
	} else
		fprintf(stderr, _("%s has unknown CAMF type %d.\n"), ifname, type);
}

const char * CLASS foveon_camf_param(const char *block, const char *param)
{
	unsigned idx, num;
	char *pos, *cp, *dp;

	for (idx = 0; idx < meta_length; idx += sget4(pos + 8)) {
		pos = meta_data + idx;
		if (strncmp(pos, "CMb", 3)) break;
		if (pos[3] != 'P') continue;
		if (strcmp(block, pos + sget4(pos + 12))) continue;
		cp = pos + sget4(pos + 16);
		num = sget4(cp);
		dp = pos + sget4(cp + 4);
		while (num--) {
			cp += 8;
			if (!strcmp(param, dp + sget4(cp)))
				return dp + sget4(cp + 4);
		}
	}
	return 0;
}

void * CLASS foveon_camf_matrix(unsigned dim[3], const char *name)
{
	unsigned i, idx, type, ndim, size, *mat;
	char *pos, *cp, *dp;
	double dsize;

	for (idx = 0; idx < meta_length; idx += sget4(pos + 8)) {
		pos = meta_data + idx;
		if (strncmp(pos, "CMb", 3)) break;
		if (pos[3] != 'M') continue;
		if (strcmp(name, pos + sget4(pos + 12))) continue;
		dim[0] = dim[1] = dim[2] = 1;
		cp = pos + sget4(pos + 16);
		type = sget4(cp);
		if ((ndim = sget4(cp + 4)) > 3) break;
		dp = pos + sget4(cp + 8);
		for (i = ndim; i--; ) {
			cp += 12;
			dim[i] = sget4(cp);
		}
		if ((dsize = (double)dim[0] * dim[1] * dim[2]) > meta_length / 4) break;
		mat = (unsigned *)malloc((size = dsize) * 4);
		merror(mat, "foveon_camf_matrix()");
		for (i = 0; i < size; i++)
			if (type && type != 6)
				mat[i] = sget4(dp + i * 4);
			else
				mat[i] = sget4(dp + i * 2) & 0xffff;
		return mat;
	}
	fprintf(stderr, _("%s: \"%s\" matrix not found!\n"), ifname, name);
	return 0;
}

int CLASS foveon_fixed(void *ptr, int size, const char *name)
{
	void *dp;
	unsigned dim[3];

	if (!name) return 0;
	dp = foveon_camf_matrix(dim, name);
	if (!dp) return 0;
	memcpy(ptr, dp, size * 4);
	free(dp);
	return 1;
}

float CLASS foveon_avg(short *pix, int range[2], float cfilt)
{
	int i;
	float val, min = FLT_MAX, max = -FLT_MAX, sum = 0;

	for (i = range[0]; i <= range[1]; i++) {
		sum += val = pix[i * 4] + (pix[i * 4] - pix[(i - 1) * 4]) * cfilt;
		if (min > val) min = val;
		if (max < val) max = val;
	}
	if (range[1] - range[0] == 1) return sum / 2;
	return (sum - min - max) / (range[1] - range[0] - 1);
}

short * CLASS foveon_make_curve(double max, double mul, double filt)
{
	short *curve;
	unsigned i, size;
	double x;

	if (!filt) filt = 0.8;
	size = 4 * M_PI*max / filt;
	if (size == UINT_MAX) size--;
	curve = (short *)calloc(size + 1, sizeof *curve);
	merror(curve, "foveon_make_curve()");
	curve[0] = size;
	for (i = 0; i < size; i++) {
		x = i*filt / max / 4;
		curve[i + 1] = (cos(x) + 1) / 2 * tanh(i*filt / mul) * mul + 0.5;
	}
	return curve;
}

void CLASS foveon_make_curves
(short **curvep, float dq[3], float div[3], float filt)
{
	double mul[3], max = 0;
	int c;

	FORC3 mul[c] = dq[c] / div[c];
	FORC3 if (max < mul[c]) max = mul[c];
	FORC3 curvep[c] = foveon_make_curve(max, mul[c], filt);
}

int CLASS foveon_apply_curve(short *curve, int i)
{
	if (abs(i) >= curve[0]) return 0;
	return i < 0 ? -curve[1 - i] : curve[1 + i];
}

#define image ((short (*)[4]) image)

void CLASS foveon_interpolate()
{
	static const short hood[] = { -1,-1, -1,0, -1,1, 0,-1, 0,1, 1,-1, 1,0, 1,1 };
	short *pix, prev[3], *curve[8], (*shrink)[3];
	float cfilt = 0, ddft[3][3][2], ppm[3][3][3];
	float cam_xyz[3][3], correct[3][3], last[3][3], trans[3][3];
	float chroma_dq[3], color_dq[3], diag[3][3], div[3];
	float(*black)[3], (*sgain)[3], (*sgrow)[3];
	float fsum[3], val, frow, num;
	int row, col, c, i, j, diff, sgx, irow, sum, min, max, limit;
	int dscr[2][2], dstb[4], (*smrow[7])[3], total[4], ipix[3];
	int work[3][3], smlast, smred, smred_p = 0, dev[3];
	int satlev[3], keep[4], active[4];
	unsigned dim[3], *badpix;
	double dsum = 0, trsum[3];
	char str[128];
	const char* cp;

	if (verbose)
		fprintf(stderr, _("Foveon interpolation...\n"));

	foveon_load_camf();
	foveon_fixed(dscr, 4, "DarkShieldColRange");
	foveon_fixed(ppm[0][0], 27, "PostPolyMatrix");
	foveon_fixed(satlev, 3, "SaturationLevel");
	foveon_fixed(keep, 4, "KeepImageArea");
	foveon_fixed(active, 4, "ActiveImageArea");
	foveon_fixed(chroma_dq, 3, "ChromaDQ");
	foveon_fixed(color_dq, 3,
		foveon_camf_param("IncludeBlocks", "ColorDQ") ?
		"ColorDQ" : "ColorDQCamRGB");
	if (foveon_camf_param("IncludeBlocks", "ColumnFilter"))
		foveon_fixed(&cfilt, 1, "ColumnFilter");

	memset(ddft, 0, sizeof ddft);
	if (!foveon_camf_param("IncludeBlocks", "DarkDrift")
		|| !foveon_fixed(ddft[1][0], 12, "DarkDrift"))
		for (i = 0; i < 2; i++) {
			foveon_fixed(dstb, 4, i ? "DarkShieldBottom" : "DarkShieldTop");
			for (row = dstb[1]; row <= dstb[3]; row++)
				for (col = dstb[0]; col <= dstb[2]; col++)
					FORC3 ddft[i + 1][c][1] += (short)image[row*width + col][c];
			FORC3 ddft[i + 1][c][1] /= (dstb[3] - dstb[1] + 1) * (dstb[2] - dstb[0] + 1);
		}

	if (!(cp = foveon_camf_param("WhiteBalanceIlluminants", model2)))
	{
		fprintf(stderr, _("%s: Invalid white balance \"%s\"\n"), ifname, model2);
		return;
	}
	foveon_fixed(cam_xyz, 9, cp);
	foveon_fixed(correct, 9,
		foveon_camf_param("WhiteBalanceCorrections", model2));
	memset(last, 0, sizeof last);
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			FORC3 last[i][j] += correct[i][c] * cam_xyz[c][j];

#define LAST(x,y) last[(i+x)%3][(c+y)%3]
	for (i = 0; i < 3; i++)
		FORC3 diag[c][i] = LAST(1, 1)*LAST(2, 2) - LAST(1, 2)*LAST(2, 1);
#undef LAST
	FORC3 div[c] = diag[c][0] * 0.3127 + diag[c][1] * 0.329 + diag[c][2] * 0.3583;
	sprintf(str, "%sRGBNeutral", model2);
	if (foveon_camf_param("IncludeBlocks", str))
		foveon_fixed(div, 3, str);
	num = 0;
	FORC3 if (num < div[c]) num = div[c];
	FORC3 div[c] /= num;

	memset(trans, 0, sizeof trans);
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			FORC3 trans[i][j] += rgb_cam[i][c] * last[c][j] * div[j];
	FORC3 trsum[c] = trans[c][0] + trans[c][1] + trans[c][2];
	dsum = (6 * trsum[0] + 11 * trsum[1] + 3 * trsum[2]) / 20;
	for (i = 0; i < 3; i++)
		FORC3 last[i][c] = trans[i][c] * dsum / trsum[i];
	memset(trans, 0, sizeof trans);
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			FORC3 trans[i][j] += (i == c ? 32 : -1) * last[c][j] / 30;

	foveon_make_curves(curve, color_dq, div, cfilt);
	FORC3 chroma_dq[c] /= 3;
	foveon_make_curves(curve + 3, chroma_dq, div, cfilt);
	FORC3 dsum += chroma_dq[c] / div[c];
	curve[6] = foveon_make_curve(dsum, dsum, cfilt);
	curve[7] = foveon_make_curve(dsum * 2, dsum * 2, cfilt);

	sgain = (float(*)[3]) foveon_camf_matrix(dim, "SpatialGain");
	if (!sgain) return;
	sgrow = (float(*)[3]) calloc(dim[1], sizeof *sgrow);
	sgx = (width + dim[1] - 2) / (dim[1] - 1);

	black = (float(*)[3]) calloc(height, sizeof *black);
	for (row = 0; row < height; row++) {
		for (i = 0; i < 6; i++)
			((float *)ddft[0])[i] = ((float *)ddft[1])[i] +
			row / (height - 1.0) * (((float *)ddft[2])[i] - ((float *)ddft[1])[i]);
		FORC3 black[row][c] =
			(foveon_avg(image[row*width] + c, dscr[0], cfilt) +
				foveon_avg(image[row*width] + c, dscr[1], cfilt) * 3
				- ddft[0][c][0]) / 4 - ddft[0][c][1];
	}
	memcpy(black, black + 8, sizeof *black * 8);
	memcpy(black + height - 11, black + height - 22, 11 * sizeof *black);
	memcpy(last, black, sizeof last);

	for (row = 1; row < height - 1; row++) {
		FORC3 if (last[1][c] > last[0][c]) {
			if (last[1][c] > last[2][c])
				black[row][c] = (last[0][c] > last[2][c]) ? last[0][c] : last[2][c];
		} else
			if (last[1][c] < last[2][c])
				black[row][c] = (last[0][c] < last[2][c]) ? last[0][c] : last[2][c];
		memmove(last, last + 1, 2 * sizeof last[0]);
		memcpy(last[2], black[row + 1], sizeof last[2]);
	}
	FORC3 black[row][c] = (last[0][c] + last[1][c]) / 2;
	FORC3 black[0][c] = (black[1][c] + black[3][c]) / 2;

	val = 1 - exp(-1 / 24.0);
	memcpy(fsum, black, sizeof fsum);
	for (row = 1; row < height; row++)
		FORC3 fsum[c] += black[row][c] =
		(black[row][c] - black[row - 1][c])*val + black[row - 1][c];
	memcpy(last[0], black[height - 1], sizeof last[0]);
	FORC3 fsum[c] /= height;
	for (row = height; row--; )
		FORC3 last[0][c] = black[row][c] =
		(black[row][c] - fsum[c] - last[0][c])*val + last[0][c];

	memset(total, 0, sizeof total);
	for (row = 2; row < height; row += 4)
		for (col = 2; col < width; col += 4) {
			FORC3 total[c] += (short)image[row*width + col][c];
			total[3]++;
		}
	for (row = 0; row < height; row++)
		FORC3 black[row][c] += fsum[c] / 2 + total[c] / (total[3] * 100.0);

	for (row = 0; row < height; row++) {
		for (i = 0; i < 6; i++)
			((float *)ddft[0])[i] = ((float *)ddft[1])[i] +
			row / (height - 1.0) * (((float *)ddft[2])[i] - ((float *)ddft[1])[i]);
		pix = image[row*width];
		memcpy(prev, pix, sizeof prev);
		frow = row / (height - 1.0) * (dim[2] - 1);
		if ((irow = frow) == dim[2] - 1) irow--;
		frow -= irow;
		for (i = 0; i < dim[1]; i++)
			FORC3 sgrow[i][c] = sgain[irow   *dim[1] + i][c] * (1 - frow) +
			sgain[(irow + 1)*dim[1] + i][c] * frow;
		for (col = 0; col < width; col++) {
			FORC3{
		  diff = pix[c] - prev[c];
		  prev[c] = pix[c];
		  ipix[c] = pix[c] + floor((diff + (diff*diff >> 14)) * cfilt
			  - ddft[0][c][1] - ddft[0][c][0] * ((float)col / width - 0.5)
			  - black[row][c]);
			}
				FORC3{
			  work[0][c] = ipix[c] * ipix[c] >> 14;
			  work[2][c] = ipix[c] * work[0][c] >> 14;
			  work[1][2 - c] = ipix[(c + 1) % 3] * ipix[(c + 2) % 3] >> 14;
			}
				FORC3{
			  for (val = i = 0; i < 3; i++)
				for (j = 0; j < 3; j++)
				  val += ppm[c][i][j] * work[i][j];
			  ipix[c] = floor((ipix[c] + floor(val)) *
				  (sgrow[col / sgx][c] * (sgx - col%sgx) +
					sgrow[col / sgx + 1][c] * (col%sgx)) / sgx / div[c]);
			  if (ipix[c] > 32000) ipix[c] = 32000;
			  pix[c] = ipix[c];
			}
			pix += 4;
		}
	}
	free(black);
	free(sgrow);
	free(sgain);

	if ((badpix = (unsigned *)foveon_camf_matrix(dim, "BadPixels"))) {
		for (i = 0; i < dim[0]; i++) {
			col = (badpix[i] >> 8 & 0xfff) - keep[0];
			row = (badpix[i] >> 20) - keep[1];
			if ((unsigned)(row - 1) > height - 3 || (unsigned)(col - 1) > width - 3)
				continue;
			memset(fsum, 0, sizeof fsum);
			for (sum = j = 0; j < 8; j++)
				if (badpix[i] & (1 << j)) {
					FORC3 fsum[c] += (short)
						image[(row + hood[j * 2])*width + col + hood[j * 2 + 1]][c];
					sum++;
				}
			if (sum) FORC3 image[row*width + col][c] = fsum[c] / sum;
		}
		free(badpix);
	}

	/* Array for 5x5 Gaussian averaging of red values */
	smrow[6] = (int(*)[3]) calloc(width * 5, sizeof **smrow);
	merror(smrow[6], "foveon_interpolate()");
	for (i = 0; i < 5; i++)
		smrow[i] = smrow[6] + i*width;

	/* Sharpen the reds against these Gaussian averages */
	for (smlast = -1, row = 2; row < height - 2; row++) {
		while (smlast < row + 2) {
			for (i = 0; i < 6; i++)
				smrow[(i + 5) % 6] = smrow[i];
			pix = image[++smlast*width + 2];
			for (col = 2; col < width - 2; col++) {
				smrow[4][col][0] =
					(pix[0] * 6 + (pix[-4] + pix[4]) * 4 + pix[-8] + pix[8] + 8) >> 4;
				pix += 4;
			}
		}
		pix = image[row*width + 2];
		for (col = 2; col < width - 2; col++) {
			smred = (6 * smrow[2][col][0]
				+ 4 * (smrow[1][col][0] + smrow[3][col][0])
				+ smrow[0][col][0] + smrow[4][col][0] + 8) >> 4;
			if (col == 2)
				smred_p = smred;
			i = pix[0] + ((pix[0] - ((smred * 7 + smred_p) >> 3)) >> 3);
			if (i > 32000) i = 32000;
			pix[0] = i;
			smred_p = smred;
			pix += 4;
		}
	}

	/* Adjust the brighter pixels for better linearity */
	min = 0xffff;
	FORC3{
	  i = satlev[c] / div[c];
	  if (min > i) min = i;
	}
	limit = min * 9 >> 4;
	for (pix = image[0]; pix < image[height*width]; pix += 4) {
		if (pix[0] <= limit || pix[1] <= limit || pix[2] <= limit)
			continue;
		min = max = pix[0];
		for (c = 1; c < 3; c++) {
			if (min > pix[c]) min = pix[c];
			if (max < pix[c]) max = pix[c];
		}
		if (min >= limit * 2) {
			pix[0] = pix[1] = pix[2] = max;
		} else {
			i = 0x4000 - ((min - limit) << 14) / limit;
			i = 0x4000 - (i*i >> 14);
			i = i*i >> 14;
			FORC3 pix[c] += (max - pix[c]) * i >> 14;
		}
	}
	/*
	   Because photons that miss one detector often hit another,
	   the sum R+G+B is much less noisy than the individual colors.
	   So smooth the hues without smoothing the total.
	 */
	for (smlast = -1, row = 2; row < height - 2; row++) {
		while (smlast < row + 2) {
			for (i = 0; i < 6; i++)
				smrow[(i + 5) % 6] = smrow[i];
			pix = image[++smlast*width + 2];
			for (col = 2; col < width - 2; col++) {
				FORC3 smrow[4][col][c] = (pix[c - 4] + 2 * pix[c] + pix[c + 4] + 2) >> 2;
				pix += 4;
			}
		}
		pix = image[row*width + 2];
		for (col = 2; col < width - 2; col++) {
			FORC3 dev[c] = -foveon_apply_curve(curve[7], pix[c] -
				((smrow[1][col][c] + 2 * smrow[2][col][c] + smrow[3][col][c]) >> 2));
			sum = (dev[0] + dev[1] + dev[2]) >> 3;
			FORC3 pix[c] += dev[c] - sum;
			pix += 4;
		}
	}
	for (smlast = -1, row = 2; row < height - 2; row++) {
		while (smlast < row + 2) {
			for (i = 0; i < 6; i++)
				smrow[(i + 5) % 6] = smrow[i];
			pix = image[++smlast*width + 2];
			for (col = 2; col < width - 2; col++) {
				FORC3 smrow[4][col][c] =
					(pix[c - 8] + pix[c - 4] + pix[c] + pix[c + 4] + pix[c + 8] + 2) >> 2;
				pix += 4;
			}
		}
		pix = image[row*width + 2];
		for (col = 2; col < width - 2; col++) {
			for (total[3] = 375, sum = 60, c = 0; c < 3; c++) {
				for (total[c] = i = 0; i < 5; i++)
					total[c] += smrow[i][col][c];
				total[3] += total[c];
				sum += pix[c];
			}
			if (sum < 0) sum = 0;
			j = total[3] > 375 ? (sum << 16) / total[3] : sum * 174;
			FORC3 pix[c] += foveon_apply_curve(curve[6],
				((j*total[c] + 0x8000) >> 16) - pix[c]);
			pix += 4;
		}
	}

	/* Transform the image to a different colorspace */
	for (pix = image[0]; pix < image[height*width]; pix += 4) {
		FORC3 pix[c] -= foveon_apply_curve(curve[c], pix[c]);
		sum = (pix[0] + pix[1] + pix[1] + pix[2]) >> 2;
		FORC3 pix[c] -= foveon_apply_curve(curve[c], pix[c] - sum);
		FORC3{
		  for (dsum = i = 0; i < 3; i++)
		dsum += trans[c][i] * pix[i];
		  if (dsum < 0)  dsum = 0;
		  if (dsum > 24000) dsum = 24000;
		  ipix[c] = dsum + 0.5;
		}
		FORC3 pix[c] = ipix[c];
	}

	/* Smooth the image bottom-to-top and save at 1/4 scale */
	shrink = (short(*)[3]) calloc((height / 4), (width / 4) * sizeof *shrink);
	merror(shrink, "foveon_interpolate()");
	for (row = height / 4; row--; )
		for (col = 0; col < width / 4; col++) {
			ipix[0] = ipix[1] = ipix[2] = 0;
			for (i = 0; i < 4; i++)
				for (j = 0; j < 4; j++)
					FORC3 ipix[c] += image[(row * 4 + i)*width + col * 4 + j][c];
			FORC3
				if (row + 2 > height / 4)
					shrink[row*(width / 4) + col][c] = ipix[c] >> 4;
				else
					shrink[row*(width / 4) + col][c] =
					(shrink[(row + 1)*(width / 4) + col][c] * 1840 + ipix[c] * 141 + 2048) >> 12;
		}
	/* From the 1/4-scale image, smooth right-to-left */
	for (row = 0; row < (height & ~3); row++) {
		ipix[0] = ipix[1] = ipix[2] = 0;
		if ((row & 3) == 0)
			for (col = width & ~3; col--; )
				FORC3 smrow[0][col][c] = ipix[c] =
				(shrink[(row / 4)*(width / 4) + col / 4][c] * 1485 + ipix[c] * 6707 + 4096) >> 13;

		/* Then smooth left-to-right */
		ipix[0] = ipix[1] = ipix[2] = 0;
		for (col = 0; col < (width & ~3); col++)
			FORC3 smrow[1][col][c] = ipix[c] =
			(smrow[0][col][c] * 1485 + ipix[c] * 6707 + 4096) >> 13;

		/* Smooth top-to-bottom */
		if (row == 0)
			memcpy(smrow[2], smrow[1], sizeof **smrow * width);
		else
			for (col = 0; col < (width & ~3); col++)
				FORC3 smrow[2][col][c] =
				(smrow[2][col][c] * 6707 + smrow[1][col][c] * 1485 + 4096) >> 13;

		/* Adjust the chroma toward the smooth values */
		for (col = 0; col < (width & ~3); col++) {
			for (i = j = 30, c = 0; c < 3; c++) {
				i += smrow[2][col][c];
				j += image[row*width + col][c];
			}
			j = (j << 16) / i;
			for (sum = c = 0; c < 3; c++) {
				ipix[c] = foveon_apply_curve(curve[c + 3],
					((smrow[2][col][c] * j + 0x8000) >> 16) - image[row*width + col][c]);
				sum += ipix[c];
			}
			sum >>= 3;
			FORC3{
		  i = image[row*width + col][c] + ipix[c] - sum;
		  if (i < 0) i = 0;
		  image[row*width + col][c] = i;
			}
		}
	}
	free(shrink);
	free(smrow[6]);
	for (i = 0; i < 8; i++)
		free(curve[i]);

	/* Trim off the black border */
	active[1] -= keep[1];
	active[3] -= 2;
	i = active[2] - active[0];
	for (row = 0; row < active[3] - active[1]; row++)
		memcpy(image[row*i], image[(row + active[1])*width + active[0]],
			i * sizeof *image);
	width = i;
	height = row;
}
#undef image

/* RESTRICTED code ends here */

void CLASS crop_masked_pixels()
{
	int row, col;
	unsigned r, c, m, mblack[8], zero, val;

	if (load_raw == &CLASS phase_one_load_raw ||
		load_raw == &CLASS phase_one_load_raw_c)
		phase_one_correct();
	for (row = 0; row < height; row++)
		for (col = 0; col < width; col++)
			BAYER2(row, col) = RAW(row + top_margin, col + left_margin);
	if (mask[0][3] > 0) goto mask_set;
	if (load_raw == &CLASS lossless_jpeg_load_raw) {
		mask[0][1] = mask[1][1] += 2;
		mask[0][3] -= 2;
		goto sides;
	}
	if ((load_raw == &CLASS eight_bit_load_raw && strncmp(model, "DC2", 3)) ||
		(load_raw == &CLASS packed_load_raw && (load_flags & 32))) {
	sides:
		mask[0][0] = mask[1][0] = top_margin;
		mask[0][2] = mask[1][2] = top_margin + height;
		mask[0][3] += left_margin;
		mask[1][1] += left_margin + width;
		mask[1][3] += raw_width;
	}
	if (load_raw == &CLASS nokia_load_raw) {
		mask[0][2] = top_margin;
		mask[0][3] = width;
	}
mask_set:
	memset(mblack, 0, sizeof mblack);
	for (zero = m = 0; m < 8; m++)
		for (row = MAX(mask[m][0], 0); row < MIN(mask[m][2], raw_height); row++)
			for (col = MAX(mask[m][1], 0); col < MIN(mask[m][3], raw_width); col++) {
				c = FC(row - top_margin, col - left_margin);
				mblack[c] += val = RAW(row, col);
				mblack[4 + c]++;
				zero += !val;
			}
	if (zero < mblack[4] && mblack[5] && mblack[6] && mblack[7]) {
		FORC4 cblack[c] = mblack[c] / mblack[4 + c];
		cblack[4] = cblack[5] = cblack[6] = 0;
	}
}

void CLASS remove_zeroes()
{
	unsigned row, col, tot, n, r, c;

	for (row = 0; row < height; row++)
		for (col = 0; col < width; col++)
			if (BAYER(row, col) == 0) {
				tot = n = 0;
				for (r = row - 2; r <= row + 2; r++)
					for (c = col - 2; c <= col + 2; c++)
						if (r < height && c < width &&
							FC(r, c) == FC(row, col) && BAYER(r, c))
							tot += (n++, BAYER(r, c));
				if (n) BAYER(row, col) = tot / n;
			}
}

/*
   Seach from the current directory up to the root looking for
   a ".badpixels" file, and fix those pixels now.
 */
void CLASS bad_pixels(const char *cfname)
{
	FILE *fp = 0;
	char *fname, *cp, line[128];
	int len, time, row, col, r, c, rad, tot, n, fixed = 0;

	if (!filters) return;
	if (cfname)
		fp = fopen(cfname, "r");
	else {
		for (len = 32; ; len *= 2) {
			fname = (char *)malloc(len);
			if (!fname) return;
			if (getcwd(fname, len - 16)) break;
			free(fname);
			if (errno != ERANGE) return;
		}
		if (fname[1] == ':')
			memmove(fname, fname + 2, len - 2);
		for (cp = fname; *cp; cp++)
			if (*cp == '\\') *cp = '/';
		cp = fname + strlen(fname);
		if (cp[-1] == '/') cp--;
		while (*fname == '/') {
			strcpy(cp, "/.badpixels");
			if ((fp = fopen(fname, "r"))) break;
			if (cp == fname) break;
			while (*--cp != '/');
		}
		free(fname);
	}
	if (!fp) return;
	while (fgets(line, 128, fp)) {
		cp = strchr(line, '#');
		if (cp) *cp = 0;
		if (sscanf(line, "%d %d %d", &col, &row, &time) != 3) continue;
		if ((unsigned)col >= width || (unsigned)row >= height) continue;
		if (time > timestamp) continue;
		for (tot = n = 0, rad = 1; rad < 3 && n == 0; rad++)
			for (r = row - rad; r <= row + rad; r++)
				for (c = col - rad; c <= col + rad; c++)
					if ((unsigned)r < height && (unsigned)c < width &&
						(r != row || c != col) && fcol(r, c) == fcol(row, col)) {
						tot += BAYER2(r, c);
						n++;
					}
		BAYER2(row, col) = tot / n;
		if (verbose) {
			if (!fixed++)
				fprintf(stderr, _("Fixed dead pixels at:"));
			fprintf(stderr, " %d,%d", col, row);
		}
	}
	if (fixed) fputc('\n', stderr);
	fclose(fp);
}

void CLASS subtract(const char *fname)
{
	FILE *fp;
	int dim[3] = { 0,0,0 }, comment = 0, number = 0, error = 0, nd = 0, c, row, col;
	ushort *pixel;

	if (!(fp = fopen(fname, "rb"))) {
		perror(fname);  return;
	}
	if (fgetc(fp) != 'P' || fgetc(fp) != '5') error = 1;
	while (!error && nd < 3 && (c = fgetc(fp)) != EOF) {
		if (c == '#')  comment = 1;
		if (c == '\n') comment = 0;
		if (comment) continue;
		if (isdigit(c)) number = 1;
		if (number) {
			if (isdigit(c)) dim[nd] = dim[nd] * 10 + c - '0';
			else if (isspace(c)) {
				number = 0;  nd++;
			} else error = 1;
		}
	}
	if (error || nd < 3) {
		fprintf(stderr, _("%s is not a valid PGM file!\n"), fname);
		fclose(fp);  return;
	} else if (dim[0] != width || dim[1] != height || dim[2] != 65535) {
		fprintf(stderr, _("%s has the wrong dimensions!\n"), fname);
		fclose(fp);  return;
	}
	pixel = (ushort *)calloc(width, sizeof *pixel);
	merror(pixel, "subtract()");
	for (row = 0; row < height; row++) {
		fread(pixel, 2, width, fp);
		for (col = 0; col < width; col++)
			BAYER(row, col) = MAX(BAYER(row, col) - ntohs(pixel[col]), 0);
	}
	free(pixel);
	fclose(fp);
	memset(cblack, 0, sizeof cblack);
	black = 0;
}

void CLASS gamma_curve(double pwr, double ts, int mode, int imax)
{
	int i;
	double g[6], bnd[2] = { 0,0 }, r;

	g[0] = pwr;
	g[1] = ts;
	g[2] = g[3] = g[4] = 0;
	bnd[g[1] >= 1] = 1;
	if (g[1] && (g[1] - 1)*(g[0] - 1) <= 0) {
		for (i = 0; i < 48; i++) {
			g[2] = (bnd[0] + bnd[1]) / 2;
			if (g[0]) bnd[(pow(g[2] / g[1], -g[0]) - 1) / g[0] - 1 / g[2] > -1] = g[2];
			else	bnd[g[2] / exp(1 - 1 / g[2]) < g[1]] = g[2];
		}
		g[3] = g[2] / g[1];
		if (g[0]) g[4] = g[2] * (1 / g[0] - 1);
	}
	if (g[0]) g[5] = 1 / (g[1] * SQR(g[3]) / 2 - g[4] * (1 - g[3]) +
		(1 - pow(g[3], 1 + g[0]))*(1 + g[4]) / (1 + g[0])) - 1;
	else      g[5] = 1 / (g[1] * SQR(g[3]) / 2 + 1
		- g[2] - g[3] - g[2] * g[3] * (log(g[3]) - 1)) - 1;
	if (!mode--) {
		memcpy(gamm, g, sizeof gamm);
		return;
	}
	for (i = 0; i < 0x10000; i++) {
		curve[i] = 0xffff;
		if ((r = (double)i / imax) < 1)
			curve[i] = 0x10000 * (mode
				? (r < g[3] ? r*g[1] : (g[0] ? pow(r, g[0])*(1 + g[4]) - g[4] : log(r)*g[2] + 1))
				: (r < g[2] ? r / g[1] : (g[0] ? pow((r + g[4]) / (1 + g[4]), 1 / g[0]) : exp((r - 1) / g[2]))));
	}
}

void CLASS pseudoinverse(double(*in)[3], double(*out)[3], int size)
{
	double work[3][6], num;
	int i, j, k;

	for (i = 0; i < 3; i++) {
		for (j = 0; j < 6; j++)
			work[i][j] = j == i + 3;
		for (j = 0; j < 3; j++)
			for (k = 0; k < size; k++)
				work[i][j] += in[k][i] * in[k][j];
	}
	for (i = 0; i < 3; i++) {
		num = work[i][i];
		for (j = 0; j < 6; j++)
			work[i][j] /= num;
		for (k = 0; k < 3; k++) {
			if (k == i) continue;
			num = work[k][i];
			for (j = 0; j < 6; j++)
				work[k][j] -= work[i][j] * num;
		}
	}
	for (i = 0; i < size; i++)
		for (j = 0; j < 3; j++)
			for (out[i][j] = k = 0; k < 3; k++)
				out[i][j] += work[j][k + 3] * in[i][k];
}

void CLASS cam_xyz_coeff(float rgb_cam[3][4], double cam_xyz[4][3])
{
	double cam_rgb[4][3], inverse[4][3], num;
	int i, j, k;

	for (i = 0; i < colors; i++)		/* Multiply out XYZ colorspace */
		for (j = 0; j < 3; j++)
			for (cam_rgb[i][j] = k = 0; k < 3; k++)
				cam_rgb[i][j] += cam_xyz[i][k] * xyz_rgb[k][j];

	for (i = 0; i < colors; i++) {		/* Normalize cam_rgb so that */
		for (num = j = 0; j < 3; j++)		/* cam_rgb * (1,1,1) is (1,1,1,1) */
			num += cam_rgb[i][j];
		for (j = 0; j < 3; j++)
			cam_rgb[i][j] /= num;
		pre_mul[i] = 1 / num;
	}
	pseudoinverse(cam_rgb, inverse, colors);
	for (i = 0; i < 3; i++)
		for (j = 0; j < colors; j++)
			rgb_cam[i][j] = inverse[j][i];
}

void CLASS hat_transform(float *temp, float *base, int st, int size, int sc)
{
	int i;
	for (i = 0; i < sc; i++)
		temp[i] = 2 * base[st*i] + base[st*(sc - i)] + base[st*(i + sc)];
	for (; i + sc < size; i++)
		temp[i] = 2 * base[st*i] + base[st*(i - sc)] + base[st*(i + sc)];
	for (; i < size; i++)
		temp[i] = 2 * base[st*i] + base[st*(i - sc)] + base[st*(2 * size - 2 - (i + sc))];
}

void CLASS wavelet_denoise()
{
	float *fimg = 0, *temp, thold, mul[2], avg, diff;
	int scale = 1, size, lev, hpass, lpass, row, col, nc, c, i, wlast, blk[2];
	ushort *window[4];
	static const float noise[] =
	{ 0.8002,0.2735,0.1202,0.0585,0.0291,0.0152,0.0080,0.0044 };

	if (verbose) fprintf(stderr, _("Wavelet denoising...\n"));

	while (maximum << scale < 0x10000) scale++;
	maximum <<= --scale;
	black <<= scale;
	FORC4 cblack[c] <<= scale;
	if ((size = iheight*iwidth) < 0x15550000)
		fimg = (float *)malloc((size * 3 + iheight + iwidth) * sizeof *fimg);
	merror(fimg, "wavelet_denoise()");
	temp = fimg + size * 3;
	if ((nc = colors) == 3 && filters) nc++;
	FORC(nc) {			/* denoise R,G1,B,G3 individually */
		for (i = 0; i < size; i++)
			fimg[i] = 256 * sqrt(image[i][c] << scale);
		for (hpass = lev = 0; lev < 5; lev++) {
			lpass = size*((lev & 1) + 1);
			for (row = 0; row < iheight; row++) {
				hat_transform(temp, fimg + hpass + row*iwidth, 1, iwidth, 1 << lev);
				for (col = 0; col < iwidth; col++)
					fimg[lpass + row*iwidth + col] = temp[col] * 0.25;
			}
			for (col = 0; col < iwidth; col++) {
				hat_transform(temp, fimg + lpass + col, iwidth, iheight, 1 << lev);
				for (row = 0; row < iheight; row++)
					fimg[lpass + row*iwidth + col] = temp[row] * 0.25;
			}
			thold = threshold * noise[lev];
			for (i = 0; i < size; i++) {
				fimg[hpass + i] -= fimg[lpass + i];
				if (fimg[hpass + i] < -thold) fimg[hpass + i] += thold;
				else if (fimg[hpass + i] > thold) fimg[hpass + i] -= thold;
				else	 fimg[hpass + i] = 0;
				if (hpass) fimg[i] += fimg[hpass + i];
			}
			hpass = lpass;
		}
		for (i = 0; i < size; i++)
			image[i][c] = CLIP(SQR(fimg[i] + fimg[lpass + i]) / 0x10000);
	}
	if (filters && colors == 3) {  /* pull G1 and G3 closer together */
		for (row = 0; row < 2; row++) {
			mul[row] = 0.125 * pre_mul[FC(row + 1, 0) | 1] / pre_mul[FC(row, 0) | 1];
			blk[row] = cblack[FC(row, 0) | 1];
		}
		for (i = 0; i < 4; i++)
			window[i] = (ushort *)fimg + width*i;
		for (wlast = -1, row = 1; row < height - 1; row++) {
			while (wlast < row + 1) {
				for (wlast++, i = 0; i < 4; i++)
					window[(i + 3) & 3] = window[i];
				for (col = FC(wlast, 1) & 1; col < width; col += 2)
					window[2][col] = BAYER(wlast, col);
			}
			thold = threshold / 512;
			for (col = (FC(row, 0) & 1) + 1; col < width - 1; col += 2) {
				avg = (window[0][col - 1] + window[0][col + 1] +
					window[2][col - 1] + window[2][col + 1] - blk[~row & 1] * 4)
					* mul[row & 1] + (window[1][col] + blk[row & 1]) * 0.5;
				avg = avg < 0 ? 0 : sqrt(avg);
				diff = sqrt(BAYER(row, col)) - avg;
				if (diff < -thold) diff += thold;
				else if (diff > thold) diff -= thold;
				else diff = 0;
				BAYER(row, col) = CLIP(SQR(avg + diff) + 0.5);
			}
		}
	}
	free(fimg);
}

void CLASS scale_colors()
{
	unsigned bottom, right, size, row, col, ur, uc, i, x, y, c, sum[8];
	int val, dark, sat;
	double dsum[8], dmin, dmax;
	float scale_mul[4], fr, fc;
	ushort *img = 0, *pix;

	if (user_mul[0])
		memcpy(pre_mul, user_mul, sizeof pre_mul);
	if (use_auto_wb || (use_camera_wb && cam_mul[0] == -1)) {
		memset(dsum, 0, sizeof dsum);
		bottom = MIN(greybox[1] + greybox[3], height);
		right = MIN(greybox[0] + greybox[2], width);
		for (row = greybox[1]; row < bottom; row += 8)
			for (col = greybox[0]; col < right; col += 8) {
				memset(sum, 0, sizeof sum);
				for (y = row; y < row + 8 && y < bottom; y++)
					for (x = col; x < col + 8 && x < right; x++)
						FORC4{
						  if (filters) {
						c = fcol(y,x);
						val = BAYER2(y,x);
						  } else
						val = image[y*width + x][c];
						  if (val > maximum - 25) goto skip_block;
						  if ((val -= cblack[c]) < 0) val = 0;
						  sum[c] += val;
						  sum[c + 4]++;
						  if (filters) break;
					}
				FORC(8) dsum[c] += sum[c];
			skip_block:;
			}
		FORC4 if (dsum[c]) pre_mul[c] = dsum[c + 4] / dsum[c];
	}
	if (use_camera_wb && cam_mul[0] != -1) {
		memset(sum, 0, sizeof sum);
		for (row = 0; row < 8; row++)
			for (col = 0; col < 8; col++) {
				c = FC(row, col);
				if ((val = white[row][col] - cblack[c]) > 0)
					sum[c] += val;
				sum[c + 4]++;
			}
		if (sum[0] && sum[1] && sum[2] && sum[3])
			FORC4 pre_mul[c] = (float)sum[c + 4] / sum[c];
		else if (cam_mul[0] && cam_mul[2])
			memcpy(pre_mul, cam_mul, sizeof pre_mul);
		else
			fprintf(stderr, _("%s: Cannot use camera white balance.\n"), ifname);
	}
	if (pre_mul[1] == 0) pre_mul[1] = 1;
	if (pre_mul[3] == 0) pre_mul[3] = colors < 4 ? pre_mul[1] : 1;
	dark = black;
	sat = maximum;
	if (threshold) wavelet_denoise();
	maximum -= black;
	for (dmin = DBL_MAX, dmax = c = 0; c < 4; c++) {
		if (dmin > pre_mul[c])
			dmin = pre_mul[c];
		if (dmax < pre_mul[c])
			dmax = pre_mul[c];
	}
	if (!highlight) dmax = dmin;
	FORC4 scale_mul[c] = (pre_mul[c] /= dmax) * 65535.0 / maximum;
	if (verbose) {
		fprintf(stderr,
			_("Scaling with darkness %d, saturation %d, and\nmultipliers"), dark, sat);
		FORC4 fprintf(stderr, " %f", pre_mul[c]);
		fputc('\n', stderr);
	}
	if (filters > 1000 && (cblack[4] + 1) / 2 == 1 && (cblack[5] + 1) / 2 == 1) {
		FORC4 cblack[FC(c / 2, c % 2)] +=
			cblack[6 + c / 2 % cblack[4] * cblack[5] + c % 2 % cblack[5]];
		cblack[4] = cblack[5] = 0;
	}
	size = iheight*iwidth;
	for (i = 0; i < size * 4; i++) {
		if (!(val = ((ushort *)image)[i])) continue;
		if (cblack[4] && cblack[5])
			val -= cblack[6 + i / 4 / iwidth % cblack[4] * cblack[5] +
			i / 4 % iwidth % cblack[5]];
		val -= cblack[i & 3];
		val *= scale_mul[i & 3];
		((ushort *)image)[i] = CLIP(val);
	}
	if ((aber[0] != 1 || aber[2] != 1) && colors == 3) {
		if (verbose)
			fprintf(stderr, _("Correcting chromatic aberration...\n"));
		for (c = 0; c < 4; c += 2) {
			if (aber[c] == 1) continue;
			img = (ushort *)malloc(size * sizeof *img);
			merror(img, "scale_colors()");
			for (i = 0; i < size; i++)
				img[i] = image[i][c];
			for (row = 0; row < iheight; row++) {
				ur = fr = (row - iheight*0.5) * aber[c] + iheight*0.5;
				if (ur > iheight - 2) continue;
				fr -= ur;
				for (col = 0; col < iwidth; col++) {
					uc = fc = (col - iwidth*0.5) * aber[c] + iwidth*0.5;
					if (uc > iwidth - 2) continue;
					fc -= uc;
					pix = img + ur*iwidth + uc;
					image[row*iwidth + col][c] =
						(pix[0] * (1 - fc) + pix[1] * fc) * (1 - fr) +
						(pix[iwidth] * (1 - fc) + pix[iwidth + 1] * fc) * fr;
				}
			}
			free(img);
		}
	}
}

void CLASS pre_interpolate()
{
	ushort(*img)[4];
	int row, col, c;

	if (shrink) {
		if (half_size) {
			height = iheight;
			width = iwidth;
			if (filters == 9) {
				for (row = 0; row < 3; row++)
					for (col = 1; col < 4; col++)
						if (!(image[row*width + col][0] | image[row*width + col][2]))
							goto break2;  break2:
				for (; row < height; row += 3)
					for (col = (col - 1) % 3 + 1; col < width - 1; col += 3) {
						img = image + row*width + col;
						for (c = 0; c < 3; c += 2)
							img[0][c] = (img[-1][c] + img[1][c]) >> 1;
					}
			}
		} else {
			img = (ushort(*)[4]) calloc(height, width * sizeof *img);
			merror(img, "pre_interpolate()");
			for (row = 0; row < height; row++)
				for (col = 0; col < width; col++) {
					c = fcol(row, col);
					img[row*width + col][c] = image[(row >> 1)*iwidth + (col >> 1)][c];
				}
			free(image);
			image = img;
			shrink = 0;
		}
	}
	if (filters > 1000 && colors == 3) {
		mix_green = four_color_rgb ^ half_size;
		if (four_color_rgb | half_size) colors++;
		else {
			for (row = FC(1, 0) >> 1; row < height; row += 2)
				for (col = FC(row, 1) & 1; col < width; col += 2)
					image[row*width + col][1] = image[row*width + col][3];
			filters &= ~((filters & 0x55555555) << 1);
		}
	}
	if (half_size) filters = 0;
}

void CLASS border_interpolate(int border)
{
	unsigned row, col, y, x, f, c, sum[8];

	for (row = 0; row < height; row++)
		for (col = 0; col < width; col++) {
			if (col == border && row >= border && row < height - border)
				col = width - border;
			memset(sum, 0, sizeof sum);
			for (y = row - 1; y != row + 2; y++)
				for (x = col - 1; x != col + 2; x++)
					if (y < height && x < width) {
						f = fcol(y, x);
						sum[f] += image[y*width + x][f];
						sum[f + 4]++;
					}
			f = fcol(row, col);
			FORCC if (c != f && sum[c + 4])
				image[row*width + col][c] = sum[c] / sum[c + 4];
		}
}

void CLASS lin_interpolate()
{
	int code[16][16][32], size = 16, *ip, sum[4];
	int f, c, i, x, y, row, col, shift, color;
	ushort *pix;

	if (verbose) fprintf(stderr, _("Bilinear interpolation...\n"));
	if (filters == 9) size = 6;
	border_interpolate(1);
	for (row = 0; row < size; row++)
		for (col = 0; col < size; col++) {
			ip = code[row][col] + 1;
			f = fcol(row, col);
			memset(sum, 0, sizeof sum);
			for (y = -1; y <= 1; y++)
				for (x = -1; x <= 1; x++) {
					shift = (y == 0) + (x == 0);
					color = fcol(row + y, col + x);
					if (color == f) continue;
					*ip++ = (width*y + x) * 4 + color;
					*ip++ = shift;
					*ip++ = color;
					sum[color] += 1 << shift;
				}
			code[row][col][0] = (ip - code[row][col]) / 3;
			FORCC
				if (c != f) {
					*ip++ = c;
					*ip++ = 256 / sum[c];
				}
		}
	for (row = 1; row < height - 1; row++)
		for (col = 1; col < width - 1; col++) {
			pix = image[row*width + col];
			ip = code[row % size][col % size];
			memset(sum, 0, sizeof sum);
			for (i = *ip++; i--; ip += 3)
				sum[ip[2]] += pix[ip[0]] << ip[1];
			for (i = colors; --i; ip += 2)
				pix[ip[0]] = sum[ip[0]] * ip[1] >> 8;
		}
}

/*
   This algorithm is officially called:

   "Interpolation using a Threshold-based variable number of gradients"

   described in http://scien.stanford.edu/pages/labsite/1999/psych221/projects/99/tingchen/algodep/vargra.html

   I've extended the basic idea to work with non-Bayer filter arrays.
   Gradients are numbered clockwise from NW=0 to W=7.
 */
void CLASS vng_interpolate()
{
	static const signed char *cp, terms[] = {
	  -2,-2,+0,-1,0,0x01, -2,-2,+0,+0,1,0x01, -2,-1,-1,+0,0,0x01,
	  -2,-1,+0,-1,0,0x02, -2,-1,+0,+0,0,0x03, -2,-1,+0,+1,1,0x01,
	  -2,+0,+0,-1,0,0x06, -2,+0,+0,+0,1,0x02, -2,+0,+0,+1,0,0x03,
	  -2,+1,-1,+0,0,0x04, -2,+1,+0,-1,1,0x04, -2,+1,+0,+0,0,0x06,
	  -2,+1,+0,+1,0,0x02, -2,+2,+0,+0,1,0x04, -2,+2,+0,+1,0,0x04,
	  -1,-2,-1,+0,0,0x80, -1,-2,+0,-1,0,0x01, -1,-2,+1,-1,0,0x01,
	  -1,-2,+1,+0,1,0x01, -1,-1,-1,+1,0,0x88, -1,-1,+1,-2,0,0x40,
	  -1,-1,+1,-1,0,0x22, -1,-1,+1,+0,0,0x33, -1,-1,+1,+1,1,0x11,
	  -1,+0,-1,+2,0,0x08, -1,+0,+0,-1,0,0x44, -1,+0,+0,+1,0,0x11,
	  -1,+0,+1,-2,1,0x40, -1,+0,+1,-1,0,0x66, -1,+0,+1,+0,1,0x22,
	  -1,+0,+1,+1,0,0x33, -1,+0,+1,+2,1,0x10, -1,+1,+1,-1,1,0x44,
	  -1,+1,+1,+0,0,0x66, -1,+1,+1,+1,0,0x22, -1,+1,+1,+2,0,0x10,
	  -1,+2,+0,+1,0,0x04, -1,+2,+1,+0,1,0x04, -1,+2,+1,+1,0,0x04,
	  +0,-2,+0,+0,1,0x80, +0,-1,+0,+1,1,0x88, +0,-1,+1,-2,0,0x40,
	  +0,-1,+1,+0,0,0x11, +0,-1,+2,-2,0,0x40, +0,-1,+2,-1,0,0x20,
	  +0,-1,+2,+0,0,0x30, +0,-1,+2,+1,1,0x10, +0,+0,+0,+2,1,0x08,
	  +0,+0,+2,-2,1,0x40, +0,+0,+2,-1,0,0x60, +0,+0,+2,+0,1,0x20,
	  +0,+0,+2,+1,0,0x30, +0,+0,+2,+2,1,0x10, +0,+1,+1,+0,0,0x44,
	  +0,+1,+1,+2,0,0x10, +0,+1,+2,-1,1,0x40, +0,+1,+2,+0,0,0x60,
	  +0,+1,+2,+1,0,0x20, +0,+1,+2,+2,0,0x10, +1,-2,+1,+0,0,0x80,
	  +1,-1,+1,+1,0,0x88, +1,+0,+1,+2,0,0x08, +1,+0,+2,-1,0,0x40,
	  +1,+0,+2,+1,0,0x10
	}, chood[] = { -1,-1, -1,0, -1,+1, 0,+1, +1,+1, +1,0, +1,-1, 0,-1 };
	ushort(*brow[5])[4], *pix;
	int prow = 8, pcol = 2, *ip, *code[16][16], gval[8], gmin, gmax, sum[4];
	int row, col, x, y, x1, x2, y1, y2, t, weight, grads, color, diag;
	int g, diff, thold, num, c;

	lin_interpolate();
	if (verbose) fprintf(stderr, _("VNG interpolation...\n"));

	if (filters == 1) prow = pcol = 16;
	if (filters == 9) prow = pcol = 6;
	ip = (int *)calloc(prow*pcol, 1280);
	merror(ip, "vng_interpolate()");
	for (row = 0; row < prow; row++)		/* Precalculate for VNG */
		for (col = 0; col < pcol; col++) {
			code[row][col] = ip;
			for (cp = terms, t = 0; t < 64; t++) {
				y1 = *cp++;  x1 = *cp++;
				y2 = *cp++;  x2 = *cp++;
				weight = *cp++;
				grads = *cp++;
				color = fcol(row + y1, col + x1);
				if (fcol(row + y2, col + x2) != color) continue;
				diag = (fcol(row, col + 1) == color && fcol(row + 1, col) == color) ? 2 : 1;
				if (abs(y1 - y2) == diag && abs(x1 - x2) == diag) continue;
				*ip++ = (y1*width + x1) * 4 + color;
				*ip++ = (y2*width + x2) * 4 + color;
				*ip++ = weight;
				for (g = 0; g < 8; g++)
					if (grads & 1 << g) *ip++ = g;
				*ip++ = -1;
			}
			*ip++ = INT_MAX;
			for (cp = chood, g = 0; g < 8; g++) {
				y = *cp++;  x = *cp++;
				*ip++ = (y*width + x) * 4;
				color = fcol(row, col);
				if (fcol(row + y, col + x) != color && fcol(row + y * 2, col + x * 2) == color)
					*ip++ = (y*width + x) * 8 + color;
				else
					*ip++ = 0;
			}
		}
	brow[4] = (ushort(*)[4]) calloc(width * 3, sizeof **brow);
	merror(brow[4], "vng_interpolate()");
	for (row = 0; row < 3; row++)
		brow[row] = brow[4] + row*width;
	for (row = 2; row < height - 2; row++) {		/* Do VNG interpolation */
		for (col = 2; col < width - 2; col++) {
			pix = image[row*width + col];
			ip = code[row % prow][col % pcol];
			memset(gval, 0, sizeof gval);
			while ((g = ip[0]) != INT_MAX) {		/* Calculate gradients */
				diff = ABS(pix[g] - pix[ip[1]]) << ip[2];
				gval[ip[3]] += diff;
				ip += 5;
				if ((g = ip[-1]) == -1) continue;
				gval[g] += diff;
				while ((g = *ip++) != -1)
					gval[g] += diff;
			}
			ip++;
			gmin = gmax = gval[0];			/* Choose a threshold */
			for (g = 1; g < 8; g++) {
				if (gmin > gval[g]) gmin = gval[g];
				if (gmax < gval[g]) gmax = gval[g];
			}
			if (gmax == 0) {
				memcpy(brow[2][col], pix, sizeof *image);
				continue;
			}
			thold = gmin + (gmax >> 1);
			memset(sum, 0, sizeof sum);
			color = fcol(row, col);
			for (num = g = 0; g < 8; g++, ip += 2) {		/* Average the neighbors */
				if (gval[g] <= thold) {
					FORCC
						if (c == color && ip[1])
							sum[c] += (pix[c] + pix[ip[1]]) >> 1;
						else
							sum[c] += pix[ip[0] + c];
					num++;
				}
			}
			FORCC{					/* Save to buffer */
		  t = pix[color];
		  if (c != color)
			t += (sum[c] - sum[color]) / num;
		  brow[2][col][c] = CLIP(t);
			}
		}
		if (row > 3)				/* Write buffer to image */
			memcpy(image[(row - 2)*width + 2], brow[0] + 2, (width - 4) * sizeof *image);
		for (g = 0; g < 4; g++)
			brow[(g - 1) & 3] = brow[g];
	}
	memcpy(image[(row - 2)*width + 2], brow[0] + 2, (width - 4) * sizeof *image);
	memcpy(image[(row - 1)*width + 2], brow[1] + 2, (width - 4) * sizeof *image);
	free(brow[4]);
	free(code[0][0]);
}

/*
   Patterned Pixel Grouping Interpolation by Alain Desbiolles
*/
void CLASS ppg_interpolate()
{
	int dir[5] = { 1, width, -1, -width, 1 };
	int row, col, diff[2], guess[2], c, d, i;
	ushort(*pix)[4];

	border_interpolate(3);
	if (verbose) fprintf(stderr, _("PPG interpolation...\n"));

	/*  Fill in the green layer with gradients and pattern recognition: */
	for (row = 3; row < height - 3; row++)
		for (col = 3 + (FC(row, 3) & 1), c = FC(row, col); col < width - 3; col += 2) {
			pix = image + row*width + col;
			for (i = 0; (d = dir[i]) > 0; i++) {
				guess[i] = (pix[-d][1] + pix[0][c] + pix[d][1]) * 2
					- pix[-2 * d][c] - pix[2 * d][c];
				diff[i] = (ABS(pix[-2 * d][c] - pix[0][c]) +
					ABS(pix[2 * d][c] - pix[0][c]) +
					ABS(pix[-d][1] - pix[d][1])) * 3 +
					(ABS(pix[3 * d][1] - pix[d][1]) +
						ABS(pix[-3 * d][1] - pix[-d][1])) * 2;
			}
			d = dir[i = diff[0] > diff[1]];
			pix[0][1] = ULIM(guess[i] >> 2, pix[d][1], pix[-d][1]);
		}
	/*  Calculate red and blue for each green pixel:		*/
	for (row = 1; row < height - 1; row++)
		for (col = 1 + (FC(row, 2) & 1), c = FC(row, col + 1); col < width - 1; col += 2) {
			pix = image + row*width + col;
			for (i = 0; (d = dir[i]) > 0; c = 2 - c, i++)
				pix[0][c] = CLIP((pix[-d][c] + pix[d][c] + 2 * pix[0][1]
					- pix[-d][1] - pix[d][1]) >> 1);
		}
	/*  Calculate blue for red pixels and vice versa:		*/
	for (row = 1; row < height - 1; row++)
		for (col = 1 + (FC(row, 1) & 1), c = 2 - FC(row, col); col < width - 1; col += 2) {
			pix = image + row*width + col;
			for (i = 0; (d = dir[i] + dir[i + 1]) > 0; i++) {
				diff[i] = ABS(pix[-d][c] - pix[d][c]) +
					ABS(pix[-d][1] - pix[0][1]) +
					ABS(pix[d][1] - pix[0][1]);
				guess[i] = pix[-d][c] + pix[d][c] + 2 * pix[0][1]
					- pix[-d][1] - pix[d][1];
			}
			if (diff[0] != diff[1])
				pix[0][c] = CLIP(guess[diff[0] > diff[1]] >> 1);
			else
				pix[0][c] = CLIP((guess[0] + guess[1]) >> 2);
		}
}

void CLASS cielab(ushort rgb[3], short lab[3])
{
	int c, i, j, k;
	float r, xyz[3];
	static float cbrt[0x10000], xyz_cam[3][4];

	if (!rgb) {
		for (i = 0; i < 0x10000; i++) {
			r = i / 65535.0;
			cbrt[i] = r > 0.008856 ? pow(r, 1 / 3.0) : 7.787*r + 16 / 116.0;
		}
		for (i = 0; i < 3; i++)
			for (j = 0; j < colors; j++)
				for (xyz_cam[i][j] = k = 0; k < 3; k++)
					xyz_cam[i][j] += xyz_rgb[i][k] * rgb_cam[k][j] / d65_white[i];
		return;
	}
	xyz[0] = xyz[1] = xyz[2] = 0.5;
	FORCC{
	  xyz[0] += xyz_cam[0][c] * rgb[c];
	  xyz[1] += xyz_cam[1][c] * rgb[c];
	  xyz[2] += xyz_cam[2][c] * rgb[c];
	}
	xyz[0] = cbrt[CLIP((int)xyz[0])];
	xyz[1] = cbrt[CLIP((int)xyz[1])];
	xyz[2] = cbrt[CLIP((int)xyz[2])];
	lab[0] = 64 * (116 * xyz[1] - 16);
	lab[1] = 64 * 500 * (xyz[0] - xyz[1]);
	lab[2] = 64 * 200 * (xyz[1] - xyz[2]);
}

#define TS 512		/* Tile Size */
#define fcol(row,col) xtrans[(row+6) % 6][(col+6) % 6]

/*
   Frank Markesteijn's algorithm for Fuji X-Trans sensors
 */
void CLASS xtrans_interpolate(int passes)
{
	int c, d, f, g, h, i, v, ng, row, col, top, left, mrow, mcol;
	int val, ndir, pass, hm[8], avg[4], color[3][8];
	static const short orth[12] = { 1,0,0,1,-1,0,0,-1,1,0,0,1 },
		patt[2][16] = { { 0,1,0,-1,2,0,-1,0,1,1,1,-1,0,0,0,0 },
				{ 0,1,0,-2,1,0,-2,0,1,1,-2,-2,1,-1,-1,1 } },
		dir[4] = { 1,TS,TS + 1,TS - 1 };
	short allhex[3][3][2][8], *hex;
	ushort min, max, sgrow, sgcol;
	ushort(*rgb)[TS][TS][3], (*rix)[3], (*pix)[4];
	short(*lab)[TS][3], (*lix)[3];
	float(*drv)[TS][TS], diff[6], tr;
	char(*homo)[TS][TS], *buffer;

	if (verbose)
		fprintf(stderr, _("%d-pass X-Trans interpolation...\n"), passes);

	cielab(0, 0);
	ndir = 4 << (passes > 1);
	buffer = (char *)malloc(TS*TS*(ndir * 11 + 6));
	merror(buffer, "xtrans_interpolate()");
	rgb = (ushort(*)[TS][TS][3]) buffer;
	lab = (short(*)[TS][3])(buffer + TS*TS*(ndir * 6));
	drv = (float(*)[TS][TS])   (buffer + TS*TS*(ndir * 6 + 6));
	homo = (char(*)[TS][TS])   (buffer + TS*TS*(ndir * 10 + 6));

	/* Map a green hexagon around each non-green pixel and vice versa:	*/
	for (row = 0; row < 3; row++)
		for (col = 0; col < 3; col++)
			for (ng = d = 0; d < 10; d += 2) {
				g = fcol(row, col) == 1;
				if (fcol(row + orth[d], col + orth[d + 2]) == 1) ng = 0; else ng++;
				if (ng == 4) { sgrow = row; sgcol = col; }
				if (ng == g + 1) FORC(8) {
					v = orth[d] * patt[g][c * 2] + orth[d + 1] * patt[g][c * 2 + 1];
					h = orth[d + 2] * patt[g][c * 2] + orth[d + 3] * patt[g][c * 2 + 1];
					allhex[row][col][0][c ^ (g * 2 & d)] = h + v*width;
					allhex[row][col][1][c ^ (g * 2 & d)] = h + v*TS;
				}
			}

	/* Set green1 and green3 to the minimum and maximum allowed values:	*/
	for (row = 2; row < height - 2; row++)
		for (min = ~(max = 0), col = 2; col < width - 2; col++) {
			if (fcol(row, col) == 1 && (min = ~(max = 0))) continue;
			pix = image + row*width + col;
			hex = allhex[row % 3][col % 3][0];
			if (!max) FORC(6) {
				val = pix[hex[c]][1];
				if (min > val) min = val;
				if (max < val) max = val;
			}
			pix[0][1] = min;
			pix[0][3] = max;
			switch ((row - sgrow) % 3) {
			case 1: if (row < height - 3) { row++; col--; } break;
			case 2: if ((min = ~(max = 0)) && (col += 2) < width - 3 && row > 2) row--;
			}
		}

	for (top = 3; top < height - 19; top += TS - 16)
		for (left = 3; left < width - 19; left += TS - 16) {
			mrow = MIN(top + TS, height - 3);
			mcol = MIN(left + TS, width - 3);
			for (row = top; row < mrow; row++)
				for (col = left; col < mcol; col++)
					memcpy(rgb[0][row - top][col - left], image[row*width + col], 6);
			FORC3 memcpy(rgb[c + 1], rgb[0], sizeof *rgb);

			/* Interpolate green horizontally, vertically, and along both diagonals: */
			for (row = top; row < mrow; row++)
				for (col = left; col < mcol; col++) {
					if ((f = fcol(row, col)) == 1) continue;
					pix = image + row*width + col;
					hex = allhex[row % 3][col % 3][0];
					color[1][0] = 174 * (pix[hex[1]][1] + pix[hex[0]][1]) -
						46 * (pix[2 * hex[1]][1] + pix[2 * hex[0]][1]);
					color[1][1] = 223 * pix[hex[3]][1] + pix[hex[2]][1] * 33 +
						92 * (pix[0][f] - pix[-hex[2]][f]);
					FORC(2) color[1][2 + c] =
						164 * pix[hex[4 + c]][1] + 92 * pix[-2 * hex[4 + c]][1] + 33 *
						(2 * pix[0][f] - pix[3 * hex[4 + c]][f] - pix[-3 * hex[4 + c]][f]);
					FORC4 rgb[c ^ !((row - sgrow) % 3)][row - top][col - left][1] =
						LIM(color[1][c] >> 8, pix[0][1], pix[0][3]);
				}

			for (pass = 0; pass < passes; pass++) {
				if (pass == 1)
					memcpy(rgb += 4, buffer, 4 * sizeof *rgb);

				/* Recalculate green from interpolated values of closer pixels:	*/
				if (pass) {
					for (row = top + 2; row < mrow - 2; row++)
						for (col = left + 2; col < mcol - 2; col++) {
							if ((f = fcol(row, col)) == 1) continue;
							pix = image + row*width + col;
							hex = allhex[row % 3][col % 3][1];
							for (d = 3; d < 6; d++) {
								rix = &rgb[(d - 2) ^ !((row - sgrow) % 3)][row - top][col - left];
								val = rix[-2 * hex[d]][1] + 2 * rix[hex[d]][1]
									- rix[-2 * hex[d]][f] - 2 * rix[hex[d]][f] + 3 * rix[0][f];
								rix[0][1] = LIM(val / 3, pix[0][1], pix[0][3]);
							}
						}
				}

				/* Interpolate red and blue values for solitary green pixels:	*/
				for (row = (top - sgrow + 4) / 3 * 3 + sgrow; row < mrow - 2; row += 3)
					for (col = (left - sgcol + 4) / 3 * 3 + sgcol; col < mcol - 2; col += 3) {
						rix = &rgb[0][row - top][col - left];
						h = fcol(row, col + 1);
						memset(diff, 0, sizeof diff);
						for (i = 1, d = 0; d < 6; d++, i ^= TS ^ 1, h ^= 2) {
							for (c = 0; c < 2; c++, h ^= 2) {
								g = 2 * rix[0][1] - rix[i << c][1] - rix[-i << c][1];
								color[h][d] = g + rix[i << c][h] + rix[-i << c][h];
								if (d > 1)
									diff[d] += SQR(rix[i << c][1] - rix[-i << c][1]
										- rix[i << c][h] + rix[-i << c][h]) + SQR(g);
							}
							if (d > 1 && (d & 1))
								if (diff[d - 1] < diff[d])
									FORC(2) color[c * 2][d] = color[c * 2][d - 1];
							if (d < 2 || (d & 1)) {
								FORC(2) rix[0][c * 2] = CLIP(color[c * 2][d] / 2);
								rix += TS*TS;
							}
						}
					}

				/* Interpolate red for blue pixels and vice versa:		*/
				for (row = top + 3; row < mrow - 3; row++)
					for (col = left + 3; col < mcol - 3; col++) {
						if ((f = 2 - fcol(row, col)) == 1) continue;
						rix = &rgb[0][row - top][col - left];
						c = (row - sgrow) % 3 ? TS : 1;
						h = 3 * (c ^ TS ^ 1);
						for (d = 0; d < 4; d++, rix += TS*TS) {
							i = d > 1 || ((d ^ c) & 1) ||
								((ABS(rix[0][1] - rix[c][1]) + ABS(rix[0][1] - rix[-c][1])) <
									2 * (ABS(rix[0][1] - rix[h][1]) + ABS(rix[0][1] - rix[-h][1]))) ? c : h;
							rix[0][f] = CLIP((rix[i][f] + rix[-i][f] +
								2 * rix[0][1] - rix[i][1] - rix[-i][1]) / 2);
						}
					}

				/* Fill in red and blue for 2x2 blocks of green:		*/
				for (row = top + 2; row < mrow - 2; row++) if ((row - sgrow) % 3)
					for (col = left + 2; col < mcol - 2; col++) if ((col - sgcol) % 3) {
						rix = &rgb[0][row - top][col - left];
						hex = allhex[row % 3][col % 3][1];
						for (d = 0; d < ndir; d += 2, rix += TS*TS)
							if (hex[d] + hex[d + 1]) {
								g = 3 * rix[0][1] - 2 * rix[hex[d]][1] - rix[hex[d + 1]][1];
								for (c = 0; c < 4; c += 2) rix[0][c] =
									CLIP((g + 2 * rix[hex[d]][c] + rix[hex[d + 1]][c]) / 3);
							} else {
								g = 2 * rix[0][1] - rix[hex[d]][1] - rix[hex[d + 1]][1];
								for (c = 0; c < 4; c += 2) rix[0][c] =
									CLIP((g + rix[hex[d]][c] + rix[hex[d + 1]][c]) / 2);
							}
					}
			}
			rgb = (ushort(*)[TS][TS][3]) buffer;
			mrow -= top;
			mcol -= left;

			/* Convert to CIELab and differentiate in all directions:	*/
			for (d = 0; d < ndir; d++) {
				for (row = 2; row < mrow - 2; row++)
					for (col = 2; col < mcol - 2; col++)
						cielab(rgb[d][row][col], lab[row][col]);
				for (f = dir[d & 3], row = 3; row < mrow - 3; row++)
					for (col = 3; col < mcol - 3; col++) {
						lix = &lab[row][col];
						g = 2 * lix[0][0] - lix[f][0] - lix[-f][0];
						drv[d][row][col] = SQR(g)
							+ SQR((2 * lix[0][1] - lix[f][1] - lix[-f][1] + g * 500 / 232))
							+ SQR((2 * lix[0][2] - lix[f][2] - lix[-f][2] - g * 500 / 580));
					}
			}

			/* Build homogeneity maps from the derivatives:			*/
			memset(homo, 0, ndir*TS*TS);
			for (row = 4; row < mrow - 4; row++)
				for (col = 4; col < mcol - 4; col++) {
					for (tr = FLT_MAX, d = 0; d < ndir; d++)
						if (tr > drv[d][row][col])
							tr = drv[d][row][col];
					tr *= 8;
					for (d = 0; d < ndir; d++)
						for (v = -1; v <= 1; v++)
							for (h = -1; h <= 1; h++)
								if (drv[d][row + v][col + h] <= tr)
									homo[d][row][col]++;
				}

			/* Average the most homogenous pixels for the final result:	*/
			if (height - top < TS + 4) mrow = height - top + 2;
			if (width - left < TS + 4) mcol = width - left + 2;
			for (row = MIN(top, 8); row < mrow - 8; row++)
				for (col = MIN(left, 8); col < mcol - 8; col++) {
					for (d = 0; d < ndir; d++)
						for (hm[d] = 0, v = -2; v <= 2; v++)
							for (h = -2; h <= 2; h++)
								hm[d] += homo[d][row + v][col + h];
					for (d = 0; d < ndir - 4; d++)
						if (hm[d] < hm[d + 4]) hm[d] = 0; else
							if (hm[d] > hm[d + 4]) hm[d + 4] = 0;
					for (max = hm[0], d = 1; d < ndir; d++)
						if (max < hm[d]) max = hm[d];
					max -= max >> 3;
					memset(avg, 0, sizeof avg);
					for (d = 0; d < ndir; d++)
						if (hm[d] >= max) {
							FORC3 avg[c] += rgb[d][row][col][c];
							avg[3]++;
						}
					FORC3 image[(row + top)*width + col + left][c] = avg[c] / avg[3];
				}
		}
	free(buffer);
	border_interpolate(8);
}
#undef fcol

/*
   Adaptive Homogeneity-Directed interpolation is based on
   the work of Keigo Hirakawa, Thomas Parks, and Paul Lee.
 */
void CLASS ahd_interpolate()
{
	int i, j, top, left, row, col, tr, tc, c, d, val, hm[2];
	static const int dir[4] = { -1, 1, -TS, TS };
	unsigned ldiff[2][4], abdiff[2][4], leps, abeps;
	ushort(*rgb)[TS][TS][3], (*rix)[3], (*pix)[4];
	short(*lab)[TS][TS][3], (*lix)[3];
	char(*homo)[TS][TS], *buffer;

	if (verbose) fprintf(stderr, _("AHD interpolation...\n"));

	cielab(0, 0);
	border_interpolate(5);
	buffer = (char *)malloc(26 * TS*TS);
	merror(buffer, "ahd_interpolate()");
	rgb = (ushort(*)[TS][TS][3]) buffer;
	lab = (short(*)[TS][TS][3])(buffer + 12 * TS*TS);
	homo = (char(*)[TS][TS])   (buffer + 24 * TS*TS);

	for (top = 2; top < height - 5; top += TS - 6)
		for (left = 2; left < width - 5; left += TS - 6) {

			/*  Interpolate green horizontally and vertically:		*/
			for (row = top; row < top + TS && row < height - 2; row++) {
				col = left + (FC(row, left) & 1);
				for (c = FC(row, col); col < left + TS && col < width - 2; col += 2) {
					pix = image + row*width + col;
					val = ((pix[-1][1] + pix[0][c] + pix[1][1]) * 2
						- pix[-2][c] - pix[2][c]) >> 2;
					rgb[0][row - top][col - left][1] = ULIM(val, pix[-1][1], pix[1][1]);
					val = ((pix[-width][1] + pix[0][c] + pix[width][1]) * 2
						- pix[-2 * width][c] - pix[2 * width][c]) >> 2;
					rgb[1][row - top][col - left][1] = ULIM(val, pix[-width][1], pix[width][1]);
				}
			}
			/*  Interpolate red and blue, and convert to CIELab:		*/
			for (d = 0; d < 2; d++)
				for (row = top + 1; row < top + TS - 1 && row < height - 3; row++)
					for (col = left + 1; col < left + TS - 1 && col < width - 3; col++) {
						pix = image + row*width + col;
						rix = &rgb[d][row - top][col - left];
						lix = &lab[d][row - top][col - left];
						if ((c = 2 - FC(row, col)) == 1) {
							c = FC(row + 1, col);
							val = pix[0][1] + ((pix[-1][2 - c] + pix[1][2 - c]
								- rix[-1][1] - rix[1][1]) >> 1);
							rix[0][2 - c] = CLIP(val);
							val = pix[0][1] + ((pix[-width][c] + pix[width][c]
								- rix[-TS][1] - rix[TS][1]) >> 1);
						} else
							val = rix[0][1] + ((pix[-width - 1][c] + pix[-width + 1][c]
								+ pix[+width - 1][c] + pix[+width + 1][c]
								- rix[-TS - 1][1] - rix[-TS + 1][1]
								- rix[+TS - 1][1] - rix[+TS + 1][1] + 1) >> 2);
						rix[0][c] = CLIP(val);
						c = FC(row, col);
						rix[0][c] = pix[0][c];
						cielab(rix[0], lix[0]);
					}
			/*  Build homogeneity maps from the CIELab images:		*/
			memset(homo, 0, 2 * TS*TS);
			for (row = top + 2; row < top + TS - 2 && row < height - 4; row++) {
				tr = row - top;
				for (col = left + 2; col < left + TS - 2 && col < width - 4; col++) {
					tc = col - left;
					for (d = 0; d < 2; d++) {
						lix = &lab[d][tr][tc];
						for (i = 0; i < 4; i++) {
							ldiff[d][i] = ABS(lix[0][0] - lix[dir[i]][0]);
							abdiff[d][i] = SQR(lix[0][1] - lix[dir[i]][1])
								+ SQR(lix[0][2] - lix[dir[i]][2]);
						}
					}
					leps = MIN(MAX(ldiff[0][0], ldiff[0][1]),
						MAX(ldiff[1][2], ldiff[1][3]));
					abeps = MIN(MAX(abdiff[0][0], abdiff[0][1]),
						MAX(abdiff[1][2], abdiff[1][3]));
					for (d = 0; d < 2; d++)
						for (i = 0; i < 4; i++)
							if (ldiff[d][i] <= leps && abdiff[d][i] <= abeps)
								homo[d][tr][tc]++;
				}
			}
			/*  Combine the most homogenous pixels for the final result:	*/
			for (row = top + 3; row < top + TS - 3 && row < height - 5; row++) {
				tr = row - top;
				for (col = left + 3; col < left + TS - 3 && col < width - 5; col++) {
					tc = col - left;
					for (d = 0; d < 2; d++)
						for (hm[d] = 0, i = tr - 1; i <= tr + 1; i++)
							for (j = tc - 1; j <= tc + 1; j++)
								hm[d] += homo[d][i][j];
					if (hm[0] != hm[1])
						FORC3 image[row*width + col][c] = rgb[hm[1] > hm[0]][tr][tc][c];
					else
						FORC3 image[row*width + col][c] =
						(rgb[0][tr][tc][c] + rgb[1][tr][tc][c]) >> 1;
				}
			}
		}
	free(buffer);
}
#undef TS

void CLASS median_filter()
{
	ushort(*pix)[4];
	int pass, c, i, j, k, med[9];
	static const uchar opt[] =	/* Optimal 9-element median search */
	{ 1,2, 4,5, 7,8, 0,1, 3,4, 6,7, 1,2, 4,5, 7,8,
	  0,3, 5,8, 4,7, 3,6, 1,4, 2,5, 4,7, 4,2, 6,4, 4,2 };

	for (pass = 1; pass <= med_passes; pass++) {
		if (verbose)
			fprintf(stderr, _("Median filter pass %d...\n"), pass);
		for (c = 0; c < 3; c += 2) {
			for (pix = image; pix < image + width*height; pix++)
				pix[0][3] = pix[0][c];
			for (pix = image + width; pix < image + width*(height - 1); pix++) {
				if ((pix - image + 1) % width < 2) continue;
				for (k = 0, i = -width; i <= width; i += width)
					for (j = i - 1; j <= i + 1; j++)
						med[k++] = pix[j][3] - pix[j][1];
				for (i = 0; i < sizeof opt; i += 2)
					if (med[opt[i]] > med[opt[i + 1]])
						SWAP(med[opt[i]], med[opt[i + 1]]);
				pix[0][c] = CLIP(med[4] + pix[0][1]);
			}
		}
	}
}

void CLASS blend_highlights()
{
	int clip = INT_MAX, row, col, c, i, j;
	static const float trans[2][4][4] =
	{ { { 1,1,1 }, { 1.7320508,-1.7320508,0 }, { -1,-1,2 } },
	  { { 1,1,1,1 }, { 1,-1,1,-1 }, { 1,1,-1,-1 }, { 1,-1,-1,1 } } };
	static const float itrans[2][4][4] =
	{ { { 1,0.8660254,-0.5 }, { 1,-0.8660254,-0.5 }, { 1,0,1 } },
	  { { 1,1,1,1 }, { 1,-1,1,-1 }, { 1,1,-1,-1 }, { 1,-1,-1,1 } } };
	float cam[2][4], lab[2][4], sum[2], chratio;

	if ((unsigned)(colors - 3) > 1) return;
	if (verbose) fprintf(stderr, _("Blending highlights...\n"));
	FORCC if (clip > (i = 65535 * pre_mul[c])) clip = i;
	for (row = 0; row < height; row++)
		for (col = 0; col < width; col++) {
			FORCC if (image[row*width + col][c] > clip) break;
			if (c == colors) continue;
			FORCC{
		  cam[0][c] = image[row*width + col][c];
		  cam[1][c] = MIN(cam[0][c],clip);
			}
			for (i = 0; i < 2; i++) {
				FORCC for (lab[i][c] = j = 0; j < colors; j++)
					lab[i][c] += trans[colors - 3][c][j] * cam[i][j];
				for (sum[i] = 0, c = 1; c < colors; c++)
					sum[i] += SQR(lab[i][c]);
			}
			chratio = sqrt(sum[1] / sum[0]);
			for (c = 1; c < colors; c++)
				lab[0][c] *= chratio;
			FORCC for (cam[0][c] = j = 0; j < colors; j++)
				cam[0][c] += itrans[colors - 3][c][j] * lab[0][j];
			FORCC image[row*width + col][c] = cam[0][c] / colors;
		}
}

#define SCALE (4 >> shrink)
void CLASS recover_highlights()
{
	float *map, sum, wgt, grow;
	int hsat[4], count, spread, change, val, i;
	unsigned high, wide, mrow, mcol, row, col, kc, c, d, y, x;
	ushort *pixel;
	static const signed char dir[8][2] =
	{ {-1,-1}, {-1,0}, {-1,1}, {0,1}, {1,1}, {1,0}, {1,-1}, {0,-1} };

	if (verbose) fprintf(stderr, _("Rebuilding highlights...\n"));

	grow = pow(2, 4 - highlight);
	FORCC hsat[c] = 32000 * pre_mul[c];
	for (kc = 0, c = 1; c < colors; c++)
		if (pre_mul[kc] < pre_mul[c]) kc = c;
	high = height / SCALE;
	wide = width / SCALE;
	map = (float *)calloc(high, wide * sizeof *map);
	merror(map, "recover_highlights()");
	FORCC if (c != kc) {
		memset(map, 0, high*wide * sizeof *map);
		for (mrow = 0; mrow < high; mrow++)
			for (mcol = 0; mcol < wide; mcol++) {
				sum = wgt = count = 0;
				for (row = mrow*SCALE; row < (mrow + 1)*SCALE; row++)
					for (col = mcol*SCALE; col < (mcol + 1)*SCALE; col++) {
						pixel = image[row*width + col];
						if (pixel[c] / hsat[c] == 1 && pixel[kc] > 24000) {
							sum += pixel[c];
							wgt += pixel[kc];
							count++;
						}
					}
				if (count == SCALE*SCALE)
					map[mrow*wide + mcol] = sum / wgt;
			}
		for (spread = 32 / grow; spread--; ) {
			for (mrow = 0; mrow < high; mrow++)
				for (mcol = 0; mcol < wide; mcol++) {
					if (map[mrow*wide + mcol]) continue;
					sum = count = 0;
					for (d = 0; d < 8; d++) {
						y = mrow + dir[d][0];
						x = mcol + dir[d][1];
						if (y < high && x < wide && map[y*wide + x] > 0) {
							sum += (1 + (d & 1)) * map[y*wide + x];
							count += 1 + (d & 1);
						}
					}
					if (count > 3)
						map[mrow*wide + mcol] = -(sum + grow) / (count + grow);
				}
			for (change = i = 0; i < high*wide; i++)
				if (map[i] < 0) {
					map[i] = -map[i];
					change = 1;
				}
			if (!change) break;
		}
		for (i = 0; i < high*wide; i++)
			if (map[i] == 0) map[i] = 1;
		for (mrow = 0; mrow < high; mrow++)
			for (mcol = 0; mcol < wide; mcol++) {
				for (row = mrow*SCALE; row < (mrow + 1)*SCALE; row++)
					for (col = mcol*SCALE; col < (mcol + 1)*SCALE; col++) {
						pixel = image[row*width + col];
						if (pixel[c] / hsat[c] > 1) {
							val = pixel[kc] * map[mrow*wide + mcol];
							if (pixel[c] < val) pixel[c] = CLIP(val);
						}
					}
			}
	}
	free(map);
}
#undef SCALE

void CLASS tiff_get(unsigned base,
	unsigned *tag, unsigned *type, unsigned *len, unsigned *save)
{
	*tag = get2();
	*type = get2();
	*len = get4();
	*save = ftell(ifp) + 4;
	if (*len * ("11124811248484"[*type < 14 ? *type : 0] - '0') > 4)
		fseek(ifp, get4() + base, SEEK_SET);
}

void CLASS parse_thumb_note(int base, unsigned toff, unsigned tlen)
{
	unsigned entries, tag, type, len, save;

	entries = get2();
	while (entries--) {
		tiff_get(base, &tag, &type, &len, &save);
		if (tag == toff) thumb_offset = get4() + base;
		if (tag == tlen) thumb_length = get4();
		fseek(ifp, save, SEEK_SET);
	}
}

int CLASS parse_tiff_ifd(int base);

void CLASS parse_makernote(int base, int uptag)
{
	static const uchar xlat[2][256] = {
	{ 0xc1,0xbf,0x6d,0x0d,0x59,0xc5,0x13,0x9d,0x83,0x61,0x6b,0x4f,0xc7,0x7f,0x3d,0x3d,
	  0x53,0x59,0xe3,0xc7,0xe9,0x2f,0x95,0xa7,0x95,0x1f,0xdf,0x7f,0x2b,0x29,0xc7,0x0d,
	  0xdf,0x07,0xef,0x71,0x89,0x3d,0x13,0x3d,0x3b,0x13,0xfb,0x0d,0x89,0xc1,0x65,0x1f,
	  0xb3,0x0d,0x6b,0x29,0xe3,0xfb,0xef,0xa3,0x6b,0x47,0x7f,0x95,0x35,0xa7,0x47,0x4f,
	  0xc7,0xf1,0x59,0x95,0x35,0x11,0x29,0x61,0xf1,0x3d,0xb3,0x2b,0x0d,0x43,0x89,0xc1,
	  0x9d,0x9d,0x89,0x65,0xf1,0xe9,0xdf,0xbf,0x3d,0x7f,0x53,0x97,0xe5,0xe9,0x95,0x17,
	  0x1d,0x3d,0x8b,0xfb,0xc7,0xe3,0x67,0xa7,0x07,0xf1,0x71,0xa7,0x53,0xb5,0x29,0x89,
	  0xe5,0x2b,0xa7,0x17,0x29,0xe9,0x4f,0xc5,0x65,0x6d,0x6b,0xef,0x0d,0x89,0x49,0x2f,
	  0xb3,0x43,0x53,0x65,0x1d,0x49,0xa3,0x13,0x89,0x59,0xef,0x6b,0xef,0x65,0x1d,0x0b,
	  0x59,0x13,0xe3,0x4f,0x9d,0xb3,0x29,0x43,0x2b,0x07,0x1d,0x95,0x59,0x59,0x47,0xfb,
	  0xe5,0xe9,0x61,0x47,0x2f,0x35,0x7f,0x17,0x7f,0xef,0x7f,0x95,0x95,0x71,0xd3,0xa3,
	  0x0b,0x71,0xa3,0xad,0x0b,0x3b,0xb5,0xfb,0xa3,0xbf,0x4f,0x83,0x1d,0xad,0xe9,0x2f,
	  0x71,0x65,0xa3,0xe5,0x07,0x35,0x3d,0x0d,0xb5,0xe9,0xe5,0x47,0x3b,0x9d,0xef,0x35,
	  0xa3,0xbf,0xb3,0xdf,0x53,0xd3,0x97,0x53,0x49,0x71,0x07,0x35,0x61,0x71,0x2f,0x43,
	  0x2f,0x11,0xdf,0x17,0x97,0xfb,0x95,0x3b,0x7f,0x6b,0xd3,0x25,0xbf,0xad,0xc7,0xc5,
	  0xc5,0xb5,0x8b,0xef,0x2f,0xd3,0x07,0x6b,0x25,0x49,0x95,0x25,0x49,0x6d,0x71,0xc7 },
	{ 0xa7,0xbc,0xc9,0xad,0x91,0xdf,0x85,0xe5,0xd4,0x78,0xd5,0x17,0x46,0x7c,0x29,0x4c,
	  0x4d,0x03,0xe9,0x25,0x68,0x11,0x86,0xb3,0xbd,0xf7,0x6f,0x61,0x22,0xa2,0x26,0x34,
	  0x2a,0xbe,0x1e,0x46,0x14,0x68,0x9d,0x44,0x18,0xc2,0x40,0xf4,0x7e,0x5f,0x1b,0xad,
	  0x0b,0x94,0xb6,0x67,0xb4,0x0b,0xe1,0xea,0x95,0x9c,0x66,0xdc,0xe7,0x5d,0x6c,0x05,
	  0xda,0xd5,0xdf,0x7a,0xef,0xf6,0xdb,0x1f,0x82,0x4c,0xc0,0x68,0x47,0xa1,0xbd,0xee,
	  0x39,0x50,0x56,0x4a,0xdd,0xdf,0xa5,0xf8,0xc6,0xda,0xca,0x90,0xca,0x01,0x42,0x9d,
	  0x8b,0x0c,0x73,0x43,0x75,0x05,0x94,0xde,0x24,0xb3,0x80,0x34,0xe5,0x2c,0xdc,0x9b,
	  0x3f,0xca,0x33,0x45,0xd0,0xdb,0x5f,0xf5,0x52,0xc3,0x21,0xda,0xe2,0x22,0x72,0x6b,
	  0x3e,0xd0,0x5b,0xa8,0x87,0x8c,0x06,0x5d,0x0f,0xdd,0x09,0x19,0x93,0xd0,0xb9,0xfc,
	  0x8b,0x0f,0x84,0x60,0x33,0x1c,0x9b,0x45,0xf1,0xf0,0xa3,0x94,0x3a,0x12,0x77,0x33,
	  0x4d,0x44,0x78,0x28,0x3c,0x9e,0xfd,0x65,0x57,0x16,0x94,0x6b,0xfb,0x59,0xd0,0xc8,
	  0x22,0x36,0xdb,0xd2,0x63,0x98,0x43,0xa1,0x04,0x87,0x86,0xf7,0xa6,0x26,0xbb,0xd6,
	  0x59,0x4d,0xbf,0x6a,0x2e,0xaa,0x2b,0xef,0xe6,0x78,0xb6,0x4e,0xe0,0x2f,0xdc,0x7c,
	  0xbe,0x57,0x19,0x32,0x7e,0x2a,0xd0,0xb8,0xba,0x29,0x00,0x3c,0x52,0x7d,0xa8,0x49,
	  0x3b,0x2d,0xeb,0x25,0x49,0xfa,0xa3,0xaa,0x39,0xa7,0xc5,0xa7,0x50,0x11,0x36,0xfb,
	  0xc6,0x67,0x4a,0xf5,0xa5,0x12,0x65,0x7e,0xb0,0xdf,0xaf,0x4e,0xb3,0x61,0x7f,0x2f } };
	unsigned offset = 0, entries, tag, type, len, save, c;
	unsigned ver97 = 0, serial = 0, i, wbi = 0, wb[4] = { 0,0,0,0 };
	uchar buf97[324], ci, cj, ck;
	short morder, sorder = order;
	char buf[10];
	/*
	   The MakerNote might have its own TIFF header (possibly with
	   its own byte-order!), or it might just be a table.
	 */
	fread(buf, 1, 10, ifp);
	if (!strncmp(buf, "KDK", 3) ||	/* these aren't TIFF tables */
		!strncmp(buf, "VER", 3) ||
		!strncmp(buf, "IIII", 4) ||
		!strncmp(buf, "MMMM", 4)) return;
	if (!strcmp(buf, "OLYMPUS") ||
		!strcmp(buf, "PENTAX ")) {
		base = ftell(ifp) - 10;
		fseek(ifp, -2, SEEK_CUR);
		order = get2();
		if (buf[0] == 'O') get2();
	} else if (!strncmp(buf, "FUJIFILM", 8)) {
		base = ftell(ifp) - 10;
	nf: order = 0x4949;
		fseek(ifp, 2, SEEK_CUR);
	} else if (!strcmp(buf, "OLYMP") ||
		!strcmp(buf, "Ricoh"))
		fseek(ifp, -2, SEEK_CUR);
	else if (!strcmp(buf, "AOC") ||
		!strcmp(buf, "QVC"))
		fseek(ifp, -4, SEEK_CUR);
	else {
		fseek(ifp, -10, SEEK_CUR);
	}
	entries = get2();
	if (entries > 1000) return;
	morder = order;
	while (entries--) {
		order = morder;
		tiff_get(base, &tag, &type, &len, &save);
		tag |= uptag << 16;
		if (tag == 4 && len > 26 && len < 35) {
			if ((i = (get4(), get2())) != 0x7fff && !iso_speed)
				iso_speed = 50 * pow(2, i / 32.0 - 4);
			if ((i = (get2(), get2())) != 0x7fff && !aperture)
				aperture = pow(2, i / 64.0);
			if ((i = get2()) != 0xffff && !shutter)
				shutter = pow(2, (short)i / -32.0);
			wbi = (get2(), get2());
			shot_order = (get2(), get2());
		}
		if (tag == 7 && type == 2 && len > 20)
			fgets(model2, 64, ifp);
		if (tag == 8 && type == 4)
			shot_order = get4();
		if (tag == 0xc && len == 4)
			FORC3 cam_mul[(c << 1 | c >> 1) & 3] = getreal(type);
		if (tag == 0xd && type == 7 && get2() == 0xaaaa) {
			for (c = i = 2; (ushort)c != 0xbbbb && i < len; i++)
				c = c << 8 | fgetc(ifp);
			while ((i += 4) < len - 5)
				if (get4() == 257 && (i = len) && (c = (get4(), fgetc(ifp))) < 3)
					flip = "065"[c] - '0';
		}
		if (tag == 0x10 && type == 4)
			unique_id = get4();
		if (tag == 0x14 && type == 7) {
			if (len == 2560) {
				fseek(ifp, 1248, SEEK_CUR);
				goto get2_256;
			}
			fread(buf, 1, 10, ifp);
			if (!strncmp(buf, "NRW ", 4)) {
				fseek(ifp, strcmp(buf + 4, "0100") ? 46 : 1546, SEEK_CUR);
				cam_mul[0] = get4() << 2;
				cam_mul[1] = get4() + get4();
				cam_mul[2] = get4() << 2;
			}
		}
		if (tag == 0x15 && type == 2 && is_raw)
			fread(model, 64, 1, ifp);
		if (tag == 0x1d)
			while ((c = fgetc(ifp)) && c != EOF)
				serial = serial * 10 + (isdigit(c) ? c - '0' : c % 10);
		if (tag == 0x29 && type == 1) {
			c = wbi < 18 ? "012347800000005896"[wbi] - '0' : 0;
			fseek(ifp, 8 + c * 32, SEEK_CUR);
			FORC4 cam_mul[c ^ (c >> 1) ^ 1] = get4();
		}
		if (tag == 0x3d && type == 3 && len == 4)
			FORC4 cblack[c ^ c >> 1] = get2() >> (14 - tiff_bps);
		if (tag == 0x81 && type == 4) {
			data_offset = get4();
			fseek(ifp, data_offset + 41, SEEK_SET);
			raw_height = get2() * 2;
			raw_width = get2();
			filters = 0x61616161;
		}
		if ((tag == 0x81 && type == 7) ||
			(tag == 0x100 && type == 7) ||
			(tag == 0x280 && type == 1)) {
			thumb_offset = ftell(ifp);
			thumb_length = len;
		}
		if (tag == 0x88 && type == 4 && (thumb_offset = get4()))
			thumb_offset += base;
		if (tag == 0x89 && type == 4)
			thumb_length = get4();
		if (tag == 0x8c || tag == 0x96)
			meta_offset = ftell(ifp);
		if (tag == 0x97) {
			for (i = 0; i < 4; i++)
				ver97 = ver97 * 10 + fgetc(ifp) - '0';
			switch (ver97) {
			case 100:
				fseek(ifp, 68, SEEK_CUR);
				FORC4 cam_mul[(c >> 1) | ((c & 1) << 1)] = get2();
				break;
			case 102:
				fseek(ifp, 6, SEEK_CUR);
				FORC4 cam_mul[c ^ (c >> 1)] = get2();
				break;
			case 103:
				fseek(ifp, 16, SEEK_CUR);
				FORC4 cam_mul[c] = get2();
			}
			if (ver97 >= 200) {
				if (ver97 != 205) fseek(ifp, 280, SEEK_CUR);
				fread(buf97, 324, 1, ifp);
			}
		}
		if (tag == 0xa1 && type == 7) {
			order = 0x4949;
			fseek(ifp, 140, SEEK_CUR);
			FORC3 cam_mul[c] = get4();
		}
		if (tag == 0xa4 && type == 3) {
			fseek(ifp, wbi * 48, SEEK_CUR);
			FORC3 cam_mul[c] = get2();
		}
		if (tag == 0xa7 && (unsigned)(ver97 - 200) < 17) {
			ci = xlat[0][serial & 0xff];
			cj = xlat[1][fgetc(ifp) ^ fgetc(ifp) ^ fgetc(ifp) ^ fgetc(ifp)];
			ck = 0x60;
			for (i = 0; i < 324; i++)
				buf97[i] ^= (cj += ci * ck++);
			i = "66666>666;6A;:;55"[ver97 - 200] - '0';
			FORC4 cam_mul[c ^ (c >> 1) ^ (i & 1)] =
				sget2(buf97 + (i & -2) + c * 2);
		}
		if (tag == 0x200 && len == 3)
			shot_order = (get4(), get4());
		if (tag == 0x200 && len == 4)
			FORC4 cblack[c ^ c >> 1] = get2();
		if (tag == 0x201 && len == 4)
			FORC4 cam_mul[c ^ (c >> 1)] = get2();
		if (tag == 0x220 && type == 7)
			meta_offset = ftell(ifp);
		if (tag == 0x401 && type == 4 && len == 4)
			FORC4 cblack[c ^ c >> 1] = get4();
		if (tag == 0xe01) {		/* Nikon Capture Note */
			order = 0x4949;
			fseek(ifp, 22, SEEK_CUR);
			for (offset = 22; offset + 22 < len; offset += 22 + i) {
				tag = get4();
				fseek(ifp, 14, SEEK_CUR);
				i = get4() - 4;
				if (tag == 0x76a43207) flip = get2();
				else fseek(ifp, i, SEEK_CUR);
			}
		}
		if (tag == 0xe80 && len == 256 && type == 7) {
			fseek(ifp, 48, SEEK_CUR);
			cam_mul[0] = get2() * 508 * 1.078 / 0x10000;
			cam_mul[2] = get2() * 382 * 1.173 / 0x10000;
		}
		if (tag == 0xf00 && type == 7) {
			if (len == 614)
				fseek(ifp, 176, SEEK_CUR);
			else if (len == 734 || len == 1502)
				fseek(ifp, 148, SEEK_CUR);
			else goto next;
			goto get2_256;
		}
		if ((tag == 0x1011 && len == 9) || tag == 0x20400200)
			for (i = 0; i < 3; i++)
				FORC3 cmatrix[i][c] = ((short)get2()) / 256.0;
		if ((tag == 0x1012 || tag == 0x20400600) && len == 4)
			FORC4 cblack[c ^ c >> 1] = get2();
		if (tag == 0x1017 || tag == 0x20400100)
			cam_mul[0] = get2() / 256.0;
		if (tag == 0x1018 || tag == 0x20400100)
			cam_mul[2] = get2() / 256.0;
		if (tag == 0x2011 && len == 2) {
		get2_256:
			order = 0x4d4d;
			cam_mul[0] = get2() / 256.0;
			cam_mul[2] = get2() / 256.0;
		}
		if ((tag | 0x70) == 0x2070 && (type == 4 || type == 13))
			fseek(ifp, get4() + base, SEEK_SET);
		if (tag == 0x2020 && !strncmp(buf, "OLYMP", 5))
			parse_thumb_note(base, 257, 258);
		if (tag == 0x2040)
			parse_makernote(base, 0x2040);
		if (tag == 0xb028) {
			fseek(ifp, get4() + base, SEEK_SET);
			parse_thumb_note(base, 136, 137);
		}
		if (tag == 0x4001 && len > 500) {
			i = len == 582 ? 50 : len == 653 ? 68 : len == 5120 ? 142 : 126;
			fseek(ifp, i, SEEK_CUR);
			FORC4 cam_mul[c ^ (c >> 1)] = get2();
			for (i += 18; i <= len; i += 10) {
				get2();
				FORC4 sraw_mul[c ^ (c >> 1)] = get2();
				if (sraw_mul[1] == 1170) break;
			}
		}
		if (tag == 0x4021 && get4() && get4())
			FORC4 cam_mul[c] = 1024;
		if (tag == 0xa021)
			FORC4 cam_mul[c ^ (c >> 1)] = get4();
		if (tag == 0xa028)
			FORC4 cam_mul[c ^ (c >> 1)] -= get4();
		if (tag == 0xb001)
			unique_id = get2();
	next:
		fseek(ifp, save, SEEK_SET);
	}
quit:
	order = sorder;
}

/*
   Since the TIFF DateTime string has no timezone information,
   assume that the camera's clock was set to Universal Time.
 */
void CLASS get_timestamp(int reversed)
{
	struct tm t;
	char str[20];
	int i;

	str[19] = 0;
	if (reversed)
		for (i = 19; i--; ) str[i] = fgetc(ifp);
	else
		fread(str, 19, 1, ifp);
	memset(&t, 0, sizeof t);
	if (sscanf(str, "%d:%d:%d %d:%d:%d", &t.tm_year, &t.tm_mon,
		&t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6)
		return;
	t.tm_year -= 1900;
	t.tm_mon -= 1;
	t.tm_isdst = -1;
	if (mktime(&t) > 0)
		timestamp = mktime(&t);
}

void CLASS parse_exif(int base)
{
	unsigned entries, tag, type, len, save, c;
	double expo;

	entries = get2();
	while (entries--) {
		tiff_get(base, &tag, &type, &len, &save);
		switch (tag) {
		case 33434:  tiff_ifd[tiff_nifds - 1].shutter =
			shutter = getreal(type);		break;
		case 33437:  aperture = getreal(type);		break;
		case 34855:  iso_speed = get2();			break;
		case 36867:
		case 36868:  get_timestamp(0);			break;
		case 37377:  if ((expo = -getreal(type)) < 128)
			tiff_ifd[tiff_nifds - 1].shutter =
			shutter = pow(2, expo);		break;
		case 37378:  aperture = pow(2, getreal(type) / 2);	break;
		case 37386:  focal_len = getreal(type);		break;
		case 37500:  parse_makernote(base, 0);		break;
		case 41730:
			if (get4() == 0x20002)
				for (exif_cfa = c = 0; c < 8; c += 2)
					exif_cfa |= fgetc(ifp) * 0x01010101 << c;
		}
		fseek(ifp, save, SEEK_SET);
	}
}

void CLASS parse_gps(int base)
{
	unsigned entries, tag, type, len, save, c;

	entries = get2();
	while (entries--) {
		tiff_get(base, &tag, &type, &len, &save);
		switch (tag) {
		case 1: case 3: case 5:
			gpsdata[29 + tag / 2] = getc(ifp);			break;
		case 2: case 4: case 7:
			FORC(6) gpsdata[tag / 3 * 6 + c] = get4();		break;
		case 6:
			FORC(2) gpsdata[18 + c] = get4();			break;
		case 18: case 29:
			fgets((char *)(gpsdata + 14 + tag / 3), MIN(len, 12), ifp);
		}
		fseek(ifp, save, SEEK_SET);
	}
}

void CLASS romm_coeff(float romm_cam[3][3])
{
	static const float rgb_romm[3][3] =	/* ROMM == Kodak ProPhoto */
	{ {  2.034193, -0.727420, -0.306766 },
	  { -0.228811,  1.231729, -0.002922 },
	  { -0.008565, -0.153273,  1.161839 } };
	int i, j, k;

	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			for (cmatrix[i][j] = k = 0; k < 3; k++)
				cmatrix[i][j] += rgb_romm[i][k] * romm_cam[k][j];
}

void CLASS parse_mos(int offset)
{
	char data[40];
	int skip, from, i, c, neut[4], planes = 0, frot = 0;
	static const char *mod[] =
	{ "","DCB2","Volare","Cantare","CMost","Valeo 6","Valeo 11","Valeo 22",
	  "Valeo 11p","Valeo 17","","Aptus 17","Aptus 22","Aptus 75","Aptus 65",
	  "Aptus 54S","Aptus 65S","Aptus 75S","AFi 5","AFi 6","AFi 7",
	  "AFi-II 7","Aptus-II 7","","Aptus-II 6","","","Aptus-II 10","Aptus-II 5",
	  "","","","","Aptus-II 10R","Aptus-II 8","","Aptus-II 12","","AFi-II 12" };
	float romm_cam[3][3];

	fseek(ifp, offset, SEEK_SET);
	while (1) {
		if (get4() != 0x504b5453) break;
		get4();
		fread(data, 1, 40, ifp);
		skip = get4();
		from = ftell(ifp);
		if (!strcmp(data, "JPEG_preview_data")) {
			thumb_offset = from;
			thumb_length = skip;
		}
		if (!strcmp(data, "icc_camera_profile")) {
			profile_offset = from;
			profile_length = skip;
		}
		if (!strcmp(data, "ShootObj_back_type")) {
			fscanf(ifp, "%d", &i);
			if ((unsigned)i < sizeof mod / sizeof(*mod))
				strcpy(model, mod[i]);
		}
		if (!strcmp(data, "icc_camera_to_tone_matrix")) {
			for (i = 0; i < 9; i++)
				((float *)romm_cam)[i] = int_to_float(get4());
			romm_coeff(romm_cam);
		}
		if (!strcmp(data, "CaptProf_color_matrix")) {
			for (i = 0; i < 9; i++)
				fscanf(ifp, "%f", (float *)romm_cam + i);
			romm_coeff(romm_cam);
		}
		if (!strcmp(data, "CaptProf_number_of_planes"))
			fscanf(ifp, "%d", &planes);
		if (!strcmp(data, "CaptProf_raw_data_rotation"))
			fscanf(ifp, "%d", &flip);
		if (!strcmp(data, "CaptProf_mosaic_pattern"))
			FORC4{
		  fscanf(ifp, "%d", &i);
		  if (i == 1) frot = c ^ (c >> 1);
		}
			if (!strcmp(data, "ImgProf_rotation_angle")) {
				fscanf(ifp, "%d", &i);
				flip = i - flip;
			}
		if (!strcmp(data, "NeutObj_neutrals") && !cam_mul[0]) {
			FORC4 fscanf(ifp, "%d", neut + c);
			FORC3 cam_mul[c] = (float)neut[0] / neut[c + 1];
		}
		if (!strcmp(data, "Rows_data"))
			load_flags = get4();
		parse_mos(from);
		fseek(ifp, skip + from, SEEK_SET);
	}
	if (planes)
		filters = (planes == 1) * 0x01010101 *
		(uchar) "\x94\x61\x16\x49"[(flip / 90 + frot) & 3];
}

void CLASS linear_table(unsigned len)
{
	int i;
	if (len > 0x1000) len = 0x1000;
	read_shorts(curve, len);
	for (i = len; i < 0x1000; i++)
		curve[i] = curve[i - 1];
	maximum = curve[0xfff];
}

int CLASS parse_tiff(int base);

int CLASS parse_tiff_ifd(int base)
{
	unsigned entries, tag, type, len, plen = 16, save;
	int ifd, use_cm = 0, cfa, i, j, c, ima_len = 0;
	char software[64], *cbuf, *cp;
	uchar cfa_pat[16], cfa_pc[] = { 0,1,2,3 }, tab[256];
	double cc[4][4], cm[4][3], cam_xyz[4][3], num;
	double ab[] = { 1,1,1,1 }, asn[] = { 0,0,0,0 }, xyz[] = { 1,1,1 };
	unsigned *buf;
	struct jhead jh;
	FILE *sfp;

	if (tiff_nifds >= sizeof tiff_ifd / sizeof tiff_ifd[0])
		return 1;
	ifd = tiff_nifds++;
	for (j = 0; j < 4; j++)
		for (i = 0; i < 4; i++)
			cc[j][i] = i == j;
	entries = get2();
	if (entries > 512) return 1;
	while (entries--) {
		tiff_get(base, &tag, &type, &len, &save);
		switch (tag) {
		case 5:   width = get2();  break;
		case 6:   height = get2();  break;
		case 7:   width += get2();  break;
		case 9:   if ((i = get2())) filters = i;  break;
		case 17: case 18:
			if (type == 3 && len == 1)
				cam_mul[(tag - 17) * 2] = get2() / 256.0;
			break;
		case 23:
			if (type == 3) iso_speed = get2();
			break;
		case 28: case 29: case 30:
			cblack[tag - 28] = get2();
			cblack[3] = cblack[1];
			break;
		case 36: case 37: case 38:
			cam_mul[tag - 36] = get2();
			break;
		case 39:
			if (len < 50 || cam_mul[0]) break;
			fseek(ifp, 12, SEEK_CUR);
			FORC3 cam_mul[c] = get2();
			break;
		case 46:
			if (type != 7 || fgetc(ifp) != 0xff || fgetc(ifp) != 0xd8) break;
			thumb_offset = ftell(ifp) - 2;
			thumb_length = len;
			break;
		case 61440:			/* Fuji HS10 table */
			fseek(ifp, get4() + base, SEEK_SET);
			parse_tiff_ifd(base);
			break;
		case 2: case 256: case 61441:	/* ImageWidth */
			tiff_ifd[ifd].width = getint(type);
			break;
		case 3: case 257: case 61442:	/* ImageHeight */
			tiff_ifd[ifd].height = getint(type);
			break;
		case 258:				/* BitsPerSample */
		case 61443:
			tiff_ifd[ifd].samples = len & 7;
			tiff_ifd[ifd].bps = getint(type);
			if (tiff_bps < tiff_ifd[ifd].bps)
				tiff_bps = tiff_ifd[ifd].bps;
			break;
		case 61446:
			raw_height = 0;
			if (tiff_ifd[ifd].bps > 12) break;
			load_raw = &CLASS packed_load_raw;
			load_flags = get4() ? 24 : 80;
			break;
		case 259:				/* Compression */
			tiff_ifd[ifd].comp = getint(type);
			break;
		case 262:				/* PhotometricInterpretation */
			tiff_ifd[ifd].phint = get2();
			break;
		case 270:				/* ImageDescription */
			fread(desc, 512, 1, ifp);
			break;
		case 271:				/* Make */
			//fgets(make, 64, ifp);
			break;
		case 272:				/* Model */
			fgets(model, 64, ifp);
			break;
		case 280:				/* Panasonic RW2 offset */
			if (type != 4)
				break;
		case 273:				/* StripOffset */
		case 513:				/* JpegIFOffset */
		case 61447:
			tiff_ifd[ifd].offset = get4() + base;
			if (!tiff_ifd[ifd].bps && tiff_ifd[ifd].offset > 0) {
				fseek(ifp, tiff_ifd[ifd].offset, SEEK_SET);
				if (ljpeg_start(&jh, 1)) {
					tiff_ifd[ifd].comp = 6;
					tiff_ifd[ifd].width = jh.wide;
					tiff_ifd[ifd].height = jh.high;
					tiff_ifd[ifd].bps = jh.bits;
					tiff_ifd[ifd].samples = jh.clrs;
					if (!(jh.sraw || (jh.clrs & 1)))
						tiff_ifd[ifd].width *= jh.clrs;
					if ((tiff_ifd[ifd].width > 4 * tiff_ifd[ifd].height) & ~jh.clrs) {
						tiff_ifd[ifd].width /= 2;
						tiff_ifd[ifd].height *= 2;
					}
					i = order;
					parse_tiff(tiff_ifd[ifd].offset + 12);
					order = i;
				}
			}
			break;
		case 274:				/* Orientation */
			tiff_ifd[ifd].flip = "50132467"[get2() & 7] - '0';
			break;
		case 277:				/* SamplesPerPixel */
			tiff_ifd[ifd].samples = getint(type) & 7;
			break;
		case 279:				/* StripByteCounts */
		case 514:
		case 61448:
			tiff_ifd[ifd].bytes = get4();
			break;
		case 61454:
			FORC3 cam_mul[(4 - c) % 3] = getint(type);
			break;
		case 305:  case 11:		/* Software */
			fgets(software, 64, ifp);
			if (!strncmp(software, "Adobe", 5) ||
				!strncmp(software, "dcraw", 5) ||
				!strncmp(software, "UFRaw", 5) ||
				!strncmp(software, "Bibble", 6) ||
				!strcmp(software, "Digital Photo Professional"))
				is_raw = 0;
			break;
		case 306:				/* DateTime */
			get_timestamp(0);
			break;
		case 315:				/* Artist */
			fread(artist, 64, 1, ifp);
			break;
		case 322:				/* TileWidth */
			tiff_ifd[ifd].tile_width = getint(type);
			break;
		case 323:				/* TileLength */
			tiff_ifd[ifd].tile_length = getint(type);
			break;
		case 324:				/* TileOffsets */
			tiff_ifd[ifd].offset = len > 1 ? ftell(ifp) : get4();
			if (len == 1)
				tiff_ifd[ifd].tile_width = tiff_ifd[ifd].tile_length = 0;
			if (len == 4) {
				load_raw = &CLASS sinar_4shot_load_raw;
				is_raw = 5;
			}
			break;
		case 330:				/* SubIFDs */
			while (len--) {
				i = ftell(ifp);
				fseek(ifp, get4() + base, SEEK_SET);
				if (parse_tiff_ifd(base)) break;
				fseek(ifp, i + 4, SEEK_SET);
			}
			break;
		case 29443:
			FORC4 cam_mul[c ^ (c < 2)] = get2();
			break;
		case 29459:
			FORC4 cam_mul[c] = get2();
			i = (cam_mul[1] == 1024 && cam_mul[2] == 1024) << 1;
			SWAP(cam_mul[i], cam_mul[i + 1])
				break;
		case 33405:			/* Model2 */
			fgets(model2, 64, ifp);
			break;
		case 33421:			/* CFARepeatPatternDim */
			if (get2() == 6 && get2() == 6)
				filters = 9;
			break;
		case 33422:			/* CFAPattern */
			if (filters == 9) {
				FORC(36) ((char *)xtrans)[c] = fgetc(ifp) & 3;
				break;
			}
		case 33434:			/* ExposureTime */
			tiff_ifd[ifd].shutter = shutter = getreal(type);
			break;
		case 33437:			/* FNumber */
			aperture = getreal(type);
			break;
		case 34306:			/* Leaf white balance */
			FORC4 cam_mul[c ^ 1] = 4096.0 / get2();
			break;
		case 34307:			/* Leaf CatchLight color matrix */
			fread(software, 1, 7, ifp);
			if (strncmp(software, "MATRIX", 6)) break;
			colors = 4;
			for (raw_color = i = 0; i < 3; i++) {
				FORC4 fscanf(ifp, "%f", &rgb_cam[i][c ^ 1]);
				if (!use_camera_wb) continue;
				num = 0;
				FORC4 num += rgb_cam[i][c];
				FORC4 rgb_cam[i][c] /= num;
			}
			break;
		case 34310:			/* Leaf metadata */
			parse_mos(ftell(ifp));
		case 34665:			/* EXIF tag */
			fseek(ifp, get4() + base, SEEK_SET);
			parse_exif(base);
			break;
		case 34853:			/* GPSInfo tag */
			fseek(ifp, get4() + base, SEEK_SET);
			parse_gps(base);
			break;
		case 34675:			/* InterColorProfile */
		case 50831:			/* AsShotICCProfile */
			profile_offset = ftell(ifp);
			profile_length = len;
			break;
		case 37386:			/* FocalLength */
			focal_len = getreal(type);
			break;
		case 37393:			/* ImageNumber */
			shot_order = getint(type);
			break;
		case 40976:
			strip_offset = get4();
			switch (tiff_ifd[ifd].comp) {
			case 32770: load_raw = &CLASS samsung_load_raw;   break;
			case 32772: load_raw = &CLASS samsung2_load_raw;  break;
			case 32773: load_raw = &CLASS samsung3_load_raw;  break;
			}
			break;
		case 46279:
			if (!ima_len) break;
			fseek(ifp, 38, SEEK_CUR);
		case 46274:
			fseek(ifp, 40, SEEK_CUR);
			raw_width = get4();
			raw_height = get4();
			left_margin = get4() & 7;
			width = raw_width - left_margin - (get4() & 7);
			top_margin = get4() & 7;
			height = raw_height - top_margin - (get4() & 7);
			if (raw_width == 7262) {
				height = 5444;
				width = 7244;
				left_margin = 7;
			}
			fseek(ifp, 52, SEEK_CUR);
			FORC3 cam_mul[c] = getreal(11);
			fseek(ifp, 114, SEEK_CUR);
			flip = (get2() >> 7) * 90;
			if (width * height * 6 == ima_len) {
				if (flip % 180 == 90) SWAP(width, height);
				raw_width = width;
				raw_height = height;
				left_margin = top_margin = filters = flip = 0;
			}
			sprintf(model, "Ixpress %d-Mp", height*width / 1000000);
			load_raw = &CLASS imacon_full_load_raw;
			if (filters) {
				if (left_margin & 1) filters = 0x61616161;
				load_raw = &CLASS unpacked_load_raw;
			}
			maximum = 0xffff;
			break;
		case 50454:			/* Sinar tag */
		case 50455:
			if (!(cbuf = (char *)malloc(len))) break;
			fread(cbuf, 1, len, ifp);
			for (cp = cbuf - 1; cp && cp < cbuf + len; cp = strchr(cp, '\n'))
				if (!strncmp(++cp, "Neutral ", 8))
					sscanf(cp + 8, "%f %f %f", cam_mul, cam_mul + 1, cam_mul + 2);
			free(cbuf);
			break;
		case 50459:			/* Hasselblad tag */
			i = order;
			j = ftell(ifp);
			c = tiff_nifds;
			order = get2();
			fseek(ifp, j + (get2(), get4()), SEEK_SET);
			parse_tiff_ifd(j);
			maximum = 0xffff;
			tiff_nifds = c;
			order = i;
			break;
		case 50710:			/* CFAPlaneColor */
			if (filters == 9) break;
			if (len > 4) len = 4;
			colors = len;
			fread(cfa_pc, 1, colors, ifp);
		guess_cfa_pc:
			FORCC tab[cfa_pc[c]] = c;
			cdesc[c] = 0;
			for (i = 16; i--; )
				filters = filters << 2 | tab[cfa_pat[i % plen]];
			filters -= !filters;
			break;
		case 291:
		case 50712:			/* LinearizationTable */
			linear_table(len);
			break;
		case 50713:			/* BlackLevelRepeatDim */
			cblack[4] = get2();
			cblack[5] = get2();
			if (cblack[4] * cblack[5] > sizeof cblack / sizeof *cblack - 6)
				cblack[4] = cblack[5] = 1;
			break;
		case 61450:
			cblack[4] = cblack[5] = MIN(sqrt(len), 64);
		case 50714:			/* BlackLevel */
			if (!(cblack[4] * cblack[5]))
				cblack[4] = cblack[5] = 1;
			FORC(cblack[4] * cblack[5])
				cblack[6 + c] = getreal(type);
			black = 0;
			break;
		case 50715:			/* BlackLevelDeltaH */
		case 50716:			/* BlackLevelDeltaV */
			for (num = i = 0; i < (len & 0xffff); i++)
				num += getreal(type);
			black += num / len + 0.5;
			break;
		case 50717:			/* WhiteLevel */
			maximum = getint(type);
			break;
		case 50718:			/* DefaultScale */
			pixel_aspect = getreal(type);
			pixel_aspect /= getreal(type);
			break;
		case 50721:			/* ColorMatrix1 */
		case 50722:			/* ColorMatrix2 */
			FORCC for (j = 0; j < 3; j++)
				cm[c][j] = getreal(type);
			use_cm = 1;
			break;
		case 50723:			/* CameraCalibration1 */
		case 50724:			/* CameraCalibration2 */
			for (i = 0; i < colors; i++)
				FORCC cc[i][c] = getreal(type);
			break;
		case 50727:			/* AnalogBalance */
			FORCC ab[c] = getreal(type);
			break;
		case 50728:			/* AsShotNeutral */
			FORCC asn[c] = getreal(type);
			break;
		case 50729:			/* AsShotWhiteXY */
			xyz[0] = getreal(type);
			xyz[1] = getreal(type);
			xyz[2] = 1 - xyz[0] - xyz[1];
			FORC3 xyz[c] /= d65_white[c];
			break;
		case 50752:
			read_shorts(cr2_slice, 3);
			break;
		case 50829:			/* ActiveArea */
			top_margin = getint(type);
			left_margin = getint(type);
			height = getint(type) - top_margin;
			width = getint(type) - left_margin;
			break;
		case 50830:			/* MaskedAreas */
			for (i = 0; i < len && i < 32; i++)
				((int *)mask)[i] = getint(type);
			black = 0;
			break;
		case 51009:			/* OpcodeList2 */
			meta_offset = ftell(ifp);
			break;
		case 65026:
			if (type == 2) fgets(model2, 64, ifp);
		}
		fseek(ifp, save, SEEK_SET);
	}
	for (i = 0; i < colors; i++)
		FORCC cc[i][c] *= ab[i];
	if (use_cm) {
		FORCC for (i = 0; i < 3; i++)
			for (cam_xyz[c][i] = j = 0; j < colors; j++)
				cam_xyz[c][i] += cc[c][j] * cm[j][i] * xyz[i];
		cam_xyz_coeff(cmatrix, cam_xyz);
	}
	if (asn[0]) {
		cam_mul[3] = 0;
		FORCC cam_mul[c] = 1 / asn[c];
	}
	if (!use_cm)
		FORCC pre_mul[c] /= cc[c][c];
	return 0;
}

int CLASS parse_tiff(int base)
{
	int doff;

	fseek(ifp, base, SEEK_SET);
	order = get2();
	if (order != 0x4949 && order != 0x4d4d) return 0;
	get2();
	while ((doff = get4())) {
		fseek(ifp, doff + base, SEEK_SET);
		if (parse_tiff_ifd(base)) break;
	}
	return 1;
}

void CLASS apply_tiff()
{
	int max_samp = 0, ties = 0, os, ns, raw = -1, thm = -1, i;
	struct jhead jh;

	thumb_misc = 16;
	if (thumb_offset) {
		fseek(ifp, thumb_offset, SEEK_SET);
		if (ljpeg_start(&jh, 1)) {
			thumb_misc = jh.bits;
			thumb_width = jh.wide;
			thumb_height = jh.high;
		}
	}
	for (i = tiff_nifds; i--; ) {
		if (tiff_ifd[i].shutter)
			shutter = tiff_ifd[i].shutter;
		tiff_ifd[i].shutter = shutter;
	}
	for (i = 0; i < tiff_nifds; i++) {
		if (max_samp < tiff_ifd[i].samples)
			max_samp = tiff_ifd[i].samples;
		if (max_samp > 3) max_samp = 3;
		os = raw_width*raw_height;
		ns = tiff_ifd[i].width*tiff_ifd[i].height;
		if (tiff_bps) {
			os *= tiff_bps;
			ns *= tiff_ifd[i].bps;
		}
		if ((tiff_ifd[i].comp != 6 || tiff_ifd[i].samples != 3) &&
			(tiff_ifd[i].width | tiff_ifd[i].height) < 0x10000 &&
			ns && ((ns > os && (ties = 1)) ||
			(ns == os && shot_select == ties++))) {
			raw_width = tiff_ifd[i].width;
			raw_height = tiff_ifd[i].height;
			tiff_bps = tiff_ifd[i].bps;
			tiff_compress = tiff_ifd[i].comp;
			data_offset = tiff_ifd[i].offset;
			tiff_flip = tiff_ifd[i].flip;
			tiff_samples = tiff_ifd[i].samples;
			tile_width = tiff_ifd[i].tile_width;
			tile_length = tiff_ifd[i].tile_length;
			shutter = tiff_ifd[i].shutter;
			raw = i;
		}
	}
	if (is_raw == 1 && ties) is_raw = ties;
	if (!tile_width) tile_width = INT_MAX;
	if (!tile_length) tile_length = INT_MAX;
	for (i = tiff_nifds; i--; )
		if (tiff_ifd[i].flip) tiff_flip = tiff_ifd[i].flip;
	if (raw >= 0 && !load_raw)
		switch (tiff_compress) {
		case 32767:
			load_flags = 79;
		case 32769:
			load_flags++;
		case 32770:
		case 32773: goto slr;
		case 0:  case 1:
			if (tiff_ifd[raw].bytes * 2 == raw_width*raw_height * 3)
				load_flags = 24;
			if (tiff_ifd[raw].bytes * 5 == raw_width*raw_height * 8) {
				load_flags = 81;
				tiff_bps = 12;
			} slr:
			switch (tiff_bps) {
			case  8: load_raw = &CLASS eight_bit_load_raw;	break;
			case 12: if (tiff_ifd[raw].phint == 2)
				load_flags = 6;
				load_raw = &CLASS packed_load_raw;		break;
			case 14: load_flags = 0;
			case 16: load_raw = &CLASS unpacked_load_raw;
				if (tiff_ifd[raw].bytes * 7 > raw_width*raw_height)
					load_raw = &CLASS olympus_load_raw;
			}
			break;
		case 6:  case 7:  case 99:
			load_raw = &CLASS lossless_jpeg_load_raw;		break;
		case 34713:
			if ((raw_width + 9) / 10 * 16 * raw_height == tiff_ifd[raw].bytes) {
				load_raw = &CLASS packed_load_raw;
				load_flags = 1;
			} else if (raw_width*raw_height * 3 == tiff_ifd[raw].bytes * 2) {
				load_raw = &CLASS packed_load_raw;
				if (model[0] == 'N') load_flags = 80;
			} else if (raw_width*raw_height * 2 == tiff_ifd[raw].bytes) {
				load_raw = &CLASS unpacked_load_raw;
				load_flags = 4;
				order = 0x4d4d;
			}
			break;
		case 65535:
			load_raw = &CLASS pentax_load_raw;			break;
		case 32867: case 34892: break;
		default: is_raw = 0;
		}
	for (i = 0; i < tiff_nifds; i++)
		if (i != raw && tiff_ifd[i].samples == max_samp &&
			tiff_ifd[i].width * tiff_ifd[i].height / (SQR(tiff_ifd[i].bps) + 1) >
			thumb_width *       thumb_height / (SQR(thumb_misc) + 1)
			&& tiff_ifd[i].comp != 34892) {
			thumb_width = tiff_ifd[i].width;
			thumb_height = tiff_ifd[i].height;
			thumb_offset = tiff_ifd[i].offset;
			thumb_length = tiff_ifd[i].bytes;
			thumb_misc = tiff_ifd[i].bps;
			thm = i;
		}
	if (thm >= 0) {
		thumb_misc |= tiff_ifd[thm].samples << 5;
	}
}

/*
   Many cameras have a "debug mode" that writes JPEG and raw
   at the same time.  The raw file has no header, so try to
   to open the matching JPEG file and read its metadata.
 */
void CLASS parse_external_jpeg()
{
	const char *file, *ext;
	char *jname, *jfile, *jext;
	FILE *save = ifp;

	ext = strrchr(ifname, '.');
	file = strrchr(ifname, '/');
	if (!file) file = strrchr(ifname, '\\');
	if (!file) file = ifname - 1;
	file++;
	if (!ext || strlen(ext) != 4 || ext - file != 8) return;
	jname = (char *)malloc(strlen(ifname) + 1);
	merror(jname, "parse_external_jpeg()");
	strcpy(jname, ifname);
	jfile = file - ifname + jname;
	jext = ext - ifname + jname;
	if (strcasecmp(ext, ".jpg")) {
		strcpy(jext, isupper(ext[1]) ? ".JPG" : ".jpg");
		if (isdigit(*file)) {
			memcpy(jfile, file + 4, 4);
			memcpy(jfile + 4, file, 4);
		}
	} else
		while (isdigit(*--jext)) {
			if (*jext != '9') {
				(*jext)++;
				break;
			}
			*jext = '0';
		}
	if (strcmp(jname, ifname)) {
		if ((ifp = fopen(jname, "rb"))) {
			if (verbose)
				fprintf(stderr, _("Reading metadata from %s ...\n"), jname);
			parse_tiff(12);
			thumb_offset = 0;
			is_raw = 1;
			fclose(ifp);
		}
	}
	if (!timestamp)
		fprintf(stderr, _("Failed to read metadata from %s\n"), jname);
	free(jname);
	ifp = save;
}

/*
   CIFF block 0x1030 contains an 8x8 white sample.
   Load this into white[][] for use in scale_colors().
 */
void CLASS ciff_block_1030()
{
	static const ushort key[] = { 0x410, 0x45f3 };
	int i, bpp, row, col, vbits = 0;
	unsigned long bitbuf = 0;

	if ((get2(), get4()) != 0x80008 || !get4()) return;
	bpp = get2();
	if (bpp != 10 && bpp != 12) return;
	for (i = row = 0; row < 8; row++)
		for (col = 0; col < 8; col++) {
			if (vbits < bpp) {
				bitbuf = bitbuf << 16 | (get2() ^ key[i++ & 1]);
				vbits += 16;
			}
			white[row][col] = bitbuf >> (vbits -= bpp) & ~(-1 << bpp);
		}
}

/*
   Parse a CIFF file, better known as Canon CRW format.
 */
void CLASS parse_ciff(int offset, int length, int depth)
{
	int tboff, nrecs, c, type, len, save, wbi = -1;
	ushort key[] = { 0x410, 0x45f3 };

	fseek(ifp, offset + length - 4, SEEK_SET);
	tboff = get4() + offset;
	fseek(ifp, tboff, SEEK_SET);
	nrecs = get2();
	if ((nrecs | depth) > 127) return;
	while (nrecs--) {
		type = get2();
		len = get4();
		save = ftell(ifp) + 4;
		fseek(ifp, offset + get4(), SEEK_SET);
		if ((((type >> 8) + 8) | 8) == 0x38)
			parse_ciff(ftell(ifp), len, depth + 1); /* Parse a sub-table */
		if (type == 0x0810)
			fread(artist, 64, 1, ifp);
		if (type == 0x1810) {
			width = get4();
			height = get4();
			pixel_aspect = int_to_float(get4());
			flip = get4();
		}
		if (type == 0x1835)			/* Get the decoder table */
			tiff_compress = get4();
		if (type == 0x2007) {
			thumb_offset = ftell(ifp);
			thumb_length = len;
		}
		if (type == 0x1818) {
			shutter = pow(2, -int_to_float((get4(), get4())));
			aperture = pow(2, int_to_float(get4()) / 2);
		}
		if (type == 0x102a) {
			iso_speed = pow(2, (get4(), get2()) / 32.0 - 4) * 50;
			aperture = pow(2, (get2(), (short)get2()) / 64.0);
			shutter = pow(2, -((short)get2()) / 32.0);
			wbi = (get2(), get2());
			if (wbi > 17) wbi = 0;
			fseek(ifp, 32, SEEK_CUR);
			if (shutter > 1e6) shutter = get2() / 10.0;
		}
		if (type == 0x102c) {
			if (get2() > 512) {		/* Pro90, G1 */
				fseek(ifp, 118, SEEK_CUR);
				FORC4 cam_mul[c ^ 2] = get2();
			} else {				/* G2, S30, S40 */
				fseek(ifp, 98, SEEK_CUR);
				FORC4 cam_mul[c ^ (c >> 1) ^ 1] = get2();
			}
		}
		if (type == 0x0032) {
			if (len == 768) {			/* EOS D30 */
				fseek(ifp, 72, SEEK_CUR);
				FORC4 cam_mul[c ^ (c >> 1)] = 1024.0 / get2();
				if (!wbi) cam_mul[0] = -1;	/* use my auto white balance */
			} else if (!cam_mul[0]) {
				if (get2() == key[0])		/* Pro1, G6, S60, S70 */
					c = (strstr(model, "Pro1") ?
						"012346000000000000" : "01345:000000006008")[wbi] - '0' + 2;
				else {				/* G3, G5, S45, S50 */
					c = "023457000000006000"[wbi] - '0';
					key[0] = key[1] = 0;
				}
				fseek(ifp, 78 + c * 8, SEEK_CUR);
				FORC4 cam_mul[c ^ (c >> 1) ^ 1] = get2() ^ key[c & 1];
				if (!wbi) cam_mul[0] = -1;
			}
		}
		if (type == 0x10a9) {		/* D60, 10D, 300D, and clones */
			if (len > 66) wbi = "0134567028"[wbi] - '0';
			fseek(ifp, 2 + wbi * 8, SEEK_CUR);
			FORC4 cam_mul[c ^ (c >> 1)] = get2();
		}
		if (type == 0x1030 && (0x18040 >> wbi & 1))
			ciff_block_1030();		/* all that don't have 0x10a9 */
		if (type == 0x1031) {
			raw_width = (get2(), get2());
			raw_height = get2();
		}
		if (type == 0x5029) {
			focal_len = len >> 16;
			if ((len & 0xffff) == 2) focal_len /= 32;
		}
		if (type == 0x5813) flash_used = int_to_float(len);
		if (type == 0x5814) canon_ev = int_to_float(len);
		if (type == 0x5817) shot_order = len;
		if (type == 0x5834) unique_id = len;
		if (type == 0x580e) timestamp = len;
		if (type == 0x180e) timestamp = get4();
		fseek(ifp, save, SEEK_SET);
	}
}

int CLASS parse_jpeg(int offset)
{
	int len, save, hlen, mark;

	fseek(ifp, offset, SEEK_SET);
	if (fgetc(ifp) != 0xff || fgetc(ifp) != 0xd8) return 0;

	while (fgetc(ifp) == 0xff && (mark = fgetc(ifp)) != 0xda) {
		order = 0x4d4d;
		len = get2() - 2;
		save = ftell(ifp);
		if (mark == 0xc0 || mark == 0xc3 || mark == 0xc9) {
			fgetc(ifp);
			raw_height = get2();
			raw_width = get2();
		}
		order = get2();
		hlen = get4();
		if (get4() == 0x48454150)		/* "HEAP" */
			parse_ciff(save + hlen, len - hlen, 0);
		if (parse_tiff(save + 6)) apply_tiff();
		fseek(ifp, save + len, SEEK_SET);
	}
	return 1;
}

void CLASS parse_riff()
{
	unsigned i, size, end;
	char tag[4], date[64], month[64];
	static const char mon[12][4] =
	{ "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
	struct tm t;

	order = 0x4949;
	fread(tag, 4, 1, ifp);
	size = get4();
	end = ftell(ifp) + size;
	if (!memcmp(tag, "RIFF", 4) || !memcmp(tag, "LIST", 4)) {
		get4();
		while (ftell(ifp) + 7 < end && !feof(ifp))
			parse_riff();
	} else if (!memcmp(tag, "nctg", 4)) {
		while (ftell(ifp) + 7 < end) {
			i = get2();
			size = get2();
			if ((i + 1) >> 1 == 10 && size == 20)
				get_timestamp(0);
			else fseek(ifp, size, SEEK_CUR);
		}
	} else if (!memcmp(tag, "IDIT", 4) && size < 64) {
		fread(date, 64, 1, ifp);
		date[size] = 0;
		memset(&t, 0, sizeof t);
		if (sscanf(date, "%*s %s %d %d:%d:%d %d", month, &t.tm_mday,
			&t.tm_hour, &t.tm_min, &t.tm_sec, &t.tm_year) == 6) {
			for (i = 0; i < 12 && strcasecmp(mon[i], month); i++);
			t.tm_mon = i;
			t.tm_year -= 1900;
			if (mktime(&t) > 0)
				timestamp = mktime(&t);
		}
	} else
		fseek(ifp, size, SEEK_CUR);
}

void CLASS parse_qt(int end)
{
	unsigned save, size;
	char tag[4];

	order = 0x4d4d;
	while (ftell(ifp) + 7 < end) {
		save = ftell(ifp);
		if ((size = get4()) < 8) return;
		fread(tag, 4, 1, ifp);
		if (!memcmp(tag, "moov", 4) ||
			!memcmp(tag, "udta", 4) ||
			!memcmp(tag, "CNTH", 4))
			parse_qt(save + size);
		if (!memcmp(tag, "CNDA", 4))
			parse_jpeg(ftell(ifp));
		fseek(ifp, save + size, SEEK_SET);
	}
}

void CLASS parse_redcine()
{
	unsigned i, len, rdvo;

	order = 0x4d4d;
	is_raw = 0;
	fseek(ifp, 52, SEEK_SET);
	width = get4();
	height = get4();
	fseek(ifp, 0, SEEK_END);
	fseek(ifp, -(i = ftello(ifp) & 511), SEEK_CUR);
	if (get4() != i || get4() != 0x52454f42) {
		fprintf(stderr, _("%s: Tail is missing, parsing from head...\n"), ifname);
		fseek(ifp, 0, SEEK_SET);
		while ((len = get4()) != EOF) {
			if (get4() == 0x52454456)
				if (is_raw++ == shot_select)
					data_offset = ftello(ifp) - 8;
			fseek(ifp, len - 8, SEEK_CUR);
		}
	} else {
		rdvo = get4();
		fseek(ifp, 12, SEEK_CUR);
		is_raw = get4();
		fseeko(ifp, rdvo + 8 + shot_select * 4, SEEK_SET);
		data_offset = get4();
	}
}

char * CLASS foveon_gets(int offset, char *str, int len)
{
	int i;
	fseek(ifp, offset, SEEK_SET);
	for (i = 0; i < len - 1; i++)
		if ((str[i] = get2()) == 0) break;
	str[i] = 0;
	return str;
}

/*
   All matrices are from Adobe DNG Converter unless otherwise noted.
 */
void CLASS adobe_coeff(const char *make, const char *model)
{
	static const struct {
		const char *prefix;
		short black, maximum, trans[12];
	} table[] = {
	  { "Olympus AIR A01", 0, 0,
	  { 8992,-3093,-639,-2563,10721,2122,-437,1270,5473 } },
	  { "Olympus C5050", 0, 0,
	  { 10508,-3124,-1273,-6079,14294,1901,-1653,2306,6237 } },
	  { "Olympus C5060", 0, 0,
	  { 10445,-3362,-1307,-7662,15690,2058,-1135,1176,7602 } },
	  { "Olympus C7070", 0, 0,
	  { 10252,-3531,-1095,-7114,14850,2436,-1451,1723,6365 } },
	  { "Olympus C70", 0, 0,
	  { 10793,-3791,-1146,-7498,15177,2488,-1390,1577,7321 } },
	  { "Olympus C80", 0, 0,
	  { 8606,-2509,-1014,-8238,15714,2703,-942,979,7760 } },
	  { "Olympus E-10", 0, 0xffc,
	  { 12745,-4500,-1416,-6062,14542,1580,-1934,2256,6603 } },
	  { "Olympus E-1", 0, 0,
	  { 11846,-4767,-945,-7027,15878,1089,-2699,4122,8311 } },
	  { "Olympus E-20", 0, 0xffc,
	  { 13173,-4732,-1499,-5807,14036,1895,-2045,2452,7142 } },
	  { "Olympus E-300", 0, 0,
	  { 7828,-1761,-348,-5788,14071,1830,-2853,4518,6557 } },
	  { "Olympus E-330", 0, 0,
	  { 8961,-2473,-1084,-7979,15990,2067,-2319,3035,8249 } },
	  { "Olympus E-30", 0, 0xfbc,
	  { 8144,-1861,-1111,-7763,15894,1929,-1865,2542,7607 } },
	  { "Olympus E-3", 0, 0xf99,
	  { 9487,-2875,-1115,-7533,15606,2010,-1618,2100,7389 } },
	  { "Olympus E-400", 0, 0,
	  { 6169,-1483,-21,-7107,14761,2536,-2904,3580,8568 } },
	  { "Olympus E-410", 0, 0xf6a,
	  { 8856,-2582,-1026,-7761,15766,2082,-2009,2575,7469 } },
	  { "Olympus E-420", 0, 0xfd7,
	  { 8746,-2425,-1095,-7594,15612,2073,-1780,2309,7416 } },
	  { "Olympus E-450", 0, 0xfd2,
	  { 8745,-2425,-1095,-7594,15613,2073,-1780,2309,7416 } },
	  { "Olympus E-500", 0, 0,
	  { 8136,-1968,-299,-5481,13742,1871,-2556,4205,6630 } },
	  { "Olympus E-510", 0, 0xf6a,
	  { 8785,-2529,-1033,-7639,15624,2112,-1783,2300,7817 } },
	  { "Olympus E-520", 0, 0xfd2,
	  { 8344,-2322,-1020,-7596,15635,2048,-1748,2269,7287 } },
	  { "Olympus E-5", 0, 0xeec,
	  { 11200,-3783,-1325,-4576,12593,2206,-695,1742,7504 } },
	  { "Olympus E-600", 0, 0xfaf,
	  { 8453,-2198,-1092,-7609,15681,2008,-1725,2337,7824 } },
	  { "Olympus E-620", 0, 0xfaf,
	  { 8453,-2198,-1092,-7609,15681,2008,-1725,2337,7824 } },
	  { "Olympus E-P1", 0, 0xffd,
	  { 8343,-2050,-1021,-7715,15705,2103,-1831,2380,8235 } },
	  { "Olympus E-P2", 0, 0xffd,
	  { 8343,-2050,-1021,-7715,15705,2103,-1831,2380,8235 } },
	  { "Olympus E-P3", 0, 0,
	  { 7575,-2159,-571,-3722,11341,2725,-1434,2819,6271 } },
	  { "Olympus E-P5", 0, 0,
	  { 8380,-2630,-639,-2887,10725,2496,-627,1427,5438 } },
	  { "Olympus E-PL1s", 0, 0,
	  { 11409,-3872,-1393,-4572,12757,2003,-709,1810,7415 } },
	  { "Olympus E-PL1", 0, 0,
	  { 11408,-4289,-1215,-4286,12385,2118,-387,1467,7787 } },
	  { "Olympus E-PL2", 0, 0xcf3,
	  { 15030,-5552,-1806,-3987,12387,1767,-592,1670,7023 } },
	  { "Olympus E-PL3", 0, 0,
	  { 7575,-2159,-571,-3722,11341,2725,-1434,2819,6271 } },
	  { "Olympus E-PL5", 0, 0xfcb,
	  { 8380,-2630,-639,-2887,10725,2496,-627,1427,5438 } },
	  { "Olympus E-PL6", 0, 0,
	  { 8380,-2630,-639,-2887,10725,2496,-627,1427,5438 } },
	  { "Olympus E-PL7", 0, 0,
	  { 9197,-3190,-659,-2606,10830,2039,-458,1250,5458 } },
	  { "Olympus E-PM1", 0, 0,
	  { 7575,-2159,-571,-3722,11341,2725,-1434,2819,6271 } },
	  { "Olympus E-PM2", 0, 0,
	  { 8380,-2630,-639,-2887,10725,2496,-627,1427,5438 } },
	  { "Olympus E-M10", 0, 0,	/* also E-M10 Mark II */
	  { 8380,-2630,-639,-2887,10725,2496,-627,1427,5438 } },
	  { "Olympus E-M1", 0, 0,
	  { 7687,-1984,-606,-4327,11928,2721,-1381,2339,6452 } },
	  { "Olympus E-M5MarkII", 0, 0,
	  { 9422,-3258,-711,-2655,10898,2015,-512,1354,5512 } },
	  { "Olympus E-M5", 0, 0xfe1,
	  { 8380,-2630,-639,-2887,10725,2496,-627,1427,5438 } },
	  { "Olympus PEN-F", 0, 0,
	  { 9476,-3182,-765,-2613,10958,1893,-449,1315,5268 } },
	  { "Olympus SH-2", 0, 0,
	  { 10156,-3425,-1077,-2611,11177,1624,-385,1592,5080 } },
	  { "Olympus SP350", 0, 0,
	  { 12078,-4836,-1069,-6671,14306,2578,-786,939,7418 } },
	  { "Olympus SP3", 0, 0,
	  { 11766,-4445,-1067,-6901,14421,2707,-1029,1217,7572 } },
	  { "Olympus SP500UZ", 0, 0xfff,
	  { 9493,-3415,-666,-5211,12334,3260,-1548,2262,6482 } },
	  { "Olympus SP510UZ", 0, 0xffe,
	  { 10593,-3607,-1010,-5881,13127,3084,-1200,1805,6721 } },
	  { "Olympus SP550UZ", 0, 0xffe,
	  { 11597,-4006,-1049,-5432,12799,2957,-1029,1750,6516 } },
	  { "Olympus SP560UZ", 0, 0xff9,
	  { 10915,-3677,-982,-5587,12986,2911,-1168,1968,6223 } },
	  { "Olympus SP570UZ", 0, 0,
	  { 11522,-4044,-1146,-4736,12172,2904,-988,1829,6039 } },
	  { "Olympus STYLUS1", 0, 0,
	  { 8360,-2420,-880,-3928,12353,1739,-1381,2416,5173 } },
	  { "Olympus TG-4", 0, 0,
	  { 11426,-4159,-1126,-2066,10678,1593,-120,1327,4998 } },
	  { "Olympus XZ-10", 0, 0,
	  { 9777,-3483,-925,-2886,11297,1800,-602,1663,5134 } },
	  { "Olympus XZ-1", 0, 0,
	  { 10901,-4095,-1074,-1141,9208,2293,-62,1417,5158 } },
	  { "Olympus XZ-2", 0, 0,
	  { 9777,-3483,-925,-2886,11297,1800,-602,1663,5134 } },
	};
	double cam_xyz[4][3];
	char name[130];
	int i, j;

	sprintf(name, "%s %s", make, model);
	for (i = 0; i < sizeof table / sizeof *table; i++)
		if (!strncmp(name, table[i].prefix, strlen(table[i].prefix))) {
			if (table[i].black)   black = (ushort)table[i].black;
			if (table[i].maximum) maximum = (ushort)table[i].maximum;
			if (table[i].trans[0]) {
				for (raw_color = j = 0; j < 12; j++)
					((double *)cam_xyz)[j] = table[i].trans[j] / 10000.0;
				cam_xyz_coeff(rgb_cam, cam_xyz);
			}
			break;
		}
}

void CLASS simple_coeff(int index)
{
	static const float table[][12] = {
		/* index 0 -- all Foveon cameras */
		{ 1.4032,-0.2231,-0.1016,-0.5263,1.4816,0.017,-0.0112,0.0183,0.9113 },
		/* index 1 -- Kodak DC20 and DC25 */
		{ 2.25,0.75,-1.75,-0.25,-0.25,0.75,0.75,-0.25,-0.25,-1.75,0.75,2.25 },
		/* index 2 -- Logitech Fotoman Pixtura */
		{ 1.893,-0.418,-0.476,-0.495,1.773,-0.278,-1.017,-0.655,2.672 },
		/* index 3 -- Nikon E880, E900, and E990 */
		{ -1.936280,  1.800443, -1.448486,  2.584324,
		   1.405365, -0.524955, -0.289090,  0.408680,
		  -1.204965,  1.082304,  2.941367, -1.818705 }
	};
	int i, c;

	for (raw_color = i = 0; i < 3; i++)
		FORCC rgb_cam[i][c] = table[index][i*colors + c];
}

short CLASS guess_byte_order(int words)
{
	uchar test[4][2];
	int t = 2, msb;
	double diff, sum[2] = { 0,0 };

	fread(test[0], 2, 2, ifp);
	for (words -= 2; words--; ) {
		fread(test[t], 2, 1, ifp);
		for (msb = 0; msb < 2; msb++) {
			diff = (test[t ^ 2][msb] << 8 | test[t ^ 2][!msb])
				- (test[t][msb] << 8 | test[t][!msb]);
			sum[msb] += diff*diff;
		}
		t = (t + 1) & 3;
	}
	return sum[0] < sum[1] ? 0x4d4d : 0x4949;
}

float CLASS find_green(int bps, int bite, int off0, int off1)
{
	UINT64 bitbuf = 0;
	int vbits, col, i, c;
	ushort img[2][2064];
	double sum[] = { 0,0 };

	FORC(2) {
		fseek(ifp, c ? off1 : off0, SEEK_SET);
		for (vbits = col = 0; col < width; col++) {
			for (vbits -= bps; vbits < 0; vbits += bite) {
				bitbuf <<= bite;
				for (i = 0; i < bite; i += 8)
					bitbuf |= (unsigned)(fgetc(ifp) << i);
			}
			img[c][col] = bitbuf << (64 - bps - vbits) >> (64 - bps);
		}
	}
	FORC(width - 1) {
		sum[c & 1] += ABS(img[0][c] - img[1][c + 1]);
		sum[~c & 1] += ABS(img[1][c] - img[0][c + 1]);
	}
	return 100 * log(sum[0] / sum[1]);
}

/*
   Identify which camera created this file, and set global variables
   accordingly.
 */
void CLASS identify()
{
	static const short pana[][6] = {
	  { 3130, 1743,  4,  0, -6,  0 },
	  { 3130, 2055,  4,  0, -6,  0 },
	  { 3130, 2319,  4,  0, -6,  0 },
	  { 3170, 2103, 18,  0,-42, 20 },
	  { 3170, 2367, 18, 13,-42,-21 },
	  { 3177, 2367,  0,  0, -1,  0 },
	  { 3304, 2458,  0,  0, -1,  0 },
	  { 3330, 2463,  9,  0, -5,  0 },
	  { 3330, 2479,  9,  0,-17,  4 },
	  { 3370, 1899, 15,  0,-44, 20 },
	  { 3370, 2235, 15,  0,-44, 20 },
	  { 3370, 2511, 15, 10,-44,-21 },
	  { 3690, 2751,  3,  0, -8, -3 },
	  { 3710, 2751,  0,  0, -3,  0 },
	  { 3724, 2450,  0,  0,  0, -2 },
	  { 3770, 2487, 17,  0,-44, 19 },
	  { 3770, 2799, 17, 15,-44,-19 },
	  { 3880, 2170,  6,  0, -6,  0 },
	  { 4060, 3018,  0,  0,  0, -2 },
	  { 4290, 2391,  3,  0, -8, -1 },
	  { 4330, 2439, 17, 15,-44,-19 },
	  { 4508, 2962,  0,  0, -3, -4 },
	  { 4508, 3330,  0,  0, -3, -6 },
	};
	static const ushort canon[][11] = {
	  { 1944, 1416,   0,  0, 48,  0 },
	  { 2144, 1560,   4,  8, 52,  2, 0, 0, 0, 25 },
	  { 2224, 1456,  48,  6,  0,  2 },
	  { 2376, 1728,  12,  6, 52,  2 },
	  { 2672, 1968,  12,  6, 44,  2 },
	  { 3152, 2068,  64, 12,  0,  0, 16 },
	  { 3160, 2344,  44, 12,  4,  4 },
	  { 3344, 2484,   4,  6, 52,  6 },
	  { 3516, 2328,  42, 14,  0,  0 },
	  { 3596, 2360,  74, 12,  0,  0 },
	  { 3744, 2784,  52, 12,  8, 12 },
	  { 3944, 2622,  30, 18,  6,  2 },
	  { 3948, 2622,  42, 18,  0,  2 },
	  { 3984, 2622,  76, 20,  0,  2, 14 },
	  { 4104, 3048,  48, 12, 24, 12 },
	  { 4116, 2178,   4,  2,  0,  0 },
	  { 4152, 2772, 192, 12,  0,  0 },
	  { 4160, 3124, 104, 11,  8, 65 },
	  { 4176, 3062,  96, 17,  8,  0, 0, 16, 0, 7, 0x49 },
	  { 4192, 3062,  96, 17, 24,  0, 0, 16, 0, 0, 0x49 },
	  { 4312, 2876,  22, 18,  0,  2 },
	  { 4352, 2874,  62, 18,  0,  0 },
	  { 4476, 2954,  90, 34,  0,  0 },
	  { 4480, 3348,  12, 10, 36, 12, 0, 0, 0, 18, 0x49 },
	  { 4480, 3366,  80, 50,  0,  0 },
	  { 4496, 3366,  80, 50, 12,  0 },
	  { 4768, 3516,  96, 16,  0,  0, 0, 16 },
	  { 4832, 3204,  62, 26,  0,  0 },
	  { 4832, 3228,  62, 51,  0,  0 },
	  { 5108, 3349,  98, 13,  0,  0 },
	  { 5120, 3318, 142, 45, 62,  0 },
	  { 5280, 3528,  72, 52,  0,  0 },
	  { 5344, 3516, 142, 51,  0,  0 },
	  { 5344, 3584, 126,100,  0,  2 },
	  { 5360, 3516, 158, 51,  0,  0 },
	  { 5568, 3708,  72, 38,  0,  0 },
	  { 5632, 3710,  96, 17,  0,  0, 0, 16, 0, 0, 0x49 },
	  { 5712, 3774,  62, 20, 10,  2 },
	  { 5792, 3804, 158, 51,  0,  0 },
	  { 5920, 3950, 122, 80,  2,  0 },
	  { 6096, 4056,  72, 34,  0,  0 },
	  { 6288, 4056, 264, 34,  0,  0 },
	  { 8896, 5920, 160, 64,  0,  0 },
	};
	static const struct {
		ushort id;
		char model[20];
	} unique[] = {
	  { 0x168, "EOS 10D" },    { 0x001, "EOS-1D" },
	  { 0x175, "EOS 20D" },    { 0x174, "EOS-1D Mark II" },
	  { 0x234, "EOS 30D" },    { 0x232, "EOS-1D Mark II N" },
	  { 0x190, "EOS 40D" },    { 0x169, "EOS-1D Mark III" },
	  { 0x261, "EOS 50D" },    { 0x281, "EOS-1D Mark IV" },
	  { 0x287, "EOS 60D" },    { 0x167, "EOS-1DS" },
	  { 0x325, "EOS 70D" },
	  { 0x350, "EOS 80D" },    { 0x328, "EOS-1D X Mark II" },
	  { 0x170, "EOS 300D" },   { 0x188, "EOS-1Ds Mark II" },
	  { 0x176, "EOS 450D" },   { 0x215, "EOS-1Ds Mark III" },
	  { 0x189, "EOS 350D" },   { 0x324, "EOS-1D C" },
	  { 0x236, "EOS 400D" },   { 0x269, "EOS-1D X" },
	  { 0x252, "EOS 500D" },   { 0x213, "EOS 5D" },
	  { 0x270, "EOS 550D" },   { 0x218, "EOS 5D Mark II" },
	  { 0x286, "EOS 600D" },   { 0x285, "EOS 5D Mark III" },
	  { 0x301, "EOS 650D" },   { 0x302, "EOS 6D" },
	  { 0x326, "EOS 700D" },   { 0x250, "EOS 7D" },
	  { 0x393, "EOS 750D" },   { 0x289, "EOS 7D Mark II" },
	  { 0x347, "EOS 760D" },
	  { 0x254, "EOS 1000D" },
	  { 0x288, "EOS 1100D" },
	  { 0x327, "EOS 1200D" },  { 0x382, "Canon EOS 5DS" },
	  { 0x404, "EOS 1300D" },  { 0x401, "Canon EOS 5DS R" },
	  { 0x346, "EOS 100D" },
	}, sonique[] = {
	  { 0x002, "DSC-R1" },     { 0x100, "DSLR-A100" },
	  { 0x101, "DSLR-A900" },  { 0x102, "DSLR-A700" },
	  { 0x103, "DSLR-A200" },  { 0x104, "DSLR-A350" },
	  { 0x105, "DSLR-A300" },  { 0x108, "DSLR-A330" },
	  { 0x109, "DSLR-A230" },  { 0x10a, "DSLR-A290" },
	  { 0x10d, "DSLR-A850" },  { 0x111, "DSLR-A550" },
	  { 0x112, "DSLR-A500" },  { 0x113, "DSLR-A450" },
	  { 0x116, "NEX-5" },      { 0x117, "NEX-3" },
	  { 0x118, "SLT-A33" },    { 0x119, "SLT-A55V" },
	  { 0x11a, "DSLR-A560" },  { 0x11b, "DSLR-A580" },
	  { 0x11c, "NEX-C3" },     { 0x11d, "SLT-A35" },
	  { 0x11e, "SLT-A65V" },   { 0x11f, "SLT-A77V" },
	  { 0x120, "NEX-5N" },     { 0x121, "NEX-7" },
	  { 0x123, "SLT-A37" },    { 0x124, "SLT-A57" },
	  { 0x125, "NEX-F3" },     { 0x126, "SLT-A99V" },
	  { 0x127, "NEX-6" },      { 0x128, "NEX-5R" },
	  { 0x129, "DSC-RX100" },  { 0x12a, "DSC-RX1" },
	  { 0x12e, "ILCE-3000" },  { 0x12f, "SLT-A58" },
	  { 0x131, "NEX-3N" },     { 0x132, "ILCE-7" },
	  { 0x133, "NEX-5T" },     { 0x134, "DSC-RX100M2" },
	  { 0x135, "DSC-RX10" },   { 0x136, "DSC-RX1R" },
	  { 0x137, "ILCE-7R" },    { 0x138, "ILCE-6000" },
	  { 0x139, "ILCE-5000" },  { 0x13d, "DSC-RX100M3" },
	  { 0x13e, "ILCE-7S" },    { 0x13f, "ILCA-77M2" },
	  { 0x153, "ILCE-5100" },  { 0x154, "ILCE-7M2" },
	  { 0x155, "DSC-RX100M4" },{ 0x156, "DSC-RX10M2" },
	  { 0x158, "DSC-RX1RM2" }, { 0x15a, "ILCE-QX1" },
	  { 0x15b, "ILCE-7RM2" },  { 0x15e, "ILCE-7SM2" },
	  { 0x161, "ILCA-68" },    { 0x165, "ILCE-6300" },
	};
	static const struct {
		unsigned fsize;
		ushort rw, rh;
		uchar lm, tm, rm, bm, lf, cf, max, flags;
		char make[10], model[20];
		ushort offset;
	} table[] = {
	  {   786432,1024, 768, 0, 0, 0, 0, 0,0x94,0,0,"AVT","F-080C" },
	  {  1447680,1392,1040, 0, 0, 0, 0, 0,0x94,0,0,"AVT","F-145C" },
	  {  1920000,1600,1200, 0, 0, 0, 0, 0,0x94,0,0,"AVT","F-201C" },
	  {  5067304,2588,1958, 0, 0, 0, 0, 0,0x94,0,0,"AVT","F-510C" },
	  {  5067316,2588,1958, 0, 0, 0, 0, 0,0x94,0,0,"AVT","F-510C",12 },
	  { 10134608,2588,1958, 0, 0, 0, 0, 9,0x94,0,0,"AVT","F-510C" },
	  { 10134620,2588,1958, 0, 0, 0, 0, 9,0x94,0,0,"AVT","F-510C",12 },
	  { 16157136,3272,2469, 0, 0, 0, 0, 9,0x94,0,0,"AVT","F-810C" },
	  { 15980544,3264,2448, 0, 0, 0, 0, 8,0x61,0,1,"AgfaPhoto","DC-833m" },
	  {  9631728,2532,1902, 0, 0, 0, 0,96,0x61,0,0,"Alcatel","5035D" },
	  {  2868726,1384,1036, 0, 0, 0, 0,64,0x49,0,8,"Baumer","TXG14",1078 },
	  {  5298000,2400,1766,12,12,44, 2,40,0x94,0,2,"Canon","PowerShot SD300" },
	  {  6553440,2664,1968, 4, 4,44, 4,40,0x94,0,2,"Canon","PowerShot A460" },
	  {  6573120,2672,1968,12, 8,44, 0,40,0x94,0,2,"Canon","PowerShot A610" },
	  {  6653280,2672,1992,10, 6,42, 2,40,0x94,0,2,"Canon","PowerShot A530" },
	  {  7710960,2888,2136,44, 8, 4, 0,40,0x94,0,2,"Canon","PowerShot S3 IS" },
	  {  9219600,3152,2340,36,12, 4, 0,40,0x94,0,2,"Canon","PowerShot A620" },
	  {  9243240,3152,2346,12, 7,44,13,40,0x49,0,2,"Canon","PowerShot A470" },
	  { 10341600,3336,2480, 6, 5,32, 3,40,0x94,0,2,"Canon","PowerShot A720 IS" },
	  { 10383120,3344,2484,12, 6,44, 6,40,0x94,0,2,"Canon","PowerShot A630" },
	  { 12945240,3736,2772,12, 6,52, 6,40,0x94,0,2,"Canon","PowerShot A640" },
	  { 15636240,4104,3048,48,12,24,12,40,0x94,0,2,"Canon","PowerShot A650" },
	  { 15467760,3720,2772, 6,12,30, 0,40,0x94,0,2,"Canon","PowerShot SX110 IS" },
	  { 15534576,3728,2778,12, 9,44, 9,40,0x94,0,2,"Canon","PowerShot SX120 IS" },
	  { 18653760,4080,3048,24,12,24,12,40,0x94,0,2,"Canon","PowerShot SX20 IS" },
	  { 19131120,4168,3060,92,16, 4, 1,40,0x94,0,2,"Canon","PowerShot SX220 HS" },
	  { 21936096,4464,3276,25,10,73,12,40,0x16,0,2,"Canon","PowerShot SX30 IS" },
	  { 24724224,4704,3504, 8,16,56, 8,40,0x94,0,2,"Canon","PowerShot A3300 IS" },
	  { 30858240,5248,3920, 8,16,56,16,40,0x94,0,2,"Canon","IXUS 160" },
	  {  1976352,1632,1211, 0, 2, 0, 1, 0,0x94,0,1,"Casio","QV-2000UX" },
	  {  3217760,2080,1547, 0, 0,10, 1, 0,0x94,0,1,"Casio","QV-3*00EX" },
	  {  6218368,2585,1924, 0, 0, 9, 0, 0,0x94,0,1,"Casio","QV-5700" },
	  {  7816704,2867,2181, 0, 0,34,36, 0,0x16,0,1,"Casio","EX-Z60" },
	  {  2937856,1621,1208, 0, 0, 1, 0, 0,0x94,7,13,"Casio","EX-S20" },
	  {  4948608,2090,1578, 0, 0,32,34, 0,0x94,7,1,"Casio","EX-S100" },
	  {  6054400,2346,1720, 2, 0,32, 0, 0,0x94,7,1,"Casio","QV-R41" },
	  {  7426656,2568,1928, 0, 0, 0, 0, 0,0x94,0,1,"Casio","EX-P505" },
	  {  7530816,2602,1929, 0, 0,22, 0, 0,0x94,7,1,"Casio","QV-R51" },
	  {  7542528,2602,1932, 0, 0,32, 0, 0,0x94,7,1,"Casio","EX-Z50" },
	  {  7562048,2602,1937, 0, 0,25, 0, 0,0x16,7,1,"Casio","EX-Z500" },
	  {  7753344,2602,1986, 0, 0,32,26, 0,0x94,7,1,"Casio","EX-Z55" },
	  {  9313536,2858,2172, 0, 0,14,30, 0,0x94,7,1,"Casio","EX-P600" },
	  { 10834368,3114,2319, 0, 0,27, 0, 0,0x94,0,1,"Casio","EX-Z750" },
	  { 10843712,3114,2321, 0, 0,25, 0, 0,0x94,0,1,"Casio","EX-Z75" },
	  { 10979200,3114,2350, 0, 0,32,32, 0,0x94,7,1,"Casio","EX-P700" },
	  { 12310144,3285,2498, 0, 0, 6,30, 0,0x94,0,1,"Casio","EX-Z850" },
	  { 12489984,3328,2502, 0, 0,47,35, 0,0x94,0,1,"Casio","EX-Z8" },
	  { 15499264,3754,2752, 0, 0,82, 0, 0,0x94,0,1,"Casio","EX-Z1050" },
	  { 18702336,4096,3044, 0, 0,24, 0,80,0x94,7,1,"Casio","EX-ZR100" },
	  {  7684000,2260,1700, 0, 0, 0, 0,13,0x94,0,1,"Casio","QV-4000" },
	  {   787456,1024, 769, 0, 1, 0, 0, 0,0x49,0,0,"Creative","PC-CAM 600" },
	  { 28829184,4384,3288, 0, 0, 0, 0,36,0x61,0,0,"DJI" },
	  { 15151104,4608,3288, 0, 0, 0, 0, 0,0x94,0,0,"Matrix" },
	  {  3840000,1600,1200, 0, 0, 0, 0,65,0x49,0,0,"Foculus","531C" },
	  {   307200, 640, 480, 0, 0, 0, 0, 0,0x94,0,0,"Generic" },
	  {    62464, 256, 244, 1, 1, 6, 1, 0,0x8d,0,0,"Kodak","DC20" },
	  {   124928, 512, 244, 1, 1,10, 1, 0,0x8d,0,0,"Kodak","DC20" },
	  {  1652736,1536,1076, 0,52, 0, 0, 0,0x61,0,0,"Kodak","DCS200" },
	  {  4159302,2338,1779, 1,33, 1, 2, 0,0x94,0,0,"Kodak","C330" },
	  {  4162462,2338,1779, 1,33, 1, 2, 0,0x94,0,0,"Kodak","C330",3160 },
	  {  2247168,1232, 912, 0, 0,16, 0, 0,0x00,0,0,"Kodak","C330" },
	  {  3370752,1232, 912, 0, 0,16, 0, 0,0x00,0,0,"Kodak","C330" },
	  {  6163328,2864,2152, 0, 0, 0, 0, 0,0x94,0,0,"Kodak","C603" },
	  {  6166488,2864,2152, 0, 0, 0, 0, 0,0x94,0,0,"Kodak","C603",3160 },
	  {   460800, 640, 480, 0, 0, 0, 0, 0,0x00,0,0,"Kodak","C603" },
	  {  9116448,2848,2134, 0, 0, 0, 0, 0,0x00,0,0,"Kodak","C603" },
	  { 12241200,4040,3030, 2, 0, 0,13, 0,0x49,0,0,"Kodak","12MP" },
	  { 12272756,4040,3030, 2, 0, 0,13, 0,0x49,0,0,"Kodak","12MP",31556 },
	  { 18000000,4000,3000, 0, 0, 0, 0, 0,0x00,0,0,"Kodak","12MP" },
	  {   614400, 640, 480, 0, 3, 0, 0,64,0x94,0,0,"Kodak","KAI-0340" },
	  { 15360000,3200,2400, 0, 0, 0, 0,96,0x16,0,0,"Lenovo","A820" },
	  {  3884928,1608,1207, 0, 0, 0, 0,96,0x16,0,0,"Micron","2010",3212 },
	  {  1138688,1534, 986, 0, 0, 0, 0, 0,0x61,0,0,"Minolta","RD175",513 },
	  {  1581060,1305, 969, 0, 0,18, 6, 6,0x1e,4,1,"Nikon","E900" },
	  {  2465792,1638,1204, 0, 0,22, 1, 6,0x4b,5,1,"Nikon","E950" },
	  {  2940928,1616,1213, 0, 0, 0, 7,30,0x94,0,1,"Nikon","E2100" },
	  {  4771840,2064,1541, 0, 0, 0, 1, 6,0xe1,0,1,"Nikon","E990" },
	  {  4775936,2064,1542, 0, 0, 0, 0,30,0x94,0,1,"Nikon","E3700" },
	  {  5865472,2288,1709, 0, 0, 0, 1, 6,0xb4,0,1,"Nikon","E4500" },
	  {  5869568,2288,1710, 0, 0, 0, 0, 6,0x16,0,1,"Nikon","E4300" },
	  {  7438336,2576,1925, 0, 0, 0, 1, 6,0xb4,0,1,"Nikon","E5000" },
	  {  8998912,2832,2118, 0, 0, 0, 0,30,0x94,7,1,"Nikon","COOLPIX S6" },
	  {  5939200,2304,1718, 0, 0, 0, 0,30,0x16,0,0,"Olympus","C770UZ" },
	  {  3178560,2064,1540, 0, 0, 0, 0, 0,0x94,0,1,"Pentax","Optio S" },
	  {  4841984,2090,1544, 0, 0,22, 0, 0,0x94,7,1,"Pentax","Optio S" },
	  {  6114240,2346,1737, 0, 0,22, 0, 0,0x94,7,1,"Pentax","Optio S4" },
	  { 10702848,3072,2322, 0, 0, 0,21,30,0x94,0,1,"Pentax","Optio 750Z" },
	  {  4147200,1920,1080, 0, 0, 0, 0, 0,0x49,0,0,"Photron","BC2-HD" },
	  {  4151666,1920,1080, 0, 0, 0, 0, 0,0x49,0,0,"Photron","BC2-HD",8 },
	  { 13248000,2208,3000, 0, 0, 0, 0,13,0x61,0,0,"Pixelink","A782" },
	  {  6291456,2048,1536, 0, 0, 0, 0,96,0x61,0,0,"RoverShot","3320AF" },
	  {   311696, 644, 484, 0, 0, 0, 0, 0,0x16,0,8,"ST Micro","STV680 VGA" },
	  { 16098048,3288,2448, 0, 0,24, 0, 9,0x94,0,1,"Samsung","S85" },
	  { 16215552,3312,2448, 0, 0,48, 0, 9,0x94,0,1,"Samsung","S85" },
	  { 20487168,3648,2808, 0, 0, 0, 0,13,0x94,5,1,"Samsung","WB550" },
	  { 24000000,4000,3000, 0, 0, 0, 0,13,0x94,5,1,"Samsung","WB550" },
	  { 12582980,3072,2048, 0, 0, 0, 0,33,0x61,0,0,"Sinar","",68 },
	  { 33292868,4080,4080, 0, 0, 0, 0,33,0x61,0,0,"Sinar","",68 },
	  { 44390468,4080,5440, 0, 0, 0, 0,33,0x61,0,0,"Sinar","",68 },
	  {  1409024,1376,1024, 0, 0, 1, 0, 0,0x49,0,0,"Sony","XCD-SX910CR" },
	  {  2818048,1376,1024, 0, 0, 1, 0,97,0x49,0,0,"Sony","XCD-SX910CR" },
	};
	char head[32], *cp;
	int hlen, flen, fsize, zero_fsize = 1, i, c;
	struct jhead jh;

	tiff_flip = flip = filters = UINT_MAX;	/* unknown */
	raw_height = raw_width = cr2_slice[0] = 0;
	maximum = height = width = top_margin = left_margin = 0;
	cdesc[0] = desc[0] = artist[0] = model[0] = model2[0] = 0;
	iso_speed = shutter = aperture = focal_len = unique_id = 0;
	tiff_nifds = 0;
	memset(tiff_ifd, 0, sizeof tiff_ifd);
	memset(gpsdata, 0, sizeof gpsdata);
	memset(cblack, 0, sizeof cblack);
	memset(white, 0, sizeof white);
	memset(mask, 0, sizeof mask);
	thumb_offset = thumb_length = thumb_width = thumb_height = 0;
	load_raw = thumb_load_raw = 0;
//	write_thumb = &CLASS jpeg_thumb;
	data_offset = meta_offset = meta_length = tiff_bps = tiff_compress = 0;
	zero_after_ff = dng_version = load_flags = 0;
	timestamp = shot_order = tiff_samples = black = is_foveon = 0;
	mix_green = profile_length = data_error = zero_is_bad = 0;
	pixel_aspect = is_raw = raw_color = 1;
	tile_width = tile_length = 0;
	for (i = 0; i < 4; i++) {
		cam_mul[i] = i == 1;
		pre_mul[i] = i < 3;
		FORC3 cmatrix[c][i] = 0;
		FORC3 rgb_cam[c][i] = c == i;
	}
	colors = 3;
	for (i = 0; i < 0x10000; i++) curve[i] = i;

	order = get2();
	hlen = get4();
	fseek(ifp, 0, SEEK_SET);
	fread(head, 1, 32, ifp);
	fseek(ifp, 0, SEEK_END);
	flen = fsize = ftell(ifp);
	if (order == 0x4949 || order == 0x4d4d) {
		if (parse_tiff(0)) apply_tiff();
	} else if (!memcmp(head, "\xff\xd8\xff\xe1", 4) &&
		!memcmp(head + 6, "Exif", 4)) {
		fseek(ifp, 4, SEEK_SET);
		data_offset = 4 + get2();
		fseek(ifp, data_offset, SEEK_SET);
		if (fgetc(ifp) != 0xff)
			parse_tiff(12);
		thumb_offset = 0;
	} else if (!memcmp(head, "RIFF", 4)) {
		fseek(ifp, 0, SEEK_SET);
		parse_riff();
	} else if (!memcmp(head + 4, "ftypqt   ", 9)) {
		fseek(ifp, 0, SEEK_SET);
		parse_qt(fsize);
		is_raw = 0;
	}
	if (zero_fsize)
		fsize = 0;
	desc[511] = artist[63] = model[63] = model2[63] = 0;

	if (!height) height = raw_height;
	if (!width)  width = raw_width;
	if (height == 2624 && width == 3936)	/* Pentax K10D and Samsung GX10 */
	{
		height = 2616;   width = 3896;
	}
	if (height == 3136 && width == 4864)  /* Pentax K20D and Samsung GX20 */
	{
		height = 3124;   width = 4688; filters = 0x16161616;
	}
	if (width == 4352 && (!strcmp(model, "K-r") || !strcmp(model, "K-x")))
	{
		width = 4309; filters = 0x16161616;
	}
	if (width >= 4960 && !strncmp(model, "K-5", 3))
	{
		left_margin = 10; width = 4950; filters = 0x16161616;
	}
	if (width == 4736 && !strcmp(model, "K-7"))
	{
		height = 3122;   width = 4684; filters = 0x16161616; top_margin = 2;
	}
	if (width == 6080 && !strcmp(model, "K-3"))
	{
		left_margin = 4;  width = 6040;
	}
	if (width == 7424 && !strcmp(model, "645D"))
	{
		height = 5502;   width = 7328; filters = 0x61616161; top_margin = 29;
		left_margin = 48;
	}
	if (height == 3014 && width == 4096)	/* Ricoh GX200 */
		width = 4014;
	if (dng_version) {
		if (filters == UINT_MAX) filters = 0;
		if (filters) is_raw *= tiff_samples;
		else	 colors = tiff_samples;
		switch (tiff_compress) {
		case 0:
		case 1:     load_raw = &CLASS   packed_dng_load_raw;  break;
		case 7:     load_raw = &CLASS lossless_dng_load_raw;  break;
		case 34892: load_raw = &CLASS    lossy_dng_load_raw;  break;
		default:    load_raw = 0;
		}
		goto dng_skip;
	}
	for (i = 0; i < sizeof unique / sizeof *unique; i++)
		if (unique_id == 0x80000000 + unique[i].id) {
			adobe_coeff("Canon", unique[i].model);
			if (model[4] == 'K' && strlen(model) == 8)
				strcpy(model, unique[i].model);
		}
	for (i = 0; i < sizeof sonique / sizeof *sonique; i++)
		if (unique_id == sonique[i].id)
			strcpy(model, sonique[i].model);

	/* Set parameters based on camera name (for non-DNG files). */

	if (!strcmp(model, "KAI-0340")
		&& find_green(16, 16, 3840, 5120) < 25) {
		height = 480;
		top_margin = filters = 0;
		strcpy(model, "C603");
	}
	if (is_foveon) {
		if (height * 2 < width) pixel_aspect = 0.5;
		if (height > width) pixel_aspect = 2;
		filters = 0;
		simple_coeff(0);
	} else if (!strcmp(model, "PowerShot A5") ||
		!strcmp(model, "PowerShot A5 Zoom")) {
		height = 773;
		width = 960;
		raw_width = 992;
		pixel_aspect = 256 / 235.0;
		filters = 0x1e4e1e4e;
		goto canon_a5;
	} else if (!strcmp(model, "PowerShot A50")) {
		height = 968;
		width = 1290;
		raw_width = 1320;
		filters = 0x1b4e4b1e;
		goto canon_a5;
	} else if (!strcmp(model, "PowerShot Pro70")) {
		height = 1024;
		width = 1552;
		filters = 0x1e4b4e1b;
	canon_a5:
		colors = 4;
		tiff_bps = 10;
		load_raw = &CLASS packed_load_raw;
		load_flags = 40;
	} else if (!strcmp(model, "PowerShot Pro90 IS") ||
		!strcmp(model, "PowerShot G1")) {
		colors = 4;
		filters = 0xb4b4b4b4;
	} else if (!strcmp(model, "PowerShot SX220 HS")) {
		mask[1][3] = -4;
	} else if (!strcmp(model, "EOS D2000C")) {
		filters = 0x61616161;
		black = curve[200];
	} else if (!strcmp(model, "D1")) {
		cam_mul[0] *= 256 / 527.0;
		cam_mul[2] *= 256 / 317.0;
	} else if (!strcmp(model, "D1X")) {
		width -= 4;
		pixel_aspect = 0.5;
	} else if (!strcmp(model, "D40X") ||
		!strcmp(model, "D60") ||
		!strcmp(model, "D80") ||
		!strcmp(model, "D3000")) {
		height -= 3;
		width -= 4;
	} else if (!strcmp(model, "D3") ||
		!strcmp(model, "D3S") ||
		!strcmp(model, "D700")) {
		width -= 4;
		left_margin = 2;
	} else if (!strcmp(model, "D3100")) {
		width -= 28;
		left_margin = 6;
	} else if (!strcmp(model, "D5000") ||
		!strcmp(model, "D90")) {
		width -= 42;
	} else if (!strcmp(model, "D5100") ||
		!strcmp(model, "D7000") ||
		!strcmp(model, "COOLPIX A")) {
		width -= 44;
	} else if (!strcmp(model, "D3200") ||
		!strncmp(model, "D6", 2) ||
		!strncmp(model, "D800", 4)) {
		width -= 46;
	} else if (!strcmp(model, "D4") ||
		!strcmp(model, "Df")) {
		width -= 52;
		left_margin = 2;
	} else if (!strncmp(model, "D40", 3) ||
		!strncmp(model, "D50", 3) ||
		!strncmp(model, "D70", 3)) {
		width--;
	} else if (!strcmp(model, "D100")) {
		if (load_flags)
			raw_width = (width += 3) + 3;
	} else if (!strcmp(model, "D200")) {
		left_margin = 1;
		width -= 4;
		filters = 0x94949494;
	} else if (!strncmp(model, "D2H", 3)) {
		left_margin = 6;
		width -= 14;
	} else if (!strncmp(model, "D2X", 3)) {
		if (width == 3264) width -= 32;
		else width -= 8;
	} else if (!strncmp(model, "D300", 4)) {
		width -= 32;
	} else if (!strncmp(model, "COOLPIX P", 9) && raw_width != 4032) {
		load_flags = 24;
		filters = 0x94949494;
		if (model[9] == '7' && iso_speed >= 400)
			black = 255;
	} else if (!strncmp(model, "1 ", 2)) {
		height -= 2;
	} else if (fsize == 1581060) {
		simple_coeff(3);
		pre_mul[0] = 1.2085;
		pre_mul[1] = 1.0943;
		pre_mul[3] = 1.1103;
	} else if (fsize == 3178560) {
		cam_mul[0] *= 4;
		cam_mul[2] *= 4;
	} else if (fsize == 4771840) {
		if (strcmp(model, "E995")) {
			filters = 0xb4b4b4b4;
			simple_coeff(3);
			pre_mul[0] = 1.196;
			pre_mul[1] = 1.246;
			pre_mul[2] = 1.018;
		}
	} else if (fsize == 2940928) {
		if (!strcmp(model, "E2500")) {
			height -= 2;
			load_flags = 6;
			colors = 4;
			filters = 0x4b4b4b4b;
		}
	} else if (!strcmp(model, "*ist D")) {
		load_raw = &CLASS unpacked_load_raw;
		data_error = -1;
	} else if (!strcmp(model, "*ist DS")) {
		height -= 2;
	} else if (!strcmp(model, "EX1")) {
		order = 0x4949;
		height -= 20;
		top_margin = 2;
		if ((width -= 6) > 3682) {
			height -= 10;
			width -= 46;
			top_margin = 8;
		}
	} else if (!strcmp(model, "WB2000")) {
		order = 0x4949;
		height -= 3;
		top_margin = 2;
		if ((width -= 10) > 3718) {
			height -= 28;
			width -= 56;
			top_margin = 8;
		}
	} else if (strstr(model, "WB550")) {
		strcpy(model, "WB550");
	} else if (!strcmp(model, "EX2F")) {
		height = 3045;
		width = 4070;
		top_margin = 3;
		order = 0x4949;
		filters = 0x49494949;
		load_raw = &CLASS unpacked_load_raw;
	} else if (!strcmp(model, "STV680 VGA")) {
		black = 16;
	} else if (!strcmp(model, "N95")) {
		height = raw_height - (top_margin = 2);
	} else if (!strcmp(model, "640x480")) {
		gamma_curve(0.45, 4.5, 1, 255);
	} else {//if (!strcmp(make, "Olympus")) {
		height += height & 1;
		if (exif_cfa) filters = exif_cfa;
		if (width == 4100) width -= 4;
		if (width == 4080) width -= 24;
		if (width == 9280) { width -= 6; height -= 6; }
		if (load_raw == &CLASS unpacked_load_raw)
			load_flags = 4;
		tiff_bps = 12;
		if (!strcmp(model, "E-300") ||
			!strcmp(model, "E-500")) {
			width -= 20;
			if (load_raw == &CLASS unpacked_load_raw) {
				maximum = 0xfc3;
				memset(cblack, 0, sizeof cblack);
			}
		} else if (!strcmp(model, "E-330")) {
			width -= 30;
			if (load_raw == &CLASS unpacked_load_raw)
				maximum = 0xf79;
		} else if (!strcmp(model, "SP550UZ")) {
			thumb_length = flen - (thumb_offset = 0xa39800);
			thumb_height = 480;
			thumb_width = 640;
		} else if (!strcmp(model, "TG-4")) {
			width -= 16;
		}
	}
	if (!model[0])
		sprintf(model, "%dx%d", width, height);
	if (filters == UINT_MAX) filters = 0x94949494;
	if (thumb_offset && !thumb_height) {
		fseek(ifp, thumb_offset, SEEK_SET);
		if (ljpeg_start(&jh, 1)) {
			thumb_width = jh.wide;
			thumb_height = jh.high;
		}
	}
dng_skip:
	if ((use_camera_matrix & (use_camera_wb || dng_version))
		&& cmatrix[0][0] > 0.125) {
		memcpy(rgb_cam, cmatrix, sizeof cmatrix);
		raw_color = 0;
	}
	if (raw_color) adobe_coeff("Olympus", model);
	if (raw_height < height) raw_height = height;
	if (raw_width < width) raw_width = width;
	if (!tiff_bps) tiff_bps = 12;
	if (!maximum) maximum = (1 << tiff_bps) - 1;
	if (!load_raw || height < 22 || width < 22 ||
		tiff_bps > 16 || tiff_samples > 6 || colors > 4)
		is_raw = 0;
	if (load_raw == &CLASS redcine_load_raw) {
		fprintf(stderr, _("%s: You must link dcraw with %s!!\n"),
			ifname, "libjasper");
		is_raw = 0;
	}
	if (load_raw == &CLASS lossy_dng_load_raw) {
		fprintf(stderr, _("%s: You must link dcraw with %s!!\n"),
			ifname, "libjpeg");
		is_raw = 0;
	}
	if (!cdesc[0])
		strcpy(cdesc, colors == 3 ? "RGBG" : "GMCY");
	if (!raw_height) raw_height = height;
	if (!raw_width) raw_width = width;
	if (filters > 999 && colors == 3)
		filters |= ((filters >> 2 & 0x22222222) |
		(filters << 2 & 0x88888888)) & filters << 1;
notraw:
	if (flip == UINT_MAX) flip = tiff_flip;
	if (flip == UINT_MAX) flip = 0;
}

void CLASS convert_to_rgb()
{
	int row, col, c, i, j, k;
	ushort *img;
	float out[3], out_cam[3][4];
	double num, inverse[3][3];
	static const double xyzd50_srgb[3][3] =
	{ { 0.436083, 0.385083, 0.143055 },
	  { 0.222507, 0.716888, 0.060608 },
	  { 0.013930, 0.097097, 0.714022 } };
	static const double rgb_rgb[3][3] =
	{ { 1,0,0 }, { 0,1,0 }, { 0,0,1 } };
	static const double adobe_rgb[3][3] =
	{ { 0.715146, 0.284856, 0.000000 },
	  { 0.000000, 1.000000, 0.000000 },
	  { 0.000000, 0.041166, 0.958839 } };
	static const double wide_rgb[3][3] =
	{ { 0.593087, 0.404710, 0.002206 },
	  { 0.095413, 0.843149, 0.061439 },
	  { 0.011621, 0.069091, 0.919288 } };
	static const double prophoto_rgb[3][3] =
	{ { 0.529317, 0.330092, 0.140588 },
	  { 0.098368, 0.873465, 0.028169 },
	  { 0.016879, 0.117663, 0.865457 } };
	static const double aces_rgb[3][3] =
	{ { 0.432996, 0.375380, 0.189317 },
	  { 0.089427, 0.816523, 0.102989 },
	  { 0.019165, 0.118150, 0.941914 } };
	static const double(*out_rgb[])[3] =
	{ rgb_rgb, adobe_rgb, wide_rgb, prophoto_rgb, xyz_rgb, aces_rgb };
	static const char *name[] =
	{ "sRGB", "Adobe RGB (1998)", "WideGamut D65", "ProPhoto D65", "XYZ", "ACES" };
	static const unsigned phead[] =
	{ 1024, 0, 0x2100000, 0x6d6e7472, 0x52474220, 0x58595a20, 0, 0, 0,
	  0x61637370, 0, 0, 0x6e6f6e65, 0, 0, 0, 0, 0xf6d6, 0x10000, 0xd32d };
	unsigned pbody[] =
	{ 10, 0x63707274, 0, 36,	/* cprt */
	  0x64657363, 0, 40,	/* desc */
	  0x77747074, 0, 20,	/* wtpt */
	  0x626b7074, 0, 20,	/* bkpt */
	  0x72545243, 0, 14,	/* rTRC */
	  0x67545243, 0, 14,	/* gTRC */
	  0x62545243, 0, 14,	/* bTRC */
	  0x7258595a, 0, 20,	/* rXYZ */
	  0x6758595a, 0, 20,	/* gXYZ */
	  0x6258595a, 0, 20 };	/* bXYZ */
	static const unsigned pwhite[] = { 0xf351, 0x10000, 0x116cc };
	unsigned pcurve[] = { 0x63757276, 0, 1, 0x1000000 };

	gamma_curve(gamm[0], gamm[1], 0, 0);
	memcpy(out_cam, rgb_cam, sizeof out_cam);
	raw_color |= colors == 1 || document_mode ||
		output_color < 1 || output_color > 6;
	if (!raw_color) {
		oprof = (unsigned *)calloc(phead[0], 1);
		merror(oprof, "convert_to_rgb()");
		memcpy(oprof, phead, sizeof phead);
		if (output_color == 5) oprof[4] = oprof[5];
		oprof[0] = 132 + 12 * pbody[0];
		for (i = 0; i < pbody[0]; i++) {
			oprof[oprof[0] / 4] = i ? (i > 1 ? 0x58595a20 : 0x64657363) : 0x74657874;
			pbody[i * 3 + 2] = oprof[0];
			oprof[0] += (pbody[i * 3 + 3] + 3) & -4;
		}
		memcpy(oprof + 32, pbody, sizeof pbody);
		oprof[pbody[5] / 4 + 2] = strlen(name[output_color - 1]) + 1;
		memcpy((char *)oprof + pbody[8] + 8, pwhite, sizeof pwhite);
		pcurve[3] = (short)(256 / gamm[5] + 0.5) << 16;
		for (i = 4; i < 7; i++)
			memcpy((char *)oprof + pbody[i * 3 + 2], pcurve, sizeof pcurve);
		pseudoinverse((double(*)[3]) out_rgb[output_color - 1], inverse, 3);
		for (i = 0; i < 3; i++)
			for (j = 0; j < 3; j++) {
				for (num = k = 0; k < 3; k++)
					num += xyzd50_srgb[i][k] * inverse[j][k];
				oprof[pbody[j * 3 + 23] / 4 + i + 2] = num * 0x10000 + 0.5;
			}
		for (i = 0; i < phead[0] / 4; i++)
			oprof[i] = htonl(oprof[i]);
		strcpy((char *)oprof + pbody[2] + 8, "auto-generated by dcraw");
		strcpy((char *)oprof + pbody[5] + 12, name[output_color - 1]);
		for (i = 0; i < 3; i++)
			for (j = 0; j < colors; j++)
				for (out_cam[i][j] = k = 0; k < 3; k++)
					out_cam[i][j] += out_rgb[output_color - 1][i][k] * rgb_cam[k][j];
	}
	if (verbose)
		fprintf(stderr, raw_color ? _("Building histograms...\n") :
			_("Converting to %s colorspace...\n"), name[output_color - 1]);

	memset(histogram, 0, sizeof histogram);
	for (img = image[0], row = 0; row < height; row++)
		for (col = 0; col < width; col++, img += 4) {
			if (!raw_color) {
				out[0] = out[1] = out[2] = 0;
				FORCC{
				  out[0] += out_cam[0][c] * img[c];
				  out[1] += out_cam[1][c] * img[c];
				  out[2] += out_cam[2][c] * img[c];
				}
				FORC3 img[c] = CLIP((int)out[c]);
			} else if (document_mode)
				img[0] = img[fcol(row, col)];
			FORCC histogram[c][img[c] >> 3]++;
		}
	if (colors == 4 && output_color) colors = 3;
	if (document_mode && filters) colors = 1;
}

void CLASS stretch()
{
	ushort newdim, (*img)[4], *pix0, *pix1;
	int row, col, c;
	double rc, frac;

	if (pixel_aspect == 1) return;
	if (verbose) fprintf(stderr, _("Stretching the image...\n"));
	if (pixel_aspect < 1) {
		newdim = height / pixel_aspect + 0.5;
		img = (ushort(*)[4]) calloc(width, newdim * sizeof *img);
		merror(img, "stretch()");
		for (rc = row = 0; row < newdim; row++, rc += pixel_aspect) {
			frac = rc - (c = rc);
			pix0 = pix1 = image[c*width];
			if (c + 1 < height) pix1 += width * 4;
			for (col = 0; col < width; col++, pix0 += 4, pix1 += 4)
				FORCC img[row*width + col][c] = pix0[c] * (1 - frac) + pix1[c] * frac + 0.5;
		}
		height = newdim;
	} else {
		newdim = width * pixel_aspect + 0.5;
		img = (ushort(*)[4]) calloc(height, newdim * sizeof *img);
		merror(img, "stretch()");
		for (rc = col = 0; col < newdim; col++, rc += 1 / pixel_aspect) {
			frac = rc - (c = rc);
			pix0 = pix1 = image[c];
			if (c + 1 < width) pix1 += 4;
			for (row = 0; row < height; row++, pix0 += width * 4, pix1 += width * 4)
				FORCC img[row*newdim + col][c] = pix0[c] * (1 - frac) + pix1[c] * frac + 0.5;
		}
		width = newdim;
	}
	free(image);
	image = img;
}

int CLASS flip_index(int row, int col)
{
	if (flip & 4) SWAP(row, col);
	if (flip & 2) row = iheight - 1 - row;
	if (flip & 1) col = iwidth - 1 - col;
	return row * iwidth + col;
}

struct tiff_tag {
	ushort tag, type;
	int count;
	union { char c[4]; short s[2]; int i; } val;
};

struct tiff_hdr {
	ushort order, magic;
	int ifd;
	ushort pad, ntag;
	struct tiff_tag tag[23];
	int nextifd;
	ushort pad2, nexif;
	struct tiff_tag exif[4];
	ushort pad3, ngps;
	struct tiff_tag gpst[10];
	short bps[4];
	int rat[10];
	unsigned gps[26];
	char desc[512], make[64], model[64], soft[32], date[20], artist[64];
};

void CLASS tiff_set(struct tiff_hdr *th, ushort *ntag,
	ushort tag, ushort type, int count, int val)
{
	struct tiff_tag *tt;
	int c;

	tt = (struct tiff_tag *)(ntag + 1) + (*ntag)++;
	tt->val.i = val;
	if (type == 1 && count <= 4)
		FORC(4) tt->val.c[c] = val >> (c << 3);
	else if (type == 2) {
		count = strnlen((char *)th + val, count - 1) + 1;
		if (count <= 4)
			FORC(4) tt->val.c[c] = ((char *)th)[val + c];
	} else if (type == 3 && count <= 2)
		FORC(2) tt->val.s[c] = val >> (c << 4);
	tt->count = count;
	tt->type = type;
	tt->tag = tag;
}

#define TOFF(ptr) ((char *)(&(ptr)) - (char *)th)

void CLASS write_ppm_tiff()
{
	struct tiff_hdr th;
	uchar *ppm;
	ushort *ppm2;
	int c, row, col, soff, rstep, cstep;
	int perc, val, total, white = 0x2000;

	perc = width * height * 0.01;		/* 99th percentile white level */
	if (!((highlight & ~2) || no_auto_bright))
		for (white = c = 0; c < colors; c++) {
			for (val = 0x2000, total = 0; --val > 32; )
				if ((total += histogram[c][val]) > perc) break;
			if (white < val) white = val;
		}
	gamma_curve(gamm[0], gamm[1], 2, (white << 3) / bright);
	iheight = height;
	iwidth = width;
	if (flip & 4) SWAP(height, width);
	ppm = (uchar *)calloc(width, colors*output_bps / 8);
	ppm2 = (ushort *)ppm;
	merror(ppm, "write_ppm_tiff()");
	if (colors > 3)
		fprintf(ofp,
			"P7\nWIDTH %d\nHEIGHT %d\nDEPTH %d\nMAXVAL %d\nTUPLTYPE %s\nENDHDR\n",
			width, height, colors, (1 << output_bps) - 1, cdesc);
	else
		fprintf(ofp, "P%d\n%d %d\n%d\n",
			colors / 2 + 5, width, height, (1 << output_bps) - 1);
	soff = flip_index(0, 0);
	cstep = flip_index(0, 1) - soff;
	rstep = flip_index(1, 0) - flip_index(0, width);
	for (row = 0; row < height; row++, soff += rstep) {
		for (col = 0; col < width; col++, soff += cstep)
			if (output_bps == 8)
				FORCC ppm[col*colors + c] = curve[image[soff][c]] >> 8;
			else FORCC ppm2[col*colors + c] = curve[image[soff][c]];
			if (output_bps == 16 && !output_tiff && htons(0x55aa) != 0x55aa)
				swab((char *)ppm2, (char *)ppm2, width*colors * 2);
			fwrite(ppm, colors*output_bps / 8, width, ofp);
	}
	free(ppm);
}

int CLASS main(int argc, const char **argv)
{
	int arg, status = 0, quality, i, c;
	int timestamp_only = 0, thumbnail_only = 0, identify_only = 0;
	int user_qual = -1, user_black = -1, user_sat = -1, user_flip = -1;
	int use_fuji_rotate = 1, write_to_stdout = 0, read_from_stdin = 0;
	const char *sp, *bpfile = 0, *dark_frame = 0, *write_ext;
	char opm, opt, *ofname, *cp;
	struct utimbuf ut;

	putenv((char *) "TZ=UTC");

	if (argc == 1) {
		printf(_("\nRaw photo decoder \"dcraw\" v%s"), DCRAW_VERSION);
		printf(_("\nby Dave Coffin, dcoffin a cybercom o net\n"));
		printf(_("\nUsage:  %s [OPTION]... [FILE]...\n\n"), argv[0]);
		puts(_("-v        Print verbose messages"));
		puts(_("-c        Write image data to standard output"));
		puts(_("-e        Extract embedded thumbnail image"));
		puts(_("-i        Identify files without decoding them"));
		puts(_("-i -v     Identify files and show metadata"));
		puts(_("-z        Change file dates to camera timestamp"));
		puts(_("-w        Use camera white balance, if possible"));
		puts(_("-a        Average the whole image for white balance"));
		puts(_("-A <x y w h> Average a grey box for white balance"));
		puts(_("-r <r g b g> Set custom white balance"));
		puts(_("+M/-M     Use/don't use an embedded color matrix"));
		puts(_("-C <r b>  Correct chromatic aberration"));
		puts(_("-P <file> Fix the dead pixels listed in this file"));
		puts(_("-K <file> Subtract dark frame (16-bit raw PGM)"));
		puts(_("-k <num>  Set the darkness level"));
		puts(_("-S <num>  Set the saturation level"));
		puts(_("-n <num>  Set threshold for wavelet denoising"));
		puts(_("-H [0-9]  Highlight mode (0=clip, 1=unclip, 2=blend, 3+=rebuild)"));
		puts(_("-t [0-7]  Flip image (0=none, 3=180, 5=90CCW, 6=90CW)"));
		puts(_("-o [0-6]  Output colorspace (raw,sRGB,Adobe,Wide,ProPhoto,XYZ,ACES)"));
		puts(_("-d        Document mode (no color, no interpolation)"));
		puts(_("-D        Document mode without scaling (totally raw)"));
		puts(_("-j        Don't stretch or rotate raw pixels"));
		puts(_("-W        Don't automatically brighten the image"));
		puts(_("-b <num>  Adjust brightness (default = 1.0)"));
		puts(_("-g <p ts> Set custom gamma curve (default = 2.222 4.5)"));
		puts(_("-q [0-3]  Set the interpolation quality"));
		puts(_("-h        Half-size color image (twice as fast as \"-q 0\")"));
		puts(_("-f        Interpolate RGGB as four colors"));
		puts(_("-m <num>  Apply a 3x3 median filter to R-G and B-G"));
		puts(_("-s [0..N-1] Select one raw image or \"all\" from each file"));
		puts(_("-6        Write 16-bit instead of 8-bit"));
		puts(_("-4        Linear 16-bit, same as \"-6 -W -g 1 1\""));
		puts(_("-T        Write TIFF instead of PPM"));
		puts("");
		return 1;
	}
	argv[argc] = "";
	for (arg = 1; (((opm = argv[arg][0]) - 2) | 2) == '+'; ) {
		opt = argv[arg++][1];
		if ((cp = (char *)strchr(sp = "nbrkStqmHACg", opt)))
			for (i = 0; i < "114111111422"[cp - sp] - '0'; i++)
				if (!isdigit(argv[arg + i][0])) {
					fprintf(stderr, _("Non-numeric argument to \"-%c\"\n"), opt);
					return 1;
				}
		switch (opt) {
		case 'n':  threshold = atof(argv[arg++]);  break;
		case 'b':  bright = atof(argv[arg++]);  break;
		case 'r':
			FORC4 user_mul[c] = atof(argv[arg++]);  break;
		case 'C':  aber[0] = 1 / atof(argv[arg++]);
			aber[2] = 1 / atof(argv[arg++]);  break;
		case 'g':  gamm[0] = atof(argv[arg++]);
			gamm[1] = atof(argv[arg++]);
			if (gamm[0]) gamm[0] = 1 / gamm[0]; break;
		case 'k':  user_black = atoi(argv[arg++]);  break;
		case 'S':  user_sat = atoi(argv[arg++]);  break;
		case 't':  user_flip = atoi(argv[arg++]);  break;
		case 'q':  user_qual = atoi(argv[arg++]);  break;
		case 'm':  med_passes = atoi(argv[arg++]);  break;
		case 'H':  highlight = atoi(argv[arg++]);  break;
		case 's':
			shot_select = abs(atoi(argv[arg]));
			multi_out = !strcmp(argv[arg++], "all");
			break;
		case 'o':
			if (isdigit(argv[arg][0]) && !argv[arg][1])
				output_color = atoi(argv[arg++]);
			break;
		case 'P':  bpfile = argv[arg++];  break;
		case 'K':  dark_frame = argv[arg++];  break;
		case 'z':  timestamp_only = 1;  break;
		case 'e':  thumbnail_only = 1;  break;
		case 'i':  identify_only = 1;  break;
		case 'c':  write_to_stdout = 1;  break;
		case 'v':  verbose = 1;  break;
		case 'h':  half_size = 1;  break;
		case 'f':  four_color_rgb = 1;  break;
		case 'A':  FORC4 greybox[c] = atoi(argv[arg++]);
		case 'a':  use_auto_wb = 1;  break;
		case 'w':  use_camera_wb = 1;  break;
		case 'M':  use_camera_matrix = 3 * (opm == '+');  break;
		case 'I':  read_from_stdin = 1;  break;
		case 'E':  document_mode++;
		case 'D':  document_mode++;
		case 'd':  document_mode++;
		case 'j':  use_fuji_rotate = 0;  break;
		case 'W':  no_auto_bright = 1;  break;
		case 'T':  output_tiff = 1;  break;
		case '4':  gamm[0] = gamm[1] =
			no_auto_bright = 1;
		case '6':  output_bps = 16;  break;
		default:
			fprintf(stderr, _("Unknown option \"-%c\".\n"), opt);
			return 1;
		}
	}
	if (arg == argc) {
		fprintf(stderr, _("No files to process.\n"));
		return 1;
	}
	if (write_to_stdout) {
		if (isatty(1)) {
			fprintf(stderr, _("Will not write an image to the terminal!\n"));
			return 1;
		}
		if (setmode(1, O_BINARY) < 0) {
			perror("setmode()");
			return 1;
		}
	}
	for (; arg < argc; arg++) {
		status = 1;
		raw_image = 0;
		image = 0;
		oprof = 0;
		meta_data = ofname = 0;
		ofp = stdout;
		if (setjmp(failure)) {
			if (fileno(ifp) > 2) fclose(ifp);
			if (fileno(ofp) > 2) fclose(ofp);
			status = 1;
			goto cleanup;
		}
		ifname = argv[arg];
		if (!(ifp = fopen(ifname, "rb"))) {
			perror(ifname);
			continue;
		}
		status = (identify(), !is_raw);
		if (user_flip >= 0)
			flip = user_flip;
		switch ((flip + 3600) % 360) {
		case 270:  flip = 5;  break;
		case 180:  flip = 3;  break;
		case  90:  flip = 6;
		}
		if (timestamp_only) {
			if ((status = !timestamp))
				fprintf(stderr, _("%s has no timestamp.\n"), ifname);
			else if (identify_only)
				printf("%10ld%10d %s\n", (long)timestamp, shot_order, ifname);
			else {
				if (verbose)
					fprintf(stderr, _("%s time set to %d.\n"), ifname, (int)timestamp);
				ut.actime = ut.modtime = timestamp;
				utime(ifname, &ut);
			}
			goto next;
		}
		write_fun = &CLASS write_ppm_tiff;
		if (thumbnail_only) {
			if ((status = !thumb_offset)) {
				fprintf(stderr, _("%s has no thumbnail.\n"), ifname);
				goto next;
			} else if (thumb_load_raw) {
				load_raw = thumb_load_raw;
				data_offset = thumb_offset;
				height = thumb_height;
				width = thumb_width;
				filters = 0;
				colors = 3;
			}
		}
		if (!is_raw)
			fprintf(stderr, _("Cannot decode file %s\n"), ifname);
		if (!is_raw) goto next;
		shrink = filters && (half_size || (!identify_only &&
			(threshold || aber[0] != 1 || aber[2] != 1)));
		iheight = (height + shrink) >> shrink;
		iwidth = (width + shrink) >> shrink;
		if (identify_only) {
			if (verbose) {
				if (document_mode == 3) {
					top_margin = left_margin = 0;
					height = raw_height;
					width = raw_width;
				}
				iheight = (height + shrink) >> shrink;
				iwidth = (width + shrink) >> shrink;
				if (flip & 4)
					SWAP(iheight, iwidth);
				printf(_("Image size:  %4d x %d\n"), width, height);
				printf(_("Output size: %4d x %d\n"), iwidth, iheight);
				printf(_("Raw colors: %d"), colors);
				if (filters) {
					int fhigh = 2, fwide = 2;
					if ((filters ^ (filters >> 8)) & 0xff)   fhigh = 4;
					if ((filters ^ (filters >> 16)) & 0xffff) fhigh = 8;
					if (filters == 1) fhigh = fwide = 16;
					if (filters == 9) fhigh = fwide = 6;
					printf(_("\nFilter pattern: "));
					for (i = 0; i < fhigh; i++)
						for (c = i && putchar('/') && 0; c < fwide; c++)
							putchar(cdesc[fcol(i, c)]);
				}
				printf(_("\nDaylight multipliers:"));
				FORCC printf(" %f", pre_mul[c]);
				if (cam_mul[0] > 0) {
					printf(_("\nCamera multipliers:"));
					FORC4 printf(" %f", cam_mul[c]);
				}
				putchar('\n');
			} else
				printf(_("%s is a %s %s image.\n"), ifname, "Olympus", model);
		next:
			fclose(ifp);
			continue;
		}
		if (meta_length) {
			meta_data = (char *)malloc(meta_length);
			merror(meta_data, "main()");
		}
		if (filters || colors == 1) {
			raw_image = (ushort *)calloc((raw_height + 7), raw_width * 2);
			merror(raw_image, "main()");
		} else {
			image = (ushort(*)[4]) calloc(iheight, iwidth * sizeof *image);
			merror(image, "main()");
		}
		if (shot_select >= is_raw)
			fprintf(stderr, _("%s: \"-s %d\" requests a nonexistent image!\n"),
				ifname, shot_select);
		fseeko(ifp, data_offset, SEEK_SET);
		if (raw_image && read_from_stdin)
			fread(raw_image, 2, raw_height*raw_width, stdin);
		else (*load_raw)();
		if (document_mode == 3) {
			top_margin = left_margin = 0;
			height = raw_height;
			width = raw_width;
		}
		iheight = (height + shrink) >> shrink;
		iwidth = (width + shrink) >> shrink;
		if (raw_image) {
			image = (ushort(*)[4]) calloc(iheight, iwidth * sizeof *image);
			merror(image, "main()");
			crop_masked_pixels();
			free(raw_image);
		}
		if (zero_is_bad) remove_zeroes();
		bad_pixels(bpfile);
		if (dark_frame) subtract(dark_frame);
		quality = 2; //+ !fuji_width;
		if (user_qual >= 0) quality = user_qual;
		i = cblack[3];
		FORC3 if (i > cblack[c]) i = cblack[c];
		FORC4 cblack[c] -= i;
		black += i;
		i = cblack[6];
		FORC(cblack[4] * cblack[5])
			if (i > cblack[6 + c]) i = cblack[6 + c];
		FORC(cblack[4] * cblack[5])
			cblack[6 + c] -= i;
		black += i;
		if (user_black >= 0) black = user_black;
		FORC4 cblack[c] += black;
		if (user_sat > 0) maximum = user_sat;
		if (is_foveon) {
			if (document_mode || load_raw == &CLASS foveon_dp_load_raw) {
				for (i = 0; i < height*width * 4; i++)
					if ((short)image[0][i] < 0) image[0][i] = 0;
			} else foveon_interpolate();
		} else if (document_mode < 2)
			scale_colors();
		pre_interpolate();
		if (filters && !document_mode) {
			if (quality == 0)
				lin_interpolate();
			else if (quality == 1 || colors > 3)
				vng_interpolate();
			else if (quality == 2 && filters > 1000)
				ppg_interpolate();
			else if (filters == 9)
				xtrans_interpolate(quality * 2 - 3);
			else
				ahd_interpolate();
		}
		if (mix_green)
			for (colors = 3, i = 0; i < height*width; i++)
				image[i][1] = (image[i][1] + image[i][3]) >> 1;
		if (!is_foveon && colors == 3) median_filter();
		if (!is_foveon && highlight == 2) blend_highlights();
		if (!is_foveon && highlight > 2) recover_highlights();
		convert_to_rgb();
	thumbnail:
		if (output_tiff && write_fun == &CLASS write_ppm_tiff)
			write_ext = ".tiff";
		else
			write_ext = ".pgm\0.ppm\0.ppm\0.pam" + colors * 5 - 5;
		ofname = (char *)malloc(strlen(ifname) + 64);
		merror(ofname, "main()");
		if (write_to_stdout)
			strcpy(ofname, _("standard output"));
		else {
			strcpy(ofname, ifname);
			if ((cp = strrchr(ofname, '.'))) *cp = 0;
			if (multi_out)
				sprintf(ofname + strlen(ofname), "_%0*d",
					snprintf(0, 0, "%d", is_raw - 1), shot_select);
			if (thumbnail_only)
				strcat(ofname, ".thumb");
			strcat(ofname, write_ext);
			ofp = fopen(ofname, "wb");
			if (!ofp) {
				status = 1;
				perror(ofname);
				goto cleanup;
			}
		}
		if (verbose)
			fprintf(stderr, _("Writing data to %s ...\n"), ofname);
		(*write_fun)();
		fclose(ifp);
		if (ofp != stdout) fclose(ofp);
	cleanup:
		if (meta_data) free(meta_data);
		if (ofname) free(ofname);
		if (oprof) free(oprof);
		if (image) free(image);
		if (multi_out) {
			if (++shot_select < is_raw) arg--;
			else shot_select = 0;
		}
	}
	return status;
}
