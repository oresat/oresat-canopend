#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "CANopen.h"
#include "file_transfer_ext.h"
#include "sdo_client_node.h"
#include "parse_int.h"
#include "sdo_client.h"

extern CO_t *CO;

static void usage(char *name) {
    printf("%s <interface> <node-id> <src>\n", name);
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        printf("invalid number of args\n\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    int node_id;
    int r = parse_int_arg(argv[2], &node_id);
    if (r < 0) {
        printf("invalid node id: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    r = sdo_client_node_start(argv[1]);
    if (r < 0) {
        printf("node start failure: %d\n", -r);
        return EXIT_FAILURE;
    }

    CO_SDO_abortCode_t abort_code = sdo_write_str(CO->SDOclient, node_id, FREAD_CACHE_INDEX, FILE_TRANSFER_SUBINDEX_NAME, argv[3]);
    if (abort_code != 0) {
        goto abort;
    }

    abort_code = sdo_read_to_file(CO->SDOclient, node_id, FREAD_CACHE_INDEX, FILE_TRANSFER_SUBINDEX_DATA, argv[3]);
    if (abort_code != 0) {
        goto abort;
    }

    sdo_client_node_stop();
    return EXIT_SUCCESS;

abort:
    sdo_client_node_stop();
    printf("SDO Abort: 0x%x - %s\n", abort_code, get_sdo_abort_string(abort_code));
    return EXIT_FAILURE;
}
