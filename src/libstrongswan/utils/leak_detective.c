/*
 * Copyright (C) 2013 Tobias Brunner
 * Copyright (C) 2006-2013 Martin Willi
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

#define _GNU_SOURCE
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <syslog.h>
#include <netdb.h>
#include <locale.h>
#include <dlfcn.h>
#include <time.h>
#include <errno.h>

#ifdef __APPLE__
#include <sys/mman.h>
#include <malloc/malloc.h>
/* overload some of our types clashing with mach */
#define host_t strongswan_host_t
#define processor_t strongswan_processor_t
#define thread_t strongswan_thread_t
#endif /* __APPLE__ */

#include "leak_detective.h"

#include <library.h>
#include <utils/debug.h>
#include <utils/backtrace.h>
#include <collections/hashtable.h>
#include <threading/thread_value.h>
#include <threading/spinlock.h>

typedef struct private_leak_detective_t private_leak_detective_t;

/**
 * private data of leak_detective
 */
struct private_leak_detective_t {

	/**
	 * public functions
	 */
	leak_detective_t public;
};

/**
 * Magic value which helps to detect memory corruption. Yummy!
 */
#define MEMORY_HEADER_MAGIC 0x7ac0be11

/**
 * Magic written to tail of allocation
 */
#define MEMORY_TAIL_MAGIC 0xcafebabe

/**
 * Pattern which is filled in memory before freeing it
 */
#define MEMORY_FREE_PATTERN 0xFF

/**
 * Pattern which is filled in newly allocated memory
 */
#define MEMORY_ALLOC_PATTERN 0xEE

typedef struct memory_header_t memory_header_t;
typedef struct memory_tail_t memory_tail_t;

/**
 * Header which is prepended to each allocated memory block
 */
struct memory_header_t {

	/**
	 * Pointer to previous entry in linked list
	 */
	memory_header_t *previous;

	/**
	 * Pointer to next entry in linked list
	 */
	memory_header_t *next;

	/**
	 * backtrace taken during (re-)allocation
	 */
	backtrace_t *backtrace;

	/**
	 * Padding to make sizeof(memory_header_t) == 32
	 */
	u_int32_t padding[sizeof(void*) == sizeof(u_int32_t) ? 3 : 0];

	/**
	 * Number of bytes following after the header
	 */
	u_int32_t bytes;

	/**
	 * magic bytes to detect bad free or heap underflow, MEMORY_HEADER_MAGIC
	 */
	u_int32_t magic;

}__attribute__((__packed__));

/**
 * tail appended to each allocated memory block
 */
struct memory_tail_t {

	/**
	 * Magic bytes to detect heap overflow, MEMORY_TAIL_MAGIC
	 */
	u_int32_t magic;

}__attribute__((__packed__));

/**
 * first mem header is just a dummy to chain
 * the others on it...
 */
static memory_header_t first_header = {
	.magic = MEMORY_HEADER_MAGIC,
};

/**
 * Spinlock to access header linked list
 */
static spinlock_t *lock;

/**
 * Is leak detection currently enabled?
 */
static bool enabled = FALSE;

/**
 * Is leak detection disabled for the current thread?
 */
static thread_value_t *thread_disabled;

/**
 * Installs the malloc hooks, enables leak detection
 */
static void enable_leak_detective()
{
	enabled = TRUE;
}

/**
 * Uninstalls the malloc hooks, disables leak detection
 */
static void disable_leak_detective()
{
	enabled = FALSE;
}

/**
 * Enable/Disable leak detective for the current thread
 *
 * @return Previous value
 */
static bool enable_thread(bool enable)
{
	bool before;

	before = thread_disabled->get(thread_disabled) == NULL;
	thread_disabled->set(thread_disabled, enable ? NULL : (void*)TRUE);
	return before;
}

#ifdef __APPLE__

/**
 * Copy of original default zone, with functions we call in hooks
 */
static malloc_zone_t original;

/**
 * Call original malloc()
 */
static void* real_malloc(size_t size)
{
	return original.malloc(malloc_default_zone(), size);
}

/**
 * Call original free()
 */
static void real_free(void *ptr)
{
	original.free(malloc_default_zone(), ptr);
}

/**
 * Call original realloc()
 */
static void* real_realloc(void *ptr, size_t size)
{
	return original.realloc(malloc_default_zone(), ptr, size);
}

