/*   Workslate WK-100 Emulator
 *   Copyright (C) 2025 John Maushammer
 *
 *   Based on EXORcister simulator
 *        Copyright (C) 2011 Joseph H. Allen
 *   And with improvements by Joel Rees -  exorsim6801
 *         https://osdn.net/users/reiisi/pf/exorsim6801/wiki/FrontPage
 *
 * This is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 1, or (at your option) any later version.
 *
 * It is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this software; see the file COPYING.  If not, write to the Free Software Foundation,
 * 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define USE_ANSI_XY 1   // emulate screen using ANSI escape codes (default)
                        // Looks nicer, but hides debugging info
#define USE_HEXVIEW 0   // display unknown control characters as hex codes


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifndef WASM
#include <fcntl.h>
#endif
#include <poll.h>
#include <signal.h>
#include <unistd.h>    /* JMR20201103 */
#include <time.h>

#include "exorsim.h"    /* JMR20201121 */
#include "sim6800.h"
#include "unasm6800.h"    /* JMR202021103 */
#include "workslate.h"
#include "exorterm.h"
#include "utils.h"    /* JMR20201103 */

/* Clock frequency */
// This is 1/4 of the external crystal.  Up to 2 MHz with "B" version of CPU.
// The board has a 4.9152 MHz crystal (also used on the serial port adapter), but this
// doesn't work to well - probably because we overestimate the cycles (e.g. INX takes 6, 
// DECB takes 5, BHI takes 4), so 2 MHz is probably closer to accurate.
#define E_CLOCK_FREQUENCY  20000000  // TODO this is 20MHz; should be 2 MHz
//#define E_CLOCK_FREQUENCY  (4915200/4)

/* Memory */
#define RAMSIZE 0x4000     // code looks like it wouild support 32kB! but not tested
unsigned char ram[RAMSIZE];
#define ROMSTART 0x8000    // 0x8000-FFFF
unsigned char rom_u14[0x8000]; // not used
unsigned char rom_u15[0x8000];
unsigned char rom_u16[0x8000]; // boot bank

// Tag the stack so we can identify the return stack's contents
#define TAGSIZE 0x300     // stack is only < 0x20F so we don't need all RAM
char ram_tag[TAGSIZE];

extern int trace_idx; // for debugging

/////////////////////////////
#define ADDR_PORT1_DDR     0x00
#define ADDR_PORT2_DDR     0x01
#define ADDR_PORT1         0x02
#define ADDR_PORT2         0x03
#define ADDR_PORT3_DDR     0x04
#define ADDR_PORT4_DDR     0x05
#define ADDR_PORT3         0x06
#define ADDR_PORT4         0x07

#define ADDR_TCSR          0x08   // Timer Control/Status Register (TCSR) -- TODO
#define ADDR_CHR           0x09   // free-running counter
#define ADDR_CLR           0x0A   //
#define ADDR_OCHR          0x0B   // Output compare register
#define ADDR_OCLR          0x0C
//#define ADDR_ICHR          0x0D
//#define ADDR_ICLR          0x0E
#define ADDR_PORT3_CSR     0x0F

#define ADDR_RMCR          0x10   // SCI Rate and Mode Control Register  (only lower 4 bits)
#define ADDR_TRCSR         0x11   // Transmit/ Receive Control and Status Register
#define ADDR_SCRDR         0x12   // SCI Receiver Data Register
#define ADDR_SCTDR         0x13   // SCI Transmit Data Register - used with "Print" command

#define ADDR_RAMCTRL       0x14

// 0x15-1F is internal CPU registers - reserved

#define ADDR_KBD           0x20  // read and write

// 0x24-25 is LCD
#define ADDR_LCD_DATA      0x24
#define ADDR_LCD_INSTR     0x25

// 0x2C & 0x2D has the tape PLA
#define ADDR_TAPE_PLA_2C   0x2c  // seen at powerup. Used by NMI routine (which counts tape position).  Doc says has power-down bit.
#define ADDR_TAPE_PLA_2D   0x2d  // seen at powerup, and also =0x41 just before SLP instruction with no interrupts

#define ADDR_DTMF          0x30  // seen at D6CC

// 0x40-0x7F is RTC
#define ADDR_RTC_START     0x40
#define ADDR_RTC_END       0x7F
static unsigned char rtc_mem[ADDR_RTC_END - ADDR_RTC_START + 1];


#define ADDR_RAMTEST_ADDRESS  0x4000  // probed to see if there is memory

#define ADDR_SCI_VECTOR       0xFFF0  // IRQ2/SCI (RDRF, ORFE, TORE)
#define ADDR_TOF_VECTOR       0xFFF2  // IRQ2/Timer Overflow (TOF)
#define ADDR_OCF_VECTOR       0xFFF4  // IRQ2/Timer Output Compare (OCF)
#define ADDR_ICF_VECTOR       0xFFF6  // IRQ2/Timer Input Capture (lCF)
#define ADDR_IRQ1_VECTOR      0xFFF8
#define ADDR_SWI_VECTOR       0xFFFA
#define ADDR_NMI_VECTOR       0xFFFC
#define ADDR_RST_VECTOR       0xFFFE


/* fwd decl */
void rtc_update(struct timespec *ts);
int test_serial_rx_fifo_has_character(void);
uint8_t pull_serial_rx_fifo(void);
void clear_kbd_fifo(void);
void workslate_hw_reset(void);

/////////////////////////////
// General logger utility
//

#if !defined(WASM)
void logit(char * s, uint8_t data)
{   static FILE *f = NULL;
    if (f==NULL) f=fopen("register_access_log.txt", "w");

    //fprintf(f, "%04x %s\n", Timer_Counter, s);

// TODO: use internal cyclecount instead of system time
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "%3ld.%06ld - PC=%d.%04x data=%02x %s\n", ts.tv_sec % 1000, ts.tv_nsec / 1000,
            get_bank(), pc, data, s);
}
#else
#define logit(str, data) ;
#endif

/////////////////////////////
// Built in Timer
//
// Init:  TCSR=0x0C  ... it's weird that no other regs are initialized.
//                       Enable Timer Overflow Interrupt (0xFFF2)
//                       Enable Output Compare Interrupt (0xFFF4)
// ... which one does what?
// Output compare seems to do keyboard scan.

unsigned short Timer_Counter = 0x0000;
unsigned short Timer_OutputCompare = 0xFFFF;

