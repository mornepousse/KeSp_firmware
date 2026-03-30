/* KaSe keyboard-specific CDC command handlers.
   Registers with the generic CDC dispatch framework at init. */
#include "cdc_internal.h"
#include "cdc_keyboard_cmds.h"
#include "matrix_scan.h"
#include "key_stats.h"
#include "keyboard_config.h"
#include "keyboard_task.h"
#include "keymap.h"
#include "key_definitions.h"
#include "dfu_manager.h"
#include "tap_dance.h"
#include "combo.h"
#include "leader.h"
#include "tama_engine.h"
#include "key_features.h"
#include "hid_bluetooth_manager.h"
#include "status_display.h"
#include "version.h"

#if BOARD_HAS_POSITION_MAP
#include "board_position_map.h"
#endif

/* Binary response type IDs (KaSe protocol, used by start_command_queue) */
enum {
	CDC_RESP_NONE = 0,
	CDC_RESP_LAYER,
	CDC_RESP_CURRENT_LAYER,
	CDC_RESP_LAYER_NAME,
	CDC_RESP_MACROS,
	CDC_RESP_ALL_LAYOUT_NAMES,
	CDC_RESP_PING,
	CDC_RESP_DEBUG,
};

/* ── Keymap commands ─────────────────────────────────────────────── */

static void cmd_setlayer_command(const char *arg)
{
	if (arg == NULL)
	{
		cdc_send_line("ERR SETLAYER: missing arguments");
		return;
	}

	static char buffer[CDC_CMD_MAX_LEN];
	strncpy(buffer, arg, sizeof(buffer));
	buffer[sizeof(buffer) - 1] = '\0';

	char *sep = strchr(buffer, ':');
	if (!sep)
	{
		cdc_send_line("ERR SETLAYER: format SETLAYER<idx>:v1,v2,...");
		return;
	}

	*sep = '\0';
	char *idx_str = buffer;
	char *list_str = sep + 1;
	trim_spaces(idx_str);
	trim_spaces(list_str);
	if (*idx_str == '\0' || *list_str == '\0')
	{
		cdc_send_line("ERR SETLAYER: empty index or list");
		return;
	}

	int idx = atoi(idx_str);
	if (idx < 0 || idx >= LAYERS)
	{
		cdc_send_line("ERR SETLAYER: invalid index");
		return;
	}

	char dbg_msg[96];
	snprintf(dbg_msg, sizeof(dbg_msg), "DBG SETLAYER idx=%d", idx);
	cdc_send_line(dbg_msg);

	size_t expected = MATRIX_ROWS * MATRIX_COLS;
	uint16_t parsed[ expected ];
	size_t count = 0;
	char *saveptr = NULL;
	char *tok = strtok_r(list_str, ",", &saveptr);
	while (tok && count < expected)
	{
		trim_spaces(tok);
		long v = strtol(tok, NULL, 0);
		if (v < 0 || v > 0xFFFF)
		{
			cdc_send_line("ERR SETLAYER: value out of range");
			return;
		}
		parsed[count++] = (uint16_t)v;
		tok = strtok_r(NULL, ",", &saveptr);
	}

	if (count != expected)
	{
		char msg[64];
		snprintf(msg, sizeof(msg), "ERR SETLAYER: expected %u values, got %u", (unsigned)expected, (unsigned)count);
		cdc_send_line(msg);
		return;
	}


	for (size_t v2_row = 0; v2_row < MATRIX_ROWS; ++v2_row)
	{
		for (size_t v2_col = 0; v2_col < MATRIX_COLS; ++v2_col)
		{
			size_t pos = v2_row * MATRIX_COLS + v2_col;
#if BOARD_HAS_POSITION_MAP
			int v1_row, v1_col;
			v2_to_v1_pos(v2_row, v2_col, &v1_row, &v1_col);
			keymaps[idx][v1_row][v1_col] = parsed[pos];
#else
			keymaps[idx][v2_row][v2_col] = parsed[pos];
#endif
		}
	}

	size_t total_elements = (size_t)LAYERS * MATRIX_ROWS * MATRIX_COLS;
	size_t total_bytes = total_elements * sizeof(uint16_t);
	save_keymaps((uint16_t *)keymaps, total_bytes);

	uint16_t preview[6] = {0};
	for (size_t i = 0; i < 6 && i < expected; ++i)
		preview[i] = parsed[i];

	snprintf(dbg_msg, sizeof(dbg_msg), "DBG SETLAYER persisted %u bytes", (unsigned)total_bytes);
	cdc_send_line(dbg_msg);
	snprintf(dbg_msg, sizeof(dbg_msg), "DBG SETLAYER preview:%04X,%04X,%04X,%04X,%04X,%04X",
			 preview[0], preview[1], preview[2], preview[3], preview[4], preview[5]);
	cdc_send_line(dbg_msg);


	if ((uint8_t)idx == current_layout)
		status_display_update_layer_name();

	cdc_send_line("OK");
}

static void cmd_get_current_layer_index(void)
{

	ESP_LOGI(TAG_CDC, "Current layer: %d", current_layout);
	start_command_queue(CDC_RESP_CURRENT_LAYER, 1);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)&current_layout, 1);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

static void cmd_get_name_layer(uint8_t layer)
{
	if (layer >= LAYERS) {
		cdc_send_line("ERROR:INVALID_LAYER");
		return;
	}
	const char *name = default_layout_names[layer];
	size_t name_len = strlen(name);
	size_t total_size = 1 + name_len;
	start_command_queue(CDC_RESP_LAYER_NAME, total_size);
	uint8_t idx = (uint8_t)layer;
	tinyusb_cdcacm_write_queue(CDC_ITF, &idx, 1);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)name, name_len);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

