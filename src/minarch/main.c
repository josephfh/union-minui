#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <SDL/SDL.h>
#include <SDL/SDL_image.h>
#include <SDL/SDL_ttf.h>

#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <libgen.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#include "libretro.h"
#include "defines.h"
#include "utils.h"
#include "api.h"
#include "scaler_neon.h"

static SDL_Surface* screen;

///////////////////////////////////////

static struct Game {
	char path[MAX_PATH];
	char name[MAX_PATH]; // TODO: rename to basename?
	void* data;
	size_t size;
} game;
static void Game_open(char* path) {
	strcpy((char*)game.path, path);
	strcpy((char*)game.name, strrchr(path, '/')+1);
		
	FILE *file = fopen(game.path, "r");
	if (file==NULL) {
		LOG_error("Error opening game: %s\n\t%s\n", game.path, strerror(errno));
		return;
	}
	
	fseek(file, 0, SEEK_END);
	game.size = ftell(file);
	
	rewind(file);
	game.data = malloc(game.size);
	fread(game.data, sizeof(uint8_t), game.size, file);
	
	fclose(file);
}
static void Game_close(void) {
	free(game.data);
}

///////////////////////////////

static struct Core {
	int initialized;
	
	const char tag[8]; // eg. GBC
	const char name[128]; // eg. gambatte
	const char version[128]; // eg. Gambatte (v0.5.0-netlink 7e02df6)
	const char sys_dir[MAX_PATH]; // eg. /mnt/sdcard/.userdata/rg35xx/GB-gambatte
	
	double fps;
	double sample_rate;
	
	void* handle;
	void (*init)(void);
	void (*deinit)(void);
	
	void (*get_system_info)(struct retro_system_info *info);
	void (*get_system_av_info)(struct retro_system_av_info *info);
	void (*set_controller_port_device)(unsigned port, unsigned device);
	
	void (*reset)(void);
	void (*run)(void);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *data, size_t size);
	bool (*unserialize)(const void *data, size_t size);
	bool (*load_game)(const struct retro_game_info *game);
	bool (*load_game_special)(unsigned game_type, const struct retro_game_info *info, size_t num_info);
	void (*unload_game)(void);
	unsigned (*get_region)(void);
	void *(*get_memory_data)(unsigned id);
	size_t (*get_memory_size)(unsigned id);
	retro_audio_buffer_status_callback_t audio_buffer_status;
} core;

///////////////////////////////////////
// saves and states

static void SRAM_getPath(char* filename) {
	sprintf(filename, SDCARD_PATH "/Saves/%s/%s.sav", core.tag, game.name);
}

static void SRAM_read(void) {
	size_t sram_size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size) return;
	
	char filename[MAX_PATH];
	SRAM_getPath(filename);
	printf("sav path (read): %s\n", filename);
	
	FILE *sram_file = fopen(filename, "r");
	if (!sram_file) return;

	void* sram = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);

	if (!sram || !fread(sram, 1, sram_size, sram_file)) {
		LOG_error("Error reading SRAM data\n");
	}

	fclose(sram_file);
}
static void SRAM_write(void) {
	size_t sram_size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size) return;
	
	char filename[MAX_PATH];
	SRAM_getPath(filename);
	printf("sav path (write): %s\n", filename);
		
	FILE *sram_file = fopen(filename, "w");
	if (!sram_file) {
		LOG_error("Error opening SRAM file: %s\n", strerror(errno));
		return;
	}

	void *sram = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);

	if (!sram || sram_size != fwrite(sram, 1, sram_size, sram_file)) {
		LOG_error("Error writing SRAM data to file\n");
	}

	fclose(sram_file);

	sync();
}

static int state_slot = 0;
static void State_getPath(char* filename) {
	sprintf(filename, SDCARD_PATH "/.userdata/" PLATFORM "/%s-%s/%s.st%i", core.tag, core.name, game.name, state_slot);
}

