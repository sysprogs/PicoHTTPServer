#include <string.h>
#include "server_settings.h"
#include <portmacro.h>
#include "hardware/flash.h"

const union 
{
	pico_server_settings settings;
	char padding[FLASH_SECTOR_SIZE];
} __attribute__((aligned(FLASH_SECTOR_SIZE))) s_Settings = { 
	.settings = { 
		.ip_address = 0x017BA8C0,
		.network_mask = 0x00FFFFFF,
		.secondary_address = 0x006433c6,	//TEST-NET-2. See the comment before 'secondary_address' definition for details.
		.network_name = WIFI_SSID,
		.network_password = WIFI_PASSWORD,
		.hostname = "picohttp",
		.domain_name = "piconet.local",
		.dns_ignores_network_suffix = true,
	}
};


const pico_server_settings *get_pico_server_settings()
{
	return &s_Settings.settings;
}

void write_pico_server_settings(const pico_server_settings *new_settings)
{
	portENTER_CRITICAL();
	flash_range_erase((uint32_t)&s_Settings - XIP_BASE, FLASH_SECTOR_SIZE);
	flash_range_program((uint32_t)&s_Settings - XIP_BASE, (const uint8_t *)new_settings, sizeof(*new_settings));
	portEXIT_CRITICAL();
}


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

