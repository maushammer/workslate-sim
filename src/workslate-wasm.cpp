#include <cheerp/clientlib.h>
#include <cheerp/client.h>
#include <cheerpfs.h>
#include "sim6800.h"

static const int lcd_base_x = 54+6;    // upper left corner
static const int lcd_base_y = 83+8;
static const int lcd_base_width = 644;
static const int lcd_base_height = 304;

// The actual hardware is 46 x 16 chars with 6 x 8 characters.  We'll set a transform so we
// can draw with these native units, and let the canvas scale to whatever the background image is.
#define lcd_native_cols 46
#define lcd_native_rows 16
static const int lcd_native_char_width = 6;
static const int lcd_native_char_height = 8;
static const int lcd_native_width = lcd_native_cols * lcd_native_char_width;
static const int lcd_native_height = lcd_native_rows * lcd_native_char_height;


/* bridge to the javascript functions in workslate.html */
namespace [[cheerp::genericjs]] client
{
  int getKeyboardActive(void);
}


/*-- These should be in workslate-wasm.h --*/
#define SIM_CYCLES_PER_FRAME 2000000/2   // 2 MHz clock, simulate 1/60 sec frame
void push_kbd_fifo(uint32_t f);       // from workslate_hw.c
void rtc_update(struct timespec *ts); // from workslate_hw.c

extern unsigned char rom_u14[0x8000]; // not used
extern unsigned char rom_u15[0x8000];
extern unsigned char rom_u16[0x8000]; // boot bank
extern unsigned char resource_u15_bin[];
extern unsigned char resource_u16_bin[];
void set_system_time(uint64_t milliseconds);
void workslate_hw_reset(void);
unsigned char mread(unsigned short addr);

extern FILE *mon_out;
extern FILE *mon_in;

