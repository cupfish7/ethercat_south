#include "ethercat_south_parse.h"   /* 结构体和公开函数声明都在这里 */
#include "cJSON/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define CONFIG_PATH_MAX      1024
#define CONFIG_NAME_MAX      256
#define CONFIG_MAX_FILES     256
#define CONFIG_JSON_FILE_MAX (1024u * 1024u)   /* 单个配置文件的字节上限 */

/* ---------- 数据类型 ---------- */

const char *south_type_name(south_data_type_t type)
{
    switch (type) {
    case SOUTH_TYPE_BOOL: return "bool";
    case SOUTH_TYPE_I8:   return "i8";
    case SOUTH_TYPE_U8:   return "u8";
    case SOUTH_TYPE_I16:  return "i16";
    case SOUTH_TYPE_U16:  return "u16";
    case SOUTH_TYPE_I32:  return "i32";
    case SOUTH_TYPE_U32:  return "u32";
    case SOUTH_TYPE_I64:  return "i64";
    case SOUTH_TYPE_U64:  return "u64";
    case SOUTH_TYPE_F32:  return "f32";
    case SOUTH_TYPE_F64:  return "f64";
    default:              return "unknown";
    }
}

int south_type_bit_length(south_data_type_t type)
{
    switch (type) {
    case SOUTH_TYPE_BOOL:
    case SOUTH_TYPE_I8:
    case SOUTH_TYPE_U8:   return 8;
    case SOUTH_TYPE_I16:
    case SOUTH_TYPE_U16:  return 16;
    case SOUTH_TYPE_I32:
    case SOUTH_TYPE_U32:
    case SOUTH_TYPE_F32:  return 32;
    case SOUTH_TYPE_I64:
    case SOUTH_TYPE_U64:
    case SOUTH_TYPE_F64:  return 64;
    default:              return 0;
    }
}

int south_type_width_bytes(south_data_type_t type)
{
    int bits = south_type_bit_length(type);
    return bits > 0 ? bits / 8 : 0;
}

/* ---------- 字段层 ----------
   缺失：required=0 时保持默认值；required=1 时返回 -1。
   存在但类型错、超长、越界：一律返回 -1。 */

static int get_str_field(const cJSON *obj, const char *key, char *dst,
                         size_t dst_size, int required, const char *ctx)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    size_t len;

    if (item == NULL) {
        if (required)
            printf("%s: 缺少字段 \"%s\"\n", ctx, key);
        return required ? -1 : 0;
    }
    if (!cJSON_IsString(item)) {
        printf("%s: 字段 \"%s\" 必须是字符串\n", ctx, key);
        return -1;
    }

    len = strlen(item->valuestring);
    if (len >= dst_size) {
        printf("%s: 字段 \"%s\" 过长（%lu 字节，上限 %lu）\n",
               ctx, key, (unsigned long)len, (unsigned long)(dst_size - 1));
        return -1;
    }

    memcpy(dst, item->valuestring, len + 1);
    return 0;
}

static int get_i32_field(const cJSON *obj, const char *key, int32_t *dst,
                         int required, const char *ctx)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    double d;

    if (item == NULL) {
        if (required)
            printf("%s: 缺少字段 \"%s\"\n", ctx, key);
        return required ? -1 : 0;
    }
    if (!cJSON_IsNumber(item)) {
        printf("%s: 字段 \"%s\" 必须是数字\n", ctx, key);
        return -1;
    }

    d = item->valuedouble;
    if (d < (double)INT32_MIN || d > (double)INT32_MAX ||
        d != (double)(int32_t)d) {
        printf("%s: 字段 \"%s\" 不是有效的 32 位整数\n", ctx, key);
        return -1;
    }

    *dst = (int32_t)d;
    return 0;
}

static int get_u32_field(const cJSON *obj, const char *key, uint32_t *dst,
                         int required, const char *ctx)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    double d;

    if (item == NULL) {
        if (required)
            printf("%s: 缺少字段 \"%s\"\n", ctx, key);
        return required ? -1 : 0;
    }
    if (!cJSON_IsNumber(item)) {
        printf("%s: 字段 \"%s\" 必须是数字\n", ctx, key);
        return -1;
    }

    d = item->valuedouble;
    if (d < 0.0 || d > 4294967295.0 || d != (double)(uint32_t)d) {
        printf("%s: 字段 \"%s\" 不是有效的 32 位无符号整数\n", ctx, key);
        return -1;
    }

    *dst = (uint32_t)d;
    return 0;
}

