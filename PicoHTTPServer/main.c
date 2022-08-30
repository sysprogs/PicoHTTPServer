#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>

#include <lwip/ip4_addr.h>
#include <lwip/netif.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

#include "dhcpserver/dhcpserver.h"
#include <stdarg.h>

#define TEST_TASK_PRIORITY (tskIDLE_PRIORITY + 2UL)

static void run_server()
{
	while (true)
	{
		vTaskDelay(100);
	}
}

static void main_task(__unused void *params)
{
	if (cyw43_arch_init())
	{
		printf("failed to initialise\n");
		return;
	}

	cyw43_arch_enable_ap_mode(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_OPEN);

	ip4_addr_t gw, mask;
	IP4_ADDR(&gw, 192, 168, 4, 1);
	IP4_ADDR(&mask, 255, 255, 255, 0);

	// Start the dhcp server
	static dhcp_server_t dhcp_server;
	dhcp_server_init(&dhcp_server, &gw, &mask);
	dns_server_init(&gw);
	http_server_init(&gw);
	
	run_server();
	cyw43_arch_deinit();
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

int main(void)
{
	stdio_init_all();
	TaskHandle_t task;
	s_PrintfSemaphore = xSemaphoreCreateMutex();
	xTaskCreate(main_task, "MainThread", configMINIMAL_STACK_SIZE, NULL, TEST_TASK_PRIORITY, &task);
	vTaskStartScheduler();
}