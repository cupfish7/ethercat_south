#include "ethercat_south_parse.h"   /* 三个结构体和公开函数声明都在这里 */
#include "cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
#include <windows.h>       //移植的时候注意注意
#endif  // _WIN32

#define CONFIG_PATH_MAX      1024
#define CONFIG_JSON_FILE_MAX (1024u * 1024u)   /* 单个配置文件的字节上限 */

/* ---------- 字段层：缺失即保持默认值，存在但类型错则报错 ---------- */

static int get_str_field(const cJSON *obj, const char *key, char *dst, size_t dst_size)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item == NULL) return 0;                  /* 字段缺失：保持默认值 */
    if (!cJSON_IsString(item)) return -1;        /* 类型错：报错，不要静默取空 */
    strncpy(dst, item->valuestring, dst_size - 1);   /* dst 已 memset，末尾必为 '\0' */
    return 0;
}

static int get_int_field(const cJSON *obj, const char *key, int *dst)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item == NULL) return 0;
    if (!cJSON_IsNumber(item)) return -1;
    *dst = item->valueint;
    return 0;
}

static int get_double_field(const cJSON *obj, const char *key, double *dst)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item == NULL) return 0;
    if (!cJSON_IsNumber(item)) return -1;
    *dst = item->valuedouble;
    return 0;
}

/* 读十六进制字符串字段（"0x00000002"）到 uint32_t。
   字段缺失保持默认值(0)；类型不对或解析失败返回 -1。
   base 传 16 时 strtoul 允许 "0x" 前缀，所以 JSON 里写不写前缀都能认。 */
static int get_hex_u32_field(const cJSON *obj, const char *key, uint32_t *dst)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    char *end = NULL;
    unsigned long v;

    if (item == NULL) return 0;
    if (!cJSON_IsString(item)) return -1;

    errno = 0;
    v = strtoul(item->valuestring, &end, 16);
    if (end == item->valuestring || *end != '\0') return -1;
    if (errno != 0 || v > 0xFFFFFFFFUL) return -1;

    *dst = (uint32_t)v;
    return 0;
}

int parse_tag(cJSON *tag_json, TagConfig *tag)
{
    memset(tag, 0, sizeof(*tag));

    if (get_str_field(tag_json, "address",    tag->address,    sizeof(tag->address))    != 0) return -1;
    if (get_str_field(tag_json, "raw_type",   tag->raw_type,   sizeof(tag->raw_type))   != 0) return -1;
    if (get_str_field(tag_json, "value_type", tag->value_type, sizeof(tag->value_type)) != 0) return -1;
    if (get_str_field(tag_json, "direction",  tag->direction,  sizeof(tag->direction))  != 0) return -1;
    if (get_int_field(tag_json, "offset", &tag->offset)                                 != 0) return -1;
    if (get_str_field(tag_json, "unit",       tag->unit,       sizeof(tag->unit))       != 0) return -1;
    if (get_double_field(tag_json, "scale", &tag->scale)                                != 0) return -1;
    if (get_str_field(tag_json, "name",       tag->name,       sizeof(tag->name))       != 0) return -1;

    return 0;
}

int parse_slave(cJSON *slave_json, SlaveConfig *slave)
{
    memset(slave, 0, sizeof(*slave));

    if (get_str_field(slave_json, "prefix",   slave->prefix,   sizeof(slave->prefix))   != 0) return -1;
    if (get_str_field(slave_json, "protocol", slave->protocol, sizeof(slave->protocol)) != 0) return -1;
    if (get_str_field(slave_json, "node",     slave->node,     sizeof(slave->node))     != 0) return -1;
    if (get_str_field(slave_json, "slave",    slave->slave,    sizeof(slave->slave))    != 0) return -1;
    if (get_hex_u32_field(slave_json, "vendor_id",    &slave->vendor_id)                != 0) return -1;
    if (get_hex_u32_field(slave_json, "product_code", &slave->product_code)             != 0) return -1;
    if (get_int_field(slave_json, "period_ms", &slave->period_ms)                       != 0) return -1;
    if (get_str_field(slave_json, "publish",  slave->publish,  sizeof(slave->publish))  != 0) return -1;

    // 解析 tags
    cJSON *tags = cJSON_GetObjectItem(slave_json, "tags");
    if (tags) {
        if (!cJSON_IsObject(tags)) return -1;

        cJSON *count = cJSON_GetObjectItem(tags, "count");
        if (count) {
            if (!cJSON_IsNumber(count)) return -1;
            slave->tag_count = count->valueint;
        }

        cJSON *tag_array = cJSON_GetObjectItem(tags, "tag");
        if (tag_array) {
            if (!cJSON_IsArray(tag_array)) return -1;
            int i = 0;
            cJSON *tag;
            cJSON_ArrayForEach(tag, tag_array) {
                if (i < MAX_TAGS) {
                    if (parse_tag(tag, &slave->tags[i]) != 0) return -1;
                    i++;
                }
            }
        }
    }

    return 0;
}

/* ---------- 把解析出的从站追加到 config ---------- */

