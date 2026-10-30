/* USER API functions. */
#include "rt.h"

static void InitApp(CPU *c)            /* BOOL InitApp(HINSTANCE): creates the message queue */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = 1;
    api_ret(c, n);
}

const ApiDef g_user_api[] = {
    { "USER", 5, "InitApp", InitApp },
    { NULL, 0, NULL, NULL }
};
