#include "master.h"
#include "worker.h"
#include "ipc.h"
#include "cJSON.h"
#include "unistd.h"
#include "stdio.h"
#include "stdlib.h"
#include "stddef.h"
#include "errno.h"
#include "pthread.h"
#include "fcntl.h"
#include "string.h"
#include "sys/stat.h"
#include "sys/syslog.h"
#include "sys/un.h"
#include "sys/socket.h"



/* 函数名: void master_init_log()
 * 功能: 初始化syslog
 * 参数:
 * 返回值:
 */
void master_init_log(){
    int logfd = open("debug.log",O_RDWR|O_CREAT|O_APPEND,0644);

    if(logfd == -1){
        printf("failed to open debug.log\n");
        exit(1);
    }
    close(STDERR_FILENO);
    dup2(logfd,STDERR_FILENO);
    close(logfd);
    openlog("mms-master", LOG_PID|LOG_CONS|LOG_PERROR,0);   
}

/* 函数名: void load_config(struct master_cfg_st &cfg)
 * 功能: 加载配置参数
 * 参数:
 * 返回值: 无
 */
void master_load_config(master_cfg_st *cfg){
    struct stat st_config_f;
    cJSON *config_json_root;
    cJSON *workers_num_item;
    char *config_str;
    int config_str_len;
    int fd_config;
    /*获取配置文件大小(写过python和java之后，感觉这个好麻烦)*/
    stat("./config.json", &st_config_f);
    config_str_len =  st_config_f.st_size;
    /*以下加载配置文件*/
    config_str = (char*)malloc(config_str_len + 1);
    fd_config = open("./config.json", O_RDONLY);
    read(fd_config, config_str, config_str_len);
    close(fd_config);  // 关闭文件
    /*解析json*/
    config_json_root = cJSON_Parse(config_str);
    workers_num_item = cJSON_GetObjectItemCaseSensitive(config_json_root, "workers_num");
    if(cJSON_IsNumber(workers_num_item)){
        cfg->workers_num = workers_num_item->valueint;
    }
}


/* 函数名: int master_wkmng_sockalloc(master_wkmng_dec *p_wkmng_dec, int sockfd)
 * 功能: 把客户端套接字分配到合适的worker进程里
 * 参数: master_wkmng_dec *p_wkmng_dec, worker进程管理器信息描述结构体
 *       int sockfd, 客户端的套接字
 * 返回值: 1, 分配成功
 *        <0, 分配失败，具体失败位置对应具体的负值
 */
int master_wkmng_sockalloc(master_wkmng_dec *p_wkmng_dec, int sockfd){

}


/* 函数名: void *master_wkmng_rcvthread(void *arg)
 * 功能: worker进程信息接受线程
 * 参数: void *arg, 指向worker进程信息描述结构体
 * 返回值: 无
 */
void *master_wkmng_rcvthread(void *arg){
    master_wk_dec *wk_dec = (master_wk_dec*)arg;

    syslog(LOG_DEBUG,"Start thread:master_wkmng_rcvthread");
    while(1){
        /*
         *
         * 这里应该有接受进程信息的代码(暂时不写)
         *
         *
         * */
        sleep(1);
    }
}


/* 函数名: int master_create_worker()
 * 功能: 创建worker进程
 * 参数: master_dec *mdec, master进程信息描述结构指针
 * 返回值: <0, 出错了
 *          1, worker进程创建成功
 */