static void State_read(void) { // from picoarch
	size_t state_size = core.serialize_size();
	if (!state_size) return;

	void *state = calloc(1, state_size);
	if (!state) {
		LOG_error("Couldn't allocate memory for state\n");
		goto error;
	}

	char filename[MAX_PATH];
	State_getPath(filename);
	
	FILE *state_file = fopen(filename, "r");
	if (!state_file) {
		if (state_slot!=8) { // st8 is a default state in MiniUI and may not exist, that's okay
			LOG_error("Error opening state file: %s (%s)\n", filename, strerror(errno));
		}
		goto error;
	}

	if (state_size != fread(state, 1, state_size, state_file)) {
		LOG_error("Error reading state data from file: %s (%s)\n", filename, strerror(errno));
		goto error;
	}

	if (!core.unserialize(state, state_size)) {
		LOG_error("Error restoring save state: %s (%s)\n", filename, strerror(errno));
		goto error;
	}

error:
	if (state) free(state);
	if (state_file) fclose(state_file);
}
static void State_write(void) { // from picoarch
	size_t state_size = core.serialize_size();
	if (!state_size) return;

	void *state = calloc(1, state_size);
	if (!state) {
		LOG_error("Couldn't allocate memory for state\n");
		goto error;
	}

	char filename[MAX_PATH];
	State_getPath(filename);
	
	FILE *state_file = fopen(filename, "w");
	if (!state_file) {
		LOG_error("Error opening state file: %s (%s)\n", filename, strerror(errno));
		goto error;
	}

	if (!core.serialize(state, state_size)) {
		LOG_error("Error creating save state: %s (%s)\n", filename, strerror(errno));
		goto error;
	}

	if (state_size != fwrite(state, 1, state_size, state_file)) {
		LOG_error("Error writing state data to file: %s (%s)\n", filename, strerror(errno));
		goto error;
	}

error:
	if (state) free(state);
	if (state_file) fclose(state_file);

	sync();
}

///////////////////////////////

// callbacks
static struct retro_disk_control_ext_callback disk_control_ext;

