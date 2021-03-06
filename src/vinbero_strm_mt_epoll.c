#include <dlfcn.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vinbero_com/vinbero_com_Status.h>
#include <vinbero_com/vinbero_com_Error.h>
#include <vinbero_com/vinbero_com_Call.h>
#include <vinbero_com/vinbero_com_Config.h>
#include <vinbero_com/vinbero_com_Log.h>
#include <vinbero_com/vinbero_com_TlModule.h>
#include <vinbero_com/vinbero_com_Module.h>
#include <vinbero_com/vinbero_com_ClModule.h>
#include <vinbero_iface_MODULE/vinbero_iface_MODULE.h>
#include <vinbero_iface_TLOCAL/vinbero_iface_TLOCAL.h>
#include <vinbero_iface_TLSERVICE/vinbero_iface_TLSERVICE.h>
#include <vinbero_iface_CLOCAL/vinbero_iface_CLOCAL.h>
#include <vinbero_iface_CLSERVICE/vinbero_iface_CLSERVICE.h>
#include <libgenc/genc_Cast.h>
#include <libgenc/genc_Tree.h>
#include <gaio.h>
#include "config.h"

VINBERO_COM_MODULE_META_NAME("vinbero_strm_mt_epoll")
VINBERO_COM_MODULE_META_LICENSE("MPL-2.0")
VINBERO_COM_MODULE_META_VERSION(
    VINBERO_STRM_MT_EPOLL_VERSION_MAJOR,
    VINBERO_STRM_MT_EPOLL_VERSION_MINOR,
    VINBERO_STRM_MT_EPOLL_VERSION_PATCH
)
VINBERO_COM_MODULE_META_IN_IFACES("TLOCAL,TLSERVICE")
VINBERO_COM_MODULE_META_OUT_IFACES("TLOCAL,CLOCAL,CLSERVICE")
VINBERO_COM_MODULE_META_CHILD_COUNT(-1, -1)

VINBERO_IFACE_MODULE_FUNCS;
VINBERO_IFACE_TLOCAL_FUNCS;
VINBERO_IFACE_TLSERVICE_FUNCS;

struct vinbero_strm_mt_epoll_Module {
    struct itimerspec clientTimeout;
};

struct vinbero_strm_mt_epoll_TlModule {
    struct epoll_event* epollEventArray;
    int epollEventArraySize;
    int* clientSocketArray;
    int* clientTimerFdArray;
    struct vinbero_com_ClModule** clModuleArray;
    int clientArraySize;
    struct gaio_Methods clientIoMethods;
};

int
vinbero_iface_MODULE_init(struct vinbero_com_Module* module) {
    VINBERO_COM_LOG_TRACE2();
    module->localModule.pointer = malloc(1 * sizeof(struct vinbero_strm_mt_epoll_Module));
    struct vinbero_strm_mt_epoll_Module* localModule = module->localModule.pointer;
    int out;
    vinbero_com_Config_getInt(module->config, module, "vinbero_strm_mt_epoll.clientTimeoutSeconds", &out, 3);
    localModule->clientTimeout.it_value.tv_sec = out;
    vinbero_com_Config_getInt(module->config, module, "vinbero_strm_mt_epoll.clientTimeoutSeconds", &out, 3);
    localModule->clientTimeout.it_interval.tv_sec = out;
    vinbero_com_Config_getInt(module->config, module, "vinbero_strm_mt_epoll.clientTimeoutNanoSeconds", &out, 0);
    localModule->clientTimeout.it_value.tv_nsec = out;
    vinbero_com_Config_getInt(module->config, module, "vinbero_strm_mt_epoll.clientTimeoutNanoSeconds", &out, 0);
    localModule->clientTimeout.it_interval.tv_nsec = out;
    return VINBERO_COM_STATUS_SUCCESS;
}

int
vinbero_iface_MODULE_rInit(struct vinbero_com_Module* module) {
    return VINBERO_COM_STATUS_SUCCESS;
}

