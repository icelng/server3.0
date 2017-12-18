/*单例模式,一个进程只允许存在一个user-manager*/
#include "usrmng.h"
#include "stdio.h"
#include "unistd.h"
#include "sys/socket.h"
#include "arpa/inet.h"
#include "pthread.h"
#include "encdec.h"
#include "cJSON.h"
#include "sys/stat.h"
#include "fcntl.h"
#include "middleware.h"
#include "log.h"

/* 自定义数据声明 */
/* 数据报头联合体 */
union usrmng_login_head{
    unsigned char u8[4];
    unsigned int u32;
};


/*私有全局变量*/
static unsigned long g_pid_login_thread;
static char *g_login_rsa_prikey = NULL;  // 登录认证所需的rsa秘钥
static char *g_login_rsa_pubkey = NULL;  // 登录认证所需的rsa公钥
static struct mw_chain *g_p_login_chain;  // 登录中间件链
/*配置,初始化时，会根据配置文件装载相应的配置值*/
static struct usrmng_cfg{
    unsigned int login_rsa_keylen;  // 登录认证RSA秘钥长度
    int login_socket_port;  // 登录的套接字(tcp)端口
    unsigned int max_login_num;  // 最大登录数量
}g_usrmng_cfg;
/*私有全局变量*/


/*以下是函数声明部分*/
int usrmng_load_config(struct usrmng_cfg *pcfg, char *cfg_file_path);
int usrmng_create_rsakeypair(char *prikeybuf, char *pubkeybuf, unsigned int pribuf_size, unsigned int pubbuf_size, unsigned int key_len);
void *usrmng_login_thread(void *arg);
void *usrmng_login_accept_thread(void *arg);
/*以上是函数声明部分*/


/* 以下是中间件的定义 */

/* 中间件名: usrmng_mw_login_rcv
 * 功能: 接收用户名和密码(加密的)
 * 中间数据参数类型: 接收用户名和密码的套接字
 */
MIDDLEWARE_DEFINE(usrmng_mw_login_rcv, pchain, pmdata)
    struct next_mw_arg{  // 下一个中间件的参数（中间数据）
        int clt_socket;
        unsigned int verify_info_size;
        char *verify_info;
    }*p_next_mw_arg;
    struct mw_middata *p_new_mdata;
    const char syc_c = '\r';  //同步字符
    const char ctl_c = 0x10;  //控制字符
    int clt_socket = *(int *)pmdata->data;
    int syc_cnt = 0;  //同步计数
    int monitor_cnt = 0; //监听字符计数
    int h_i = 0; //接收head的index
    unsigned int msg_data_size;  // 数据报的数据大小,在这是加密的用户名+密码的大小
    unsigned char msg_type;  // 数据报的类型
    union usrmng_login_head head;
    struct timeval timeout;   //设置超时用
    char rcv_c;
    int rcv_status = 0;//0,处于监听状态，1处于报文接收状态

    log_out(LOG_DEBUG, "Go into middleware:login_rcv");

    timeout.tv_sec = 5;  //5秒钟超时
    timeout.tv_usec = 0;
    //设置接收超时
    setsockopt(clt_socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(struct timeval));

    while(1){
        if(rcv_status == 0){  //处于报文接收状态
            if(monitor_cnt++ == 50){ //如果监听的字符计数超过了50则退出
                /* 异常 */
                MIDDLEWARE_EXCEPTION(USRMNG_LOGIN_RCV_EXCEPTION, 1, NULL);
            }
            if(recv(clt_socket,&rcv_c,1,0) == -1){
                /* 异常 */
                MIDDLEWARE_EXCEPTION(USRMNG_LOGIN_RCV_EXCEPTION, 2, NULL);
            }
            if(rcv_c == syc_c){
                if(++syc_cnt == 3){  //开始接收head
                    while(1){
                        if(recv(clt_socket,&rcv_c,1,0) == -1){
                            /* 异常 */
                            MIDDLEWARE_EXCEPTION(USRMNG_LOGIN_RCV_EXCEPTION, 3, NULL);
                        }
                        if(rcv_c == syc_c)continue;
                        if(rcv_c == ctl_c){
                            if(recv(clt_socket,&rcv_c,1,0) == -1){
                                /* 异常 */
                                MIDDLEWARE_EXCEPTION(USRMNG_LOGIN_RCV_EXCEPTION, 4, NULL);
                            }
                        }
                        head.u8[h_i++] = rcv_c;
                        if(h_i == 4){  // 头的大小
                            rcv_status = 1;
                            break;
                        }
                    }
                }
            }else{
                syc_cnt = 0;
            }
        }else if(rcv_status == 1){
            /* 低24位是数据报的数据大小，并且是4字节对齐 */
            msg_data_size = (head.u32 & 0x0FFF) << 2;
            /* 高8位是数据报类型 */
            msg_type = head.u8[3];
            p_new_mdata = mw_create_middata(pchain, msg_data_size + sizeof(struct next_mw_arg));
            if(p_new_mdata == NULL){
                /* 异常 */
                MIDDLEWARE_EXCEPTION(USRMNG_LOGIN_RCV_EXCEPTION, 5, NULL);
            }
            p_next_mw_arg = p_new_mdata->data;
            p_next_mw_arg->verify_info = (void*)p_next_mw_arg + sizeof(struct next_mw_arg);
            p_next_mw_arg->verify_info_size = msg_data_size;
            p_next_mw_arg->clt_socket = clt_socket;
            if(recv(clt_socket, p_next_mw_arg->verify_info, msg_data_size, 0) < 0){
                /* 异常 */
                mw_destroy_middata(pchain, p_new_mdata);  // 在此中间件里申请得到的中间数据记得释放掉
                MIDDLEWARE_EXCEPTION(USRMNG_LOGIN_RCV_EXCEPTION, 6, NULL);
            }
            /* 用户名和密码（RSA加密的）接收完成，到下一个中间件 */
            MIDDLEWARE_NEXT(p_new_mdata);
        }
    }
