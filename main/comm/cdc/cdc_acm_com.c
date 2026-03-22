// Gestion commandes CDC ACM
#include "cdc_acm_com.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include "esp_log.h"
#include "matrix.h"			  // current_layout, layer_changed
#include "keyboard_config.h"  // MAX_LAYER
#include "i2c_oled_display.h" // write_text_to_display
#include "tinyusb_cdc_acm.h"	  // tinyusb_cdcacm_write_queue
#include "keyboard_manager.h" // macros
#include "keymap.h"			  // default_layout_names
#include "key_definitions.h"
#include "dfu_manager.h"      // reboot_to_dfu
#include "status_display.h" // status_display_show_DFU_prog
#include "tusb.h"           // tud_cdc_n_connected

// Interface CDC utilisée (0 par défaut)
#ifndef CDC_ITF
#define CDC_ITF TINYUSB_CDC_ACM_0
#endif

static const char *TAG_CDC = "CDC_CMD";

#if BOARD_HAS_POSITION_MAP
#include "board_position_map.h"
#endif

// Forward declarations used by handlers defined above
static void cdc_send_line(const char *text);
static void trim_spaces(char *str);

// Custom log handler to redirect ESP_LOG to CDC
static vprintf_like_t original_log_vprintf = NULL;

static int cdc_log_vprintf(const char *fmt, va_list args) {
    // 1. Call original log handler (UART/JTAG) if it exists
    int ret = 0;
    if (original_log_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = original_log_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    // 2. Send to CDC if connected
    // Use a small stack buffer; truncate if too long
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
        
        // Check connection to avoid blocking/filling buffer when no host
        if (tud_cdc_n_connected(CDC_ITF)) {
            // Write to CDC; this puts data in the TinyUSB FIFO
            tinyusb_cdcacm_write_queue(CDC_ITF, (uint8_t*)buf, len);
            // Flush immediately so logs appear in real-time
            tinyusb_cdcacm_write_flush(CDC_ITF, 0);
        }
    }
    return ret;
}

// ----------------------------------------------------------------------------------
// FIFO circulaire de lignes de commandes
// ----------------------------------------------------------------------------------
static cdc_cmd_t cmd_fifo[CDC_CMD_FIFO_DEPTH];
static uint16_t fifo_w = 0; // write index
static uint16_t fifo_r = 0; // read index
static uint16_t fifo_count = 0;

// Buffer d'assemblage de la ligne courante
static char assemble_buf[CDC_CMD_MAX_LEN];
static uint16_t assemble_len = 0; 

void cdc_cmd_fifo_init(void)
{
	fifo_w = fifo_r = fifo_count = 0;
	assemble_len = 0;
}

// Commande: SETLAYER<layer>:<val1>,<val2>,...,<valN>
// Exemple: SETLAYER0:0x0004,0x0005,0x0006,... (N must be MATRIX_ROWS*MATRIX_COLS)
static void cmd_setlayer_command(const char *arg)
{
	if (arg == NULL)
	{
		cdc_send_line("ERR SETLAYER: arguments manquants");
		return;
	}

	// Copy into a static scratch buffer to keep stack usage small when CDC_CMD_MAX_LEN is large
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
		cdc_send_line("ERR SETLAYER: index ou liste vide");
		return;
	}

	int idx = atoi(idx_str);
	if (idx < 0 || idx >= LAYERS)
	{
		cdc_send_line("ERR SETLAYER: index invalide");
		return;
	}

	char dbg_msg[96];
	snprintf(dbg_msg, sizeof(dbg_msg), "DBG SETLAYER idx=%d", idx);
	cdc_send_line(dbg_msg);

	// Parse comma separated values
	size_t expected = MATRIX_ROWS * MATRIX_COLS;
	uint16_t parsed[ expected ];
	size_t count = 0;
	char *saveptr = NULL;
	char *tok = strtok_r(list_str, ",", &saveptr);
	while (tok && count < expected)
	{
		trim_spaces(tok);
		long v = strtol(tok, NULL, 0); // support 0x.. or decimal
		if (v < 0 || v > 0xFFFF)
		{
			cdc_send_line("ERR SETLAYER: valeur hors plage");
			return;
		}
		parsed[count++] = (uint16_t)v;
		tok = strtok_r(NULL, ",", &saveptr);
	}

	if (count != expected)
	{
		char msg[64];
		snprintf(msg, sizeof(msg), "ERR SETLAYER: attendu %u valeurs, recu %u", (unsigned)expected, (unsigned)count);
		cdc_send_line(msg);
		return;
	}

	// Write into keymaps (with VERSION_1 position translation)
	// Data from PC is in VERSION_2 format, translate to internal format
	extern uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS];
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

	// Persist all keymaps (size in BYTES)
	size_t total_elements = (size_t)LAYERS * MATRIX_ROWS * MATRIX_COLS;
	size_t total_bytes = total_elements * sizeof(uint16_t);
	save_keymaps((uint16_t *)keymaps, total_bytes);

	// Small readback preview of first 6 entries of this layer for host-side verification
	uint16_t preview[6] = {0};
	for (size_t i = 0; i < 6 && i < expected; ++i)
	{
		preview[i] = parsed[i];
	}

	// Split debug across two lines to avoid truncation
	snprintf(dbg_msg, sizeof(dbg_msg), "DBG SETLAYER persisted %u bytes", (unsigned)total_bytes);
	cdc_send_line(dbg_msg);
	snprintf(dbg_msg, sizeof(dbg_msg), "DBG SETLAYER preview:%04X,%04X,%04X,%04X,%04X,%04X",
			 preview[0], preview[1], preview[2], preview[3], preview[4], preview[5]);
	cdc_send_line(dbg_msg);

	// If this is the current layer, refresh display
	extern uint8_t current_layout;
	if ((uint8_t)idx == current_layout)
	{
		status_display_update_layer_name();
	}

	cdc_send_line("OK");
}