int
vinbero_iface_TLOCAL_init(struct vinbero_com_TlModule* tlModule) {
    VINBERO_COM_LOG_TRACE2();
    tlModule->localTlModule.pointer = malloc(sizeof(struct vinbero_strm_mt_epoll_TlModule));
    struct vinbero_strm_mt_epoll_TlModule* localTlModule = tlModule->localTlModule.pointer;
    int workerCount;
    int workerMaxClients;
    if(vinbero_com_Config_getRequiredInt(tlModule->module->config, tlModule->module, "vinbero_mt.workerCount", &workerCount) == false)
        return VINBERO_COM_ERROR_INVALID_CONFIG;
    vinbero_com_Config_getInt(tlModule->module->config, tlModule->module, "vinbero_strm_mt_epoll.workerMaxClients", &workerMaxClients, 1024);
    localTlModule->epollEventArraySize = workerMaxClients * 2 + 1 + 1; // '* 2': socket, timerfd; '+ 1': serverSocket;  '+ 1': exitEventFd
    localTlModule->epollEventArray = malloc(localTlModule->epollEventArraySize * sizeof(struct epoll_event));
    localTlModule->clientArraySize = workerMaxClients * 2 * workerCount + 1 + 1 + 1 + 3; //'+ 1': serverSocket; '+ 1': epollFd; '+ 1': exitEventFd; '+ 3': stdin, stdout, stderr; multipliying workerCount because file descriptors are shared among threads;
    localTlModule->clientSocketArray = malloc(localTlModule->clientArraySize * sizeof(int));
    memset(localTlModule->clientSocketArray, -1, localTlModule->clientArraySize * sizeof(int));
    localTlModule->clientTimerFdArray = malloc(localTlModule->clientArraySize * sizeof(int));
    memset(localTlModule->clientTimerFdArray, -1, localTlModule->clientArraySize * sizeof(int));
    localTlModule->clModuleArray = calloc(localTlModule->clientArraySize, sizeof(struct vinbero_com_ClModule*));
    localTlModule->clientIoMethods.read = gaio_Fd_read;
    localTlModule->clientIoMethods.write = gaio_Fd_write;
    localTlModule->clientIoMethods.sendfile = gaio_Generic_sendfile;
    localTlModule->clientIoMethods.fstat = gaio_Fd_fstat;
    localTlModule->clientIoMethods.fileno = gaio_Fd_fileno;
    localTlModule->clientIoMethods.close = gaio_Fd_close;

    return VINBERO_COM_STATUS_SUCCESS;
}

int
vinbero_iface_TLOCAL_rInit(struct vinbero_com_TlModule* tlModule) {
    VINBERO_COM_LOG_TRACE2();
    return VINBERO_COM_STATUS_SUCCESS;
}

static int
vinbero_strm_mt_epoll_loadChildClModules(struct vinbero_com_ClModule* clModule) {
    int ret;
    GENC_TREE_NODE_INIT2(clModule, GENC_TREE_NODE_SIZE(clModule->tlModule));
    GENC_TREE_NODE_FOREACH(clModule->tlModule, index) {
        struct vinbero_com_ClModule* childClModule = malloc(sizeof(struct vinbero_com_ClModule));
        GENC_TREE_NODE_ADD(clModule, childClModule);
        childClModule->tlModule = GENC_TREE_NODE_RAW_GET(clModule->tlModule, index);
        childClModule->localClModule.pointer = NULL;
        childClModule->arg = NULL;
        ret = vinbero_strm_mt_epoll_loadChildClModules(childClModule);
        if(ret < VINBERO_COM_STATUS_SUCCESS)
            return ret;
    }
    return VINBERO_COM_STATUS_SUCCESS;
}

static int
vinbero_strm_mt_epoll_initChildClModules(struct vinbero_com_ClModule* clModule) {
    VINBERO_COM_LOG_TRACE2();
    int ret;
    GENC_TREE_NODE_FOREACH(clModule, index) {
        struct vinbero_com_ClModule* childClModule = GENC_TREE_NODE_RAW_GET(clModule, index);
        if(childClModule->arg == NULL)
            childClModule->arg = clModule->arg;
        VINBERO_COM_CALL(CLOCAL, init, childClModule->tlModule->module, &ret, childClModule);
        if(ret < VINBERO_COM_STATUS_SUCCESS)
            return ret;
        if(vinbero_strm_mt_epoll_initChildClModules(childClModule) < VINBERO_COM_STATUS_SUCCESS)
            return ret;
    }
    return VINBERO_COM_STATUS_SUCCESS;
}

