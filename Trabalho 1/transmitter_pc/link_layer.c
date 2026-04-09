#include "link_layer.h"
#include "protocol.h"
#include "serial_port.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static struct termios oldtio;
static volatile int alarmFired = 0;
static volatile int alarmCount = 0;
static int sequenceNumber = 0;

typedef enum { SM_START, SM_FLAG, SM_A, SM_C, SM_BCC1_OK, SM_DONE } SMState;

static void alarmHandler(int sig)
{
    (void)sig;
    alarmFired = 1;
    alarmCount++;
    printf("Alarm #%d\n", (int)alarmCount);
}

static unsigned char rr_for(int nr)  { return (nr == 0) ? C_RR0 : C_RR1; }
static unsigned char rej_for(int nr) { return (nr == 0) ? C_REJ0 : C_REJ1; }

static int is_valid_ctrl(unsigned char c)
{
    return (c == C_SET  || c == C_UA   || c == C_DISC ||
            c == C_RR0  || c == C_RR1  ||
            c == C_REJ0 || c == C_REJ1 ||
            c == C_I0   || c == C_I1);
}

static void build_sup_frame(unsigned char *frame, unsigned char addr, unsigned char ctrl)
{
    frame[0] = FLAG;
    frame[1] = addr;
    frame[2] = ctrl;
    frame[3] = addr ^ ctrl;
    frame[4] = FLAG;
}

static int stuff_byte(unsigned char byte, unsigned char *dst, int idx)
{
    if (byte == FLAG || byte == ESC) {
        dst[idx++] = ESC;
        dst[idx++] = byte ^ ESC_XOR;
    } else {
        dst[idx++] = byte;
    }
    return idx;
}

static int build_iframe(unsigned char *frame,
                        unsigned char ctrl,
                        const unsigned char *payload,
                        int payload_len)
{
    frame[0] = FLAG;
    frame[1] = A_TX;
    frame[2] = ctrl;
    frame[3] = frame[1] ^ frame[2];

    unsigned char bcc2 = 0x00;
    for (int i = 0; i < payload_len; i++) {
        bcc2 ^= payload[i];
    }

    int idx = 4;
    for (int i = 0; i < payload_len; i++) {
        idx = stuff_byte(payload[i], frame, idx);
    }
    idx = stuff_byte(bcc2, frame, idx);

    frame[idx++] = FLAG;
    return idx;
}

static int recv_sup_frame(int fd, unsigned char *Aout, unsigned char *Cout)
{
    SMState state = SM_START;
    unsigned char buf = 0;
    unsigned char Aread = 0, Cread = 0;
    int bytes_read;

    while (state != SM_DONE) {
        if (alarmFired) return 0;

        bytes_read = read(fd, &buf, 1);
        if (bytes_read <= 0) continue;

        switch (state) {
            case SM_START:
                if (buf == FLAG) state = SM_FLAG;
                break;

            case SM_FLAG:
                if (buf == FLAG) state = SM_FLAG;
                else if (buf == A_TX || buf == A_RX) {
                    Aread = buf;
                    state = SM_A;
                }
                else state = SM_START;
                break;

            case SM_A:
                if (buf == FLAG) state = SM_FLAG;
                else if (is_valid_ctrl(buf)) {
                    Cread = buf;
                    state = SM_C;
                }
                else state = SM_START;
                break;

            case SM_C:
                if (buf == FLAG) state = SM_FLAG;
                else if (buf == (Aread ^ Cread)) state = SM_BCC1_OK;
                else state = SM_START;
                break;

            case SM_BCC1_OK:
                if (buf == FLAG) {
                    *Aout = Aread;
                    *Cout = Cread;
                    state = SM_DONE;
                }
                else state = SM_START;
                break;

            default:
                state = SM_START;
                break;
        }
    }

    return 1;
}

int llopen(const char *serialPort)
{
    int fd = open_serial_port(serialPort);
    if (fd < 0) return -1;

    if (configure_serial_port(fd, &oldtio) < 0) {
        close_serial_port(fd);
        return -1;
    }

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = alarmHandler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;

    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        restore_serial_port(fd, &oldtio);
        close_serial_port(fd);
        return -1;
    }

    unsigned char set_frame[5];
    unsigned char Aread = 0, Cread = 0;

    build_sup_frame(set_frame, A_TX, C_SET);
    alarmCount = 0;

    while (alarmCount < MAX_RETRANS) {
        printf("Sending SET (attempt %d)...\n", (int)alarmCount + 1);
        write(fd, set_frame, 5);

        alarmFired = 0;
        alarm(TIMEOUT_SECS);

        if (recv_sup_frame(fd, &Aread, &Cread)) {
            alarm(0);
            alarmFired = 0;

            if (Aread == A_TX && Cread == C_UA) {
                printf("Connection opened successfully\n");
                sequenceNumber = 0;
                return fd;
            }
        }
    }

    restore_serial_port(fd, &oldtio);
    close_serial_port(fd);
    return -1;
}

int llwrite(int fd, const unsigned char *buffer, int length)
{
    unsigned char iframe[MAX_FRAME_SIZE];
    unsigned char Aread = 0, Cread = 0;

    unsigned char ctrl = (sequenceNumber == 0) ? C_I0 : C_I1;
    int flen = build_iframe(iframe, ctrl, buffer, length);

    alarmCount = 0;

    while (alarmCount < MAX_RETRANS) {
        printf("Sending I frame Ns=%d (attempt %d)...\n", sequenceNumber, (int)alarmCount + 1);
        write(fd, iframe, flen);

        alarmFired = 0;
        alarm(TIMEOUT_SECS);

        if (recv_sup_frame(fd, &Aread, &Cread)) {
            alarm(0);
            alarmFired = 0;

            if (Aread != A_TX) continue;

            if (Cread == rr_for(1 - sequenceNumber)) {
                printf("RR correto para Ns=%d\n", sequenceNumber);
                sequenceNumber = 1 - sequenceNumber;
                return length;
            }

            if (Cread == rej_for(sequenceNumber)) {
                printf("REJ recebido para Ns=%d, retransmitindo...\n", sequenceNumber);
                alarm(0);
                alarmFired = 0;
                continue;
            }
        }
    }

    return -1;
}

int llclose(int fd)
{
    unsigned char disc_frame[5];
    unsigned char ua_frame[5];
    unsigned char Aread = 0, Cread = 0;

    build_sup_frame(disc_frame, A_TX, C_DISC);
    build_sup_frame(ua_frame, A_RX, C_UA);

    alarmCount = 0;

    while (alarmCount < MAX_RETRANS) {
        printf("Sending DISC (attempt %d)...\n", (int)alarmCount + 1);
        write(fd, disc_frame, 5);

        alarmFired = 0;
        alarm(TIMEOUT_SECS);

        if (recv_sup_frame(fd, &Aread, &Cread)) {
            alarm(0);
            alarmFired = 0;

            if (Aread == A_RX && Cread == C_DISC) {
                printf("DISC do recetor recebido -> enviar UA final\n");
                write(fd, ua_frame, 5);

                if (restore_serial_port(fd, &oldtio) < 0) return -1;
                if (close_serial_port(fd) < 0) return -1;
                return 0;
            }
        }
    }

    restore_serial_port(fd, &oldtio);
    close_serial_port(fd);
    return -1;
}
