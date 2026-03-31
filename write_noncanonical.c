// Francisco Nunes e Alexandre Vital
// Write to serial port in non-canonical mode
//
// Versao ajustada ao guia do protocolo RCOM

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

// Baudrate settings are defined in <asm/termbits.h>, which is
// included by <termios.h>
#define BAUDRATE B38400
#define _POSIX_SOURCE 1

#define FALSE 0
#define TRUE 1

#define MAX_RETRANS 3
#define TIMEOUT_SECS 3
#define IFRAME_SIZE 9

#define FLAG 0x7E
#define A_TX  0x03
#define A_RX  0x01

// Supervision frames
#define C_SET   0x03
#define C_UA    0x07
#define C_DISC  0x0B
#define C_RR0   0x05
#define C_RR1   0x85
#define C_REJ0  0x01
#define C_REJ1  0x81

// Information frames
#define C_I0    0x00
#define C_I1    0x40

typedef enum {
    START,
    FLAG_RCV,
    A_RCV,
    C_RCV,
    BCC_OK,
    STOP
} State;

// alarme
volatile int alarmEnabled = FALSE;
volatile int alarmCount = 0;

void alarmHandler(int signal)
{
    alarmEnabled = FALSE;
    alarmCount++;
    printf("Alarm #%d received\n", alarmCount);
}

unsigned char rr_for(int nr)
{
    return (nr == 0) ? C_RR0 : C_RR1;
}

unsigned char rej_for(int nr)
{
    return (nr == 0) ? C_REJ0 : C_REJ1;
}

int is_valid_supervision_control(unsigned char c)
{
    return (c == C_UA || c == C_RR0 || c == C_RR1 ||
            c == C_REJ0 || c == C_REJ1 || c == C_DISC || c == C_SET);
}

void build_supervision_frame(unsigned char *frame, unsigned char address, unsigned char control)
{
    frame[0] = FLAG;
    frame[1] = address;
    frame[2] = control;
    frame[3] = address ^ control;
    frame[4] = FLAG;
}

void build_iframe(unsigned char *frame,
                  unsigned char control,
                  const unsigned char *data,
                  int data_len)
{
    unsigned char bcc2 = 0x00;

    frame[0] = FLAG;
    frame[1] = A_TX;
    frame[2] = control;
    frame[3] = frame[1] ^ frame[2];

    for (int i = 0; i < data_len; i++) {
        frame[4 + i] = data[i];
        if (i == 0) bcc2 = data[i];
        else bcc2 ^= data[i];
    }

    if (data_len == 0) bcc2 = 0x00;

    frame[4 + data_len] = bcc2;
    frame[5 + data_len] = FLAG;
}

int receive_supervision_frame(int fd, unsigned char *Aread, unsigned char *Cread)
{
    State state = START;
    unsigned char buf = 0;
    int bytes_read;

    while (state != STOP && alarmEnabled) {
        bytes_read = read(fd, &buf, 1);

        if (bytes_read <= 0) continue;

        switch (state) {
            case START:
                if (buf == FLAG) state = FLAG_RCV;
                break;

            case FLAG_RCV:
                if (buf == FLAG) {
                    state = FLAG_RCV;
                }
                else if (buf == A_TX || buf == A_RX) {
                    *Aread = buf;
                    state = A_RCV;
                }
                else {
                    state = START;
                }
                break;

            case A_RCV:
                if (buf == FLAG) {
                    state = FLAG_RCV;
                }
                else if (is_valid_supervision_control(buf)) {
                    *Cread = buf;
                    state = C_RCV;
                }
                else {
                    state = START;
                }
                break;

            case C_RCV:
                if (buf == FLAG) {
                    state = FLAG_RCV;
                }
                else if (buf == ((*Aread) ^ (*Cread))) {
                    state = BCC_OK;
                }
                else {
                    state = START;
                }
                break;

            case BCC_OK:
                if (buf == FLAG) {
                    state = STOP;
                }
                else {
                    state = START;
                }
                break;

            default:
                state = START;
                break;
        }
    }

    return (state == STOP) ? 1 : 0;
}

