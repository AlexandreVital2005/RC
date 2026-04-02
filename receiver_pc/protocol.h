#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdio.h>

#define TRANSMITTER 0
#define RECEIVER    1

#define BUF_SIZE        4096
#define MAX_FRAME_SIZE  8192
#define MAX_FILENAME    256

typedef struct {
    FILE *out_file;
    char output_filename[MAX_FILENAME];
    long expected_file_size;
    long written_bytes;
    int transfer_done;
} AppReceiverContext;

#endif