MIDDLEWARE_DEFINE_END


/* 中间件名: usrmng_mw_login_dec
 * 功能: 解密用户名和密码
 * 中间数据参数类型: 经RSA加密的用户名和密码
 */
MIDDLEWARE_DEFINE(usrmng_mw_login_dec, pchain, pmdata)
    log_out(LOG_DEBUG, "Go into middleware:login_dec");
    MIDDLEWARE_COMPLETE();  // 暂时停止(调试用)
MIDDLEWARE_DEFINE_END

/* 中间件名: usrmng_mw_login_verify
 * 功能: 校验用户名和密码
 * 中间数据参数类型: 明文的用户名和32bitMD5的密码+用户登录的套接字
 */
MIDDLEWARE_DEFINE(usrmng_mw_login_verify, pchain, pmdata)
    log_out(LOG_DEBUG, "Go into middleware:login_verify");

MIDDLEWARE_DEFINE_END

/* 中间件名: usrmng_mw_login_register
 * 功能: 生成用户信息节点，并且把节点插入(注册)到红黑树中,并且向数据库标记登录状
 *       态。
 * 中间数据参数类型: 用户登录的套接字+用户ID
 */
MIDDLEWARE_DEFINE(usrmng_mw_login_register, pchain, pmdata)
    log_out(LOG_DEBUG, "Go into middleware:login_register");
MIDDLEWARE_DEFINE_END

/* 中间件名: usrmng_mw_login_allocsocket
 * 功能: 利用worker-manager模块分配套接字给worker进程
 * 中间数据参数类型: 用户登录的套接字+用户ID
 */
MIDDLEWARE_DEFINE(usrmng_mw_login_allocsocket, pchain, pmdata)
    log_out(LOG_DEBUG, "Go into middleware:login_allocsocket");
MIDDLEWARE_DEFINE_END

/* 异常中间件名: usrmng_login_rcv_except
 * 功能: 登录接收用户名密码异常处理
 * 中间数据参数类型: 
 */
MIDDLEWARE_EXCEPTION_DEFINE(usrmng_login_rcv_except, pchain, except_type, sub_type, pmdata)
    log_out(LOG_DEBUG, "Login Recv Exception!sub_type:%d", sub_type);
    MIDDLEWARE_COMPLETE();
MIDDLEWARE_DEFINE_END

/* 异常中间件名: usrmng_login_dec_except
 * 功能: 登录用户名+密码解密异常
 * 中间数据参数类型: 
 */
MIDDLEWARE_EXCEPTION_DEFINE(usrmng_login_dec_except, pchain, except_type, sub_type, pmdata)
    log_out(LOG_DEBUG, "Login Recv Exception!sub_type:%d", sub_type);
    MIDDLEWARE_COMPLETE();
MIDDLEWARE_DEFINE_END


/* 以上是中间件的定义 */

/* 函数名: int usrmng_init()
 * 功能: 初始化用户管理器
 * 参数: 无
 * 返回值: 1, 初始化成功
 *        <0, 初始化失败
 */
