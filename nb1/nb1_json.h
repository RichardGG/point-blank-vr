#ifndef NB1_JSON_H
#define NB1_JSON_H
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct Json { JType t; double num; char *str; int n; struct Json **kid; char **key; } Json;
Json *json_parse(const char *text);
void json_free(Json *j);
const Json *json_get(const Json *o, const char *key);
#endif
