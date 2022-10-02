/* main.c -- Machine At War 3/Land Air Sea Warfare .so loader
 *
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2022 Rinnegatamante
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.	See the LICENSE file for details.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <vitashark.h>
#include <vitaGL.h>
#include <zlib.h>

#define AL_ALEXT_PROTOTYPES
#include <AL/alext.h>
#include <AL/efx.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_image.h>
#include <SLES/OpenSLES.h>

#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wchar.h>
#include <wctype.h>

#include <ALmixer.h>

#include <vorbis/vorbisfile.h>
#include <ogg/ogg.h>
#include <mpg123.h>

#include <math.h>
#include <math_neon.h>

#include <errno.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <curl/curl.h>

#include "main.h"
#include "config.h"
#include "dialog.h"
#include "so_util.h"
#include "sha1.h"

#ifdef DEBUG
#define dlog printf
#else
#define dlog
#endif

#define CONFIG_FILE_PATH "app0:settings.cfg"

SceTouchData old_touch = {0};
enum {
	TOUCH_DOWN,
	TOUCH_UP,
	TOUCH_HOLD
};

char DATA_PATH[256];

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

int enable_dlcs = 0;

void loadConfig(void) {
  char buffer[30];
  int value;

  FILE *config = fopen(CONFIG_FILE_PATH, "r");

  if (config) {
    while (EOF != fscanf(config, "%[^=]=%d\n", buffer, &value)) {
      if (strcmp("enable_dlcs", buffer) == 0) enable_dlcs = value;
    }
    fclose(config);
  }
}

static char fake_vm[0x1000];
static char fake_env[0x1000];

int file_exists(const char *path) {
	SceIoStat stat;
	return sceIoGetstat(path, &stat) >= 0;
}

int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;

unsigned int _pthread_stack_default_user = 1 * 1024 * 1024;

so_module isotope_mod;

void *__wrap_memcpy(void *dest, const void *src, size_t n) {
	return sceClibMemcpy(dest, src, n);
}

void *__wrap_memmove(void *dest, const void *src, size_t n) {
	return sceClibMemmove(dest, src, n);
}

void *__wrap_memset(void *s, int c, size_t n) {
	return sceClibMemset(s, c, n);
}

char *getcwd_hook(char *buf, size_t size) {
	strcpy(buf, DATA_PATH);
	return buf;
}

int debugPrintf(char *text, ...) {
#ifdef DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, text);
	vsprintf(string, text, list);
	va_end(list);

	SceUID fd = sceIoOpen("ux0:data/isotope_log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, string, strlen(string));
		sceIoClose(fd);
	}
#endif
	return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	dlog("[LOG] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_write(int prio, const char *tag, const char *fmt, ...) {
#ifdef DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	dlog("[LOGW] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list list) {
#ifdef DEBUG
	static char string[0x8000];

	vsprintf(string, fmt, list);
	va_end(list);

	dlog("[LOGV] %s: %s\n", tag, string);
#endif
	return 0;
}

int ret0(void) {
	return 0;
}

int ret1(void) {
	return 1;
}

int pthread_mutex_init_fake(pthread_mutex_t **uid, const pthread_mutexattr_t *mutexattr) {
	pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
	if (!m)
		return -1;

	const int recursive = (mutexattr && *(const int *)mutexattr == 1);
	*m = recursive ? PTHREAD_RECURSIVE_MUTEX_INITIALIZER : PTHREAD_MUTEX_INITIALIZER;

	int ret = pthread_mutex_init(m, mutexattr);
	if (ret < 0) {
		free(m);
		return -1;
	}

	*uid = m;

	return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
	if (uid && *uid && (uintptr_t)*uid > 0x8000) {
		pthread_mutex_destroy(*uid);
		free(*uid);
		*uid = NULL;
	}
	return 0;
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
	int ret = 0;
	if (!*uid) {
		ret = pthread_mutex_init_fake(uid, NULL);
	} else if ((uintptr_t)*uid == 0x4000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy(&attr);
	} else if ((uintptr_t)*uid == 0x8000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy(&attr);
	}
	if (ret < 0)
		return ret;
	return pthread_mutex_lock(*uid);
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
	int ret = 0;
	if (!*uid) {
		ret = pthread_mutex_init_fake(uid, NULL);
	} else if ((uintptr_t)*uid == 0x4000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy(&attr);
	} else if ((uintptr_t)*uid == 0x8000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy(&attr);
	}
	if (ret < 0)
		return ret;
	return pthread_mutex_trylock(*uid);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
	int ret = 0;
	if (!*uid) {
		ret = pthread_mutex_init_fake(uid, NULL);
	} else if ((uintptr_t)*uid == 0x4000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy(&attr);
	} else if ((uintptr_t)*uid == 0x8000) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
		ret = pthread_mutex_init_fake(uid, &attr);
		pthread_mutexattr_destroy(&attr);
	}
	if (ret < 0)
		return ret;
	return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
	pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
	if (!c)
		return -1;

	*c = PTHREAD_COND_INITIALIZER;

	int ret = pthread_cond_init(c, NULL);
	if (ret < 0) {
		free(c);
		return -1;
	}

	*cnd = c;

	return 0;
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	return pthread_cond_broadcast(*cnd);
}

int pthread_cond_signal_fake(pthread_cond_t **cnd) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	return pthread_cond_signal(*cnd);
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
	if (cnd && *cnd) {
		pthread_cond_destroy(*cnd);
		free(*cnd);
		*cnd = NULL;
	}
	return 0;
}

int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	return pthread_cond_wait(*cnd, *mtx);
}

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	return pthread_cond_timedwait(*cnd, *mtx, t);
}

int clock_gettime_hook(int clk_id, struct timespec *t) {
	struct timeval now;
	int rv = gettimeofday(&now, NULL);
	if (rv)
		return rv;
	t->tv_sec = now.tv_sec;
	t->tv_nsec = now.tv_usec * 1000;

	return 0;
}

int pthread_cond_timedwait_relative_np_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, struct timespec *ts) {
	if (!*cnd) {
		if (pthread_cond_init_fake(cnd, NULL) < 0)
			return -1;
	}
	
	if (ts != NULL) {
		struct timespec ct;
		clock_gettime_hook(0, &ct);
		ts->tv_sec += ct.tv_sec;
		ts->tv_nsec += ct.tv_nsec;
	}
	
	pthread_cond_timedwait(*cnd, *mtx, ts); // FIXME
	return 0;
}

int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
	return pthread_create(thread, NULL, entry, arg);
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
	if (!once_control || !init_routine)
		return -1;
	if (__sync_lock_test_and_set(once_control, 1) == 0)
		(*init_routine)();
	return 0;
}

int GetCurrentThreadId(void) {
	return sceKernelGetThreadId();
}

extern void *__aeabi_ldiv0;
extern void *__aeabi_ul2d;

int GetEnv(void *vm, void **env, int r2) {
	*env = fake_env;
	return 0;
}

int GetDPI() {
	return 200;
}

void *(*string_constructor)(void *this, const char *str);

void GetLocale(void *a1) {
	int lang = -1;
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
	switch (lang) {
	case SCE_SYSTEM_PARAM_LANG_JAPANESE:
		string_constructor(a1, "jp");
		break;
	case SCE_SYSTEM_PARAM_LANG_FRENCH:
		string_constructor(a1, "fr");
		break;
	case SCE_SYSTEM_PARAM_LANG_SPANISH:
		string_constructor(a1, "es");
		break;
	case SCE_SYSTEM_PARAM_LANG_GERMAN:
		string_constructor(a1, "de");
		break;
	case SCE_SYSTEM_PARAM_LANG_ITALIAN:
		string_constructor(a1, "it");
		break;
	case SCE_SYSTEM_PARAM_LANG_DUTCH:
		string_constructor(a1, "nl");
		break;
	case SCE_SYSTEM_PARAM_LANG_CHINESE_S:
	case SCE_SYSTEM_PARAM_LANG_CHINESE_T:
		string_constructor(a1, "cn");
		break;
	case SCE_SYSTEM_PARAM_LANG_KOREAN:
		string_constructor(a1, "kr");
		break;
	case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR:
		string_constructor(a1, "br");
		break;
	case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT:
		string_constructor(a1, "pt");
		break;
	case SCE_SYSTEM_PARAM_LANG_RUSSIAN:
		string_constructor(a1, "ru");
		break;	
	case SCE_SYSTEM_PARAM_LANG_FINNISH:
		string_constructor(a1, "fi");
		break;
	case SCE_SYSTEM_PARAM_LANG_SWEDISH:
		string_constructor(a1, "se");
		break;
	case SCE_SYSTEM_PARAM_LANG_DANISH:
		string_constructor(a1, "dk");
		break;
	case SCE_SYSTEM_PARAM_LANG_NORWEGIAN:
		string_constructor(a1, "no");
		break;
	case SCE_SYSTEM_PARAM_LANG_POLISH:
		string_constructor(a1, "pl");
		break;
	case SCE_SYSTEM_PARAM_LANG_TURKISH:
		string_constructor(a1, "tr");
		break;
	default:
		string_constructor(a1, "en");
		break;
	}
}

void GetVersionOS(void *a1) {
	string_constructor(a1, "8.0");
}

void (*SendTouchEvent)(void *this, int type, int x, int y, GLsizei w) = NULL;
void (*SaveSettings)(void *this, int type);
uint32_t *g_pApp;

so_hook serialize_hook;
void *serialize_ptr = NULL;
void SerializeSettings(void *this, int type) {
	if (!serialize_ptr)
		serialize_ptr = this;
	SO_CONTINUE(int, serialize_hook, this, type);
}

pthread_t saver_thread;
void *settings_saver(void *argv) {
	for (;;) {
		if (serialize_ptr) {
			//printf("saving settings\n");
			SaveSettings(serialize_ptr, 0);
		}
		sceKernelDelayThread(3 * 1000 * 1000);
	}
}

so_hook menuchanged_hook;
int OnMenuChanged(void *this, void *unk, void *unk2, void *unk3, void *unk4, void *unk5, int *unk6) {
	if (unk6[20]) {
		unk6[20] = 0;
	}
	return SO_CONTINUE(int, menuchanged_hook, this, unk, unk2, unk3, unk4, unk5, unk6);
}

int CheckForPurchase(int *this, char *item) {
	return !(this[2320] == 200);
}

void patch_game(void) {
	string_constructor = so_symbol(&isotope_mod, "_ZN6stringC2EPKc");
	hook_addr(so_symbol(&isotope_mod, "_Z6GetDPIv"), (uintptr_t)&GetDPI);
	hook_addr(so_symbol(&isotope_mod, "_Z9GetLocaleR6string"), (uintptr_t)&GetLocale);
	hook_addr(so_symbol(&isotope_mod, "_Z12GetVersionOSv"), (uintptr_t)&GetVersionOS);
	hook_addr(so_symbol(&isotope_mod, "_Z19isInternetAvailablev"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&isotope_mod, "_Z7iapInitPc"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&isotope_mod, "_Z16ShellExecuteUtilPvPcjb"), (uintptr_t)&ret0);
	SaveSettings = so_symbol(&isotope_mod, "_ZN12CAtomGameImp17SerializeSettingsEb");
	serialize_hook = hook_addr(SaveSettings, (uintptr_t)&SerializeSettings);
	pthread_create(&saver_thread, NULL, settings_saver, NULL);
	
	if (enable_dlcs)
		hook_addr(so_symbol(&isotope_mod, "_ZN12CAtomGameImp16CheckForPurchaseEPc"), (uintptr_t)&CheckForPurchase);
	
	SendTouchEvent = so_symbol(&isotope_mod, "_ZN12CAtomGameImp5TouchEN13CApplication29TouchTypeE6_POINTi");
	g_pApp = (uint32_t *)so_symbol(&isotope_mod, "g_pApp");
}

extern void *__aeabi_atexit;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_ldivmod;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_uldivmod;
extern void *__cxa_atexit;
extern void *__cxa_finalize;
extern void *__cxa_call_unexpected;
extern void *__gnu_unwind_frame;
extern void *__stack_chk_fail;
int open(const char *pathname, int flags);

static int __stack_chk_guard_fake = 0x42424242;

static FILE __sF_fake[0x1000][3];

int stat_hook(const char *pathname, void *statbuf) {
	//dlog("stat(%s)\n", pathname);
	struct stat st;
	int res = stat(pathname, &st);
	if (res == 0)
		*(uint64_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
	return memalign(length, 0x1000);
}

int munmap(void *addr, size_t length) {
	free(addr);
	return 0;
}

int fstat_hook(int fd, void *statbuf) {
	struct stat st;
	int res = fstat(fd, &st);
	if (res == 0)
		*(uint64_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;

char *basename(char *path) {
	char *p = path;
	if (strlen(path) == 1)
		return path;
	char *slash = strstr(p, "/");
	while (slash) {
		p = slash + 1;
		slash = strstr(p, "/");
	}
	return p;
}

void *sceClibMemclr(void *dst, SceSize len) {
	return sceClibMemset(dst, 0, len);
}

void *sceClibMemset2(void *dst, SceSize len, int ch) {
	return sceClibMemset(dst, ch, len);
}

void *Android_JNI_GetEnv() {
	return fake_env;
}

char *SDL_AndroidGetExternalStoragePath() {
	return DATA_PATH;
}

char *SDL_AndroidGetInternalStoragePath() {
	return DATA_PATH;
}

char *SDL_GetBasePath_hook() {
	char *r = (char *)SDL_malloc(512);
	sprintf(r, "%s/assets/", DATA_PATH);
	return r;
}

int g_SDL_BufferGeometry_w;
int g_SDL_BufferGeometry_h;

void abort_hook() {
	//dlog("ABORT CALLED!!!\n");
	uint8_t *p = NULL;
	p[0] = 1;
}

int ret99() {
	return 99;
}

int chdir_hook(const char *path) {
	return 0;
}

void *SDL_GL_GetProcAddress_fake(const char *symbol) {
	void *r = vglGetProcAddress(symbol);
	if (!r) {
		dlog("Cannot find symbol %s\n", symbol);
	}
	return r;
}

#define SCE_ERRNO_MASK 0xFF

#define DT_DIR 4
#define DT_REG 8

struct android_dirent {
	char pad[18];
	unsigned char d_type;
	char d_name[256];
};

typedef struct {
	SceUID uid;
	struct android_dirent dir;
} android_DIR;

int closedir_fake(android_DIR *dirp) {
	if (!dirp || dirp->uid < 0) {
		errno = EBADF;
		return -1;
	}

	int res = sceIoDclose(dirp->uid);
	dirp->uid = -1;

	free(dirp);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return -1;
	}

	errno = 0;
	return 0;
}

android_DIR *opendir_fake(const char *dirname) {
	//printf("opendir(%s)\n", dirname);
	SceUID uid = sceIoDopen(dirname);

	if (uid < 0) {
		errno = uid & SCE_ERRNO_MASK;
		return NULL;
	}

	android_DIR *dirp = calloc(1, sizeof(android_DIR));

	if (!dirp) {
		sceIoDclose(uid);
		errno = ENOMEM;
		return NULL;
	}

	dirp->uid = uid;

	errno = 0;
	return dirp;
}

struct android_dirent *readdir_fake(android_DIR *dirp) {
	if (!dirp) {
		errno = EBADF;
		return NULL;
	}

	SceIoDirent sce_dir;
	int res = sceIoDread(dirp->uid, &sce_dir);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return NULL;
	}

	if (res == 0) {
		errno = 0;
		return NULL;
	}

	dirp->dir.d_type = SCE_S_ISDIR(sce_dir.d_stat.st_mode) ? DT_DIR : DT_REG;
	strcpy(dirp->dir.d_name, sce_dir.d_name);
	return &dirp->dir;
}

struct tm local_time;
struct tm *localtime_hook(const time_t *timer) {
	SceDateTime date;
	sceRtcGetCurrentClockLocalTime(&date);
	local_time.tm_year = date.year;
	local_time.tm_mon = date.month;
	local_time.tm_mday = date.day;
	local_time.tm_hour = date.hour;
	local_time.tm_min = date.minute;
	local_time.tm_sec = date.second;
	return &local_time;
}

SDL_Surface *IMG_Load_hook(const char *file) {
	char real_fname[256];
	//printf("loading %s\n", file);
	if (strncmp(file, "ux0:", 4)) {
		sprintf(real_fname, "%s/assets/%s", DATA_PATH, file);
		return IMG_Load(real_fname);
	}
	return IMG_Load(file);
}

SDL_Texture * IMG_LoadTexture_hook(SDL_Renderer *renderer, const char *file) {
	char real_fname[256];
	//printf("loading %s\n", file);
	if (strncmp(file, "ux0:", 4)) {
		sprintf(real_fname, "%s/assets/%s", DATA_PATH, file);
		return IMG_LoadTexture(renderer, real_fname);
	}
	return IMG_LoadTexture(renderer, file);
}

SDL_RWops *SDL_RWFromFile_hook(const char *fname, const char *mode) {
	SDL_RWops *f;
	char real_fname[256];
	//printf("SDL_RWFromFile(%s,%s)\n", fname, mode);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%s/assets/%s", DATA_PATH, fname);
		//printf("SDL_RWFromFile patched to %s\n", real_fname);
		f = SDL_RWFromFile(real_fname, mode);
	} else {
		char *s = strstr(fname + 1, "ux0:");
		if (s)
			fname = s;
		//printf("SDL_RWFromFile patched to %s\n", fname);
		f = SDL_RWFromFile(fname, mode);
	}
	return f;
}

FILE *fopen_hook(char *fname, char *mode) {
	FILE *f;
	char real_fname[256];
	//printf("fopen(%s,%s)\n", fname, mode);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%s/%s", DATA_PATH, fname);
		f = fopen(real_fname, mode);
	} else {
		f = fopen(fname, mode);
	}
	return f;
}

void *dlsym_hook( void *handle, const char *symbol) {
	//printf("dlsym: %s\n", symbol);

	return vglGetProcAddress(symbol);
}

int SDL_Init_fake(uint32_t flags) {
	int r = SDL_Init(flags);
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
	return r;
}

int SDL_PollEvent_fake(SDL_Event *event) {
	// Discard touch inputs
	int r = SDL_PollEvent(event);
	while (r && (event->type >= 0x700)) {
		r = SDL_PollEvent(event);
	}
	
	// Reimplement touch
	if (!r) {
		SceTouchData touch;
		sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
		for (int i = 0; i < 6; i++) {
			if (touch.reportNum > i) {
				SendTouchEvent(*g_pApp, old_touch.reportNum > i ? TOUCH_HOLD : TOUCH_DOWN, touch.report[i].x / 2, touch.report[i].y / 2, i);
			} else if (old_touch.reportNum > i) {
				SendTouchEvent(*g_pApp, TOUCH_UP, old_touch.report[i].x / 2, old_touch.report[i].y / 2, i);
			}
		}
		sceClibMemcpy(&old_touch, &touch, sizeof(SceTouchData));
	}
	
	return r;
}

static so_default_dynlib default_dynlib[] = {
	{ "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
	{ "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
	{ "SL_IID_ENVIRONMENTALREVERB", (uintptr_t)&SL_IID_ENVIRONMENTALREVERB },
	{ "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
	{ "SL_IID_PLAYBACKRATE", (uintptr_t)&SL_IID_PLAYBACKRATE },
	{ "SL_IID_SEEK", (uintptr_t)&SL_IID_SEEK },
	{ "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
	{ "slCreateEngine", (uintptr_t)&slCreateEngine },
	{ "opendir", (uintptr_t)&opendir_fake },
	{ "readdir", (uintptr_t)&readdir_fake },
	{ "closedir", (uintptr_t)&closedir_fake },
	{ "g_SDL_BufferGeometry_w", (uintptr_t)&g_SDL_BufferGeometry_w },
	{ "g_SDL_BufferGeometry_h", (uintptr_t)&g_SDL_BufferGeometry_h },
	{ "__aeabi_memclr", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr4", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr8", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove4", (uintptr_t)&sceClibMemmove },
	{ "__aeabi_memmove8", (uintptr_t)&sceClibMemmove },
	{ "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove", (uintptr_t)&sceClibMemmove },
	{ "__aeabi_memset", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset4", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset8", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
	{ "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
	{ "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
	{ "__aeabi_ul2d", (uintptr_t)&__aeabi_ul2d },
	{ "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
	{ "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
	{ "__android_log_print", (uintptr_t)&__android_log_print },
	{ "__android_log_vprint", (uintptr_t)&__android_log_vprint },
	{ "__android_log_write", (uintptr_t)&__android_log_write },
	{ "__cxa_atexit", (uintptr_t)&__cxa_atexit },
	{ "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
	{ "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
	{ "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
	{ "__cxa_finalize", (uintptr_t)&__cxa_finalize },
	{ "__errno", (uintptr_t)&__errno },
	{ "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
	{ "__gnu_Unwind_Find_exidx", (uintptr_t)&ret0 },
	{ "dl_unwind_find_exidx", (uintptr_t)&ret0 },
	// { "__google_potentially_blocking_region_begin", (uintptr_t)&__google_potentially_blocking_region_begin },
	// { "__google_potentially_blocking_region_end", (uintptr_t)&__google_potentially_blocking_region_end },
	{ "__sF", (uintptr_t)&__sF_fake },
	{ "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
	{ "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
	{ "_ctype_", (uintptr_t)&BIONIC_ctype_},
	{ "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_},
	{ "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_},
	{ "abort", (uintptr_t)&abort_hook },
	{ "access", (uintptr_t)&access },
	{ "acos", (uintptr_t)&acos },
	{ "acosh", (uintptr_t)&acosh },
	{ "asctime", (uintptr_t)&asctime },
	{ "acosf", (uintptr_t)&acosf },
	{ "asin", (uintptr_t)&asin },
	{ "asinh", (uintptr_t)&asinh },
	{ "asinf", (uintptr_t)&asinf },
	{ "atan", (uintptr_t)&atan },
	{ "atanh", (uintptr_t)&atanh },
	{ "atan2", (uintptr_t)&atan2 },
	{ "atan2f", (uintptr_t)&atan2f },
	{ "atanf", (uintptr_t)&atanf },
	{ "atoi", (uintptr_t)&atoi },
	{ "atol", (uintptr_t)&atol },
	{ "atoll", (uintptr_t)&atoll },
	{ "basename", (uintptr_t)&basename },
	// { "bind", (uintptr_t)&bind },
	{ "bsearch", (uintptr_t)&bsearch },
	{ "btowc", (uintptr_t)&btowc },
	{ "calloc", (uintptr_t)&calloc },
	{ "ceil", (uintptr_t)&ceil },
	{ "ceilf", (uintptr_t)&ceilf },
	{ "chdir", (uintptr_t)&chdir_hook },
	{ "clearerr", (uintptr_t)&clearerr },
	{ "clock", (uintptr_t)&clock },
	{ "clock_gettime", (uintptr_t)&clock_gettime_hook },
	{ "close", (uintptr_t)&close },
	{ "cos", (uintptr_t)&cos },
	{ "cosf", (uintptr_t)&cosf },
	{ "cosh", (uintptr_t)&cosh },
	{ "crc32", (uintptr_t)&crc32 },
	{ "deflate", (uintptr_t)&deflate },
	{ "deflateEnd", (uintptr_t)&deflateEnd },
	{ "deflateInit_", (uintptr_t)&deflateInit_ },
	{ "deflateInit2_", (uintptr_t)&deflateInit2_ },
	{ "deflateReset", (uintptr_t)&deflateReset },
	{ "difftime", (uintptr_t)&difftime },
	{ "dlopen", (uintptr_t)&ret0 },
	{ "dlsym", (uintptr_t)&dlsym_hook },
	{ "exit", (uintptr_t)&exit },
	{ "exp", (uintptr_t)&exp },
	{ "exp2", (uintptr_t)&exp2 },
	{ "expf", (uintptr_t)&expf },
	{ "fabsf", (uintptr_t)&fabsf },
	{ "fclose", (uintptr_t)&fclose },
	{ "fcntl", (uintptr_t)&ret0 },
	// { "fdopen", (uintptr_t)&fdopen },
	{ "ferror", (uintptr_t)&ferror },
	{ "fflush", (uintptr_t)&fflush },
	{ "fgetpos", (uintptr_t)&fgetpos },
	{ "fsetpos", (uintptr_t)&fsetpos },
	{ "floor", (uintptr_t)&floor },
	{ "floorf", (uintptr_t)&floorf },
	{ "fmod", (uintptr_t)&fmod },
	{ "fmodf", (uintptr_t)&fmodf },
	{ "fopen", (uintptr_t)&fopen_hook },
	{ "fprintf", (uintptr_t)&fprintf },
	{ "fputc", (uintptr_t)&fputc },
	// { "fputwc", (uintptr_t)&fputwc },
	{ "fputs", (uintptr_t)&fputs },
	{ "fread", (uintptr_t)&fread },
	{ "free", (uintptr_t)&free },
	{ "frexp", (uintptr_t)&frexp },
	{ "frexpf", (uintptr_t)&frexpf },
	// { "fscanf", (uintptr_t)&fscanf },
	{ "fseek", (uintptr_t)&fseek },
	{ "fseeko", (uintptr_t)&fseeko },
	{ "fstat", (uintptr_t)&fstat },
	{ "ftell", (uintptr_t)&ftell },
	{ "ftello", (uintptr_t)&ftello },
	// { "ftruncate", (uintptr_t)&ftruncate },
	{ "fwrite", (uintptr_t)&fwrite },
	{ "getc", (uintptr_t)&getc },
	{ "getpid", (uintptr_t)&ret0 },
	{ "getcwd", (uintptr_t)&getcwd_hook },
	{ "getenv", (uintptr_t)&ret0 },
	{ "getwc", (uintptr_t)&getwc },
	{ "gettimeofday", (uintptr_t)&gettimeofday },
	{ "gzopen", (uintptr_t)&gzopen },
	{ "inflate", (uintptr_t)&inflate },
	{ "inflateEnd", (uintptr_t)&inflateEnd },
	{ "inflateInit_", (uintptr_t)&inflateInit_ },
	{ "inflateInit2_", (uintptr_t)&inflateInit2_ },
	{ "inflateReset", (uintptr_t)&inflateReset },
	{ "isalnum", (uintptr_t)&isalnum },
	{ "isalpha", (uintptr_t)&isalpha },
	{ "iscntrl", (uintptr_t)&iscntrl },
	{ "isdigit", (uintptr_t)&isdigit },
	{ "islower", (uintptr_t)&islower },
	{ "ispunct", (uintptr_t)&ispunct },
	{ "isprint", (uintptr_t)&isprint },
	{ "isspace", (uintptr_t)&isspace },
	{ "isupper", (uintptr_t)&isupper },
	{ "iswalpha", (uintptr_t)&iswalpha },
	{ "iswcntrl", (uintptr_t)&iswcntrl },
	{ "iswctype", (uintptr_t)&iswctype },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswlower", (uintptr_t)&iswlower },
	{ "iswprint", (uintptr_t)&iswprint },
	{ "iswpunct", (uintptr_t)&iswpunct },
	{ "iswspace", (uintptr_t)&iswspace },
	{ "iswupper", (uintptr_t)&iswupper },
	{ "iswxdigit", (uintptr_t)&iswxdigit },
	{ "isxdigit", (uintptr_t)&isxdigit },
	{ "ldexp", (uintptr_t)&ldexp },
	{ "ldexpf", (uintptr_t)&ldexpf },
	// { "listen", (uintptr_t)&listen },
	{ "localtime", (uintptr_t)&localtime_hook },
	{ "localtime_r", (uintptr_t)&localtime_r },
	{ "log", (uintptr_t)&log },
	{ "logf", (uintptr_t)&logf },
	{ "log10", (uintptr_t)&log10 },
	{ "log10f", (uintptr_t)&log10f },
	{ "longjmp", (uintptr_t)&longjmp },
	{ "lrand48", (uintptr_t)&lrand48 },
	{ "lrint", (uintptr_t)&lrint },
	{ "lrintf", (uintptr_t)&lrintf },
	{ "lseek", (uintptr_t)&lseek },
	{ "malloc", (uintptr_t)&malloc },
	{ "mbrtowc", (uintptr_t)&mbrtowc },
	{ "memalign", (uintptr_t)&memalign },
	{ "memchr", (uintptr_t)&sceClibMemchr },
	{ "memcmp", (uintptr_t)&sceClibMemcmp },
	{ "memcpy", (uintptr_t)&sceClibMemcpy },
	{ "memmove", (uintptr_t)&sceClibMemmove },
	{ "memset", (uintptr_t)&sceClibMemset },
	{ "mkdir", (uintptr_t)&mkdir },
	// { "mmap", (uintptr_t)&mmap},
	// { "munmap", (uintptr_t)&munmap},
	{ "modf", (uintptr_t)&modf },
	{ "modff", (uintptr_t)&modff },
	// { "poll", (uintptr_t)&poll },
	// { "open", (uintptr_t)&open },
	{ "pow", (uintptr_t)&pow },
	{ "powf", (uintptr_t)&powf },
	{ "printf", (uintptr_t)&printf },
	{ "pthread_attr_destroy", (uintptr_t)&ret0 },
	{ "pthread_attr_init", (uintptr_t)&ret0 },
	{ "pthread_attr_setdetachstate", (uintptr_t)&ret0 },
	{ "pthread_attr_setstacksize", (uintptr_t)&ret0 },
	{ "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake},
	{ "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},
	{ "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},
	{ "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},
	{ "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake},
	{ "pthread_cond_timedwait_relative_np", (uintptr_t)&pthread_cond_timedwait_relative_np_fake}, // FIXME
	{ "pthread_create", (uintptr_t)&pthread_create_fake },
	{ "pthread_getschedparam", (uintptr_t)&pthread_getschedparam },
	{ "pthread_getspecific", (uintptr_t)&pthread_getspecific },
	{ "pthread_key_create", (uintptr_t)&pthread_key_create },
	{ "pthread_key_delete", (uintptr_t)&pthread_key_delete },
	{ "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
	{ "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
	{ "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
	{ "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
	{ "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
	{ "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy},
	{ "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init},
	{ "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype},
	{ "pthread_once", (uintptr_t)&pthread_once_fake },
	{ "pthread_self", (uintptr_t)&pthread_self },
	{ "pthread_setname_np", (uintptr_t)&ret0 },
	{ "pthread_getschedparam", (uintptr_t)&pthread_getschedparam },
	{ "pthread_setschedparam", (uintptr_t)&pthread_setschedparam },
	{ "pthread_setspecific", (uintptr_t)&pthread_setspecific },
	{ "sched_get_priority_min", (uintptr_t)&ret0 },
	{ "sched_get_priority_max", (uintptr_t)&ret99 },
	{ "putc", (uintptr_t)&putc },
	{ "puts", (uintptr_t)&puts },
	{ "putwc", (uintptr_t)&putwc },
	{ "qsort", (uintptr_t)&qsort },
	{ "rand", (uintptr_t)&rand },
	{ "gmtime", (uintptr_t)&gmtime },
	{ "read", (uintptr_t)&read },
	{ "realpath", (uintptr_t)&realpath },
	{ "realloc", (uintptr_t)&realloc },
	{ "rename", (uintptr_t)&rename },
	{ "remove", (uintptr_t)&remove },
	// { "recv", (uintptr_t)&recv },
	{ "roundf", (uintptr_t)&roundf },
	{ "rint", (uintptr_t)&rint },
	{ "rintf", (uintptr_t)&rintf },
	// { "send", (uintptr_t)&send },
	// { "sendto", (uintptr_t)&sendto },
	{ "setenv", (uintptr_t)&ret0 },
	{ "setjmp", (uintptr_t)&setjmp },
	{ "setlocale", (uintptr_t)&ret0 },
	// { "setsockopt", (uintptr_t)&setsockopt },
	{ "setvbuf", (uintptr_t)&setvbuf },
	{ "sin", (uintptr_t)&sin },
	{ "sinf", (uintptr_t)&sinf },
	{ "sinh", (uintptr_t)&sinh },
	//{ "sincos", (uintptr_t)&sincos },
	{ "snprintf", (uintptr_t)&snprintf },
	// { "socket", (uintptr_t)&socket },
	{ "sprintf", (uintptr_t)&sprintf },
	{ "sqrt", (uintptr_t)&sqrt },
	{ "sqrtf", (uintptr_t)&sqrtf },
	{ "srand", (uintptr_t)&srand },
	{ "srand48", (uintptr_t)&srand48 },
	{ "sscanf", (uintptr_t)&sscanf },
	{ "stat", (uintptr_t)&stat_hook },
	{ "strcasecmp", (uintptr_t)&strcasecmp },
	{ "strcasestr", (uintptr_t)&strstr },
	{ "strcat", (uintptr_t)&strcat },
	{ "strchr", (uintptr_t)&strchr },
	{ "strcmp", (uintptr_t)&sceClibStrcmp },
	{ "strcoll", (uintptr_t)&strcoll },
	{ "strcpy", (uintptr_t)&strcpy },
	{ "strcspn", (uintptr_t)&strcspn },
	{ "strdup", (uintptr_t)&strdup },
	{ "strerror", (uintptr_t)&strerror },
	{ "strftime", (uintptr_t)&strftime },
	{ "strlcpy", (uintptr_t)&strlcpy },
	{ "strlen", (uintptr_t)&strlen },
	{ "strncasecmp", (uintptr_t)&sceClibStrncasecmp },
	{ "strncat", (uintptr_t)&sceClibStrncat },
	{ "strncmp", (uintptr_t)&sceClibStrncmp },
	{ "strncpy", (uintptr_t)&sceClibStrncpy },
	{ "strpbrk", (uintptr_t)&strpbrk },
	{ "strrchr", (uintptr_t)&sceClibStrrchr },
	{ "strstr", (uintptr_t)&sceClibStrstr },
	{ "strtod", (uintptr_t)&strtod },
	{ "strtol", (uintptr_t)&strtol },
	{ "strtoul", (uintptr_t)&strtoul },
	{ "strtoll", (uintptr_t)&strtoll },
	{ "strtoull", (uintptr_t)&strtoull },
	{ "strxfrm", (uintptr_t)&strxfrm },
	{ "sysconf", (uintptr_t)&ret0 },
	{ "tan", (uintptr_t)&tan },
	{ "tanf", (uintptr_t)&tanf },
	{ "tanh", (uintptr_t)&tanh },
	{ "time", (uintptr_t)&time },
	{ "tolower", (uintptr_t)&tolower },
	{ "toupper", (uintptr_t)&toupper },
	{ "towlower", (uintptr_t)&towlower },
	{ "towupper", (uintptr_t)&towupper },
	{ "ungetc", (uintptr_t)&ungetc },
	{ "ungetwc", (uintptr_t)&ungetwc },
	{ "usleep", (uintptr_t)&usleep },
	{ "vfprintf", (uintptr_t)&vfprintf },
	{ "vprintf", (uintptr_t)&vprintf },
	{ "vsnprintf", (uintptr_t)&vsnprintf },
	{ "vsprintf", (uintptr_t)&vsprintf },
	{ "vswprintf", (uintptr_t)&vswprintf },
	{ "wcrtomb", (uintptr_t)&wcrtomb },
	{ "wcscoll", (uintptr_t)&wcscoll },
	{ "wcscmp", (uintptr_t)&wcscmp },
	{ "wcsncpy", (uintptr_t)&wcsncpy },
	{ "wcsftime", (uintptr_t)&wcsftime },
	{ "wcslen", (uintptr_t)&wcslen },
	{ "wcsxfrm", (uintptr_t)&wcsxfrm },
	{ "wctob", (uintptr_t)&wctob },
	{ "wctype", (uintptr_t)&wctype },
	{ "wmemchr", (uintptr_t)&wmemchr },
	{ "wmemcmp", (uintptr_t)&wmemcmp },
	{ "wmemcpy", (uintptr_t)&wmemcpy },
	{ "wmemmove", (uintptr_t)&wmemmove },
	{ "wmemset", (uintptr_t)&wmemset },
	{ "write", (uintptr_t)&write },
	// { "writev", (uintptr_t)&writev },
	{ "glClearColor", (uintptr_t)&glClearColor },
	{ "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
	{ "glTexImage2D", (uintptr_t)&glTexImage2D },
	{ "glDeleteTextures", (uintptr_t)&glDeleteTextures },
	{ "glGenTextures", (uintptr_t)&glGenTextures },
	{ "glBindTexture", (uintptr_t)&glBindTexture },
	{ "glTexParameteri", (uintptr_t)&glTexParameteri },
	{ "glGetError", (uintptr_t)&ret0 },
	{ "glMatrixMode", (uintptr_t)&glMatrixMode },
	{ "glLoadIdentity", (uintptr_t)&glLoadIdentity },
	{ "glScalef", (uintptr_t)&glScalef },
	{ "glClear", (uintptr_t)&glClear },
	{ "glGetString", (uintptr_t)&glGetString },
	{ "glGetIntegerv", (uintptr_t)&glGetIntegerv },
	{ "glOrthof", (uintptr_t)&glOrthof },
	{ "glViewport", (uintptr_t)&glViewport },
	{ "glScissor", (uintptr_t)&glScissor },
	{ "glEnable", (uintptr_t)&glEnable },
	{ "glDisable", (uintptr_t)&glDisable },
	{ "glEnableClientState", (uintptr_t)&glEnableClientState },
	{ "glDisableClientState", (uintptr_t)&glDisableClientState },
	{ "glBlendFunc", (uintptr_t)&glBlendFunc },
	{ "glColorPointer", (uintptr_t)&glColorPointer },
	{ "glVertexPointer", (uintptr_t)&glVertexPointer },
	{ "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
	{ "glDrawElements", (uintptr_t)&glDrawElements },
	{ "glPixelStorei", (uintptr_t)&glPixelStorei },
	{ "glPushMatrix", (uintptr_t)&glPushMatrix },
	{ "glPopMatrix", (uintptr_t)&glPopMatrix },
	{ "glDrawArrays", (uintptr_t)&glDrawArrays },
	{ "SDL_IsTextInputActive", (uintptr_t)&SDL_IsTextInputActive },
	{ "SDL_GameControllerEventState", (uintptr_t)&SDL_GameControllerEventState },
	{ "SDL_WarpMouseInWindow", (uintptr_t)&SDL_WarpMouseInWindow },
	{ "SDL_AndroidGetExternalStoragePath", (uintptr_t)&SDL_AndroidGetExternalStoragePath },
	{ "SDL_AndroidGetInternalStoragePath", (uintptr_t)&SDL_AndroidGetInternalStoragePath },
	{ "SDL_GetBasePath", (uintptr_t)&SDL_GetBasePath_hook },
	{ "SDL_Android_Init", (uintptr_t)&ret1 },
	{ "SDL_AddTimer", (uintptr_t)&SDL_AddTimer },
	{ "SDL_CondSignal", (uintptr_t)&SDL_CondSignal },
	{ "SDL_CondWait", (uintptr_t)&SDL_CondWait },
	{ "SDL_ConvertSurfaceFormat", (uintptr_t)&SDL_ConvertSurfaceFormat },
	{ "SDL_CreateCond", (uintptr_t)&SDL_CreateCond },
	{ "SDL_CreateMutex", (uintptr_t)&SDL_CreateMutex },
	{ "SDL_CreateRenderer", (uintptr_t)&SDL_CreateRenderer },
	{ "SDL_CreateRGBSurface", (uintptr_t)&SDL_CreateRGBSurface },
	{ "SDL_CreateTexture", (uintptr_t)&SDL_CreateTexture },
	{ "SDL_CreateTextureFromSurface", (uintptr_t)&SDL_CreateTextureFromSurface },
	{ "SDL_CreateThread", (uintptr_t)&SDL_CreateThread },
	{ "SDL_CreateWindow", (uintptr_t)&SDL_CreateWindow },
	{ "SDL_Delay", (uintptr_t)&SDL_Delay },
	{ "SDL_DestroyMutex", (uintptr_t)&SDL_DestroyMutex },
	{ "SDL_DestroyRenderer", (uintptr_t)&SDL_DestroyRenderer },
	{ "SDL_DestroyTexture", (uintptr_t)&SDL_DestroyTexture },
	{ "SDL_DestroyWindow", (uintptr_t)&SDL_DestroyWindow },
	{ "SDL_FillRect", (uintptr_t)&SDL_FillRect },
	{ "SDL_FreeSurface", (uintptr_t)&SDL_FreeSurface },
	{ "SDL_GetCurrentDisplayMode", (uintptr_t)&SDL_GetCurrentDisplayMode },
	{ "SDL_GetDisplayMode", (uintptr_t)&SDL_GetDisplayMode },
	{ "SDL_GetError", (uintptr_t)&SDL_GetError },
	{ "SDL_GetModState", (uintptr_t)&SDL_GetModState },
	{ "SDL_GetMouseState", (uintptr_t)&SDL_GetMouseState },
	{ "SDL_GetRGBA", (uintptr_t)&SDL_GetRGBA },
	{ "SDL_GameControllerAddMappingsFromRW", (uintptr_t)&SDL_GameControllerAddMappingsFromRW },
	{ "SDL_GetNumDisplayModes", (uintptr_t)&SDL_GetNumDisplayModes },
	{ "SDL_GetRendererInfo", (uintptr_t)&SDL_GetRendererInfo },
	{ "SDL_GetTextureBlendMode", (uintptr_t)&SDL_GetTextureBlendMode },
	{ "SDL_GetPrefPath", (uintptr_t)&SDL_GetPrefPath },
	{ "SDL_GetTextureColorMod", (uintptr_t)&SDL_GetTextureColorMod },
	{ "SDL_GetTicks", (uintptr_t)&SDL_GetTicks },
	{ "SDL_GetVersion", (uintptr_t)&SDL_GetVersion },
	{ "SDL_GL_BindTexture", (uintptr_t)&SDL_GL_BindTexture },
	{ "SDL_GL_GetCurrentContext", (uintptr_t)&SDL_GL_GetCurrentContext },
	{ "SDL_GL_MakeCurrent", (uintptr_t)&SDL_GL_MakeCurrent },
	{ "SDL_GL_SetAttribute", (uintptr_t)&SDL_GL_SetAttribute },
	{ "SDL_Init", (uintptr_t)&SDL_Init_fake },
	{ "SDL_InitSubSystem", (uintptr_t)&SDL_InitSubSystem },
	{ "SDL_IntersectRect", (uintptr_t)&SDL_IntersectRect },
	{ "SDL_LockMutex", (uintptr_t)&SDL_LockMutex },
	{ "SDL_LockSurface", (uintptr_t)&SDL_LockSurface },
	{ "SDL_Log", (uintptr_t)&ret0 },
	{ "SDL_LogMessage", (uintptr_t)&SDL_LogMessage },
	{ "SDL_free", (uintptr_t)&SDL_free },
	{ "SDL_free_REAL", (uintptr_t)&SDL_free },
	{ "SDL_LogError", (uintptr_t)&ret0 },
	{ "SDL_LogSetPriority", (uintptr_t)&ret0 },
	{ "SDL_MapRGB", (uintptr_t)&SDL_MapRGB },
	{ "SDL_JoystickInstanceID", (uintptr_t)&SDL_JoystickInstanceID },
	{ "SDL_GameControllerGetAxis", (uintptr_t)&SDL_GameControllerGetAxis },
	{ "SDL_JoystickName", (uintptr_t)&SDL_JoystickName },
	{ "SDL_GetRenderTarget", (uintptr_t)&SDL_GetRenderTarget },
	{ "SDL_LockTexture", (uintptr_t)&SDL_LockTexture },
	{ "SDL_RenderReadPixels", (uintptr_t)&SDL_RenderReadPixels },
	{ "SDL_UnlockTexture", (uintptr_t)&SDL_UnlockTexture },
	{ "SDL_JoystickNameForIndex", (uintptr_t)&SDL_JoystickNameForIndex },
	{ "SDL_JoystickEventState", (uintptr_t)&SDL_JoystickEventState },
	// { "SDL_ResetKeyboard", (uintptr_t)&SDL_ResetKeyboard },
	{ "SDL_SetClipboardText", (uintptr_t)&SDL_SetClipboardText },
	{ "SDL_GetClipboardText", (uintptr_t)&SDL_GetClipboardText },
	{ "SDL_HasClipboardText", (uintptr_t)&SDL_HasClipboardText },
	{ "SDL_QuitSubSystem", (uintptr_t)&SDL_QuitSubSystem },
	{ "SDL_GameControllerClose", (uintptr_t)&SDL_GameControllerClose },
	{ "SDL_JoystickClose", (uintptr_t)&SDL_JoystickClose },
	{ "SDL_JoystickOpen", (uintptr_t)&SDL_JoystickOpen },
	{ "SDL_GameControllerGetAttached", (uintptr_t)&SDL_GameControllerGetAttached },
	{ "SDL_JoystickGetAttached", (uintptr_t)&SDL_JoystickGetAttached },
	{ "SDL_JoystickNumAxes", (uintptr_t)&SDL_JoystickNumAxes },
	{ "SDL_JoystickGetAxis", (uintptr_t)&SDL_JoystickGetAxis },
	{ "SDL_JoystickNumButtons", (uintptr_t)&SDL_JoystickNumButtons },
	{ "SDL_MinimizeWindow", (uintptr_t)&SDL_MinimizeWindow },
	{ "SDL_PeepEvents", (uintptr_t)&SDL_PeepEvents },
	{ "SDL_PumpEvents", (uintptr_t)&SDL_PumpEvents },
	{ "SDL_PushEvent", (uintptr_t)&SDL_PushEvent },
	{ "SDL_PollEvent", (uintptr_t)&SDL_PollEvent_fake },
	{ "SDL_QueryTexture", (uintptr_t)&SDL_QueryTexture },
	{ "SDL_Quit", (uintptr_t)&SDL_Quit },
	{ "SDL_RemoveTimer", (uintptr_t)&SDL_RemoveTimer },
	{ "SDL_RenderClear", (uintptr_t)&SDL_RenderClear },
	{ "SDL_RenderCopy", (uintptr_t)&SDL_RenderCopy },
	{ "SDL_RenderFillRect", (uintptr_t)&SDL_RenderFillRect },
	{ "SDL_RenderPresent", (uintptr_t)&SDL_RenderPresent },
	{ "SDL_RWFromFile", (uintptr_t)&SDL_RWFromFile_hook },
	{ "SDL_RWread", (uintptr_t)&SDL_RWread },
	{ "SDL_RWwrite", (uintptr_t)&SDL_RWwrite },
	{ "SDL_RWclose", (uintptr_t)&SDL_RWclose },
	{ "SDL_RWsize", (uintptr_t)&SDL_RWsize },
	{ "SDL_RWFromMem", (uintptr_t)&SDL_RWFromMem },
	{ "SDL_SetColorKey", (uintptr_t)&SDL_SetColorKey },
	{ "SDL_SetEventFilter", (uintptr_t)&SDL_SetEventFilter },
	{ "SDL_SetHint", (uintptr_t)&SDL_SetHint },
	{ "SDL_SetMainReady_REAL", (uintptr_t)&SDL_SetMainReady },
	{ "SDL_SetRenderDrawBlendMode", (uintptr_t)&SDL_SetRenderDrawBlendMode },
	{ "SDL_SetRenderDrawColor", (uintptr_t)&SDL_SetRenderDrawColor },
	{ "SDL_SetRenderTarget", (uintptr_t)&SDL_SetRenderTarget },
	{ "SDL_SetTextureBlendMode", (uintptr_t)&SDL_SetTextureBlendMode },
	{ "SDL_SetTextureColorMod", (uintptr_t)&SDL_SetTextureColorMod },
	{ "SDL_ShowCursor", (uintptr_t)&SDL_ShowCursor },
	{ "SDL_ShowSimpleMessageBox", (uintptr_t)&SDL_ShowSimpleMessageBox },
	{ "SDL_StartTextInput", (uintptr_t)&SDL_StartTextInput },
	{ "SDL_StopTextInput", (uintptr_t)&SDL_StopTextInput },
	{ "SDL_strdup", (uintptr_t)&SDL_strdup },
	{ "SDL_strdup_REAL", (uintptr_t)&SDL_strdup },
	{ "SDL_UnlockMutex", (uintptr_t)&SDL_UnlockMutex },
	{ "SDL_UnlockSurface", (uintptr_t)&SDL_UnlockSurface },
	{ "SDL_UpdateTexture", (uintptr_t)&SDL_UpdateTexture },
	{ "SDL_UpperBlit", (uintptr_t)&SDL_UpperBlit },
	{ "SDL_WaitThread", (uintptr_t)&SDL_WaitThread },
	{ "SDL_GetKeyFromScancode", (uintptr_t)&SDL_GetKeyFromScancode },
	{ "SDL_GetNumVideoDisplays", (uintptr_t)&SDL_GetNumVideoDisplays },
	{ "SDL_GetDisplayBounds", (uintptr_t)&SDL_GetDisplayBounds },
	{ "SDL_UnionRect", (uintptr_t)&SDL_UnionRect },
	{ "SDL_GetKeyboardFocus", (uintptr_t)&SDL_GetKeyboardFocus },
	{ "SDL_GetRelativeMouseMode", (uintptr_t)&SDL_GetRelativeMouseMode },
	{ "SDL_NumJoysticks", (uintptr_t)&SDL_NumJoysticks },
	{ "SDL_GL_GetDrawableSize", (uintptr_t)&SDL_GL_GetDrawableSize },
	{ "SDL_GameControllerOpen", (uintptr_t)&SDL_GameControllerOpen },
	{ "SDL_GameControllerGetJoystick", (uintptr_t)&SDL_GameControllerGetJoystick },
	{ "SDL_HapticOpenFromJoystick", (uintptr_t)&SDL_HapticOpenFromJoystick },
	{ "SDL_GetPerformanceFrequency", (uintptr_t)&SDL_GetPerformanceFrequency },
	{ "SDL_GetPerformanceCounter", (uintptr_t)&SDL_GetPerformanceCounter },
	{ "SDL_GetMouseFocus", (uintptr_t)&SDL_GetMouseFocus },
	{ "SDL_ShowMessageBox", (uintptr_t)&SDL_ShowMessageBox },
	{ "SDL_RaiseWindow", (uintptr_t)&SDL_RaiseWindow },
	{ "SDL_GL_GetAttribute", (uintptr_t)&SDL_GL_GetAttribute },
	{ "SDL_GL_CreateContext", (uintptr_t)&SDL_GL_CreateContext },
	{ "SDL_GL_GetProcAddress", (uintptr_t)&SDL_GL_GetProcAddress_fake },
	{ "SDL_GL_DeleteContext", (uintptr_t)&SDL_GL_DeleteContext },
	{ "SDL_GetDesktopDisplayMode", (uintptr_t)&SDL_GetDesktopDisplayMode },
	{ "SDL_SetWindowData", (uintptr_t)&SDL_SetWindowData },
	{ "SDL_GetWindowFlags", (uintptr_t)&SDL_GetWindowFlags },
	{ "SDL_GetWindowSize", (uintptr_t)&SDL_GetWindowSize },
	{ "SDL_GetWindowDisplayIndex", (uintptr_t)&SDL_GetWindowDisplayIndex },
	{ "SDL_SetWindowFullscreen", (uintptr_t)&SDL_SetWindowFullscreen },
	{ "SDL_SetWindowSize", (uintptr_t)&SDL_SetWindowSize },
	{ "SDL_SetWindowPosition", (uintptr_t)&SDL_SetWindowPosition },
	{ "SDL_GL_GetCurrentWindow", (uintptr_t)&SDL_GL_GetCurrentWindow },
	{ "SDL_GetWindowData", (uintptr_t)&SDL_GetWindowData },
	{ "SDL_GetWindowTitle", (uintptr_t)&SDL_GetWindowTitle },
	{ "SDL_RenderCopyEx", (uintptr_t)&SDL_RenderCopyEx },
	{ "SDL_SetWindowTitle", (uintptr_t)&SDL_SetWindowTitle },
	{ "SDL_GetWindowPosition", (uintptr_t)&SDL_GetWindowPosition },
	{ "SDL_GL_SetSwapInterval", (uintptr_t)&ret0 },
	{ "SDL_IsGameController", (uintptr_t)&SDL_IsGameController },
	{ "SDL_JoystickGetDeviceGUID", (uintptr_t)&SDL_JoystickGetDeviceGUID },
	{ "SDL_GameControllerNameForIndex", (uintptr_t)&SDL_GameControllerNameForIndex },
	{ "SDL_GetWindowFromID", (uintptr_t)&SDL_GetWindowFromID },
	{ "SDL_GL_SwapWindow", (uintptr_t)&SDL_GL_SwapWindow },
	{ "SDL_SetMainReady", (uintptr_t)&SDL_SetMainReady },
	{ "SDL_NumAccelerometers", (uintptr_t)&ret0 },
	{ "SDL_RegisterEvents", (uintptr_t)&SDL_RegisterEvents },
	{ "SDL_calloc", (uintptr_t)&SDL_calloc },
	{ "SDL_GL_GetSwapInterval", (uintptr_t)&SDL_GL_GetSwapInterval },
	{ "SDL_GetWindowID", (uintptr_t)&SDL_GetWindowID },
	{ "SDL_SetWindowBordered", (uintptr_t)&SDL_SetWindowBordered },
	{ "SDL_RWFromConstMem", (uintptr_t)&SDL_RWFromConstMem },
	{ "SDL_GetPlatform", (uintptr_t)&SDL_GetPlatform },
	{ "SDL_strcasecmp", (uintptr_t)&SDL_strcasecmp },
	{ "SDL_Error", (uintptr_t)&SDL_Error },
	{ "SDL_strcmp", (uintptr_t)&SDL_strcmp },
	{ "SDL_atoi", (uintptr_t)&SDL_atoi },
	{ "SDL_LogSetAllPriority", (uintptr_t)&SDL_LogSetAllPriority },
	{ "SDL_isdigit", (uintptr_t)&SDL_isdigit },
	{ "SDL_atof", (uintptr_t)&SDL_atof },
	{ "SDL_GetNumAudioDrivers", (uintptr_t)&SDL_GetNumAudioDrivers },
	{ "SDL_snprintf", (uintptr_t)&SDL_snprintf },
	{ "SDL_GetAudioDriver", (uintptr_t)&SDL_GetAudioDriver },
	{ "SDL_GetNumVideoDrivers", (uintptr_t)&SDL_GetNumVideoDrivers },
	{ "SDL_GetVideoDriver", (uintptr_t)&SDL_GetVideoDriver },
	{ "SDL_VideoInit", (uintptr_t)&SDL_VideoInit },
	{ "SDL_GetCurrentVideoDriver", (uintptr_t)&SDL_GetCurrentVideoDriver },
	{ "SDL_GetDisplayName", (uintptr_t)&SDL_GetDisplayName },
	{ "SDL_memset", (uintptr_t)&SDL_memset },
	{ "SDL_GetDisplayUsableBounds", (uintptr_t)&SDL_GetDisplayUsableBounds },
	{ "SDL_PixelFormatEnumToMasks", (uintptr_t)&SDL_PixelFormatEnumToMasks },
	{ "SDL_GetPixelFormatName", (uintptr_t)&SDL_GetPixelFormatName },
	{ "SDL_GetNumRenderDrivers", (uintptr_t)&SDL_GetNumRenderDrivers },
	{ "SDL_GetRenderDriverInfo", (uintptr_t)&SDL_GetRenderDriverInfo },
	{ "SDL_SetWindowMinimumSize", (uintptr_t)&SDL_SetWindowMinimumSize },
	{ "SDL_SetWindowMaximumSize", (uintptr_t)&SDL_SetWindowMaximumSize },
	{ "SDL_SetWindowDisplayMode", (uintptr_t)&SDL_SetWindowDisplayMode },
	{ "SDL_SetWindowHitTest", (uintptr_t)&SDL_SetWindowHitTest },
	{ "SDL_LoadBMP_RW", (uintptr_t)&SDL_LoadBMP_RW },
	{ "SDL_SetWindowIcon", (uintptr_t)&SDL_SetWindowIcon },
	{ "SDL_ShowWindow", (uintptr_t)&SDL_ShowWindow },
	{ "SDL_RenderSetLogicalSize", (uintptr_t)&SDL_RenderSetLogicalSize },
	{ "SDL_RenderSetScale", (uintptr_t)&SDL_RenderSetScale },
	{ "SDL_strlcpy", (uintptr_t)&SDL_strlcpy },
	{ "SDL_AudioInit", (uintptr_t)&SDL_AudioInit },
	{ "SDL_GetCurrentAudioDriver", (uintptr_t)&SDL_GetCurrentAudioDriver },
	{ "SDL_OpenAudio", (uintptr_t)&SDL_OpenAudio },
	{ "SDL_strlen", (uintptr_t)&SDL_strlen },
	{ "SDL_vsnprintf", (uintptr_t)&SDL_vsnprintf },
	{ "SDL_GetScancodeName", (uintptr_t)&SDL_GetScancodeName },
	{ "SDL_GetKeyName", (uintptr_t)&SDL_GetKeyName },
	{ "SDL_GetGlobalMouseState", (uintptr_t)&SDL_GetGlobalMouseState },
	{ "SDL_RenderGetViewport", (uintptr_t)&SDL_RenderGetViewport },
	{ "SDL_SaveBMP_RW", (uintptr_t)&SDL_SaveBMP_RW },
	{ "SDL_RenderGetClipRect", (uintptr_t)&SDL_RenderGetClipRect },
	{ "SDL_RenderSetClipRect", (uintptr_t)&SDL_RenderSetClipRect },
	{ "SDL_CaptureMouse", (uintptr_t)&SDL_CaptureMouse },
	{ "SDL_GetWindowGrab", (uintptr_t)&SDL_GetWindowGrab },
	{ "SDL_GetWindowOpacity", (uintptr_t)&SDL_GetWindowOpacity },
	{ "SDL_SetWindowOpacity", (uintptr_t)&SDL_SetWindowOpacity },
	{ "SDL_SetRelativeMouseMode", (uintptr_t)&SDL_SetRelativeMouseMode },
	{ "SDL_SetWindowGrab", (uintptr_t)&SDL_SetWindowGrab },
	{ "SDL_MaximizeWindow", (uintptr_t)&SDL_MaximizeWindow },
	{ "SDL_RestoreWindow", (uintptr_t)&SDL_RestoreWindow },
	{ "SDL_VideoQuit", (uintptr_t)&SDL_VideoQuit },
	{ "SDL_AudioQuit", (uintptr_t)&SDL_AudioQuit },
	{ "SDL_GetSystemRAM", (uintptr_t)&SDL_GetSystemRAM },
	{ "Android_JNI_GetEnv", (uintptr_t)&Android_JNI_GetEnv },
	{ "IMG_Load", (uintptr_t)&IMG_Load_hook },
	{ "IMG_LoadTexture", (uintptr_t)&IMG_LoadTexture_hook },
	{ "IMG_LoadTexture_RW", (uintptr_t)&IMG_LoadTexture_RW },
	{ "raise", (uintptr_t)&raise },
	{ "alIsSource", (uintptr_t)&alIsSource },
	{ "alBufferData", (uintptr_t)&alBufferData },
	{ "alDeleteBuffers", (uintptr_t)&alDeleteBuffers },
	{ "alDeleteSources", (uintptr_t)&alDeleteSources },
	{ "alDistanceModel", (uintptr_t)&alDistanceModel },
	{ "alGenBuffers", (uintptr_t)&alGenBuffers },
	{ "alGenSources", (uintptr_t)&alGenSources },
	{ "alcGetCurrentContext", (uintptr_t)&alcGetCurrentContext },
	{ "alGetBufferi", (uintptr_t)&alGetBufferi },
	{ "alGetEnumValue", (uintptr_t)&alGetEnumValue },
	{ "alGetError", (uintptr_t)&alGetError },
	{ "alGetSourcei", (uintptr_t)&alGetSourcei },
	{ "alGetString", (uintptr_t)&alGetString },
	{ "alGetSourcef", (uintptr_t)&alGetSourcef },
	{ "alIsBuffer", (uintptr_t)&alIsBuffer },
	{ "alListener3f", (uintptr_t)&alListener3f },
	{ "alListenerf", (uintptr_t)&alListenerf },
	{ "alListenerfv", (uintptr_t)&alListenerfv },
	{ "alSource3f", (uintptr_t)&alSource3f },
	{ "alSourcePause", (uintptr_t)&alSourcePause },
	{ "alSourcePlay", (uintptr_t)&alSourcePlay },
	{ "alSourceQueueBuffers", (uintptr_t)&alSourceQueueBuffers },
	{ "alSourceStop", (uintptr_t)&alSourceStop },
	{ "alSourceUnqueueBuffers", (uintptr_t)&alSourceUnqueueBuffers },
	{ "alSourcef", (uintptr_t)&alSourcef },
	{ "alSourcei", (uintptr_t)&alSourcei },
	{ "alcCaptureSamples", (uintptr_t)&alcCaptureSamples },
	{ "alcCaptureStart", (uintptr_t)&alcCaptureStart },
	{ "alcCaptureStop", (uintptr_t)&alcCaptureStop },
	{ "alcCaptureOpenDevice", (uintptr_t)&alcCaptureOpenDevice },
	{ "alcCloseDevice", (uintptr_t)&alcCloseDevice },
	{ "alcCreateContext", (uintptr_t)&alcCreateContext },
	{ "alcGetContextsDevice", (uintptr_t)&alcGetContextsDevice },
	{ "alcGetError", (uintptr_t)&alcGetError },
	{ "alcGetIntegerv", (uintptr_t)&alcGetIntegerv },
	{ "alcGetString", (uintptr_t)&alcGetString },
	{ "alcMakeContextCurrent", (uintptr_t)&alcMakeContextCurrent },
	{ "alcDestroyContext", (uintptr_t)&alcDestroyContext },
	{ "alcOpenDevice", (uintptr_t)&alcOpenDevice },
	{ "alcProcessContext", (uintptr_t)&alcProcessContext },
	{ "alcPauseCurrentDevice", (uintptr_t)&ret0 },
	{ "alcResumeCurrentDevice", (uintptr_t)&ret0 },
	{ "alcSuspendContext", (uintptr_t)&alcSuspendContext },
	{ "alcIsExtensionPresent", (uintptr_t)&alcIsExtensionPresent },
	{ "alcGetProcAddress", (uintptr_t)&alcGetProcAddress },
	{ "alIsExtensionPresent", (uintptr_t)&alIsExtensionPresent },
	{ "alcSuspend", (uintptr_t)&ret0 }, // FIXME
	{ "alcResume", (uintptr_t)&ret0 }, // FIXME
	{ "alGetListenerf", (uintptr_t)&alGetListenerf },
	{ "alSourceRewind", (uintptr_t)&alSourceRewind },
	{ "mpg123_init", (uintptr_t)&mpg123_init },
	{ "mpg123_open_feed", (uintptr_t)&mpg123_open_feed },
	{ "mpg123_decode", (uintptr_t)&mpg123_decode },
	{ "mpg123_delete", (uintptr_t)&mpg123_delete },
	{ "mpg123_getformat", (uintptr_t)&mpg123_getformat },
	{ "mpg123_format_none", (uintptr_t)&mpg123_format_none },
	{ "mpg123_format", (uintptr_t)&mpg123_format },
	{ "mpg123_read", (uintptr_t)&mpg123_read },
	{ "mpg123_feed", (uintptr_t)&mpg123_feed },
	{ "mpg123_exit", (uintptr_t)&mpg123_exit },
	{ "mpg123_new", (uintptr_t)&mpg123_new },
	{ "mpg123_strerror", (uintptr_t)&mpg123_strerror },
	{ "mpg123_plain_strerror", (uintptr_t)&mpg123_plain_strerror },
	{ "mpg123_seek", (uintptr_t)&mpg123_seek },
	{ "mpg123_close", (uintptr_t)&mpg123_close },
	{ "mpg123_rates", (uintptr_t)&mpg123_rates },
	{ "mpg123_replace_reader", (uintptr_t)&mpg123_replace_reader },
	{ "mpg123_open_fd", (uintptr_t)&mpg123_open_fd },
	{ "mpg123_scan", (uintptr_t)&mpg123_scan },
	{ "mpg123_tell", (uintptr_t)&mpg123_tell },
	{ "mpg123_supported_decoders", (uintptr_t)&mpg123_supported_decoders },
	{ "mpg123_decoders", (uintptr_t)&mpg123_decoders },
	{ "vorbis_analysis", (uintptr_t)&vorbis_analysis },
	{ "vorbis_analysis_blockout", (uintptr_t)&vorbis_analysis_blockout },
	{ "vorbis_analysis_buffer", (uintptr_t)&vorbis_analysis_buffer },
	{ "vorbis_analysis_headerout", (uintptr_t)&vorbis_analysis_headerout },
	{ "vorbis_analysis_init", (uintptr_t)&vorbis_analysis_init },
	{ "vorbis_analysis_wrote", (uintptr_t)&vorbis_analysis_wrote },
	{ "vorbis_bitrate_addblock", (uintptr_t)&vorbis_bitrate_addblock },
	{ "vorbis_bitrate_flushpacket", (uintptr_t)&vorbis_bitrate_flushpacket },
	{ "vorbis_block_clear", (uintptr_t)&vorbis_block_clear },
	{ "vorbis_block_init", (uintptr_t)&vorbis_block_init },
	{ "vorbis_comment_add", (uintptr_t)&vorbis_comment_add },
	{ "vorbis_comment_add_tag", (uintptr_t)&vorbis_comment_add_tag },
	{ "vorbis_comment_clear", (uintptr_t)&vorbis_comment_clear },
	{ "vorbis_comment_init", (uintptr_t)&vorbis_comment_init },
	{ "vorbis_comment_query", (uintptr_t)&vorbis_comment_query },
	{ "vorbis_comment_query_count", (uintptr_t)&vorbis_comment_query_count },
	{ "vorbis_commentheader_out", (uintptr_t)&vorbis_commentheader_out },
	{ "vorbis_dsp_clear", (uintptr_t)&vorbis_dsp_clear },
	{ "vorbis_info_blocksize", (uintptr_t)&vorbis_info_blocksize },
	{ "vorbis_info_clear", (uintptr_t)&vorbis_info_clear },
	{ "vorbis_info_init", (uintptr_t)&vorbis_info_init },
	{ "vorbis_packet_blocksize", (uintptr_t)&vorbis_packet_blocksize },
	{ "vorbis_synthesis", (uintptr_t)&vorbis_synthesis },
	{ "vorbis_synthesis_blockin", (uintptr_t)&vorbis_synthesis_blockin },
	{ "vorbis_synthesis_headerin", (uintptr_t)&vorbis_synthesis_headerin },
	{ "vorbis_synthesis_init", (uintptr_t)&vorbis_synthesis_init },
	{ "vorbis_synthesis_pcmout", (uintptr_t)&vorbis_synthesis_pcmout },
	{ "vorbis_synthesis_read", (uintptr_t)&vorbis_synthesis_read },
	{ "vorbis_synthesis_trackonly", (uintptr_t)&vorbis_synthesis_trackonly },
	{ "oggpack_writetrunc", (uintptr_t)&oggpack_writetrunc },
	{ "oggpack_writeinit", (uintptr_t)&oggpack_writeinit },
	{ "oggpack_writecopy", (uintptr_t)&oggpack_writecopy },
	{ "oggpack_writeclear", (uintptr_t)&oggpack_writeclear },
	{ "oggpack_writealign", (uintptr_t)&oggpack_writealign },
	{ "oggpack_write", (uintptr_t)&oggpack_write },
	{ "oggpack_reset", (uintptr_t)&oggpack_reset },
	{ "oggpack_readinit", (uintptr_t)&oggpack_readinit },
	{ "oggpack_read1", (uintptr_t)&oggpack_read1 },
	{ "oggpack_read", (uintptr_t)&oggpack_read },
	{ "oggpack_look1", (uintptr_t)&oggpack_look1 },
	{ "oggpack_look", (uintptr_t)&oggpack_look },
	{ "oggpack_get_buffer", (uintptr_t)&oggpack_get_buffer },
	{ "oggpack_bytes", (uintptr_t)&oggpack_bytes },
	{ "oggpack_bits", (uintptr_t)&oggpack_bits },
	{ "oggpack_adv1", (uintptr_t)&oggpack_adv1 },
	{ "oggpack_adv", (uintptr_t)&oggpack_adv },
	{ "ogg_sync_wrote", (uintptr_t)&ogg_sync_wrote },
	{ "ogg_sync_reset", (uintptr_t)&ogg_sync_reset },
	{ "ogg_sync_pageseek", (uintptr_t)&ogg_sync_pageseek },
	{ "ogg_sync_pageout", (uintptr_t)&ogg_sync_pageout },
	{ "ogg_sync_init", (uintptr_t)&ogg_sync_init },
	{ "ogg_sync_destroy", (uintptr_t)&ogg_sync_destroy },
	{ "ogg_sync_clear", (uintptr_t)&ogg_sync_clear },
	{ "ogg_sync_buffer", (uintptr_t)&ogg_sync_buffer },
	{ "ogg_stream_reset_serialno", (uintptr_t)&ogg_stream_reset_serialno },
	{ "ogg_stream_reset", (uintptr_t)&ogg_stream_reset },
	{ "ogg_stream_pageout", (uintptr_t)&ogg_stream_pageout },
	{ "ogg_stream_pagein", (uintptr_t)&ogg_stream_pagein },
	{ "ogg_stream_packetpeek", (uintptr_t)&ogg_stream_packetpeek },
	{ "ogg_stream_packetout", (uintptr_t)&ogg_stream_packetout },
	{ "ogg_stream_packetin", (uintptr_t)&ogg_stream_packetin },
	{ "ogg_stream_init", (uintptr_t)&ogg_stream_init },
	{ "ogg_stream_flush", (uintptr_t)&ogg_stream_flush },
	{ "ogg_stream_eos", (uintptr_t)&ogg_stream_eos },
	{ "ogg_stream_destroy", (uintptr_t)&ogg_stream_destroy },
	{ "ogg_stream_clear", (uintptr_t)&ogg_stream_clear },
	{ "ogg_page_version", (uintptr_t)&ogg_page_version },
	{ "ogg_page_serialno", (uintptr_t)&ogg_page_serialno },
	{ "ogg_page_pageno", (uintptr_t)&ogg_page_pageno },
	{ "ogg_page_packets", (uintptr_t)&ogg_page_packets },
	{ "ogg_page_granulepos", (uintptr_t)&ogg_page_granulepos },
	{ "ogg_page_eos", (uintptr_t)&ogg_page_eos },
	{ "ogg_page_continued", (uintptr_t)&ogg_page_continued },
	{ "ogg_page_checksum_set", (uintptr_t)&ogg_page_checksum_set },
	{ "ogg_page_bos", (uintptr_t)&ogg_page_bos },
	{ "ogg_packet_clear", (uintptr_t)&ogg_packet_clear },
	{ "ov_read", (uintptr_t)&ov_read },
	{ "ov_clear", (uintptr_t)&ov_clear },
	{ "ov_open_callbacks", (uintptr_t)&ov_open_callbacks },
	{ "ov_info", (uintptr_t)&ov_info },
	{ "ov_seekable", (uintptr_t)&ov_seekable },
	{ "ov_time_total", (uintptr_t)&ov_time_total },
	{ "ov_time_seek", (uintptr_t)&ov_time_seek },
	{ "curl_global_init", (uintptr_t)&curl_global_init },
	{ "curl_easy_init", (uintptr_t)&curl_easy_init },
	{ "curl_easy_setopt", (uintptr_t)&curl_easy_setopt },
	{ "curl_easy_perform", (uintptr_t)&curl_easy_perform },
	{ "curl_easy_strerror", (uintptr_t)&curl_easy_strerror },
	{ "curl_easy_getinfo", (uintptr_t)&curl_easy_getinfo },
	{ "curl_easy_cleanup", (uintptr_t)&curl_easy_cleanup },
	{ "curl_global_cleanup", (uintptr_t)&curl_global_cleanup },
	{ "BlurrrPlatformAndroid_GetJNIEnv", (uintptr_t)&Android_JNI_GetEnv },
	{ "ALmixer_FreeData", (uintptr_t)&ALmixer_FreeData },
	{ "ALmixer_LoadAll_RW", (uintptr_t)&ALmixer_LoadAll_RW },
	{ "ALmixer_GetError", (uintptr_t)&ALmixer_GetError },
	{ "ALmixer_LoadStream", (uintptr_t)&ALmixer_LoadStream },
	{ "ALmixer_PlayChannel", (uintptr_t)&ALmixer_PlayChannel },
	{ "ALmixer_HaltChannel", (uintptr_t)&ALmixer_HaltChannel },
	{ "ALmixer_PauseChannel", (uintptr_t)&ALmixer_PauseChannel },
	{ "ALmixer_ResumeChannel", (uintptr_t)&ALmixer_ResumeChannel },
	{ "ALmixer_GetTotalTime", (uintptr_t)&ALmixer_GetTotalTime },
	{ "ALmixer_SeekChannel", (uintptr_t)&ALmixer_SeekChannel },
	{ "ALmixer_SetVolumeChannel", (uintptr_t)&ALmixer_SetVolumeChannel },
	{ "ALmixer_GetVolumeChannel", (uintptr_t)&ALmixer_GetVolumeChannel },
	{ "ALmixer_IsPlayingChannel", (uintptr_t)&ALmixer_IsPlayingChannel },
	{ "ALmixer_IsPausedChannel", (uintptr_t)&ALmixer_IsPausedChannel },
	{ "ALmixer_Init", (uintptr_t)&ALmixer_Init },
	{ "ALmixer_GetFrequency", (uintptr_t)&ALmixer_GetFrequency },
	{ "ALmixer_CountTotalChannels", (uintptr_t)&ALmixer_CountTotalChannels },
	{ "ALmixer_CompiledWithThreadBackend", (uintptr_t)&ALmixer_CompiledWithThreadBackend },
	{ "ALmixer_Quit", (uintptr_t)&ALmixer_Quit },
	{ "ALmixer_EndInterruption", (uintptr_t)&ALmixer_EndInterruption },
	{ "ALmixer_BeginInterruption", (uintptr_t)&ALmixer_BeginInterruption },
	{ "ALmixer_CountUnreservedFreeChannels", (uintptr_t)&ALmixer_CountUnreservedFreeChannels },
	{ "ALmixer_CountUnreservedUsedChannels", (uintptr_t)&ALmixer_CountUnreservedUsedChannels },
};
static size_t numhooks = sizeof(default_dynlib) / sizeof(*default_dynlib);

int check_kubridge(void) {
	int search_unk[2];
	return _vshKernelSearchModuleByName("kubridge", search_unk);
}

enum MethodIDs {
	UNKNOWN = 0,
	INIT,
} MethodIDs;

typedef struct {
	char *name;
	enum MethodIDs id;
} NameToMethodID;

static NameToMethodID name_to_method_ids[] = {
	{ "<init>", INIT },
};

int GetMethodID(void *env, void *class, const char *name, const char *sig) {
	printf("GetMethodID: %s\n", name);

	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0) {
			return name_to_method_ids[i].id;
		}
	}

	return UNKNOWN;
}

int GetStaticMethodID(void *env, void *class, const char *name, const char *sig) {
	printf("GetStaticMethodID: %s\n", name);
	
	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0)
			return name_to_method_ids[i].id;
	}

	return UNKNOWN;
}

void CallStaticVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
}

int CallStaticBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

int CallStaticIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;	
	}
}

int64_t CallStaticLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;	
	}
}

uint64_t CallLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return -1;
}

void *FindClass(void) {
	return (void *)0x41414141;
}

void *NewGlobalRef(void *env, char *str) {
	return (void *)0x42424242;
}

void DeleteGlobalRef(void *env, char *str) {
}

void *NewObjectV(void *env, void *clazz, int methodID, uintptr_t args) {
	return (void *)0x43434343;
}

void *GetObjectClass(void *env, void *obj) {
	return (void *)0x44444444;
}

char *NewStringUTF(void *env, char *bytes) {
	return bytes;
}

char *GetStringUTFChars(void *env, char *string, int *isCopy) {
	return string;
}

size_t GetStringUTFLength(void *env, char *string) {
	return strlen(string);	
}

int GetJavaVM(void *env, void **vm) {
	*vm = fake_vm;
	return 0;
}

int GetFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

int GetBooleanField(void *env, void *obj, int fieldID) {
	return 0;
}

void *CallObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return NULL;
	}
}

int CallBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

void CallVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		break;
	}
}

int GetStaticFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

void *GetStaticObjectField(void *env, void *clazz, int fieldID) {
	switch (fieldID) {
	default:
		return NULL;
	}
}

void GetStringUTFRegion(void *env, char *str, size_t start, size_t len, char *buf) {
	sceClibMemcpy(buf, &str[start], len);
	buf[len] = 0;
}

void *CallStaticObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return NULL;
}

int GetIntField(void *env, void *obj, int fieldID) { return 0; }

float GetFloatField(void *env, void *obj, int fieldID) {
	switch (fieldID) {
	default:
		return 0.0f;
	}
}

float CallStaticFloatMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		if (methodID != UNKNOWN) {
			dlog("CallStaticDoubleMethodV(%d)\n", methodID);
		}
		return 0;
	}
}

/*int crasher(unsigned int argc, void *argv) {
	uint32_t *nullptr = NULL;
	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_SELECT) *nullptr = 0;
		sceKernelDelayThread(100);
	}
}*/

