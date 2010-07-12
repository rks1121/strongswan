/*
 * Copyright (C) 2010 Martin Willi
 * Copyright (C) 2010 revosec AG
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

/**
 * @defgroup eap eap
 * @{ @ingroup libstrongswan
 */

#ifndef EAP_H_
#define EAP_H_

typedef enum eap_code_t eap_code_t;
typedef enum eap_type_t eap_type_t;

#include <library.h>

/**
 * EAP code, type of an EAP message
 */
enum eap_code_t {
	EAP_REQUEST = 1,
	EAP_RESPONSE = 2,
	EAP_SUCCESS = 3,
	EAP_FAILURE = 4,
};

/**
 * enum names for eap_code_t.
 */
extern enum_name_t *eap_code_names;

/**
 * short string enum names for eap_code_t.
 */
extern enum_name_t *eap_code_short_names;

/**
 * EAP types, defines the EAP method implementation
 */
enum eap_type_t {
	EAP_IDENTITY = 1,
	EAP_NOTIFICATION = 2,
	EAP_NAK = 3,
	EAP_MD5 = 4,
	EAP_OTP = 5,
	EAP_GTC = 6,
	EAP_TLS = 13,
	EAP_SIM = 18,
	EAP_TTLS = 21,
	EAP_AKA = 23,
	EAP_MSCHAPV2 = 26,
	EAP_TNC = 38,
	/** not a method, but an implementation providing different methods */
	EAP_RADIUS = 253,
	EAP_EXPANDED = 254,
	EAP_EXPERIMENTAL = 255,
};

/**
 * enum names for eap_type_t.
 */
extern enum_name_t *eap_type_names;

/**
 * short string enum names for eap_type_t.
 */
extern enum_name_t *eap_type_short_names;

/**
 * Lookup the EAP method type from a string.
 *
 * @param name		EAP method name (such as "md5", "aka")
 * @return			method type, 0 if unkown
 */
eap_type_t eap_type_from_string(char *name);

#endif /** EAP_H_ @}*/