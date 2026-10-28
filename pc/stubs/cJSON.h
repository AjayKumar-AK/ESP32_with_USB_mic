#pragma once
typedef struct cJSON { char *valuestring; double valuedouble; } cJSON;
cJSON *cJSON_Parse(const char *s);
void cJSON_Delete(cJSON *j);
cJSON *cJSON_GetObjectItem(const cJSON *j, const char *k);
int cJSON_IsString(const cJSON *j);