static int
vinbero_strm_mt_epoll_rInitChildClModules(struct vinbero_com_ClModule* clModule) {
    VINBERO_COM_LOG_TRACE2();
    int ret;
    GENC_TREE_NODE_FOREACH(clModule, index) {
        struct vinbero_com_ClModule* childClModule = GENC_TREE_NODE_RAW_GET(clModule, index);
        ret = vinbero_strm_mt_epoll_rInitChildClModules(childClModule);
        if(ret < VINBERO_COM_STATUS_SUCCESS)
            return ret;
        VINBERO_COM_CALL(CLOCAL, rInit, childClModule->tlModule->module, &ret, childClModule);
        if(ret < VINBERO_COM_STATUS_SUCCESS)
            return ret;
    }
    return VINBERO_COM_STATUS_SUCCESS;
}

static int
vinbero_strm_mt_epoll_destroyChildClModules(struct vinbero_com_ClModule* clModule) {
    VINBERO_COM_LOG_TRACE2();
    int ret;
    GENC_TREE_NODE_FOREACH(clModule, index) {
        struct vinbero_com_ClModule* childClModule = GENC_TREE_NODE_RAW_GET(clModule, index);
        VINBERO_COM_CALL(CLOCAL, destroy, childClModule->tlModule->module, &ret, childClModule);
        if(ret < VINBERO_COM_STATUS_SUCCESS)
            return ret;
        ret = vinbero_strm_mt_epoll_destroyChildClModules(childClModule);
        if(ret < VINBERO_COM_STATUS_SUCCESS)
            return ret;
    }
    return VINBERO_COM_STATUS_SUCCESS;
}
static int
vinbero_strm_mt_epoll_rDestroyChildClModules(struct vinbero_com_ClModule* clModule) {
    VINBERO_COM_LOG_TRACE2();
    int ret;
    GENC_TREE_NODE_FOREACH(clModule, index) {
        struct vinbero_com_ClModule* childClModule = GENC_TREE_NODE_RAW_GET(clModule, index);
        ret = vinbero_strm_mt_epoll_rDestroyChildClModules(childClModule);
        if(ret < VINBERO_COM_STATUS_SUCCESS) {
            VINBERO_COM_LOG_ERROR("vinbero_strm_mt_epoll_rDstroyChildClModules() FAILED");
            GENC_TREE_NODE_FREE(childClModule);
            free(childClModule);
            return ret;
        }
        VINBERO_COM_CALL(CLOCAL, rDestroy, childClModule->tlModule->module, &ret, childClModule);
        if(ret < VINBERO_COM_STATUS_SUCCESS) {
            VINBERO_COM_LOG_ERROR("vinbero_iface_CLOCAL_rDestroy() FAILED");
            GENC_TREE_NODE_FREE(childClModule);
            free(childClModule);
            return ret;
        }
        GENC_TREE_NODE_FREE(childClModule);
        free(childClModule);
    }
    return VINBERO_COM_STATUS_SUCCESS;
}

static void
vinbero_strm_mt_epoll_destroyClient(struct vinbero_com_TlModule* tlModule, int clientSocket, int timerFd) {
    struct vinbero_strm_mt_epoll_TlModule* localTlModule = tlModule->localTlModule.pointer;
    vinbero_strm_mt_epoll_destroyChildClModules(localTlModule->clModuleArray[clientSocket]);
    vinbero_strm_mt_epoll_rDestroyChildClModules(localTlModule->clModuleArray[clientSocket]);
    free(localTlModule->clModuleArray[clientSocket]->arg);
    GENC_TREE_NODE_FREE(localTlModule->clModuleArray[clientSocket]);
    free(localTlModule->clModuleArray[clientSocket]);
    close(clientSocket); // to prevent double close
    close(timerFd);
    localTlModule->clModuleArray[clientSocket] = NULL;
    localTlModule->clientSocketArray[timerFd] = -1;
    localTlModule->clientTimerFdArray[clientSocket] = -1;
}

