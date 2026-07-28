/* gpu-pdh.c -- vendor neutral GPU load and memory through the Windows
|  performance counters, the same source Task Manager uses.
|
|  Two counter sets carry everything that is available without a vendor SDK:
|
|      \GPU Engine(*)\Utilization Percentage
|          One instance per process, per adapter, per engine, named
|          pid_<n>_luid_0x<hi>_0x<lo>_phys_<n>_eng_<n>_engtype_<name>
|
|      \GPU Adapter Memory(*)\Dedicated Usage
|          One instance per adapter, named luid_0x<hi>_0x<lo>_phys_<n>
|
|  Task Manager's "GPU %" is not the sum of every engine, which would happily
|  exceed 100; it is the busiest single engine type.  So the instances are
|  summed per engine type and the largest of those sums is reported.
|
|  There is no temperature and no power here.  Those need a vendor library.
|
|  Requires Windows 10 1709 or newer; on anything older PdhAddEnglishCounter
|  fails for these paths and the backend deactivates itself.
*/

#include "gpu-common.h"

#include <pdh.h>
#include <stdio.h>
#include <string.h>

#ifndef PDH_MORE_DATA
#define PDH_MORE_DATA		((PDH_STATUS) 0x800007D2L)
#endif

#define MAX_ENGTYPES		12
#define ENGTYPE_LEN		24

typedef PDH_STATUS (WINAPI *PdhOpenQueryW_t)(LPCWSTR, DWORD_PTR, PDH_HQUERY *);
typedef PDH_STATUS (WINAPI *PdhAddEnglishCounterW_t)(PDH_HQUERY, LPCWSTR,
				DWORD_PTR, PDH_HCOUNTER *);
typedef PDH_STATUS (WINAPI *PdhCollectQueryData_t)(PDH_HQUERY);
typedef PDH_STATUS (WINAPI *PdhGetFormattedCounterArrayW_t)(PDH_HCOUNTER,
				DWORD, LPDWORD, LPDWORD,
				PPDH_FMT_COUNTERVALUE_ITEM_W);
typedef PDH_STATUS (WINAPI *PdhCloseQuery_t)(PDH_HQUERY);

static HMODULE				pdh_dll;
static PdhOpenQueryW_t			p_OpenQuery;
static PdhAddEnglishCounterW_t		p_AddCounter;
static PdhCollectQueryData_t		p_Collect;
static PdhGetFormattedCounterArrayW_t	p_GetArray;
static PdhCloseQuery_t			p_CloseQuery;

static PDH_HQUERY	query;
static PDH_HCOUNTER	c_engine;
static PDH_HCOUNTER	c_memory;

/* PdhGetFormattedCounterArray wants a caller supplied buffer that it sizes
|  for us on the first call.  Keeping it around avoids reallocating every
|  second; on a busy machine the engine array runs to a few hundred entries. */
static BYTE		*engine_buf;
static DWORD		engine_buf_size;
static BYTE		*memory_buf;
static DWORD		memory_buf_size;

/* Per adapter accumulator, rebuilt on every sample. */
typedef struct
	{
	gchar	type[MAX_ENGTYPES][ENGTYPE_LEN];
	gdouble	sum[MAX_ENGTYPES];
	gint	n;
	}
	EngAccum;


/* Pull "luid_0x<hi>_0x<lo>" out of a PDH instance name.  Returns FALSE if the
|  instance is not shaped the way the GPU counter sets name theirs. */
static gboolean
parse_luid(const gchar *inst, guint64 *out)
	{
	const gchar	*p;
	unsigned int	hi = 0, lo = 0;

	p = strstr(inst, "luid_0x");
	if (!p)
		return FALSE;

	if (sscanf(p, "luid_0x%x_0x%x", &hi, &lo) != 2)
		return FALSE;

	*out = ((guint64) hi << 32) | (guint64) lo;
	return TRUE;
	}