// Setter for local time variable - number of milliseconds since 1970 (WASM only)
#ifdef WASM
static uint64_t time_milliseconds;
void set_system_time(uint64_t milliseconds)
{
    time_milliseconds = milliseconds;
}
#endif

// The simulator doesn't count cycles, so we estimate by attaching this to mread & mwrite
// (we don't emulate the ADDR_OCHR one-cycle inhibit feature so we could misstrigger,
// but that doesn't look possible in the workslate ISR)

// 5 msec is 30% CPU and more responsive.  100 msec is 20% CPU and less responsive.
// We could optimize SLP instruction, but we don'.t
#define SLEEP_STEP_TIME 0.005 
void advance_cycle(void)
{
    cycles_simulated_this_tick++;  // Count cycles so sim() can simulate a fixed time (used in WASM)

#ifndef WASM   // WASM regulates time differently
    // Slow down to real time
    static uint32_t cyclecount = -1;
    static struct timespec ts;
    if(cyclecount++ >= SLEEP_STEP_TIME * E_CLOCK_FREQUENCY) {  // every 5 milliseconds of simulated time
        if(cyclecount == 0) {  // if first time
            clock_gettime(CLOCK_MONOTONIC, &ts);
        }
        cyclecount = 0;
        ts.tv_nsec += SLEEP_STEP_TIME * 1000000000;
        if(ts.tv_nsec >=  1000000000) {
            ts.tv_nsec -= 1000000000;
            ts.tv_sec++;
            rtc_update(&ts);
        }
#ifdef __MACH__
        clock_nanosleep_abstime(&ts);
#else
        // linux version untested
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
#endif
    }
#endif // WASM
    // WASM updates rtc with function advance_rtc_if_needed()


    //--- CPU ---
    // Timer has no pre-scaler ... just advances on every E cycle.
    Timer_Counter++;

    if ((Timer_Counter == 0x0000) && (ram[ADDR_TCSR] & 0x04))  {   // Overflow
        // logit(ram[ADDR_TCSR] & 0x20 ? "Timer overflow - TOF was set" : "Timer overflow - TOF was clear", 0);
        ram[ADDR_TCSR] |= 0x20;  // set TOF flag
        assert_irq(ADDR_TOF_VECTOR);
        // stop = 1;
        // printf("TIMER OVERFLOW");
    }

    if ((Timer_Counter == Timer_OutputCompare) && (ram[ADDR_TCSR] & 0x08))  {   // Output Compare
        // logit(ram[ADDR_TCSR] & 0x40 ? "Timer compare - OCF was set" : "Timer compare - OCF was clear", 0);
        ram[ADDR_TCSR] |= 0x40;  // set OCF flag
        assert_irq(ADDR_OCF_VECTOR);
        // stop = 1;
        // printf("TIMER OUTPUT COMPARE");
    }

    //--- Serial Port ---
    if((ram[ADDR_TRCSR] & 0x08) && test_serial_rx_fifo_has_character())
    {   // if RX enabled & have a character
        ram[ADDR_TRCSR] |= 0x80;  // set RDRF (we have a character)
        if(ram[ADDR_TRCSR] & 0x10) {    // If Recieve interrupt enable
            assert_irq(ADDR_SCI_VECTOR);
        }
    }

    //--- RTC ---
    // TODO:  This takes 20% of out sim time
    static unsigned int rtc_counter = 0;
    unsigned char rate_select = rtc_mem[0x0A] & 0x0F;
    if(rate_select) {
        unsigned char prev_PF = rtc_counter & (1 << (rate_select + 7)) ? 1 : 0;
        rtc_counter++;
        unsigned char PF = rtc_counter & (1 << (rate_select + 7)) ? 1 : 0;
        // Update registers with new PF
        if(PF && !prev_PF) {  // rising edge can trigger PIE interrupt
            // flag gets marked even if interrupts aren't enabled
            rtc_mem[0x0C] |= 0x40;
            // Check if we should trigger an interrupt
            if(rtc_mem[0x0B] & 0x40) { // PIE interrupt enable
                // logit(rtc_mem[0x0C] & 0x80 ? "RTC PIE - PF was set" : "RTC PIE - PF was clear", 0);
                rtc_mem[0x0C] |= 0x80;  // signal we generated an interrupt
                assert_irq(ADDR_IRQ1_VECTOR);
            }
        }
    } // else counter is stopped, so don't do anything
}

/////////////////////////////
// RTC Chip - HD146818FP
//
// Init:  RTC 0xA = 20 - sets divisors (including interrupt)
//        RTC 0xB = 13   update-ended interrupt enabled
//                       SQW square wave disabled.
//                       BCD mode, 24 hour mode, daylight savings time
//
// if RTC time is invalid, it will reset time to 9/26/83 at 8:00 am
//
// TODO: Generate interrupts, keep time
// Square wave output seems like tone generator - not for interrupz
// Interrupts:
// --> PIE (periodic interrupt) is probably connected to IRQ1.  Used for timing beeps
// --> also alarm flag (AIE)   (doesn't seem used)
// --> update ended flag (UIE)
//
// Seems to always be in BCD mode, so we assume that.  It's also always 24hr mode
// We could init with system time if we wanted to.
//
// Workslate will power off if seconds stays at 0.

static inline uint8_t bin2bcd(uint8_t bin)
{    return ((bin / 10)<<4) | (bin % 10);
}
static inline uint8_t bcd2bin(uint8_t bcd)
{    return ((bcd / 16) * 10) + (bcd & 0x0f);
}

static void write_rtc(unsigned short addr, unsigned char data)
{
    if(addr >= 0x0E) {        // Simple RAM
        rtc_mem[addr] = data;
        return;
    }
    // printf("\nwrite RTC 0x%X = %02x\n", addr, data);
    switch(addr) {
        case 0:  // seconds
        case 1:  // seconds alarm
        case 2:  // minutes
        case 3:  // minutes alarm
        case 4:  // hours
        case 5:  // hours alarm
        case 6:  // day of week
        case 7:  // day of month
        case 8:  // month
        case 9:  // year
            rtc_mem[addr] = bcd2bin(data);
            break;
        case 10:  // REG A
        case 11:  // REG B
            rtc_mem[addr] = data;
            break;
        case 12:  // REG C   read-only
        case 13:  // REG D   read-only
        default:
            stop = 1; // unimplemented
            break;
    }
}

