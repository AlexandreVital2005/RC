#include "application_layer.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
                      AppReceiverContext *ctx)
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

        strncpy(ctx->output_filename, fname, MAX_FILENAME - 1);
        ctx->output_filename[MAX_FILENAME - 1] = '\0';
        ctx->expected_file_size = fsize;
        ctx->written_bytes = 0;

        if (ctx->out_file != NULL) {
            fclose(ctx->out_file);
            ctx->out_file = NULL;
        }

        ctx->out_file = fopen(ctx->output_filename, "wb");
        if (ctx->out_file == NULL) {
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

        if (ctx->out_file == NULL) {
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

        size_t written = fwrite(&packet[3], 1, data_len, ctx->out_file);
        if ((int)written != data_len) {
            perror("fwrite");
            return -1;
        }

        ctx->written_bytes += data_len;

        printf("DATA packet recebido: %d bytes (total escrito = %ld)\n",
               data_len, ctx->written_bytes);

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

        if (ctx->out_file != NULL) {
            fclose(ctx->out_file);
            ctx->out_file = NULL;
        }

        if (strcmp(ctx->output_filename, fname) != 0) {
            printf("Aviso: nome no END diferente do START\n");
        }

        if (ctx->expected_file_size != fsize) {
            printf("Aviso: tamanho no END diferente do START\n");
        }

        if (ctx->written_bytes != ctx->expected_file_size) {
            printf("Aviso: bytes escritos (%ld) diferentes do esperado (%ld)\n",
                   ctx->written_bytes, ctx->expected_file_size);
        }

        ctx->transfer_done = 1;
        return 0;
    }

    printf("Tipo de pacote aplicacao desconhecido: 0x%02X\n", control);
    return -1;
}