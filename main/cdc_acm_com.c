// Gestion commandes CDC ACM
#include "cdc_acm_com.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include "esp_log.h"
#include "matrix.h"          // current_layout, layer_changed
#include "keyboard_config.h"   // MAX_LAYER
#include "i2c_oled_display.h"  // write_text_to_display
#include "tusb_cdc_acm.h"      // tinyusb_cdcacm_write_queue
#include "keyboard_manager.h"  // macros
#include "keymap.h"           // default_layout_names

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

static inline bool fifo_full(void) { return fifo_count == CDC_CMD_FIFO_DEPTH; }
static inline bool fifo_empty(void) { return fifo_count == 0; }

bool cdc_cmd_push(const char *line, uint16_t len)
{
	if (len == 0) return false;
	if (len >= CDC_CMD_MAX_LEN) len = CDC_CMD_MAX_LEN - 1; // tronquer
	if (fifo_full()) {
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
	if (fifo_empty()) return false;
	*out = cmd_fifo[fifo_r];
	fifo_r = (fifo_r + 1) % CDC_CMD_FIFO_DEPTH;
	fifo_count--;
	return true;
}

bool cdc_cmd_peek(cdc_cmd_t *out)
{
	if (fifo_empty()) return false;
	*out = cmd_fifo[fifo_r];
	return true;
}

uint16_t cdc_cmd_count(void)
{
	return fifo_count;
}

void start_queue(void)
{
	char prefix[16];
	snprintf(prefix, sizeof(prefix), "C>");
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t*)prefix, strlen(prefix));
}

void end_queue(void)
{
	const char *suffix = "<C";
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t*)suffix, strlen(suffix));
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
static void cmd_help(void)
{
	ESP_LOGI(TAG_CDC, "Commandes: L<n>, L?, LAYERS?, DISP:<txt>, MACRO?, MACRO<n>=k1,k2,..., HELP");
	char help[] = "Cmd: L<n>, L?, LAYERS?, DISP:<txt>, MACRO?, MACRO<n>=k1,k2,..., HELP\r\n";
	//tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t*)help, strlen(help));
	send_data(help, strlen(help));
}

static void cmd_set_layer(const char *arg)
{
	if (!isdigit((unsigned char)arg[0])) {
		ESP_LOGW(TAG_CDC, "Layer invalide");
		return;
	}
	int layer = atoi(arg);
	if (layer < 0 || layer > MAX_LAYER) {
		ESP_LOGW(TAG_CDC, "Layer hors limites (%d)", layer);
		return;
	}
	extern uint8_t current_layout;
	extern uint8_t last_layer;
	last_layer = current_layout = (uint8_t)layer;
	layer_changed();
	ESP_LOGI(TAG_CDC, "Layer -> %d", layer);
}

static void cmd_show_layer(void)
{
	extern uint8_t current_layout;
	ESP_LOGI(TAG_CDC, "Layer courant: %d", current_layout);
	char out[32];
	int n = snprintf(out, sizeof(out), "L=%d\r\n", current_layout);
	if (n > 0) {
		//(void)tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t*)out, (size_t)n);
		//(void)tinyusb_cdcacm_write_flush(CDC_ITF, 0);
		send_data(out, strlen(out));
	}
}

static void cmd_display_text(const char *txt)
{
	if (txt == NULL || *txt == '\0') {
		ESP_LOGW(TAG_CDC, "Texte vide");
		return;
	}
	write_text_to_display((char*)txt, 0, 8); // simple position
	ESP_LOGI(TAG_CDC, "Affiche: %s", txt);
	// Accusé de réception
	char ack[16] = "OK\r\n";
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t*)ack, 4);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
	//send_data(ack, strlen(ack));
}

static void cmd_list_layers(void)
{ 
	char line[96];
	for (int i=0;i<LAYERS;i++) {
		int n = snprintf(line, sizeof(line), "%d:%s ", i, default_layout_names[i]);
		if (n>0) {
			tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t*)line, (size_t)n);
		}
	} 
	//send_data(line, strlen(line));
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

static void cmd_get_layer_Keymap(uint8_t layer){
	// Envoie le keymap complet de la layer demandée sous forme hex (par lignes)
	if (layer >= LAYERS) {
		ESP_LOGW(TAG_CDC, "Layer %u invalide", (unsigned)layer);
		return;
	}
	extern uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS];
	char line[160]; // suffisamment grand pour 13 * (4 hex + espace) + entête
	for (int r = 0; r < MATRIX_ROWS; r++) {
		int pos = snprintf(line, sizeof(line), "R%d:", r);
		for (int c = 0; c < MATRIX_COLS && pos < (int)sizeof(line) - 6; c++) {
			uint16_t code = keymaps[layer][r][c];
			// Ajout du code en hex (4 digits)
			pos += snprintf(line + pos, sizeof(line) - pos, "%04X", code);
			if (c < MATRIX_COLS - 1 && pos < (int)sizeof(line) - 2) {
				line[pos++] = ' ';
				line[pos] = '\0';
			}
		}
		// Fin de ligne
		if (pos < (int)sizeof(line) - 3) {
			line[pos++] = '-';
			line[pos] = '\0';
		}
		(void)tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t*)line, strlen(line));
	}
	//send_data(line, strlen(line));
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

