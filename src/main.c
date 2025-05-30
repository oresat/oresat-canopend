#include "CANopen.h"
#include "CO_epoll_interface.h"
#include "OD.h"
#include "config.h"
#include "ecss_time_ext.h"
#include "fcache.h"
#include "file_transfer_ext.h"
#include "ipc.h"
#include "ipc_broadcast.h"
#include "ipc_consume.h"
#include "ipc_respond.h"
#include "load_configs.h"
#include "logger.h"
#include "os_command_ext.h"
#include "system.h"
#include "system_ext.h"
#include <linux/reboot.h>
#include <net/if.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <wordexp.h>

#define CACHE_BASE_ROOT_PATH "/var/cache/oresat"
#define CACHE_BASE_HOME_PATH "~/.cache/oresat"

#define FREAD_CACHE_DIR       "fread"
#define FREAD_CACHE_ROOT_PATH CACHE_BASE_ROOT_PATH "/" FREAD_CACHE_DIR
#define FREAD_CACHE_ROOT_HOME CACHE_BASE_HOME_PATH "/" FREAD_CACHE_DIR

#define FWRITE_CACHE_DIR       "fwrite"
#define FWRITE_CACHE_ROOT_PATH CACHE_BASE_ROOT_PATH "/" FWRITE_CACHE_DIR
#define FWRITE_CACHE_ROOT_HOME CACHE_BASE_HOME_PATH "/" FWRITE_CACHE_DIR

#define DEFAULT_NODE_ID       0x7C
#define DEFAULT_CAN_INTERFACE "can0"

#define MAIN_THREAD_INTERVAL_US 100000
#define TMR_THREAD_INTERVAL_US  1000
#define BUS_CHECK_INTERVAL_S    5

#define NMT_CONTROL \
    (CO_NMT_STARTUP_TO_OPERATIONAL | CO_NMT_ERR_ON_ERR_REG | CO_ERR_REG_GENERIC_ERR | CO_ERR_REG_COMMUNICATION)
#define FIRST_HB_TIME        500
#define SDO_SRV_TIMEOUT_TIME 1000
#define SDO_CLI_TIMEOUT_TIME 500
#define SDO_CLI_BLOCK        false

static CO_t *co = NULL;
static OD_t *od = NULL;
static CO_config_t config;
static CO_config_t base_config;
static fcache_t *fread_cache = NULL;
static fcache_t *fwrite_cache = NULL;
static uint8_t node_id = DEFAULT_NODE_ID;
static CO_epoll_t ep_rt;
static volatile sig_atomic_t CO_endProgram = 0;
static char od_path[256] = {0};
static char node_path[256] = {0};

static void *rt_thread(void *arg);
static void *ipc_responder_thread(void *arg);
static void *ipc_consumer_thread(void *arg);
static void *ipc_broadcaster_thread(void *arg);

static void sigHandler(int sig) {
    (void)sig;
    CO_endProgram = 1;
}

static void EmergencyRxCallback(const uint16_t ident, const uint16_t errorCode, const uint8_t errorRegister,
                                const uint8_t errorBit, const uint32_t infoCode) {
    int16_t nodeIdRx = ident ? (ident & 0x7F) : node_id;

    log_printf(LOG_NOTICE, DBG_EMERGENCY_RX, nodeIdRx, errorCode, errorRegister, errorBit, infoCode);
    ipc_broadcast_emcy(nodeIdRx, errorCode, infoCode);
}

static char *NmtState2Str(CO_NMT_internalState_t state) {
    switch (state) {
    case CO_NMT_INITIALIZING:
        return "initializing";
    case CO_NMT_PRE_OPERATIONAL:
        return "pre-operational";
    case CO_NMT_OPERATIONAL:
        return "operational";
    case CO_NMT_STOPPED:
        return "stopped";
    default:
        return "unknown";
    }
}

static void NmtChangedCallback(CO_NMT_internalState_t state) {
    log_printf(LOG_NOTICE, DBG_NMT_CHANGE, NmtState2Str(state), state);
}

