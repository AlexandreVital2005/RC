#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "protocol.h"
#include "link_layer.h"
#include "application_layer.h"

int main(int argc, char *argv[])
{
    //srand(time(NULL));

    if (argc != 2) {
        printf("Uso: %s <SerialPort>\n", argv[0]);
        printf("Exemplo: %s /dev/ttyS1\n", argv[0]);
        return 1;
    }

    const char *serialPort = argv[1];

    int fd = llopen(serialPort, RECEIVER);
    if (fd < 0) {
        printf("Erro no llopen\n");
        return 1;
    }

    unsigned char packet[BUF_SIZE];

    AppReceiverContext ctx;
    ctx.out_file = NULL;
    ctx.output_filename[0] = '\0';
    ctx.expected_file_size = 0;
    ctx.written_bytes = 0;
    ctx.transfer_done = 0;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (!ctx.transfer_done) {
        int len = llread(fd, packet);

        if (len == -2) {
            printf("Pedido de fecho recebido\n");
            break;
        }

        if (len < 0) {
            printf("Erro no llread\n");
            continue;
        }

        if (handle_app_packet(packet, len, &ctx) < 0) {
            printf("Erro a processar pacote da aplicação\n");
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    if (ctx.out_file != NULL) {
        fclose(ctx.out_file);
        ctx.out_file = NULL;
    }

    if (llclose(fd, RECEIVER) < 0) {
        printf("Erro no llclose\n");
        return 1;
    }

    double duration = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

    double bytes_per_sec = 0;
    double bits_per_sec = 0;
    double eff = 0;

    if (duration > 0)
{
    bytes_per_sec = ctx.written_bytes/duration;
    bits_per_sec = (ctx.written_bytes * 8) / duration;
    eff = bits_per_sec/38400 * 100;
}

    if (ctx.transfer_done) {
        printf("Transferência concluída com sucesso\n");
        printf("Ficheiro reconstruído: %s\n", ctx.output_filename);
    }

    printf("-------------------Stats-------------------\n");
    printf("Bytes recebidos: %ld\n",ctx.written_bytes);
    printf("Tempo total: %.3f s\n",duration);
    printf("Taxa Bytes: %.2f bytes/s\n",bytes_per_sec);
    printf("Taxa Bits: %.2f bits/s\n",bits_per_sec);
    printf("Eficiência: %.2f%% \n",eff);

    return 0;
}
