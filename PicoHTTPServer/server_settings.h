#pragma once

#include <sys/types.h>

typedef struct
{
	uint32_t ip_address;
	uint32_t network_mask;
	uint32_t secondary_address;
	char network_name[32];
	char network_password[32];
	char hostname[32];
	char domain_name[32];
} pico_server_settings;

const pico_server_settings *get_pico_server_settings();
void write_pico_server_settings(pico_server_settings *new_settings);

const char *get_next_domain_name_component(const char *domain_name, int *position, int *length);