/**
 * Hook definition: static function with _hook suffix, takes additional zone
 */
#define HOOK(ret, name, ...) \
	static ret name ## _hook(malloc_zone_t *_z, __VA_ARGS__)

/**
 * forward declaration of hooks
 */
HOOK(void*, malloc, size_t bytes);
HOOK(void*, calloc, size_t nmemb, size_t size);
HOOK(void*, valloc, size_t size);
HOOK(void, free, void *ptr);
HOOK(void*, realloc, void *old, size_t bytes);

/**
 * malloc zone size(), must consider the memory header prepended
 */
HOOK(size_t, size, const void *ptr)
{
	bool before;
	size_t size;

	if (enabled)
	{
		before = enable_thread(FALSE);
		if (before)
		{
			ptr -= sizeof(memory_header_t);
		}
	}
	size = original.size(malloc_default_zone(), ptr);
	if (enabled)
	{
		enable_thread(before);
	}
	return size;
}

/**
 * Version of malloc zones we currently support
 */
#define MALLOC_ZONE_VERSION 8 /* Snow Leopard */

/**
 * Hook-in our malloc functions into the default zone
 */
static bool register_hooks()
{
	malloc_zone_t *zone;
	void *page;

	zone = malloc_default_zone();
	if (zone->version != MALLOC_ZONE_VERSION)
	{
		DBG1(DBG_CFG, "malloc zone version %d unsupported (requiring %d)",
			 zone->version, MALLOC_ZONE_VERSION);
		return FALSE;
	}

	original = *zone;

	page = (void*)((uintptr_t)zone / getpagesize() * getpagesize());
	if (mprotect(page, getpagesize(), PROT_WRITE | PROT_READ) != 0)
	{
		DBG1(DBG_CFG, "malloc zone unprotection failed: %s", strerror(errno));
		return FALSE;
	}

	zone->size = size_hook;
	zone->malloc = malloc_hook;
	zone->calloc = calloc_hook;
	zone->valloc = valloc_hook;
	zone->free = free_hook;
	zone->realloc = realloc_hook;

	/* those other functions can be NULLed out to not use them */
	zone->batch_malloc = NULL;
	zone->batch_free = NULL;
	zone->memalign = NULL;
	zone->free_definite_size = NULL;

	return TRUE;
}

#else /* !__APPLE__ */

/**
 * dlsym() might do a malloc(), but we can't do one before we get the malloc()
 * function pointer. Use this minimalistic malloc implementation instead.
 */
static void* malloc_for_dlsym(size_t size)
{
	static char buf[1024] = {};
	static size_t used = 0;
	char *ptr;

	/* roundup to a multiple of 32 */
	size = (size - 1) / 32 * 32 + 32;

	if (used + size > sizeof(buf))
	{
		return NULL;
	}
	ptr = buf + used;
	used += size;
	return ptr;
}

/**
 * Lookup a malloc function, while disabling wrappers
 */
static void* get_malloc_fn(char *name)
{
	bool before = FALSE;
	void *fn;

	if (enabled)
	{
		before = enable_thread(FALSE);
	}
	fn = dlsym(RTLD_NEXT, name);
	if (enabled)
	{
		enable_thread(before);
	}
	return fn;
}

/**
 * Call original malloc()
 */
static void* real_malloc(size_t size)
{
	static void* (*fn)(size_t size);
	static int recursive = 0;

	if (!fn)
	{
		/* checking recursiveness should actually be thread-specific. But as
		 * it is very likely that the first allocation is done before we go
		 * multi-threaded, we keep it simple. */
		if (recursive)
		{
			return malloc_for_dlsym(size);
		}
		recursive++;
		fn = get_malloc_fn("malloc");
		recursive--;
	}
	return fn(size);
}

/**
 * Call original free()
 */
static void real_free(void *ptr)
{
	static void (*fn)(void *ptr);

	if (!fn)
	{
		fn = get_malloc_fn("free");
	}
	return fn(ptr);
}

/**
 * Call original realloc()
 */
static void* real_realloc(void *ptr, size_t size)
{
	static void* (*fn)(void *ptr, size_t size);

	if (!fn)
	{
		fn = get_malloc_fn("realloc");
	}
	return fn(ptr, size);
}

/**
 * Hook definition: plain function overloading existing malloc calls
 */