static int get_double_field(const cJSON *obj, const char *key, double *dst,
                            int required, const char *ctx)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);

    if (item == NULL) {
        if (required)
            printf("%s: 缺少字段 \"%s\"\n", ctx, key);
        return required ? -1 : 0;
    }
    if (!cJSON_IsNumber(item)) {
        printf("%s: 字段 \"%s\" 必须是数字\n", ctx, key);
        return -1;
    }

    *dst = item->valuedouble;
    return 0;
}

static int get_bool_field(const cJSON *obj, const char *key, int *dst,
                          int required, const char *ctx)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);

    if (item == NULL) {
        if (required)
            printf("%s: 缺少字段 \"%s\"\n", ctx, key);
        return required ? -1 : 0;
    }
    if (!cJSON_IsBool(item)) {
        printf("%s: 字段 \"%s\" 必须是 true/false\n", ctx, key);
        return -1;
    }

    *dst = cJSON_IsTrue(item) ? 1 : 0;
    return 0;
}

/* 十六进制字符串（"0x00004321"）→ uint32_t。
   strtoul 用 base 16 时允许 "0x" 前缀，所以写不写都能认。 */
static int get_hex_u32_field(const cJSON *obj, const char *key, uint32_t *dst,
                             int required, const char *ctx)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    char *end = NULL;
    unsigned long v;

    if (item == NULL) {
        if (required)
            printf("%s: 缺少字段 \"%s\"\n", ctx, key);
        return required ? -1 : 0;
    }
    if (!cJSON_IsString(item)) {
        printf("%s: 字段 \"%s\" 必须是十六进制字符串\n", ctx, key);
        return -1;
    }

    errno = 0;
    v = strtoul(item->valuestring, &end, 16);
    if (end == item->valuestring || *end != '\0' ||
        errno != 0 || v > 0xFFFFFFFFUL) {
        printf("%s: 字段 \"%s\" 不是合法的 32 位十六进制数\n", ctx, key);
        return -1;
    }

    *dst = (uint32_t)v;
    return 0;
}

/* Alias_Address / Position：接受 JSON 字符串或数字，范围 0..65535。
   字符串以 "0x" 开头按十六进制，否则按十进制。 */
static int get_alias_field(const cJSON *obj, const char *key, uint16_t *dst,
                           int required, const char *ctx)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    unsigned long v = 0;
    int base;

    if (item == NULL) {
        if (required)
            printf("%s: 缺少字段 \"%s\"\n", ctx, key);
        return required ? -1 : 0;
    }

    if (cJSON_IsString(item)) {
        char *end = NULL;
        const char *s = item->valuestring;

        if (*s == '\0') {
            printf("%s: 字段 \"%s\" 不能为空\n", ctx, key);
            return -1;
        }

        base = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
        errno = 0;
        v = strtoul(s, &end, base);
        if (end == s || *end != '\0' || errno != 0 || v > 0xFFFFUL) {
            printf("%s: 字段 \"%s\" 不是合法的 16 位地址\n", ctx, key);
            return -1;
        }
    } else if (cJSON_IsNumber(item)) {
        double d = item->valuedouble;
        if (d < 0.0 || d > 65535.0 || d != (double)(uint16_t)d) {
            printf("%s: 字段 \"%s\" 不是合法的 16 位地址\n", ctx, key);
            return -1;
        }
        v = (unsigned long)d;
    } else {
        printf("%s: 字段 \"%s\" 必须是字符串或数字\n", ctx, key);
        return -1;
    }

    *dst = (uint16_t)v;
    return 0;
}

/* assign_activate：十六进制字符串或数字，0..65535。 */
static int get_hex_or_u16_field(const cJSON *obj, const char *key, uint16_t *dst,
                                int required, const char *ctx)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    unsigned long v;

    if (item == NULL) {
        if (required)
            printf("%s: 缺少字段 \"%s\"\n", ctx, key);
        return required ? -1 : 0;
    }

    if (cJSON_IsString(item)) {
        char *end = NULL;
        errno = 0;
        v = strtoul(item->valuestring, &end, 16);
        if (end == item->valuestring || *end != '\0' ||
            errno != 0 || v > 0xFFFFUL) {
            printf("%s: 字段 \"%s\" 不是合法的 16 位十六进制数\n", ctx, key);
            return -1;
        }
    } else if (cJSON_IsNumber(item)) {
        double d = item->valuedouble;
        if (d < 0.0 || d > 65535.0 || d != (double)(uint16_t)d) {
            printf("%s: 字段 \"%s\" 不是合法的 16 位整数\n", ctx, key);
            return -1;
        }
        v = (unsigned long)d;
    } else {
        printf("%s: 字段 \"%s\" 必须是十六进制字符串或数字\n", ctx, key);
        return -1;
    }

    *dst = (uint16_t)v;
    return 0;
}

