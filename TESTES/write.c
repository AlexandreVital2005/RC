// Francisco Nunes e Alexandre Vital
// Write to serial port in non-canonical mode
//
// Versao com SET/UA, I0, I1 e DISC

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#define BAUDRATE B38400
#define _POSIX_SOURCE 1

#define FALSE 0
#define TRUE  1

#define MAX_RETRANS    3
#define TIMEOUT_SECS   3
#define MAX_FRAME_SIZE 1024

#define FLAG  0x7E
#define A_TX  0x03   // commands from Tx / replies from Rx
#define A_RX  0x01   // commands from Rx / replies from Tx

// Supervision control bytes
#define C_SET  0x03
#define C_UA   0x07
#define C_DISC 0x0B
#define C_RR0  0x05
#define C_RR1  0x85
#define C_REJ0 0x01
#define C_REJ1 0x81

// Information frame control bytes
#define C_I0   0x00
#define C_I1   0x40

// ---- State machine states for supervision-frame reception ----
typedef enum { SM_START, SM_FLAG, SM_A, SM_C, SM_BCC1_OK, SM_DONE } SMState;

// ---- Alarm globals ----
volatile int alarmFired  = FALSE;   // set to TRUE by the signal handler
volatile int alarmCount  = 0;       // number of times alarm has fired

void alarmHandler(int sig)
{
    (void)sig;
    alarmFired = TRUE;
    alarmCount++;
    printf("Alarm #%d\n", alarmCount);
}

// ---- Helpers ----
unsigned char rr_for (int nr) { return (nr == 0) ? C_RR0 : C_RR1;  }
unsigned char rej_for(int nr) { return (nr == 0) ? C_REJ0 : C_REJ1; }

int is_valid_ctrl(unsigned char c)
{
    return (c == C_SET  || c == C_UA   || c == C_DISC ||
            c == C_RR0  || c == C_RR1  ||
            c == C_REJ0 || c == C_REJ1 ||
            c == C_I0   || c == C_I1);
}

void build_sup_frame(unsigned char *frame,
                     unsigned char addr,
                     unsigned char ctrl)
{
    frame[0] = FLAG;
    frame[1] = addr;
    frame[2] = ctrl;
    frame[3] = addr ^ ctrl;
    frame[4] = FLAG;
}

// Build an I frame.  Returns total frame length.
// NOTE: no byte-stuffing here – add it for a fully-spec-compliant version.
int build_iframe(unsigned char *frame,
                 unsigned char ctrl,
                 const unsigned char *payload,
                 int payload_len)
{
    frame[0] = FLAG;
    frame[1] = A_TX;
    frame[2] = ctrl;
    frame[3] = frame[1] ^ frame[2];   // BCC1

    unsigned char bcc2 = 0x00;
    for (int i = 0; i < payload_len; i++) {
        frame[4 + i] = payload[i];
        bcc2 ^= payload[i];
    }

    frame[4 + payload_len] = bcc2;
    frame[5 + payload_len] = FLAG;

    return payload_len + 6;
}