static void cmd_list_layout_names(void)
{
	size_t total_size = 1;
	for (int i = 0; i < LAYERS; ++i)
		total_size += 1 + strlen(default_layout_names[i]) + 1;

	start_command_queue(CDC_RESP_ALL_LAYOUT_NAMES, total_size);

	uint8_t layers_count = (uint8_t)LAYERS;
	tinyusb_cdcacm_write_queue(CDC_ITF, &layers_count, 1);

	for (int i = 0; i < LAYERS; ++i)
	{
		uint8_t idx = (uint8_t)i;
		const char *name = default_layout_names[i];
		size_t name_len = strlen(name);
		tinyusb_cdcacm_write_queue(CDC_ITF, &idx, 1);
		tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)name, name_len);
		uint8_t sep = ';';
		tinyusb_cdcacm_write_queue(CDC_ITF, &sep, 1);
	}
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

static void cmd_get_keymap_by_layer(uint8_t layer)
{
	if (layer >= LAYERS)
	{
		ESP_LOGW(TAG_CDC, "Layer %u invalid", (unsigned)layer);
		return;
	}

	size_t total_size = (MATRIX_ROWS * MATRIX_COLS * sizeof(uint16_t)) + 2;
	start_command_queue(CDC_RESP_LAYER, total_size);
	unsigned char bytes[2];
	bytes[0] = (unsigned char)(layer & 0xFF);
	bytes[1] = (unsigned char)((layer >> 8) & 0xFF);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)bytes, 2);
	for (int v2_row = 0; v2_row < MATRIX_ROWS; v2_row++)
	{
		for (int v2_col = 0; v2_col < MATRIX_COLS; v2_col++)
		{
#if BOARD_HAS_POSITION_MAP
			int v1_row, v1_col;
			v2_to_v1_pos(v2_row, v2_col, &v1_row, &v1_col);
			uint16_t code = keymaps[layer][v1_row][v1_col];
#else
			uint16_t code = keymaps[layer][v2_row][v2_col];
#endif
			bytes[0] = (unsigned char)(code & 0xFF);
			bytes[1] = (unsigned char)((code >> 8) & 0xFF);
			tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)bytes, 2);
		}
	}
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

static void cmd_set_key(const char *arg)
{
	int layer, row, col;
	unsigned int value;
	if (sscanf(arg, "%d,%d,%d,%x", &layer, &row, &col, &value) != 4)
	{
		ESP_LOGW(TAG_CDC, "SETKEY: invalid format");
		return;
	}
	if (layer < 0 || layer >= LAYERS || row < 0 || row >= MATRIX_ROWS || col < 0 || col >= MATRIX_COLS)
	{
		ESP_LOGW(TAG_CDC, "SETKEY: index out of range");
		return;
	}
#if BOARD_HAS_POSITION_MAP
	int internal_row, internal_col;
	v2_to_v1_pos(row, col, &internal_row, &internal_col);
#else
	int internal_row = row;
	int internal_col = col;
#endif

	keymaps[layer][internal_row][internal_col] = (uint16_t)value;
	ESP_LOGI(TAG_CDC, "SETKEY: [%d][%d][%d] = 0x%04X (v2: r%d,c%d)", layer, internal_row, internal_col, value, row, col);
	size_t total_elements = (size_t)LAYERS * MATRIX_ROWS * MATRIX_COLS;
	save_keymaps((uint16_t *)keymaps, total_elements * sizeof(uint16_t));
}

/* ── Layout name commands ────────────────────────────────────────── */

static void cmd_set_layout_name(const char *arg)
{
	if (arg == NULL)
	{
		cdc_send_line("ERR LAYOUTNAME: missing arguments");
		return;
	}

	char buffer[CDC_CMD_MAX_LEN];
	strncpy(buffer, arg, sizeof(buffer));
	buffer[sizeof(buffer) - 1] = '\0';

	char *sep = strchr(buffer, ':');
	if (!sep)
	{
		cdc_send_line("ERR LAYOUTNAME: format LAYOUTNAME<idx>:<name>");
		return;
	}

	*sep = '\0';
	char *idx_str = buffer;
	char *name_str = sep + 1;
	trim_spaces(idx_str);
	trim_spaces(name_str);

	if (*idx_str == '\0' || *name_str == '\0')
	{
		cdc_send_line("ERR LAYOUTNAME: empty index or name");
		return;
	}

	int idx = atoi(idx_str);
	if (idx < 0 || idx >= LAYERS)
	{
		cdc_send_line("ERR LAYOUTNAME: invalid index");
		return;
	}

	strncpy(default_layout_names[idx], name_str, MAX_LAYOUT_NAME_LENGTH - 1);
	default_layout_names[idx][MAX_LAYOUT_NAME_LENGTH - 1] = '\0';
	save_layout_names(default_layout_names, LAYERS);


	if ((uint8_t)idx == current_layout)
		status_display_update_layer_name();

	const char *name = default_layout_names[idx];
	size_t name_len = strlen(name);
	size_t total_size = 1 + name_len;
	start_command_queue(CDC_RESP_LAYER_NAME, total_size);
	uint8_t uidx = (uint8_t)idx;
	tinyusb_cdcacm_write_queue(CDC_ITF, &uidx, 1);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)name, name_len);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

/* ── Macro commands ──────────────────────────────────────────────── */

static uint16_t macro_keycode_from_index(size_t idx)
{
	return (uint16_t)(MACRO_1 + (idx * 0x0100));
}