enum cdc_command_type
{
	c_nope_t = 0,
	c_get_layer_t,
	c_get_current_layer_index_t,
	c_get_name_layer_t,
	c_get_macros_t,
	c_get_all_layout_names_t,
	c_ping_t,
	c_debug_t,

};

static inline bool fifo_full(void) { return fifo_count == CDC_CMD_FIFO_DEPTH; }
static inline bool fifo_empty(void) { return fifo_count == 0; }

bool cdc_cmd_push(const char *line, uint16_t len)
{
	if (len == 0)
		return false;
	if (len >= CDC_CMD_MAX_LEN)
		len = CDC_CMD_MAX_LEN - 1; // tronquer
	if (fifo_full())
	{
		ESP_LOGW(TAG_CDC, "FIFO pleine - drop commande");
		return false;
	}
	cdc_cmd_t *slot = &cmd_fifo[fifo_w];
	memcpy(slot->line, line, len);
	slot->line[len] = '\0';
	slot->len = len;
	fifo_w = (fifo_w + 1) % CDC_CMD_FIFO_DEPTH;
	fifo_count++;
	ESP_LOGI(TAG_CDC, "CDC RX line len=%u", (unsigned)len);
	return true;
}

bool cdc_cmd_pop(cdc_cmd_t *out)
{
	if (fifo_empty())
		return false;
	*out = cmd_fifo[fifo_r];
	fifo_r = (fifo_r + 1) % CDC_CMD_FIFO_DEPTH;
	fifo_count--;
	return true;
}

bool cdc_cmd_peek(cdc_cmd_t *out)
{
	if (fifo_empty())
		return false;
	*out = cmd_fifo[fifo_r];
	return true;
}

uint16_t cdc_cmd_count(void)
{
	return fifo_count;
}

void start_command_queue(unsigned char type, size_t total_size)
{
	unsigned char data_tmp[5];
	data_tmp[0] = 'C';
	data_tmp[1] = '>';	 // start
	data_tmp[2] = type;	 // type

	if (total_size > 0x0000)
	{
		data_tmp[3] = (unsigned char)(total_size & 0xFF);
		data_tmp[4] = (unsigned char)((total_size >> 8) & 0xFF);
	}
	else
	{
		data_tmp[3] = 0;
		data_tmp[4] = 0;
	} 

	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)data_tmp, 5);
} 
void cdc_send_layer(uint8_t layer)
{
	start_command_queue(c_get_current_layer_index_t, 1);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)&layer, 1); 
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

void cdc_ping(void)
{
	start_command_queue(c_ping_t, 0);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}


// ----------------------------------------------------------------------------------
// Parsing des commandes
// Commandes proposées:
//   L<n>        => changer layer (0..MAX_LAYER)
//   DISP:<txt>  => afficher texte sur l'écran (si présent)
//   HELP        => lister commandes
//   L?          => renvoie le layer courant
// (Extension future: MACRO:<id>, etc.)
// ----------------------------------------------------------------------------------

__attribute__((unused)) static void cmd_set_layer(const char *arg)
{
	if (!isdigit((unsigned char)arg[0]))
	{
		ESP_LOGW(TAG_CDC, "Layer invalide");
		return;
	}
	int layer = atoi(arg);
	if (layer < 0 || layer > MAX_LAYER)
	{
		ESP_LOGW(TAG_CDC, "Layer hors limites (%d)", layer);
		return;
	}
	extern uint8_t current_layout;
	extern uint8_t last_layer;
	last_layer = current_layout = (uint8_t)layer;
	layer_changed();
	ESP_LOGI(TAG_CDC, "Layer -> %d", layer);
}