// Font.  LSB=top.   A space before the first tab means it's been visually checked vs. workslate
static uint8_t const lcd_font[128][6] = {
	{ 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa },  // 00   0  
 	{ 0x10, 0x10, 0x10, 0x54, 0x38, 0x10 },  // 01   1  →
	{ 0x00, 0x7F, 0x41, 0x55, 0x41, 0x7F },  // 02   2  ✇ (square tape)
	{ 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa },  // 03   3  
 	{ 0x00, 0x22, 0x14, 0x08, 0x14, 0x22 },  // 04   4  ×
 	{ 0x00, 0x08, 0x08, 0x2A, 0x08, 0x08 },  // 05   5  ÷
 	{ 0x00, 0x54, 0x34, 0x1C, 0x16, 0x15 },  // 06   6  ≠
	{ 0x00, 0x20, 0x38, 0x7C, 0x38, 0x20 },  // 07   7 🔔
	{ 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa },  // 08   8    maybe error symbol? seen in status bar
 	{ 0x3E, 0x41, 0x4F, 0x51, 0x61, 0x3E },  // 09   9  ◷
 	{ 0x00, 0x0F, 0x08, 0xF8, 0x50, 0x10 },  // 0A  10  ␊
 	{ 0x00, 0x08, 0x1C, 0x3E, 0x7F, 0x00 },  // 0B  11  ◀
	{ 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa },  // 0C  12  
 	{ 0x00, 0x0F, 0x09, 0xF9, 0x50, 0xA0 },  // 0D  13  ␍
 	{ 0x00, 0x00, 0xF8, 0xF8, 0x18, 0x18 },  // 0E  14  ┏
 	{ 0x18, 0x18, 0xF8, 0xF8, 0x00, 0x00 },  // 0F  15  ┓
 	{ 0x00, 0x00, 0x1F, 0x1F, 0x18, 0x18 },  // 10  16  ┗
 	{ 0x18, 0x18, 0x1F, 0x1F, 0x00, 0x00 },  // 11  17  ┛
 	{ 0x18, 0x18, 0xFF, 0xFF, 0x18, 0x18 },  // 12  18  ╋
 	{ 0x18, 0x18, 0xF8, 0xF8, 0x18, 0x18 },  // 13  19  ┳
 	{ 0x18, 0x18, 0x1F, 0x1F, 0x18, 0x18 },  // 14  20  ┻
 	{ 0x00, 0x00, 0xFF, 0xFF, 0x18, 0x18 },  // 15  21  ┣
 	{ 0x18, 0x18, 0xFF, 0xFF, 0x00, 0x00 },  // 16  22  ┫
 	{ 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00 },  // 17  23  ┃
 	{ 0x18, 0x18, 0x18, 0x18, 0x18, 0x18 },  // 18  24  ━
	{ 0x14, 0x14, 0x14, 0x14, 0x14, 0x14 },  // 19  25  ═
	{ 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa },  // 1A  26  ▣
	{ 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa },  // 1B  27  
 	{ 0x00, 0x0F, 0x09, 0x19, 0xF0, 0x10 },  // 1C  28  CT
	{ 0xFF, 0xF1, 0xF1, 0xF1, 0xF1, 0xFF },  // 1D  29  ⬓
	{ 0xFF, 0x81, 0x81, 0xFF, 0xFF, 0xFF },  // 1E  30  ◨
	{ 0x7F, 0x41, 0x41, 0x41, 0x41, 0x7F },  // 1F  31  ▢
 	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // 20  32
 	{ 0x00, 0x00, 0x00, 0x4F, 0x00, 0x00 },  // 21  33  !
 	{ 0x00, 0x00, 0x07, 0x00, 0x07, 0x00 },  // 22  34  "
 	{ 0x00, 0x14, 0x7F, 0x14, 0x7F, 0x14 },  // 23  35  #
 	{ 0x00, 0x24, 0x2A, 0x7F, 0x2A, 0x12 },  // 24  36  $
 	{ 0x00, 0x63, 0x13, 0x08, 0x64, 0x63 },  // 25  37  %
 	{ 0x00, 0x36, 0x49, 0x55, 0x22, 0x50 },  // 26  38  &
 	{ 0x00, 0x00, 0x05, 0x03, 0x00, 0x00 },  // 27  39  '
 	{ 0x00, 0x00, 0x1C, 0x22, 0x41, 0x00 },  // 28  40  (
 	{ 0x00, 0x00, 0x41, 0x22, 0x1C, 0x00 },  // 29  41  )
 	{ 0x00, 0x14, 0x08, 0x3E, 0x08, 0x14 },  // 2A  42  *
	{ 0x00, 0x08, 0x08, 0x3E, 0x08, 0x08 },  // 2B  43  +
	{ 0x00, 0x00, 0x50, 0x30, 0x00, 0x00 },  // 2C  44  ,
 	{ 0x00, 0x08, 0x08, 0x08, 0x08, 0x08 },  // 2D  45  -
	{ 0x00, 0x00, 0x60, 0x60, 0x00, 0x00 },  // 2E  46  .
 	{ 0x00, 0x40, 0x20, 0x10, 0x08, 0x04 },  // 2F  47  /
	{ 0x00, 0x3E, 0x51, 0x49, 0x45, 0x3E },  // 30  48  0
 	{ 0x00, 0x00, 0x42, 0x7F, 0x40, 0x00 },  // 31  49  1
 	{ 0x00, 0x42, 0x61, 0x51, 0x49, 0x46 },  // 32  50  2
 	{ 0x00, 0x21, 0x41, 0x45, 0x4B, 0x31 },  // 33  51  3
 	{ 0x00, 0x18, 0x14, 0x12, 0x7F, 0x10 },  // 34  52  4
 	{ 0x00, 0x27, 0x45, 0x45, 0x45, 0x39 },  // 35  53  5
 	{ 0x00, 0x3C, 0x4A, 0x49, 0x49, 0x30 },  // 36  54  6
 	{ 0x00, 0x01, 0x71, 0x09, 0x05, 0x03 },  // 37  55  7
 	{ 0x00, 0x36, 0x49, 0x49, 0x49, 0x36 },  // 38  56  8
 	{ 0x00, 0x06, 0x49, 0x49, 0x29, 0x1E },  // 39  57  9
	{ 0x00, 0x00, 0x36, 0x36, 0x00, 0x00 },  // 3A  58  :
	{ 0x00, 0x00, 0x56, 0x36, 0x00, 0x00 },  // 3B  59  ;
 	{ 0x00, 0x08, 0x14, 0x22, 0x41, 0x00 },  // 3C  60  <
	{ 0x00, 0x14, 0x14, 0x14, 0x14, 0x14 },  // 3D  61  =
 	{ 0x00, 0x00, 0x41, 0x22, 0x14, 0x08 },  // 3E  62  >
 	{ 0x00, 0x02, 0x01, 0x51, 0x09, 0x06 },  // 3F  63  ?
 	{ 0x00, 0x3E, 0x41, 0x5D, 0x55, 0x1E },  // 40  64  @
	{ 0x00, 0x7E, 0x09, 0x09, 0x09, 0x7E },  // 41  65  A
	{ 0x00, 0x7F, 0x49, 0x49, 0x49, 0x36 },  // 42  66  B
	{ 0x00, 0x3E, 0x41, 0x41, 0x41, 0x22 },  // 43  67  C
	{ 0x00, 0x7F, 0x41, 0x41, 0x22, 0x1C },  // 44  68  D
	{ 0x00, 0x7F, 0x49, 0x49, 0x49, 0x41 },  // 45  69  E
	{ 0x00, 0x7F, 0x09, 0x09, 0x09, 0x01 },  // 46  70  F
	{ 0x00, 0x3E, 0x41, 0x49, 0x49, 0x7A },  // 47  71  G
	{ 0x00, 0x7F, 0x08, 0x08, 0x08, 0x7F },  // 48  72  H
	{ 0x00, 0x00, 0x41, 0x7F, 0x41, 0x00 },  // 49  73  I
	{ 0x00, 0x20, 0x40, 0x41, 0x3F, 0x01 },  // 4A  74  J
	{ 0x00, 0x7F, 0x08, 0x14, 0x22, 0x41 },  // 4B  75  K
	{ 0x00, 0x7F, 0x40, 0x40, 0x40, 0x40 },  // 4C  76  L
	{ 0x00, 0x7F, 0x02, 0x0C, 0x02, 0x7F },  // 4D  77  M
	{ 0x00, 0x7F, 0x04, 0x08, 0x10, 0x7F },  // 4E  78  N
	{ 0x00, 0x3E, 0x41, 0x41, 0x41, 0x3E },  // 4F  79  O
	{ 0x00, 0x7F, 0x09, 0x09, 0x09, 0x06 },  // 50  80  P
	{ 0x00, 0x3E, 0x41, 0x51, 0x21, 0x5E },  // 51  81  Q
	{ 0x00, 0x7F, 0x09, 0x19, 0x29, 0x46 },  // 52  82  R
	{ 0x00, 0x46, 0x49, 0x49, 0x49, 0x31 },  // 53  83  S
	{ 0x00, 0x01, 0x01, 0x7F, 0x01, 0x01 },  // 54  84  T
	{ 0x00, 0x3F, 0x40, 0x40, 0x40, 0x3F },  // 55  85  U
	{ 0x00, 0x1F, 0x20, 0x40, 0x20, 0x1F },  // 56  86  V
	{ 0x00, 0x3F, 0x40, 0x38, 0x40, 0x3F },  // 57  87  W
	{ 0x00, 0x63, 0x14, 0x08, 0x14, 0x63 },  // 58  88  X
	{ 0x00, 0x07, 0x08, 0x70, 0x08, 0x07 },  // 59  89  Y
	{ 0x00, 0x61, 0x51, 0x49, 0x45, 0x43 },  // 5A  90  Z
 	{ 0x00, 0x00, 0x7F, 0x41, 0x41, 0x00 },  // 5B  91  [
 	{ 0x00, 0x02, 0x04, 0x08, 0x10, 0x20 },  // 5C  92  '\'
 	{ 0x00, 0x00, 0x41, 0x41, 0x7F, 0x00 },  // 5D  93  ]
 	{ 0x00, 0x04, 0x02, 0x01, 0x02, 0x04 },  // 5E  94  ^
 	{ 0x40, 0x40, 0x40, 0x40, 0x40, 0x40 },  // 5F  95  _
 	{ 0x00, 0x00, 0x03, 0x05, 0x00, 0x00 },  // 60  96  `
 	{ 0x00, 0x20, 0x54, 0x54, 0x54, 0x78 },  // 61  97  a
 	{ 0x00, 0x7F, 0x44, 0x44, 0x44, 0x38 },  // 62  98  b
 	{ 0x00, 0x38, 0x44, 0x44, 0x44, 0x00 },  // 63  99  c
 	{ 0x00, 0x38, 0x44, 0x44, 0x44, 0x7F },  // 64 100  d
 	{ 0x00, 0x38, 0x54, 0x54, 0x54, 0x18 },  // 65 101  e
 	{ 0x00, 0x08, 0x7E, 0x09, 0x01, 0x02 },  // 66 102  f
 	{ 0x00, 0x18, 0xA4, 0xA4, 0xA4, 0x7C },  // 67 103  g
 	{ 0x00, 0x7F, 0x08, 0x04, 0x04, 0x78 },  // 68 104  h
 	{ 0x00, 0x00, 0x44, 0x7D, 0x40, 0x00 },  // 69 105  i
 	{ 0x00, 0x40, 0x80, 0x88, 0x7A, 0x00 },  // 6A 106  j
 	{ 0x00, 0x7F, 0x10, 0x28, 0x44, 0x00 },  // 6B 107  k
 	{ 0x00, 0x00, 0x41, 0x7F, 0x40, 0x00 },  // 6C 108  l
 	{ 0x00, 0x7C, 0x04, 0x18, 0x04, 0x7C },  // 6D 109  m
	{ 0x00, 0x7C, 0x08, 0x04, 0x04, 0x78 },  // 6E 110  n
	{ 0x00, 0x38, 0x44, 0x44, 0x44, 0x38 },  // 6F 111  o
 	{ 0x00, 0xFC, 0x24, 0x24, 0x24, 0x18 },  // 70 112  p
 	{ 0x00, 0x18, 0x24, 0x24, 0x28, 0xFC },  // 71 113  q
 	{ 0x00, 0x7C, 0x08, 0x04, 0x04, 0x08 },  // 72 114  r
 	{ 0x00, 0x48, 0x54, 0x54, 0x54, 0x24 },  // 73 115  s
 	{ 0x00, 0x04, 0x3F, 0x44, 0x40, 0x20 },  // 74 116  t
 	{ 0x00, 0x3C, 0x40, 0x40, 0x20, 0x7C },  // 75 117  u
 	{ 0x00, 0x1C, 0x20, 0x40, 0x20, 0x1C },  // 76 118  v
 	{ 0x00, 0x3C, 0x40, 0x30, 0x40, 0x3C },  // 77 119  w
 	{ 0x00, 0x44, 0x28, 0x10, 0x28, 0x44 },  // 78 120  x
 	{ 0x00, 0x1C, 0xA0, 0xA0, 0xA0, 0x7C },  // 79 121  y
 	{ 0x00, 0x44, 0x64, 0x54, 0x4C, 0x44 },  // 7A 122  z
 	{ 0x00, 0x00, 0x08, 0x36, 0x41, 0x00 },  // 7B 123  {
 	{ 0x00, 0x00, 0x00, 0x77, 0x00, 0x00 },  // 7C 124  |
 	{ 0x00, 0x00, 0x41, 0x36, 0x08, 0x00 },  // 7D 125  }
 	{ 0x00, 0x04, 0x02, 0x04, 0x08, 0x04 },  // 7E 126  ~
 	{ 0x00, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F }   // 7F 127 typed with Special-L
};

