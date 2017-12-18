 
#define DEFAULT_WKMNG_USOCKET_PATH "./master-wkmng-usocket"
/*配置结构体*/
typedef struct {
    char *wkmng_usocket_path;  // unix socket 对应文件路径
}wk_cfg_st;

void wk_start();
