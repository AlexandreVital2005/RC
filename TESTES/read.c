// Alexandre Vital e Francisco Nunes
// Read from serial port in non-canonical mode
//
// Versao com START / DATA / END e destuffing para reconstruir o ficheiro

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define BAUDRATE B38400
#define _POSIX_SOURCE 1

#define FALSE 0
#define TRUE  1

#define BUF_SIZE 4096

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

typedef enum {
    S_START,
    S_FLAG,
    S_A,
    S_C,
    S_BCC1_OK,
    S_DATA
} State;

unsigned char rr_for(int nr)  { return (nr == 0) ? C_RR0 : C_RR1; }
unsigned char rej_for(int nr) { return (nr == 0) ? C_REJ0 : C_REJ1; }

void build_sup_frame(unsigned char *frame, unsigned char addr, unsigned char ctrl)
{
    frame[0] = FLAG;
    frame[1] = addr;
    frame[2] = ctrl;
    frame[3] = addr ^ ctrl;
    frame[4] = FLAG;
}

int destuff_bytes(const unsigned char *src, int src_len, unsigned char *dst)
{
    int j = 0;

    for (int i = 0; i < src_len; i++) {
        if (src[i] == ESC) {
            if (i + 1 >= src_len) return -1;
            dst[j++] = src[i + 1] ^ ESC_XOR;
            i++;
        } else {
            dst[j++] = src[i];
        }
    }

    return j;
}