static void cmd_list_macros(void)
{
	size_t count = 0;
	size_t total_size = 1;
	for (size_t i = 0; i < MAX_MACROS; ++i)
	{
		if (macros_list[i].name[0] == '\0') continue;
		++count;
		size_t name_len = strlen(macros_list[i].name);
		if (name_len > 255) name_len = 255;
		uint8_t keys_len = 0;
		for (int k = 0; k < 6; ++k)
			if (macros_list[i].keys[k] != 0) ++keys_len;
		total_size += 1 + 2 + 1 + name_len + 1 + keys_len;
	}

	start_command_queue(CDC_RESP_MACROS, total_size);
	uint8_t ucount = (uint8_t)count;
	tinyusb_cdcacm_write_queue(CDC_ITF, &ucount, 1);

	for (size_t i = 0; i < MAX_MACROS; ++i)
	{
		if (macros_list[i].name[0] == '\0') continue;

		uint8_t idx = (uint8_t)i;
		uint16_t kc = macro_keycode_from_index(i);
		uint8_t kc_bytes[2] = { (uint8_t)(kc & 0xFF), (uint8_t)((kc >> 8) & 0xFF) };
		size_t name_len = strlen(macros_list[i].name);
		if (name_len > 255) name_len = 255;
		uint8_t name_len_u8 = (uint8_t)name_len;
		uint8_t keys_len = 0;
		for (int k = 0; k < 6; ++k)
			if (macros_list[i].keys[k] != 0) ++keys_len;

		tinyusb_cdcacm_write_queue(CDC_ITF, &idx, 1);
		tinyusb_cdcacm_write_queue(CDC_ITF, kc_bytes, 2);
		tinyusb_cdcacm_write_queue(CDC_ITF, &name_len_u8, 1);
		tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)macros_list[i].name, name_len);
		tinyusb_cdcacm_write_queue(CDC_ITF, &keys_len, 1);
		if (keys_len > 0)
		{
			uint8_t tmp_keys[6];
			uint8_t pos = 0;
			for (int k = 0; k < 6; ++k)
				if (macros_list[i].keys[k] != 0 && pos < sizeof(tmp_keys))
					tmp_keys[pos++] = macros_list[i].keys[k];
			if (pos > 0)
				tinyusb_cdcacm_write_queue(CDC_ITF, tmp_keys, pos);
		}
	}
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

static void cmd_macro_add(const char *arg)
{
	if (arg == NULL)
	{
		cdc_send_line("ERR MACROADD: missing arguments");
		return;
	}
	char buffer[CDC_CMD_MAX_LEN];
	strncpy(buffer, arg, sizeof(buffer));
	buffer[sizeof(buffer) - 1] = '\0';
	char *saveptr = NULL;
	char *slot_str = strtok_r(buffer, ";", &saveptr);
	char *name_str = strtok_r(NULL, ";", &saveptr);
	char *keys_str = strtok_r(NULL, ";", &saveptr);
	if (!slot_str || !name_str || !keys_str)
	{
		cdc_send_line("ERR MACROADD format: MACROADD slot;name;hex1,hex2,...");
		return;
	}
	trim_spaces(slot_str);
	trim_spaces(name_str);
	trim_spaces(keys_str);
	if (*slot_str == '\0' || *name_str == '\0' || *keys_str == '\0')
	{
		cdc_send_line("ERR MACROADD: empty parameter");
		return;
	}
	int slot = atoi(slot_str);
	if (slot < 0 || slot >= MAX_MACROS)
	{
		cdc_send_line("ERR MACROADD: invalid slot");
		return;
	}
	uint8_t parsed_keys[6] = {0};
	char *keys_save = NULL;
	char *token = strtok_r(keys_str, ",", &keys_save);
	int key_index = 0;
	while (token && key_index < 6)
	{
		trim_spaces(token);
		if (*token == '\0') { token = strtok_r(NULL, ",", &keys_save); continue; }
		char *endptr = NULL;
		unsigned long value = strtoul(token, &endptr, 0);
		if (endptr == token || value > 0xFF)
		{
			cdc_send_line("ERR MACROADD: invalid keycode");
			return;
		}
		parsed_keys[key_index++] = (uint8_t)value;
		token = strtok_r(NULL, ",", &keys_save);
	}
	if (key_index == 0)
	{
		cdc_send_line("ERR MACROADD: no keys provided");
		return;
	}
	while (key_index < 6) parsed_keys[key_index++] = 0;

	strncpy(macros_list[slot].name, name_str, MAX_MACRO_NAME_LENGTH - 1);
	macros_list[slot].name[MAX_MACRO_NAME_LENGTH - 1] = '\0';
	memcpy(macros_list[slot].keys, parsed_keys, sizeof(parsed_keys));
	macros_list[slot].key_definition = macro_keycode_from_index((size_t)slot);
	if ((size_t)(slot + 1) > macros_count) macros_count = slot + 1;
	save_macros(macros_list, macros_count);
	char msg[64];
	snprintf(msg, sizeof(msg), "MACRO %d saved", slot);
	cdc_send_line(msg);
}

/* MACROSEQ slot;name;key1:mod1,key2:mod2,FF:delay,...  — sequence macro */
static void cmd_macro_add_seq(const char *arg)
{
	if (!arg) { cdc_send_line("ERR MACROSEQ: missing args"); return; }
	char buffer[CDC_CMD_MAX_LEN];
	strncpy(buffer, arg, sizeof(buffer)); buffer[sizeof(buffer)-1] = '\0';

	char *saveptr = NULL;
	char *slot_str = strtok_r(buffer, ";", &saveptr);
	char *name_str = strtok_r(NULL, ";", &saveptr);
	char *steps_str = strtok_r(NULL, ";", &saveptr);
	if (!slot_str || !name_str || !steps_str) {
		cdc_send_line("ERR MACROSEQ format: slot;name;key:mod,key:mod,...");
		return;
	}
	trim_spaces(slot_str); trim_spaces(name_str); trim_spaces(steps_str);

	int slot = atoi(slot_str);
	if (slot < 0 || slot >= MAX_MACROS) { cdc_send_line("ERR MACROSEQ: invalid slot"); return; }

	macro_step_t steps[MACRO_MAX_STEPS] = {0};
	char *tok = steps_str;
	int step_idx = 0;
	while (tok && *tok && step_idx < MACRO_MAX_STEPS) {
		char *next = strchr(tok, ',');
		if (next) *next = '\0';
		trim_spaces(tok);

		char *colon = strchr(tok, ':');
		if (colon) {
			*colon = '\0';
			steps[step_idx].keycode = (uint8_t)strtoul(tok, NULL, 16);
			steps[step_idx].modifier = (uint8_t)strtoul(colon + 1, NULL, 16);
		} else {
			steps[step_idx].keycode = (uint8_t)strtoul(tok, NULL, 16);
			steps[step_idx].modifier = 0;
		}
		step_idx++;
		tok = next ? next + 1 : NULL;
	}

	strncpy(macros_list[slot].name, name_str, MAX_MACRO_NAME_LENGTH - 1);
	macros_list[slot].name[MAX_MACRO_NAME_LENGTH - 1] = '\0';
	memcpy(macros_list[slot].steps, steps, sizeof(steps));
	memset(macros_list[slot].keys, 0, sizeof(macros_list[slot].keys));
	macros_list[slot].key_definition = macro_keycode_from_index((size_t)slot);
	if ((size_t)(slot + 1) > macros_count) macros_count = slot + 1;
	save_macros(macros_list, macros_count);

	char msg[64];
	snprintf(msg, sizeof(msg), "MACRO %d saved (%d steps)", slot, step_idx);
	cdc_send_line(msg);
}