#define HOOK(ret, name, ...) ret name(__VA_ARGS__)

/**
 * Hook initialization when not using hooks
 */
static bool register_hooks()
{
	return TRUE;
}

#endif /* !__APPLE__ */

/**
 * Leak report white list
 *
 * List of functions using static allocation buffers or should be suppressed
 * otherwise on leak report.
 */
char *whitelist[] = {
	/* backtraces, including own */
	"backtrace_create",
	"safe_strerror",
	/* pthread stuff */
	"pthread_create",
	"pthread_setspecific",
	"__pthread_setspecific",
	/* glibc functions */
	"inet_ntoa",
	"strerror",
	"getprotobyname",
	"getprotobynumber",
	"getservbyport",
	"getservbyname",
	"gethostbyname",
	"gethostbyname2",
	"gethostbyname_r",
	"gethostbyname2_r",
	"getnetbyname",
	"getpwnam_r",
	"getgrnam_r",
	"register_printf_function",
	"register_printf_specifier",
	"syslog",
	"vsyslog",
	"__syslog_chk",
	"__vsyslog_chk",
	"getaddrinfo",
	"setlocale",
	"getpass",
	"getpwent_r",
	"setpwent",
	"endpwent",
	"getspnam_r",
	"getpwuid_r",
	"initgroups",
	/* ignore dlopen, as we do not dlclose to get proper leak reports */
	"dlopen",
	"dlerror",
	"dlclose",
	"dlsym",
	/* mysql functions */
	"mysql_init_character_set",
	"init_client_errs",
	"my_thread_init",
	/* fastcgi library */
	"FCGX_Init",
	/* libxml */
	"xmlInitCharEncodingHandlers",
	"xmlInitParser",
	"xmlInitParserCtxt",
	/* libcurl */
	"Curl_client_write",
	/* ClearSilver */
	"nerr_init",
	/* libgcrypt */
	"gcry_control",
	"gcry_check_version",
	"gcry_randomize",
	"gcry_create_nonce",
	/* NSPR */
	"PR_CallOnce",
	/* libapr */
	"apr_pool_create_ex",
	/* glib */
	"g_type_init_with_debug_flags",
	"g_type_register_static",
	"g_type_class_ref",
	"g_type_create_instance",
	"g_type_add_interface_static",
	"g_type_interface_add_prerequisite",
	"g_socket_connection_factory_lookup_type",
	/* libgpg */
	"gpg_err_init",
	/* gnutls */
	"gnutls_global_init",
};

/**
 * Some functions are hard to whitelist, as they don't use a symbol directly.
 * Use some static initialization to suppress them on leak reports
 */
static void init_static_allocations()
{
	tzset();
}

/**
 * Hashtable hash function
 */
static u_int hash(backtrace_t *key)
{
	enumerator_t *enumerator;
	void *addr;
	u_int hash = 0;

	enumerator = key->create_frame_enumerator(key);
	while (enumerator->enumerate(enumerator, &addr))
	{
		hash = chunk_hash_inc(chunk_from_thing(addr), hash);
	}
	enumerator->destroy(enumerator);

	return hash;
}

/**
 * Hashtable equals function
 */
static bool equals(backtrace_t *a, backtrace_t *b)
{
	return a->equals(a, b);
}

/**
 * Summarize and print backtraces
 */
static int print_traces(private_leak_detective_t *this,
						FILE *out, int thresh, bool detailed, int *whitelisted)
{
	int leaks = 0;
	memory_header_t *hdr;
	enumerator_t *enumerator;
	hashtable_t *entries;
	struct {
		/** associated backtrace */
		backtrace_t *backtrace;
		/** total size of all allocations */
		size_t bytes;
		/** number of allocations */
		u_int count;
	} *entry;
	bool before;

	before = enable_thread(FALSE);

	entries = hashtable_create((hashtable_hash_t)hash,
							   (hashtable_equals_t)equals, 1024);
	lock->lock(lock);
	for (hdr = first_header.next; hdr != NULL; hdr = hdr->next)
	{
		if (whitelisted &&
			hdr->backtrace->contains_function(hdr->backtrace,
											  whitelist, countof(whitelist)))
		{
			(*whitelisted)++;
			continue;
		}
		entry = entries->get(entries, hdr->backtrace);
		if (entry)
		{
			entry->bytes += hdr->bytes;
			entry->count++;
		}
		else
		{
			INIT(entry,
				.backtrace = hdr->backtrace,
				.bytes = hdr->bytes,
				.count = 1,
			);
			entries->put(entries, hdr->backtrace, entry);
		}
		leaks++;
	}
	lock->unlock(lock);
	enumerator = entries->create_enumerator(entries);
	while (enumerator->enumerate(enumerator, NULL, &entry))
	{
		if (!thresh || entry->bytes >= thresh)
		{
			fprintf(out, "%d bytes total, %d allocations, %d bytes average:\n",
					entry->bytes, entry->count, entry->bytes / entry->count);
			entry->backtrace->log(entry->backtrace, out, detailed);
		}
		free(entry);
	}
	enumerator->destroy(enumerator);
	entries->destroy(entries);

	enable_thread(before);
	return leaks;
}