/* ---------- 枚举 / 地址 ---------- */

static int parse_raw_type(const char *s, south_data_type_t *out)
{
    if (strcmp(s, "bool") == 0)  { *out = SOUTH_TYPE_BOOL; return 0; }
    if (strcmp(s, "i8") == 0)    { *out = SOUTH_TYPE_I8;   return 0; }
    if (strcmp(s, "u8") == 0)    { *out = SOUTH_TYPE_U8;   return 0; }
    if (strcmp(s, "i16") == 0)   { *out = SOUTH_TYPE_I16;  return 0; }
    if (strcmp(s, "u16") == 0)   { *out = SOUTH_TYPE_U16;  return 0; }
    if (strcmp(s, "i32") == 0)   { *out = SOUTH_TYPE_I32;  return 0; }
    if (strcmp(s, "u32") == 0)   { *out = SOUTH_TYPE_U32;  return 0; }
    if (strcmp(s, "i64") == 0)   { *out = SOUTH_TYPE_I64;  return 0; }
    if (strcmp(s, "u64") == 0)   { *out = SOUTH_TYPE_U64;  return 0; }
    if (strcmp(s, "real") == 0)  { *out = SOUTH_TYPE_F32;  return 0; }
    if (strcmp(s, "lreal") == 0) { *out = SOUTH_TYPE_F64;  return 0; }
    return -1;
}

static int parse_value_type(const char *s, south_data_type_t *out)
{
    if (strcmp(s, "i8") == 0)  { *out = SOUTH_TYPE_I8;  return 0; }
    if (strcmp(s, "u8") == 0)  { *out = SOUTH_TYPE_U8;  return 0; }
    if (strcmp(s, "i16") == 0) { *out = SOUTH_TYPE_I16; return 0; }
    if (strcmp(s, "u16") == 0) { *out = SOUTH_TYPE_U16; return 0; }
    if (strcmp(s, "i32") == 0) { *out = SOUTH_TYPE_I32; return 0; }
    if (strcmp(s, "u32") == 0) { *out = SOUTH_TYPE_U32; return 0; }
    if (strcmp(s, "i64") == 0) { *out = SOUTH_TYPE_I64; return 0; }
    if (strcmp(s, "u64") == 0) { *out = SOUTH_TYPE_U64; return 0; }
    if (strcmp(s, "f32") == 0) { *out = SOUTH_TYPE_F32; return 0; }
    if (strcmp(s, "f64") == 0) { *out = SOUTH_TYPE_F64; return 0; }
    return -1;
}

static int parse_direction(const char *s, south_direction_t *out)
{
    if (strcmp(s, "in") == 0)  { *out = SOUTH_DIR_IN;  return 0; }
    if (strcmp(s, "out") == 0) { *out = SOUTH_DIR_OUT; return 0; }
    return -1;
}

static int parse_mode(const char *s, south_sync_mode_t *out)
{
    if (strcmp(s, "freerun") == 0) { *out = SOUTH_MODE_FREERUN; return 0; }
    if (strcmp(s, "sm") == 0)      { *out = SOUTH_MODE_SM;      return 0; }
    if (strcmp(s, "dc") == 0)      { *out = SOUTH_MODE_DC;      return 0; }
    return -1;
}

/* "0x6064:00" → index / subindex。
   两段都按十六进制解析：0x 前缀可有可无，子索引 "08" 也按十六进制，
   不会像 base 0 那样被当成非法八进制。 */
static int parse_pdo_addr(const char *s, uint16_t *index, uint8_t *subindex)
{
    char *end = NULL;
    unsigned long idx, sub;

    if (s == NULL || *s == '\0') return -1;

    idx = strtoul(s, &end, 16);
    if (end == s || *end != ':') return -1;

    s = end + 1;
    sub = strtoul(s, &end, 16);
    if (end == s || *end != '\0') return -1;
    if (idx > 0xFFFFUL || sub > 0xFFUL) return -1;

    *index = (uint16_t)idx;
    *subindex = (uint8_t)sub;
    return 0;
}