static void
vinbero_strm_mt_epoll_handleConnection(struct vinbero_com_TlModule* tlModule, int epollFd, int* serverSocket) {
    VINBERO_COM_LOG_TRACE2();
    int ret;
    struct vinbero_strm_mt_epoll_Module* localModule = tlModule->module->localModule.pointer;
    struct vinbero_strm_mt_epoll_TlModule* localTlModule = tlModule->localTlModule.pointer;
    int clientSocket;
    int timerFd;
    struct epoll_event epollEvent;
    memset(&epollEvent, 0, 1 * sizeof(struct epoll_event));
    if((clientSocket = accept(*serverSocket, NULL, NULL)) == -1) {
        if(errno != EAGAIN)
            VINBERO_COM_LOG_ERROR("accept() FAILED");
        return;
    }
    VINBERO_COM_LOG_DEBUG("ACCEPTED CLIENT SOCKET %d", clientSocket);
    if(clientSocket > (localTlModule->clientArraySize - 1) - 1) { // '-1': room for timerfd
        VINBERO_COM_LOG_ERROR("UNABLE TO ACCEPT CLIENTS ANYMORE");
        close(clientSocket);
        return; 
    }
    if(fcntl(clientSocket, F_SETFL, fcntl(clientSocket, F_GETFL, 0) | O_NONBLOCK) == -1) {
        VINBERO_COM_LOG_ERROR("fcntl() FAILED");
        close(clientSocket);
        return;
    }
    epollEvent.events = EPOLLET | EPOLLIN | EPOLLRDHUP | EPOLLHUP;
    epollEvent.data.fd = clientSocket;
    if(epoll_ctl(epollFd, EPOLL_CTL_ADD, clientSocket, &epollEvent) == -1) {
        VINBERO_COM_LOG_ERROR("epoll_ctl() FAILED");
        close(clientSocket);
        return;
    }
    if((localTlModule->clientTimerFdArray[clientSocket] = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) == -1) {
        VINBERO_COM_LOG_ERROR("timerfd_create() FAILED");
        close(clientSocket);
        return;
    }
    if(fcntl(localTlModule->clientTimerFdArray[clientSocket], F_SETFL, fcntl(localTlModule->clientTimerFdArray[clientSocket], F_GETFL, 0) | O_NONBLOCK) == -1) {
        VINBERO_COM_LOG_ERROR("fcntl() failed");
        close(clientSocket);
        close(localTlModule->clientTimerFdArray[clientSocket]);
        localTlModule->clientTimerFdArray[clientSocket] = -1;
        return;
    }
    if(timerfd_settime(localTlModule->clientTimerFdArray[clientSocket], 0, &localModule->clientTimeout, NULL) == -1) {
        VINBERO_COM_LOG_ERROR("timerfd_settime() failed");
        close(clientSocket);
        close(localTlModule->clientTimerFdArray[clientSocket]);
        localTlModule->clientTimerFdArray[clientSocket] = -1;
        return;
    }
    epollEvent.events = EPOLLIN | EPOLLET;
    epollEvent.data.fd = localTlModule->clientTimerFdArray[clientSocket];
    if(epoll_ctl(epollFd, EPOLL_CTL_ADD, localTlModule->clientTimerFdArray[clientSocket], &epollEvent) == -1) {
        VINBERO_COM_LOG_ERROR("epoll_ctl() failed");
        close(clientSocket);
        close(localTlModule->clientTimerFdArray[clientSocket]);
        localTlModule->clientTimerFdArray[clientSocket] = -1;
        return;
    }
    localTlModule->clientSocketArray[localTlModule->clientTimerFdArray[clientSocket]] = clientSocket;

    struct gaio_Io* clientIo = malloc(sizeof(struct gaio_Io));
    clientIo->object.integer = clientSocket;
    clientIo->methods = &localTlModule->clientIoMethods;

    localTlModule->clModuleArray[clientSocket] = malloc(1 * sizeof(struct vinbero_com_ClModule));
    localTlModule->clModuleArray[clientSocket]->tlModule = tlModule;
    localTlModule->clModuleArray[clientSocket]->arg = clientIo;
    GENC_TREE_NODE_INIT(localTlModule->clModuleArray[clientSocket]);
    timerFd = localTlModule->clientTimerFdArray[clientSocket];
    if((ret = vinbero_strm_mt_epoll_loadChildClModules(localTlModule->clModuleArray[clientSocket])) < VINBERO_COM_STATUS_SUCCESS) {
        VINBERO_COM_LOG_ERROR("vinbero_strm_mt_epoll_loadChildClModules() FAILED");
        vinbero_strm_mt_epoll_destroyClient(tlModule, clientSocket, timerFd);
        return;
    }
    if((ret = vinbero_strm_mt_epoll_initChildClModules(localTlModule->clModuleArray[clientSocket])) < VINBERO_COM_STATUS_SUCCESS) {
        VINBERO_COM_LOG_ERROR("vinbero_strm_mt_epoll_initChildClModules() FAILED");
        vinbero_strm_mt_epoll_destroyClient(tlModule, clientSocket, timerFd);
        return;
    }
    if((ret = vinbero_strm_mt_epoll_rInitChildClModules(localTlModule->clModuleArray[clientSocket])) < VINBERO_COM_STATUS_SUCCESS) {
        VINBERO_COM_LOG_ERROR("vinbero_strm_mt_epoll_rInitChildClModules() FAILED");
        vinbero_strm_mt_epoll_destroyClient(tlModule, clientSocket, timerFd);
        return;
    }
}