int usrmng_init(char *cfg_file_path){
    usrmng_load_config(&g_usrmng_cfg, cfg_file_path);  // 加载配置
    /*生成登录RSA秘钥对*/
    g_login_rsa_prikey = malloc(g_usrmng_cfg.login_rsa_keylen);
    g_login_rsa_pubkey = malloc(g_usrmng_cfg.login_rsa_keylen);
    if(usrmng_create_rsakeypair(g_login_rsa_pubkey, 
                g_login_rsa_prikey, 
                g_usrmng_cfg.login_rsa_keylen, 
                g_usrmng_cfg.login_rsa_keylen, 
                g_usrmng_cfg.login_rsa_keylen) < 0){
        return -1;

    }
    /* 初始化登录中间件链 */
    g_p_login_chain = mw_create_chain(NULL);  // 默认选项的中间件链
    /* 按顺序添加中间件 */
    mw_chain_add(g_p_login_chain, usrmng_mw_login_rcv);
    mw_chain_add(g_p_login_chain, usrmng_mw_login_dec);
    /*打开登录套接字连接接收线程*/
    pthread_create(&g_pid_login_thread, NULL, usrmng_login_accept_thread, NULL);
    return 1;
}

/* 函数名: int usrmng_load_config()
 * 功能: 加载配置文件，给g_usrmng_cfg装载相应的配置值
 * 参数: 无
 * 返回值: 1, 加载成功
 *        <0, 加载的过程中出现了一些意外
 */
int usrmng_load_config(struct usrmng_cfg *pcfg, char *cfg_file_path){
    struct stat st_config_f;
    cJSON *config_json_root;
    cJSON *config_json_usrmng;
    char *config_str;
    int config_str_len;
    int fd_config;
    /*获取配置文件大小(写过python和java之后，感觉这个好麻烦)*/
    stat(cfg_file_path, &st_config_f);
    config_str_len =  st_config_f.st_size;
    /*以下加载配置文件*/
    config_str = (char*)malloc(config_str_len + 1);
    fd_config = open(cfg_file_path, O_RDONLY);
    read(fd_config, config_str, config_str_len);
    close(fd_config);  // 关闭文件
    /*解析json*/
    config_json_root = cJSON_Parse(config_str);
    config_json_usrmng = cJSON_GetObjectItemCaseSensitive(config_json_root, "master");
    config_json_usrmng = cJSON_GetObjectItemCaseSensitive(config_json_usrmng, "usrmng");
    if(config_json_usrmng == NULL){
        /*没有找到master,或者usrmng，则使用默认的配置*/
        pcfg->login_rsa_keylen = USRMNG_DEFAULT_LOGIN_RSA_KEYLEN;
        pcfg->login_socket_port = USRMNG_DEFAULT_LOGIN_SOCKET_PORT;
        pcfg->max_login_num = USRMNG_DEFAULT_MAX_LOGIN_NUM;
        return 1;
    }
    /*挨个参数读取*/
    /*login_rsa_keylen*/
    USRMNG_LOAD_PARAM_INT(
            pcfg->login_rsa_keylen, 
            config_json_usrmng, 
            login_rsa_keylen, 
            USRMNG_DEFAULT_LOGIN_RSA_KEYLEN);
    /*login_socket_port*/
    USRMNG_LOAD_PARAM_INT(
            pcfg->login_socket_port, 
            config_json_usrmng, 
            login_rsa_socket_port, 
            USRMNG_DEFAULT_LOGIN_SOCKET_PORT);
    /*max_login_num*/
    USRMNG_LOAD_PARAM_INT(
            pcfg->max_login_num, 
            config_json_usrmng, 
            max_login_num, 
            USRMNG_DEFAULT_MAX_LOGIN_NUM);

    return 1;
}


/* 函数名: int usrmng_create_rsakeypair(char *prikeybuf, char *pubkeybuf, unsigned int pribuf_size, unsigned int pubbuf_size, unsgined int len)
 * 功能: 生成RSA密钥对(Base64编码)
 * 参数: char *prikeybuf, 私钥缓存区
 *       char *pubkeybuf, 公钥缓存区
 *       unsigned int pribuf_size, 私钥缓存区大小
 *       unsigned int pubbuf_size, 公钥缓存区大小
 *       unsigned int key_len, 秘钥长度
 * 返回值: 1, 生成成功
 *        <0, 生成失败
 */