// ---------------------------------------------------------------
// Receive ONE supervision frame.
// Blocks byte-by-byte; returns 1 on success, 0 if alarm fired
// before a complete frame arrived.
// On success, *Aout and *Cout hold the address and control bytes.
// ---------------------------------------------------------------
int recv_sup_frame(int fd, unsigned char *Aout, unsigned char *Cout)
{
    SMState       state = SM_START;
    unsigned char buf   = 0;
    unsigned char Aread = 0, Cread = 0;
    int           bytes_read;

    while (state != SM_DONE) {

        // If alarm fired while we were blocked, bail out
        if (alarmFired) return 0;

        bytes_read = read(fd, &buf, 1);

        if (bytes_read <= 0) continue;

        switch (state) {

            case SM_START:
                if (buf == FLAG) state = SM_FLAG;
                break;

            case SM_FLAG:
                if (buf == FLAG) {
                    state = SM_FLAG;              // consecutive flags
                }
                else if (buf == A_TX || buf == A_RX) {
                    Aread = buf;
                    state = SM_A;
                }
                else {
                    state = SM_START;
                }
                break;

            case SM_A:
                if (buf == FLAG) {
                    state = SM_FLAG;
                }
                else if (is_valid_ctrl(buf)) {
                    Cread = buf;
                    state = SM_C;
                }
                else {
                    state = SM_START;
                }
                break;

            case SM_C:
                if (buf == FLAG) {
                    state = SM_FLAG;
                }
                else if (buf == (Aread ^ Cread)) {
                    state = SM_BCC1_OK;
                }
                else {
                    state = SM_START;
                }
                break;

            case SM_BCC1_OK:
                if (buf == FLAG) {
                    *Aout = Aread;
                    *Cout = Cread;
                    state = SM_DONE;
                }
                else {
                    state = SM_START;
                }
                break;

            default:
                state = SM_START;
                break;
        }
    }

    return 1;
}

// ---------------------------------------------------------------
// llopen: send SET, wait for UA
// Returns 1 on success, -1 on failure.
// ---------------------------------------------------------------
int llopen_tx(int fd)
{
    unsigned char set_frame[5];
    unsigned char Aread = 0, Cread = 0;

    build_sup_frame(set_frame, A_TX, C_SET);

    alarmCount = 0;

    while (alarmCount < MAX_RETRANS) {
        printf("Sending SET (attempt %d)...\n", alarmCount + 1);
        write(fd, set_frame, 5);

        alarmFired = FALSE;
        alarm(TIMEOUT_SECS);

        if (recv_sup_frame(fd, &Aread, &Cread)) {
            alarm(0);
            alarmFired = FALSE;
            printf("Received: A=0x%02X C=0x%02X\n", Aread, Cread);

            if (Aread == A_TX && Cread == C_UA) {
                printf("Connection opened successfully\n");
                return 1;
            }
            // Unexpected frame – try again
        }
        else {
            // Alarm fired – loop will increment via alarmCount
        }
    }

    printf("llopen failed after %d attempts\n", MAX_RETRANS);
    return -1;
}

// ---------------------------------------------------------------
// llwrite: send one I frame, wait for RR or REJ, retransmit on
// timeout or REJ.
// Returns payload_len on success, -1 on failure.
// ---------------------------------------------------------------
int llwrite_tx(int fd,
               const unsigned char *payload,
               int payload_len,
               int *seqNum)
{
    unsigned char iframe[MAX_FRAME_SIZE];
    unsigned char Aread = 0, Cread = 0;

    unsigned char ctrl   = (*seqNum == 0) ? C_I0 : C_I1;
    int           flen   = build_iframe(iframe, ctrl, payload, payload_len);

    alarmCount = 0;

    while (alarmCount < MAX_RETRANS) {
        printf("Sending I frame Ns=%d (attempt %d)...\n",
               *seqNum, alarmCount + 1);
        write(fd, iframe, flen);

        alarmFired = FALSE;
        alarm(TIMEOUT_SECS);

        if (recv_sup_frame(fd, &Aread, &Cread)) {
            alarm(0);
            alarmFired = FALSE;
            printf("Received: A=0x%02X C=0x%02X\n", Aread, Cread);

            if (Aread != A_TX) {
                // Wrong address – ignore, wait for correct response
                continue;
            }

            if (Cread == rr_for(1 - *seqNum)) {
                // Correct RR for the frame we just sent
                printf("RR correto para Ns=%d\n", *seqNum);
                *seqNum = 1 - *seqNum;
                return payload_len;
            }

            if (Cread == rej_for(*seqNum)) {
                // Negative ACK – retransmit immediately (cancel alarm first)
                printf("REJ recebido para Ns=%d, retransmitindo...\n", *seqNum);
                alarmFired = FALSE;   // force re-send on next loop iteration
                alarm(0);
                continue;
            }

            printf("Resposta inesperada: 0x%02X\n", Cread);
        }
        // else: alarm fired, loop will retry
    }

    printf("llwrite falhou para Ns=%d\n", *seqNum);
    return -1;
}