METHOD(leak_detective_t, report, void,
	private_leak_detective_t *this, bool detailed)
{
	if (lib->leak_detective)
	{
		int leaks = 0, whitelisted = 0;

		leaks = print_traces(this, stderr, 0, detailed, &whitelisted);
		switch (leaks)
		{
			case 0:
				fprintf(stderr, "No leaks detected");
				break;
			case 1:
				fprintf(stderr, "One leak detected");
				break;
			default:
				fprintf(stderr, "%d leaks detected", leaks);
				break;
		}
		fprintf(stderr, ", %d suppressed by whitelist\n", whitelisted);
	}
	else
	{
		fprintf(stderr, "Leak detective disabled\n");
	}
}

METHOD(leak_detective_t, set_state, bool,
	private_leak_detective_t *this, bool enable)
{
	if (enable == enabled)
	{
		return enabled;
	}
	if (enable)
	{
		enable_leak_detective();
	}
	else
	{
		disable_leak_detective();
	}
	return !enabled;
}

METHOD(leak_detective_t, usage, void,
	private_leak_detective_t *this, FILE *out)
{
	bool detailed;
	int thresh;

	thresh = lib->settings->get_int(lib->settings,
					"libstrongswan.leak_detective.usage_threshold", 10240);
	detailed = lib->settings->get_bool(lib->settings,
					"libstrongswan.leak_detective.detailed", TRUE);

	print_traces(this, out, thresh, detailed, NULL);
}

/**
 * Wrapped malloc() function
 */
HOOK(void*, malloc, size_t bytes)
{
	memory_header_t *hdr;
	memory_tail_t *tail;
	bool before;

	if (!enabled || thread_disabled->get(thread_disabled))
	{
		return real_malloc(bytes);
	}

	hdr = real_malloc(sizeof(memory_header_t) + bytes + sizeof(memory_tail_t));
	tail = ((void*)hdr) + bytes + sizeof(memory_header_t);
	/* set to something which causes crashes */
	memset(hdr, MEMORY_ALLOC_PATTERN,
		   sizeof(memory_header_t) + bytes + sizeof(memory_tail_t));

	before = enable_thread(FALSE);
	hdr->backtrace = backtrace_create(2);
	enable_thread(before);

	hdr->magic = MEMORY_HEADER_MAGIC;
	hdr->bytes = bytes;
	tail->magic = MEMORY_TAIL_MAGIC;

	/* insert at the beginning of the list */
	lock->lock(lock);
	hdr->next = first_header.next;
	if (hdr->next)
	{
		hdr->next->previous = hdr;
	}
	hdr->previous = &first_header;
	first_header.next = hdr;
	lock->unlock(lock);

	return hdr + 1;
}

/**
 * Wrapped calloc() function
 */
HOOK(void*, calloc, size_t nmemb, size_t size)
{
	void *ptr;

	size *= nmemb;
	ptr = malloc(size);
	memset(ptr, 0, size);

	return ptr;
}

/**
 * Wrapped valloc(), TODO: currently not supported
 */
HOOK(void*, valloc, size_t size)
{
	DBG1(DBG_LIB, "valloc() used, but leak-detective hook missing");
	return NULL;
}

/**
 * Wrapped free() function
 */
