/*
 * CANopen main program file for CANopenNode on Linux.
 *
 * @file        CO_main_basic.c
 * @author      Janez Paternoster
 * @copyright   2020 Janez Paternoster
 *
 * This file is part of <https://github.com/CANopenNode/CANopenNode>, a CANopen Stack.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this
 * file except in compliance with the License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <syslog.h>
#include <time.h>
#include <sys/epoll.h>
#include <net/if.h>
#include <linux/reboot.h>
#include <sys/reboot.h>

#include "CANopen.h"
#include "OD.h"
#include "CO_error.h"
#include "CO_epoll_interface.h"

#define MAIN_THREAD_INTERVAL_US 100000
#define TMR_THREAD_INTERVAL_US 1000

#define NMT_CONTROL \
    (CO_NMT_STARTUP_TO_OPERATIONAL | CO_NMT_ERR_ON_ERR_REG | CO_ERR_REG_GENERIC_ERR | CO_ERR_REG_COMMUNICATION)
#define FIRST_HB_TIME 500
#define SDO_SRV_TIMEOUT_TIME 1000
#define SDO_CLI_TIMEOUT_TIME 500
#define SDO_CLI_BLOCK false

CO_t* CO = NULL;
static uint8_t CO_activeNodeId = 0x7C;
static CO_epoll_t epRT;
static void* rt_thread(void* arg);
static volatile sig_atomic_t CO_endProgram = 0;

static void
sigHandler(int sig) {
    (void)sig;
    CO_endProgram = 1;
}

#if (CO_CONFIG_EM) & CO_CONFIG_EM_CONSUMER
static void
EmergencyRxCallback(const uint16_t ident, const uint16_t errorCode, const uint8_t errorRegister, const uint8_t errorBit,
                    const uint32_t infoCode) {
    int16_t nodeIdRx = ident ? (ident & 0x7F) : CO_activeNodeId;

    log_printf(LOG_NOTICE, DBG_EMERGENCY_RX, nodeIdRx, errorCode, errorRegister, errorBit, infoCode);
}
#endif

#if ((CO_CONFIG_NMT)&CO_CONFIG_NMT_CALLBACK_CHANGE) || ((CO_CONFIG_HB_CONS)&CO_CONFIG_HB_CONS_CALLBACK_CHANGE)
static char*
NmtState2Str(CO_NMT_internalState_t state) {
    switch (state) {
        case CO_NMT_INITIALIZING: return "initializing";
        case CO_NMT_PRE_OPERATIONAL: return "pre-operational";
        case CO_NMT_OPERATIONAL: return "operational";
        case CO_NMT_STOPPED: return "stopped";
        default: return "unknown";
    }
}
#endif

#if (CO_CONFIG_NMT) & CO_CONFIG_NMT_CALLBACK_CHANGE
static void
NmtChangedCallback(CO_NMT_internalState_t state) {
    log_printf(LOG_NOTICE, DBG_NMT_CHANGE, NmtState2Str(state), state);
}
#endif

#if (CO_CONFIG_HB_CONS) & CO_CONFIG_HB_CONS_CALLBACK_CHANGE
static void
HeartbeatNmtChangedCallback(uint8_t nodeId, uint8_t idx, CO_NMT_internalState_t state, void* object) {
    (void)object;
    log_printf(LOG_NOTICE, DBG_HB_CONS_NMT_CHANGE, nodeId, idx, NmtState2Str(state), state);
}
#endif

static void
printUsage(char* progName) {
    printf("Usage: %s <CAN device name> [options]\n", progName);
    printf("\n"
           "Options:\n"
           "  -i <Node ID>        CANopen Node-id (1..127)\n");
    printf("  -p <RT priority>    Real-time priority of RT thread (1 .. 99). If not set or\n"
           "                      set to -1, then normal scheduler is used for RT thread.\n");
}

int
main(int argc, char* argv[]) {
    int programExit = EXIT_SUCCESS;
    CO_epoll_t epMain;
    pthread_t rt_thread_id;
    int rtPriority = -1;
    CO_NMT_reset_cmd_t reset = CO_RESET_NOT;
    CO_ReturnError_t err;
    CO_CANptrSocketCan_t CANptr = {0};
    int opt;
    bool_t firstRun = true;

    char* CANdevice = NULL;
    int16_t nodeIdFromArgs = -1;

    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        printUsage(argv[0]);
        exit(EXIT_SUCCESS);
    }
    while ((opt = getopt(argc, argv, "hi:p:")) != -1) {
        switch (opt) {
            case 'h': {
                printUsage(argv[0]);
                exit(EXIT_SUCCESS);
            }
            case 'i': {
                long int nodeIdLong = strtol(optarg, NULL, 0);
                nodeIdFromArgs = (nodeIdLong < 0 || nodeIdLong > 0xFF) ? 0 : (uint8_t)strtol(optarg, NULL, 0);
                break;
            }
            case 'p': rtPriority = strtol(optarg, NULL, 0); break;
            default: printUsage(argv[0]); exit(EXIT_FAILURE);
        }
    }

    if (optind < argc) {
        CANdevice = argv[optind];
        CANptr.can_ifindex = if_nametoindex(CANdevice);
    }

    if ((nodeIdFromArgs == 0 || nodeIdFromArgs > 127)) {
        log_printf(LOG_CRIT, DBG_WRONG_NODE_ID, nodeIdFromArgs);
        printUsage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (rtPriority != -1
        && (rtPriority < sched_get_priority_min(SCHED_FIFO) || rtPriority > sched_get_priority_max(SCHED_FIFO))) {
        log_printf(LOG_CRIT, DBG_WRONG_PRIORITY, rtPriority);
        printUsage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (CANptr.can_ifindex == 0) {
        log_printf(LOG_CRIT, DBG_NO_CAN_DEVICE, CANdevice);
        exit(EXIT_FAILURE);
    }

    uint32_t heapMemoryUsed = 0;
    CO_config_t* config_ptr = NULL;
    CO = CO_new(config_ptr, &heapMemoryUsed);
    if (CO == NULL) {
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
    err = CO_epoll_create(&epRT, TMR_THREAD_INTERVAL_US);
    if (err != CO_ERROR_NO) {
        log_printf(LOG_CRIT, DBG_GENERAL, "CO_epoll_create(RT), err=", err);
        exit(EXIT_FAILURE);
    }
    CANptr.epoll_fd = epRT.epoll_fd;

    while (reset != CO_RESET_APP && reset != CO_RESET_QUIT && CO_endProgram == 0) {
        uint32_t errInfo;

        if (!firstRun) {
            CO_LOCK_OD(CO->CANmodule);
            CO->CANmodule->CANnormal = false;
            CO_UNLOCK_OD(CO->CANmodule);
        }

        CO_CANsetConfigurationMode((void*)&CANptr);
        CO_CANmodule_disable(CO->CANmodule);

        err = CO_CANinit(CO, (void*)&CANptr, 0 /* bit rate not used */);
        if (err != CO_ERROR_NO) {
            log_printf(LOG_CRIT, DBG_CAN_OPEN, "CO_CANinit()", err);
            programExit = EXIT_FAILURE;
            CO_endProgram = 1;
            continue;
        }

        errInfo = 0;
        err = CO_CANopenInit(CO, NULL, NULL, OD, NULL, NMT_CONTROL, FIRST_HB_TIME,
                             SDO_SRV_TIMEOUT_TIME, SDO_CLI_TIMEOUT_TIME,
                             SDO_CLI_BLOCK, CO_activeNodeId, &errInfo);
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

        CO_epoll_initCANopenMain(&epMain, CO);
        if (!CO->nodeIdUnconfigured) {
            if (errInfo != 0) {
                CO_errorReport(CO->em, CO_EM_INCONSISTENT_OBJECT_DICT, CO_EMC_DATA_SET, errInfo);
            }
#if (CO_CONFIG_EM) & CO_CONFIG_EM_CONSUMER
            CO_EM_initCallbackRx(CO->em, EmergencyRxCallback);
#endif
#if (CO_CONFIG_NMT) & CO_CONFIG_NMT_CALLBACK_CHANGE
            CO_NMT_initCallbackChanged(CO->NMT, NmtChangedCallback);
#endif
#if (CO_CONFIG_HB_CONS) & CO_CONFIG_HB_CONS_CALLBACK_CHANGE
            CO_HBconsumer_initCallbackNmtChanged(CO->HBcons, 0, NULL, HeartbeatNmtChangedCallback);
#endif

            log_printf(LOG_INFO, DBG_CAN_OPEN_INFO, CO_activeNodeId, "communication reset");
        } else {
            log_printf(LOG_INFO, DBG_CAN_OPEN_INFO, CO_activeNodeId, "node-id not initialized");
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
        }

        errInfo = 0;
        err = CO_CANopenInitPDO(CO, CO->em, OD, CO_activeNodeId, &errInfo);
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

        CO_CANsetNormalMode(CO->CANmodule);

        log_printf(LOG_INFO, DBG_CAN_OPEN_INFO, CO_activeNodeId, "running ...");

        reset = CO_RESET_NOT;
        while (reset == CO_RESET_NOT && CO_endProgram == 0) {
            CO_epoll_wait(&epMain);
            CO_epoll_processMain(&epMain, CO, false, &reset);
            CO_epoll_processLast(&epMain);
        }
    }

    CO_endProgram = 1;
    if (pthread_join(rt_thread_id, NULL) != 0) {
        log_printf(LOG_CRIT, DBG_ERRNO, "pthread_join()");
        exit(EXIT_FAILURE);
    }

    CO_epoll_close(&epRT);
    CO_epoll_close(&epMain);
    CO_CANsetConfigurationMode((void*)&CANptr);
    CO_delete(CO);

    log_printf(LOG_INFO, DBG_CAN_OPEN_INFO, CO_activeNodeId, "finished");
    exit(programExit);
}

static void*
rt_thread(void* arg) {
    (void)arg;
    while (CO_endProgram == 0) {
        CO_epoll_wait(&epRT);
        CO_epoll_processRT(&epRT, CO, true);
        CO_epoll_processLast(&epRT);
    }
    return NULL;
}