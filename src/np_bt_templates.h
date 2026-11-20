#ifndef NP_BT_TEMPLATES_H
#define NP_BT_TEMPLATES_H

/* Built-in binary format templates (010 Editor style). */
typedef struct np_bt_builtin {
    const char *name;   /* short id, e.g. "png" */
    const char *label;  /* display name */
    const char *source; /* template script */
} np_bt_builtin;

extern const np_bt_builtin np_bt_builtins[];

#endif /* NP_BT_TEMPLATES_H */
