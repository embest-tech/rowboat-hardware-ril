// author:	tary
// date:	10:20 2010-12-01

#ifndef __CONF3GMOD_H__
#define __CONF3GMOD_H__

#ifdef __cplusplus
extern "C" {
#endif

int configure_3g_module(const char* mod_name);
const char* get_current_3g_module(void);

int ppp_options_conf(const char* module, const char** env);
#ifdef __cplusplus
}
#endif

#endif //__CONF3GMOD_H__
