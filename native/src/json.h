/* json.h - small JSON reader (objects, arrays, strings, numbers, bool, null). */
#ifndef SD_JSON_H
#define SD_JSON_H

#include <glib.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal {
    JType type;
    double num;
    gboolean b;
    char *str;
    GPtrArray *items;  /* J_ARR: JVal*; J_OBJ: JVal* values */
    GPtrArray *keys;   /* J_OBJ: char* */
} JVal;

JVal *json_parse(const char *text, GError **err);
void json_free(JVal *v);
const JVal *json_get(const JVal *obj, const char *key);
const char *json_str(const JVal *v, const char *def);
double json_num(const JVal *v, double def);
void json_append_string(GString *out, const char *s); /* quoted + escaped */

#endif
