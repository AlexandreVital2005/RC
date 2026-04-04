#include <stdio.h>
#include <stdlib.h>

#include "protocol.h"
#include "link_layer.h"
#include "application_layer.h"

int main(int argc, char *argv[])
{
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

    if (ctx.out_file != NULL) {
        fclose(ctx.out_file);
        ctx.out_file = NULL;
    }

    if (llclose(fd, RECEIVER) < 0) {
        printf("Erro no llclose\n");
        return 1;
    }

    if (ctx.transfer_done) {
        printf("Transferência concluída com sucesso\n");
        printf("Ficheiro reconstruído: %s\n", ctx.output_filename);
    }

    return 0;
}