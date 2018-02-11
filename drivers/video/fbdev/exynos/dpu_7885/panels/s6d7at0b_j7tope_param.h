#ifndef __S6D7AT0B_PARAM_H__
#define __S6D7AT0B_PARAM_H__
#include <linux/types.h>
#include <linux/kernel.h>

#define EXTEND_BRIGHTNESS	306
#define UI_MAX_BRIGHTNESS	255
#define UI_DEFAULT_BRIGHTNESS	128

struct lcd_seq_info {
	unsigned char	*cmd;
	unsigned int	len;
	unsigned int	sleep;
};

struct i2c_rom_data {
	u8 addr;
	u8 val;
};

static const struct i2c_rom_data LM3632_INIT[] = {
	{0x09, 0x41},
	{0x02, 0x50},
	{0x03, 0x8D},
	{0x04, 0x05},
	{0x05, 0xCC},
	{0x0A, 0x19},
	{0x0D, 0x1C},
	{0x0E, 0x1E},
	{0x0F, 0x1E},
	{0x0C, 0x1F},
};

/*
 * The applied command [0x9F] level key
 * 01h, 10h, 12h, 13h, 18h, 19h, 20h, 21h, 22h, 23h, 28h,
 * 29h, 30h, 34h, 35h, 36h, 38h, 39h, 3Ah, 44h, 4Fh, 51h.
 */

static const unsigned char SEQ_TEST_KEY_ON_9F[] = {
	0x9F,
	0x5A, 0x5A,
};

static const unsigned char SEQ_TEST_KEY_ON_F0[] = {
	0xF0,
	0x5A, 0x5A,
};

static const unsigned char SEQ_TEST_KEY_ON_FC[] = {
	0xFC,
	0x5A, 0x5A,
};

static const unsigned char SEQ_S6D7AT0B_36[] = {
	0x36,
	0x14
};

static const unsigned char SEQ_S6D7AT0B_71[] = {
	0x71,
	0x08
};

static const unsigned char SEQ_S6D7AT0B_73[] = {
	0x73,
	0xB0, 0x4E, 0x00, 0x01, 0xA4, 0x00,
};

static const unsigned char SEQ_S6D7AT0B_B1[] = {
	0xB1,
	0x11, 0x22, 0x10, 0x01,
};

static const unsigned char SEQ_S6D7AT0B_B3[] = {
	0xB3,
	0x00, 0xB4, 0x07, 0x70, 0x10, 0xB8, 0x02, 0x02, 0x06, 0x40,
	0x00, 0xED, 0x00, 0xED, 0x00, 0xED,
};

static const unsigned char SEQ_S6D7AT0B_B6[] = {
	0xB6,
	0x80, 0x00, 0x07, 0x10, 0x00,
};

static const unsigned char SEQ_S6D7AT0B_BB[] = {
	0xBB,
	0x21,
};

