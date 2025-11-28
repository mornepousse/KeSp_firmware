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
#include "tusb_cdc_acm.h"	  // tinyusb_cdcacm_write_queue
#include "keyboard_manager.h" // macros
#include "keymap.h"			  // default_layout_names
#include "key_definitions.h"

// Interface CDC utilisée (0 par défaut)
#ifndef CDC_ITF
#define CDC_ITF TINYUSB_CDC_ACM_0
#endif

static const char *TAG_CDC = "CDC_CMD";

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

enum cdc_command_type
{
	c_nope_t = 0,
	c_get_layer_t,
	c_get_current_layer_index_t,
	c_get_name_layer_t,
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
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
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

void cdc_debug(const char *msg)
{
	size_t len = strlen(msg);
	//if (len > 0)
	//{
		start_command_queue(c_debug_t, len);
		tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)msg, len);
		tinyusb_cdcacm_write_flush(CDC_ITF, 0);
	//}
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

static void cmd_set_layer(const char *arg)
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

static void cmd_display_text(const char *txt)
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
	// Envoie la liste des noms de layers, séparés par des \n 
	size_t total_size = strlen(default_layout_names[layer]);
	start_command_queue(c_get_name_layer_t, total_size);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)default_layout_names[layer], total_size); 
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
	//tinyusb_cdcacm_write_queue(CDC_ITF, layer, 2); 
	unsigned char bytes[2];
	bytes[0] = (unsigned char)(layer & 0xFF);
	bytes[1] = (unsigned char)((layer >> 8) & 0xFF);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)bytes, 2); 
	// Envoi de la matrice
	for (int r = 0; r < MATRIX_ROWS; r++)
	{
		for (int c = 0; c < MATRIX_COLS; c++)
		{
			uint16_t code = keymaps[layer][r][c];
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
		// send_data("ERR index\r\n", 12);
		return;
	}
	extern uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS];
	keymaps[layer][row][col] = (uint16_t)value;
	//cdc_debug($"SETKEY OK %d,%d,%d", layer, row, col, value);
	ESP_LOGI(TAG_CDC, "SETKEY: [%d][%d][%d] = 0x%04X", layer, row, col, value);
	save_keymaps((uint16_t *)keymaps, LAYERS * MATRIX_ROWS * MATRIX_COLS);
	// send_data("OK\r\n", 5);
}

static void cdc_send_line(const char *text)
{
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)text, strlen(text));
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)"\r\n", 2);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
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

static uint16_t macro_keycode_from_index(size_t idx)
{
	return (uint16_t)(MACRO_1 + (idx * 0x0100));
}

static void cmd_list_macros(void)
{
	char line[160];
	bool any = false;
	for (size_t i = 0; i < MAX_MACROS; ++i)
	{
		if (macros_list[i].name[0] == '\0')
			continue;
		any = true;
		int len = snprintf(line, sizeof(line), "[%02u] %s (0x%04X) keys:", (unsigned)i,
				   macros_list[i].name, macro_keycode_from_index(i));
		for (int k = 0; k < 6 && len < (int)sizeof(line); ++k)
		{
			if (macros_list[i].keys[k] == 0)
				continue;
			len += snprintf(line + len, sizeof(line) - len, " 0x%02X", macros_list[i].keys[k]);
		}
		cdc_send_line(line);
	}
	if (!any)
	{
		cdc_send_line("Aucune macro definie");
	}
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

static void parse_and_execute(const char *line)
{
	//cdc_debug("RX:");
	// ESP_LOGI(TAG_CDC, "Parsing command: %s", line); 
	// size_t line_length = strlen(line);
	// uint8_t next = 0;
	// size_t i = 0;
	// uint8_t previousRxByte = 0;
	// for (; i < line_length; i++)
	// {
	// 	if (i > 0 && line[i] == '>' && previousRxByte == 'C')
	// 	{
	// 		next = 1;
	// 		break;
	// 	}
	// 	previousRxByte = line[i];
	// }
	// if (next == 0)
	// 	return;
	// next = 0;

	// enum cdc_command_type type_command = (enum cdc_command_type)line[i];
	// i++;
	// uint16_t data_size = (line[i] | (line[i + 1] << 8));
	// i += 2;
	// const char *data = line + i;
	
	// switch (type_command)
	// {
	// 	case c_nope_t:
	// 	/* code */
	// 	break;
	// 	case c_get_layer_t:
	// 		cmd_get_keymap_by_layer((uint8_t)data[0]);
	// 	break;
	// 	case c_get_current_layer_index_t:
	// 		cmd_get_current_layer_index();
	// 		break;
	// 	case c_get_name_layer_t:
	// 		cmd_get_name_layer((uint8_t)data[0]);
	// 		break;
	// 	default:
	// 	break;
	// }

	
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
		cmd_get_name_layer((unsigned char)line[2]);
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
	ESP_LOGW(TAG_CDC, "Commande inconnue: %s", line);
	// ...existing code...
	
}

// ----------------------------------------------------------------------------------
// Réception de chunks USB CDC -> assemblage en lignes terminées par \n ou \r
// ----------------------------------------------------------------------------------
void receive_data(const char *data, uint16_t len)
{
	static bool last_was_cr = false; // pour gérer CRLF
	for (size_t i = 0; i < len; ++i)
	{
		char c = data[i];
		if (c == '\r')
		{
			// Fin de ligne sur CR
			if (assemble_len > 0)
			{
				cdc_cmd_push(assemble_buf, assemble_len);
				assemble_len = 0;
			}
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
			if (assemble_len > 0)
			{
				cdc_cmd_push(assemble_buf, assemble_len);
				assemble_len = 0;
			}
			continue;
		}
		last_was_cr = false;
		if (assemble_len < (CDC_CMD_MAX_LEN - 1))
		{
			assemble_buf[assemble_len++] = c;
		}
		else
		{
			// overflow - flush implicite
			cdc_cmd_push(assemble_buf, assemble_len);
			assemble_len = 0;
		}
	}
}

// ----------------------------------------------------------------------------------
// Tâche de traitement (à créer dans main si souhaité)
// ----------------------------------------------------------------------------------
void cdc_process_commands_task(void *arg)
{
	//ESP_LOGI(TAG_CDC, "C");
	//cdc_debug("C");
	cdc_cmd_t cmd;
	for (;;)
	{
		while (cdc_cmd_pop(&cmd))
		{
			parse_and_execute(cmd.line);
			//cdc_debug("C");
		}

		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

void init_cdc_commands(void)
{
	cdc_cmd_fifo_init();
	xTaskCreate(cdc_process_commands_task, "cdc_cmd", 3072, NULL, 4, NULL);
}
