#include <stdarg.h>
#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>
#include <hardware/watchdog.h>

#include <lwip/ip4_addr.h>
#include <lwip/netif.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

#include "dhcpserver/dhcpserver.h"
#include "dns/dnsserver.h"
#include "server_settings.h"
#include "httpserver.h"
#include "../tools/SimpleFSBuilder/SimpleFS.h"

#define TEST_TASK_PRIORITY (tskIDLE_PRIORITY + 2UL)

struct SimpleFSContext
{
	GlobalFSHeader *header;
	StoredFileEntry *entries;
	char *names, *data;
} s_SimpleFS;

bool simplefs_init(struct SimpleFSContext *ctx, void *data)
{
	ctx->header = (GlobalFSHeader *)data;
	if (ctx->header->Magic != kSimpleFSHeaderMagic)
		return false;
	ctx->entries = (StoredFileEntry *)(ctx->header + 1);
	ctx->names = (char *)(ctx->entries + ctx->header->EntryCount);
	ctx->data = (char *)(ctx->names + ctx->header->NameBlockSize);
	return true;
}

static bool do_retrieve_file(http_connection conn, enum http_request_type type, char *path, void *context)
{
	for (int i = 0; i < s_SimpleFS.header->EntryCount; i++)
	{
		if (!strcmp(s_SimpleFS.names + s_SimpleFS.entries[i].NameOffset, path))
		{
			http_server_send_reply(conn, 
				"200 OK", 
				s_SimpleFS.names + s_SimpleFS.entries[i].ContentTypeOffset,
				s_SimpleFS.data + s_SimpleFS.entries[i].DataOffset,
				s_SimpleFS.entries[i].FileSize);
			return true;
		}
	}
	
	return false;
}

static char *parse_server_settings(http_connection conn, pico_server_settings *settings)
{
	bool has_password = false, use_domain = false, use_second_ip = false;
	bool bad_password = false, bad_domain = false;
	
	for (;;)
	{
		char *line = http_server_read_post_line(conn);
		if (!line)
			break;
				
		char *p = strchr(line, '=');
		if (!p)
			continue;
		*p++ = 0;
		if (!strcasecmp(line, "has_password")) 
			has_password = !strcasecmp(p, "true") || p[0] == '1';
		else if (!strcasecmp(line, "use_domain")) 
			use_domain = !strcasecmp(p, "true") || p[0] == '1';
		else if (!strcasecmp(line, "use_second_ip")) 
			use_second_ip = !strcasecmp(p, "true") || p[0] == '1';
		else if (!strcasecmp(line, "ssid")) 
		{
			if (strlen(p) >= sizeof(settings->network_name))
				return "SSID too long";
			if (!p[0])
				return "missing SSID";
			strcpy(settings->network_name, p);
		}
		else if (!strcasecmp(line, "password")) 
		{
			if (strlen(p) >= sizeof(settings->network_password))
				bad_password = true;
			else
				strcpy(settings->network_password, p);
		}
		else if (!strcasecmp(line, "hostname")) 
		{
			if (strlen(p) >= sizeof(settings->hostname))
				return "hostname too long";
			if (!p[0])
				return "missing hostname";
			strcpy(settings->hostname, p);
		}
		else if (!strcasecmp(line, "domain")) 
		{
			if (strlen(p) >= sizeof(settings->domain_name))
				bad_domain = true;
			else
				strcpy(settings->domain_name, p);
		}
		else if (!strcasecmp(line, "ipaddr")) 
		{
			settings->ip_address = ipaddr_addr(p);
			if (!settings->ip_address || settings->ip_address == -1)
				return "invalid IP address";
		}
		else if (!strcasecmp(line, "netmask")) 
		{
			settings->network_mask = ipaddr_addr(p);
			if (!settings->network_mask || settings->network_mask == -1)
				return "invalid network mask";
		}
		else if (!strcasecmp(line, "ipaddr2")) 
		{
			settings->secondary_address = ipaddr_addr(p);
		}
	}
	
	if (!has_password)
		memset(settings->network_password, 0, sizeof(settings->network_password));
	else if (bad_password)
		return "password too long";
	
	if (!use_domain)
		memset(settings->domain_name, 0, sizeof(settings->domain_name));
	else if (bad_domain)
		return "domain too long";
	
	if (!use_second_ip)
		settings->secondary_address = 0;
	else if (!settings->secondary_address || settings->secondary_address == -1)
		return "invalid secondary IP address";
	
	return NULL;
}