static void HeartbeatNmtChangedCallback(uint8_t nodeId, uint8_t idx, CO_NMT_internalState_t state, void *object) {
    (void)object;
    log_printf(LOG_NOTICE, DBG_HB_CONS_NMT_CHANGE, nodeId, idx, NmtState2Str(state), state);
    ipc_broadcast_hb(nodeId, state);
}

static void printUsage(char *progName) {
    printf("Usage: %s [options]\n", progName);
    printf("\n");
    printf("Options:\n");
    printf("  -i <interface>      CAN interface (default: " DEFAULT_CAN_INTERFACE ")\n");
    printf("  -m                  Node is the network manager node\n");
    printf("  -n <node-id>        CANopen node id (default: 0x%X)\n", DEFAULT_NODE_ID);
    printf("  -p <priority>       Real-time priority of RT thread (1 .. 99). If not set or\n");
    printf("                      set to -1, then normal scheduler is used for RT thread\n");
    printf("                      (default: -1).\n");
    printf("  -v                  Verbose logging\n");
}

static void fix_cob_ids(OD_t *od, uint8_t node_id) {
    if (!od) {
        return;
    }
    int i;
    uint32_t cob_id;
    OD_entry_t *entry;
    for (int e = 0; e < od->size; e++) {
        entry = &od->list[e];
        if ((entry->index >= 0x1400) && (entry->index < 0x1600)) {
            i = entry->index - 0x1400;
            OD_get_u32(entry, 1, &cob_id, true);
            if ((cob_id & 0x7FF) == (0x200U + (0x100U * (i % 4) + (i / 4)))) {
                OD_set_u32(entry, 1, cob_id + node_id, true);
            }
        }
        if ((entry->index >= 0x1800) && (entry->index < 0x1A00)) {
            i = entry->index - 0x1800;
            OD_get_u32(entry, 1, &cob_id, true);
            if ((cob_id & 0x7FF) == (0x180U + (0x100U * (i % 4) + (i / 4)))) {
                OD_set_u32(entry, 1, cob_id + node_id, true);
            }
        }
    }
}