int master_create_worker(master_wkmng_dec *p_wkmng_dec){
    uid_t fpid;
    uid_t accept_pid = 0;  // 请求socket连接的进程id
    int accept_usockfd;  // accept得到的socket描述符
    int ret_val;  // 专门用来记录函数返回值
    int err_val = 0;
    int err_t;  // 计算失败次数
    int errno_temp;
    master_wk_dec *pwkdec = NULL;  // 指向worker进程描述结构体的指针

    fpid = fork();
    if(fpid == 0){
        /*子进程*/
        wk_start();  // 启动worker
    }else{
        /*当前进程(父进程), 此时的fpid为子进程的pid*/
        err_t = 0;
        while(1){
            /*尝试接收刚刚fork的子进程的unix socket连接请求*/
            accept_usockfd = unix_socket_accept(p_wkmng_dec->listen_sockfd, &accept_pid);
            if(fpid == accept_pid){  // 接收连接请求的子进程正式刚刚建立的子进程
                break;
            }
            if(++err_t == MAX_ACCEPT_ERROR_TIMES){
                err_val = -1;
                errno_temp = errno;
                goto ret_err1;
            }
        }
        /*此刻没有向内存池申请内存，是因为这里开辟内存的频次不高*/
        pwkdec = (master_wk_dec *)malloc(sizeof(master_wk_dec)); 
        if(pwkdec == NULL){
            err_val = -2;
            errno_temp = errno;
            goto ret_err2;
        }
        /*此时此刻已经成功地建立了unix socket通信,开始填充worker描述结构体*/
        pwkdec->client_num = 0;  // 刚建立的worker进程所持有客户端套接字数量为0
        pwkdec->unix_sockfd = accept_usockfd;
        pwkdec->rcvthread_id = 0;
        sem_wait(&p_wkmng_dec->wk_list_mutex);  // 互斥访问链表
        my_list_add(&pwkdec->wk_list_node, &p_wkmng_dec->wk_list_head);  // 链入worker进程链表
        /*最后启动worker进程信息接受线程(一个worker进程对应一个信息接受线程)*/
        if(pthread_create(&pwkdec->rcvthread_id, NULL, master_wkmng_rcvthread, pwkdec) < 0){
            syslog(LOG_DEBUG,"Failed to create thread:master_wkmng_rcvthread");
            err_val = -3;
            errno_temp = errno;
            goto ret_err3;
        }
    }
    return 1;
ret_err3:
    free(pwkdec);
ret_err2:
    close(accept_usockfd);
ret_err1:
    errno = errno_temp;
    return err_val;
}


/* 函数名: int master_init_wkmng(master_cfg_st *cfg)
 * 功能: 启动worker进程管理器, 因为工作进程并不多，所以不使用epoll，而是采用一个
 *       一个worker进程对应master进程中一个线程的机制。
 * 参数: master_cfg_st *cfg, 指向配置结构体的指针
 * 返回值: <0, 出现了错误
 *          1, 启动成功
 */
int master_init_wkmng(master_cfg_st *cfg){
    int err_val = 0, errno_temp;
    int err_ret;
    int i;
    master_wkmng_dec *p_wkmng_dec = NULL;
    syslog(LOG_DEBUG,"Init worker-manager");
    /*初始化worker进程管理信息描述结构体*/
    p_wkmng_dec = (master_wkmng_dec *)malloc(sizeof(master_wkmng_dec));
    if(p_wkmng_dec == NULL){
        err_val = -1;
        errno_temp = errno;
        goto ret_err1;
    }
    INIT_LIST_HEAD(&p_wkmng_dec->wk_list_head);  // 初始化worker链表
    sem_init(&p_wkmng_dec->wk_list_mutex, 0, 1);  // 初始化worker链表锁
    /*创建监听worker进程请求unix套接字连接的套接字*/
    p_wkmng_dec->listen_sockfd = unix_socket_listen(cfg->wkmng_usocket_path);
    if(p_wkmng_dec->listen_sockfd < 0){
        syslog(LOG_DEBUG,"Failed to create unix socket for worker-manager!err_val:%d", p_wkmng_dec->listen_sockfd);
        err_val = -2;
        errno = errno_temp;
        goto ret_err2;
    }
    syslog(LOG_DEBUG,"Create workers");
    for(i =  0;i < cfg->workers_num;i++){
        err_ret = master_create_worker(p_wkmng_dec);  // 创建工作进程
        if(err_ret < 0){
            syslog(LOG_DEBUG,"Failed to create woker!err_val:%d", err_ret);
            err_val = -3;
            errno = errno_temp;
            goto ret_err2;
        }
    }
    return 1;
ret_err2:
    free(p_wkmng_dec);
ret_err1:
    errno = errno_temp;
    return err_val;
}

int main(){
    master_cfg_st cfg;  // 配置结构体

    master_load_config(&cfg);  // 加载配置参数
    master_init_log();  // 初始化syslog
    master_init_wkmng(&cfg);  // 初始化worker进程管理器

    return 1;
}