static int
vinbero_strm_mt_epoll_handleRequest(struct vinbero_com_TlModule* tlModule, int* serverSocket, int clientSocket, int timerFd) {
    VINBERO_COM_LOG_TRACE2();
    struct vinbero_strm_mt_epoll_Module* localModule = tlModule->module->localModule.pointer;
    struct vinbero_strm_mt_epoll_TlModule* localTlModule = tlModule->localTlModule.pointer;
    int ret;
    if(timerfd_settime(localTlModule->clientTimerFdArray[clientSocket], 0, &localModule->clientTimeout, NULL) == -1) {
        VINBERO_COM_LOG_ERROR("timerfd_settime() FAILED");
        return VINBERO_COM_ERROR_UNKNOWN;
    }
    GENC_TREE_NODE_FOREACH(tlModule->module, index) {
        do {
            struct vinbero_com_Module* childModule = GENC_TREE_NODE_RAW_GET(tlModule->module, index);
            struct vinbero_com_ClModule* childClModule = GENC_TREE_NODE_RAW_GET(localTlModule->clModuleArray[clientSocket], index);
            VINBERO_COM_CALL(CLSERVICE, call, childModule, &ret, childClModule);
            if(ret < VINBERO_COM_STATUS_SUCCESS)
                return ret;
        } while(ret == VINBERO_COM_STATUS_CONTINUE);
    }
    return VINBERO_COM_STATUS_SUCCESS;
}