static void cmd_get_current_layer_index(void)
{
	extern uint8_t current_layout; 
	ESP_LOGI(TAG_CDC, "Layer courant: %d", current_layout);
	start_command_queue(c_get_current_layer_index_t, 1);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)&current_layout, 1); 
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

__attribute__((unused)) static void cmd_display_text(const char *txt)
{
	if (txt == NULL || *txt == '\0')
	{
		ESP_LOGW(TAG_CDC, "Texte vide");
		return;
	}
	write_text_to_display((char *)txt, 0, 8); // simple position
	ESP_LOGI(TAG_CDC, "Affiche: %s", txt);
	// Accusé de réception
	char ack[16] = "OK\r\n";
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)ack, 4);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
	// send_data(ack, strlen(ack));
}

static void cmd_get_name_layer(uint8_t layer)
{
	if (layer >= LAYERS) {
		cdc_send_line("ERROR:INVALID_LAYER");
		return;
	}
	// Réponse binaire: [index (1 octet)] [nom (len octets, sans '\0')]
	const char *name = default_layout_names[layer];
	size_t name_len = strlen(name);
	size_t total_size = 1 + name_len;
	start_command_queue(c_get_name_layer_t, total_size);
	uint8_t idx = (uint8_t)layer;
	tinyusb_cdcacm_write_queue(CDC_ITF, &idx, 1);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)name, name_len);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

// Liste tous les noms de layouts dans une seule trame binaire
static void cmd_list_layout_names(void)
{
	// Format payload:
	// [LAYERS (1 octet)] puis pour chaque layout:
	//   [index (1 octet)] [nom (N octets, sans '\0')] [';' (1 octet separateur)]

	// Calcul longueur totale
	size_t total_size = 1; // nombre de layers
	for (int i = 0; i < LAYERS; ++i)
	{
		total_size += 1 + strlen(default_layout_names[i]) + 1; // index + nom + separateur
	}

	start_command_queue(c_get_all_layout_names_t, total_size);

	uint8_t layers_count = (uint8_t)LAYERS;
	tinyusb_cdcacm_write_queue(CDC_ITF, &layers_count, 1);

	for (int i = 0; i < LAYERS; ++i)
	{
		uint8_t idx = (uint8_t)i;
		const char *name = default_layout_names[i];
		size_t name_len = strlen(name);

		tinyusb_cdcacm_write_queue(CDC_ITF, &idx, 1);//index
		tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)name, name_len);
		// Ajout d'un caractere de separation entre les noms
		uint8_t sep = ';';
		tinyusb_cdcacm_write_queue(CDC_ITF, &sep, 1);
	}

	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

static void cmd_get_keymap_by_layer(uint8_t layer)
{
	// Envoie le keymap complet de la layer demandée sous forme binaire (MATRIX_ROWS * MATRIX_COLS * 2 bytes)
	if (layer >= LAYERS)
	{
		ESP_LOGW(TAG_CDC, "Layer %u invalide", (unsigned)layer);
		return;
	}
	extern uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS];
	size_t total_size = (MATRIX_ROWS * MATRIX_COLS * sizeof(uint16_t)) + 2; 
	start_command_queue(c_get_layer_t, total_size);
	unsigned char bytes[2];
	bytes[0] = (unsigned char)(layer & 0xFF);
	bytes[1] = (unsigned char)((layer >> 8) & 0xFF);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)bytes, 2); 
	// Envoi de la matrice (traduite en format VERSION_2 pour le PC)
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

// Commande: SETKEY layer,row,col,value
static void cmd_set_key(const char *arg)
{
	// Format attendu: layer,row,col,value
	int layer, row, col;
	unsigned int value;
	if (sscanf(arg, "%d,%d,%d,%x", &layer, &row, &col, &value) != 4)
	{
		ESP_LOGW(TAG_CDC, "SETKEY: format invalide");
		// send_data("ERR format\r\n", 12);
		return;
	}
	if (layer < 0 || layer >= LAYERS || row < 0 || row >= MATRIX_ROWS || col < 0 || col >= MATRIX_COLS)
	{
		ESP_LOGW(TAG_CDC, "SETKEY: index hors limites");
		return;
	}
	// Translate (row,col) from CDC (VERSION_2 format) to internal format
#if BOARD_HAS_POSITION_MAP
	int internal_row, internal_col;
	v2_to_v1_pos(row, col, &internal_row, &internal_col);
#else
	int internal_row = row;
	int internal_col = col;
#endif
	extern uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS];
	keymaps[layer][internal_row][internal_col] = (uint16_t)value;
	ESP_LOGI(TAG_CDC, "SETKEY: [%d][%d][%d] = 0x%04X (v2: r%d,c%d)", layer, internal_row, internal_col, value, row, col);
	size_t total_elements = (size_t)LAYERS * MATRIX_ROWS * MATRIX_COLS;
	save_keymaps((uint16_t *)keymaps, total_elements * sizeof(uint16_t));
	// send_data("OK\r\n", 5);
}

