// Minimal JSON reader for the shader reflection files (totk_xlate --target spirv). One allocation per document.
#pragma once
#include <stddef.h>
typedef enum { JS_NULL, JS_FALSE, JS_TRUE, JS_NUM, JS_STR, JS_ARR, JS_OBJ } js_type;
typedef struct js js;
struct js { js_type t; double num; const char *str; const char *key; js *child, *next; int n; };
js *js_parse(const char *text, size_t len);   // NULL on syntax error; free with js_free
void js_free(js *root);
js *js_get(const js *obj, const char *key);   // member of an object, NULL if absent
js *js_at(const js *arr, int i);
int js_int(const js *v, int dflt);
const char *js_str(const js *v, const char *dflt);
int js_bool(const js *v, int dflt);