static bool do_handle_api_call(http_connection conn, enum http_request_type type, char *path, void *context)
{
	static int s_InitializedMask = 0;
	
	if (!strcmp(path, "readpins"))
	{
		http_write_handle reply = http_server_begin_write_reply(conn, "200 OK", "text/json");
		http_server_write_reply(reply, "{\"led0v\": \"%d\"", cyw43_arch_gpio_get(0));
		
		int values = gpio_get_all();
		
		for (int i = 0; i < 29; i++)
		{
			if (i > 22 && i < 26)
				continue;
			
			if (s_InitializedMask & (1 << i))
				http_server_write_reply(reply, ",\"gpio%dd\": \"%s\",\"gpio%dv\": \"%d\"", i, gpio_get_dir(i) ? "OUT" : "IN", i, (values >> i) & 1);
		}
		
		http_server_end_write_reply(reply, "}");
		return true;
	}
	else if (!memcmp(path, "writepin/", 9))
	{
		//e.g. 'writepin/led0?v=1'
		char *port = path + 9;
		char *arg = strchr(port, '?');
		if (arg)
		{
			*arg++ = 0;
			char *value = strchr(arg, '=');
			*value++ = 0;
		
			if (!strcmp(port, "led0"))
				cyw43_arch_gpio_put(0, value[0] == '1');
			else if (!memcmp(port, "gpio", 4))
			{
				int gpio = atoi(port + 4);
				if (!(s_InitializedMask & (1 << gpio)))
				{
					gpio_init(gpio);
					s_InitializedMask |= (1 << gpio);
				}

				if (arg[0] == 'd' && value[0] == 'I')
				{
					gpio_set_pulls(gpio, true, false);
					gpio_set_dir(gpio, GPIO_IN);
				}
				else
				{
					gpio_set_pulls(gpio, false, false);
					gpio_set_dir(gpio, GPIO_OUT);

					if (arg[0] == 'v')
						gpio_put(gpio, value[0] == '1');
				}
			}
			
			return true;
		}
	}
	else if (!strcmp(path, "settings"))
	{
		if (type == HTTP_POST)
		{
			static pico_server_settings settings;
			settings = *get_pico_server_settings();

			char *err = parse_server_settings(conn, &settings);
			if (err)
			{
				http_server_send_reply(conn, "200 OK", "text/plain", err, -1);
				return true;
			}
			
			write_pico_server_settings(&settings);
			http_server_send_reply(conn, "200 OK", "text/plain", "OK", -1);
			watchdog_reboot(0, SRAM_END, 500);			
			return true;
		}
		else
		{
			const pico_server_settings *settings = get_pico_server_settings();
			http_write_handle reply = http_server_begin_write_reply(conn, "200 OK", "text/json");
			http_server_write_reply(reply, "{\"ssid\": \"%s\"", settings->network_name);
			http_server_write_reply(reply, ",\"has_password\": %d, \"password\" : \"%s\"", settings->network_password[0] != 0, settings->network_password);
			http_server_write_reply(reply, ",\"hostname\" : \"%s\"", settings->hostname);
			http_server_write_reply(reply, ",\"use_domain\": %d, \"domain\" : \"%s\"", settings->domain_name[0] != 0, settings->domain_name);
			http_server_write_reply(reply, ",\"ipaddr\" : \"%d.%d.%d.%d\"", (settings->ip_address >> 0) & 0xFF, (settings->ip_address >> 8) & 0xFF, (settings->ip_address >> 16) & 0xFF, (settings->ip_address >> 24) & 0xFF);
			http_server_write_reply(reply, ",\"netmask\" : \"%d.%d.%d.%d\"", (settings->network_mask >> 0) & 0xFF, (settings->network_mask >> 8) & 0xFF, (settings->network_mask >> 16) & 0xFF, (settings->network_mask >> 24) & 0xFF);
			http_server_write_reply(reply, ",\"use_second_ip\": %d", settings->secondary_address != 0);
			http_server_write_reply(reply, ",\"ipaddr2\" : \"%d.%d.%d.%d\"", (settings->secondary_address >> 0) & 0xFF, (settings->secondary_address >> 8) & 0xFF, (settings->secondary_address >> 16) & 0xFF, (settings->secondary_address >> 24) & 0xFF);

			http_server_end_write_reply(reply, "}");
			return true;
		}
	}
	
	return false;
}