/* 十六进制字符串 → 原始字节。允许可选的 "0x" 前缀。 */
static int decode_hex_bytes(const char *s, uint8_t *dst, size_t cap, size_t *len)
{
    size_t n, i;

    if (s == NULL) return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;

    n = strlen(s);
    if (n == 0 || (n % 2) != 0 || (n / 2) > cap) return -1;

    for (i = 0; i < n / 2; i++) {
        unsigned int hi, lo;
        char a = s[2 * i];
        char b = s[2 * i + 1];

        if (a >= '0' && a <= '9') hi = (unsigned int)(a - '0');
        else if (a >= 'a' && a <= 'f') hi = (unsigned int)(a - 'a' + 10);
        else if (a >= 'A' && a <= 'F') hi = (unsigned int)(a - 'A' + 10);
        else return -1;

        if (b >= '0' && b <= '9') lo = (unsigned int)(b - '0');
        else if (b >= 'a' && b <= 'f') lo = (unsigned int)(b - 'a' + 10);
        else if (b >= 'A' && b <= 'F') lo = (unsigned int)(b - 'A' + 10);
        else return -1;

        dst[i] = (uint8_t)((hi << 4) | lo);
    }

    *len = n / 2;
    return 0;
}

static const char *peek_protocol(const cJSON *obj)
{
    const cJSON *p = cJSON_GetObjectItem(obj, "protocol");
    if (p == NULL || !cJSON_IsString(p)) return NULL;
    return p->valuestring;
}

/* ---------- tag ---------- */

int parse_tag(cJSON *tag_json, TagConfig *tag)
{
    char raw[16];
    char val[16];
    char dir[8];

    memset(tag, 0, sizeof(*tag));
    tag->scale = 1.0;
    strcpy(tag->unit, "none");

    if (!cJSON_IsObject(tag_json)) {
        printf("tag: 不是 JSON 对象\n");
        return -1;
    }

    if (get_str_field(tag_json, "address", tag->address,
                      sizeof(tag->address), 1, "tag") != 0) return -1;
    if (parse_pdo_addr(tag->address, &tag->index, &tag->subindex) != 0) {
        printf("tag: address \"%s\" 不是 index:subindex 形式\n", tag->address);
        return -1;
    }

    if (get_str_field(tag_json, "raw_type", raw, sizeof(raw), 1, "tag") != 0)
        return -1;
    if (parse_raw_type(raw, &tag->raw_type) != 0) {
        printf("tag: raw_type \"%s\" 不认识\n", raw);
        return -1;
    }

    if (get_str_field(tag_json, "value_type", val, sizeof(val), 1, "tag") != 0)
        return -1;
    if (parse_value_type(val, &tag->value_type) != 0) {
        printf("tag: value_type \"%s\" 不认识\n", val);
        return -1;
    }

    if (get_str_field(tag_json, "direction", dir, sizeof(dir), 1, "tag") != 0)
        return -1;
    if (parse_direction(dir, &tag->direction) != 0) {
        printf("tag: direction \"%s\" 只能是 in/out\n", dir);
        return -1;
    }

    /* 旧字段迁移提示：offset 已改名为 json_offset。 */
    if (cJSON_GetObjectItem(tag_json, "offset") != NULL) {
        printf("tag: 字段 \"offset\" 已改名为 \"json_offset\"\n");
        return -1;
    }
    if (get_i32_field(tag_json, "json_offset", &tag->json_offset, 0, "tag") != 0)
        return -1;

    if (get_str_field(tag_json, "unit", tag->unit,
                      sizeof(tag->unit), 0, "tag") != 0) return -1;
    if (get_double_field(tag_json, "scale", &tag->scale, 0, "tag") != 0)
        return -1;
    if (tag->scale != tag->scale) {          /* NaN */
        printf("tag: scale 不是有效数字\n");
        return -1;
    }

    if (get_str_field(tag_json, "name", tag->name,
                      sizeof(tag->name), 1, "tag") != 0) return -1;

    return 0;
}

/* ---------- dc ---------- */

