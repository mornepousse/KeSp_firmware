#pragma once

#include <stdint.h>
#include <stdbool.h>

// Taille du buffer ligne (commande texte)

#define CDC_CMD_MAX_LEN 64
// Nombre de lignes stockables
#define CDC_CMD_FIFO_DEPTH 8

typedef struct {
	char line[CDC_CMD_MAX_LEN];
	uint16_t len; // longueur utile
} cdc_cmd_t;

// API FIFO commandes
void cdc_cmd_fifo_init(void);
bool cdc_cmd_push(const char *line, uint16_t len); // push une ligne complète
bool cdc_cmd_pop(cdc_cmd_t *out);
bool cdc_cmd_peek(cdc_cmd_t *out); // optionnel
uint16_t cdc_cmd_count(void);

// Appelé par l'ISR / callback TinyUSB avec des chunks arbitraires
void receive_data(const char *data, uint16_t len);

// A appeler périodiquement dans une tâche pour traiter les commandes
void cdc_process_commands_task(void *arg);

void init_cdc_commands(void);

void send_data(const char *data, uint16_t len);