int usrmng_create_rsakeypair(char *pubkeybuf, char *prikeybuf, unsigned int pubbuf_size, unsigned int pribuf_size, unsigned int key_len){
    unsigned int nbits;  // 秘钥长度
    int gen_err_times = 0;
    if(prikeybuf == NULL || pubkeybuf == NULL){
        return -1;
    }
    switch(key_len){
        case 1024:
            nbits = 1024;
            if(pribuf_size < 921 || pubbuf_size < 273){
                return -2;
            }
            break;
        case 2048:
            nbits = 2048;
            if(pribuf_size < 1709 || pubbuf_size < 452){
                return -3;
            }
            break;
        case 4096:
            nbits = 4096;
            if(pribuf_size < 3277 || pubbuf_size < 801){
                return -4;
            }
        default:
            /*长度非法*/
            return -5;
    }
    while(1){
        /*生成秘钥对*/
        if(rsa_gen_keys(nbits, pubkeybuf, prikeybuf) < 0){
            if(++gen_err_times == 5){
                return -6;
            }
            continue;
        }
        /*验证秘钥对有效性*/
        if(rsa_check_keypair(pubkeybuf, prikeybuf) != 1){
            /*秘钥对无效*/
            if(++gen_err_times == 5){
                return -6;
            }
            continue;
        }
    }
    
    return 1;
}

/* 函数名: int usrmng_get_login_pubkey(char *keybuf, unsigned int buf_size)
 * 功能: 获取认证登录所需的公钥
 * 参数: char *keybuf, 接收公钥(base64编码)的缓存
 *       unsigned int buf_size, 缓存大小
 * 返回值: 1, 获取成功
 *        <0, 获取失败
 */
int usrmng_get_login_pubkey(char *keybuf, unsigned int buf_size){
    if(keybuf == NULL){
        /*传入了非法指针*/
        return -1;
    }
    if(g_login_rsa_pubkey == NULL){
        /*User Manager还未被初始化*/
        return -2;
    }
    if(buf_size < strlen(g_login_rsa_pubkey) + 1){
        /*缓存区空间不够*/
        return -3;
    }
    strcpy(keybuf, g_login_rsa_pubkey);
    return 1;
}


/* 函数名: void* usrmng_login_accept_thread(void *arg)
 * 功能: 登录套接字连接接收
 * 参数: void *arg, 线程执行的参数
 * 返回值: 无
 */
void* usrmng_login_accept_thread(void *arg){
    int err_val;
    int clt_sockfd;  // 客户端的套接字
    int server_sockfd;
    int sin_size;
    int option_val = 1;  // 套接字设置值
    struct sockaddr_in clt_addr, server_addr;
    struct mw_middata *p_mdata;
    /* 创建服务端套接字 */
    if((server_sockfd = socket(AF_INET,SOCK_STREAM,0)) == -1){
        log_out(LOG_DEBUG, "Failed to create login(server) sockfd!%s", strerror(errno));
        exit(1);  // 创建失败了，当然要退出了
    }
    /* 设置允许端口重用 */
    if(setsockopt(server_sockfd, 
                SOL_SOCKET, 
                SO_REUSEADDR, 
                &option_val, 
                sizeof(option_val)) < 0){  
        log_out(LOG_DEBUG, "Failed to set option for login-sockfd!%s", strerror(errno));
    }
    memset(&server_addr,0,sizeof(struct sockaddr_in));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(g_usrmng_cfg.login_socket_port);  // 端口可配置
    if(bind(server_sockfd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr_in)) == -1){
        log_out(LOG_DEBUG, "Failed to bind login-sockfd!%s", strerror(errno));
        exit(1);  // 绑定失败的话，就没必要继续执行了
    }
    /* 绑定好了之后，开始监听连接 */
    listen(server_sockfd, g_usrmng_cfg.max_login_num);
    
    while(1){
        memset(&clt_addr,0,sizeof(struct sockaddr_in));
        sin_size = sizeof(struct sockaddr_in);  
        /* 接收客户端的连接请求 */
        if((clt_sockfd = accept(server_sockfd, 
                        (struct sockaddr*)&clt_addr,
                        (socklen_t *)&sin_size)) < 0){
            log_out(LOG_DEBUG, "Login accept error:%s", strerror(errno));
            continue;
        }
        /* 三次握手完毕之后，下来工作由登录中间件负责 */
        p_mdata = mw_create_middata(g_p_login_chain, sizeof(int));
        if(p_mdata == NULL){
            log_out(LOG_DEBUG, "Failed to create midata for clt_sockfd");
            continue;
        }
        *(int *)p_mdata->data = clt_sockfd;
        /* 启动登录中间件链 */
        if((err_val = mw_chain_start(g_p_login_chain, p_mdata)) < 0){
            log_out(LOG_DEBUG, "Failed to start login-chain!%s,err_val:%d", strerror(errno), err_val);
            mw_destroy_middata(g_p_login_chain, p_mdata);
            continue;
        }
    }
}
