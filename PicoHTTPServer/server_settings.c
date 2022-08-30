#include <string.h>
#include "server_settings.h"

static pico_server_settings s_Settings = { 
	.ip_address = 0x017BA8C0,
	.network_mask = 0x00FFFFFF,
	.secondary_address = 0x01010101,
	.network_name = WIFI_SSID,
	.network_password = WIFI_PASSWORD,
	.hostname = "picohttp",
	.domain_name = "piconet.local"
};

const pico_server_settings *get_pico_server_settings()
{
	return &s_Settings;
}

//void write_pico_server_settings(pico_server_settings *new_settings);

const char *get_next_domain_name_component(const char *domain_name, int *position, int *length)
{
	if (!domain_name || !position || !length)
		return NULL;
	
	int pos = *position;
	const char *p = strchr(domain_name + pos, '.');
	if (p)
	{
		*position = p + 1 - domain_name;
		*length = p - domain_name - pos;
		return domain_name + pos;
	}
	else if (domain_name[pos])
	{
		*length = strlen(domain_name + pos);
		*position = pos + *length;
		return domain_name + pos;
	}
	else
		return NULL;
}