int main(int argc, char *argv[]) {
    int programExit = EXIT_SUCCESS;
    CO_epoll_t epMain;
    pthread_t rt_thread_id;
    pthread_t ipc_responder_thread_id;
    pthread_t ipc_consumer_thread_id;
    pthread_t ipc_broadcaster_thread_id;
    int rtPriority = -1;
    CO_NMT_reset_cmd_t reset = CO_RESET_NOT;
    CO_ReturnError_t err;
    CO_CANptrSocketCan_t CANptr = {0};
    int opt;
    bool firstRun = true;
    char can_interface[20] = DEFAULT_CAN_INTERFACE;
    bool loaded_od_conf = false;
    bool network_manager_node = false;

    get_default_node_config_path(node_path, 256);
    get_default_od_config_path(od_path, 256);

    int r = -ENOENT;
    if (is_file(node_path)) {
        r = node_config_load(node_path, can_interface, &node_id, &network_manager_node);
    } else {
        make_node_config(node_path);
    }

    while ((opt = getopt(argc, argv, "hi:mn:p:v")) != -1) {
        switch (opt) {
        case 'h':
            printUsage(argv[0]);
            exit(EXIT_SUCCESS);
        case 'i':
            strncpy(can_interface, optarg, strlen(optarg));
            break;
        case 'm':
            network_manager_node = true;
            break;
        case 'n':
            node_id = strtol(optarg, NULL, 16);
            break;
        case 'p':
            rtPriority = strtol(optarg, NULL, 0);
            break;
        case 'v':
            log_level_set(LOG_DEBUG);
            break;
        default:
            printUsage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if (getuid() != 0) {
        log_warning("not running as root");
    }
    if (r) {
        log_info("loaded node settings from %s", od_path);
    }

    if (rtPriority != -1 &&
        (rtPriority < sched_get_priority_min(SCHED_FIFO) || rtPriority > sched_get_priority_max(SCHED_FIFO))) {
        log_printf(LOG_CRIT, DBG_WRONG_PRIORITY, rtPriority);
        printUsage(argv[0]);
        exit(EXIT_FAILURE);
    }

    log_info("starting %s %s", PROJECT_NAME, PROJECT_VERSION);

    bool first_interface_check = true;
    do {
        CANptr.can_ifindex = if_nametoindex(can_interface);
        if ((first_interface_check) && (CANptr.can_ifindex == 0)) {
            log_critical("can't find CAN interface %s", can_interface);
            first_interface_check = false;
        }
        sleep_ms(250);
    } while (CANptr.can_ifindex == 0);
    if (!first_interface_check) {
        log_info("found CAN interface %s", can_interface);
    }

    if (od_path[0] != '\0') {
        if (od_config_load(od_path, &od, !network_manager_node) < 0) {
            log_critical("failed to load od objects from %s", od_path);
        } else {
            log_info("loaded od objects from %s", od_path);
            loaded_od_conf = true;
        }
    }
    if (od == NULL) {
        log_info("using internal od objects only", od_path);
        od = OD;
        loaded_od_conf = false;
    }
    fix_cob_ids(od, node_id);
    fill_config(od, &config);
    fill_config(OD, &base_config);

    uint32_t heapMemoryUsed = 0;
    co = CO_new(&config, &heapMemoryUsed);
    if (co == NULL) {
        log_printf(LOG_CRIT, DBG_GENERAL, "CO_new(), heapMemoryUsed=", heapMemoryUsed);
        exit(EXIT_FAILURE);
    }

    if (signal(SIGINT, sigHandler) == SIG_ERR) {
        log_printf(LOG_CRIT, DBG_ERRNO, "signal(SIGINT, sigHandler)");
        exit(EXIT_FAILURE);
    }
    if (signal(SIGTERM, sigHandler) == SIG_ERR) {
        log_printf(LOG_CRIT, DBG_ERRNO, "signal(SIGTERM, sigHandler)");
        exit(EXIT_FAILURE);
    }

    err = CO_epoll_create(&epMain, MAIN_THREAD_INTERVAL_US);
    if (err != CO_ERROR_NO) {
        log_printf(LOG_CRIT, DBG_GENERAL, "CO_epoll_create(main), err=", err);
        exit(EXIT_FAILURE);
    }
    err = CO_epoll_create(&ep_rt, TMR_THREAD_INTERVAL_US);
    if (err != CO_ERROR_NO) {
        log_printf(LOG_CRIT, DBG_GENERAL, "CO_epoll_create(RT), err=", err);
        exit(EXIT_FAILURE);
    }
    CANptr.epoll_fd = ep_rt.epoll_fd;

    if (network_manager_node == false) {
        if (getuid() == 0) {
            fread_cache = fcache_init(FREAD_CACHE_ROOT_PATH);
            fwrite_cache = fcache_init(FWRITE_CACHE_ROOT_PATH);
        } else {
            wordexp_t exp_result;
            char tmp_path[256];
            wordexp(CACHE_BASE_HOME_PATH, &exp_result, 0);
            sprintf(tmp_path, "%s/%s", exp_result.we_wordv[0], FREAD_CACHE_DIR);
            fread_cache = fcache_init(tmp_path);
            sprintf(tmp_path, "%s/%s", exp_result.we_wordv[0], FWRITE_CACHE_DIR);
            fwrite_cache = fcache_init(tmp_path);
            wordfree(&exp_result);
        }
        log_info("fread cache path: %s", fread_cache->dir_path);
        log_info("fwrite cache path: %s", fwrite_cache->dir_path);

        os_command_extension_init(od);
        ecss_time_extension_init(od);
        file_transfer_extension_init(od, fread_cache, fwrite_cache);
        system_extension_init(od);
    }

    ipc_init(od);

    while ((reset != CO_RESET_APP) && (reset != CO_RESET_QUIT) && (CO_endProgram == 0)) {
        uint32_t errInfo;

        if (!firstRun) {
            CO_LOCK_OD(co->CANmodule);
            co->CANmodule->CANnormal = false;
            CO_UNLOCK_OD(co->CANmodule);
        }

        CO_CANsetConfigurationMode((void *)&CANptr);
        CO_CANmodule_disable(co->CANmodule);

        err = CO_CANinit(co, (void *)&CANptr, 0 /* bit rate not used */);
        if (err != CO_ERROR_NO) {
            log_printf(LOG_CRIT, DBG_CAN_OPEN, "CO_CANinit()", err);
            programExit = EXIT_FAILURE;
            CO_endProgram = 1;
            continue;
        }

        errInfo = 0;
        err = CO_CANopenInit(co, NULL, NULL, od, NULL, NMT_CONTROL, FIRST_HB_TIME, SDO_SRV_TIMEOUT_TIME,
                             SDO_CLI_TIMEOUT_TIME, SDO_CLI_BLOCK, node_id, &errInfo);
        if (err != CO_ERROR_NO) {
            if (err == CO_ERROR_OD_PARAMETERS) {
                log_printf(LOG_CRIT, DBG_OD_ENTRY, errInfo);
            } else {
                log_printf(LOG_CRIT, DBG_CAN_OPEN, "CO_CANopenInit()", err);
            }
            programExit = EXIT_FAILURE;
            CO_endProgram = 1;
            continue;
        }

        CO_epoll_initCANopenMain(&epMain, co);
        if (!co->nodeIdUnconfigured) {
            if (errInfo != 0) {
                CO_errorReport(co->em, CO_EM_INCONSISTENT_OBJECT_DICT, CO_EMC_DATA_SET, errInfo);
            }
            if (config.CNT_EM) {
                CO_EM_initCallbackRx(co->em, EmergencyRxCallback);
            }
            CO_NMT_initCallbackChanged(co->NMT, NmtChangedCallback);
            if (config.CNT_HB_CONS) {
                CO_HBconsumer_initCallbackNmtChanged(co->HBcons, 0, NULL, HeartbeatNmtChangedCallback);
            }

            log_printf(LOG_INFO, DBG_CAN_OPEN_INFO, node_id, "communication reset");
        } else {
            log_printf(LOG_INFO, DBG_CAN_OPEN_INFO, node_id, "node-id not initialized");
        }

        if (firstRun) {
            firstRun = false;

            if (pthread_create(&rt_thread_id, NULL, rt_thread, NULL) != 0) {
                log_printf(LOG_CRIT, DBG_ERRNO, "pthread_create(rt_thread)");
                programExit = EXIT_FAILURE;
                CO_endProgram = 1;
                continue;
            }
            if (rtPriority > 0) {
                struct sched_param param;

                param.sched_priority = rtPriority;
                if (pthread_setschedparam(rt_thread_id, SCHED_FIFO, &param) != 0) {
                    log_printf(LOG_CRIT, DBG_ERRNO, "pthread_setschedparam()");
                    programExit = EXIT_FAILURE;
                    CO_endProgram = 1;
                    continue;
                }
            }
            if (pthread_create(&ipc_responder_thread_id, NULL, ipc_responder_thread, NULL) != 0) {
                log_printf(LOG_CRIT, DBG_ERRNO, "pthread_create(ipc_responder_thread)");
                programExit = EXIT_FAILURE;
                CO_endProgram = 1;
                continue;
            }
            if (pthread_create(&ipc_consumer_thread_id, NULL, ipc_consumer_thread, NULL) != 0) {
                log_printf(LOG_CRIT, DBG_ERRNO, "pthread_create(ipc_consumer_thread)");
                programExit = EXIT_FAILURE;
                CO_endProgram = 1;
                continue;
            }
            if (pthread_create(&ipc_broadcaster_thread_id, NULL, ipc_broadcaster_thread, NULL) != 0) {
                log_printf(LOG_CRIT, DBG_ERRNO, "pthread_create(ipc_broadcaster_thread)");
                programExit = EXIT_FAILURE;
                CO_endProgram = 1;
                continue;
            }
        }

        errInfo = 0;
        err = CO_CANopenInitPDO(co, co->em, od, node_id, &errInfo);
        if (err != CO_ERROR_NO) {
            if (err == CO_ERROR_OD_PARAMETERS) {
                log_printf(LOG_CRIT, DBG_OD_ENTRY, errInfo);
            } else {
                log_printf(LOG_CRIT, DBG_CAN_OPEN, "CO_CANopenInitPDO()", err);
            }
            programExit = EXIT_FAILURE;
            CO_endProgram = 1;
            continue;
        }

        CO_CANsetNormalMode(co->CANmodule);

        log_printf(LOG_INFO, DBG_CAN_OPEN_INFO, node_id, "running ...");

        reset = CO_RESET_NOT;
        while (reset == CO_RESET_NOT && CO_endProgram == 0) {
            CO_epoll_wait(&epMain);
            CO_epoll_processMain(&epMain, co, false, &reset);
            CO_epoll_processLast(&epMain);

            static uint32_t last_check = 0;
            uint32_t uptime_s = get_uptime_s();
            if (uptime_s > (last_check + BUS_CHECK_INTERVAL_S)) {
                ipc_broadcast_bus_status(co);
                last_check = uptime_s;
            }
        }
    }

    CO_endProgram = 1;

    ipc_free();

    if (pthread_join(rt_thread_id, NULL) != 0) {
        log_printf(LOG_CRIT, DBG_ERRNO, "pthread_join()");
        exit(EXIT_FAILURE);
    }
    if (pthread_join(ipc_responder_thread_id, NULL) != 0) {
        log_printf(LOG_CRIT, DBG_ERRNO, "pthread_join()");
        exit(EXIT_FAILURE);
    }
    if (pthread_join(ipc_consumer_thread_id, NULL) != 0) {
        log_printf(LOG_CRIT, DBG_ERRNO, "pthread_join()");
        exit(EXIT_FAILURE);
    }
    if (pthread_join(ipc_broadcaster_thread_id, NULL) != 0) {
        log_printf(LOG_CRIT, DBG_ERRNO, "pthread_join()");
        exit(EXIT_FAILURE);
    }

    if (network_manager_node == false) {
        os_command_extension_free();
        file_transfer_extension_free();

        fcache_free(fread_cache);
        fcache_free(fwrite_cache);
    }

    CO_epoll_close(&ep_rt);
    CO_epoll_close(&epMain);
    CO_CANsetConfigurationMode((void *)&CANptr);
    CO_delete(co);

    if (loaded_od_conf && (od != NULL)) {
        od_config_free(od, !network_manager_node);
    }

    log_printf(LOG_INFO, DBG_CAN_OPEN_INFO, node_id, "finished");
    exit(programExit);
}

static void *rt_thread(void *arg) {
    (void)arg;
    while (CO_endProgram == 0) {
        CO_epoll_wait(&ep_rt);
        CO_epoll_processRT(&ep_rt, co, true);
        CO_epoll_processLast(&ep_rt);
    }
    return NULL;
}

static void *ipc_responder_thread(void *arg) {
    (void)arg;
    while (CO_endProgram == 0) {
        ipc_respond_process(co, od, &config, fread_cache);
    }
    return NULL;
}

static void *ipc_consumer_thread(void *arg) {
    (void)arg;
    bool ipc_reset = false;
    while (CO_endProgram == 0) {
        ipc_consume_process(co, od, &base_config, &config, od_path, &ipc_reset);
        if (ipc_reset) {
            CO_endProgram = 1;
        }
    }
    return NULL;
}

static void *ipc_broadcaster_thread(void *arg) {
    (void)arg;
    while (CO_endProgram == 0) {
        ipc_broadcast_process();
    }
    return NULL;
}