static int parse_dc(cJSON *slave_json, SlaveConfig *slave)
{
    cJSON *dc = cJSON_GetObjectItem(slave_json, "dc");
    const char *ctx = slave->node[0] ? slave->node : "slave";

    if (dc == NULL) {
        printf("%s: mode=dc 但缺少 dc 对象\n", ctx);
        return -1;
    }
    if (!cJSON_IsObject(dc)) {
        printf("%s: dc 必须是对象\n", ctx);
        return -1;
    }

    if (get_hex_or_u16_field(dc, "assign_activate",
                             &slave->dc.assign_activate, 1, ctx) != 0)
        return -1;
    if (slave->dc.assign_activate == 0) {
        printf("%s: mode=dc 时 dc.assign_activate 不能为 0\n", ctx);
        return -1;
    }

    if (get_u32_field(dc, "sync0_cycle_ns",
                      &slave->dc.sync0_cycle_ns, 1, ctx) != 0)
        return -1;
    if (slave->dc.sync0_cycle_ns == 0) {
        printf("%s: dc.sync0_cycle_ns 必须大于 0\n", ctx);
        return -1;
    }

    if (get_i32_field(dc, "sync0_shift_ns",
                      &slave->dc.sync0_shift_ns, 0, ctx) != 0)
        return -1;
    if (get_u32_field(dc, "sync1_cycle_ns",
                      &slave->dc.sync1_cycle_ns, 0, ctx) != 0)
        return -1;
    if (get_i32_field(dc, "sync1_shift_ns",
                      &slave->dc.sync1_shift_ns, 0, ctx) != 0)
        return -1;

    return 0;
}

/* ---------- start_sdo ---------- */

static int sdo_index_is_pdo_area(uint16_t index)
{
    return (index >= 0x1600 && index <= 0x17FF) ||
           (index >= 0x1A00 && index <= 0x1BFF) ||
           (index >= 0x1C10 && index <= 0x1C2F);
}

static int parse_start_sdo(cJSON *slave_json, SlaveConfig *slave)
{
    cJSON *root = cJSON_GetObjectItem(slave_json, "start_sdo");
    cJSON *arr;
    cJSON *item;
    cJSON *count;
    const char *ctx = slave->node[0] ? slave->node : "slave";
    int declared = -1;
    int n = 0;
    int i;

    if (root == NULL) return 0;
    if (!cJSON_IsObject(root)) {
        printf("%s: start_sdo 必须是对象\n", ctx);
        return -1;
    }

    count = cJSON_GetObjectItem(root, "count");
    if (count != NULL) {
        if (get_i32_field(root, "count", &declared, 1, ctx) != 0) return -1;
        if (declared < 0 || declared > MAX_START_SDO) {
            printf("%s: start_sdo.count 超出 0..%d\n", ctx, MAX_START_SDO);
            return -1;
        }
    }

    arr = cJSON_GetObjectItem(root, "sdo");
    if (arr != NULL) {
        if (!cJSON_IsArray(arr)) {
            printf("%s: start_sdo.sdo 必须是数组\n", ctx);
            return -1;
        }

        cJSON_ArrayForEach(item, arr) {
            StartSdoConfig *sd;
            char type_name[16];
            char hex[2 * MAX_SDO_DATA + 4];
            size_t len = 0;
            int width;

            if (n >= MAX_START_SDO) {
                printf("%s: start_sdo 条目超过 %d 条\n", ctx, MAX_START_SDO);
                return -1;
            }
            if (!cJSON_IsObject(item)) {
                printf("%s: start_sdo.sdo[%d] 不是对象\n", ctx, n);
                return -1;
            }
            if (cJSON_GetObjectItem(item, "transition") != NULL) {
                printf("%s: start_sdo 不再支持 transition（IgH 统一在 PREOP 下发）\n",
                       ctx);
                return -1;
            }

            sd = &slave->sdo[n];

            if (get_str_field(item, "address", sd->address,
                              sizeof(sd->address), 1, ctx) != 0) return -1;
            if (parse_pdo_addr(sd->address, &sd->index, &sd->subindex) != 0) {
                printf("%s: start_sdo address \"%s\" 非法\n", ctx, sd->address);
                return -1;
            }
            if (sdo_index_is_pdo_area(sd->index)) {
                printf("%s: start_sdo 0x%04X 属于 PDO 分配/映射区，请用 PDO 配置接口\n",
                       ctx, sd->index);
                return -1;
            }

            if (get_str_field(item, "data_type", type_name,
                              sizeof(type_name), 1, ctx) != 0) return -1;
            if (parse_raw_type(type_name, &sd->data_type) != 0) {
                printf("%s: start_sdo.data_type \"%s\" 不认识\n",
                       ctx, type_name);
                return -1;
            }
            width = south_type_width_bytes(sd->data_type);
            if (width <= 0) {
                printf("%s: start_sdo.data_type 宽度非法\n", ctx);
                return -1;
            }

            if (get_str_field(item, "data_hex", hex, sizeof(hex), 1, ctx) != 0)
                return -1;
            if (decode_hex_bytes(hex, sd->data, MAX_SDO_DATA, &len) != 0) {
                printf("%s: start_sdo.data_hex \"%s\" 非法（需要偶数个十六进制字符）\n",
                       ctx, hex);
                return -1;
            }
            sd->data_len = (uint8_t)len;
            if ((int)len != width) {
                printf("%s: start_sdo.data_hex 长度 %lu 与 data_type %s 的 %d 字节不符\n",
                       ctx, (unsigned long)len, south_type_name(sd->data_type),
                       width);
                return -1;
            }

            sd->apply = 1;
            if (get_bool_field(item, "apply", &sd->apply, 0, ctx) != 0)
                return -1;

            for (i = 0; i < n; i++) {
                if (slave->sdo[i].index == sd->index &&
                    slave->sdo[i].subindex == sd->subindex) {
                    printf("%s: start_sdo 重复条目 0x%04X:%02X\n",
                           ctx, sd->index, sd->subindex);
                    return -1;
                }
            }

            n++;
        }
    }

    if (declared >= 0 && declared != n) {
        printf("%s: start_sdo.count=%d 与实际 %d 条不符\n", ctx, declared, n);
        return -1;
    }

    slave->sdo_count = n;
    return 0;
}

