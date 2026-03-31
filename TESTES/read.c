// Alexandre Vital e Francisco Nunes
// Read from serial port in non-canonical mode
//
// Modified by: Eduardo Nuno Almeida [enalmeida@fe.up.pt]

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

// Baudrate settings are defined in <asm/termbits.h>, which is
// included by <termios.h>
#define BAUDRATE B38400
#define _POSIX_SOURCE 1 // POSIX compliant source

#define FALSE 0
#define TRUE 1

// BUF_SIZE must be large enough to hold the data field of an I frame.
// Increase if you plan to send larger payloads.
#define BUF_SIZE 256

#define FLAG   0x7E
#define A_TX   0x03   // commands from Tx / replies from Rx
#define A_RX   0x01   // commands from Rx / replies from Tx

#define C_SET  0x03
#define C_UA   0x07
#define C_DISC 0x0B
#define C_I0   0x00
#define C_I1   0x40

// ---- State machine states used by the main reception loop ----
typedef enum {
    S_START,
    S_FLAG,
    S_A,
    S_C,
    S_BCC1_OK,   // header verified – decide: supervision or data?
    S_DATA,      // accumulating data bytes for an I frame
    S_STOP
} State;

unsigned char rr_for(int nr)  { return (nr == 0) ? 0x05 : 0x85; }
unsigned char rej_for(int nr) { return (nr == 0) ? 0x01 : 0x81; }

