/*   Workslate WK-100 Emulator
 *   Copyright (C) 2024 John Maushammer
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>    /* JMR20201103 */

#include "exorsim.h"    /* JMR20201121 */
#include "sim6800.h"
#include "unasm6800.h"    /* JMR202021103 */
#include "workslate.h"
#include "exorterm.h"
#include "utils.h"    /* JMR20201103 */

/* Options */

const char *rom_dir; /*  = "resource"; */
int lower = 0; /* Allow lower case - not used for workslate, but by some linked files */


/* Memory */
#define RAMSIZE 0x4000     // code looks like it wouild support 32kB! but not tested
extern unsigned char ram[RAMSIZE];
#define ROMSTART 0x8000    // 0x8000-FFFF
extern unsigned char rom_u14[0x8000]; // not used
extern unsigned char rom_u15[0x8000];
extern unsigned char rom_u16[0x8000]; // boot bank
#ifdef WASM
extern unsigned char resource_u15_bin[];
extern unsigned char resource_u16_bin[];
#endif


int pending_read_ahead = 1;
unsigned char read_ahead_c;

//int count = 10;

int polling = 1; /* Allow ACIA polling */


int load_rom(unsigned char * base, char * filename)
{
    char tempfilename[256];

    snprintf(tempfilename, 250, "%s/%s", rom_dir, filename);
    FILE *f = fopen(tempfilename, "rb");
    if (!f) {
        fprintf(stderr, "Couldn't load '%s'\n", tempfilename);
        return -1;
    }
    if (1 != fread(base, 32 * 1024, 1, f)) {
        fprintf(stderr, "Couldn't read '%s'\n", tempfilename);
        return -1;
    }
    printf("'%s' loaded.\n", tempfilename);
    fclose(f);

    return 0;
}


void ctrl_c()
{
    printf("\033[?25h"); // show cursor
    printf("\033[%d;%dH", 23+2, 1);  // go to bottom.  Extra +1 for border
    printf("Interrupt!\n");
    stop = 1;
}

#ifndef WASM  // this isn't used for web version
int main(int argc, char *argv[])
{
    mon_out = stdout;
    mon_in = stdin;
    const char *facts_name = "resource/workslate_facts";

    for (int x = 1; x < argc; ++x) {
        if (argv[x][0] == '-') {
            if (!strcmp(argv[x], "--facts") && x + 1 != argc) {
                ++x;
                facts_name = argv[x];
            } else if (!strcmp(argv[x], "--trace")) {
                trace = 1;
            } else if (!strcmp(argv[x], "--6800")) {
                cputype = 0x6800;
            } else if (!strcmp(argv[x], "--mon")) {
                stop = 1;
            } else if (!strcmp(argv[x], "--skip") && x + 1 != argc) {
                ++x;
                skip = atoi(argv[x]);
            } else if (!strcmp(argv[x], "--romdir") && x + 1 != argc) {
                rom_dir = argv[++x];
            } else {
                printf("Workslate simulator\n");
                printf("\n");
                printf("workslatesim [options]\n");
                printf("\n");
                printf("  --trace       Produce instruction trace on stderr\n");
                printf("  --skip nnn    Skip first nnn insns in trace\n");
                printf("  --6800        Simulate only 6800 instructions\n");
                printf("  --romdir dir  Give name for ROM directory if not 'resource'\n");
                printf("  --facts file  Process facts files for commented disassembly\n");
                printf("  --lower       Allow lowercase\n");
                printf("  --mon         Start at monitor prompt\n");
                printf("\n");
                exit(-1);
            }
        }
    }

    /* Load facts file */
    if (facts_name) {
        FILE *f;
        printf("Load facts file '%s'\n", facts_name);
        f = fopen(facts_name, "r");
        if (f) {
            parse_facts(f);
            fclose(f);
        } else {
            printf("Couldn't load '%s'\n", facts_name);
        }
    }

    /* Default memory image name */
    if (!rom_dir) {
        rom_dir = "resource";
    }

    /* Load initial memory image */
    if (   load_rom(rom_u15, "u15.bin")
        || load_rom(rom_u16, "u16.bin"))
    {
        /* Start halted if there is no ROM */
        stop = 1;
    }

    /* Read starting address from reset vector */
    workslate_hw_reset(); // set bank to start in
    pc = ((mread(0xFFFE) << 8) + mread(0xFFFF));

    /* system("stty cbreak -echo -icrnl"); */
    save_termios();
    sim_termios();

    signal(SIGINT, ctrl_c);
    printf("\nHit Ctrl-C for simulator command line.  Starting simulation...\n");

    izexorterm();

    sim(0);  // simulate with no cycle limit
    // echo test of terminal emulator
    // while (!stop) term_out(term_in());

    /* system("stty cooked echo icrnl"); */
    printf("\033[?25h"); // show cursor
    restore_termios();
    return 0;
}
#endif