typedef struct {
    int x;
    int y;
    int radius2;
    uint32_t f;} keylookup_t;
    
keylookup_t keylookup[] = {
    {162, 497, 25*25, 0x0408 }, // f1 calc
    {286, 497, 25*25, 0x0808 }, // f2 finance
    {411, 497, 25*25, 0x1008 }, // f3 memo
    {535, 497, 25*25, 0x4008 }, // f4 phone
    {679, 497, 25*25, 0x2008 }, // f5 time

    {40, 588, 25*25, 0x40000 }, // power button
    {128, 588, 25*25, 0x0101 }, // q
    {201, 588, 25*25, 0x0201 }, // w
    {274, 588, 25*25, 0x0401 }, // e
    {346, 588, 25*25, 0x0801 }, // r
    {421, 588, 25*25, 0x1001 }, // t
    {494, 588, 25*25, 0x2001 }, // y
    {568, 588, 25*25, 0x4001 }, // u
    {641, 588, 25*25, 0x8001 }, // i
    {715, 588, 25*25, 0x8002 }, // o
    {788, 588, 25*25, 0x4002 }, // p
    {862, 588, 25*25, 0x2002 }, // backspace
    {936, 588, 25*25, 0x1002 }, // 7
    {1010, 588, 25*25, 0x0102 }, // 8
    {1082, 588, 25*25, 0x0202 }, // 9
    {1156, 588, 25*25, 0x0802 }, // divide

    {40, 660, 27*27, 0x0208 }, // cancel button
    {68, 660, 27*27, 0x0208 }, // cancel button
    {145, 660, 25*25, 0x0110 }, // a
    {217, 660, 25*25, 0x0210 }, // s
    {292, 660, 25*25, 0x0410 }, // d
    {365, 660, 25*25, 0x0810 }, // f
    {439, 660, 25*25, 0x1010 }, // g
    {512, 660, 25*25, 0x2010 }, // h
    {586, 660, 25*25, 0x4010 }, // j
    {658, 660, 25*25, 0x8010 }, // k
    {732, 660, 25*25, 0x8004 }, // l
    {806, 660, 25*25, 0x2004 }, // .  (duplicate!)
    {936,  660, 25*25, 0x1004 }, // 4
    {1010, 660, 25*25, 0x0804 }, // 5
    {1082, 660, 25*25, 0x0404 }, // 6
    {1156, 660, 25*25, 0x0402 }, // multiply

//    {40, 733, 27*27, 0}, // shift button
//    {68, 733, 27*27, 0},
    {163, 733, 25*25, 0x0180 }, // z
    {237, 733, 25*25, 0x0280 }, // x
    {310, 733, 25*25, 0x0480 }, // c
    {384, 733, 25*25, 0x0880 }, // v
    {458, 733, 25*25, 0x1080 }, // b
    {531, 733, 25*25, 0x2080 }, // n
    {604, 733, 25*25, 0x4080 }, // m
    {678, 733, 25*25, 0x8080 }, // ,
    {936,  733, 25*25, 0x0140 }, // 1
    {1010, 733, 25*25, 0x0240 }, // 2
    {1082, 733, 25*25, 0x0440 }, // 3
    {1156, 733, 25*25, 0x0840 }, // -

    {800, 737, 30*30, 0x1020 },  // up
    {747, 778, 30*30, 0x4020 },  // left
    {854, 778, 30*30, 0x2020 },  // right
    {800, 816, 30*30, 0x8020 },  // down


    {40,  806, 27*27, 0x8008 }, // options button
    {68,  806, 27*27, 0x8008 },

    {132, 806, 27*27, 0x0120 }, // worksheet button
    {162, 806, 27*27, 0x0120 },

//    {225, 806, 27*27, }, // special button
//    {254, 806, 27*27, },

    {318, 806, 26*26, 0x0420 }, //   // space bar
    {348, 806, 26*26, 0x0420 }, //   // space bar
    {378, 806, 26*26, 0x0420 }, //   // space bar
    {408, 806, 26*26, 0x0420 }, //   // space bar
    {438, 806, 26*26, 0x0420 }, //   // space bar
    {468, 806, 26*26, 0x0420 }, //   // space bar
    {498, 806, 26*26, 0x0420 }, //   // space bar
    {528, 806, 26*26, 0x0420 }, //   // space bar
    {558, 806, 26*26, 0x0420 }, //   // space bar
    {584, 806, 26*26, 0x0420 }, //   // space bar

    {646, 806, 26*26, 0x0820 }, // DoIt
    {677, 806, 26*26, 0x0820 }, // DoIt

    {936,  806, 25*25, 0x8040 }, // 0
    {1010, 806, 25*25, 0x4040 }, // .
    {1082, 806, 25*25, 0x2040 }, // =
    {1156, 806, 25*25, 0x1040 }, // +

    {0,0, 0, '\0'} // radius = 0 indicates end of list
};



