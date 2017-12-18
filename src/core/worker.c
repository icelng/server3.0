#include "worker.h" 
#include "stdio.h"
#include "unistd.h"
#include "pthread.h"
#include "stdlib.h"
#include "sys/syslog.h"
#include "sys/stat.h"
#include "cJSON.h"
#include "fcntl.h"
#include "string.h"
#include "ipc.h"
#include "errno.h"


/* 函数名: void wk_load_config(struct wk_cfg_st &cfg)
 * 功能: 加载配置参数
 * 参数:
 * 返回值: 无
 */
void wk_load_config(wk_cfg_st *cfg){
    struct stat st_config_f;
    cJSON *config_json_root;
    cJSON *item_temp;
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
    /*加载wkmng_usocket_path参数值*/
    item_temp = cJSON_GetObjectItemCaseSensitive(config_json_root, "wkmng_usocket_path");
    if(cJSON_IsString(item_temp)){
        cfg->wkmng_usocket_path = (char *)malloc(strlen(item_temp->string) + 1);
        strcpy(cfg->wkmng_usocket_path, item_temp->string);
    }else{
        strcpy(cfg->wkmng_usocket_path, DEFAULT_WKMNG_USOCKET_PATH);
    }
    cJSON_Delete(config_json_root);
}

/* 函数名: void wk_init_log()
 * 功能: 初始化syslog
 * 参数:
 * 返回值:
 */
void wk_init_log(){
    int logfd = open("debug.log",O_RDWR|O_CREAT|O_APPEND,0644);

    if(logfd == -1){
        printf("failed to open debug.log\n");
        exit(1);
    }
    close(STDERR_FILENO);
    dup2(logfd,STDERR_FILENO);
    close(logfd);
    openlog("muma-server",LOG_PID|LOG_CONS|LOG_PERROR,0);   
}


/* 函数名: void *wk_wkmng_rcvsock_thread(void *arg)
 * 功能: worker管理器里，负责接收套接字的线程
 * 参数: void *arg, 指向unix套接字描述符的指针
 * 返回值: 无
 */
void *wk_wkmng_rcvsock_thread(void *arg){
    int wkmng_usocket_fd = *(int *)arg;
    
    while(1){
        /* 
         * 
         * 此处应该有接受客户端套接字的代码(暂时不写)
         *
         * */
        sleep(1);
    }
}


/* 函数名: int wk_init_wkmng(wk_cfg_st *cfg)
 * 功能: 初始化worker进程管理器(与master中worker进程管理器成一体)
 * 参数: wk_cfg_st *cfg, 配置结构体
 * 返回值: 1, 初始化能够成功
 *        <0, 失败
 */
int wk_init_wkmng(wk_cfg_st *cfg){
    int err_val;  // 函数返回的错误号
    int errno_temp;
    int usocket_fd;  // worker进程管理器所用的unix套接字描述符
    pthread_t thread_id;
    usocket_fd = unix_socket_connect(cfg->wkmng_usocket_path);
    if(usocket_fd < 0){
        syslog(LOG_DEBUG,"Failed to connect(usocket),err_val:%d", usocket_fd);
        err_val = -1;
        errno_temp = errno;
        goto ret_err1;
    }

    /*创建客户端套接字接收(从master传过来的)线程*/
    if(pthread_create(&thread_id, NULL, wk_wkmng_rcvsock_thread, &usocket_fd) < 0){
        err_val = -2;
        errno_temp = errno;
        goto ret_err2;
    }

    return 1;
ret_err2:
    close(usocket_fd);
ret_err1:
    return err_val;
}


/* 函数名: int wk_init()
 * 功能: 初始化worker进程
 * 参数:
 * 返回值: 
 */
int wk_init(){
    wk_cfg_st cfg;

    wk_load_config(&cfg);  // 加载配置参数
    wk_init_log();  // 初始化syslog
    syslog(LOG_DEBUG,"Start worker，pid:%d\n", getpid());
    wk_init_wkmng(&cfg);
    return 1;
}


/* 函数名: void wk_start()
 * 功能: 启动worker进程
 * 参数:
 * 返回值: 无
 */
void wk_start(){
    wk_init();
    while(1){
        sleep(1);
    }
}


