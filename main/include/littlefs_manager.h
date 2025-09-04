#pragma once


void littlefs_init(void);
char *get_config_file_content(void);
void write_file_content(char *filename, char *content);
void delete_file_content(char *filename);
char *get_file_content(char *filename);