// Global because I had trouble putting it in the class below (which is translated to javascript)
char screen_mem[lcd_native_cols][lcd_native_rows];  // used to blink cursor...
int cursor_x = 0;
int cursor_y = 0;
int cursor_show = 0;
int cursor_blink_state = 0;

// All the graphics code should stay on the JS side. It is possible to tag whole classes with the [[cheerp::genericjs]] tag.
// All members and methods of this class will be compiled to standard JavaScript.
class [[cheerp::genericjs]] Graphics
{
private:
    // When compiling to standard JavaScript it is possible to use DOM objects like any other C++ object.
    static client::HTMLCanvasElement* canvas;
    static client::CanvasRenderingContext2D* canvasCtx;
    static int canvas_width;
    static int canvas_height;

public:
    static void drawKeys(void)
    {
        for (int i=0; keylookup[i].radius2 != 0; i++)
        {
            canvasCtx->beginPath();
            canvasCtx->arc(keylookup[i].x, keylookup[i].y, sqrt(keylookup[i].radius2), 0, 6.2832);
            canvasCtx->set_lineWidth(1);
            canvasCtx->set_strokeStyle("blue");
            canvasCtx->stroke();
        }
    }

    // These methods are here to provide access to DOM and Canvas APIs to code compiled to WebAssembly
    static void initializeCanvas(void)
    {
        // load background image from hidden image in html
        client::HTMLImageElement* bg_image;
        bg_image = (client::HTMLImageElement*)client::document.getElementById("img1");

        // set canvas width and height from the background image
        canvas_width = bg_image->get_width();
        canvas_height = bg_image->get_height();
        canvas = (client::HTMLCanvasElement*)client::document.getElementById("workslate_canvas");
        canvas->set_width(canvas_width);
        canvas->set_height(canvas_height);

#if 0  // append to end of document
        client::document.get_body()->appendChild(canvas);
#else  // append to simulator_tab
        client::document.getElementById("simulator_tab")->appendChild(canvas);
#endif
        canvasCtx = (client::CanvasRenderingContext2D*)canvas->getContext("2d");

        // load the background image in to the canvas
        canvasCtx->drawImage(bg_image, 0, 0);

        // draw a box for the LCD area (with a small border)
        canvasCtx->set_fillStyle("green");
        canvasCtx->fillRect(lcd_base_x-6, lcd_base_y-8, lcd_base_width+12, lcd_base_height+16);

        // clear screen memory
        for (int x = 0; x < lcd_native_cols; x++) {
            for (int y = 0; y < lcd_native_rows; y++) {
                screen_mem[x][y] = ' ';
            }
        }

        //drawKeys(); // debug
    }
 