/* ---------- tags ---------- */

static int parse_tags(cJSON *slave_json, SlaveConfig *slave)
{
    cJSON *root = cJSON_GetObjectItem(slave_json, "tags");
    cJSON *arr;
    cJSON *item;
    cJSON *count;
    const char *ctx = slave->node[0] ? slave->node : "slave";
    int declared = -1;
    int n = 0;
    int i;

    if (root == NULL) {
        printf("%s: 缺少 tags\n", ctx);
        return -1;
    }
    if (!cJSON_IsObject(root)) {
        printf("%s: tags 必须是对象\n", ctx);
        return -1;
    }

    count = cJSON_GetObjectItem(root, "count");
    if (count != NULL) {
        if (get_i32_field(root, "count", &declared, 1, ctx) != 0) return -1;
        if (declared < 0 || declared > MAX_TAGS) {
            printf("%s: tags.count 超出 0..%d\n", ctx, MAX_TAGS);
            return -1;
        }
    }

    arr = cJSON_GetObjectItem(root, "tag");
    if (arr == NULL) {
        printf("%s: 缺少 tags.tag 数组\n", ctx);
        return -1;
    }
    if (!cJSON_IsArray(arr)) {
        printf("%s: tags.tag 必须是数组\n", ctx);
        return -1;
    }

    cJSON_ArrayForEach(item, arr) {
        TagConfig *tag;

        if (n >= MAX_TAGS) {
            printf("%s: tag 数量超过 %d\n", ctx, MAX_TAGS);
            return -1;
        }

        tag = &slave->tags[n];
        if (parse_tag(item, tag) != 0) {
            printf("%s: 第 %d 个 tag 解析失败\n", ctx, n);
            return -1;
        }

        for (i = 0; i < n; i++) {
            if (slave->tags[i].index == tag->index &&
                slave->tags[i].subindex == tag->subindex) {
                printf("%s: 重复的 tag 地址 0x%04X:%02X\n",
                       ctx, tag->index, tag->subindex);
                return -1;
            }
        }

        n++;
    }

    if (n == 0) {
        printf("%s: 至少需要一个 tag\n", ctx);
        return -1;
    }
    if (declared >= 0 && declared != n) {
        printf("%s: tags.count=%d 与实际 %d 条不符\n", ctx, declared, n);
        return -1;
    }

    slave->tag_count = n;
    return 0;
}

/* ---------- slave ---------- */