static void cdc_send_line(const char *text)
{
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)text, strlen(text));
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)"\r\n", 2);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

static void cdc_send_binary(const uint8_t *data, size_t len)
{
	tinyusb_cdcacm_write_queue(CDC_ITF, data, len);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

static void cdc_send_large(const char *data, size_t len)
{
	const size_t chunk = 256;
	while (len > 0) {
		size_t n = (len < chunk) ? len : chunk;
		tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)data, n);
		tinyusb_cdcacm_write_flush(CDC_ITF, pdMS_TO_TICKS(100));
		data += n;
		len -= n;
	}
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)"\r\n", 2);
	tinyusb_cdcacm_write_flush(CDC_ITF, pdMS_TO_TICKS(100));
}

static void trim_spaces(char *str)
{
	if (str == NULL)
		return;
	char *start = str;
	while (*start && isspace((unsigned char)*start))
		start++;
	char *end = start + strlen(start);
	while (end > start && isspace((unsigned char)*(end - 1)))
		end--;
	size_t len = (size_t)(end - start);
	if (start != str)
		memmove(str, start, len);
	str[len] = '\0';
}

// Commande: LAYOUTNAME<layer>:<nouveau_nom>
// Exemple: LAYOUTNAME0:AZERTY_FR
static void  cmd_set_layout_name(const char *arg)
{
	if (arg == NULL)
	{
		cdc_send_line("ERR LAYOUTNAME: arguments manquants");
		return;
	}

	char buffer[CDC_CMD_MAX_LEN];
	strncpy(buffer, arg, sizeof(buffer));
	buffer[sizeof(buffer) - 1] = '\0';

	char *sep = strchr(buffer, ':');
	if (!sep)
	{
		cdc_send_line("ERR LAYOUTNAME: format LAYOUTNAME<idx>:<nom>");
		return;
	}

	*sep = '\0';
	char *idx_str = buffer;
	char *name_str = sep + 1;

	trim_spaces(idx_str);
	trim_spaces(name_str);

	if (*idx_str == '\0' || *name_str == '\0')
	{
		cdc_send_line("ERR LAYOUTNAME: index ou nom vide");
		return;
	}

	int idx = atoi(idx_str);
	if (idx < 0 || idx >= LAYERS)
	{
		cdc_send_line("ERR LAYOUTNAME: index invalide");
		return;
	}

	strncpy(default_layout_names[idx], name_str, MAX_LAYOUT_NAME_LENGTH - 1);
	default_layout_names[idx][MAX_LAYOUT_NAME_LENGTH - 1] = '\0';

	// Sauvegarde en NVS via keymap.c
	save_layout_names(default_layout_names, LAYERS);

	// Si c'est le layout courant, rafraîchir l'affichage OLED
	extern uint8_t current_layout;
	void status_display_update_layer_name(void);
	if ((uint8_t)idx == current_layout)
	{
		status_display_update_layer_name();
	}

	// Réponse binaire: même format que cmd_get_name_layer
	const char *name = default_layout_names[idx];
	size_t name_len = strlen(name);
	size_t total_size = 1 + name_len;
	start_command_queue(c_get_name_layer_t, total_size);
	uint8_t uidx = (uint8_t)idx;
	tinyusb_cdcacm_write_queue(CDC_ITF, &uidx, 1);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)name, name_len);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

static uint16_t macro_keycode_from_index(size_t idx)
{
	return (uint16_t)(MACRO_1 + (idx * 0x0100));
}

