#include "link_layer.h"
#include "serial_port.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct termios oldtio;
static int ll_role = -1;
static int ll_sequence_tx = 0;
static int ll_sequence_rx = 0;

/* ---------------- AUXILIARES ---------------- */

static unsigned char rr_for(int nr) {
    return (nr == 0) ? C_RR0 : C_RR1;
}

static unsigned char rej_for(int nr) {
    return (nr == 0) ? C_REJ0 : C_REJ1;
}

static void build_sup_frame(unsigned char *frame, unsigned char addr, unsigned char ctrl) {
    frame[0] = FLAG;
    frame[1] = addr;
    frame[2] = ctrl;
    frame[3] = addr ^ ctrl;
    frame[4] = FLAG;
}

static unsigned char calculate_bcc2(const unsigned char *buf, int len) {
    unsigned char bcc2 = 0x00;
    for (int i = 0; i < len; i++) {
        bcc2 ^= buf[i];
    }
    return bcc2;
}

static int stuff_bytes(const unsigned char *src, int src_len, unsigned char *dst) {
    int j = 0;

    for (int i = 0; i < src_len; i++) {
        if (src[i] == FLAG || src[i] == ESC) {
            dst[j++] = ESC;
            dst[j++] = src[i] ^ ESC_XOR;
        } else {
            dst[j++] = src[i];
        }
    }

    return j;
}

