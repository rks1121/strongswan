/*
 * Copyright (C) 2007 Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#ifndef IFACE_H
#define IFACE_H

#include <library.h>
#include <utils/iterator.h>

#include "mconsole.h"

#define TAP_DEVICE "/dev/net/tun"

typedef struct iface_t iface_t;

/**
 * @brief Interface in a guest, connected to a tap device on the host.
 */
struct iface_t {
	
	/**
	 * @brief Get the interface name in the guest (e.g. eth0).
	 *
	 * @return		guest interface name
	 */
	char* (*get_guest)(iface_t *this);
	
	/**
	 * @brief Get the interface name at the host (e.g. tap0).
	 *
	 * @return		host interface (tap device) name
	 */
	char* (*get_host)(iface_t *this);
	
	/*
	bool (*up) (iface_t *this);
	bool (*down) (iface_t *this);
	bool (*add_addr) (iface_t *this, host_t *addr);
	bool (*del_addr) (iface_t *this, host_t *addr);
	iterator_t* (*create_addr_iterator) (iface_t *this);
	*/
	
	/**
	 * @brief Destroy an interface
	 */
	void (*destroy) (iface_t *this);
};

/**
 * @brief Create a new interface for a guest
 *
 * @param guest		name of the interface in the guest
 * @param mconsole	mconsole of guest
 * @return			interface descriptor, or NULL if failed
 */
iface_t *iface_create(char *guest, mconsole_t *mconsole);

#endif /* IFACE_H */
