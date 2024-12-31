#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "CANopen.h"
#include "system.h"
#include "file_transfer_ext.h"
#include "basic_node.h"
#include "sdo_client.h"

extern CO_t *CO;

int parse_int_arg(char *arg, int *value) {
    char *endptr;
    int tmp;

    if ((arg[0] == '0') && (arg[1] == 'x')) { // hex
        tmp = strtoul(arg, &endptr, 16);
    } else {
        tmp = strtoul(arg, &endptr, 10);
    }

    if (endptr == arg) {
        return -1;
    }

    *value = tmp;
    return 0;
}

static void usage(char *name) {
    printf("%s <interface> <node-id> <src> <optional-dest>\n", name);
}

int main(int argc, char* argv[]) {
    if ((argc != 4) && (argc != 5)) {
        printf("invalid number of args\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    int node_id;
    int r = parse_int_arg(argv[2], &node_id);
    if (r < 0) {
        printf("invalid node id: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    char *file;
    if (argc == 4) {
        file = basename(argv[3]);
    } else {
        file = argv[4];
    }

    if (!is_file(argv[3])) {
        printf("file does not exist: %s\n", argv[3]);
        return EXIT_FAILURE;
    }

    r = basic_node_start(argv[1]);
    if (r < 0) {
        printf("node start failure: %d\n", -r);
        return EXIT_FAILURE;
    }

    CO_SDO_abortCode_t abort_code = write_SDO(CO->SDOclient, node_id, FWRITE_CACHE_INDEX, FILE_TRANSFER_SUBINDEX_NAME, (uint8_t *)file, strlen(file) + 1);
    if (abort_code != 0) {
        goto abort;
    }

    abort_code = write_SDO_from_file(CO->SDOclient, node_id, FWRITE_CACHE_INDEX, FILE_TRANSFER_SUBINDEX_DATA, argv[3]);
    if (abort_code != 0) {
        goto abort;
    }

    basic_node_stop();
    return EXIT_SUCCESS;

abort:
    basic_node_stop();
    printf("SDO Abort: 0x%x - %s\n", abort_code, get_abort_string(abort_code));
    return EXIT_FAILURE;
}