// TODO: tmp, naive options
static struct {
	char key[128];
	char value[128];
} tmp_options[128];
static bool environment_callback(unsigned cmd, void *data) { // copied from picoarch initially
	// printf("environment_callback: %i\n", cmd); fflush(stdout);
	
	switch(cmd) {
	case RETRO_ENVIRONMENT_GET_OVERSCAN: { /* 2 */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: { /* 3 */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_SET_MESSAGE: { /* 6 */
		const struct retro_message *message = (const struct retro_message*)data;
		if (message) LOG_info("%s\n", message->msg);
		break;
	}
	// TODO: RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL 8
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: { /* 9 */
		const char **out = (const char **)data;
		if (out)
			*out = core.sys_dir;
		
		break;
	}
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: { /* 10 */
		const enum retro_pixel_format *format = (enum retro_pixel_format *)data;

		if (*format != RETRO_PIXEL_FORMAT_RGB565) { // TODO: pull from platform.h?
			/* 565 is only supported format */
			return false;
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: { /* 11 */
		const struct retro_input_descriptor *vars = (const struct retro_input_descriptor *)data;
		if (vars) {
			// TODO: create an array of char* description indexed by id
			for (int i=0; vars[i].description; i++) {
				// vars[i].id == RETRO_DEVICE_ID_JOYPAD_*, vars[i].description = name
				printf("%i %s\n", vars[i].id, vars[i].description);
			}
			return false;
		}
	} break;
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: { /* 13 */
		const struct retro_disk_control_callback *var =
			(const struct retro_disk_control_callback *)data;

		if (var) {
			memset(&disk_control_ext, 0, sizeof(struct retro_disk_control_ext_callback));
			memcpy(&disk_control_ext, var, sizeof(struct retro_disk_control_callback));
		}
		break;
	}
	// TODO: this is called whether using variables or options
	case RETRO_ENVIRONMENT_GET_VARIABLE: { /* 15 */
		struct retro_variable *var = (struct retro_variable *)data;
		if (var && var->key) {
			printf("get key: %s\n", var->key);
			for (int i=0; i<128; i++) {
				if (!strcmp(tmp_options[i].key, var->key)) {
					var->value = tmp_options[i].value;
					break;
				}
			}
			// var->value = options_get_value(var->key);
		}
		break;
	}
	// TODO: I think this is where the core reports its variables (the precursor to options)
	// TODO: this is called if RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION sets out to 0
	case RETRO_ENVIRONMENT_SET_VARIABLES: { /* 16 */
		const struct retro_variable *vars = (const struct retro_variable *)data;
		// options_free();
		if (vars) {
			// options_init_variables(vars);
			// load_config();
			
			for (int i=0; vars[i].key; i++) {
				// value appears to be NAME; DEFAULT|VALUE|VALUE|ETC
				printf("set var key: %s to value: %s\n", vars[i].key, vars[i].value);
			}
		}
		break;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: { /* 17 */
		bool *out = (bool *)data;
		if (out)
			*out = false; // options_changed();
		break;
	}
	// case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: { /* 23 */
	//         struct retro_rumble_interface *iface =
	//            (struct retro_rumble_interface*)data;
	//
	//         PA_INFO("Setup rumble interface.\n");
	//         iface->set_rumble_state = pa_set_rumble_state;
	// 	break;
	// }
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: { /* 27 */
		struct retro_log_callback *log_cb = (struct retro_log_callback *)data;
		if (log_cb)
			log_cb->log = (void (*)(enum retro_log_level, const char*, ...))LOG_note; // same difference
		break;
	}
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: { /* 31 */
		const char **out = (const char **)data;
		if (out)
			*out = NULL; // save_dir;
		break;
	}
	// RETRO_ENVIRONMENT_GET_LANGUAGE 39
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: { /* 52 */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: { /* 52 */
		unsigned *out = (unsigned *)data;
		if (out)
			*out = 1;
		break;
	}
	// TODO: options and variables are separate concepts use for the same thing...I think.
	// TODO: not used by gambatte
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: { /* 53 */
		puts("RETRO_ENVIRONMENT_SET_CORE_OPTIONS");
		// options_free();
		if (data) {
			// options_init(*(const struct retro_core_option_definition **)data);
			// load_config();
			
			const struct retro_core_option_definition *vars = *(const struct retro_core_option_definition **)data;
			for (int i=0; vars[i].key; i++) {
				const struct retro_core_option_definition *var = &vars[i];
				// printf("set key: %s to value: %s (%s)\n", var->key, var->default_value, var->desc);
				printf("set option key: %s to value: %s\n", var->key, var->default_value);
			}
		}
		break;
	}
	
	// TODO: used by gambatte, fceumm (probably others)
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: { /* 54 */
		puts("RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL");
		
		const struct retro_core_options_intl *options = (const struct retro_core_options_intl *)data;
		
		if (options && options->us) {
			// options_free();
			// options_init(options->us);
			// load_config();
			
			const struct retro_core_option_definition *vars = options->us;
			for (int i=0; vars[i].key; i++) {
				const struct retro_core_option_definition *var = &vars[i];
				// printf("set key: %s to value: %s (%s)\n", var->key, var->default_value, var->desc);
				char *default_value = (char*)var->default_value;
				if (!strcmp("gpsp_save_method", var->key)) {
					default_value = "libretro"; // TODO: tmp, patch or override gpsp
				}
				printf("set core (intl) key: %s to value: %s\n", var->key, default_value);
				strcpy(tmp_options[i].key, var->key);
				strcpy(tmp_options[i].value, default_value);
			}
		}
		break;
	}
	// TODO: not used by gambatte
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: { /* 55 */
		puts("RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY");
		
		const struct retro_core_option_display *display =
			(const struct retro_core_option_display *)data;

		if (display)
			printf("visible: %i (%s)\n", display->visible, display->key);
			// options_set_visible(display->key, display->visible);
		break;
	}
	case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION: { /* 57 */
		unsigned *out =	(unsigned *)data;
		if (out)
			*out = 1;
		break;
	}
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE: { /* 58 */
		const struct retro_disk_control_ext_callback *var =
			(const struct retro_disk_control_ext_callback *)data;

		if (var) {
			memcpy(&disk_control_ext, var, sizeof(struct retro_disk_control_ext_callback));
		}
		break;
	}
	// TODO: RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION 59
	case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: { /* 62 */
		const struct retro_audio_buffer_status_callback *cb =
			(const struct retro_audio_buffer_status_callback *)data;
		if (cb) {
			core.audio_buffer_status = cb->callback;
		} else {
			core.audio_buffer_status = NULL;
		}
		break;
	}
	// TODO: not used by gambatte
	case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY: { /* 63 */
		puts("RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY");
		
		const unsigned *latency_ms = (const unsigned *)data;
		if (latency_ms) {
			unsigned frames = *latency_ms * core.fps / 1000;
			if (frames < 30)
				// audio_buffer_size_override = frames;
				printf("audio_buffer_size_override = %i\n", frames);
			// else
			// 	PA_WARN("Audio buffer change out of range (%d), ignored\n", frames);
		}
		break;
	}
	
	// TODO: RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE 64
	// TODO: RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK 69
	// TODO: UNKNOWN 70
	// TODO: UNKNOWN 65572
	// TODO: UNKNOWN 65578
	// TODO: UNKNOWN 65581
	// TODO: UNKNOWN 65587
	default:
		LOG_debug("Unsupported environment cmd: %u\n", cmd);
		return false;
	}

	return true;
}

///////////////////////////////

// from gambatte-dms
//from RGB565
#define cR(A) (((A) & 0xf800) >> 11)
#define cG(A) (((A) & 0x7e0) >> 5)
#define cB(A) ((A) & 0x1f)
//to RGB565
#define Weight2_3(A, B)  (((((cR(A) << 1) + (cR(B) * 3)) / 5) & 0x1f) << 11 | ((((cG(A) << 1) + (cG(B) * 3)) / 5) & 0x3f) << 5 | ((((cB(A) << 1) + (cB(B) * 3)) / 5) & 0x1f))
#define Weight3_2(A, B)  (((((cR(B) << 1) + (cR(A) * 3)) / 5) & 0x1f) << 11 | ((((cG(B) << 1) + (cG(A) * 3)) / 5) & 0x3f) << 5 | ((((cB(B) << 1) + (cB(A) * 3)) / 5) & 0x1f))

static int cpu_ticks = 0;
static int fps_ticks = 0;
static int sec_start = 0;

// TODO: flesh out
static void scale1x(int w, int h, int pitch, const void *src, void *dst) {
	// pitch of src image not src buffer! 
	// eg. gb has a 160 pixel wide image but 
	// gambatte uses a 256 pixel wide buffer
	// (only matters when using memcpy) 
	int src_pitch = w * SCREEN_BPP; 
	int src_stride = pitch / SCREEN_BPP;
	int dst_stride = SCREEN_PITCH / SCREEN_BPP;
	int cpy_pitch = MIN(src_pitch, SCREEN_PITCH);
	
	uint16_t* restrict src_row = (uint16_t*)src;
	uint16_t* restrict dst_row = (uint16_t*)dst;
	for (int y=0; y<h; y++) {
		memcpy(dst_row, src_row, cpy_pitch);
		dst_row += dst_stride;
		src_row += src_stride;
	}
	
}
static void scale2x(int w, int h, int pitch, const void *src, void *dst) {
	for (unsigned y = 0; y < h; y++) {
		uint16_t* restrict src_row = (void*)src + y * pitch;
		uint16_t* restrict dst_row = (void*)dst + y * SCREEN_PITCH * 2;
		for (unsigned x = 0; x < w; x++) {
			uint16_t s = *src_row;
			
			// row 1
			*(dst_row     ) = s;
			*(dst_row + 1 ) = s;
			
			// row 2
			*(dst_row + SCREEN_WIDTH    ) = s;
			*(dst_row + SCREEN_WIDTH + 1) = s;
			
			src_row += 1;
			dst_row += 2;
		}
	}
}
static void scale3x(int w, int h, int pitch, const void *src, void *dst) {
	int row3 = SCREEN_WIDTH * 2;
	for (unsigned y = 0; y < h; y++) {
		uint16_t* restrict src_row = (void*)src + y * pitch;
		uint16_t* restrict dst_row = (void*)dst + y * SCREEN_PITCH * 3;
		for (unsigned x = 0; x < w; x++) {
			uint16_t s = *src_row;
			
			// row 1
			*(dst_row    ) = s;
			*(dst_row + 1) = s;
			*(dst_row + 2) = s;
			
			// row 2
			*(dst_row + SCREEN_WIDTH    ) = s;
			*(dst_row + SCREEN_WIDTH + 1) = s;
			*(dst_row + SCREEN_WIDTH + 2) = s;

			// row 3
			*(dst_row + row3    ) = s;
			*(dst_row + row3 + 1) = s;
			*(dst_row + row3 + 2) = s;

			src_row += 1;
			dst_row += 3;
		}
	}
}
static void scale3x_lcd(int w, int h, int pitch, const void *src, void *dst) {
	uint16_t k = 0x0000;
	int row3 = SCREEN_WIDTH * 2;
	for (unsigned y = 0; y < h; y++) {
		uint16_t* restrict src_row = (void*)src + y * pitch;
		uint16_t* restrict dst_row = (void*)dst + y * SCREEN_PITCH * 3;
		for (unsigned x = 0; x < w; x++) {
			uint16_t s = *src_row;
            uint16_t r = (s & 0b1111100000000000);
            uint16_t g = (s & 0b0000011111100000);
            uint16_t b = (s & 0b0000000000011111);
			
			// row 1
			*(dst_row    ) = k;
			*(dst_row + 1) = g;
			*(dst_row + 2) = k;
			
			// row 2
			*(dst_row + SCREEN_WIDTH    ) = r;
			*(dst_row + SCREEN_WIDTH + 1) = g;
			*(dst_row + SCREEN_WIDTH + 2) = b;

			// row 3
			*(dst_row + row3    ) = r;
			*(dst_row + row3 + 1) = k;
			*(dst_row + row3 + 2) = b;

			src_row += 1;
			dst_row += 3;
		}
	}
}
static void scale3x_dmg(int w, int h, int pitch, const void *src, void *dst) {
	uint16_t g = 0xffff;
	int row3 = SCREEN_WIDTH * 2;
	for (unsigned y = 0; y < h; y++) {
		uint16_t* restrict src_row = (void*)src + y * pitch;
		uint16_t* restrict dst_row = (void*)dst + y * SCREEN_PITCH * 3;
		for (unsigned x = 0; x < w; x++) {
			uint16_t a = *src_row;
            uint16_t b = Weight3_2( a, g);
            uint16_t c = Weight2_3( a, g);
			
			// row 1
			*(dst_row    ) = b;
			*(dst_row + 1) = a;
			*(dst_row + 2) = a;
			
			// row 2
			*(dst_row + SCREEN_WIDTH    ) = b;
			*(dst_row + SCREEN_WIDTH + 1) = a;
			*(dst_row + SCREEN_WIDTH + 2) = a;

			// row 3
			*(dst_row + row3    ) = c;
			*(dst_row + row3 + 1) = b;
			*(dst_row + row3 + 2) = b;

			src_row += 1;
			dst_row += 3;
		}
	}
}
static void scale4x(int w, int h, int pitch, const void *src, void *dst) {
	int row3 = SCREEN_WIDTH * 2;
	int row4 = SCREEN_WIDTH * 3;
	for (unsigned y = 0; y < h; y++) {
		uint16_t* restrict src_row = (void*)src + y * pitch;
		uint16_t* restrict dst_row = (void*)dst + y * SCREEN_PITCH * 4;
		for (unsigned x = 0; x < w; x++) {
			uint16_t s = *src_row;
			
			// row 1
			*(dst_row    ) = s;
			*(dst_row + 1) = s;
			*(dst_row + 2) = s;
			*(dst_row + 3) = s;
			
			// row 2
			*(dst_row + SCREEN_WIDTH    ) = s;
			*(dst_row + SCREEN_WIDTH + 1) = s;
			*(dst_row + SCREEN_WIDTH + 2) = s;
			*(dst_row + SCREEN_WIDTH + 3) = s;

			// row 3
			*(dst_row + row3    ) = s;
			*(dst_row + row3 + 1) = s;
			*(dst_row + row3 + 2) = s;
			*(dst_row + row3 + 3) = s;

			// row 4
			*(dst_row + row4    ) = s;
			*(dst_row + row4 + 1) = s;
			*(dst_row + row4 + 2) = s;
			*(dst_row + row4 + 3) = s;

			src_row += 1;
			dst_row += 4;
		}
	}
}
static void scale(const void* src, int width, int height, int pitch, void* dst) {
	int scale_x = SCREEN_WIDTH / width;
	int scale_y = SCREEN_HEIGHT / height;
	int scale = MIN(scale_x,scale_y);
	int scale_w = width * scale;
	int scale_h = height * scale;
	int ox = (SCREEN_WIDTH - scale_w) / 2;
	int oy = (SCREEN_HEIGHT - scale_h) / 2;
		
	dst += (oy * SCREEN_PITCH) + (ox * SCREEN_BPP);
	
	// TODO: trying to identify source of the framepacing issue
	// scale1x(width,height,pitch,src,dst); 

	switch (scale) {
		case 4: scale4x_n16((void*)src,dst,width,height,pitch,SCREEN_PITCH); break;
		case 3: scale3x_n16((void*)src,dst,width,height,pitch,SCREEN_PITCH); break;
		case 2: scale2x_n16((void*)src,dst,width,height,pitch,SCREEN_PITCH); break;
		default: scale1x_n16((void*)src,dst,width,height,pitch,SCREEN_PITCH); break;
		
		// case 4: scale4x(width,height,pitch,src,dst); break;
		// case 3: scale3x(width,height,pitch,src,dst); break;
		// case 3: scale3x_lcd(width,height,pitch,src,dst); break;
		// case 3: scale3x_dmg(width,height,pitch,src,dst); break;
		// case 2: scale2x(width,height,pitch,src,dst); break;
		// default: scale1x(width,height,pitch,src,dst); break;
	}
	
	// TODO: diagnosing framepacing issues
	if (1) {
		static int frame = 0;
		int w = 8;
		int h = 16;
		int fps = 60;
		int x = frame * w;

		dst -= (oy * SCREEN_PITCH) + (ox * SCREEN_BPP);
		
		dst += (SCREEN_WIDTH - (w * fps)) / 2 * SCREEN_BPP;

		void* _dst = dst;
		memset(_dst, 0, (h * SCREEN_PITCH));
		for (int y=0; y<h; y++) {
			memset(_dst-SCREEN_BPP, 0xff, SCREEN_BPP);
			memset(_dst+(w * fps * SCREEN_BPP), 0xff, SCREEN_BPP);
			_dst += SCREEN_PITCH;
		}

		dst += (x * SCREEN_BPP);

		for (int y=0; y<h; y++) {
			memset(dst, 0xff, w * SCREEN_BPP);
			dst += SCREEN_PITCH;
		}

		frame += 1;
		if (frame>=fps) frame -= fps;
	}
	
	if (0) {
		// measure framerate
		static int start = -1;
		static int ticks = 0;
		ticks += 1;
		int now = SDL_GetTicks();
		if (start==-1) start = now;
		if (now-start>=1000) {
			start = now;
			printf("fps: %i\n", ticks);
			fflush(stdout);
			ticks = 0;
		}
	}
}

static void video_refresh_callback(const void *data, unsigned width, unsigned height, size_t pitch) {
	if (!data) return;
	fps_ticks += 1;
	
	static int last_width = 0;
	static int last_height = 0;
	if (width!=last_width || height!=last_height) {
		last_width = width;
		last_height = height;
		GFX_clearAll();
	}
	scale(data,width,height,pitch,screen->pixels);
	GFX_flip(screen);
}

static void audio_sample_callback(int16_t left, int16_t right) {
	SND_batchSamples(&(const SND_Frame){left,right}, 1);
}
static size_t audio_sample_batch_callback(const int16_t *data, size_t frames) { 
	return SND_batchSamples((const SND_Frame*)data, frames);
};

static uint32_t buttons = 0;
static void input_poll_callback(void) {
	PAD_poll();

	// TODO: tmp (L)oad and w(R)ite state
	if (PAD_isPressed(BTN_MENU)) {
		if (PAD_justPressed(BTN_L1)) State_read();
		else if (PAD_justPressed(BTN_R1)) State_write();
	}
	
	// TODO: support remapping
	
	buttons = 0;
	if (PAD_isPressed(BTN_UP)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
	if (PAD_isPressed(BTN_DOWN)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
	if (PAD_isPressed(BTN_LEFT)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
	if (PAD_isPressed(BTN_RIGHT)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
	if (PAD_isPressed(BTN_A)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_A;
	if (PAD_isPressed(BTN_B)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_B;
	if (PAD_isPressed(BTN_X)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_X;
	if (PAD_isPressed(BTN_Y)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_Y;
	if (PAD_isPressed(BTN_START)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_START;
	if (PAD_isPressed(BTN_SELECT)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_SELECT;
	if (PAD_isPressed(BTN_L1)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_L;
	if (PAD_isPressed(BTN_L2)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_L2;
	if (PAD_isPressed(BTN_R1)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_R;
	if (PAD_isPressed(BTN_R2)) buttons |= 1 << RETRO_DEVICE_ID_JOYPAD_R2;
}
static int16_t input_state_callback(unsigned port, unsigned device, unsigned index, unsigned id) { // copied from picoarch
	// id == RETRO_DEVICE_ID_JOYPAD_MASK or RETRO_DEVICE_ID_JOYPAD_*
	if (port == 0 && device == RETRO_DEVICE_JOYPAD && index == 0) {
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return buttons;
		return (buttons >> id) & 1;
	}
	return 0;
}

///////////////////////////////////////

void Core_getName(char* in_name, char* out_name) {
	strcpy(out_name, basename(in_name));
	char* tmp = strrchr(out_name, '_');
	tmp[0] = '\0';
}
void Core_open(const char* core_path, const char* tag_name) {
	LOG_info("inside Core_open\n");
	core.handle = dlopen(core_path, RTLD_LAZY);
	LOG_info("after dlopen\n");
	
	if (!core.handle) LOG_error("%s\n", dlerror());
	
	core.init = dlsym(core.handle, "retro_init");
	core.deinit = dlsym(core.handle, "retro_deinit");
	core.get_system_info = dlsym(core.handle, "retro_get_system_info");
	core.get_system_av_info = dlsym(core.handle, "retro_get_system_av_info");
	core.set_controller_port_device = dlsym(core.handle, "retro_set_controller_port_device");
	core.reset = dlsym(core.handle, "retro_reset");
	core.run = dlsym(core.handle, "retro_run");
	core.serialize_size = dlsym(core.handle, "retro_serialize_size");
	core.serialize = dlsym(core.handle, "retro_serialize");
	core.unserialize = dlsym(core.handle, "retro_unserialize");
	core.load_game = dlsym(core.handle, "retro_load_game");
	core.load_game_special = dlsym(core.handle, "retro_load_game_special");
	core.unload_game = dlsym(core.handle, "retro_unload_game");
	core.get_region = dlsym(core.handle, "retro_get_region");
	core.get_memory_data = dlsym(core.handle, "retro_get_memory_data");
	core.get_memory_size = dlsym(core.handle, "retro_get_memory_size");
	
	void (*set_environment_callback)(retro_environment_t);
	void (*set_video_refresh_callback)(retro_video_refresh_t);
	void (*set_audio_sample_callback)(retro_audio_sample_t);
	void (*set_audio_sample_batch_callback)(retro_audio_sample_batch_t);
	void (*set_input_poll_callback)(retro_input_poll_t);
	void (*set_input_state_callback)(retro_input_state_t);
	
	set_environment_callback = dlsym(core.handle, "retro_set_environment");
	set_video_refresh_callback = dlsym(core.handle, "retro_set_video_refresh");
	set_audio_sample_callback = dlsym(core.handle, "retro_set_audio_sample");
	set_audio_sample_batch_callback = dlsym(core.handle, "retro_set_audio_sample_batch");
	set_input_poll_callback = dlsym(core.handle, "retro_set_input_poll");
	set_input_state_callback = dlsym(core.handle, "retro_set_input_state");
	
	struct retro_system_info info = {};
	core.get_system_info(&info);
	
	Core_getName((char*)core_path, (char*)core.name);
	sprintf((char*)core.version, "%s (%s)", info.library_name, info.library_version);
	strcpy((char*)core.tag, tag_name);
	
	sprintf((char*)core.sys_dir, SDCARD_PATH "/.userdata/" PLATFORM "/%s-%s", core.tag, core.name);
	char cmd[512];
	sprintf(cmd, "mkdir -p \"%s\"", core.sys_dir);
	system(cmd);

	set_environment_callback(environment_callback);
	set_video_refresh_callback(video_refresh_callback);
	set_audio_sample_callback(audio_sample_callback);
	set_audio_sample_batch_callback(audio_sample_batch_callback);
	set_input_poll_callback(input_poll_callback);
	set_input_state_callback(input_state_callback);
}
void Core_init(void) {
	core.init();
	core.initialized = 1;
}
void Core_load(void) {
	LOG_info("inside Core_load\n");
	
	struct retro_game_info game_info;
	game_info.path = game.path;
	game_info.data = game.data;
	game_info.size = game.size;
	
	core.load_game(&game_info);
	LOG_info("after core.load_game\n");
	
	SRAM_read();
	LOG_info("after SRAM_read\n");
	
	// NOTE: must be called after core.load_game!
	struct retro_system_av_info av_info = {};
	core.get_system_av_info(&av_info);
	LOG_info("after core.get_system_av_info\n");
	
	double a = av_info.geometry.aspect_ratio;
	int w = av_info.geometry.base_width;
	int h = av_info.geometry.base_height;
	// char r[8];
	// getRatio(a, r);
	// LOG_info("after getRatio\n");
	
	core.fps = av_info.timing.fps;
	core.sample_rate = av_info.timing.sample_rate;

	printf("%s\n%s\n", core.tag, core.version);
	// printf("%dx%d (%s)\n", w,h,r);
	printf("%f\n%f\n", core.fps, core.sample_rate);
	fflush(stdout);
}
void Core_unload(void) {
	SND_quit();
}
void Core_quit(void) {
	if (core.initialized) {
		SRAM_write();
		core.unload_game();
		core.deinit();
		core.initialized = 0;
	}
}
void Core_close(void) {
	if (core.handle) dlclose(core.handle);
}

int main(int argc , char* argv[]) {
	char core_path[MAX_PATH];
	char rom_path[MAX_PATH]; 
	char tag_name[MAX_PATH];
	
	strcpy(core_path, argv[1]);
	strcpy(rom_path, argv[2]);
	getEmuName(rom_path, tag_name);
	
	LOG_info("core_path: %s\n", core_path);
	LOG_info("rom_path: %s\n", rom_path);
	LOG_info("tag_name: %s\n", tag_name);
	
	screen = GFX_init();
	Core_open(core_path, tag_name); 		LOG_info("after Core_open\n");
	Core_init(); 							LOG_info("after Core_init\n");
	Game_open(rom_path); 					LOG_info("after Game_open\n");
	Core_load();  							LOG_info("after Core_load\n");
	SND_init(core.sample_rate, core.fps);	LOG_info("after SND_init\n");
	
	// State_read();							LOG_info("after State_read\n");
	
	sec_start = SDL_GetTicks();
	while (1) {
		GFX_startFrame();
		if (PAD_justReleased(BTN_POWER)) break; // TODO: tmp
		core.run();
		cpu_ticks += 1;
		
		int now = SDL_GetTicks();
		if (now - sec_start>=1000) {
			printf("fps: %i (%i)\n", cpu_ticks, fps_ticks);
			sec_start = now;
			cpu_ticks = 0;
			fps_ticks = 0;
		}
	}
	
	Game_close();
	Core_unload();

	Core_quit();
	Core_close(); LOG_info("after Core_close\n");
	
	SDL_FreeSurface(screen);
	GFX_quit();
	
	return EXIT_SUCCESS;
}