static unsigned char read_rtc(unsigned short addr)
{
    unsigned char saved_reg_c;
    if(addr >= 0x0E) {        // Simple RAM
        return rtc_mem[addr];
    }
    // printf("\nread RTC 0x%X\n", addr);
    switch(addr) {
#if 0  // force a time
        case 0:  // seconds
            return 0x56;
        case 2:  // minutes
            return 0x45;
        case 4:  // hours
            return 0x12;
        case 6:  // day of week
            return 0x00;
        case 7:  // day of month
            return 0x25;
        case 8:  // month
            return 0x11;
        case 9:  // year
            return 0x89;
#else
        case 0:  // seconds
        case 2:  // minutes
        case 4:  // hours
        case 6:  // day of week
        case 7:  // day of month
        case 8:  // month
        case 9:  // year
#endif
        case 1:  // seconds alarm
        case 3:  // minutes alarm
        case 5:  // hours alarm
            return bin2bcd(rtc_mem[addr]);
            break;

        case 0x0B:  // REG B
        case 0x0D:  // REG D
            return rtc_mem[addr];
        case 0x0C:  // REG C
            saved_reg_c = rtc_mem[addr];
            rtc_mem[addr] = 0;  // cleared by read
            deassert_irq(ADDR_IRQ1_VECTOR);
            return saved_reg_c;
        case 0x0A:  // REG A
            return rtc_mem[addr] & 0x7F;  // update-in-progress bit always 0
    }
    stop = 1; // unimplemented
    return 0;
}

void rtc_update(struct timespec *ts)  // ts isn't really used, but it could be in the future
{
    if ((rtc_mem[0x0B] & 0x90) == 0x10) { // if not in set mode (MSB=0) and update ended interrupt enable (UIE)
        // logit(rtc_mem[0x0C] & 0x10 ? "RTC UIE - UF was set" : "RTC UIE - UF was clear", 0);
        rtc_mem[0x0C] |= 0x10;  // signal we generated an interrupt
        assert_irq(ADDR_IRQ1_VECTOR);
    }

    // Advance time
    if (++rtc_mem[0x0] == 60) {  // seconds
        rtc_mem[0x0] = 0;
        if (++rtc_mem[0x2] == 60) {  // minutes
            rtc_mem[0x2] = 0;
            if (++rtc_mem[0x4] == 24) {  // hours (0-23)
                rtc_mem[0x4] = 0;
                // TODO: increment days, months, years, DOW
            }
        }
    }

#if 0
    // TODO: I don't think it uses the alarm... 
    if ((rtc_mem[0x0B] & 0xA0) == 0x20) { // if not in set mode (MSB=0) and alarm interrupt enable (AIE)
        // "An alarm interrupt occurs for each second that the three time bytes equal the three alarm bytes"
        // check for actual alarm
        // logit(rtc_mem[0x0C] & 0x20 ? "RTC AIE - AF was set" : "RTC AIE - AF was clear");
        rtc_mem[0x0C] |= 0x20;  // signal we generated an interrupt
        assert_irq(ADDR_IRQ1_VECTOR);
    }
#endif

#if 0
    printf("\033[1;1H t=%ld.%03ld ", ts->tv_sec, ts->tv_nsec/1000000); // print seconds in top left corner
    if(rtc_mem[0x0B] & 0x20) printf("ALARM %d:%02d:%02d ", rtc_mem[5], rtc_mem[3], rtc_mem[1]);
#endif
}

/////////////////////////////
// LCD Controller - HD61830A00
//
// 46x16 chars.  Uses external char gen to show inverse characters
//
// LCD CMD 00  ARG 29 - mode on, EXTERNAL CG
// LCD CMD 01  ARG 75 - 6x8 pitch
// LCD CMD 02  ARG 2d - hor char = 2d+1= 46
// LCD CMD 03  ARG 3f - timing
// LCD CMD 04  ARG 07 - cursor position
// LCD CMD 08  ARG 00 - display start at 0000
// LCD CMD 09  ARG 00 - display start at 0000
//
//-----------------------------------------
// Custom Font hasn't been fully extracted.
// The font ROM looks like it's probably the RCA chip (RCA had a patent on font roms),
// it's still probably unique to this product and not a standard RCA part.
//
// Unknown char:  phone hook symbol - solid
// Unknown char:  phone hook symbol - outline
// Unknown char 08: there's a symbol at top that's probably a phone or error symbol.  Only seen on emulator.
//
// We can display/type some of these characters using the Draw > Sheets command:
// 0e  0f  18  17       top-left-corner    top-right-corner  hor  vert
// 10  11  18  12       btm left corner    btm right corner  hor   +
// 15  16  13  14       |-                 -|                T    upside-down-T
// 1a  1f  18  19       solid block        hollow block      hor  double=hor
//
// https://en.wikipedia.org/wiki/Box-drawing_characters
// https://en.wikipedia.org/wiki/Code_page_437
// https://www.utf8-chartable.de/unicode-utf8-table.pl?start=9472&names=-
// https://www.compart.com/en/unicode/category/Sm
// https://shapecatcher.com
//
uint32_t iso_font[32] = {
/* 00 */  0xe28690,   // ←  e2 86 90  left arrow
/* 01 */  0xe28692,   // →  e2 86 92  right arrow (seen in sort)
/* 02 */  0xe29c87,   // ✇  E2 9C 87  tape symbol (should be squarer)
/* 03 */  0xE29ABF,   // ⚿  E2 9A BF  key symbol
/* 04 */  0xc397,     // ×  C3 97     times
/* 05 */  0xc3b7,     // ÷  C3 B7     divide
/* 06 */  0xe289a0,   // ≠  e2 89 a0  not equals
/* 07 */  0xF09F9494, // 🔔 F0 9F 94 94 bell character used for alarms (no unicode)
// 08     0xF09F939E, // 📞 F0 9F 93 9E phone hook symbol seen in status bar
/* 08 */  0xe2988e,   // ☎  E2 98 8E  phone hook symbol seen in status bar
/* 09 */  0xe297b7,   // ◷  e2 97 b7  clock symbol when timer is running
/* 0a */  0xe2908a,   // ␊  e2 90 8a  displayed by password function
/* 0b */  0xe29780,   // ◀  e2 97 80  filled arrow next to Row and Column indicators
/* 0c */  0xe2908c,   // ␌  e2 90 8c  FF character
/* 0d */  0xe2908d,   // ␍  e2 90 8d  displayed by password function
/* 0e */  0xe2948f,   // ┏  e2 94 8f
/* 0f */  0xe29493,   // ┓  e2 94 93
/* 10 */  0xe29497,   // ┗  e2 94 97
/* 11 */  0xe2949b,   // ┛  e2 94 9b
/* 12 */  0xe2958b,   // ╋  e2 95 8b  plus
/* 13 */  0xe294b3,   // ┳  e2 94 b3  T at top of bars
/* 14 */  0xe294bb,   // ┻  e2 94 bb  upside down T
/* 15 */  0xe294a3,   // ┣  e2 94 a3
/* 16 */  0xe294ab,   // ┫  e2 94 ab
/* 17 */  0xe29483,   // ┃  e2 94 83  thick vertical bar
/* 18 */  0xe29481,   // ━  e2 94 81  thick horizontal bar
/* 19 */  0xe29590,   // ═  e2 95 90
/* 1a */  0xe296a3,   // ▣  e2 96 a3        ◾  e2 97 be -- not working
/* 1b */  0xe2909b,   // ␛  e2 90 9b  EC character
/* 1c */  0xc2a9,     // ©  c2 a9  ... this char actually doesn't exist anywhere else!
                      // special-C = CT char (displays as 0x1c) ... convergent technologies?!
                      // This is a C in the top left corner, a T in the bottom right.
                      // also: ␍  e2 90 8d
/* 1d */  0xe2ac93,   // ⬓  e2 ac 93  Square with bottom half black (Unicode: 0x2b13)
/* 1e */  0xe297a8,   // ◨  e2 97 a8  Box with filled right  (these 2 seen in WINDOW)
/* 1f */  0xe296a2};  // ▢  e2 96 a2