HOOK(void, free, void *ptr)
{
	memory_header_t *hdr, *current;
	memory_tail_t *tail;
	backtrace_t *backtrace;
	bool found = FALSE, before;

	if (!enabled || thread_disabled->get(thread_disabled))
	{
		real_free(ptr);
		return;
	}
	/* allow freeing of NULL */
	if (ptr == NULL)
	{
		return;
	}
	hdr = ptr - sizeof(memory_header_t);
	tail = ptr + hdr->bytes;

	before = enable_thread(FALSE);
	if (hdr->magic != MEMORY_HEADER_MAGIC ||
		tail->magic != MEMORY_TAIL_MAGIC)
	{
		lock->lock(lock);
		for (current = &first_header; current != NULL; current = current->next)
		{
			if (current == hdr)
			{
				found = TRUE;
				break;
			}
		}
		lock->unlock(lock);
		if (found)
		{
			/* memory was allocated by our hooks but is corrupted */
			fprintf(stderr, "freeing corrupted memory (%p): "
					"header magic 0x%x, tail magic 0x%x:\n",
					ptr, hdr->magic, tail->magic);
		}
		else
		{
			/* memory was not allocated by our hooks */
			fprintf(stderr, "freeing invalid memory (%p)\n", ptr);
		}
		backtrace = backtrace_create(2);
		backtrace->log(backtrace, stderr, TRUE);
		backtrace->destroy(backtrace);
	}
	else
	{
		/* remove item from list */
		lock->lock(lock);
		if (hdr->next)
		{
			hdr->next->previous = hdr->previous;
		}
		hdr->previous->next = hdr->next;
		lock->unlock(lock);

		hdr->backtrace->destroy(hdr->backtrace);

		/* clear MAGIC, set mem to something remarkable */
		memset(hdr, MEMORY_FREE_PATTERN,
			   sizeof(memory_header_t) + hdr->bytes + sizeof(memory_tail_t));

		real_free(hdr);
	}
	enable_thread(before);
}

/**
 * Wrapped realloc() function
 */
HOOK(void*, realloc, void *old, size_t bytes)
{
	memory_header_t *hdr;
	memory_tail_t *tail;
	backtrace_t *backtrace;
	bool before;

	if (!enabled || thread_disabled->get(thread_disabled))
	{
		return real_realloc(old, bytes);
	}
	/* allow reallocation of NULL */
	if (old == NULL)
	{
		return malloc(bytes);
	}

	hdr = old - sizeof(memory_header_t);
	tail = old + hdr->bytes;

	if (hdr->magic != MEMORY_HEADER_MAGIC ||
		tail->magic != MEMORY_TAIL_MAGIC)
	{
		fprintf(stderr, "reallocating invalid memory (%p):\n"
				"header magic 0x%x:\n", old, hdr->magic);
		backtrace = backtrace_create(2);
		backtrace->log(backtrace, stderr, TRUE);
		backtrace->destroy(backtrace);
	}
	else
	{
		/* clear tail magic, allocate, set tail magic */
		memset(&tail->magic, MEMORY_ALLOC_PATTERN, sizeof(tail->magic));
	}
	hdr = real_realloc(hdr,
					   sizeof(memory_header_t) + bytes + sizeof(memory_tail_t));
	tail = ((void*)hdr) + bytes + sizeof(memory_header_t);
	tail->magic = MEMORY_TAIL_MAGIC;

	/* update statistics */
	hdr->bytes = bytes;

	before = enable_thread(FALSE);
	hdr->backtrace->destroy(hdr->backtrace);
	hdr->backtrace = backtrace_create(2);
	enable_thread(before);

	/* update header of linked list neighbours */
	lock->lock(lock);
	if (hdr->next)
	{
		hdr->next->previous = hdr;
	}
	hdr->previous->next = hdr;
	lock->unlock(lock);

	return hdr + 1;
}

METHOD(leak_detective_t, destroy, void,
	private_leak_detective_t *this)
{
	disable_leak_detective();
	lock->destroy(lock);
	thread_disabled->destroy(thread_disabled);
	free(this);
}

/*
 * see header file
 */
leak_detective_t *leak_detective_create()
{
	private_leak_detective_t *this;

	INIT(this,
		.public = {
			.report = _report,
			.usage = _usage,
			.set_state = _set_state,
			.destroy = _destroy,
		},
	);

	lock = spinlock_create();
	thread_disabled = thread_value_create(NULL);

	init_static_allocations();

	if (getenv("LEAK_DETECTIVE_DISABLE") == NULL)
	{
		if (register_hooks())
		{
			enable_leak_detective();
		}
	}
	return &this->public;
}
