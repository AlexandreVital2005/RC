// Francisco Nunes e Alexandre Vital
// Write to serial port in non-canonical mode
//
// Versao com START / DATA / END e byte stuffing para enviar penguin.gif

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

#define MAX_RETRANS     3
#define TIMEOUT_SECS    3
#define MAX_FRAME_SIZE  4096
#define DATA_CHUNK_SIZE 64 // pode ser 32 e resulta da mesma

#define FLAG  0x7E
#define ESC   0x7D
#define ESC_XOR 0x20

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

// Application layer packets
#define APP_DATA  0x01
#define APP_START 0x02
#define APP_END   0x03

// TLV types
#define T_FILESIZE 0x00
#define T_FILENAME 0x01

typedef enum { SM_START, SM_FLAG, SM_A, SM_C, SM_BCC1_OK, SM_DONE } SMState;

volatile int alarmFired = FALSE;
volatile int alarmCount = 0;

void alarmHandler(int sig)
{
    (void)sig;
    alarmFired = TRUE;
    alarmCount++;
    printf("Alarm #%d\n", alarmCount);
}

unsigned char rr_for(int nr)  { return (nr == 0) ? C_RR0 : C_RR1; }
unsigned char rej_for(int nr) { return (nr == 0) ? C_REJ0 : C_REJ1; }

int is_valid_ctrl(unsigned char c)
{
    return (c == C_SET  || c == C_UA   || c == C_DISC ||
            c == C_RR0  || c == C_RR1  ||
            c == C_REJ0 || c == C_REJ1 ||
            c == C_I0   || c == C_I1);
}

void build_sup_frame(unsigned char *frame, unsigned char addr, unsigned char ctrl)
{
    frame[0] = FLAG;
    frame[1] = addr;
    frame[2] = ctrl;
    frame[3] = addr ^ ctrl;
    frame[4] = FLAG;
}

int stuff_byte(unsigned char byte, unsigned char *dst, int idx)
{
    if (byte == FLAG || byte == ESC) {
        dst[idx++] = ESC;
        dst[idx++] = byte ^ ESC_XOR;
    } else {
        dst[idx++] = byte;
    }
    return idx;
}

int build_iframe(unsigned char *frame,
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

int recv_sup_frame(int fd, unsigned char *Aout, unsigned char *Cout)
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
        }
    }

    printf("llopen failed\n");
    return -1;
}

int llwrite_tx(int fd, const unsigned char *payload, int payload_len, int *seqNum)
{
    unsigned char iframe[MAX_FRAME_SIZE];
    unsigned char Aread = 0, Cread = 0;

    unsigned char ctrl = (*seqNum == 0) ? C_I0 : C_I1;
    int flen = build_iframe(iframe, ctrl, payload, payload_len);

    alarmCount = 0;

    while (alarmCount < MAX_RETRANS) {
        printf("Sending I frame Ns=%d (attempt %d)...\n", *seqNum, alarmCount + 1);
        write(fd, iframe, flen);

        alarmFired = FALSE;
        alarm(TIMEOUT_SECS);

        if (recv_sup_frame(fd, &Aread, &Cread)) {
            alarm(0);
            alarmFired = FALSE;
            printf("Received: A=0x%02X C=0x%02X\n", Aread, Cread);

            if (Aread != A_TX) continue;

            if (Cread == rr_for(1 - *seqNum)) {
                printf("RR correto para Ns=%d\n", *seqNum);
                *seqNum = 1 - *seqNum;
                return payload_len;
            }

            if (Cread == rej_for(*seqNum)) {
                printf("REJ recebido para Ns=%d, retransmitindo...\n", *seqNum);
                alarmFired = FALSE;
                alarm(0);
                continue;
            }

            printf("Resposta inesperada: 0x%02X\n", Cread);
        }
    }

    printf("llwrite falhou para Ns=%d\n", *seqNum);
    return -1;
}

int llclose_tx(int fd)
{
    unsigned char disc_frame[5];
    unsigned char ua_frame[5];
    unsigned char Aread = 0, Cread = 0;

    build_sup_frame(disc_frame, A_TX, C_DISC);
    build_sup_frame(ua_frame, A_RX, C_UA);

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

int get_file_size(const char *filename, long *size_out)
{
    struct stat st;
    if (stat(filename, &st) < 0) return -1;
    *size_out = st.st_size;
    return 0;
}

const char *get_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash == NULL) ? path : slash + 1;
}

