/* Registry of implemented Win16 API functions. Anything not listed resolves to a slot
 * that reports "unimplemented API" with its name and caller when called. Each module's
 * file exports its own table; they're searched in order. */
#include "rt.h"

extern const ApiDef g_kernel_api[];
extern const ApiDef g_kernel_mem_api[];
extern const ApiDef g_kernel_module_api[];
extern const ApiDef g_kernel_file_api[];
extern const ApiDef g_kernel_res_api[];
extern const ApiDef g_user_api[];
extern const ApiDef g_user_msg_api[];
extern const ApiDef g_user_wnd_api[];
extern const ApiDef g_user_misc_api[];
extern const ApiDef g_user_ctl_api[];
extern const ApiDef g_gdi_api[];
extern const ApiDef g_gdi_draw_api[];
extern const ApiDef g_gdi_pal_api[];
extern const ApiDef g_gdi_text_api[];
extern const ApiDef g_mmsystem_api[];
extern const ApiDef g_commdlg_api[];
extern const ApiDef g_wave_api[];

const ApiDef *const g_api_tables[] = {
    g_kernel_api,
    g_kernel_mem_api,
    g_kernel_module_api,
    g_kernel_file_api,
    g_kernel_res_api,
    g_user_api,
    g_user_msg_api,
    g_user_wnd_api,
    g_user_misc_api,
    g_user_ctl_api,
    g_gdi_api,
    g_gdi_draw_api,
    g_gdi_pal_api,
    g_gdi_text_api,
    g_mmsystem_api,
    g_commdlg_api,
    g_wave_api,
    NULL
};
