/* Controls */

extern int skip;
extern int trace;
extern int stop;
extern int reset;
extern int abrt;    // 0 or 1, non-maskable
extern int sp_stop;
extern uint32_t cycles_simulated_this_tick;

/* extern int brk; */
extern int hasbrk;	/* JMR20201103 'brk' conflicts with unistd library. */
extern unsigned short brk_addr;

/* CPU registers */

extern unsigned char acca;
extern unsigned char accb;
extern unsigned short ix;
extern unsigned short pc;
extern unsigned short sp;
extern unsigned char c_flag;
extern unsigned char v_flag;
extern unsigned char z_flag;
extern unsigned char n_flag;
extern unsigned char i_flag; /* 1=masked, 0=enabled */
extern unsigned char h_flag;

unsigned char read_flags();
void write_flags(unsigned char f);

/* Simulate */

void sim(uint32_t cycles_to_simulate);

void simulated(unsigned short addr); /* For exor.c, JMR20201103 */

/* Dump trace buffer */
void show_traces(int n);

/* Provided externally */

void jump(unsigned short addr);
unsigned char get_bank(void);
unsigned char mread(unsigned short addr);
unsigned char mread_raw(unsigned short addr); // like mread, but doesn't advance cycle
void mwrite(unsigned short addr, unsigned char data);
void monitor(void);

/* for stack tracing */
char tagread(unsigned short addr);
void tagwrite(unsigned short addr, char tag);

/* interrupts */
void assert_irq(uint16_t vec);
void deassert_irq(uint16_t vec);