#ifndef WASM    // not used in WASM
#if USE_ANSI_XY
static void init_ansi_screen(void)
{
    printf("\033[2J");  // clear screen
    printf("\033[1;1H"); // goto top left corner

    // Draw box around active area
    printf("\033[100m                                      WorkSlate \033[0m\n");
    for(int i=0; i< 16; i++) {
        printf("\033[100m \033[0m                                              \033[100m \033[0m\n");
    }
    printf(        "\033[100m          ┃        ┃        ┃        ┃          \033[0m\n");
//  printf("\033[92m\033[100m Calc     ┃Finance ┃ Memo   ┃ Phone  ┃ Time     \033[0m\n");  // FG=green, BG=gray
    printf("\033[92m\033[100m Calc     \033[30m┃\033[92mFinance \033[30m┃\033[92m Memo   \033[30m┃\033[92m Phone  \033[30m┃\033[92m Time     \033[0m\n");  // FG=green, BG=gray
    printf(        "\033[100m F1       ┃F2      ┃ F3     ┃ F4     ┃ F5       \033[0m\n");
    printf(        "\033[100m                                                \033[0m\n");
    printf(        "\033[100m F6       ┃F7                                   \033[0m\n");
//  printf("\033[93m\033[100m Options  ┃Worksheet                            \033[0m\n");  // FG=yellow, BG=gray
    printf("\033[93m\033[100m Options  \033[30m┃\033[93mWorksheet                            \033[0m\n");  // FG=yellow, BG=gray
    printf("\nSpecial Key:    Z     X      V     B    N       M\n");
    printf(  "Action:         Find  Print  Save  Get  Recalc  Switch\n");
    printf(  "We map to ctrl- F     P      S     G    R       W\n");
    printf(  "                                                 \n");
    printf(  "Also:  * is ctrl-A    / is ctrl-D    ≠ is ctrl-N\n");
}
#endif   // USE_ANSI_XY
#endif   // WASM

unsigned char lcd_cmd = 0x00;
uint16_t lcd_cursor_addr = 0x0000;

#define LCD_RAMSIZE 2048
unsigned char lcd_ram[LCD_RAMSIZE];

static void write_lcd_instr(unsigned char data)  // RS = 1
{
#if !WASM && USE_ANSI_XY
    static int first_run = 1;
    if(first_run) {
        init_ansi_screen();
        first_run = 0;
    }
#endif
    lcd_cmd = data;
    switch (lcd_cmd) {
        case 0x00:  // known command - set mode
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x08:
        case 0x09:
        case 0x0a:  // known command
        case 0x0b:  // known command
        case 0x0c:  // Write display data
        case 0x0d:  // Read display data
        case 0x0e:  // clear bit
        case 0x0f:  // set bits
            break;
        default:
            printf("LCD CMD %02x\n", data);
    }
}

static unsigned char read_lcd_instr(void)  // RS = 1
{
    return 0;  // busy flag never set
}

#ifndef WASM  // console version
static void move_cursor(uint16_t p)
{
#if USE_ANSI_XY
    int y = p/46;
    int x = p%46;
  #if USE_HEXVIEW
    x = x * 3;
  #endif
    printf("\033[%d;%dH", y+2, x+2);  // extra +1 for border
#endif
}
#else  // WASM version is in workslate-wasm.cpp
extern void move_cursor(uint16_t p);
#endif

#ifndef WASM  // console version
static void show_cursor(int show)
{
#if USE_ANSI_XY
    if (show) {
        printf("\033[?25h"); // show cursor
    } else {
        printf("\033[?25l"); // hide cursor
    }
    fflush(stdout);
#endif    
}
#else  // WASM version is in workslate-wasm.cpp
extern void show_cursor(int show);
#endif

#ifndef WASM  // console version
void write_char_at_position(unsigned char data, uint16_t p)
{
#if USE_ANSI_XY
    if (data & 0x80) {  // inverse mode
        printf("\033[7m");
    }
#endif
    move_cursor(p);
    unsigned char c = data & 0x7f;
#if USE_HEXVIEW
    if (c < 0x20) {
        printf("%02x ", c);
    } else {
        printf(".%c ", c);
    }
#else
    if (c < 0x20) {
        if(iso_font[c] <= 0xFF) {
            printf("%c", iso_font[c]);
        } else if(iso_font[c] <= 0xFFFF) {
            printf("%c%c", iso_font[c]>>8, iso_font[c] & 0xFF);
        } else if(iso_font[c] <= 0xFFFFFF) {
            printf("%c%c%c", iso_font[c]>>16, iso_font[c]>>8, iso_font[c] & 0xFF);
        } else {
            printf("%c%c%c%c", iso_font[c]>>24, iso_font[c]>>16, iso_font[c]>>8, iso_font[c] & 0xFF);
        }
    } else {
        printf("%c", c);
    }
#endif
#if USE_ANSI_XY
    if (data & 0x80) {  // inverse mode
        printf("\033[27m");
    }
#endif
    fflush(stdout);
}
#else  // WASM version is in workslate-wasm.cpp
extern void write_char_at_position(unsigned char data, uint16_t p);
#endif