int llopen_tx(int fd)
{
    unsigned char set_frame[5];
    unsigned char Aread = 0, Cread = 0;

    build_supervision_frame(set_frame, A_TX, C_SET);

    alarmCount = 0;

    while (alarmCount < MAX_RETRANS) {
        if (!alarmEnabled) {
            printf("Sending SET...\n");
            write(fd, set_frame, 5);
            alarm(TIMEOUT_SECS);
            alarmEnabled = TRUE;
        }

        if (receive_supervision_frame(fd, &Aread, &Cread)) {
            alarm(0);
            alarmEnabled = FALSE;

            printf("Received supervision frame: A=0x%02X C=0x%02X\n", Aread, Cread);

            if (Aread == A_TX && Cread == C_UA) {
                printf("Connection opened successfully (UA received)\n");
                return 1;
            }
        }
    }

    printf("llopen failed: UA not received\n");
    return -1;
}

int llwrite_tx(int fd,
               const unsigned char *payload,
               int payload_len,
               int *sequenceNumber)
{
    unsigned char iframe[IFRAME_SIZE];
    unsigned char Aread = 0, Cread = 0;

    unsigned char control = (*sequenceNumber == 0) ? C_I0 : C_I1;
    build_iframe(iframe, control, payload, payload_len);

    alarmCount = 0;

    while (alarmCount < MAX_RETRANS) {
        if (!alarmEnabled) {
            printf("Sending I frame Ns=%d...\n", *sequenceNumber);
            write(fd, iframe, payload_len + 6);
            alarm(TIMEOUT_SECS);
            alarmEnabled = TRUE;
        }

        if (receive_supervision_frame(fd, &Aread, &Cread)) {
            alarm(0);
            alarmEnabled = FALSE;

            printf("Received supervision frame: A=0x%02X C=0x%02X\n", Aread, Cread);

            // Espera-se resposta do recetor com A=0x03
            if (Aread != A_TX) {
                continue;
            }

            if (Cread == rr_for(1 - *sequenceNumber)) {
                printf("RR correto recebido\n");
                *sequenceNumber = 1 - *sequenceNumber;
                return payload_len;
            }

            if (Cread == rej_for(*sequenceNumber)) {
                printf("REJ recebido, vou retransmitir\n");
                // não incrementa alarmCount aqui; retransmite na próxima iteração após novo alarm
                alarmEnabled = FALSE;
                continue;
            }

            printf("Resposta inesperada, continuo a aguardar/retransmitir\n");
        }
    }

    printf("llwrite failed: no valid RR/REJ within retransmission limit\n");
    return -1;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Incorrect program usage\n"
               "Usage: %s <SerialPort>\n"
               "Example: %s /dev/ttyS1\n",
               argv[0], argv[0]);
        exit(1);
    }

    const char *serialPortName = argv[1];

    int fd = open(serialPortName, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror(serialPortName);
        exit(-1);
    }

    struct termios oldtio;
    struct termios newtio;

    if (tcgetattr(fd, &oldtio) == -1) {
        perror("tcgetattr");
        exit(-1);
    }

    memset(&newtio, 0, sizeof(newtio));
    newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;
    newtio.c_lflag = 0;

    // leitura byte a byte
    newtio.c_cc[VTIME] = 0;
    newtio.c_cc[VMIN] = 0;

    tcflush(fd, TCIOFLUSH);

    if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
        perror("tcsetattr");
        exit(-1);
    }

    printf("New termios structure set\n");

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = alarmHandler;

    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    printf("Alarm configured\n");

    // 1) abrir ligação: SET -> UA
    if (llopen_tx(fd) < 0) {
        tcsetattr(fd, TCSANOW, &oldtio);
        close(fd);
        return -1;
    }

    // 2) enviar uma I frame de teste
    unsigned char payload[3] = {0x01, 0x40, 0x67};
    int sequenceNumber = 0;

    int res = llwrite_tx(fd, payload, 3, &sequenceNumber);
    if (res < 0) {
        printf("Error sending I frame\n");
    } else {
        printf("I frame sent successfully, payload size = %d\n", res);
    }

    sleep(1);

    if (tcsetattr(fd, TCSANOW, &oldtio) == -1) {
        perror("tcsetattr");
        exit(-1);
    }

    close(fd);
    return 0;
}