static void cmd_macro_delete(const char *arg)
{
	if (arg == NULL)
	{
		cdc_send_line("ERR MACRODEL: missing arguments");
		return;
	}
	char buffer[CDC_CMD_MAX_LEN];
	strncpy(buffer, arg, sizeof(buffer));
	buffer[sizeof(buffer) - 1] = '\0';
	trim_spaces(buffer);
	if (*buffer == '\0')
	{
		cdc_send_line("ERR MACRODEL: missing slot");
		return;
	}
	int slot = atoi(buffer);
	if (slot < 0 || slot >= MAX_MACROS)
	{
		cdc_send_line("ERR MACRODEL: invalid slot");
		return;
	}
	if (macros_list[slot].name[0] == '\0')
	{
		cdc_send_line("MACRODEL: slot already empty");
		return;
	}
	macros_list[slot].name[0] = '\0';
	memset(macros_list[slot].keys, 0, sizeof(macros_list[slot].keys));
	macros_list[slot].key_definition = macro_keycode_from_index((size_t)slot);
	recalc_macros_count();
	save_macros(macros_list, macros_count);
	char msg[64];
	snprintf(msg, sizeof(msg), "MACRO %d deleted", slot);
	cdc_send_line(msg);
}

/* ── System commands ─────────────────────────────────────────────── */

static void cmd_reboot_to_dfu(void)
{
	cdc_send_line("Rebooting to DFU...");
	status_display_show_DFU_prog();
	vTaskDelay(pdMS_TO_TICKS(300));
	reboot_to_dfu();
}

/* ── Stats commands ──────────────────────────────────────────────── */

static void cmd_get_keystats(bool binary)
{
	if (binary) {
		uint8_t header[19];
		memcpy(header, "KEYSTATS", 8);
		header[8] = MATRIX_ROWS;
		header[9] = MATRIX_COLS;
		header[10] = MODULE_ID;
		pack_u32_le(&header[11], key_stats_total);
		uint32_t max_val = get_key_stats_max();
		pack_u32_le(&header[15], max_val);

		uint8_t payload[19 + MATRIX_ROWS * MATRIX_COLS * 4];
		memcpy(payload, header, 19);
		size_t offset = 19;
		for (int r = 0; r < MATRIX_ROWS; r++) {
			for (int c = 0; c < MATRIX_COLS; c++) {
#if BOARD_HAS_POSITION_MAP
				int v1_row, v1_col;
				v2_to_v1_pos(r, c, &v1_row, &v1_col);
				uint32_t count = key_stats[v1_row][v1_col];
#else
				uint32_t count = key_stats[r][c];
#endif
				pack_u32_le(&payload[offset], count);
				offset += 4;
			}
		}
		tinyusb_cdcacm_write_queue(CDC_ITF, payload, offset);
		tinyusb_cdcacm_write_flush(CDC_ITF, 0);
		cdc_send_line("");
	} else {
		char buf[128];
		snprintf(buf, sizeof(buf), "Key Statistics - Total: %lu, Max: %lu",
				(unsigned long)key_stats_total, (unsigned long)get_key_stats_max());
		cdc_send_line(buf);
		for (int r = 0; r < MATRIX_ROWS; r++) {
			char line[256] = "";
			int pos = 0;
			pos += snprintf(line + pos, sizeof(line) - pos, "R%d: ", r);
			for (int c = 0; c < MATRIX_COLS; c++)
				pos += snprintf(line + pos, sizeof(line) - pos, "%5lu ", (unsigned long)key_stats[r][c]);
			cdc_send_line(line);
		}
		cdc_send_line("OK");
	}
}