/* 只收 protocol == "ethercat" 的从站；其它协议解析完直接丢弃，槽位留给下一个。
   （不先看 protocol 是为了不重复一遍字段提取逻辑——文件总得读出来才知道是什么协议。） */
static int config_store_add_slave(Config *config, cJSON *slave_json)
{
    SlaveConfig *dst;

    if (config->slave_count >= MAX_SLAVES) return -1;

    dst = &config->slaves[config->slave_count];
    if (parse_slave(slave_json, dst) != 0) return -1;

    if (strcmp(dst->protocol, "ethercat") != 0) return 0;   /* 丢弃，不提交 */

    config->slave_count++;
    return 0;
}

static int config_store_collect_from_root(cJSON *root, Config *config)
{
    if (cJSON_IsArray(root)) {
        cJSON *slave;
        cJSON_ArrayForEach(slave, root) {
            if (config_store_add_slave(config, slave) != 0) return -1;
        }
        return 0;
    }

    if (cJSON_IsObject(root)) {
        return config_store_add_slave(config, root);
    }

    return -1;
}

/* ---------- 读一个 json 文件并追加到 config ---------- */

static int config_store_parse_file(const char *filename, Config *config)
{
    FILE *fp;
    long size;
    char *buf;
    cJSON *root;
    int rc;

    /* 二进制模式：两个平台都是纯字节流，不受 CRLF 转换影响 */
    fp = fopen(filename, "rb");
    if (fp == NULL) {
        printf("%s: 打不开 (%s)\n", filename, strerror(errno));
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    if (size <= 0 || (unsigned long)size > CONFIG_JSON_FILE_MAX) {
        printf("%s: 文件大小为 %ld，跳过\n", filename, size);
        fclose(fp);
        return -1;
    }
    rewind(fp);

    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) {
        printf("内存不足（%ld 字节）\n", size);
        fclose(fp);
        return -1;
    }

    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        printf("%s: 读取不完整\n", filename);
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    buf[size] = '\0';

    /* 已知长度，省一次 strlen，也免疫内容里混入 NUL */
    root = cJSON_ParseWithLength(buf, (size_t)size);
    free(buf);
    if (root == NULL) {
        printf("%s: JSON 语法错误\n", filename);
        return -1;
    }

    rc = config_store_collect_from_root(root, config);
    cJSON_Delete(root);                 /* 单出口，中间不 return */
    return rc;
}

/* ---------- 目录遍历 ---------- */

static int is_json_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name) return 0;   /* 无后缀，或形如 ".json" 的隐藏文件 */
    return strcmp(dot, ".json") == 0;
}

/* MinGW 的 struct dirent 没有 d_type，也没有 DT_* 宏，所以用 stat 判断是不是普通文件，
   顺便挡掉名为 xxx.json 的子目录。 */
static int is_regular_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

/* 遍历 dirname 下所有 .json 文件，解析出的从站累加到 config。
   单个文件出错就跳过，继续处理其余文件。 */
int configuration_store_load_dir(const char *dirname, Config *config)
{
    DIR *dp;
    struct dirent *de;

    config->slave_count = 0;

    dp = opendir(dirname);
    if (dp == NULL) {
        printf("打不开目录 %s: %s\n", dirname, strerror(errno));
        return -1;
    }

    while ((de = readdir(dp)) != NULL) {
        char path[CONFIG_PATH_MAX];
        const char *name = de->d_name;

        /* MinGW 的 opendir 底层是 FindFirstFile，会返回 . 和 ..，POSIX 也一定会 */
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (!is_json_name(name)) continue;

        if (snprintf(path, sizeof(path), "%s/%s", dirname, name) >= (int)sizeof(path)) {
            printf("路径过长：%s/%s\n", dirname, name);
            continue;
        }
        if (!is_regular_file(path)) continue;

        config_store_parse_file(path, config);
    }

    closedir(dp);
    return 0;
}

/* 完成整个 JSON 配置结构体化：把配置目录下所有 .json 解析进 config。
   打印配置内容由 configuration_store_load_test 负责。 */
int configuration_store_load_startup(Config *config)
{
    return configuration_store_load_dir("JSON", config);
}


void configuration_store_load_test(void)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif  // _WIN32

    static Config config;               /* 约 1.34 MB，必须离开栈 */
    int i;

    if (configuration_store_load_startup(&config) != 0) {
        printf("配置加载失败\n");
        return;
    }

    printf("共 %d 个从站\n", config.slave_count);

    for (i = 0; i < config.slave_count; i++) {
        const SlaveConfig *s = &config.slaves[i];
        int j;

        printf("从站 %d: %s (%s)\n", i, s->node, s->protocol);
        printf("  周期: %d ms\n", s->period_ms);
        printf("  标签数: %d\n", s->tag_count);

        for (j = 0; j < s->tag_count; j++) {
            const TagConfig *t = &s->tags[j];
            printf("    [%d] %s: %s, 单位 %s, 缩放 %.2f\n",
                   j, t->name, t->address, t->unit, t->scale);
        }
    }
}