// Commande: SETKEY layer,row,col,value
static void cmd_set_key(const char *arg)
{
	// Format attendu: layer,row,col,value
	int layer, row, col;
	unsigned int value;
	if (sscanf(arg, "%d,%d,%d,%x", &layer, &row, &col, &value) != 4) {
		ESP_LOGW(TAG_CDC, "SETKEY: format invalide");
		send_data("ERR format\r\n", 12);
		return;
	}
	if (layer < 0 || layer >= LAYERS || row < 0 || row >= MATRIX_ROWS || col < 0 || col >= MATRIX_COLS) {
		ESP_LOGW(TAG_CDC, "SETKEY: index hors limites");
		send_data("ERR index\r\n", 12);
		return;
	}
	extern uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS];
	keymaps[layer][row][col] = (uint16_t)value;
	ESP_LOGI(TAG_CDC, "SETKEY: [%d][%d][%d] = 0x%04X", layer, row, col, value);
	send_data("OK\r\n", 5);
}
// // Commande: SETKEY layer,row,col,value
// static void cmd_set_key(const char *arg)
// {
// 	// Format attendu: layer,row,col,value
// 	int layer, row, col;
// 	unsigned int value;
// 	if (sscanf(arg, "%d,%d,%d,%x", &layer, &row, &col, &value) != 4) {
// 		ESP_LOGW(TAG_CDC, "SETKEY: format invalide");
// 		send_data("ERR format\r\n", 12);
// 		return;
// 	}
// 	if (layer < 0 || layer >= LAYERS || row < 0 || row >= MATRIX_ROWS || col < 0 || col >= MATRIX_COLS) {
// 		ESP_LOGW(TAG_CDC, "SETKEY: index hors limites");
// 		send_data("ERR index\r\n", 12);
// 		return;
// 	}
// 	extern uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS];
// 	keymaps[layer][row][col] = (uint16_t)value;
// 	ESP_LOGI(TAG_CDC, "SETKEY: [%d][%d][%d] = 0x%04X", layer, row, col, value);
// 	send_data("OK\r\n", 5);
// }
static void parse_and_execute(const char *line)
{
	ESP_LOGI(TAG_CDC, "Parsing command: %s", line);
	// Trim espaces
	while (*line == ' ' || *line == '\t') line++;
	if (*line == '\0') return;
	if (strncasecmp(line, "HELP", 4) == 0) { cmd_help(); return; }
	if (strncasecmp(line, "L?", 2) == 0) { cmd_show_layer(); return; }
	if (strncasecmp(line, "LAYERS?", 7) == 0) { cmd_list_layers(); return; }
	if (toupper((unsigned char)line[0]) == 'L' && isdigit((unsigned char)line[1])) { cmd_set_layer(line + 1); return; }
	if (strncasecmp(line, "DISP:", 5) == 0) { cmd_display_text(line + 5); return; }
	if (strncasecmp(line, "KEYMAP?", 7) == 0) { extern uint8_t current_layout; cmd_get_layer_Keymap(current_layout); return; }
	if (strncasecmp(line, "KEYMAP", 6) == 0 && isdigit((unsigned char)line[6])) { cmd_get_layer_Keymap( (uint8_t)atoi(line+6) ); return; }
	if (strncasecmp(line, "SETKEY", 6) == 0) { cmd_set_key(line + 6); return; }
	ESP_LOGW(TAG_CDC, "Commande inconnue: %s", line);
// ...existing code...

}

// ----------------------------------------------------------------------------------
// Réception de chunks USB CDC -> assemblage en lignes terminées par \n ou \r
// ----------------------------------------------------------------------------------
void receive_data(const char *data, uint16_t len)
{
	static bool last_was_cr = false; // pour gérer CRLF
	for (size_t i = 0; i < len; ++i) {
		char c = data[i];
		if (c == '\r') {
			// Fin de ligne sur CR
			if (assemble_len > 0) {
				cdc_cmd_push(assemble_buf, assemble_len);
				assemble_len = 0;
			}
			last_was_cr = true;
			continue;
		}
		if (c == '\n') {
			// Si juste après un CR => déjà flush (CRLF), on ignore
			if (last_was_cr) {
				last_was_cr = false;
				continue;
			}
			if (assemble_len > 0) {
				cdc_cmd_push(assemble_buf, assemble_len);
				assemble_len = 0;
			}
			continue;
		}
		last_was_cr = false;
		if (assemble_len < (CDC_CMD_MAX_LEN - 1)) {
			assemble_buf[assemble_len++] = c;
		} else {
			// overflow - flush implicite
			cdc_cmd_push(assemble_buf, assemble_len);
			assemble_len = 0;
		}
	}
}



void send_data(const char *data, uint16_t len)
{
	if (data == NULL || len == 0) {
		ESP_LOGW(TAG_CDC, "Invalid data");
		return;
	}
	// Format: <START><len><data><END>
	char prefix[16];
	snprintf(prefix, sizeof(prefix), "C>%u", len);
	const char *suffix = "<C";
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t*)prefix, strlen(prefix));
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t*)data, len);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t*)suffix, strlen(suffix));
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

// ----------------------------------------------------------------------------------
// Tâche de traitement (à créer dans main si souhaité)
// ----------------------------------------------------------------------------------
void cdc_process_commands_task(void *arg)
{
	ESP_LOGI(TAG_CDC, "C");
	cdc_cmd_t cmd;
	for(;;) {
		while (cdc_cmd_pop(&cmd)) {
			parse_and_execute(cmd.line);
			ESP_LOGI(TAG_CDC, "C");
		}
		
		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

void init_cdc_commands(void)
{
	cdc_cmd_fifo_init();
	xTaskCreate(cdc_process_commands_task, "cdc_cmd", 3072, NULL, 4, NULL);
}