static const gchar *
parse_engtype(const gchar *inst)
	{
	const gchar	*p = strstr(inst, "engtype_");

	return p ? p + 8 : NULL;
	}

static void
accum_add(EngAccum *acc, const gchar *engtype, gdouble value)
	{
	gint	i;

	for (i = 0; i < acc->n; ++i)
		{
		if (!strcmp(acc->type[i], engtype))
			{
			acc->sum[i] += value;
			return;
			}
		}

	if (acc->n >= MAX_ENGTYPES)
		return;

	g_strlcpy(acc->type[acc->n], engtype, ENGTYPE_LEN);
	acc->sum[acc->n] = value;
	++acc->n;
	}

static gint
accum_busiest(const EngAccum *acc)
	{
	gdouble	best = 0.0;
	gint	i;

	if (acc->n < 1)
		return GPU_NA;

	for (i = 0; i < acc->n; ++i)
		{
		if (acc->sum[i] > best)
			best = acc->sum[i];
		}

	if (best < 0.0)
		best = 0.0;
	if (best > 100.0)
		best = 100.0;

	return (gint) (best + 0.5);
	}

/* Instance names are pure ASCII, so a narrowing copy is enough. */
static void
narrow(const WCHAR *src, gchar *dst, gint dst_size)
	{
	gint	i;

	if (!src)
		{
		dst[0] = '\0';
		return;
		}

	for (i = 0; i < dst_size - 1 && src[i]; ++i)
		dst[i] = (src[i] < 128) ? (gchar) src[i] : '?';
	dst[i] = '\0';
	}

static gint
luid_to_index(guint64 luid)
	{
	gint	i;

	for (i = 0; i < gpu_adapter_count(); ++i)
		{
		if (gpu_adapter(i)->luid == luid)
			return i;
		}
	return -1;
	}

/* Read one wildcard counter into a resizable buffer.  Returns the item count
|  and sets *items, or 0 on any failure. */
static DWORD
read_counter(PDH_HCOUNTER counter, BYTE **buf, DWORD *buf_size,
			PDH_FMT_COUNTERVALUE_ITEM_W **items)
	{
	PDH_STATUS	st;
	DWORD		size, count;

	*items = NULL;

	size = *buf_size;
	count = 0;
	st = p_GetArray(counter, PDH_FMT_DOUBLE, &size, &count,
			(PPDH_FMT_COUNTERVALUE_ITEM_W) *buf);

	if (st == PDH_MORE_DATA)
		{
		BYTE	*grown = g_try_realloc(*buf, size);

		if (!grown)
			return 0;

		*buf = grown;
		*buf_size = size;

		count = 0;
		st = p_GetArray(counter, PDH_FMT_DOUBLE, &size, &count,
				(PPDH_FMT_COUNTERVALUE_ITEM_W) *buf);
		}

	if (st != ERROR_SUCCESS || count == 0)
		return 0;

	*items = (PDH_FMT_COUNTERVALUE_ITEM_W *) *buf;
	return count;
	}


