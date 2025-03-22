int skipws(char **at_p);
int match_word(char **at_p, const char *word);
int parse_word(char **at_p, char *buf);
int parse_hex(char **at_p, int *hex);
int parse_hex1(char **at_p, int *hex);
int parse_hex2(char **at_p, int *hex);
int parse_hex4(char **at_p, int *hex);
int parse_dec(char **at_p, int *dec);

int jgetline(FILE *f, char *buf);
int hatoi(unsigned char *buf);
void hd(FILE *out, unsigned char *mem, int start, int len);  // assumes memory is in RAM
void hd2(FILE *out, int start, int len);  // uses mread to also read registers
int fields(char *buf, char *words[]);
char *jstrcpy(char *d, char *s);

void sim_termios(void);
void save_termios(void);
void restore_termios(void);
void sig_termios(void);
void nosig_termios(void);
int jstricmp(const char *d, const char *s);


#ifdef __MACH__    // OSX only:
#include <time.h>
// https://github.com/ChisholmKyle/PosixMachTiming/blob/master/src/timing_mach.h

#define TIMING_GIGA (1000000000)

/* timespec difference (monotonic) right - left */
inline void timespec_monodiff_rml(struct timespec *ts_out,
                                    const struct timespec *ts_in) {
    /* out = in - out,
       where in > out
     */
    ts_out->tv_sec = ts_in->tv_sec - ts_out->tv_sec;
    ts_out->tv_nsec = ts_in->tv_nsec - ts_out->tv_nsec;
    if (ts_out->tv_sec < 0) {
        ts_out->tv_sec = 0;
        ts_out->tv_nsec = 0;
    } else if (ts_out->tv_nsec < 0) {
        if (ts_out->tv_sec == 0) {
            ts_out->tv_sec = 0;
            ts_out->tv_nsec = 0;
        } else {
            ts_out->tv_sec = ts_out->tv_sec - 1;
            ts_out->tv_nsec = ts_out->tv_nsec + TIMING_GIGA;
        }
    } else {}
}

// May not be needed... this logic looks like it's already in the above function
inline int time_is_past_time(const struct timespec *a, const struct timespec *b)  // if a>b
{
    if(a->tv_sec > b->tv_sec) {
        return 1;
    }
    if(a->tv_sec < b->tv_sec) {
        return 0;
    }
    // seconds are equal... use nano to break the tie
    if(a->tv_nsec > b->tv_nsec) {
        return 1;
    }
    return 0;
}

/* emulate clock_nanosleep for CLOCK_MONOTONIC and TIMER_ABSTIME */
inline int clock_nanosleep_abstime ( const struct timespec *req )
{
    struct timespec ts_delta;
    int retval = clock_gettime ( CLOCK_MONOTONIC, &ts_delta );
    if (retval == 0) {
        if(time_is_past_time(req, &ts_delta)) { // if required time is past now
            timespec_monodiff_rml ( &ts_delta, req );
            retval = nanosleep ( &ts_delta, NULL );
        }
    }
    return retval;
}
#endif