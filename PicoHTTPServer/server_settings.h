#pragma once

#include <sys/types.h>

typedef struct
{
	uint32_t ip_address;
	uint32_t network_mask;
	/* The secondary IP address is needed to support the "sign into network" mechanism.
	 * Modern OSes will automatically show the 'sign into network' page if:
	 *	1. The network has valid DHCP/DNS servers
	 *	2. The DNS server resolves requests to test names to valid EXTERNAL IPs (not 192.168.x.y)
	 *	3. Issuing a HTTP GET request to the external IP results in a HTTP 302 redirect to the login page.
	 *
	 * E.g. see https://cs.android.com/android/platform/superproject/+/master:packages/modules/NetworkStack/src/com/android/server/connectivity/NetworkMonitor.java,
	 *		specifically the isDnsPrivateIpResponse() check and the "DNS response to the URL is private IP" error.
	 */
	uint32_t secondary_address;
	char network_name[32];
	char network_password[32];
	char hostname[32];
	char domain_name[32];
	uint32_t dns_ignores_network_suffix;
} pico_server_settings;

const pico_server_settings *get_pico_server_settings();
void write_pico_server_settings(const pico_server_settings *new_settings);

const char *get_next_domain_name_component(const char *domain_name, int *position, int *length);