static void set_secondary_ip_address(int address)
{
	/************************************ !!! WARNING !!! ************************************
	 * If you get an 'undefined reference to ip4_secondary_ip_address' error here,			 *
	 * you need to patch your lwIP using the lwip_patch/lwip.patch file from this repository.*
	 * This ensures that this device can pretend to be a router redirecting requests to		 *
	 * external IPs to its login page, so the OS can automatically navigate there.			 *
	 *****************************************************************************************/
	
	extern int ip4_secondary_ip_address;
	ip4_secondary_ip_address = address;
}

static void main_task(__unused void *params)
{
	
	if (cyw43_arch_init())
	{
		printf("failed to initialise\n");
		return;
	}
	
	extern void *_binary_www_fs_start;
	if (!simplefs_init(&s_SimpleFS, &_binary_www_fs_start))
	{
		printf("missing/corrupt FS image");
		return;
	}
	
	const pico_server_settings *settings = get_pico_server_settings();

	cyw43_arch_enable_ap_mode(settings->network_name, settings->network_password, settings->network_password[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN);

	struct netif *netif = netif_default;
	ip4_addr_t addr = { .addr = settings->ip_address }, mask = { .addr = settings->network_mask };
	
	netif_set_addr(netif, &addr, &mask, &addr);
	
	// Start the dhcp server
	static dhcp_server_t dhcp_server;
	dhcp_server_init(&dhcp_server, &netif->ip_addr, &netif->netmask, settings->domain_name);
	dns_server_init(netif->ip_addr.addr, settings->secondary_address, settings->hostname, settings->domain_name);
	set_secondary_ip_address(settings->secondary_address);
	http_server_instance server = http_server_create(settings->hostname, settings->domain_name, 4, 4096);
	static http_zone zone1, zone2;
	http_server_add_zone(server, &zone1, "", do_retrieve_file, NULL);
	http_server_add_zone(server, &zone2, "/api", do_handle_api_call, NULL);
	vTaskDelete(NULL);
}

xSemaphoreHandle s_PrintfSemaphore;

void debug_printf(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	xSemaphoreTake(s_PrintfSemaphore, portMAX_DELAY);
	vprintf(format, args);
	va_end(args);
	xSemaphoreGive(s_PrintfSemaphore);
}

void debug_write(const void *data, int size)
{
	xSemaphoreTake(s_PrintfSemaphore, portMAX_DELAY);
	_write(1, data, size);
	xSemaphoreGive(s_PrintfSemaphore);
}

int main(void)
{
	stdio_init_all();
	TaskHandle_t task;
	s_PrintfSemaphore = xSemaphoreCreateMutex();
	xTaskCreate(main_task, "MainThread", configMINIMAL_STACK_SIZE, NULL, TEST_TASK_PRIORITY, &task);
	vTaskStartScheduler();
}