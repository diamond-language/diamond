#ifndef WASM_STUB_TERMIOS_H
#define WASM_STUB_TERMIOS_H
typedef unsigned tcflag_t; typedef unsigned char cc_t; typedef unsigned speed_t;
struct termios { tcflag_t c_iflag, c_oflag, c_cflag, c_lflag; cc_t c_cc[32]; speed_t c_ispeed, c_ospeed; };
enum { ECHO = 8, ICANON = 2, ISIG = 1, TCSANOW = 0, TCSAFLUSH = 2, VMIN = 6, VTIME = 5 };
int tcgetattr(int, struct termios *);
int tcsetattr(int, int, const struct termios *);
#endif