static int destuff_bytes(const unsigned char *src, int src_len, unsigned char *dst) {
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

/* ---------------- RECEBER SUPERVISION FRAME ---------------- */

static int read_supervision_frame(int fd, unsigned char *Aread, unsigned char *Cread) {
    State state = S_START;
    unsigned char buf;
    int bytes_read;

    while (1) {
        bytes_read = read(fd, &buf, 1);
        if (bytes_read <= 0) continue;

        switch (state) {
            case S_START:
                if (buf == FLAG) state = S_FLAG;
                break;

            case S_FLAG:
                if (buf == FLAG) {
                    state = S_FLAG;
                } else if (buf == A_TX || buf == A_RX) {
                    *Aread = buf;
                    state = S_A;
                } else {
                    state = S_START;
                }
                break;

            case S_A:
                if (buf == FLAG) {
                    state = S_FLAG;
                } else if (buf == C_SET || buf == C_UA || buf == C_DISC ||
                           buf == C_RR0 || buf == C_RR1 ||
                           buf == C_REJ0 || buf == C_REJ1) {
                    *Cread = buf;
                    state = S_C;
                } else {
                    state = S_START;
                }
                break;

            case S_C:
                if (buf == FLAG) {
                    state = S_FLAG;
                } else if (buf == ((*Aread) ^ (*Cread))) {
                    state = S_BCC1_OK;
                } else {
                    state = S_START;
                }
                break;

            case S_BCC1_OK:
                if (buf == FLAG) {
                    return 0;
                } else {
                    state = S_START;
                }
                break;

            default:
                state = S_START;
                break;
        }
    }
}

/* ---------------- CONSTRUIR I FRAME ---------------- */

static int build_i_frame(const unsigned char *packet, int packet_len, int ns, unsigned char *frame) {
    unsigned char temp[MAX_FRAME_SIZE];
    unsigned char stuffed[MAX_FRAME_SIZE];

    int idx = 0;
    unsigned char ctrl = (ns == 0) ? C_I0 : C_I1;
    unsigned char bcc2 = calculate_bcc2(packet, packet_len);

    temp[idx++] = A_TX;
    temp[idx++] = ctrl;
    temp[idx++] = A_TX ^ ctrl;

    for (int i = 0; i < packet_len; i++) {
        temp[idx++] = packet[i];
    }

    temp[idx++] = bcc2;

    int stuffed_len = stuff_bytes(temp, idx, stuffed);

    frame[0] = FLAG;
    memcpy(&frame[1], stuffed, stuffed_len);
    frame[stuffed_len + 1] = FLAG;

    return stuffed_len + 2;
}

/* ---------------- llopen ---------------- */

int llopen(const char *serialPort, int role) {
    int fd = open_serial_port(serialPort);
    if (fd < 0) return -1;

    if (configure_serial_port(fd, &oldtio) < 0) {
        close_serial_port(fd);
        return -1;
    }

    ll_role = role;
    ll_sequence_tx = 0;
    ll_sequence_rx = 0;

    unsigned char frame[5];
    unsigned char Aread, Cread;

    if (role == RECEIVER) {
        if (read_supervision_frame(fd, &Aread, &Cread) < 0) {
            restore_serial_port(fd, &oldtio);
            close_serial_port(fd);
            return -1;
        }

        if (Cread != C_SET) {
            restore_serial_port(fd, &oldtio);
            close_serial_port(fd);
            return -1;
        }

        build_sup_frame(frame, A_TX, C_UA);
        write(fd, frame, 5);
        return fd;
    }

    if (role == TRANSMITTER) {
        build_sup_frame(frame, A_TX, C_SET);
        write(fd, frame, 5);

        if (read_supervision_frame(fd, &Aread, &Cread) < 0) {
            restore_serial_port(fd, &oldtio);
            close_serial_port(fd);
            return -1;
        }

        if (Cread != C_UA) {
            restore_serial_port(fd, &oldtio);
            close_serial_port(fd);
            return -1;
        }

        return fd;
    }

    restore_serial_port(fd, &oldtio);
    close_serial_port(fd);
    return -1;
}

/* ---------------- llwrite ---------------- */

int llwrite(int fd, const unsigned char *buffer, int length) {
    unsigned char frame[MAX_FRAME_SIZE];
    unsigned char Aread, Cread;

    int frame_len = build_i_frame(buffer, length, ll_sequence_tx, frame);

    write(fd, frame, frame_len);

    if (read_supervision_frame(fd, &Aread, &Cread) < 0) {
        return -1;
    }

    if (Cread == rr_for(1 - ll_sequence_tx)) {
        ll_sequence_tx = 1 - ll_sequence_tx;
        return length;
    }

    if (Cread == rej_for(ll_sequence_tx)) {
        return -1;
    }

    return -1;
}

/* ---------------- llread ---------------- */

int llread(int fd, unsigned char *buffer) {
    State state = S_START;
    unsigned char buf = 0;
    unsigned char Aread = 0, Cread = 0;
    int bytes_read;

    unsigned char data[MAX_FRAME_SIZE];
    unsigned char destuffed[MAX_FRAME_SIZE];
    int data_idx = 0;

    unsigned char reply[5];

    while (1) {
        bytes_read = read(fd, &buf, 1);
        if (bytes_read <= 0) continue;

        switch (state) {
            case S_START:
                if (buf == FLAG) state = S_FLAG;
                break;

            case S_FLAG:
                if (buf == FLAG) {
                    state = S_FLAG;
                } else if (buf == A_TX || buf == A_RX) {
                    Aread = buf;
                    state = S_A;
                } else {
                    state = S_START;
                }
                break;

            case S_A:
                if (buf == FLAG) {
                    state = S_FLAG;
                } else if (buf == C_I0 || buf == C_I1 || buf == C_DISC) {
                    Cread = buf;
                    state = S_C;
                } else {
                    state = S_START;
                }
                break;

            case S_C:
                if (buf == FLAG) {
                    state = S_FLAG;
                } else if (buf == (Aread ^ Cread)) {
                    state = S_BCC1_OK;
                } else {
                    state = S_START;
                }
                break;

            case S_BCC1_OK:
                if (Cread == C_DISC) {
                    if (buf == FLAG) {
                        return -2;
                    } else {
                        state = S_START;
                    }
                } else {
                    if (buf == FLAG) {
                        state = S_START;
                    } else {
                        data_idx = 0;
                        data[data_idx++] = buf;
                        state = S_DATA;
                    }
                }
                break;

            case S_DATA:
                if (buf == FLAG) {
                    int destuffed_len = destuff_bytes(data, data_idx, destuffed);
                    if (destuffed_len < 1) {
                        state = S_START;
                        break;
                    }

                    int payload_len = destuffed_len - 1;
                    unsigned char bcc2_recv = destuffed[destuffed_len - 1];
                    unsigned char bcc2_calc = calculate_bcc2(destuffed, payload_len);
                    int ns_received = (Cread == C_I1) ? 1 : 0;

                    reply[0] = FLAG;
                    reply[1] = A_TX;
                    reply[4] = FLAG;

                    if (ns_received == ll_sequence_rx) {
                        if (bcc2_calc == bcc2_recv) {
                            memcpy(buffer, destuffed, payload_len);

                            ll_sequence_rx = 1 - ll_sequence_rx;

                            reply[2] = rr_for(ll_sequence_rx);
                            reply[3] = reply[1] ^ reply[2];
                            write(fd, reply, 5);

                            return payload_len;
                        } else {
                            reply[2] = rej_for(ll_sequence_rx);
                            reply[3] = reply[1] ^ reply[2];
                            write(fd, reply, 5);
                        }
                    } else {
                        reply[2] = rr_for(ll_sequence_rx);
                        reply[3] = reply[1] ^ reply[2];
                        write(fd, reply, 5);
                    }

                    state = S_START;
                    data_idx = 0;
                } else {
                    if (data_idx < MAX_FRAME_SIZE) {
                        data[data_idx++] = buf;
                    } else {
                        state = S_START;
                        data_idx = 0;
                    }
                }
                break;

            default:
                state = S_START;
                break;
        }
    }
}

/* ---------------- llclose ---------------- */

int llclose(int fd, int role) {
    unsigned char frame[5];
    unsigned char Aread, Cread;

    if (role == TRANSMITTER) {
        build_sup_frame(frame, A_TX, C_DISC);
        write(fd, frame, 5);

        if (read_supervision_frame(fd, &Aread, &Cread) < 0) return -1;
        if (Cread != C_DISC) return -1;

        build_sup_frame(frame, A_RX, C_UA);
        write(fd, frame, 5);
    }
    else if (role == RECEIVER) {
        if (read_supervision_frame(fd, &Aread, &Cread) < 0) return -1;
        if (Cread != C_DISC) return -1;

        build_sup_frame(frame, A_RX, C_DISC);
        write(fd, frame, 5);

        if (read_supervision_frame(fd, &Aread, &Cread) < 0) return -1;
        if (Cread != C_UA) return -1;
    }

    if (restore_serial_port(fd, &oldtio) < 0) return -1;
    if (close_serial_port(fd) < 0) return -1;

    return 0;
}