static void cmd_get_bigrams(bool binary)
{
	if (binary) {
		typedef struct { uint8_t prev; uint8_t curr; uint16_t count; } bigram_entry_t;
		#define MAX_BIGRAM_ENTRIES 256
		bigram_entry_t *entries = malloc(MAX_BIGRAM_ENTRIES * sizeof(bigram_entry_t));
		if (!entries) { cdc_send_line("ERROR:OOM"); return; }
		uint16_t n_entries = 0;

		uint16_t min_in_list = 0;
		for (int i = 0; i < NUM_KEYS; i++) {
			for (int j = 0; j < NUM_KEYS; j++) {
				if (bigram_stats[i][j] == 0) continue;
				if (n_entries < MAX_BIGRAM_ENTRIES) {
					entries[n_entries].prev = (uint8_t)i;
					entries[n_entries].curr = (uint8_t)j;
					entries[n_entries].count = bigram_stats[i][j];
					n_entries++;
					if (bigram_stats[i][j] < min_in_list || min_in_list == 0)
						min_in_list = bigram_stats[i][j];
				} else if (bigram_stats[i][j] > min_in_list) {
					uint16_t min_idx = 0;
					for (uint16_t k = 1; k < MAX_BIGRAM_ENTRIES; k++)
						if (entries[k].count < entries[min_idx].count) min_idx = k;
					entries[min_idx].prev = (uint8_t)i;
					entries[min_idx].curr = (uint8_t)j;
					entries[min_idx].count = bigram_stats[i][j];
					min_in_list = entries[0].count;
					for (uint16_t k = 1; k < MAX_BIGRAM_ENTRIES; k++)
						if (entries[k].count < min_in_list) min_in_list = entries[k].count;
				}
			}
		}

		for (uint16_t i = 1; i < n_entries; i++) {
			bigram_entry_t tmp = entries[i];
			int j = i - 1;
			while (j >= 0 && entries[j].count < tmp.count) { entries[j + 1] = entries[j]; j--; }
			entries[j + 1] = tmp;
		}

		uint16_t max_val = get_bigram_stats_max();
		uint8_t header[18];
		memcpy(header, "BIGRAMS", 7);
		header[7] = 0;
		header[8] = MODULE_ID;
		header[9] = (uint8_t)NUM_KEYS;
		pack_u32_le(&header[10], bigram_total);
		pack_u16_le(&header[14], max_val);
		pack_u16_le(&header[16], n_entries);

		size_t total_size = 18 + n_entries * 4;
		uint8_t *payload = malloc(total_size);
		if (!payload) { free(entries); cdc_send_line("ERROR:OOM"); return; }
		memcpy(payload, header, 18);
		for (uint16_t i = 0; i < n_entries; i++) {
			size_t off = 18 + i * 4;
			payload[off + 0] = entries[i].prev;
			payload[off + 1] = entries[i].curr;
			pack_u16_le(&payload[off + 2], entries[i].count);
		}
		tinyusb_cdcacm_write_queue(CDC_ITF, payload, total_size);
		tinyusb_cdcacm_write_flush(CDC_ITF, 0);
		free(payload);
		free(entries);
		cdc_send_line("");
	} else {
		char buf[128];
		snprintf(buf, sizeof(buf), "Bigram Statistics - Total: %lu, Max: %u",
				(unsigned long)bigram_total, get_bigram_stats_max());
		cdc_send_line(buf);

		typedef struct { uint8_t prev; uint8_t curr; uint16_t count; } bg_t;
		bg_t top[20] = {0};
		uint8_t n = 0;
		for (int i = 0; i < NUM_KEYS; i++) {
			for (int j = 0; j < NUM_KEYS; j++) {
				if (bigram_stats[i][j] == 0) continue;
				if (n < 20) {
					top[n].prev = i; top[n].curr = j; top[n].count = bigram_stats[i][j];
					n++;
				} else {
					uint8_t mi = 0;
					for (uint8_t k = 1; k < 20; k++)
						if (top[k].count < top[mi].count) mi = k;
					if (bigram_stats[i][j] > top[mi].count) {
						top[mi].prev = i; top[mi].curr = j; top[mi].count = bigram_stats[i][j];
					}
				}
			}
		}
		for (uint8_t i = 1; i < n; i++) {
			bg_t tmp = top[i];
			int j = i - 1;
			while (j >= 0 && top[j].count < tmp.count) { top[j+1] = top[j]; j--; }
			top[j+1] = tmp;
		}
		for (uint8_t i = 0; i < n; i++) {
			uint8_t pr = top[i].prev / MATRIX_COLS, pc = top[i].prev % MATRIX_COLS;
			uint8_t cr = top[i].curr / MATRIX_COLS, cc = top[i].curr % MATRIX_COLS;
			snprintf(buf, sizeof(buf), "  R%dC%d -> R%dC%d : %u", pr, pc, cr, cc, top[i].count);
			cdc_send_line(buf);
		}
		cdc_send_line("OK");
	}
}

static void cmd_reset_bigrams(void)
{
	reset_bigram_stats();
	cdc_send_line("BIGRAMS_RESET:OK");
}

static void cmd_reset_keystats(void)
{
	reset_key_stats();
	cdc_send_line("KEYSTATS_RESET:OK");
}

/* ── Wrapper handlers ────────────────────────────────────────────── */

static void cmd_version(const char *arg)   { (void)arg; cdc_send_line(PRODUCT_NAME " v" FW_VERSION); }
static void cmd_dfu(const char *arg)       { (void)arg; cmd_reboot_to_dfu(); }
static void cmd_layer_idx(const char *arg) { (void)arg; cmd_get_current_layer_index(); }
static void cmd_macros_q(const char *arg)  { (void)arg; cmd_list_macros(); }
static void cmd_layouts_q(const char *arg) { (void)arg; cmd_list_layout_names(); }
static void cmd_layout_json(const char *arg) {
	(void)arg;
	extern const char board_layout_json[];
	cdc_send_large(board_layout_json, strlen(board_layout_json));
}
static void cmd_keymap_current(const char *arg) {
	(void)arg;

	cmd_get_keymap_by_layer(current_layout);
}
static void cmd_keymap_by_index(const char *arg) {
	if (arg && isdigit((unsigned char)arg[0]))
		cmd_get_keymap_by_layer((uint8_t)atoi(arg));
}
static void cmd_layer_name_short(const char *arg) {
	if (!arg) return;
	if (arg[0] == '?') {
		cmd_get_current_layer_index();
	} else if (toupper((unsigned char)arg[0]) == 'N' && isdigit((unsigned char)arg[1])) {
		cmd_get_name_layer((uint8_t)(arg[1] - '0'));
	} else if (isdigit((unsigned char)arg[0])) {
		cmd_get_name_layer((uint8_t)(arg[0] - '0'));
	}
}
static void cmd_keystats_text(const char *arg)   { (void)arg; cmd_get_keystats(false); }
static void cmd_keystats_bin(const char *arg)    { (void)arg; cmd_get_keystats(true); }
static void cmd_keystats_rst(const char *arg)    { (void)arg; cmd_reset_keystats(); }
static void cmd_bigrams_text(const char *arg)    { (void)arg; cmd_get_bigrams(false); }
static void cmd_bigrams_bin(const char *arg)     { (void)arg; cmd_get_bigrams(true); }
static void cmd_bigrams_rst(const char *arg)     { (void)arg; cmd_reset_bigrams(); }

/* ── Tap Dance commands ──────────────────────────────────────────── */

