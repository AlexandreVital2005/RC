#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdio.h>

#define TRANSMITTER 0
#define RECEIVER    1

#define BUF_SIZE        4096
#define MAX_FRAME_SIZE  8192
#define MAX_FILENAME    256

typedef struct {
    FILE *out_file;                         //Ficheiro atualmente aberto
    char output_filename[MAX_FILENAME];     //Nome do ficheiro a dar
    long expected_file_size;                //Tamanho anunciado 
    long written_bytes;                     //Bytes escritos
    int transfer_done;                      //Confirmação de receção
} AppReceiverContext;

#endif