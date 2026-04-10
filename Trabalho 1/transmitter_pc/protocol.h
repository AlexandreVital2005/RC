#ifndef PROTOCOL_H
#define PROTOCOL_H

#define BUF_SIZE         4096
#define MAX_FRAME_SIZE   8192
#define DATA_CHUNK_SIZE  512
#define MAX_RETRANS      3
#define TIMEOUT_SECS     3

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

#define APP_DATA  0x01
#define APP_START 0x02
#define APP_END   0x03

#define T_FILESIZE 0x00
#define T_FILENAME 0x01

#endif
