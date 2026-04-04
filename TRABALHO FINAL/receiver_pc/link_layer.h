#ifndef LINK_LAYER_H
#define LINK_LAYER_H

#include <termios.h>

#define FLAG   0x7E
#define ESC    0x7D
#define ESC_XOR 0x20

#define A_TX   0x03
#define A_RX   0x01

#define C_SET  0x03
#define C_UA   0x07
#define C_DISC 0x0B
#define C_I0   0x00
#define C_I1   0x40

#define C_RR0  0x05
#define C_RR1  0x85
#define C_REJ0 0x01
#define C_REJ1 0x81

typedef enum {
    S_START,
    S_FLAG,
    S_A,
    S_C,
    S_BCC1_OK,
    S_DATA
} State;

int llopen(const char *serialPort, int role);
int llwrite(int fd, const unsigned char *buffer, int length);
int llread(int fd, unsigned char *buffer);
int llclose(int fd, int role);

#endif