/*void abort_handler(KuKernelAbortContext *ctx) {
	printf("Crash Detected!!! (Abort Type: 0x%08X)\n", ctx->abortType);
	printf("-----------------\n");
	printf("PC: 0x%08X\n", ctx->pc);
	printf("LR: 0x%08X\n", ctx->lr);
	printf("SP: 0x%08X\n", ctx->sp);
	printf("-----------------\n");
	printf("REGISTERS:\n");
	uint32_t *registers = (uint32_t *)ctx;
	for (int i = 0; i < 13; i++) {
		printf("R%d: 0x%08X\n", i, registers[i]);		
	}
	printf("-----------------\n");
	printf("VFP REGISTERS:\n");
	for (int i = 0; i < 32; i++) {
		printf("D%d: 0x%016llX\n", i, ctx->vfpRegisters[i]);		
	}
	printf("-----------------\n");
	printf("SPSR: 0x%08X\n", ctx->SPSR);
	printf("FPSCR: 0x%08X\n", ctx->FPSCR);
	printf("FPEXC: 0x%08X\n", ctx->FPEXC);
	printf("FSR: 0x%08X\n", ctx->FSR);
	printf("FAR: 0x%08X\n", *(&(ctx->FSR) + 4)); // Using ctx->FAR gives an error for some weird reason
	sceKernelExitProcess(0);
}*/

