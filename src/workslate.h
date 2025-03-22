// these two defined in mon.c
extern FILE *mon_out;
extern FILE *mon_in;

//extern unsigned char mem[65536];
extern unsigned char mread(unsigned short addr);
extern void mwrite(unsigned short addr, unsigned char data);
extern void workslate_hw_reset(void);
extern int lower;
extern int polling;