/* TDSET <index>;<a1>,<a2>,<a3>,<a4>  — configure a tap dance slot */
static void cmd_tdset(const char *arg)
{
	if (!arg) { cdc_send_line("ERR TDSET: missing args"); return; }
	char buf[CDC_CMD_MAX_LEN];
	strncpy(buf, arg, sizeof(buf)); buf[sizeof(buf)-1] = '\0';

	char *sep = strchr(buf, ';');
	if (!sep) { cdc_send_line("ERR TDSET: format index;a1,a2,a3,a4"); return; }
	*sep = '\0';
	trim_spaces(buf);

	int idx = atoi(buf);
	if (idx < 0 || idx >= TAP_DANCE_MAX_SLOTS) { cdc_send_line("ERR TDSET: invalid index"); return; }

	uint8_t actions[4] = {0};
	char *tok = sep + 1;
	for (int i = 0; i < 4 && tok; i++) {
		char *next = strchr(tok, ',');
		if (next) *next = '\0';
		trim_spaces(tok);
		actions[i] = (uint8_t)strtoul(tok, NULL, 16); /* always hex */
		tok = next ? next + 1 : NULL;
	}

	tap_dance_set((uint8_t)idx, actions);
	tap_dance_save();

	char resp[48];
	snprintf(resp, sizeof(resp), "TDSET %d:OK", idx);
	cdc_send_line(resp);
}

/* TD? — list all configured tap dance slots */
static void cmd_tdlist(const char *arg)
{
	(void)arg;
	char buf[64];
	for (int i = 0; i < TAP_DANCE_MAX_SLOTS; i++) {
		const tap_dance_config_t *td = tap_dance_get(i);
		if (!td || (td->actions[0] == 0 && td->actions[1] == 0 &&
		            td->actions[2] == 0 && td->actions[3] == 0))
			continue;
		snprintf(buf, sizeof(buf), "TD%d: %02X,%02X,%02X,%02X",
		         i, td->actions[0], td->actions[1], td->actions[2], td->actions[3]);
		cdc_send_line(buf);
	}
	cdc_send_line("OK");
}

/* COMBOSET <index>;<r1>,<c1>,<r2>,<c2>,<result> — configure a combo (V1 coords) */
static void cmd_comboset(const char *arg)
{
	if (!arg) { cdc_send_line("ERR COMBOSET: missing args"); return; }
	char buf[CDC_CMD_MAX_LEN];
	strncpy(buf, arg, sizeof(buf)); buf[sizeof(buf)-1] = '\0';

	char *sep = strchr(buf, ';');
	if (!sep) { cdc_send_line("ERR COMBOSET: format index;r1,c1,r2,c2,result"); return; }
	*sep = '\0';
	int idx = atoi(buf);
	if (idx < 0 || idx >= COMBO_MAX_SLOTS) { cdc_send_line("ERR COMBOSET: invalid index"); return; }

	combo_config_t cfg = {0};
	int r1, c1, r2, c2, res;
	if (sscanf(sep + 1, "%d,%d,%d,%d,%x", &r1, &c1, &r2, &c2, &res) != 5) {
		cdc_send_line("ERR COMBOSET: format r1,c1,r2,c2,result_hex");
		return;
	}
	cfg.row1 = r1; cfg.col1 = c1; cfg.row2 = r2; cfg.col2 = c2; cfg.result = (uint8_t)res;
	combo_set((uint8_t)idx, &cfg);
	combo_save();

	char resp[32];
	snprintf(resp, sizeof(resp), "COMBOSET %d:OK", idx);
	cdc_send_line(resp);
}

/* COMBO? — list configured combos */
static void cmd_combolist(const char *arg)
{
	(void)arg;
	char buf[64];
	for (int i = 0; i < COMBO_MAX_SLOTS; i++) {
		const combo_config_t *c = combo_get(i);
		if (!c || c->result == 0) continue;
		snprintf(buf, sizeof(buf), "COMBO%d: r%dc%d+r%dc%d=%02X",
		         i, c->row1, c->col1, c->row2, c->col2, c->result);
		cdc_send_line(buf);
	}
	cdc_send_line("OK");
}

/* LEADERSET <index>;<seq_hex>;<result_hex>,<mod_hex> */
static void cmd_leaderset(const char *arg)
{
	if (!arg) { cdc_send_line("ERR LEADERSET: missing args"); return; }
	char buf[CDC_CMD_MAX_LEN];
	strncpy(buf, arg, sizeof(buf)); buf[sizeof(buf)-1] = '\0';

	char *sep1 = strchr(buf, ';');
	if (!sep1) { cdc_send_line("ERR LEADERSET: format index;seq;result,mod"); return; }
	*sep1 = '\0';
	int idx = atoi(buf);
	if (idx < 0 || idx >= LEADER_MAX_ENTRIES) { cdc_send_line("ERR LEADERSET: invalid index"); return; }

	char *sep2 = strchr(sep1 + 1, ';');
	if (!sep2) { cdc_send_line("ERR LEADERSET: missing result"); return; }
	*sep2 = '\0';

	leader_entry_t entry = {0};
	/* Parse sequence: comma-separated hex keycodes */
	char *tok = sep1 + 1;
	for (int i = 0; i < LEADER_MAX_SEQ_LEN && tok && *tok; i++) {
		char *next = strchr(tok, ',');
		if (next) *next = '\0';
		trim_spaces(tok);
		entry.sequence[i] = (uint8_t)strtoul(tok, NULL, 16);
		tok = next ? next + 1 : NULL;
	}

	/* Parse result and mod */
	unsigned int res = 0, mod = 0;
	sscanf(sep2 + 1, "%x,%x", &res, &mod);
	entry.result = (uint8_t)res;
	entry.result_mod = (uint8_t)mod;

	leader_set((uint8_t)idx, &entry);
	leader_save();

	char resp[32];
	snprintf(resp, sizeof(resp), "LEADERSET %d:OK", idx);
	cdc_send_line(resp);
}

/* LEADER? — list configured leader sequences */
static void cmd_leaderlist(const char *arg)
{
	(void)arg;
	char buf[80];
	for (int i = 0; i < LEADER_MAX_ENTRIES; i++) {
		const leader_entry_t *e = leader_get(i);
		if (!e || e->result == 0) continue;
		int pos = snprintf(buf, sizeof(buf), "LEADER%d: ", i);
		for (int j = 0; j < LEADER_MAX_SEQ_LEN && e->sequence[j]; j++)
			pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X,", e->sequence[j]);
		if (pos > 0 && buf[pos-1] == ',') pos--;
		snprintf(buf + pos, sizeof(buf) - pos, "->%02X+%02X", e->result, e->result_mod);
		cdc_send_line(buf);
	}
	cdc_send_line("OK");
}