    static void debugOutput(const char* str)
    {
        canvasCtx->setTransform(1,0,0,1,0,0);  // reset transform
        canvasCtx->set_font("24px sans-serif");
        // canvasCtx->fillText(str, 0, canvas_height - 24);
        canvasCtx->fillText(str, 0, 24);
    }
    // Draw text at screen coordinates (fancy ones, not the bitmap)
    static void drawStr(int x, int y, const char* str, bool inverse)
    {
        // TODO: either bitmap fonts, or we do some scaling to make the characters wider, or use SVG where we can set width
        //       or fontStretch (which doesn't work on safari)
        
        // Set a transform so we can move to row/col coordinates
        // void setTransform(double m11, double m12, double m21, double m22, double dx, double dy);
        // If a point originally had coordinates (x,y), then after the transformation it will have coordinates  (ax+cy+e,bx+dy+f)
        double scalex = (double) lcd_base_width / (double) lcd_native_cols;
        double scaley = (double) lcd_base_height / (double) lcd_native_rows;
        canvasCtx->setTransform( scalex, 0, 0, scaley,
                                 lcd_base_x, lcd_base_y + 1 * scaley - 1);
        canvasCtx->set_font("1px monospace");
#if 0
        canvasCtx->fillText(str, x, y);  // does unicode, but wrong spacing
#else
        const char *p = str;    // TODO: this won't do unicode
        char tempstr[2]="x";
        while(*p != '\0') {
            // clear background
            canvasCtx->set_fillStyle(inverse ? "black" : "green");
            canvasCtx->fillRect(x, y-1, 1.05, 1);  // TODO: a little too big. Also, text has descenders we don't erase.

            // draw character
            canvasCtx->set_fillStyle(inverse ? "green" : "black");  // normally black text
            tempstr[0] = *p++;
            canvasCtx->fillText(tempstr, x++, y-0.15);
        }
#endif
        // back to a 1:1 transform (kindof)
        // canvasCtx->setTransform(1,0,0,1,lcd_base_x, lcd_base_y);
    }


    // Draw at screen coordinates using a bitmap font
    static void drawChar(int x, int y, const char data)
    {
        unsigned char c = data & 0x7f;
        bool inverse = data & 0x80;      // inverse mode

        // Set a transform so we can move to pixel coordinates
        // void setTransform(double m11, double m12, double m21, double m22, double dx, double dy);
        // If a point originally had coordinates (x,y), then after the transformation
        // it will have coordinates  (ax+cy+e,bx+dy+f)
        double scalex = (double) lcd_base_width / (double) lcd_native_width;
        double scaley = (double) lcd_base_height / (double) lcd_native_height;
        canvasCtx->setTransform( scalex, 0, 0, scaley,
                                 lcd_base_x, lcd_base_y);

        // erase the whole character first - helps fix some antialiasing problems in the browser
        canvasCtx->set_fillStyle(inverse ? "black" : "green");
        double shrink = inverse ?  0.1 : 0; // draw black bar slightly smaller
        canvasCtx->fillRect(x * lcd_native_char_width,
                            y * lcd_native_char_height + shrink, 
                            lcd_native_char_width, lcd_native_char_height - 2*shrink);

        for (int cx = 0; cx < lcd_native_char_width; cx++) {
            uint8_t d = lcd_font[(uint8_t) c][cx] ^ (inverse ? 0xFF : 0);
            // bitmap of a single vertical column.  LSB is at top.

            for (int cy = 0; cy < lcd_native_char_height; cy++) {
                // draw a pixel
                canvasCtx->set_fillStyle(d & 0x01 ? "black" : "green");
                d = d >> 1;  // advance to next pixel
                canvasCtx->fillRect(x * lcd_native_char_width  + cx,
                                    y * lcd_native_char_height + cy, 
                                    0.9, 0.9);  // should be 0.9
            }
        }

        // back to a 1:1 transform (kindof)
        // canvasCtx->setTransform(1,0,0,1,lcd_base_x, lcd_base_y);
    }

