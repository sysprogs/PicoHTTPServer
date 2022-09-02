#include <stdarg.h>
#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>

#include <lwip/ip4_addr.h>
#include <lwip/netif.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

#include "dhcpserver/dhcpserver.h"
#include "dns/dnsserver.h"
#include "server_settings.h"
#include "httpserver.h"
#include "tools/SimpleFSBuilder/SimpleFS.h"

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

static bool do_retrieve_file(http_connection conn, char *path, void *context)
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
	http_server_instance server = http_server_create(settings->hostname, settings->domain_name, 4, 4096);
	static http_zone zone;
	http_server_add_zone(server, &zone, "", do_retrieve_file, NULL);
	vTaskDelete(NULL);
}

xSemaphoreHandle s_PrintfSemaphore;

void debug_printf(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	xSemaphoreTake(s_PrintfSemaphore, portMAX_DELAY);
	vprintf(format, args);
	xSemaphoreGive(s_PrintfSemaphore);
	va_end(args);
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