static void cmd_list_macros(void)
{
	// Format binaire via start_command_queue, pour MACROS?
	// Payload:
	//   [count (1 octet)]
	//   pour chaque macro définie:
	//     [index (1 octet)]
	//     [keycode (2 octets, little-endian)]
	//     [nameLen (1 octet)]
	//     [name (nameLen octets, sans '\0')]
	//     [keysLen (1 octet)]
	//     [keys (keysLen octets)]

	// 1) Calculer la taille totale
	size_t count = 0;
	size_t total_size = 1; // count
	for (size_t i = 0; i < MAX_MACROS; ++i)
	{
		if (macros_list[i].name[0] == '\0')
			continue;
		++count;
		size_t name_len = strlen(macros_list[i].name);
		if (name_len > 255)
			name_len = 255; // borne par 1 octet
		uint8_t keys_len = 0;
		for (int k = 0; k < 6; ++k)
		{
			if (macros_list[i].keys[k] != 0)
				++keys_len;
		}
		total_size += 1 + 2 + 1 + name_len + 1 + keys_len;
	}

	start_command_queue(c_get_macros_t, total_size);
	uint8_t ucount = (uint8_t)count;
	tinyusb_cdcacm_write_queue(CDC_ITF, &ucount, 1);

	// 2) Envoyer chaque macro définie
	for (size_t i = 0; i < MAX_MACROS; ++i)
	{
		if (macros_list[i].name[0] == '\0')
			continue;

		uint8_t idx = (uint8_t)i;
		uint16_t kc = macro_keycode_from_index(i);
		uint8_t kc_bytes[2] = {
			(uint8_t)(kc & 0xFF),
			(uint8_t)((kc >> 8) & 0xFF)
		};
		size_t name_len = strlen(macros_list[i].name);
		if (name_len > 255)
			name_len = 255;
		uint8_t name_len_u8 = (uint8_t)name_len;
		uint8_t keys_len = 0;
		for (int k = 0; k < 6; ++k)
		{
			if (macros_list[i].keys[k] != 0)
				++keys_len;
		}

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
			{
				if (macros_list[i].keys[k] != 0)
				{
					if (pos < sizeof(tmp_keys))
						tmp_keys[pos++] = macros_list[i].keys[k];
				}
			}
			if (pos > 0)
			{
				tinyusb_cdcacm_write_queue(CDC_ITF, tmp_keys, pos);
			}
		}
	}

	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

static void cmd_macro_add(const char *arg)
{
	if (arg == NULL)
	{
		cdc_send_line("ERR MACROADD: arguments manquants");
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
		cdc_send_line("ERR MACROADD format: MACROADD slot;nom;hex1,hex2,...");
		return;
	}
	trim_spaces(slot_str);
	trim_spaces(name_str);
	trim_spaces(keys_str);
	if (*slot_str == '\0' || *name_str == '\0' || *keys_str == '\0')
	{
		cdc_send_line("ERR MACROADD: parametre vide");
		return;
	}
	int slot = atoi(slot_str);
	if (slot < 0 || slot >= MAX_MACROS)
	{
		cdc_send_line("ERR MACROADD: slot invalide");
		return;
	}
	uint8_t parsed_keys[6] = {0};
	char *keys_save = NULL;
	char *token = strtok_r(keys_str, ",", &keys_save);
	int key_index = 0;
	while (token && key_index < 6)
	{
		trim_spaces(token);
		if (*token == '\0')
		{
			token = strtok_r(NULL, ",", &keys_save);
			continue;
		}
		char *endptr = NULL;
		unsigned long value = strtoul(token, &endptr, 0);
		if (endptr == token || value > 0xFF)
		{
			cdc_send_line("ERR MACROADD: code touche invalide");
			return;
		}
		parsed_keys[key_index++] = (uint8_t)value;
		token = strtok_r(NULL, ",", &keys_save);
	}
	if (key_index == 0)
	{
		cdc_send_line("ERR MACROADD: aucune touche fournie");
		return;
	}
	while (key_index < 6)
	{
		parsed_keys[key_index++] = 0;
	}
	strncpy(macros_list[slot].name, name_str, MAX_MACRO_NAME_LENGTH - 1);
	macros_list[slot].name[MAX_MACRO_NAME_LENGTH - 1] = '\0';
	memcpy(macros_list[slot].keys, parsed_keys, sizeof(parsed_keys));
	macros_list[slot].key_definition = macro_keycode_from_index((size_t)slot);
	if ((size_t)(slot + 1) > macros_count)
	{
		macros_count = slot + 1;
	}
	save_macros(macros_list, macros_count);
	char msg[64];
	snprintf(msg, sizeof(msg), "MACRO %d enregistree", slot);
	cdc_send_line(msg);
}

static void cmd_macro_delete(const char *arg)
{
	if (arg == NULL)
	{
		cdc_send_line("ERR MACRODEL: arguments manquants");
		return;
	}
	char buffer[CDC_CMD_MAX_LEN];
	strncpy(buffer, arg, sizeof(buffer));
	buffer[sizeof(buffer) - 1] = '\0';
	trim_spaces(buffer);
	if (*buffer == '\0')
	{
		cdc_send_line("ERR MACRODEL: slot manquant");
		return;
	}
	int slot = atoi(buffer);
	if (slot < 0 || slot >= MAX_MACROS)
	{
		cdc_send_line("ERR MACRODEL: slot invalide");
		return;
	}
	if (macros_list[slot].name[0] == '\0')
	{
		cdc_send_line("MACRODEL: slot deja vide");
		return;
	}
	macros_list[slot].name[0] = '\0';
	memset(macros_list[slot].keys, 0, sizeof(macros_list[slot].keys));
	macros_list[slot].key_definition = macro_keycode_from_index((size_t)slot);
	recalc_macros_count();
	save_macros(macros_list, macros_count);
	char msg[64];
	snprintf(msg, sizeof(msg), "MACRO %d supprimee", slot);
	cdc_send_line(msg);
}