int encode_file_size(unsigned char *out, long file_size)
{
    unsigned char temp[8];
    int count = 0;

    if (file_size == 0) {
        out[0] = 0;
        return 1;
    }

    while (file_size > 0) {
        temp[count++] = (unsigned char)(file_size & 0xFF);
        file_size >>= 8;
    }

    for (int i = 0; i < count; i++) {
        out[i] = temp[count - 1 - i];
    }

    return count;
}

int build_control_packet(unsigned char control,
                         const char *filename,
                         long file_size,
                         unsigned char *packet)
{
    unsigned char size_bytes[8];
    int size_len = encode_file_size(size_bytes, file_size);
    int name_len = (int)strlen(filename);
    int idx = 0;

    packet[idx++] = control;

    packet[idx++] = T_FILESIZE;
    packet[idx++] = size_len;
    memcpy(&packet[idx], size_bytes, size_len);
    idx += size_len;

    packet[idx++] = T_FILENAME;
    packet[idx++] = name_len;
    memcpy(&packet[idx], filename, name_len);
    idx += name_len;

    return idx;
}

int build_data_packet(const unsigned char *data, int data_len, unsigned char *packet)
{
    packet[0] = APP_DATA;
    packet[1] = data_len / 256;
    packet[2] = data_len % 256;
    memcpy(&packet[3], data, data_len);
    return data_len + 3;
}

int send_file(int fd, const char *filepath, int *sequenceNumber)
{
    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        perror("fopen");
        return -1;
    }

    long file_size;
    if (get_file_size(filepath, &file_size) < 0) {
        perror("stat");
        fclose(f);
        return -1;
    }

    const char *filename = get_basename(filepath);

    unsigned char packet[MAX_FRAME_SIZE];
    unsigned char chunk[DATA_CHUNK_SIZE];

    int packet_len = build_control_packet(APP_START, filename, file_size, packet);
    printf("Sending START packet for file %s (%ld bytes)\n", filename, file_size);
    if (llwrite_tx(fd, packet, packet_len, sequenceNumber) < 0) {
        fclose(f);
        return -1;
    }

    size_t nread;
    while ((nread = fread(chunk, 1, DATA_CHUNK_SIZE, f)) > 0) {
        packet_len = build_data_packet(chunk, (int)nread, packet);
        printf("Sending DATA packet with %zu bytes\n", nread);
        if (llwrite_tx(fd, packet, packet_len, sequenceNumber) < 0) {
            fclose(f);
            return -1;
        }
    }

    if (ferror(f)) {
        perror("fread");
        fclose(f);
        return -1;
    }

    packet_len = build_control_packet(APP_END, filename, file_size, packet);
    printf("Sending END packet\n");
    if (llwrite_tx(fd, packet, packet_len, sequenceNumber) < 0) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Incorrect program usage\n"
               "Usage: %s <SerialPort>\n"
               "Example: %s /dev/ttyS1\n", argv[0], argv[0]);
        exit(1);
    }

    const char *serialPortName = argv[1];
    const char *filepath = "penguin.gif";

    int fd = open(serialPortName, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror(serialPortName);
        exit(-1);
    }

    struct termios oldtio, newtio;
    if (tcgetattr(fd, &oldtio) == -1) {
        perror("tcgetattr");
        exit(-1);
    }

    memset(&newtio, 0, sizeof(newtio));
    newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 0;
    newtio.c_cc[VMIN]  = 1;

    tcflush(fd, TCIOFLUSH);

    if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
        perror("tcsetattr");
        exit(-1);
    }

    printf("New termios structure set\n");

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = alarmHandler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;

    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    printf("Alarm configured\n");

    if (llopen_tx(fd) < 0) {
        tcsetattr(fd, TCSANOW, &oldtio);
        close(fd);
        return -1;
    }

    int sequenceNumber = 0;

    if (send_file(fd, filepath, &sequenceNumber) < 0) {
        printf("Error sending file %s\n", filepath);
        tcsetattr(fd, TCSANOW, &oldtio);
        close(fd);
        return -1;
    }

    if (llclose_tx(fd) < 0) {
        printf("Error closing connection\n");
        tcsetattr(fd, TCSANOW, &oldtio);
        close(fd);
        return -1;
    }

    sleep(1);

    if (tcsetattr(fd, TCSANOW, &oldtio) == -1) {
        perror("tcsetattr");
        exit(-1);
    }

    close(fd);
    return 0;
}