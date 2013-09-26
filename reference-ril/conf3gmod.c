// author:	tary
// date:	11:58 2010-11-27

#define LOG_TAG "RILCONF3G"

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <utils/Log.h>
#include <cutils/properties.h>
#include "conf3gmod.h"

#ifndef MAX_PATH
#define	MAX_PATH		(260)
#endif

#define CONF_3G_KEYWORD		"3gsupport"
#define MAX_LINE_LEN		(1024)
#define CONF_FILES_COUNT	(sizeof conf_files / sizeof conf_files[0])
#define PPP_CONF_FILES_COUNT	(sizeof ppp_conf_files / sizeof ppp_conf_files[0])

const char* conf_files[] = {
	"/init.devkit8600.rc",
	"/system/build.prop",
};


const char* ppp_conf_files[] = {
	// "/etc/ppp/gprs-connect-chat",
	"/etc/ppp/3g-connect-chat",
	"/etc/ppp/evdo-connect-chat",
	// "/etc/ppp/peers/gprs",
	"/etc/ppp/peers/3g",
	"/etc/ppp/peers/evdo",
	"/etc/ppp/peers/w388",
};
static const char* mod_name = NULL;
static char conf_line_val[MAX_LINE_LEN] = "";


static int escape_copy(char* tar, const char* src) {
	const char* s;

	s = src;
	while (*s != ']' && *s != '\0') {
		if (*s == '\\') {
			s++;
		}
		*tar++ = *s++;
	}
	*tar = '\0';
	return (s - src); 
}

static int conf_normal_line(char* tar, const char* src) {
	if (strlen(conf_line_val) == 0) {
		return -1;
	}
	strcpy(tar, conf_line_val);
	strcat(tar, "\n");
	strcpy(conf_line_val, "");
	LOGD("~~~t~~~ replace complete for\n");
	LOGD("~~~t~~~ \torig = %s", src);
	LOGD("~~~t~~~ \tnew  = %s", tar);
	return 0;
}

static const char* env_get_var_val(const char** env, const char* name) {
	int i;
	int len;

	if (env == NULL) {
		return NULL;
	}

	len = strlen(name);
	for (i = 0; env[i] != NULL; i++) {
		if ((strncmp(env[i], name, len) == 0)
		&& (env[i][len] == '=')
		) {
			return &env[i][len + 1];
		}
	}

	return NULL;
}
/* example lines
#[3gsupport,HSPA-500HU,user $username$]
#[3gsupport,ETCOM-W338,user $username$]
*/
static int environ_copy(char* tar, const char* src, const char** env) {
	char line[MAX_LINE_LEN];
	char* s;
	const char* var_name = NULL;
	const char* var_val = NULL;

	strcpy(s = line, src);
	while (*s != ']' && *s != '\0') {
		if (*s == '$') {
			var_name = ++s;
			s = strchr(s, '$');
			*s++ = '\0';
			var_val = env_get_var_val(env, var_name);
		}
		if (var_val != NULL) {
			while (*var_val != '\0') {
				*tar++ = *var_val++;
			}
			var_val = NULL;
			continue;
		}
		if (*s == '\\') {
			s++;
		}
		*tar++ = *s++;
	}
	*tar = '\0';
	return (s - src); 
}