static void cmd_reboot_to_dfu(void)
{
	cdc_send_line("Rebooting to DFU...");
	status_display_show_DFU_prog();
	vTaskDelay(pdMS_TO_TICKS(300));
	reboot_to_dfu();
}

/**
 * @brief Send key usage statistics via CDC
 * Format: Binary packet with header + data
 * Header: "KEYSTATS" (8) + rows (1) + cols (1) + hw_version (1) + total_presses (4) + max_presses (4) = 19 bytes
 * Data: For each key [row][col] in V2 order: 4 bytes (uint32_t LE) = press count
 * Total: 19 + rows*cols*4 bytes, followed by "\r\n"
 * 
 * Or if "KEYSTATS?" text format: human readable
 */
static void cmd_get_keystats(bool binary)
{
	if (binary) {
		/* Binary format for heatmap visualization */
		uint8_t header[19];
		memcpy(header, "KEYSTATS", 8);
		header[8] = MATRIX_ROWS;
		header[9] = MATRIX_COLS;
		header[10] = MODULE_ID;  /* Hardware version: 0x01=V1, 0x02=V2/V2_DEBUG */

		/* Total presses (little-endian) */
		header[11] = (key_stats_total >> 0) & 0xFF;
		header[12] = (key_stats_total >> 8) & 0xFF;
		header[13] = (key_stats_total >> 16) & 0xFF;
		header[14] = (key_stats_total >> 24) & 0xFF;

		/* Max presses for normalization */
		uint32_t max_val = get_key_stats_max();
		header[15] = (max_val >> 0) & 0xFF;
		header[16] = (max_val >> 8) & 0xFF;
		header[17] = (max_val >> 16) & 0xFF;
		header[18] = (max_val >> 24) & 0xFF;
		
		/* Send header + all data in one batch to avoid USB fragmentation */
		uint8_t payload[19 + MATRIX_ROWS * MATRIX_COLS * 4];
		memcpy(payload, header, 19);
		size_t offset = 19;

		/* Send stats in V2 order so PC always gets consistent layout */
		for (int r = 0; r < MATRIX_ROWS; r++) {
			for (int c = 0; c < MATRIX_COLS; c++) {
#if BOARD_HAS_POSITION_MAP
				/* Translate V2 position back to V1 to read the correct counter */
				int v1_row, v1_col;
				v2_to_v1_pos(r, c, &v1_row, &v1_col);
				uint32_t count = key_stats[v1_row][v1_col];
#else
				uint32_t count = key_stats[r][c];
#endif
				payload[offset++] = (count >> 0) & 0xFF;
				payload[offset++] = (count >> 8) & 0xFF;
				payload[offset++] = (count >> 16) & 0xFF;
				payload[offset++] = (count >> 24) & 0xFF;
			}
		}
		tinyusb_cdcacm_write_queue(CDC_ITF, payload, offset);
		tinyusb_cdcacm_write_flush(CDC_ITF, 0);
		cdc_send_line("");  /* End marker */
	} else {
		/* Text format for debugging */
		char buf[128];
		snprintf(buf, sizeof(buf), "Key Statistics - Total: %lu, Max: %lu", 
				(unsigned long)key_stats_total, (unsigned long)get_key_stats_max());
		cdc_send_line(buf);
		
		for (int r = 0; r < MATRIX_ROWS; r++) {
			char line[256] = "";
			int pos = 0;
			pos += snprintf(line + pos, sizeof(line) - pos, "R%d: ", r);
			for (int c = 0; c < MATRIX_COLS; c++) {
				pos += snprintf(line + pos, sizeof(line) - pos, "%5lu ", (unsigned long)key_stats[r][c]);
			}
			cdc_send_line(line);
		}
		cdc_send_line("OK");
	}
}

/**
 * @brief Send bigram statistics via CDC
 * Binary: header + top-N bigrams (sorted by count desc)
 * Text: human-readable top-N list
 *
 * Binary header: "BIGRAMS\0" (8) + hw_version (1) + num_keys (1) + total (4) + max (2) + count_entries (2) = 18 bytes
 * Each entry: prev_key (1) + curr_key (1) + count (2) = 4 bytes
 */