static void write_lcd_data(unsigned char data)  // RS = 0
{
    switch (lcd_cmd) {
        case 0x00:           // set mode, blinks, cursor.  Mainly 0x31 (cursor off) or 39 (character blink)
            show_cursor(data & 0x08);
            break;
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x08:
        case 0x09:
            break;
        case 0x0a:  // Set Cursor Address (Low Order) (RAM Write Low Order Address)
            lcd_cursor_addr = (lcd_cursor_addr & 0xFF00) | data;
            // don't move cursor. Spec sheet says always update High, never low alone.
            // if we update now, we could set an illegal address (artifacts on screen)
            break;
        case 0x0b:  // Set Cursor Address (High Order) (RAM Write High Order Address)
            lcd_cursor_addr = (lcd_cursor_addr & 0x00FF) | ((unsigned short) data << 8);
            move_cursor(lcd_cursor_addr);
#if 0 //  !USE_ANSI_XY
            int y = lcd_cursor_addr/46;  // print this only for commands, not each normal movement
            int x = lcd_cursor_addr%46;
            printf("\n[cursor %d,%d  pc=%d.%04x]", x, y, get_bank(), pc);
#endif
            break;
        case 0x0c:  // Write Display Data
            lcd_ram[lcd_cursor_addr % LCD_RAMSIZE] = data;
            write_char_at_position(data, lcd_cursor_addr);
            lcd_cursor_addr++;
            break;
        case 0x0e:  // clear bit
            lcd_ram[lcd_cursor_addr % LCD_RAMSIZE] &= ~(1<< (data & 7));
            write_char_at_position(lcd_ram[lcd_cursor_addr % LCD_RAMSIZE], lcd_cursor_addr);
            lcd_cursor_addr++;
            break;
        case 0x0f:  // set bits
            lcd_ram[lcd_cursor_addr % LCD_RAMSIZE] |= 1<< (data & 7);
            write_char_at_position(lcd_ram[lcd_cursor_addr % LCD_RAMSIZE], lcd_cursor_addr);
            lcd_cursor_addr++;
            break;
        default:
            //  case 0x0d:  // Read display data -- we don't expect a write after this command
            printf("  CMD %02x, ARG %02x\n", lcd_cmd, data);
            break;
    }
}

static unsigned char read_lcd_data(void)  // RS = 0
{
    switch (lcd_cmd) {
        case 0x0d:  // Read display data
            return lcd_ram[(lcd_cursor_addr++ - 1) % LCD_RAMSIZE];  // the -1 doesn't make sense, but seems to work
            break;
        default:
            printf("READ LCD DATA, CMD %02x\n", lcd_cmd);
            break;
    }
    return 0;
}

/////////////////////////////
// TAPE PLA simulation
//
// There are three registers:


uint8_t reg_CSTAPER0;
uint8_t reg_CSTAPEW0;
uint8_t reg_CSTAPEW1;

// Bit 0x04 - 1=no tape, 0=tape
// Bit 0x08 - 1=can record, 0=can't record
uint8_t read_CSTAPER0(void)
{
#if 0 // test code
    static uint32_t z = 270000;
    if (z>1) { 
        z--;
    } else if (z==1) {
        printf("\n\n\ntriggering CSTAPER0\n");
//        reg_CSTAPER0 = reg_CSTAPER0 | 0x02;
        reg_CSTAPER0 = 0x00;
        // 0x04 = no tape, can't record
        // 0x08 = tape, can record
        // 0x00 = tape, can't record
        // 0x0C = tape, can't record
//        ram[ADDR_PORT2] |= 0x01;
        z--;  // z=0 --> don't trigger again
    }
#endif
    // logit("Read reg_CSTAPER0 - value ", reg_CSTAPER0);
    return reg_CSTAPER0;
}
void write_CSTAPEW0(uint8_t data)
{
    logit("Write reg_CSTAPEW0 - value ", data);
    reg_CSTAPEW0 = data;
}

void write_CSTAPEW1(uint8_t data)
{
    logit("Write reg_CSTAPEW1 - value ", data);
    reg_CSTAPEW0 = data;
}


/////////////////////////////
// Generic expander port (serial port) emulation
//
// TODO: no overflow detection
#define SERIAL_FIFO_DEPTH 32
#define SERIAL_CYCLES_DELAY  (E_CLOCK_FREQUENCY / (9600/11))  // 9600 baud, 11 bits/char
uint32_t serial_rx_fifo_head = 0;
uint32_t serial_rx_fifo_tail = 0;
uint32_t serial_cycles_until_next_char = 0;
uint32_t serial_rx_fifo[SERIAL_FIFO_DEPTH];

int test_serial_rx_fifo_empty(void) {  // internal use only
    return serial_rx_fifo_head == serial_rx_fifo_tail;
}

int test_serial_rx_fifo_has_character(void)
{
    if (test_serial_rx_fifo_empty()) {
        return 0;   // fifo empty
    }
    if (serial_cycles_until_next_char > 0) {
        serial_cycles_until_next_char--;
        return 0;  // we are waiting for next char to be ready...
    }
    return 1;  // ready!
}

void push_serial_rx_fifo(uint8_t c)
{
    if(test_serial_rx_fifo_empty()) {
        // kick off timer for first character
        serial_cycles_until_next_char = SERIAL_CYCLES_DELAY;
    }

    serial_rx_fifo[serial_rx_fifo_head] = c;
    serial_rx_fifo_head = (serial_rx_fifo_head + 1) % SERIAL_FIFO_DEPTH;
}

