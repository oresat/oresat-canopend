#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "CANopen.h"
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

static void usage(void) {
    printf("./oresat-sdo-read <interface> <node-id> <index> <subindex> <dtype>\n");
}

int main(int argc, char* argv[]) {
    if ((argc != 5) && (argc != 6)) {
        printf("invalid number of args\n");
        usage();
        return EXIT_FAILURE;
    }

    int node_id;
    int r = parse_int_arg(argv[2], &node_id);
    if (r < 0) {
        printf("invalid node id: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    int index;
    r = parse_int_arg(argv[3], &index);
    if (r < 0) {
        printf("invalid index: %s\n", argv[3]);
        return EXIT_FAILURE;
    }

    int subindex;
    r = parse_int_arg(argv[4], &subindex);
    if (r < 0) {
        printf("invalid subindex: %s\n", argv[4]);
        return EXIT_FAILURE;
    }

    size_t data_size = 1024;
    uint8_t data[data_size];
    size_t read_size;

    r = basic_node_start(argv[1]);
    if (r < 0) {
        printf("node start failure: %d\n", -r);
        return EXIT_FAILURE;
    }

    CO_SDO_abortCode_t abort_code = read_SDO(CO->SDOclient, node_id, index, subindex, data, data_size, &read_size);
    basic_node_stop();


    if (abort_code == 0) {
        if ((argc == 5) || ((argc == 6) && !strncmp(argv[5], "bytes", strlen(argv[5])))) {
            printf("0x");
            for (size_t i=0; i<read_size; i++) {
                printf("%02x", data[i]);
            }
            printf("\n");
        } else {
            char *dtype = argv[5];
            if (!strncmp(dtype, "b", strlen(dtype))) {
                printf("%d\n", *(bool *)data);
            } else if (!strncmp(dtype, "i8", strlen(dtype))) {
                printf("%d\n", *(int8_t *)data);
            } else if (!strncmp(dtype, "i16", strlen(dtype))) {
                printf("%d\n", *(int16_t *)data);
            } else if (!strncmp(dtype, "i32", strlen(dtype))) {
                printf("%d\n", *(int32_t *)data);
            } else if (!strncmp(dtype, "i64", strlen(dtype))) {
                printf("%ld\n", *(int64_t *)data);
            } else if (!strncmp(dtype, "u8", strlen(dtype))) {
                printf("%d\n", *(uint8_t *)data);
            } else if (!strncmp(dtype, "u16", strlen(dtype))) {
                printf("%d\n", *(uint16_t *)data);
            } else if (!strncmp(dtype, "u32", strlen(dtype))) {
                printf("%d\n", *(uint32_t *)data);
            } else if (!strncmp(dtype, "u64", strlen(dtype))) {
                printf("%ld\n", *(uint64_t *)data);
            } else if (!strncmp(dtype, "f32", strlen(dtype))) {
                printf("%f\n", *(float *)data);
            } else if (!strncmp(dtype, "f64", strlen(dtype))) {
                printf("%f\n", *(double *)data);
            } else if (!strncmp(dtype, "str", strlen(dtype))) {
                printf("%s\n", (char *)data);
            }
        }
    } else {
        printf("SDO Abort: 0x%x - %s\n", abort_code, get_abort_string(abort_code));
    }

    return EXIT_SUCCESS;
}