    static void draw_cursor_always(void)   // can be used to erase cursor
    {
        if (cursor_show && cursor_blink_state) {
            drawChar(cursor_x, cursor_y, (char) 0x80 + ' ');  // inverse space
        } else {
            drawChar(cursor_x, cursor_y, screen_mem[cursor_x][cursor_y]);
        }
    }

    static void blink_cursor_if_needed(uint64_t milliseconds)
    {
        // blink isn't syncrhonized to RTC clock (just like on actual hardware)
        static uint64_t prev_milliseconds = 0;
        if (milliseconds-prev_milliseconds > 500) {
            prev_milliseconds = milliseconds;
            cursor_blink_state = !cursor_blink_state;
            if (cursor_show) {          // only draw if it's shown
                draw_cursor_always();   // update on screen
            }
        }
    }


    //======= Animation frame ============================================================

    static void advance_rtc_if_needed(uint64_t milliseconds)
    {
        static uint64_t prev_seconds = 0;
        uint64_t seconds = milliseconds / 1000;
        if(prev_seconds != seconds) {
            rtc_update(NULL);  // seconds has rolled over, so advance RTC
            prev_seconds = seconds;
        }
    }


    // This method is the handler for requestAnimationFrame. The browser will call this
    // in sync with its graphics loop, usually at 60 fps.
    static void sim_frame(void)
    {
#if 0
    double m2 = client::Date().getMilliseconds();
    double s2 = client::Date().getSeconds();
    printf("sec=%d, msec=%d\n", (int) s2, (int) m2);  

    client::Date * d = (client::Date *) client::Date();// = & client::Date();
    double m1 = d->getMilliseconds();
    double s1 = d->getSeconds();
    printf("sec=%d, msec=%d\n", (int) s1, (int) m1);  
#endif
    uint64_t milliseconds = client::Date().getTime();  // milliseconds since 1970
//    printf("sec=%lld\n", milliseconds);  

        blink_cursor_if_needed(milliseconds);
        advance_rtc_if_needed(milliseconds);

        set_system_time(milliseconds);
        sim(SIM_CYCLES_PER_FRAME);        // TODO: tune number of cycles dynamically
        client::requestAnimationFrame(cheerp::Callback(sim_frame));
    }

    //======= key handlers ==============================================================
   
    // Handle the non-ascii keys
    static void keyDownHandler(client::KeyboardEvent* e)
    {
        if(!client::getKeyboardActive()) {
            return;  // user is on another tab, so simulator shouldn't respond.
        }

        uint32_t kc = e->get_keyCode(); 
        uint32_t f = 0;
        switch(kc) {
            case 0x26:f = 0x1020; break;  // up
            case 0x28:f = 0x8020; break;  // down
            case 0x27:f = 0x2020; break;  // right
            case 0x25:f = 0x4020; break;  // left
            case 0x70:f = 0x0408; break;  // F1 -->    Calc soft key
            case 0x71:f = 0x0808; break;  // F2 --> Finance soft key
            case 0x72:f = 0x1008; break;  // F3 -->    Memo soft key
            case 0x73:f = 0x4008; break;  // F4 -->   Phone soft key
            case 0x74:f = 0x2008; break;  // F5 -->    Time soft key
            case 0x75:f = 0x8008; break;  // F6 -->      Options key
            case 0x76:f = 0x0120; break;  // F7 -->    Worksheet key
            case 0x14:                    // Caps lock key is special
                      e->preventDefault();  // eat character so browser doesn't scroll window
                      push_kbd_fifo(0x20000);  // bit 17 is the special key
                      push_kbd_fifo(0x30000);  // add in the shift key (bit 16)
                      push_kbd_fifo(0);
                      break;
            default: //printf("[UNKNOWN DOWN CODE KEY %02x]\n", kc);
                break;
        }
        if (f) {  // stuff in fifo
            push_kbd_fifo(f);
            e->preventDefault();  // eat character so browser doesn't scroll window
            // stuff no-keys-pressed in fifo
            push_kbd_fifo(0);
        }
    }


    // Caps lock works differently than the other keys.  We get a keydown when it's locked,
    // and a keyup when its unlocked.  The Workslate doesn't have a keypress sequence to
    // postively undo its caps lock, so this could get out of sync with the emulator's
    // keyboard.  TODO: detect state of emulator caps lock and make sure they're in sync.
    static void keyUpHandler(client::KeyboardEvent* e)
    {
        if(!client::getKeyboardActive()) {
            return;  // user is on another tab, so simulator shouldn't respond.
        }

        uint32_t kc = e->get_keyCode(); 
        switch(kc) {
            case 0x14:                    // Caps lock key is special
                      e->preventDefault();  // eat character so browser doesn't scroll window
                      push_kbd_fifo(0x20000);  // bit 17 is the special key
                      push_kbd_fifo(0x30000);  // add in the shift key (bit 16)
                      push_kbd_fifo(0);
                      break;
            default: // printf("[UNKNOWN UP KEY CODE %02x]\n", kc);
                break;
        }
    }