uint8_t pull_serial_rx_fifo(void)
{
    if (serial_rx_fifo_head == serial_rx_fifo_tail) {  // no character for real
        return 0;
    }
    uint8_t retval = serial_rx_fifo[serial_rx_fifo_tail];
    serial_rx_fifo_tail = (serial_rx_fifo_tail + 1) % SERIAL_FIFO_DEPTH;

    if(!test_serial_rx_fifo_empty()) {
        // kick off timer for next character
        serial_cycles_until_next_char = SERIAL_CYCLES_DELAY;
    }
    return retval;
}

/////////////////////////////
// Printer Emulation
//

/////////////////////////////
// I/O Box Emulation (serial and parallel port)
//
// Parallel port didn't respond ... might be that printer wasn't connected?

const char ID_string_serial[]= {0x32, 0x20, 0x49, 0x2F, 0x4F, 0x20, 0x42, 0x6F, 0x78, 0x2E, 0x0D, 0x0A};
                            // “2 I/O Box.\r\n”    [serial port selected, not parallel]

void wk2serial(uint8_t ch)
{
    if (ch == 0x05) {   // respond to ENQ
        for (unsigned int i=0; i< sizeof(ID_string_serial); i++) {
            push_serial_rx_fifo(ID_string_serial[i]);
        }
    }
    else
    {
        push_serial_rx_fifo(ch);  // echo back (testing)
    }
}


/////////////////////////////
// Power Emulation
//
// Power on is handled by hardware and is basically a reset.
// Power off is requested by the software
//
// Note: power is only supported for WASM mode.  This is because
// in terminal mode, we only scan the unix keyboard when the software request it.
// When powered off (SLP mode), we don't scan that so we can't turn on.

int power_is_on = 0;

void power_off_requested(void)
{
    // clear screen
    show_cursor(0);
    for(uint16_t p=0; p<46*16; p++) {
        write_char_at_position(' ', p);
    }
    // next CPU instruction will be SLP, so let CPU emulator handle that.
    power_is_on = 0;
}

void power_on_requested(void)
{
    power_is_on = 1;
    
    // clear keyboard fifo
    clear_kbd_fifo();

    // reset
    workslate_hw_reset();
    pc = ((mread(0xFFFE) << 8) + mread(0xFFFF));
}

/////////////////////////////
// Keyboard
//
// This machine has a couple of key-mapping quirks
// - There are two '.' keys -- one for the numpad, one near the letters.
//   We only emulate the numpad one.
// - There are two ways to type '-' ... the subtract key and Special-I.
//   I think the special-I was meant to be an underscore, but it shows and acts like -
// - We've got four non-ascii keys:
//     multiply  -- mapped to '*' since that's what people use. To type '*', press ctrl-A
//     divide    -- mapped to '/' since that's what people use. To type '/', press ctrl-D
//     non-equal -- mapped to ctrl-N
//     CT        -- mapped to ???
//     solid box -- mapped to ???
//
// It would be nice to map all the Special key to the Alt key, but OSX maps the
// Alt (Option) keys to specific special characters (like Ω ≈ ç √ ) which isn't too portable.
//
// Special Key:   Z     X      V     B    N       M
// Action:       Find  Print  Save  Get  Recalc  Switch
// We map to:     F     P      S     G    R       W
//
// TODO:
//  - special unmarked key commands
//       special-cancel key is mentioned when using the terminal
//       special-L = big filled square ... prints as an inverted space?
//
//----------------------------
// https://viewsourcecode.org/snaptoken/kilo/02.enteringRawMode.html
//
// Matrix from documentation:
//   01234567  <-- Output bit.   Input is read on left column
//  +--------
// 0|qwertyui
// 1|89###po 
// 2| 654: L 
// 3|########
// 4|asdfghjk
// 5|## #####
// 6|123-+=.0
// 7| zxcvbnm;


#define KBD_FIFO_DEPTH 32
uint32_t kbd_fifo_head = 0;
uint32_t kbd_fifo_tail = 0;
uint32_t kbd_fifo[KBD_FIFO_DEPTH];  // 00 00 pp rr  (pp=probe, rr=response)

void push_kbd_fifo(uint32_t f)
{
    // Power key is acted on immediately & not pushed in to fifo
    if(f & 0x40000) {  // if power key
        if(power_is_on) {
            // tell software the user wants to shut down
            ram[ADDR_PORT1] = ram[ADDR_PORT1] & 0xFB;  // clear bit 2
        } else {
            power_on_requested();
        }
        return;
    }

    kbd_fifo[kbd_fifo_head] = f;
    kbd_fifo_head = (kbd_fifo_head + 1) % KBD_FIFO_DEPTH;
}

void clear_kbd_fifo(void)
{
    kbd_fifo_head = 0;
    kbd_fifo_tail = 0;
}

#ifndef WASM
static void kbd_read_terminal(void)
{
    int flags;
    unsigned char c1,c2,c3,c4,c5;
    uint64_t inkey;
    int rtn;

    flags = fcntl(fileno(stdin), F_GETFL);
    if (flags == -1) {
        perror("fcntl error");
        exit(-1);
    }
    fcntl(fileno(stdin), F_SETFL, flags | O_NONBLOCK);
    // TODO: don't read if we don't have room in FIFO
    rtn = read(fileno(stdin), &c1, 1);

    if(rtn != 1) {
        fcntl(fileno(stdin), F_SETFL, flags);
        return;  // no key press
    }

    inkey = c1;
    if(c1 == 0x1b) {  // if ESC, then check if just an ESC or an 3-char escape sequence
        rtn = read(fileno(stdin), &c2, 1);
        if(rtn == 1) {
            inkey = (inkey << 8) | c2;  // second char of sequence
            rtn = read(fileno(stdin), &c3, 1);
            if(rtn == 1) {
                inkey = (inkey << 8) | c3;  // third char of sequence
                rtn = read(fileno(stdin), &c4, 1);
                if(rtn == 1) {
                    inkey = (inkey << 8) | c4;  // fourth char of sequence
                    rtn = read(fileno(stdin), &c5, 1);
                    if(rtn == 1) {
                        inkey = (inkey << 8) | c5;  // fifth char of sequence
                    }
               }
            }
        }
    }
    fcntl(fileno(stdin), F_SETFL, flags);

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
//      case ???: f = 0x0480; special_key = 1; break;  // special-C = CT char (displays as 0x1c)
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
//      case ???: f = 0x8004; special_key = 1; break;  // special-L is a big solid box
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
        case 0x7F:f = 0x2002; break;  // backspace
        case 0x1b:f = 0x0208; break;  // ESC --->Cancel
        case 0x1b5b41:f = 0x1020; break;  // up
        case 0x1b5b42:f = 0x8020; break;  // down
        case 0x1b5b43:f = 0x2020; break;  // right
        case 0x1b5b44:f = 0x4020; break;  // left
        case     0x1b4f50:f = 0x0408; break;  // F1 -->    Calc soft key
        case     0x1b4f51:f = 0x0808; break;  // F2 --> Finance soft key
        case     0x1b4f52:f = 0x1008; break;  // F3 -->    Memo soft key
        case     0x1b4f53:f = 0x4008; break;  // F4 -->   Phone soft key
        case 0x1b5b31357e:f = 0x2008; break;  // F5 -->    Time soft key

        case 0x1b5b31377e:f = 0x8008; break;  // F6 -->      Options key
        case 0x1b5b31387e:f = 0x0120; break;  // F7 -->    Worksheet key
        // "Special" key is       0220 but it acts like a shift key
        default: printf("[UNKNOWN KEY IN %02llX]", inkey); fflush(stdout);
    }
    if (shift_key) {
        // jam in just the shift key being pressed
        push_kbd_fifo(0x10000);    // bit 16 is the shift key
        // then jam in the combo next
        f = f | 0x10000;
    }
    if (special_key) {
        // jam in just the special key being pressed
        push_kbd_fifo(0x20000);  // bit 17 is the special key
        // then jam in the combo next
        f = f | 0x20000;
    }
    if (f) {  // stuff in fifo
        push_kbd_fifo(f);
    }
    // stuff no-keys-pressed in fifo
    push_kbd_fifo(0);
}
#endif



