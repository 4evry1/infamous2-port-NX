/* Horizon layer: <sys/un.h>. No Unix-domain sockets on Horizon; the type exists so optional
 * code (the achievements client) compiles. connect() on it fails at run time. */
#ifndef HZ_SYS_UN_H
#define HZ_SYS_UN_H
#include <sys/socket.h>
struct sockaddr_un { sa_family_t sun_family; char sun_path[108]; };
#endif