static const unsigned char SEQ_S6D7AT0B_BA[] = {
	0xBA,
	0x01, 0x15, 0x12, 0x55, 0x12, 0x55, 0x12, 0x8A, 0x02, 0x58,
	0x08, 0x84, 0x00, 0x03, 0x01, 0x00, 0xC0, 0x00, 0xA0, 0x00,
	0x00, 0x00, 0x01, 0x4E, 0x01, 0x10, 0xC2, 0x89, 0x50, 0x2C,
	0x99, 0x46, 0x46, 0x00, 0x01, 0xA1, 0x56, 0x01, 0x20, 0x28,
	0x28, 0x2A, 0x2B, 0x2F, 0x32, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0x28, 0x30, 0x40, 0x58, 0x84, 0x82,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
	0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const unsigned char SEQ_S6D7AT0B_C8[] = {
	0xC8,
	0x00, 0x24, 0x00, 0x41, 0x3A, 0x00, 0x00, 0x01, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x85, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x40, 0x48,
};

static const unsigned char SEQ_S6D7AT0B_EC[] = {
	0xEC,
	0x00, 0x00, 0x00, 0x00, 0x05, 0x05, 0x08, 0x08, 0x09, 0x09,
	0x0A, 0x0A, 0x06, 0x06, 0x0F, 0x0F, 0x0D, 0x0D, 0x07, 0x07,
	0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08, 0x08,
	0x09, 0x09, 0x0A, 0x0A, 0x06, 0x06, 0x0E, 0x0E, 0x0C, 0x0C,
	0x07, 0x07, 0x10, 0x10,
};

static const unsigned char SEQ_S6D7AT0B_ED[] = {
	0xED,
	0x2D, 0x01, 0x10, 0x05, 0x00, 0x20, 0x00, 0x2E, 0x01, 0x0F,
	0x0A, 0x00, 0x00, 0x00, 0x50, 0x01, 0x14, 0x0F, 0x00, 0x42,
	0x00, 0x43, 0x01, 0x25, 0x20, 0x00, 0x60, 0x20, 0x44, 0x01,
	0x10, 0x10, 0x00, 0xA0, 0x00, 0x45, 0x21, 0x10, 0x10, 0x00,
	0x80, 0xC0, 0x40, 0x41, 0x10, 0x10, 0x22, 0xC8, 0x80, 0x40,
	0x21, 0x10, 0x10, 0x22, 0xE0, 0x20, 0x40, 0x40, 0x80, 0x08,
	0x40, 0x48, 0x80, 0x08, 0x40, 0xF8, 0x00, 0x00, 0x00, 0x00,
};

static const unsigned char SEQ_S6D7AT0B_EE[] = {
	0xEE,
	0x1A, 0x65, 0x03, 0x0E, 0x11, 0x06, 0x00, 0x00, 0x6D, 0x1A,
	0x66, 0x03, 0x0E, 0x11, 0x06, 0x00, 0x00, 0x4E, 0x1A, 0x67,
	0x03, 0x0E, 0x11, 0x06, 0x00, 0x00, 0x2F, 0x1A, 0x68, 0x03,
	0x0E, 0x11, 0x06, 0x00, 0x00, 0x10, 0x19, 0xED, 0x00, 0x00,
	0x00, 0x06, 0x00, 0x00, 0xA8, 0x19, 0xEE, 0x00, 0x00, 0x00,
	0x06, 0x00, 0x00, 0x89, 0x3A, 0x04, 0x0F, 0x5C, 0x00, 0x04,
	0x00, 0x00, 0x34, 0x3A, 0x05, 0x0F, 0x5C, 0x00, 0x04, 0x00,
	0x00, 0x0C, 0x30,
};

static const unsigned char SEQ_S6D7AT0B_EF[] = {
	0xEF,
	0x00, 0x30, 0x30, 0x30, 0x30, 0x0C, 0x40, 0x76, 0x76, 0x10,
	0x13, 0x09, 0x80, 0x00, 0x08, 0x70, 0x00, 0x00, 0x00, 0x48,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x77, 0x00, 0x00, 0x07, 0x02, 0x03, 0x04, 0x05, 0x06,
	0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x07,
	0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const unsigned char SEQ_S6D7AT0B_F2[] = {
	0xF2,
	0x48, 0x22, 0x33, 0x33, 0xE3, 0x44, 0x43, 0x00, 0x01, 0x00,
	0x00, 0x0D, 0x8C, 0x00, 0x80, 0x00, 0xA6, 0x1E, 0x08, 0x03,
	0x34, 0x02, 0x00, 0x00, 0x00, 0x00,
};

static const unsigned char SEQ_S6D7AT0B_B0[] = {
	0xB0,
	0x02,
};

static const unsigned char SEQ_S6D7AT0B_F4[] = {
	0xF4,
	0x10, 0x0A, 0x91, 0xD0, 0xD0, 0x05, 0x0A, 0x02, 0x9D, 0x55,
	0x59, 0x3B, 0x10, 0x55, 0x15, 0x26, 0xFA, 0x00, 0x00, 0x00,
	0x00, 0x00,
};

static const unsigned char SEQ_S6D7AT0B_F9[] = {
	0xF9,
	0x88, 0x88, 0x80, 0xC0, 0xC0, 0x04, 0x7A, 0x00, 0x0D, 0xC0,
	0x94, 0x1C, 0x60, 0xA0, 0x60, 0xA0, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x77, 0x00, 0x00, 0x00,
	0x00,
};

static const unsigned char SEQ_S6D7AT0B_FE[] = {
	0xFE,
	0x02, 0x00, 0x01, 0x4C, 0x00, 0x00, 0x00, 0x60, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const unsigned char SEQ_S6D7AT0B_E8[] = {
	0xE8,
	0x0A, 0x1A, 0x26, 0x21, 0x26, 0x2E, 0x2D, 0x2E, 0x32, 0x35,
	0x36, 0x32, 0x30, 0x2E, 0x2B, 0x2E, 0x39, 0x13, 0x1A, 0x26,
	0x21, 0x26, 0x2E, 0x2D, 0x2E, 0x32, 0x2F, 0x2D, 0x32, 0x30,
	0x2E, 0x2B, 0x2E, 0x3D,
};

static const unsigned char SEQ_S6D7AT0B_E9[] = {
	0xE9,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x1D, 0x00, 0x2C,
	0x00, 0x3D, 0x00, 0x4E, 0x00, 0x5E, 0x00, 0x7E, 0x00, 0x9E,
	0x00, 0xDD, 0x01, 0x1A, 0x01, 0x62, 0x01, 0xDE, 0x02, 0x58,
	0x02, 0xDA, 0x03, 0x1B, 0x03, 0x5D, 0x03, 0x9B, 0x03, 0xBB,
	0x03, 0xDB, 0x03, 0xEB, 0x03, 0xF4, 0x03, 0xF8, 0x03, 0xFC,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x1D, 0x00, 0x2C,
	0x00, 0x3D, 0x00, 0x4E, 0x00, 0x5E, 0x00, 0x7E, 0x00, 0x9E,
	0x00, 0xDD, 0x01, 0x1A, 0x01, 0x62, 0x01, 0xDE, 0x02, 0x58,
	0x02, 0xDA, 0x03, 0x1B, 0x03, 0x5D, 0x03, 0x9B, 0x03, 0xBB,
	0x03, 0xDB, 0x03, 0xEB, 0x03, 0xF4, 0x03, 0xF8, 0x03, 0xFC,
};

static const unsigned char SEQ_S6D7AT0B_EA[] = {
	0xEA,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x1D, 0x00, 0x2C,
	0x00, 0x3D, 0x00, 0x4E, 0x00, 0x5E, 0x00, 0x7E, 0x00, 0x9E,
	0x00, 0xDD, 0x01, 0x1A, 0x01, 0x62, 0x01, 0xDE, 0x02, 0x58,
	0x02, 0xDA, 0x03, 0x1B, 0x03, 0x5D, 0x03, 0x9B, 0x03, 0xBB,
	0x03, 0xDB, 0x03, 0xEB, 0x03, 0xF4, 0x03, 0xF8, 0x03, 0xFC,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x1D, 0x00, 0x2C,
	0x00, 0x3D, 0x00, 0x4E, 0x00, 0x5E, 0x00, 0x7E, 0x00, 0x9E,
	0x00, 0xDD, 0x01, 0x1A, 0x01, 0x62, 0x01, 0xDE, 0x02, 0x58,
	0x02, 0xDA, 0x03, 0x1B, 0x03, 0x5D, 0x03, 0x9B, 0x03, 0xBB,
	0x03, 0xDB, 0x03, 0xEB, 0x03, 0xF4, 0x03, 0xF8, 0x03, 0xFC,
};

static const unsigned char SEQ_S6D7AT0B_EB[] = {
	0xEB,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x1D, 0x00, 0x2C,
	0x00, 0x3D, 0x00, 0x4E, 0x00, 0x5E, 0x00, 0x7E, 0x00, 0x9E,
	0x00, 0xDD, 0x01, 0x1A, 0x01, 0x62, 0x01, 0xDE, 0x02, 0x58,
	0x02, 0xDA, 0x03, 0x1B, 0x03, 0x5D, 0x03, 0x9B, 0x03, 0xBB,
	0x03, 0xDB, 0x03, 0xEB, 0x03, 0xF4, 0x03, 0xF8, 0x03, 0xFC,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x1D, 0x00, 0x2C,
	0x00, 0x3D, 0x00, 0x4E, 0x00, 0x5E, 0x00, 0x7E, 0x00, 0x9E,
	0x00, 0xDD, 0x01, 0x1A, 0x01, 0x62, 0x01, 0xDE, 0x02, 0x58,
	0x02, 0xDA, 0x03, 0x1B, 0x03, 0x5D, 0x03, 0x9B, 0x03, 0xBB,
	0x03, 0xDB, 0x03, 0xEB, 0x03, 0xF4, 0x03, 0xF8, 0x03, 0xFC,
};

static const unsigned char SEQ_S6D7AT0B_51[] = {
	0x51,
	0x75, 0x81,
};

static const unsigned char SEQ_S6D7AT0B_53[] = {
	0x53,
	0x2C,
};

static const unsigned char SEQ_S6D7AT0B_55[] = {
	0x55,
	0x01,
};

static const unsigned char SEQ_S6D7AT0B_C0[] = {
	0xC0,
	0x01, 0x01, 0x51, 0x01, 0x00, 0xFF, 0x00, 0x04, 0x0F, 0xFF,
	0x30, 0x40, 0x08,
};

static const unsigned char SEQ_TEST_KEY_OFF_9F[] = {
	0x9F,
	0xA5, 0xA5,
};

static const unsigned char SEQ_TEST_KEY_OFF_F0[] = {
	0xF0,
	0xA5, 0xA5,
};

static const unsigned char SEQ_TEST_KEY_OFF_FC[] = {
	0xFC,
	0xA5, 0xA5,
};

static const unsigned char SEQ_SLEEP_OUT[] = {
	0x11,
	0x00, 0x00
};

static const unsigned char SEQ_SLEEP_IN[] = {
	0x10,
	0x00, 0x00
};

static const unsigned char SEQ_DISPLAY_OFF[] = {
	0x28,
	0x00, 0x00
};

static const unsigned char SEQ_DISPLAY_ON[] = {
	0x29,
	0x00, 0x00
};

/* platform brightness <-> bl reg */
static unsigned int brightness_table[EXTEND_BRIGHTNESS + 1] = {
	0,
	0,
	2,
	2,
	3,
	3,
	4,
	5,
	6,
	6,
	7,
	7,
	8,
	8,
	9,
	9,
	10,
	10,
	11,
	11,
	12,
	13,
	13,
	14,
	14,
	15,
	15,
	16,
	16,
	17,
	18,
	18,
	19,
	19,
	20,
	20,
	21,
	21,
	22,
	23,
	23,
	24,
	24,
	25,
	25,
	26,
	26,
	27,
	28,
	28,
	29,
	29,
	30,
	30,
	31,
	31,
	32,
	33,
	33,
	34,
	34,
	35,
	35,
	36,
	36,
	37,
	38,
	38,
	39,
	39,
	40,
	40,
	41,
	41,
	42,
	43,
	43,
	44,
	44,
	45,
	45,
	46,
	46,
	47,
	48,
	48,
	49,
	49,
	50,
	50,
	51,
	51,
	52,
	53,
	54,
	55,
	56,
	57,
	58,
	59,
	60,
	61,
	62,
	63,
	64,
	65,
	66,
	67,
	68,
	69,
	70,
	71,
	72,
	73,
	74,
	75,
	76,
	77,
	78,
	79,
	80,
	81,
	82,
	83,
	84,
	85,
	86,
	87,
	88,
	89,
	90,
	91,
	92,
	93,
	94,
	95,
	96,
	97,
	98,
	99,
	100,
	101,
	102,
	103,
	104,
	105,
	106,
	107,
	108,
	109,
	110,
	111,
	112,
	113,
	114,
	115,
	116,
	117,
	118,
	119,
	120,
	121,
	122,
	123,
	124,
	125,
	126,
	127,
	128,
	129,
	130,
	131,
	132,
	133,
	134,
	135,
	136,
	137,
	138,
	139,
	140,
	141,
	142,
	143,
	144,
	145,
	146,
	147,
	148,
	149,
	150,
	150,
	151,
	151,
	152,
	152,
	153,
	153,
	154,
	154,
	155,
	155,
	156,
	156,
	157,
	157,
	158,
	158,
	159,
	159,
	160,
	160,
	161,
	161,
	162,
	163,
	164,
	164,
	165,
	165,
	166,
	166,
	167,
	167,
	168,
	168,
	169,
	169,
	170,
	170,
	171,
	171,
	172,
	172,
	173,
	173,
	174,
	174,
	175,
	175,
	175,
	176,
	176,
	176,
	177,
	177,
	177,
	178,
	178,
	178,
	179,
	179,
	179,
	180,
	180,
	[UI_MAX_BRIGHTNESS ... EXTEND_BRIGHTNESS - 1] = 181,
	[EXTEND_BRIGHTNESS ... EXTEND_BRIGHTNESS] = 225
};

#endif /* __S6D7AT0B_PARAM_H__ */