/* TAMA? — query tama stats */
static void cmd_tama_query(const char *arg)
{
	(void)arg;
	const tama2_stats_t *s = tama_engine_get_stats();
	char buf[128];
	snprintf(buf, sizeof(buf), "TAMA: Lv%d hunger=%d happy=%d energy=%d health=%d keys=%lu enabled=%d",
	         s->level + 1, s->hunger, s->happiness, s->energy, s->health,
	         (unsigned long)s->total_keys, tama_engine_is_enabled());
	cdc_send_line(buf);
}

/* TAMA ENABLE/DISABLE/FEED/PLAY/SLEEP/MEDICINE */
static void cmd_tama_action(const char *arg)
{
	if (!arg) { cdc_send_line("ERR TAMA: missing action"); return; }
	char buf[32];
	strncpy(buf, arg, sizeof(buf)); buf[sizeof(buf)-1] = '\0';
	trim_spaces(buf);

	if (strcasecmp(buf, "ENABLE") == 0) {
		tama_engine_set_enabled(true);
		cdc_send_line("TAMA:ENABLED");
	} else if (strcasecmp(buf, "DISABLE") == 0) {
		tama_engine_set_enabled(false);
		cdc_send_line("TAMA:DISABLED");
	} else if (strcasecmp(buf, "FEED") == 0) {
		tama_engine_action(TAMA2_ACTION_FEED);
		cdc_send_line("TAMA:FED");
	} else if (strcasecmp(buf, "PLAY") == 0) {
		tama_engine_action(TAMA2_ACTION_PLAY);
		cdc_send_line("TAMA:PLAYED");
	} else if (strcasecmp(buf, "SLEEP") == 0) {
		tama_engine_action(TAMA2_ACTION_SLEEP);
		cdc_send_line("TAMA:SLEPT");
	} else if (strcasecmp(buf, "MEDICINE") == 0) {
		tama_engine_action(TAMA2_ACTION_MEDICINE);
		cdc_send_line("TAMA:HEALED");
	} else if (strcasecmp(buf, "SAVE") == 0) {
		tama_engine_save();
		cdc_send_line("TAMA:SAVED");
	} else {
		cdc_send_line("ERR TAMA: ENABLE|DISABLE|FEED|PLAY|SLEEP|MEDICINE|SAVE");
	}
}

/* BT? — query bluetooth status */
static void cmd_bt_query(const char *arg)
{
	(void)arg;
	char buf[128];
	snprintf(buf, sizeof(buf), "BT: slot=%d init=%d conn=%d name=%s",
	         bt_get_active_slot(),
	         hid_bluetooth_is_initialized(),
	         hid_bluetooth_is_connected(),
	         bt_get_connected_name());
	cdc_send_line(buf);
	/* List all slots */
	for (int i = 0; i < BT_MAX_DEVICES; i++) {
		const bt_device_slot_t *s = bt_get_slot(i);
		if (s && s->valid) {
			snprintf(buf, sizeof(buf), "  SLOT%d: %02X:%02X:%02X:%02X:%02X:%02X %s%s",
			         i, s->addr[0], s->addr[1], s->addr[2],
			         s->addr[3], s->addr[4], s->addr[5],
			         s->name[0] ? s->name : "(unnamed)",
			         (i == bt_get_active_slot()) ? " *" : "");
			cdc_send_line(buf);
		} else {
			snprintf(buf, sizeof(buf), "  SLOT%d: empty%s", i,
			         (i == bt_get_active_slot()) ? " *" : "");
			cdc_send_line(buf);
		}
	}
	cdc_send_line("OK");
}

/* BT SWITCH/PAIR/DISCONNECT/NEXT/PREV */
static void cmd_bt_action(const char *arg)
{
	if (!arg) { cdc_send_line("ERR BT: missing action"); return; }
	char buf[32];
	strncpy(buf, arg, sizeof(buf)); buf[sizeof(buf)-1] = '\0';
	trim_spaces(buf);

	if (strncasecmp(buf, "SWITCH ", 7) == 0) {
		int slot = atoi(buf + 7);
		if (slot < 0 || slot >= BT_MAX_DEVICES) { cdc_send_line("ERR BT: slot 0-2"); return; }
		bt_switch_slot((uint8_t)slot);
		cdc_send_line("BT:SWITCHING");
	} else if (strcasecmp(buf, "PAIR") == 0) {
		bt_start_pairing();
		cdc_send_line("BT:PAIRING");
	} else if (strcasecmp(buf, "DISCONNECT") == 0) {
		bt_disconnect();
		cdc_send_line("BT:DISCONNECTED");
	} else if (strcasecmp(buf, "NEXT") == 0) {
		bt_next_device();
		cdc_send_line("BT:NEXT");
	} else if (strcasecmp(buf, "PREV") == 0) {
		bt_prev_device();
		cdc_send_line("BT:PREV");
	} else {
		cdc_send_line("ERR BT: SWITCH <n>|PAIR|DISCONNECT|NEXT|PREV");
	}
}

/* AUTOSHIFT — toggle auto shift */
static void cmd_autoshift(const char *arg)
{
	(void)arg;
	auto_shift_toggle();
	cdc_send_line(auto_shift_is_enabled() ? "AUTOSHIFT:ON" : "AUTOSHIFT:OFF");
}