    // Handle the ASCII keys
    // TODO: Spec-Cancel (to exit rx'ing over serial port)
    static void keyPressHandler(client::KeyboardEvent* e)
    {
        if(!client::getKeyboardActive()) {
            return;  // user is on another tab, so simulator shouldn't respond.
        }

        //printf("press: keyCode %g\n", e->get_keyCode());   // has shift key, doesn't do ascii
        uint32_t inkey = e->get_keyCode();
        int shift_key = 0;
        int special_key = 0;
        if ((inkey >= 'A') && (inkey <= 'Z')) {
            inkey = inkey - 'A' + 'a';
            shift_key = 1;
        }

        uint32_t f = 0;
        switch(inkey) {
            case 'a': f = 0x0110; break;
            case 'b': f = 0x1080; break;
            case 'g'-0x60: f = 0x1080; special_key = 1; break;  // special-B --> Get
            case 'c': f = 0x0480; break;
            case 3: f = 0x0480; special_key = 1; break;  // special-C = CT char (displays as 0x1c)
            case 'd': f = 0x0410; break;
            case 'e': f = 0x0401; break;
            case '#': f = 0x0401; special_key = 1; break;  // special-E
            case 'f': f = 0x0810; break;
            case '\\':f = 0x0810; special_key = 1; break;  // special-F
            case 'g': f = 0x1010; break;
            case '^': f = 0x1010; special_key = 1; break;  // special-G
            case 'h': f = 0x2010; break;
            case '_': f = 0x2010; special_key = 1; break;  // special-H
            case 'i': f = 0x8001; break;
         // case    : f = 0x8001; special_key = 1; break;  // special-I  This shows as a '-', same as the subtract!
            case 'j': f = 0x4010; break;
            case '|': f = 0x4010; special_key = 1; break;  // special-J
            case 'k': f = 0x8010; break;
            case '~': f = 0x8010; special_key = 1; break;  // special-K
            case 'l': f = 0x8004; break;
//          case ???: f = 0x8004; special_key = 1; break;  // special-L is a big solid box (char 127)
            case 'm': f = 0x4080; break;
            case 'w'-0x60: f = 0x4080; special_key = 1; break;  // special-M--> Switch
            case 'n': f = 0x2080; break;
            case 'r'-0x60: f = 0x2080; special_key = 1; break;  // special-N --> Recalc
            case 'o': f = 0x8002; break;
            case '?': f = 0x8002; special_key = 1; break;  // special-O
            case 'p': f = 0x4002; break;
            case 'd'-0x60: f = 0x4002; special_key = 1; break;  // special-P -- actually '/' but we map that to divide
            case 'q': f = 0x0101; break;
            case '!': f = 0x0101; special_key = 1; break;  // special-Q
            case 'r': f = 0x0801; break;
            case '&': f = 0x0801; special_key = 1; break;  // special-R
            case 's': f = 0x0210; break;
            case '`': f = 0x0210; special_key = 1; break;  // special-S
            case 't': f = 0x1001; break;
            case 'a'-0x60: f = 0x1001; special_key = 1; break;  // special-T -- actually '*' but we map that to times
            case 'u': f = 0x4001; break;
            case '\"':f = 0x4001; special_key = 1; break;  // special-U
            case 'v': f = 0x0880; break;
            case 's'-0x60: f = 0x0880; special_key = 1; break;  // special-V --> Save
            case 'w': f = 0x0201; break;
            case '@': f = 0x0201; special_key = 1; break;  // special-W
            case 'x': f = 0x0280; break;
            case 'p'-0x60: f = 0x0280; special_key = 1; break;  // special-X --> Print
            case 'y': f = 0x2001; break;
            case '\'':f = 0x2001; special_key = 1; break;  // special-Y
            case 'z': f = 0x0180; break;
            case 'f'-0x60: f = 0x0180; special_key = 1; break;  // special-Z --> Find
            case '0': f = 0x8040; break;
            case '<': f = 0x8040; special_key = 1; break;  // special-0
            case '1': f = 0x0140; break;
            case '[': f = 0x0140; special_key = 1; break;  // special-1
            case '2': f = 0x0240; break;
            case ']': f = 0x0240; special_key = 1; break;  // special-2
            case '3': f = 0x0440; break;
            case 'n'-0x60: f = 0x0440; special_key = 1; break;  // special 3 -- not equals
            case '4': f = 0x1004; break;
            case '(': f = 0x1004; special_key = 1; break;  // special-4
            case '5': f = 0x0804; break;
            case ')': f = 0x0804; special_key = 1; break;  // special-5
            case '6': f = 0x0404; break;
            case '%': f = 0x0404; special_key = 1; break;  // special-6
            case '7': f = 0x1002; break;
            case '{': f = 0x1002; special_key = 1; break;  // special-7
            case '8': f = 0x0102; break;
            case '}': f = 0x0102; special_key = 1; break;  // special-8
            case '9': f = 0x0202; break;
            case '$': f = 0x0202; special_key = 1; break;  // special-9
            case ',': f = 0x8080; break;                   // (near letters not numpad) not in chart
            case ';': f = 0x8080; special_key = 1; break;  // special , ERROR: returns ,,,,,,,
            case '.': f = 0x4040; break;  // numpad
        // ^^ SAW SOMETHING ODD IN MEMO ONCE
            case '>': f = 0x4040; special_key = 1; break;  // special-.
            //            0x2004;         // duplicate '.' key next to L  -- we don't use
            case ':': f = 0x2004; special_key = 1; break; // ERROR: returns ....
            case '=': f = 0x2040; break;  // aka "Formula"
        // ^^ SAW SOMETHING ODD IN MEMO ONCE
            case '+': f = 0x1040; break;
            case '-': f = 0x0840; break;
            case '/': f = 0x0802; break;  // divide character
            case '*': f = 0x0402; break;  // multiply character
            case ' ': f = 0x0420; break;  // space bar
            case 0x0D:f = 0x0820; break;  // Enter-->DoIt
            case 0x08:f = 0x2002; break;  // backspace  (different from linux)
            case 0x1b:f = 0x0208; break;  // ESC --->Cancel
            default: //printf("[UNKNOWN KEY IN %02llX]", inkey);
                break;
        }
        if (f) {   // if we got a known key
            e->preventDefault();  // eat character so browser doesn't scroll window
            if (shift_key) {
                // jam in just the shift key being pressed
                push_kbd_fifo(0x10000);
                // then jam in the combo next
                f = f | 0x10000;
            }
            if (special_key) {
                // jam in just the special key being pressed
                // TODO: this should probably include 0x10000 if shift is pressed
                push_kbd_fifo(0x20000);  // bit 17 is the special key
                // then jam in the combo next
                f = f | 0x20000;
            }
            // push the actual key into fifo
            push_kbd_fifo(f);
            // push no-keys-pressed in fifo
            push_kbd_fifo(0);
        }
    }

#if 0
  class MouseEvent: public UIEvent {
        public:
                double get_button();
                double get_buttons();
                bool get_ctrlKey();
                bool get_altKey();
                bool get_metaKey();
                bool getModifierState(const String& keyArg);
        };
#endif
    // Handle the graphical keyboard
    static void mouseDownHandler(client::MouseEvent* e)
    {
        if(!client::getKeyboardActive()) {
            return;  // user is on another tab, so simulator shouldn't respond.
        }

        double x = e->get_offsetX();
        double y = e->get_offsetY();
        uint32_t f = 0;

        for (int i=0; !f && (keylookup[i].radius2 != 0); i++)
        {
            double r2 =  (x-keylookup[i].x) * (x-keylookup[i].x) 
                       + (y-keylookup[i].y) * (y-keylookup[i].y);
            if (r2 < keylookup[i].radius2)
            {
                f = keylookup[i].f;
            }
        }

        //printf("mousedown @ %g, %g -> key = %d\n", x, y, f);

        if (f) {   // if we got a known key
            uint32_t modifier_keys = 0;
            if (e->get_shiftKey()) {
                // jam in just the shift key being pressed
                modifier_keys = 0x10000;
                push_kbd_fifo(modifier_keys);
            }
            if (e->get_altKey()) { // also get_metaKey get_ctrlKey
                modifier_keys |= 0x20000;
                push_kbd_fifo(modifier_keys);
            }

            // push the actual key into fifo
            push_kbd_fifo(f | modifier_keys);
            // push no-keys-pressed in fifo
            push_kbd_fifo(0);
        }
    }

