/* gpu-adapter.c -- enumerate display adapters through DXGI.
|
|  DXGI is the one source that is present on every supported Windows build,
|  needs no vendor SDK, and gives all four things the rest of the plugin has
|  to have: a human readable name, the PCI vendor id, the total amount of
|  dedicated video memory, and the adapter LUID that the PDH GPU performance
|  counters key their instance names on.
|
|  Everything else in the plugin is indexed by position in this list.
*/

#define COBJMACROS

#include "gpu-common.h"

#include <dxgi.h>
#include <string.h>

/* Declared locally rather than pulled in from libdxguid, which keeps the link
|  line to plain Win32 imports. */
static const GUID	k_IID_IDXGIFactory1 =
	{ 0x770aae78, 0xf26f, 0x4dba,
		{ 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };

#ifndef DXGI_ADAPTER_FLAG_SOFTWARE
#define DXGI_ADAPTER_FLAG_SOFTWARE	2
#endif

typedef HRESULT (WINAPI *CreateDXGIFactory1_t)(REFIID riid, void **ppFactory);

static GpuAdapter	adapters[GPU_MAX];
static gint		n_adapters;


static void
wide_to_utf8(const WCHAR *src, gchar *dst, gint dst_size)
	{
	gint	n;

	dst[0] = '\0';
	if (!src)
		return;

	n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_size, NULL, NULL);
	if (n <= 0)
		dst[0] = '\0';
	else
		dst[dst_size - 1] = '\0';
	}

/* "NVIDIA GeForce RTX 3060" and friends are already fine, but Intel likes to
|  append "(R)" and AMD "(TM)"; strip them so that the panel text stays short
|  and so that name matching against the vendor libraries is more forgiving. */
static void
tidy_name(gchar *s)
	{
	static const gchar	*junk[] = { "(R)", "(TM)", "(r)", "(tm)", NULL };
	gchar			*p;
	gint			i, len;

	for (i = 0; junk[i]; ++i)
		{
		while ((p = strstr(s, junk[i])) != NULL)
			{
			len = (gint) strlen(junk[i]);
			memmove(p, p + len, strlen(p + len) + 1);
			}
		}

	/* Collapse the double spaces the removal above can leave behind. */
	while ((p = strstr(s, "  ")) != NULL)
		memmove(p, p + 1, strlen(p + 1) + 1);

	len = (gint) strlen(s);
	while (len > 0 && s[len - 1] == ' ')
		s[--len] = '\0';
	}


gboolean
gpu_adapters_init(void)
	{
	HMODULE			dxgi;
	CreateDXGIFactory1_t	create;
	IDXGIFactory1		*factory = NULL;
	IDXGIAdapter1		*adapter;
	DXGI_ADAPTER_DESC1	desc;
	UINT			i;

	n_adapters = 0;

	dxgi = LoadLibraryA("dxgi.dll");
	if (!dxgi)
		return FALSE;

	create = (CreateDXGIFactory1_t)
			GetProcAddress(dxgi, "CreateDXGIFactory1");
	if (!create)
		{
		FreeLibrary(dxgi);
		return FALSE;
		}

	if (FAILED(create(&k_IID_IDXGIFactory1, (void **) &factory)) || !factory)
		{
		FreeLibrary(dxgi);
		return FALSE;
		}

	for (i = 0; n_adapters < GPU_MAX; ++i)
		{
		if (IDXGIFactory1_EnumAdapters1(factory, i, &adapter)
					== DXGI_ERROR_NOT_FOUND)
			break;

		if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc)))
			{
			/* Skip the Basic Render Driver and any other software
			|  adapter; they have no sensors to report. */
			if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
				{
				GpuAdapter	*a = &adapters[n_adapters++];

				wide_to_utf8(desc.Description, a->name,
							GPU_NAME_LEN);
				tidy_name(a->name);

				a->vendor_id = (guint) desc.VendorId;
				a->luid = ((guint64)(guint32) desc.AdapterLuid.HighPart
						<< 32)
					| (guint64)(guint32) desc.AdapterLuid.LowPart;
				a->vram_total_mb = (gint)
					(desc.DedicatedVideoMemory
						/ (1024 * 1024));

				if (a->vram_total_mb <= 0)
					a->vram_total_mb = GPU_NA;
				}
			}

		IDXGIAdapter1_Release(adapter);
		}

	IDXGIFactory1_Release(factory);

	/* The factory holds its own reference, so the module can stay loaded
	|  for the life of the process without being tracked. */

	return (n_adapters > 0);
	}

void
gpu_adapters_cleanup(void)
	{
	n_adapters = 0;
	}

gint
gpu_adapter_count(void)
	{
	return n_adapters;
	}

const GpuAdapter *
gpu_adapter(gint i)
	{
	if (i < 0 || i >= n_adapters)
		return NULL;
	return &adapters[i];
	}

gint
gpu_adapter_match(const gchar *name, guint vendor_id, gint vendor_ordinal)
	{
	gint	i, seen;

	/* An exact name match is the most reliable key we have; the vendor
	|  libraries and DXGI usually agree on the marketing name. */
	if (name && *name)
		{
		for (i = 0; i < n_adapters; ++i)
			{
			if (vendor_id && adapters[i].vendor_id != vendor_id)
				continue;
			if (!g_ascii_strcasecmp(adapters[i].name, name))
				return i;
			}

		/* Then a looser containment test, which copes with the
		|  "(TM)"/"(R)" noise stripped above and with the vendor
		|  library using a slightly longer or shorter string. */
		for (i = 0; i < n_adapters; ++i)
			{
			if (vendor_id && adapters[i].vendor_id != vendor_id)
				continue;
			if (strstr(adapters[i].name, name)
					|| strstr(name, adapters[i].name))
				return i;
			}
		}

	/* Last resort: the nth adapter belonging to this vendor, which is
	|  correct as long as both lists enumerate in PCI order. */
	if (vendor_ordinal >= 0 && vendor_id)
		{
		for (i = 0, seen = 0; i < n_adapters; ++i)
			{
			if (adapters[i].vendor_id != vendor_id)
				continue;
			if (seen == vendor_ordinal)
				return i;
			++seen;
			}
		}

	return -1;
	}