// ---------------------------------------------------------------
// llclose: send DISC, wait for receiver's DISC, reply with UA
// Returns 1 on success, -1 on failure.
// ---------------------------------------------------------------
int llclose_tx(int fd)
{
    unsigned char disc_frame[5];
    unsigned char ua_frame[5];
    unsigned char Aread = 0, Cread = 0;

    build_sup_frame(disc_frame, A_TX,  C_DISC);
    build_sup_frame(ua_frame,   A_RX,  C_UA);   // Tx replies with A=0x01

    alarmCount = 0;

    while (alarmCount < MAX_RETRANS) {
        printf("Sending DISC (attempt %d)...\n", alarmCount + 1);
        write(fd, disc_frame, 5);

        alarmFired = FALSE;
        alarm(TIMEOUT_SECS);

        if (recv_sup_frame(fd, &Aread, &Cread)) {
            alarm(0);
            alarmFired = FALSE;
            printf("Received: A=0x%02X C=0x%02X\n", Aread, Cread);

            // Receiver replies with DISC using A=0x01
            if (Aread == A_RX && Cread == C_DISC) {
                printf("DISC do recetor recebido -> enviar UA final\n");
                write(fd, ua_frame, 5);
                return 1;
            }
        }
    }

    printf("llclose falhou\n");
    return -1;
}

// ---------------------------------------------------------------
// main
// ---------------------------------------------------------------
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
    if (fd < 0) { perror(serialPortName); exit(-1); }

    struct termios oldtio, newtio;

    if (tcgetattr(fd, &oldtio) == -1) { perror("tcgetattr"); exit(-1); }

    memset(&newtio, 0, sizeof(newtio));
    newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;
    newtio.c_lflag = 0;

    // KEY FIX: VMIN=1, VTIME=0 – read() blocks until 1 byte arrives.
    // With VMIN=0 the read() returns immediately with 0 bytes, which
    // caused the state machine to spin and race against the alarm,
    // missing bytes and losing frame synchronisation.
    newtio.c_cc[VTIME] = 0;
    newtio.c_cc[VMIN]  = 1;

    tcflush(fd, TCIOFLUSH);

    if (tcsetattr(fd, TCSANOW, &newtio) == -1) { perror("tcsetattr"); exit(-1); }

    printf("New termios structure set\n");

    // Install alarm signal handler
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = alarmHandler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;   // do NOT use SA_RESTART so read() is interrupted

    if (sigaction(SIGALRM, &act, NULL) == -1) { perror("sigaction"); exit(1); }

    printf("Alarm configured\n");

    // ---- Connection establishment ----
    if (llopen_tx(fd) < 0) {
        tcsetattr(fd, TCSANOW, &oldtio);
        close(fd);
        return -1;
    }

    int sequenceNumber = 0;

    // ---- Data transfer ----
    unsigned char payload0[3] = {0x01, 0x40, 0x67};
    unsigned char payload1[3] = {0x11, 0x22, 0x33};

    if (llwrite_tx(fd, payload0, 3, &sequenceNumber) < 0) {
        printf("Error sending I0\n");
        tcsetattr(fd, TCSANOW, &oldtio);
        close(fd);
        return -1;
    }

    if (llwrite_tx(fd, payload1, 3, &sequenceNumber) < 0) {
        printf("Error sending I1\n");
        tcsetattr(fd, TCSANOW, &oldtio);
        close(fd);
        return -1;
    }

    // ---- Connection termination ----
    if (llclose_tx(fd) < 0) {
        printf("Error closing connection\n");
        tcsetattr(fd, TCSANOW, &oldtio);
        close(fd);
        return -1;
    }

    sleep(1);

    if (tcsetattr(fd, TCSANOW, &oldtio) == -1) { perror("tcsetattr"); exit(-1); }

    close(fd);
    return 0;
}