    static void init_callbacks(void)
    {
        client::requestAnimationFrame(cheerp::Callback(Graphics::sim_frame));
        client::document.addEventListener("keydown", cheerp::Callback(Graphics::keyDownHandler));
        client::document.addEventListener("keyup", cheerp::Callback(Graphics::keyUpHandler));
        client::document.addEventListener("keypress", cheerp::Callback(Graphics::keyPressHandler));
        client::document.addEventListener("mousedown", cheerp::Callback(Graphics::mouseDownHandler));
    }
};



// Low level cursor routines called from workslate_hw.c
void move_cursor(uint16_t p)
{
    int y = p / lcd_native_cols;
    int x = p % lcd_native_cols;
    int saved_cursor_show = cursor_show;

    if(saved_cursor_show) {
        cursor_show = 0;
        Graphics::draw_cursor_always();// erase character at old position
    }

    cursor_x = x;
    cursor_y = y;

    if(saved_cursor_show) {
        cursor_show = saved_cursor_show;
        Graphics::draw_cursor_always();  // draw cursor at new position
    }
}

void show_cursor(int show)
{
    cursor_show = show ? 1 : 0;

    Graphics::draw_cursor_always();  // update screen
}

// Low level character drawing routine called from workslate_hw.c
void write_char_at_position(unsigned char data, uint16_t p)
{
    int y = p / lcd_native_cols;
    int x = p % lcd_native_cols;

    screen_mem[x][y] = data;
    Graphics::drawChar(x, y, data);

    // advance cursor
    if(++x >= lcd_native_cols) {
        x = 0;
        if(++y >= lcd_native_rows) {
            y = 0;
        }
    }
    move_cursor(x + y*lcd_native_cols);
}



// This code executes in wasm, and will automatically call the code in the graphics class which is compiled to JS
void webMain()
{
    printf("Workslate Emulator by John Maushammer\n");
    printf("Build date " __DATE__ "  " __TIME__ "\n");
    //printf("DEBUG - reached %s at " __FILE__ ":%d\n", __FUNCTION__, __LINE__);

    Graphics::initializeCanvas();

    mon_out = stdout;
    mon_in = stdin;

    /* Load initial memory image */
    memcpy(rom_u15, resource_u15_bin, 0x8000);  // from xxd-generated file
    memcpy(rom_u16, resource_u16_bin, 0x8000);  // from xxd-generated file

    /* Read starting address from reset vector */
    workslate_hw_reset(); // set bank to start in
    pc = ((mread(0xFFFE) << 8) + mread(0xFFFF));

    Graphics::init_callbacks();

    // we don't really exit here because we have the sim_frame() callback active
}