int
vinbero_iface_TLSERVICE_call(struct vinbero_com_TlModule* tlModule) {
    VINBERO_COM_LOG_TRACE2();
    struct vinbero_strm_mt_epoll_TlModule* localTlModule = tlModule->localTlModule.pointer;
    int* serverSocket = tlModule->arg;
    if(fcntl(*serverSocket, F_SETFL, fcntl(*serverSocket, F_GETFL, 0) | O_NONBLOCK) == -1) {
        VINBERO_COM_LOG_ERROR("FAILED TO SET NON-BLOCKING FOR SERVER SOCKET");
        return VINBERO_COM_ERROR_IO;
    }
    int epollFd = epoll_create1(0);
    struct epoll_event epollEvent;
    memset(&epollEvent, 0, 1 * sizeof(struct epoll_event)); // to avoid valgrind VINBERO_COM_LOG_ERRORing: syscall param epoll_ctl(event) points to uninitialised byte(s)

    epollEvent.events = EPOLLIN | EPOLLET;
    epollEvent.data.fd = *tlModule->exitEventFd;
    epoll_ctl(epollFd, EPOLL_CTL_ADD, *tlModule->exitEventFd, &epollEvent);

    epollEvent.events = EPOLLIN | EPOLLET;
    epollEvent.data.fd = *serverSocket;
    epoll_ctl(epollFd, EPOLL_CTL_ADD, *serverSocket, &epollEvent);

    for(int epollEventCount;;) {
        if((epollEventCount = epoll_wait(epollFd, localTlModule->epollEventArray, localTlModule->epollEventArraySize, -1)) == -1) {
            VINBERO_COM_LOG_ERROR("epoll_wait() FAILED");
            return VINBERO_COM_ERROR_UNKNOWN;
        }
        for(int index = 0; index < epollEventCount; ++index) {
            if(localTlModule->epollEventArray[index].data.fd == *tlModule->exitEventFd) { // exitEventFd
                VINBERO_COM_LOG_DEBUG("EXIT EVENT RECEIVED");
                uint64_t counter;
                read(*tlModule->exitEventFd, &counter, sizeof(counter));
                return VINBERO_COM_STATUS_SUCCESS;
            } else if(localTlModule->epollEventArray[index].data.fd == *serverSocket) { // serverSocket
                vinbero_strm_mt_epoll_handleConnection(tlModule, epollFd, serverSocket);
            } else if(localTlModule->clientTimerFdArray[localTlModule->epollEventArray[index].data.fd] != -1 &&
                      localTlModule->clientSocketArray[localTlModule->epollEventArray[index].data.fd] == -1) { // clientSocket
                int clientSocket = localTlModule->epollEventArray[index].data.fd;
                int timerFd = localTlModule->clientTimerFdArray[clientSocket];
                if(localTlModule->epollEventArray[index].events & EPOLLIN) {
                    VINBERO_COM_LOG_DEBUG("CLIENT SOCKET %d IS READABLE", clientSocket);
                    vinbero_strm_mt_epoll_handleRequest(tlModule, serverSocket, clientSocket, timerFd);
                } else if(localTlModule->epollEventArray[index].events & EPOLLRDHUP)
                    VINBERO_COM_LOG_DEBUG("CLIENT SOCKET %d IS DISCONNECTED", clientSocket);
                else if(localTlModule->epollEventArray[index].events & EPOLLHUP)
                    VINBERO_COM_LOG_WARN("CLIENT SOCKET %d HAS ERROR", clientSocket);
                vinbero_strm_mt_epoll_destroyClient(tlModule, clientSocket, timerFd);
            } else if(localTlModule->clientSocketArray[localTlModule->epollEventArray[index].data.fd] != -1 &&
                    localTlModule->clientTimerFdArray[localTlModule->epollEventArray[index].data.fd] == -1 &&
                    localTlModule->epollEventArray[index].events & EPOLLIN) { // clientTimerFd
                int timerFd = localTlModule->epollEventArray[index].data.fd;
                int clientSocket = localTlModule->clientSocketArray[timerFd];
                uint64_t clientTimerFdValue;
                read(timerFd, &clientTimerFdValue, sizeof(uint64_t));
                VINBERO_COM_LOG_WARN("CLIENT SOCKET %d TIMEOUT", clientSocket);
                vinbero_strm_mt_epoll_destroyClient(tlModule, clientSocket, timerFd);

            } else {
                VINBERO_COM_LOG_FATAL("UNEXPECTED FILE DESCRIPTOR");
                return VINBERO_COM_ERROR_UNKNOWN;
            }
        }
    }
    return VINBERO_COM_STATUS_SUCCESS;
}

int
vinbero_iface_TLOCAL_destroy(struct vinbero_com_TlModule* tlModule) {
    VINBERO_COM_LOG_TRACE2();
    return VINBERO_COM_STATUS_SUCCESS;
}

int
vinbero_iface_TLOCAL_rDestroy(struct vinbero_com_TlModule* tlModule) {
    VINBERO_COM_LOG_TRACE2();
    struct vinbero_strm_mt_epoll_TlModule* localTlModule = tlModule->localTlModule.pointer;
    if(localTlModule != NULL) {
        free(localTlModule->epollEventArray);
        free(localTlModule->clientSocketArray);
        free(localTlModule->clientTimerFdArray);

        for(size_t index = 0; index != localTlModule->clientArraySize; ++index) {
            if(localTlModule->clModuleArray[index] != NULL)
                free(localTlModule->clModuleArray[index]);
        }
        free(localTlModule->clModuleArray);
        free(localTlModule);
    }
    return VINBERO_COM_STATUS_SUCCESS;
}

int
vinbero_iface_MODULE_destroy(struct vinbero_com_Module* module) {
    VINBERO_COM_LOG_TRACE2();
    return VINBERO_COM_STATUS_SUCCESS;
}

int
vinbero_iface_MODULE_rDestroy(struct vinbero_com_Module* module) {
    VINBERO_COM_LOG_TRACE2();
    free(module->localModule.pointer);
    return VINBERO_COM_STATUS_SUCCESS;
}