/* KOSET index;trigger_key,trigger_mod,result_key,result_mod (all hex) */
static void cmd_koset(const char *arg)
{
	if (!arg) { cdc_send_line("ERR KOSET: missing args"); return; }
	char buf[CDC_CMD_MAX_LEN];
	strncpy(buf, arg, sizeof(buf)); buf[sizeof(buf)-1] = '\0';
	char *sep = strchr(buf, ';');
	if (!sep) { cdc_send_line("ERR KOSET: format index;tk,tm,rk,rm"); return; }
	*sep = '\0';
	int idx = atoi(buf);
	if (idx < 0 || idx >= KEY_OVERRIDE_MAX_SLOTS) { cdc_send_line("ERR KOSET: invalid index"); return; }
	unsigned int tk, tm, rk, rm;
	if (sscanf(sep + 1, "%x,%x,%x,%x", &tk, &tm, &rk, &rm) != 4) {
		cdc_send_line("ERR KOSET: format trigger_key,trigger_mod,result_key,result_mod (hex)");
		return;
	}
	key_override_t ko = { .trigger_key = tk, .trigger_mod = tm, .result_key = rk, .result_mod = rm };
	key_override_set((uint8_t)idx, &ko);
	key_override_save();
	char resp[48];
	snprintf(resp, sizeof(resp), "KOSET %d:OK", idx);
	cdc_send_line(resp);
}

/* KO? — list key overrides */
static void cmd_kolist(const char *arg)
{
	(void)arg;
	char buf[64];
	for (int i = 0; i < KEY_OVERRIDE_MAX_SLOTS; i++) {
		const key_override_t *ko = key_override_get(i);
		if (!ko || ko->trigger_key == 0) continue;
		snprintf(buf, sizeof(buf), "KO%d: %02X+%02X->%02X+%02X",
		         i, ko->trigger_key, ko->trigger_mod, ko->result_key, ko->result_mod);
		cdc_send_line(buf);
	}
	cdc_send_line("OK");
}

/* WPM? — get current words per minute */
static void cmd_wpm_query(const char *arg)
{
	(void)arg;
	char buf[32];
	snprintf(buf, sizeof(buf), "WPM: %u", wpm_get());
	cdc_send_line(buf);
}

/* TRILAYER layer1,layer2,result — configure tri-layer */
static void cmd_trilayer(const char *arg)
{
	if (!arg) { cdc_send_line("ERR TRILAYER: layer1,layer2,result"); return; }
	int l1, l2, res;
	if (sscanf(arg, "%d,%d,%d", &l1, &l2, &res) != 3) {
		cdc_send_line("ERR TRILAYER: format layer1,layer2,result");
		return;
	}
	tri_layer_set((uint8_t)l1, (uint8_t)l2, (uint8_t)res);
	char buf[48];
	snprintf(buf, sizeof(buf), "TRILAYER: L%d+L%d=L%d", l1, l2, res);
	cdc_send_line(buf);
}

/* FEATURES? — list supported advanced features */
static void cmd_features(const char *arg)
{
	(void)arg;
	cdc_send_line("MT,LT,OSM,OSL,CAPS_WORD,REPEAT,TAP_DANCE,COMBO,LEADER,GESC,LAYER_LOCK,WPM,TRI_LAYER");
}

/* ── Command table ───────────────────────────────────────────────── */

static const cdc_cmd_entry_t keyboard_cmd_table[] = {
	/* Keymap */
	{ "SETLAYER",       8,  true,  cmd_setlayer_command },
	{ "SETKEY",         6,  true,  cmd_set_key },
	{ "KEYMAP?",        7,  false, cmd_keymap_current },
	{ "KEYMAP",         6,  true,  cmd_keymap_by_index },
	/* Layout */
	{ "LAYOUTNAME",    10,  true,  cmd_set_layout_name },
	{ "LAYOUTS?",       8,  false, cmd_layouts_q },
	{ "LAYOUT?",        7,  false, cmd_layout_json },
	/* Macros */
	{ "MACROSEQ ",     9,  true,  cmd_macro_add_seq },
	{ "MACROADD",       8,  true,  cmd_macro_add },
	{ "MACRODEL",       8,  true,  cmd_macro_delete },
	{ "MACROS?",        7,  false, cmd_macros_q },
	/* Stats */
	{ "KEYSTATS_RESET",14,  false, cmd_keystats_rst },
	{ "KEYSTATS?",      9,  false, cmd_keystats_text },
	{ "KEYSTATS",       8,  false, cmd_keystats_bin },
	{ "BIGRAMS_RESET", 13,  false, cmd_bigrams_rst },
	{ "BIGRAMS?",       8,  false, cmd_bigrams_text },
	{ "BIGRAMS",        7,  false, cmd_bigrams_bin },
	/* Tap Dance */
	{ "TDSET",          5,  true,  cmd_tdset },
	{ "TD?",            3,  false, cmd_tdlist },
	/* Combos */
	{ "COMBOSET",       8,  true,  cmd_comboset },
	{ "COMBO?",         6,  false, cmd_combolist },
	/* Leader */
	{ "LEADERSET",      9,  true,  cmd_leaderset },
	{ "LEADER?",        7,  false, cmd_leaderlist },
	/* Bluetooth */
	{ "BT?",            3,  false, cmd_bt_query },
	{ "BT ",            3,  true,  cmd_bt_action },
	/* Auto Shift / Key Override / WPM / Tri-Layer */
	{ "AUTOSHIFT",     9,  false, cmd_autoshift },
	{ "KOSET",          5,  true,  cmd_koset },
	{ "KO?",            3,  false, cmd_kolist },
	{ "WPM?",           4,  false, cmd_wpm_query },
	{ "TRILAYER ",      9,  true,  cmd_trilayer },
	/* Tama */
	{ "TAMA?",          5,  false, cmd_tama_query },
	{ "TAMA ",          5,  true,  cmd_tama_action },
	/* System */
	{ "FEATURES?",     9,  false, cmd_features },
	{ "VERSION?",       8,  false, cmd_version },
	{ "OTA ",           4,  true,  cmd_ota_start },
	{ "DFU",            3,  false, cmd_dfu },
	/* Short-form layer: L? L0 LN0 etc */
	{ "L",              1,  true,  cmd_layer_name_short },
};

void cdc_keyboard_cmds_init(void)
{
	cdc_register_commands(keyboard_cmd_table,
	                      sizeof(keyboard_cmd_table) / sizeof(keyboard_cmd_table[0]));
}