int parse_slave(cJSON *slave_json, SlaveConfig *slave)
{
    const char *proto;
    char mode[16];
    const char *ctx;

    memset(slave, 0, sizeof(*slave));

    if (!cJSON_IsObject(slave_json)) {
        printf("slave: 不是 JSON 对象\n");
        return -1;
    }

    proto = peek_protocol(slave_json);
    if (proto == NULL) {
        printf("slave: 缺少字符串字段 protocol\n");
        return -1;
    }
    if (proto[0] == '\0') {
        printf("slave: protocol 不能为空\n");
        return -1;
    }
    if (strlen(proto) >= sizeof(slave->protocol)) {
        printf("slave: protocol 过长\n");
        return -1;
    }
    strcpy(slave->protocol, proto);

    if (get_str_field(slave_json, "prefix", slave->prefix,
                      sizeof(slave->prefix), 0, "slave") != 0) return -1;
    if (get_str_field(slave_json, "node", slave->node,
                      sizeof(slave->node), 0, "slave") != 0) return -1;

    /* 非 ethercat：只填公共字段，调用方丢弃。 */
    if (strcmp(proto, "ethercat") != 0) return 0;

    ctx = slave->node[0] ? slave->node : "slave";

    if (slave->node[0] == '\0') {
        printf("%s: 缺少 node\n", ctx);
        return -1;
    }

    /* 旧字段迁移提示：slave 已拆成 Alias_Address / Position。 */
    if (cJSON_GetObjectItem(slave_json, "slave") != NULL) {
        printf("%s: 字段 \"slave\" 已拆成 \"Alias_Address\" 和 \"Position\"\n", ctx);
        return -1;
    }

    if (get_alias_field(slave_json, "Alias_Address",
                        &slave->alias, 1, ctx) != 0) return -1;
    if (get_alias_field(slave_json, "Position",
                        &slave->position, 1, ctx) != 0) return -1;
    if (get_hex_u32_field(slave_json, "vendor_id",
                          &slave->vendor_id, 1, ctx) != 0) return -1;
    if (get_hex_u32_field(slave_json, "product_code",
                          &slave->product_code, 1, ctx) != 0) return -1;

    if (get_i32_field(slave_json, "period_ms",
                      &slave->period_ms, 1, ctx) != 0) return -1;
    if (slave->period_ms <= 0) {
        printf("%s: period_ms 必须大于 0\n", ctx);
        return -1;
    }

    if (get_str_field(slave_json, "mode", mode, sizeof(mode), 1, ctx) != 0)
        return -1;
    if (parse_mode(mode, &slave->mode) != 0) {
        printf("%s: mode \"%s\" 只能是 freerun/sm/dc\n", ctx, mode);
        return -1;
    }

    if (get_str_field(slave_json, "publish", slave->publish,
                      sizeof(slave->publish), 0, ctx) != 0) return -1;

    if (slave->mode == SOUTH_MODE_DC) {
        if (parse_dc(slave_json, slave) != 0) return -1;
    }

    if (parse_start_sdo(slave_json, slave) != 0) return -1;
    if (parse_tags(slave_json, slave) != 0) return -1;

    return 0;
}

/* ---------- 把解析出的从站追加到 config ---------- */