unsigned char read_kbd(unsigned char scan_enable)
{
    static int timer = 0;

#ifndef WASM
    kbd_read_terminal();
#endif
    if(kbd_fifo_head == kbd_fifo_tail) {
        return 0;  // fifo empty
    }
    if(timer++ > 200) {
        // this key has been read enough; time to move on to next key
        kbd_fifo_tail = (kbd_fifo_tail + 1) % KBD_FIFO_DEPTH;
        timer = 0;
        if(kbd_fifo_head == kbd_fifo_tail) {
            return 0;  // fifo empty
        }
    }
    // Let this key be pressed for 200 reads
    // At first the scan_enable is 0xFF to check for any key pressed.
    // Then it is a single bit to narrow in on exact character(s)
    unsigned char retval = 0;
    uint8_t shift_key_pressed = (kbd_fifo[kbd_fifo_tail] >> 16) & 1;
    uint8_t special_key_pressed = (kbd_fifo[kbd_fifo_tail] >> 16) & 2;
    uint8_t probe_column      = kbd_fifo[kbd_fifo_tail] >> 8;
    uint8_t response_row      = kbd_fifo[kbd_fifo_tail] & 0xFF;
    retval = (scan_enable & probe_column) ? response_row : 0;
    if(shift_key_pressed && (scan_enable & 0x01)) {
        retval |= 0x08;   // code for shift is 0x0108, but only appears when scanning its column
    }
    if(special_key_pressed && (scan_enable & 0x02)) {
        retval |= 0x20;   // code for "special" is 0x0220, but only appears when scanning its column
    }
    return retval;
}

/////////////////////////////
//
// MEMORY ACCESS FUNCTIONS
//
//

unsigned char get_bank(void)
{
    return ram[ADDR_PORT1] & 0x03;
}

// Tags for stack, to help decode what register was pushed:
// Valid tags are:  P=PC, A B X. F=flags(CC) 

char tagread(unsigned short addr)
{
    return (addr < TAGSIZE) ? ram_tag[addr] : '\0';
}
void tagwrite(unsigned short addr, char tag)
{
    if (addr < TAGSIZE) {
        ram_tag[addr] = tag;
    }
}

/* All memory reads go through this function */
unsigned char mread_raw(unsigned short addr)
{
    uint8_t ch;

    if((addr >= 0x80) && (addr < RAMSIZE)) {
        return ram[addr];
    } else if (addr >= ROMSTART) {
        int bank = ram[ADDR_PORT1] & 0x03;
        switch(bank) {
            case 3:  return rom_u16[addr - ROMSTART];  // starts with U16
            case 2:  return rom_u15[addr - ROMSTART];  // other rom
            case 1:  return rom_u14[addr - ROMSTART];  // unknown if really here
            case 0:  return 0xFF;
        }
    } else if ((addr >= ADDR_RTC_START) && (addr <= ADDR_RTC_END)) {
        // printf("RTC MEMORY READ  - addr %04x\n", addr);
        return read_rtc(addr - ADDR_RTC_START);
    } else {  // it's IO
        // printf("I/O MEMORY READ  - addr %04x\n", addr);
        switch(addr) {
            case ADDR_PORT2:     case ADDR_PORT3:     case ADDR_PORT4:
                                 case ADDR_PORT3_DDR: case ADDR_PORT4_DDR:
            case ADDR_RMCR:    // SCI Rate and Mode Control Register  (only lower 4 bits)
            case ADDR_OCHR:      case ADDR_OCLR:     // Output compare register
            case ADDR_RAMCTRL:
                return ram[addr];
            case ADDR_PORT1_DDR: case ADDR_PORT2_DDR:
                return 0xFF; // sometimes used as a trick to inc/dec D register - must be $FF for this write-pnly reg.
                // Workslate doesn't seem to use this trick.
            case ADDR_TAPE_PLA_2C:
                return read_CSTAPER0();
            case ADDR_PORT1:
                // return (ram[addr] & 0x63) | 0x04;  // mask out read-only and force bit 2 high (keep power on).  Change if we want to power off.
                return ram[addr] | 0x08; // Force ring-indicator off (TODO: make read-only bits of PORT1 seperate from write parts)
            case ADDR_TRCSR:   // Transmit/Receive Control and Status Register
                deassert_irq(ADDR_SCI_VECTOR);  // not exactly by the book - disables too early!
                return (ram[addr] | 0x20);  // ok to TX
            case ADDR_SCRDR:  // SCI Receiver Data Register
                ram[ADDR_TRCSR] = ram[ADDR_TRCSR] & 0x7F;  // clear RX ready bit
                ch = pull_serial_rx_fifo();
                logit("RX Char", ch);
                return ch;
            // Timer
            case ADDR_CHR:     // 0x09                 // free-running counter
                ram[ADDR_TCSR] &= ~0x20;               // clar TOF flag (actually requires TCSR read first but we assume that happened)
                deassert_irq(ADDR_TOF_VECTOR);
                ram[ADDR_CLR] = Timer_Counter & 0xFF;  // latch LSB
                return Timer_Counter >> 8;             // return MSB
            case ADDR_CLR:     // 0x0A
                return ram[addr];                      // return latched value
            case ADDR_TCSR:   // TODO
                return ram[addr];
            // Keyboard
            case ADDR_KBD:
                return read_kbd(ram[ADDR_KBD]);
            case ADDR_LCD_DATA:
                return read_lcd_data();
            case ADDR_LCD_INSTR:
                return read_lcd_instr();
#if RAM_SIZE <= ADDR_RAMTEST_ADDRESS
            case ADDR_RAMTEST_ADDRESS:  // probed to see if there is memory
                return 0xFF;
#endif
        }
    }
    printf("I/O MEMORY READ  - addr %04x\n", addr);
    stop = 1;  // undefined memory read
    return 0xFF;
}