long decode_file_size(const unsigned char *bytes, int len)
{
    long value = 0;
    for (int i = 0; i < len; i++) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

int parse_control_packet(const unsigned char *packet,
                         int packet_len,
                         char *filename_out,
                         int filename_max,
                         long *file_size_out)
{
    if (packet_len < 1) return -1;
    if (packet[0] != APP_START && packet[0] != APP_END) return -1;

    int idx = 1;
    int have_name = 0;
    int have_size = 0;

    while (idx + 1 < packet_len) {
        unsigned char T = packet[idx++];
        unsigned char L = packet[idx++];

        if (idx + L > packet_len) return -1;

        if (T == T_FILESIZE) {
            *file_size_out = decode_file_size(&packet[idx], L);
            have_size = 1;
        }
        else if (T == T_FILENAME) {
            int copy_len = (L < filename_max - 1) ? L : (filename_max - 1);
            memcpy(filename_out, &packet[idx], copy_len);
            filename_out[copy_len] = '\0';
            have_name = 1;
        }

        idx += L;
    }

    return (have_name && have_size) ? 0 : -1;
}

int handle_app_packet(const unsigned char *packet,
                      int packet_len,
                      FILE **out_file,
                      char *output_filename,
                      int filename_max,
                      long *expected_file_size,
                      long *written_bytes,
                      int *transfer_done)
{
    if (packet_len < 1) return -1;

    unsigned char control = packet[0];

    if (control == APP_START) {
        long fsize = 0;
        char fname[256] = {0};

        if (parse_control_packet(packet, packet_len, fname, sizeof(fname), &fsize) < 0) {
            printf("Erro a interpretar START packet\n");
            return -1;
        }

        printf("START packet recebido\n");
        printf("Nome do ficheiro: %s\n", fname);
        printf("Tamanho esperado: %ld bytes\n", fsize);

        strncpy(output_filename, fname, filename_max - 1);
        output_filename[filename_max - 1] = '\0';
        *expected_file_size = fsize;
        *written_bytes = 0;

        *out_file = fopen(output_filename, "wb");
        if (*out_file == NULL) {
            perror("fopen");
            return -1;
        }

        return 0;
    }

    if (control == APP_DATA) {
        if (packet_len < 3) {
            printf("DATA packet demasiado curto\n");
            return -1;
        }

        if (*out_file == NULL) {
            printf("Recebi DATA antes de START\n");
            return -1;
        }

        int L2 = packet[1];
        int L1 = packet[2];
        int data_len = L2 * 256 + L1;

        if (packet_len != data_len + 3) {
            printf("Tamanho do DATA packet inconsistente: header=%d real=%d\n",
                   data_len, packet_len - 3);
            return -1;
        }

        size_t written = fwrite(&packet[3], 1, data_len, *out_file);
        if ((int)written != data_len) {
            perror("fwrite");
            return -1;
        }

        *written_bytes += data_len;

        printf("DATA packet recebido: %d bytes (total escrito = %ld)\n",
               data_len, *written_bytes);

        return 0;
    }

    if (control == APP_END) {
        long fsize = 0;
        char fname[256] = {0};

        if (parse_control_packet(packet, packet_len, fname, sizeof(fname), &fsize) < 0) {
            printf("Erro a interpretar END packet\n");
            return -1;
        }

        printf("END packet recebido\n");
        printf("Nome no END: %s\n", fname);
        printf("Tamanho no END: %ld bytes\n", fsize);

        if (*out_file != NULL) {
            fclose(*out_file);
            *out_file = NULL;
        }

        if (strcmp(output_filename, fname) != 0) {
            printf("Aviso: nome no END diferente do START\n");
        }

        if (*expected_file_size != fsize) {
            printf("Aviso: tamanho no END diferente do START\n");
        }

        if (*written_bytes != *expected_file_size) {
            printf("Aviso: bytes escritos (%ld) diferentes do esperado (%ld)\n",
                   *written_bytes, *expected_file_size);
        }

        *transfer_done = 1;
        return 0;
    }

    printf("Tipo de pacote aplicacao desconhecido: 0x%02X\n", control);
    return -1;
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

    State state = S_START;
    unsigned char buf = 0;
    unsigned char Aread = 0, Cread = 0;
    int bytes_read;

    unsigned char data[BUF_SIZE];
    unsigned char destuffed[BUF_SIZE];
    int data_idx = 0;

    int NS_esperado = 0;

    unsigned char reply[5];

    int done = 0;
    int transfer_done = 0;

    FILE *out_file = NULL;
    char output_filename[256] = {0};
    long expected_file_size = 0;
    long written_bytes = 0;
    int closing_phase = 0;

    while (!done) {
        bytes_read = read(fd, &buf, 1);
        if (bytes_read <= 0) continue;

        switch (state) {

            case S_START:
                if (buf == FLAG) state = S_FLAG;
                break;

            case S_FLAG:
                if (buf == FLAG) {
                    state = S_FLAG;
                }
                else if (buf == A_TX || buf == A_RX) {
                    Aread = buf;
                    state = S_A;
                }
                else {
                    state = S_START;
                }
                break;

            case S_A:
                if (buf == FLAG) {
                    state = S_FLAG;
                }
                else if (buf == C_SET  || buf == C_UA   ||
                         buf == C_DISC ||
                         buf == C_I0   || buf == C_I1   ||
                         buf == C_RR0  || buf == C_RR1  ||
                         buf == C_REJ0 || buf == C_REJ1) {
                    Cread = buf;
                    state = S_C;
                }
                else {
                    state = S_START;
                }
                break;

            case S_C:
                if (buf == FLAG) {
                    state = S_FLAG;
                }
                else if (buf == (Aread ^ Cread)) {
                    state = S_BCC1_OK;
                }
                else {
                    printf("BCC1 errado: recebido 0x%02X esperado 0x%02X\n",
                           buf, Aread ^ Cread);
                    state = S_START;
                }
                break;

            case S_BCC1_OK:
                if (Cread == C_I0 || Cread == C_I1) {
                    data_idx = 0;
                    memset(data, 0, sizeof(data));

                    if (buf == FLAG) {
                        printf("I frame vazio (sem BCC2)\n");
                        state = S_START;
                    }
                    else {
                        data[data_idx++] = buf;
                        state = S_DATA;
                    }
                }
                else {
                    if (buf == FLAG) {
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
                            build_sup_frame(reply, A_RX, C_DISC);
                            write(fd, reply, 5);
                            closing_phase = 1;
                        }
                        else if (Cread == C_UA && closing_phase) {
                            printf("Recebi UA final -> fechar ligacao\n");
                            done = 1;
                        }

                        state = S_START;
                    }
                    else {
                        printf("FLAG final esperada, recebido 0x%02X\n", buf);
                        state = S_START;
                    }
                }
                break;

            case S_DATA:
                if (buf == FLAG) {
                    if (data_idx < 1) {
                        printf("I frame demasiado curto\n");
                        state = S_START;
                        data_idx = 0;
                        break;
                    }

                    int destuffed_len = destuff_bytes(data, data_idx, destuffed);
                    if (destuffed_len < 1) {
                        printf("Erro no destuffing\n");
                        data_idx = 0;
                        memset(data, 0, sizeof(data));
                        state = S_START;
                        break;
                    }

                    int payload_len = destuffed_len - 1;
                    unsigned char bcc2_recv = destuffed[destuffed_len - 1];
                    int NS_recebido = (Cread == C_I1) ? 1 : 0;

                    printf("NS recebido = %d, expectedNs = %d\n", NS_recebido, NS_esperado);

                    unsigned char bcc2_calc = 0x00;
                    for (int j = 0; j < payload_len; j++) {
                        bcc2_calc ^= destuffed[j];
                    }

                    printf("BCC2 calculado = 0x%02X\n", bcc2_calc);
                    printf("BCC2 recebido  = 0x%02X\n", bcc2_recv);

                    reply[0] = FLAG;
                    reply[1] = A_TX;
                    reply[4] = FLAG;

                    if (NS_recebido == NS_esperado) {
                        if (bcc2_calc == bcc2_recv) {
                            printf("New correct frame received\n");
                            printf("Payload length = %d\n", payload_len);

                            if (handle_app_packet(destuffed,
                                                  payload_len,
                                                  &out_file,
                                                  output_filename,
                                                  sizeof(output_filename),
                                                  &expected_file_size,
                                                  &written_bytes,
                                                  &transfer_done) < 0) {
                                printf("Erro a processar pacote da aplicacao\n");
                            }

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
                        printf("Duplicate frame -> discard, send RR\n");
                        reply[2] = rr_for(NS_esperado);
                        reply[3] = reply[1] ^ reply[2];
                        write(fd, reply, 5);
                    }

                    data_idx = 0;
                    memset(data, 0, sizeof(data));
                    memset(destuffed, 0, sizeof(destuffed));
                    state = S_START;
                }
                else {
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

    if (out_file != NULL) fclose(out_file);

    if (transfer_done) {
        printf("Transferencia concluida. Ficheiro reconstruido: %s\n", output_filename);
    }

    sleep(1);

    if (tcsetattr(fd, TCSANOW, &oldtio) == -1) {
        perror("tcsetattr");
        exit(-1);
    }

    close(fd);
    return 0;
}