static gboolean
pdh_open(void)
	{
	pdh_dll = LoadLibraryA("pdh.dll");
	if (!pdh_dll)
		return FALSE;

	p_OpenQuery  = (PdhOpenQueryW_t)
			GetProcAddress(pdh_dll, "PdhOpenQueryW");
	p_AddCounter = (PdhAddEnglishCounterW_t)
			GetProcAddress(pdh_dll, "PdhAddEnglishCounterW");
	p_Collect    = (PdhCollectQueryData_t)
			GetProcAddress(pdh_dll, "PdhCollectQueryData");
	p_GetArray   = (PdhGetFormattedCounterArrayW_t)
			GetProcAddress(pdh_dll, "PdhGetFormattedCounterArrayW");
	p_CloseQuery = (PdhCloseQuery_t)
			GetProcAddress(pdh_dll, "PdhCloseQuery");

	if (!p_OpenQuery || !p_AddCounter || !p_Collect || !p_GetArray
			|| !p_CloseQuery)
		goto fail;

	if (p_OpenQuery(NULL, 0, &query) != ERROR_SUCCESS)
		goto fail;

	/* English counter names, because the localized ones differ per install
	|  and this has to work on a Slovak Windows too. */
	if (p_AddCounter(query, L"\\GPU Engine(*)\\Utilization Percentage",
				0, &c_engine) != ERROR_SUCCESS)
		c_engine = NULL;

	if (p_AddCounter(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage",
				0, &c_memory) != ERROR_SUCCESS)
		c_memory = NULL;

	if (!c_engine && !c_memory)
		{
		p_CloseQuery(query);
		query = NULL;
		goto fail;
		}

	engine_buf_size = 64 * 1024;
	engine_buf = g_try_malloc(engine_buf_size);
	memory_buf_size = 8 * 1024;
	memory_buf = g_try_malloc(memory_buf_size);

	if (!engine_buf || !memory_buf)
		{
		p_CloseQuery(query);
		query = NULL;
		goto fail;
		}

	/* Utilization Percentage is a rate, so the first collection only
	|  primes the counter; the value is meaningless until the second. */
	p_Collect(query);

	return TRUE;

fail:
	g_free(engine_buf);
	engine_buf = NULL;
	g_free(memory_buf);
	memory_buf = NULL;
	FreeLibrary(pdh_dll);
	pdh_dll = NULL;
	return FALSE;
	}

static gboolean
pdh_sample(GpuSample *out)
	{
	EngAccum			acc[GPU_MAX];
	PDH_FMT_COUNTERVALUE_ITEM_W	*items;
	DWORD				count, i;
	gchar				inst[256];
	guint64				luid;
	const gchar			*engtype;
	gint				idx, n = gpu_adapter_count();

	if (!query)
		return FALSE;

	if (p_Collect(query) != ERROR_SUCCESS)
		return TRUE;	/* A transient miss is not fatal. */

	memset(acc, 0, sizeof(acc));

	if (c_engine)
		{
		count = read_counter(c_engine, &engine_buf, &engine_buf_size,
					&items);

		for (i = 0; i < count; ++i)
			{
			narrow(items[i].szName, inst, sizeof(inst));

			if (!parse_luid(inst, &luid))
				continue;

			idx = luid_to_index(luid);
			if (idx < 0)
				continue;

			engtype = parse_engtype(inst);
			if (!engtype)
				continue;

			accum_add(&acc[idx], engtype,
					items[i].FmtValue.doubleValue);
			}

		for (idx = 0; idx < n; ++idx)
			out[idx].load = accum_busiest(&acc[idx]);
		}

	if (c_memory)
		{
		count = read_counter(c_memory, &memory_buf, &memory_buf_size,
					&items);

		for (i = 0; i < count; ++i)
			{
			narrow(items[i].szName, inst, sizeof(inst));

			if (!parse_luid(inst, &luid))
				continue;

			idx = luid_to_index(luid);
			if (idx < 0)
				continue;

			out[idx].mem_used = (gint)
				(items[i].FmtValue.doubleValue
					/ (1024.0 * 1024.0));
			}
		}

	/* DXGI already told us how much dedicated memory each board has. */
	for (idx = 0; idx < n; ++idx)
		out[idx].mem_total = gpu_adapter(idx)->vram_total_mb;

	return TRUE;
	}

static void
pdh_close(void)
	{
	if (query && p_CloseQuery)
		p_CloseQuery(query);
	query = NULL;

	g_free(engine_buf);
	engine_buf = NULL;
	g_free(memory_buf);
	memory_buf = NULL;

	if (pdh_dll)
		FreeLibrary(pdh_dll);
	pdh_dll = NULL;
	}

GpuBackend	gpu_backend_pdh =
	{
	"PDH",
	pdh_open,
	pdh_sample,
	pdh_close,
	FALSE
	};