int main(int argc, char *argv[]) {
	//kuKernelRegisterAbortHandler(abort_handler, NULL);
	//SceUID crasher_thread = sceKernelCreateThread("crasher", crasher, 0x40, 0x1000, 0, 0, NULL);
	//sceKernelStartThread(crasher_thread, 0, NULL);	
	//sceSysmoduleLoadModule(SCE_SYSMODULE_RAZOR_CAPTURE);
	
	SceAppUtilInitParam init_param;
	SceAppUtilBootParam boot_param;
	memset(&init_param, 0, sizeof(SceAppUtilInitParam));
	memset(&boot_param, 0, sizeof(SceAppUtilBootParam));
	sceAppUtilInit(&init_param, &boot_param);

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);

	if (check_kubridge() < 0)
		fatal_error("Error: kubridge.skprx is not installed.");

	if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:/data/external/libshacccg.suprx"))
		fatal_error("Error: libshacccg.suprx is not installed.");
	
	loadConfig();
	
	// Generating selected game path
	char fname[256];
	int8_t game_idx = 2;
	FILE *f = fopen("ux0:data/isotope.tmp", "rb");
	if (f) {
		fread(&game_idx, 1, 1, f);
		fclose(f);
		sceIoRemove("ux0:data/isotope.tmp");
	}
	sprintf(DATA_PATH, "ux0:data/isotope/game%d", game_idx);
	
	printf("Loading libMySDLMainActivity\n");
	sprintf(fname, "%s/libMySDLMainActivity.so", DATA_PATH);
	if (so_file_load(&isotope_mod, fname, LOAD_ADDRESS) < 0)
		fatal_error("Error could not load %s.", fname);
	so_relocate(&isotope_mod);
	so_resolve(&isotope_mod, default_dynlib, sizeof(default_dynlib), 0);
	
	patch_game();
	so_flush_caches(&isotope_mod);
	so_initialize(&isotope_mod);
	
	vglUseCachedMem(GL_TRUE);
	vglInitExtended(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
	
	memset(fake_vm, 'A', sizeof(fake_vm));
	*(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm; // just point to itself...
	*(uintptr_t *)(fake_vm + 0x10) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x14) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x18) = (uintptr_t)GetEnv;

	memset(fake_env, 'A', sizeof(fake_env));
	*(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env; // just point to itself...
	*(uintptr_t *)(fake_env + 0x18) = (uintptr_t)FindClass;
	*(uintptr_t *)(fake_env + 0x3C) = (uintptr_t)ret0; // ExceptionOccurred
	*(uintptr_t *)(fake_env + 0x4C) = (uintptr_t)ret0;// PushLocalFrame
	*(uintptr_t *)(fake_env + 0x54) = (uintptr_t)NewGlobalRef;
	*(uintptr_t *)(fake_env + 0x58) = (uintptr_t)DeleteGlobalRef;
	*(uintptr_t *)(fake_env + 0x5C) = (uintptr_t)ret0; // DeleteLocalRef
	*(uintptr_t *)(fake_env + 0x74) = (uintptr_t)NewObjectV;
	*(uintptr_t *)(fake_env + 0x7C) = (uintptr_t)GetObjectClass;
	*(uintptr_t *)(fake_env + 0x80) = (uintptr_t)ret1; // IsInstanceOf
	*(uintptr_t *)(fake_env + 0x84) = (uintptr_t)GetMethodID;
	*(uintptr_t *)(fake_env + 0x8C) = (uintptr_t)CallObjectMethodV;
	*(uintptr_t *)(fake_env + 0x98) = (uintptr_t)CallBooleanMethodV;
	*(uintptr_t *)(fake_env + 0xD4) = (uintptr_t)CallLongMethodV;
	*(uintptr_t *)(fake_env + 0xF8) = (uintptr_t)CallVoidMethodV;
	*(uintptr_t *)(fake_env + 0x178) = (uintptr_t)GetFieldID;
	*(uintptr_t *)(fake_env + 0x17C) = (uintptr_t)GetBooleanField;
	*(uintptr_t *)(fake_env + 0x190) = (uintptr_t)GetIntField;
	*(uintptr_t *)(fake_env + 0x198) = (uintptr_t)GetFloatField;
	*(uintptr_t *)(fake_env + 0x1C4) = (uintptr_t)GetStaticMethodID;
	*(uintptr_t *)(fake_env + 0x1CC) = (uintptr_t)CallStaticObjectMethodV;
	*(uintptr_t *)(fake_env + 0x1D8) = (uintptr_t)CallStaticBooleanMethodV;
	*(uintptr_t *)(fake_env + 0x208) = (uintptr_t)CallStaticIntMethodV;
	*(uintptr_t *)(fake_env + 0x21C) = (uintptr_t)CallStaticLongMethodV;
	*(uintptr_t *)(fake_env + 0x220) = (uintptr_t)CallStaticFloatMethodV;
	*(uintptr_t *)(fake_env + 0x238) = (uintptr_t)CallStaticVoidMethodV;
	*(uintptr_t *)(fake_env + 0x240) = (uintptr_t)GetStaticFieldID;
	*(uintptr_t *)(fake_env + 0x244) = (uintptr_t)GetStaticObjectField;
	*(uintptr_t *)(fake_env + 0x29C) = (uintptr_t)NewStringUTF;
	*(uintptr_t *)(fake_env + 0x2A0) = (uintptr_t)GetStringUTFLength;
	*(uintptr_t *)(fake_env + 0x2A0) = (uintptr_t)GetStringUTFLength;
	*(uintptr_t *)(fake_env + 0x2A4) = (uintptr_t)GetStringUTFChars;
	*(uintptr_t *)(fake_env + 0x2A8) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_env + 0x36C) = (uintptr_t)GetJavaVM;
	*(uintptr_t *)(fake_env + 0x374) = (uintptr_t)GetStringUTFRegion;
	
	// Disabling rearpad
	SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);
	
	int (* SDL_main)(void) = (void *) so_symbol(&isotope_mod, "SDL_main");
    SDL_main();
	
	return 0;
}
