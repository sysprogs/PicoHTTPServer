#pragma once
#include <sys/types.h>

void dns_server_init(uint32_t primary_ip,
	uint32_t secondary_ip,
	const char *host_name, 
	const char *domain_name,
	bool dns_ignores_network_suffix);