static int config_store_add_slave(Config *config, cJSON *slave_json)
{
    const char *proto;
    SlaveConfig *dst;
    int i;

    if (!cJSON_IsObject(slave_json)) {
        printf("从站项不是 JSON 对象\n");
        return -1;
    }

    proto = peek_protocol(slave_json);
    if (proto == NULL) {
        printf("从站缺少字符串字段 protocol\n");
        return -1;
    }
    if (proto[0] == '\0') {
        printf("从站 protocol 不能为空\n");
        return -1;
    }

    /* 非 ethercat：不提交，也不做严格校验。 */
    if (strcmp(proto, "ethercat") != 0) return 0;

    if (config->slave_count >= MAX_SLAVES) {
        printf("从站数量超过 MAX_SLAVES=%d\n", MAX_SLAVES);
        return -1;
    }

    dst = &config->slaves[config->slave_count];
    if (parse_slave(slave_json, dst) != 0) {
        memset(dst, 0, sizeof(*dst));
        return -1;
    }

    for (i = 0; i < config->slave_count; i++) {
        if (config->slaves[i].alias == dst->alias &&
            config->slaves[i].position == dst->position) {
            printf("重复的从站地址 alias=%u position=%u\n",
                   dst->alias, dst->position);
            memset(dst, 0, sizeof(*dst));
            return -1;
        }
    }

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

    printf("JSON 根节点必须是对象或数组\n");
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
        printf("%s: 文件大小为 %ld，非法\n", filename, size);
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
    cJSON_Delete(root);
    if (rc != 0) printf("%s: 配置内容非法\n", filename);
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

static int name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int configuration_store_load_dir(const char *dirname, Config *config)
{
    DIR *dp;
    struct dirent *de;
    static char names[CONFIG_MAX_FILES][CONFIG_NAME_MAX];
    int n = 0;
    int i;

    if (dirname == NULL || config == NULL) return -1;

    config->slave_count = 0;

    dp = opendir(dirname);
    if (dp == NULL) {
        printf("打不开目录 %s: %s\n", dirname, strerror(errno));
        return -1;
    }

    while ((de = readdir(dp)) != NULL) {
        const char *name = de->d_name;

        /* MinGW 的 opendir 底层是 FindFirstFile，会返回 . 和 ..，POSIX 也一定会 */
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (!is_json_name(name)) continue;

        if (n >= CONFIG_MAX_FILES) {
            printf("目录下 .json 文件超过 %d 个\n", CONFIG_MAX_FILES);
            closedir(dp);
            return -1;
        }
        if (strlen(name) >= CONFIG_NAME_MAX) {
            printf("文件名过长：%s\n", name);
            closedir(dp);
            return -1;
        }
        strcpy(names[n++], name);
    }
    closedir(dp);

    /* 排序后处理：Config.slaves[] 的顺序（也就是 frame.node_id）稳定可复现。 */
    qsort(names, (size_t)n, sizeof(names[0]), name_cmp);

    for (i = 0; i < n; i++) {
        char path[CONFIG_PATH_MAX];

        if (snprintf(path, sizeof(path), "%s/%s", dirname, names[i]) >=
            (int)sizeof(path)) {
            printf("路径过长：%s/%s\n", dirname, names[i]);
            return -1;
        }
        if (!is_regular_file(path)) continue;

        if (config_store_parse_file(path, config) != 0) return -1;
    }

    return 0;
}

int configuration_store_load_startup(Config *config)
{
    if (config == NULL) return -1;
    if (configuration_store_load_dir("JSON", config) != 0) return -1;

    if (config->slave_count == 0) {
        printf("配置里没有 ethercat 从站\n");
        return -1;
    }

    return 0;
}

/* ---------- 测试 / 演示 ---------- */

const char *south_mode_name(south_sync_mode_t mode)
{
    switch (mode) {
    case SOUTH_MODE_FREERUN: return "freerun";
    case SOUTH_MODE_SM:      return "sm";
    case SOUTH_MODE_DC:      return "dc";
    default:                 return "unknown";
    }
}

void configuration_store_load_test(void)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    static Config config;               /* 约 1.5 MB，必须离开栈 */
    int i;

    if (configuration_store_load_startup(&config) != 0) {
        printf("配置加载失败\n");
        return;
    }

    printf("共 %d 个从站\n", config.slave_count);

    for (i = 0; i < config.slave_count; i++) {
        const SlaveConfig *s = &config.slaves[i];
        int j;

        printf("从站 %d: %s (%s)  alias=%u position=%u  模式=%s  周期=%d ms\n",
               i, s->node, s->protocol, s->alias, s->position,
               south_mode_name(s->mode), s->period_ms);

        if (s->mode == SOUTH_MODE_DC) {
            printf("  DC: assign_activate=0x%04X sync0=%u ns shift0=%d ns"
                   " sync1=%u ns shift1=%d ns\n",
                   s->dc.assign_activate, s->dc.sync0_cycle_ns,
                   s->dc.sync0_shift_ns, s->dc.sync1_cycle_ns,
                   s->dc.sync1_shift_ns);
        }

        printf("  SDO: %d 条\n", s->sdo_count);
        for (j = 0; j < s->sdo_count; j++) {
            const StartSdoConfig *sd = &s->sdo[j];
            int k;
            printf("    [%d] %s type=%s apply=%d data=",
                   j, sd->address, south_type_name(sd->data_type), sd->apply);
            for (k = 0; k < sd->data_len; k++) printf("%02X", sd->data[k]);
            printf("\n");
        }

        printf("  标签数: %d\n", s->tag_count);
        for (j = 0; j < s->tag_count; j++) {
            const TagConfig *t = &s->tags[j];
            printf("    [%d] %s: %s  raw=%s value=%s dir=%s json_offset=%d"
                   " 单位 %s 缩放 %.2f\n",
                   j, t->name, t->address,
                   south_type_name(t->raw_type), south_type_name(t->value_type),
                   t->direction == SOUTH_DIR_OUT ? "out" : "in",
                   t->json_offset, t->unit, t->scale);
        }
    }
}
