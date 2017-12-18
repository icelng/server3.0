

#define USRMNG_DEFAULT_LOGIN_RSA_KEYLEN 2048
#define USRMNG_DEFAULT_LOGIN_SOCKET_PORT 1080
#define USRMNG_DEFAULT_MAX_LOGIN_NUM 10000

#define USRMNG_LOGIN_RCV_EXCEPTION 1
#define USRMNG_LOGIN_DEC_EXCEPTION 2
#define USRMNG_LOGIN_VERIFY_EXCEPTION 3
#define USRMNG_LOGIN_REGISTER_EXCEPTION 4
#define USRMNG_LOGIN_ALLOCSOCKET_EXCEPTION 5


#define USRMNG_LOAD_PARAM_INT(param_var, cjson_root, param_name, default_val) \
    do{ \
        cJSON *item; \
        item = cJSON_GetObjectItemCaseSensitive(config_json_usrmng, #param_name); \
        if(cJSON_IsNumber(item)){ \
            param_var = item->valueint; \
        }else{ \
            param_var = default_val; \
        } \
    }while(0)


int usrmng_get_login_pubkey(char *keybuf, unsigned int buf_size);
int usrmng_init();