// Build a 5-byte supervision frame into *frame.
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
    newtio.c_cc[VTIME] = 0;
    newtio.c_cc[VMIN]  = 1;  // block until exactly 1 byte is available

    tcflush(fd, TCIOFLUSH);

    if (tcsetattr(fd, TCSANOW, &newtio) == -1) { perror("tcsetattr"); exit(-1); }

    printf("New termios structure set\n");

    // ----------------------------------------------------------------
    // State-machine variables
    // ----------------------------------------------------------------
    State state        = S_START;
    unsigned char buf  = 0;
    unsigned char Aread = 0, Cread = 0;
    int           bytes_read;

    // Data accumulation for I frames
    unsigned char data[BUF_SIZE];
    int           data_idx = 0;

    // Stop-and-wait sequence number the receiver expects next
    int NS_esperado = 0;

    // Supervision reply buffer
    unsigned char reply[5];

    // Flag: set to 1 once we've done the full DISC/UA handshake
    int done = 0;

    // ----------------------------------------------------------------
    // Main reception loop
    // ----------------------------------------------------------------
    while (!done) {

        bytes_read = read(fd, &buf, 1);
        if (bytes_read <= 0) continue;   // nothing yet – try again

        switch (state) {

            // ---- Wait for opening FLAG ----
            case S_START:
                if (buf == FLAG) {
                    state = S_FLAG;
                }
                break;

            // ---- We are inside the flag(s) at the start of a frame ----
            case S_FLAG:
                if (buf == FLAG) {
                    // consecutive flags – stay
                    state = S_FLAG;
                }
                else if (buf == A_TX || buf == A_RX) {
                    Aread = buf;
                    state = S_A;
                }
                else {
                    // Not a valid address – resync
                    state = S_START;
                }
                break;

            // ---- We have the address byte ----
            case S_A:
                if (buf == FLAG) {
                    state = S_FLAG;   // false start – try again
                }
                else if (buf == C_SET  || buf == C_UA   ||
                         buf == C_DISC ||
                         buf == C_I0   || buf == C_I1   ||
                         buf == 0x05   || buf == 0x85   ||  // RR0, RR1
                         buf == 0x01   || buf == 0x81) {    // REJ0, REJ1
                    Cread = buf;
                    state = S_C;
                }
                else {
                    state = S_START;
                }
                break;

            // ---- We have the control byte – verify BCC1 ----
            case S_C:
                if (buf == FLAG) {
                    state = S_FLAG;
                }
                else if (buf == (Aread ^ Cread)) {
                    // BCC1 correct
                    state = S_BCC1_OK;
                }
                else {
                    // BCC1 wrong – ignore frame, resync
                    printf("BCC1 errado: recebido 0x%02X esperado 0x%02X\n",
                           buf, Aread ^ Cread);
                    state = S_START;
                }
                break;

            // ---- Header verified – branch on frame type ----
            case S_BCC1_OK:
                if (Cread == C_I0 || Cread == C_I1) {
                    // Information frame: start accumulating data
                    data_idx = 0;
                    memset(data, 0, sizeof(data));

                    // The byte we just read is the first data byte
                    // (it is NOT a FLAG – if it were, we would have
                    //  caught it below; handle that edge case too)
                    if (buf == FLAG) {
                        // Empty I frame (no data, no BCC2) – treat as error
                        printf("I frame vazio (sem BCC2)\n");
                        state = S_START;
                    }
                    else {
                        data[data_idx++] = buf;
                        state = S_DATA;
                    }
                }
                else {
                    // Supervision frame: next byte must be closing FLAG
                    if (buf == FLAG) {
                        // Complete supervision frame received
                        printf("Supervision frame completa: A=0x%02X C=0x%02X\n",
                               Aread, Cread);

                        if (Cread == C_SET) {
                            printf("Recebi SET -> enviar UA\n");
                            printf("---------------------//-------------------\n");

                            build_sup_frame(reply, A_TX, C_UA);
                            write(fd, reply, 5);

                        }
                        else if (Cread == C_DISC) {
                            printf("Recebi DISC -> enviar DISC\n");
                            printf("---------------------//-------------------\n");

                            // Receiver sends DISC with A=0x01
                            build_sup_frame(reply, A_RX, C_DISC);
                            write(fd, reply, 5);

                            // Now wait for the final UA from the transmitter
                            // Re-use the state machine (it will naturally
                            // parse the UA frame that follows).
                            // We just stay in the loop; when UA arrives
                            // we will reach this branch again with C_UA
                            // and set done=1 below.
                        }
                        else if (Cread == C_UA) {
                            // Final UA after the DISC exchange
                            printf("Recebi UA final -> fechar ligacao\n");
                            done = 1;
                        }

                        state = S_START;
                    }
                    else {
                        // Unexpected byte where FLAG should be
                        printf("FLAG final esperada, recebido 0x%02X\n", buf);
                        state = S_START;
                    }
                }
                break;

            // ---- Accumulate data bytes until closing FLAG ----
            case S_DATA:
                if (buf == FLAG) {
                    // End of I frame
                    if (data_idx < 1) {
                        printf("I frame demasiado curto\n");
                        state = S_START;
                        data_idx = 0;
                        break;
                    }

                    // The last byte stored is BCC2
                    int payload_len         = data_idx - 1;
                    unsigned char bcc2_recv = data[data_idx - 1];

                    int NS_recebido = (Cread == C_I1) ? 1 : 0;

                    printf("NS recebido = %d, expectedNs = %d\n",
                           NS_recebido, NS_esperado);

                    // Compute BCC2 over payload bytes
                    unsigned char bcc2_calc = 0x00;
                    for (int j = 0; j < payload_len; j++)
                        bcc2_calc ^= data[j];

                    printf("BCC2 calculado = 0x%02X\n", bcc2_calc);
                    printf("BCC2 recebido  = 0x%02X\n", bcc2_recv);

                    // Build reply skeleton (A=0x03, Tx address for Rx replies)
                    reply[0] = FLAG;
                    reply[1] = A_TX;
                    reply[4] = FLAG;

                    if (NS_recebido == NS_esperado) {
                        // New frame
                        if (bcc2_calc == bcc2_recv) {
                            printf("New correct frame received\n");
                            printf("Payload length = %d\n", payload_len);
                            printf("Payload bytes: ");
                            for (int j = 0; j < payload_len; j++)
                                printf("0x%02X ", data[j]);
                            printf("\n");

                            NS_esperado = 1 - NS_esperado;

                            reply[2] = rr_for(NS_esperado);
                            reply[3] = reply[1] ^ reply[2];
                            write(fd, reply, 5);
                        }
                        else {
                            printf("New frame with BCC2 error -> REJ\n");
                            reply[2] = rej_for(NS_esperado);
                            reply[3] = reply[1] ^ reply[2];
                            write(fd, reply, 5);
                        }
                    }
                    else {
                        // Duplicate frame
                        printf("Duplicate frame -> discard, send RR\n");
                        reply[2] = rr_for(NS_esperado);
                        reply[3] = reply[1] ^ reply[2];
                        write(fd, reply, 5);
                    }

                    data_idx = 0;
                    memset(data, 0, sizeof(data));
                    state = S_START;
                }
                else {
                    // Accumulate data byte
                    if (data_idx < BUF_SIZE) {
                        data[data_idx++] = buf;
                    }
                    else {
                        printf("Buffer overflow in DATA state\n");
                        data_idx = 0;
                        memset(data, 0, sizeof(data));
                        state = S_START;
                    }
                }
                break;

            default:
                state = S_START;
                break;
        }
    }

    sleep(1);

    // Restore old port settings
    if (tcsetattr(fd, TCSANOW, &oldtio) == -1) { perror("tcsetattr"); exit(-1); }

    close(fd);
    return 0;
}
