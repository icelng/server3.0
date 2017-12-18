#include "list.h"
#include "semaphore.h"

#define MAX_ACCEPT_ERROR_TIMES 5
#define DEFAULT_WKMNG_USOCKET_PATH "./master-wkmng-usocket"

/*worker进程管理信息描述结构*/
typedef struct {
    int listen_sockfd;  // 监听接收unix socket连接请求的套接字描述符
    struct list_head wk_list_head;  // worker进程链表头
    sem_t wk_list_mutex;  // worker进程链表锁
}master_wkmng_dec;

/*worker进程信息描述结构*/
typedef struct {
    struct list_head wk_list_node;  // 嵌入到一个链表里,作为worker链表中的某个节点
    unsigned long int rcvthread_id;  // 信息接受线程id
    int unix_sockfd;  // unix套接字描述符
    int client_num;  // 客户端数量
}master_wk_dec;

/*配置结构体*/
typedef struct {
    char *wkmng_usocket_path;  // unix socket 对应文件路径
    int workers_num;  // 工作线程数量
}master_cfg_st;