static void cmd_get_bigrams(bool binary)
{
	if (binary) {
		/* Collect non-zero bigrams */
		typedef struct { uint8_t prev; uint8_t curr; uint16_t count; } bigram_entry_t;
		/* Max entries we can send — limit to 256 to keep packet reasonable */
		#define MAX_BIGRAM_ENTRIES 256
		bigram_entry_t *entries = malloc(MAX_BIGRAM_ENTRIES * sizeof(bigram_entry_t));
		if (!entries) {
			cdc_send_line("ERROR:OOM");
			return;
		}
		uint16_t n_entries = 0;

		/* Find all non-zero entries, keep top MAX_BIGRAM_ENTRIES by insertion */
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
					/* Replace the smallest entry */
					uint16_t min_idx = 0;
					for (uint16_t k = 1; k < MAX_BIGRAM_ENTRIES; k++)
						if (entries[k].count < entries[min_idx].count)
							min_idx = k;
					entries[min_idx].prev = (uint8_t)i;
					entries[min_idx].curr = (uint8_t)j;
					entries[min_idx].count = bigram_stats[i][j];
					/* Recalculate min */
					min_in_list = entries[0].count;
					for (uint16_t k = 1; k < MAX_BIGRAM_ENTRIES; k++)
						if (entries[k].count < min_in_list)
							min_in_list = entries[k].count;
				}
			}
		}

		/* Sort by count descending (simple insertion sort, N is small) */
		for (uint16_t i = 1; i < n_entries; i++) {
			bigram_entry_t tmp = entries[i];
			int j = i - 1;
			while (j >= 0 && entries[j].count < tmp.count) {
				entries[j + 1] = entries[j];
				j--;
			}
			entries[j + 1] = tmp;
		}

		/* Build packet: header (18 bytes) + entries (4 bytes each) */
		uint16_t max_val = get_bigram_stats_max();
		uint8_t header[18];
		memcpy(header, "BIGRAMS", 7);
		header[7] = 0;
		header[8] = MODULE_ID;
		header[9] = (uint8_t)NUM_KEYS;
		header[10] = (bigram_total >> 0) & 0xFF;
		header[11] = (bigram_total >> 8) & 0xFF;
		header[12] = (bigram_total >> 16) & 0xFF;
		header[13] = (bigram_total >> 24) & 0xFF;
		header[14] = (max_val >> 0) & 0xFF;
		header[15] = (max_val >> 8) & 0xFF;
		header[16] = (n_entries >> 0) & 0xFF;
		header[17] = (n_entries >> 8) & 0xFF;

		size_t total_size = 18 + n_entries * 4;
		uint8_t *payload = malloc(total_size);
		if (!payload) {
			free(entries);
			cdc_send_line("ERROR:OOM");
			return;
		}
		memcpy(payload, header, 18);
		for (uint16_t i = 0; i < n_entries; i++) {
			size_t off = 18 + i * 4;
			payload[off + 0] = entries[i].prev;
			payload[off + 1] = entries[i].curr;
			payload[off + 2] = (entries[i].count >> 0) & 0xFF;
			payload[off + 3] = (entries[i].count >> 8) & 0xFF;
		}
		tinyusb_cdcacm_write_queue(CDC_ITF, payload, total_size);
		tinyusb_cdcacm_write_flush(CDC_ITF, 0);
		free(payload);
		free(entries);
		cdc_send_line("");
	} else {
		/* Text format: top 20 bigrams */
		char buf[128];
		snprintf(buf, sizeof(buf), "Bigram Statistics - Total: %lu, Max: %u",
				(unsigned long)bigram_total, get_bigram_stats_max());
		cdc_send_line(buf);

		/* Collect top 20 */
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
		/* Sort desc */
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


static void parse_and_execute(const char *line)
{
	// Trim espaces
	while (*line == ' ' || *line == '\t')
		line++;
	if (*line == '\0')
		return;
	if (strncasecmp(line, "L?", 2) == 0)
	{
		cmd_get_current_layer_index();
		return;
	}
	if (toupper((unsigned char)line[0]) == 'L' && toupper((unsigned char)line[1]) == 'N' && isdigit((unsigned char)line[2]))
	{
		cmd_get_name_layer((unsigned char)(line[2] - '0'));
		return;
	}
	if (toupper((unsigned char)line[0]) == 'L' && isdigit((unsigned char)line[1]))
	{
		//cmd_set_layer(line + 1);
		cmd_get_name_layer((unsigned char)(line[1] - '0'));
		return;
	}
	// if (strncasecmp(line, "DISP:", 5) == 0) { cmd_display_text(line + 5); return; }
	if (strncasecmp(line, "KEYMAP?", 7) == 0)
	{
		extern uint8_t current_layout;
		cmd_get_keymap_by_layer(current_layout);
		return;
	}
	if (strncasecmp(line, "KEYMAP", 6) == 0 && isdigit((unsigned char)line[6]))
	{
		cmd_get_keymap_by_layer((uint8_t)atoi(line + 6));
		return;
	}
	if (strncasecmp(line, "SETKEY", 6) == 0)
	{
		cmd_set_key(line + 6);
		return;
	}
	if (strncasecmp(line, "SETLAYER", 8) == 0)
	{
		cmd_setlayer_command(line + 8);
		return;
	}
	if (strncasecmp(line, "MACROS?", 7) == 0)
	{
		cmd_list_macros();
		return;
	}
	if (strncasecmp(line, "MACROADD", 8) == 0)
	{
		cmd_macro_add(line + 8);
		return;
	}
	if (strncasecmp(line, "MACRODEL", 8) == 0)
	{
		cmd_macro_delete(line + 8);
		return;
	}
	if (strncasecmp(line, "LAYOUT?", 7) == 0 && (line[7] == '\0' || line[7] == 'S'))
	{
		if (line[7] == '\0') {
			/* LAYOUT? — return physical key position JSON */
			extern const char board_layout_json[];
			cdc_send_large(board_layout_json, strlen(board_layout_json));
			return;
		}
		/* fall through to LAYOUTS? below */
	}
	if (strncasecmp(line, "LAYOUTS?", 8) == 0)
	{
		cmd_list_layout_names();
		return;
	}
	if (strncasecmp(line, "LAYOUTNAME", 10) == 0)
	{
		cmd_set_layout_name(line + 10);
		return;
	}
	if (strncasecmp(line, "DFU", 3) == 0)
	{
		cmd_reboot_to_dfu();
		return;
	}
	if (strncasecmp(line, "KEYSTATS?", 9) == 0)
	{
		cmd_get_keystats(false);  /* Text format */
		return;
	}
	if (strncasecmp(line, "KEYSTATS", 8) == 0 && (line[8] == '\0' || line[8] == ' '))
	{
		cmd_get_keystats(true);  /* Binary format for heatmap */
		return;
	}
	if (strncasecmp(line, "KEYSTATS_RESET", 14) == 0)
	{
		cmd_reset_keystats();
		return;
	}
	if (strncasecmp(line, "BIGRAMS?", 8) == 0)
	{
		cmd_get_bigrams(false);
		return;
	}
	if (strncasecmp(line, "BIGRAMS", 7) == 0 && (line[7] == '\0' || line[7] == ' '))
	{
		cmd_get_bigrams(true);
		return;
	}
	if (strncasecmp(line, "BIGRAMS_RESET", 13) == 0)
	{
		cmd_reset_bigrams();
		return;
	}
	ESP_LOGW(TAG_CDC, "Commande inconnue: %s", line);
	// ...existing code...
	
}

// ----------------------------------------------------------------------------------
// Réception de chunks USB CDC -> assemblage en lignes terminées par \n ou \r
// ----------------------------------------------------------------------------------
void receive_data(const char *data, uint16_t len)
{
	static bool last_was_cr = false; // pour gérer CRLF
	static bool overflowed = false;  // si une ligne dépasse CDC_CMD_MAX_LEN, on drop jusqu'à prochain \r/\n
	for (size_t i = 0; i < len; ++i)
	{
		char c = data[i];
		if (c == '\r')
		{
			// Fin de ligne sur CR
			if (!overflowed && assemble_len > 0)
			{
				cdc_cmd_push(assemble_buf, assemble_len);
				assemble_len = 0;
			}
			overflowed = false;
			last_was_cr = true;
			continue;
		}
		if (c == '\n')
		{
			// Si juste après un CR => déjà flush (CRLF), on ignore
			if (last_was_cr)
			{
				last_was_cr = false;
				continue;
			}
			if (!overflowed && assemble_len > 0)
			{
				cdc_cmd_push(assemble_buf, assemble_len);
				assemble_len = 0;
			}
			overflowed = false;
			continue;
		}
		last_was_cr = false;
		if (!overflowed && assemble_len < (CDC_CMD_MAX_LEN - 1))
		{
			assemble_buf[assemble_len++] = c;
		}
		else
		{
			// overflow - drop remainder until end-of-line
			overflowed = true;
			ESP_LOGW(TAG_CDC, "CDC RX overflow (len>=%u)", (unsigned)CDC_CMD_MAX_LEN);
		}
	}
}

// ----------------------------------------------------------------------------------
// Tâche de traitement (à créer dans main si souhaité)
// ----------------------------------------------------------------------------------
void cdc_process_commands_task(void *arg)
{
	cdc_cmd_t cmd;
	for (;;)
	{
		while (cdc_cmd_pop(&cmd))
		{
			parse_and_execute(cmd.line);
		}

		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

void init_cdc_commands(void)
{
	cdc_cmd_fifo_init();
	
	// Disabled: log redirection to CDC causes watchdog issues
	// original_log_vprintf = esp_log_set_vprintf(cdc_log_vprintf);

	// Increase stack depth to cope with larger CDC_CMD_MAX_LEN buffers
	xTaskCreate(cdc_process_commands_task, "cdc_cmd", 6144, NULL, 4, NULL);
}