unsigned char mread(unsigned short addr)
{
    advance_cycle(); // count cycles & operate timers
    return mread_raw(addr);
}

/* All memory writes go through this function */
void mwrite(unsigned short addr, unsigned char data)
{
    advance_cycle(); // count cycles & operate timers
    if((addr >= 0x80) && (addr < RAMSIZE)) {
        ram[addr] = data;  // RAM Write
        return;
    } else if (addr >= ROMSTART) {
        // stop = 1;       // ROM write
        return;
    } else if ((addr >= ADDR_RTC_START) && (addr <= ADDR_RTC_END)) {
        // printf("RTC MEMORY WRITE - addr %04x val %02x\n", addr, data);
        write_rtc(addr - ADDR_RTC_START, data);
        return;
    } else {               // IO write
        // printf("I/O MEMORY WRITE - addr %04x val %02x\n", addr, data);
        switch(addr) {
                                 case ADDR_PORT2:     case ADDR_PORT3:     case ADDR_PORT4:
            case ADDR_PORT1_DDR: case ADDR_PORT2_DDR: case ADDR_PORT3_DDR: case ADDR_PORT4_DDR:
            case ADDR_RMCR:  // SCI Rate and Mode Control Register  (only lower 4 bits)
            case ADDR_RAMCTRL:
            case ADDR_KBD:
            case ADDR_DTMF:
                ram[addr] = data;
                break;
            case ADDR_PORT1:
                // bit 2 is read-only (0=user requested power off)
                ram[addr] = (ram[addr] & 0x04) | (data & 0xFB);
                break;
            case ADDR_TRCSR:   // Transmit/ Receive Control and Status Register
                logit("Write TRCSR", data);
                ram[addr] = data;
                break;
            case ADDR_SCTDR: // SCI Transmit Data Register - used with "Print" command
                logit("TX Char", data);
#if 0
                FILE *ff = fopen("tx_log.txt","a");
                fprintf(ff, "%c", data);
                fclose(ff);
#endif
                wk2serial(data);  // assume connected to serial port, not printer TODO: multiplex
                // assume that it goes out immediately ... so check if tx interrupt enabled
                if(ram[ADDR_TRCSR] & 0x04) {
                    assert_irq(ADDR_SCI_VECTOR);
                }
                break;
            case ADDR_TAPE_PLA_2C:
                write_CSTAPEW0(data);
                break;
            case ADDR_TAPE_PLA_2D:
                write_CSTAPEW1(data);
                if(data & 0x40) { // if power-off requested
                    power_off_requested();
                }
                break;
            // Timer
            case ADDR_CHR:     // 0x09 -- free-running counter
                Timer_Counter = 0xFFF8; // we can only reset to this value with any write
                break;
            case ADDR_CLR:     // 0x0A -- ignore writes to LSB
                break;
            case ADDR_TCSR:
                ram[addr] = data;
                break;
            case ADDR_OCHR:      case ADDR_OCLR:     // Output compare register
                ram[addr] = data;
                Timer_OutputCompare = (((unsigned short) ram[ADDR_OCHR]) << 8)
                                      | ram[ADDR_OCLR];
                ram[ADDR_TCSR] &= ~0x40;             // clear OCF flag (actually requires TCSR read first but we assume that happened)
                deassert_irq(ADDR_OCF_VECTOR);
                break;
            // LCD
            case ADDR_LCD_DATA:
                write_lcd_data(data);
                break;
            case ADDR_LCD_INSTR:
                write_lcd_instr(data);
                break;
            case ADDR_RAMTEST_ADDRESS:  // probed to see if there is memory
                break;
            default:
                printf("I/O MEMORY WRITE - addr %04x val %02x\n", addr, data);
                stop = 1;  // undefined memory write
                return;
        }
    }
}




/* All jumps go through this function */
unsigned short pull2();
void jump(unsigned short addr)
{
    /* don't simulate any functions yet */
    pc = addr;
    return;

#if 0
    // example code from exorsim:
    switch (addr) {
        case 0xF9CF: case 0xF9DC: /* Output a character */ {
            term_out(acca);
            /* putchar(acca); fflush(stdout); */
            c_flag = 0; /* Carry is error status */
            break;
        }
        } default: {
            pc = addr;
            return;
        }
    }
    simulated(addr);
    addr = pull2();
    jump(addr);
#endif
}

void workslate_hw_reset(void)
{
    // printf("DEBUG - reached %s at " __FILE__ ":%d\n", __FUNCTION__, __LINE__);
    ram[ADDR_PORT1] = 0x07;  // start in bank 0 (u16.bin).  Power requested on (bit 2)
    power_is_on = 1;

    memset(ram_tag, 0, TAGSIZE);

    // set RTC to illegal time so firmware sets it to default date
    rtc_mem[0]=255;  // seconds
    rtc_mem[2]=255;  // minutes
    rtc_mem[4]=255;  // hours

    // PLA
    reg_CSTAPER0 = 0x04; // 0x04 means tape head in position to pass self test

    // Timer:
    // mwrite(ADDR_TRCSR, 0x20);
    mwrite(ADDR_TCSR, 0x00);
    Timer_Counter = 0x0000;
    Timer_OutputCompare = 0xFFFF;
}