/* example lines
#[3gsupport,HSPA-500HU,    setprop ro.radio.3g.name HSPA-500HU]
#[3gsupport,ETCOM-W338,    setprop ro.radio.3g.name ETCOM-W338]
#[3gsupport,ETCOM-E300,    setprop ro.radio.3g.name ETCOM-E300]
#[3gsupport,ETCOM-W388,    setprop ro.radio.3g.name ETCOM-W388]
*/
static int conf_line_scan(char* tar, const char* src, const char** env) {
	char line[MAX_LINE_LEN];
	char* s;
	const char* fld;

	strcpy(s = line, src);
	strcpy(tar, src);

	while (isspace(*s)) s++;

	if (*s != '#') {
		conf_normal_line(tar, src);
		return 0;
	}

	do { ++s; } while (isspace(*s));

	if (*s != '[') {
		return 0;
	} else if (strchr(s, ']') == NULL) {
		return 0;
	}

	do { ++s; } while (isspace(*s));

	fld = strtok(s, ",");
	// LOGD("key = %s\n", fld);
	if (strcmp(fld, CONF_3G_KEYWORD) != 0) {
		return 0;
	}
	// LOGD("%s", src);

	fld = strtok(NULL, ",");
	if (strcmp(fld, mod_name) != 0) {
		return 0;
	}

	// strtok had set *(fld + strlen(fld)) = '\0'
	
	// so skip it
	fld = fld + strlen(fld) + 1;
	if (env == NULL) {
		escape_copy(conf_line_val, fld);
	} else {
		environ_copy(conf_line_val, fld, env);
	}
	LOGD("~~~t~~~ target line hit \n%s", src);

	return 1;
}


static int conf_file_scan(const char* filename, const char** env) {
	int fd;
	FILE *srcf;
	FILE *tmpf;
	char tmpf_name[MAX_PATH];
	char line[MAX_LINE_LEN];
	char newline[MAX_LINE_LEN];

	if ((srcf = fopen(filename, "rb+")) == NULL) {
		LOGE("open file %s error\n", filename);
		return -1;
	}

	sprintf(tmpf_name, "%s.~XXXXXX", filename);
	if ((fd = mkstemp(tmpf_name)) == -1) {
		LOGE("open temporary file error\n");
		fclose(srcf);
		return -1;
	}

	if ((tmpf = fdopen(fd, "wb+")) == NULL) {
		LOGE("fdopen error\n");
		fclose(srcf);
		return -1;
	}

	LOGD("temp file name = %s\n", tmpf_name);

	while (fgets(line, MAX_LINE_LEN, srcf) != NULL) {
		// LOGD("%s", line);
		conf_line_scan(newline, line, env);
		fputs(newline, tmpf);
	}

	fclose(srcf);
	fclose(tmpf);
	if (unlink(filename) != 0) {
		LOGE("unlink file %s error\n", filename);
		return -1;
	}
	rename(tmpf_name, filename);
	return 0;
}

int ppp_options_conf(const char* module, const char** env) {
	int i;
	int r;

	if (module != NULL) {
		mod_name = module;
	}

	LOGD("Module_Name = %s\n", mod_name);

	for (i = 0; i < (int)PPP_CONF_FILES_COUNT; i++) {
		r = conf_file_scan(ppp_conf_files[i], env);
		if (r < 0) {
			break;
		}
	}

	sync();

	return r;
}
#if defined(CONF3GMOD_SHLIB)

int configure_3g_module(const char* module) {
	int i;
	int r;

	if (module != NULL) {
		mod_name = module;
	}

	LOGD("Module_Name = %s\n", mod_name);

	for (i = 0; i < (int)CONF_FILES_COUNT; i++) {
		r = conf_file_scan(conf_files[i], NULL);
		if (r < 0) {
			break;
		}
	}

	sync();

	return r;
}

const char* get_current_3g_module(void) {
	static char mod_name[PROPERTY_VALUE_MAX];

	if (0 == property_get("ro.radio.3g.name", mod_name, "")) {
		return mod_name;
	}

	return mod_name;
}

#else

int main(int argc, char* argv[]) {
	int i;
	const char* env[] = {
		NULL,
		NULL,
		NULL,
		NULL,
	};

	mod_name = "ETCOM-W338";
	if (argc >= 2) {
		mod_name = argv[1];
	}

#if 1
	LOGD("Module_Name = %s\n", mod_name);

	for (i = 0; i < CONF_FILES_COUNT; i++) {
		conf_file_scan(conf_files[i], NULL);
	}

	sync();
#else

	env[0] = "username=\"card\"";
	env[1] = "password=\"card\"";
	env[2] = "apn=\"cmnet\"";
	env[3] = NULL;
	ppp_options_conf(NULL, env);
#endif
	return 0;
}

#endif
