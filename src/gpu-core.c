/* gpu-core.c -- backend registry, sampling thread and merge.
|
|  The sampler runs on its own thread at 1 Hz so that a vendor library taking
|  its time can never stall the GKrellM main loop.  Each active backend fills
|  a private array, and the arrays are then merged field by field in priority
|  order: whoever comes first and has an actual number wins.
|
|  Priority is vendor library, then PDH.  PDH knows load and memory for every
|  board on the machine, but it can never know temperature or power, so it
|  serves as the floor rather than the authority.
*/

#include "gpu-common.h"

#include <string.h>

#define SAMPLE_PERIOD_MS	1000

/* Order matters: this is the merge priority. */
static GpuBackend	*backends[] =
	{
	&gpu_backend_nvidia,
	&gpu_backend_amd,
	&gpu_backend_intel,
	&gpu_backend_pdh
	};

#define N_BACKENDS	(sizeof(backends) / sizeof(backends[0]))

static GpuSample	merged[GPU_MAX];
static ULONGLONG	merged_stamp;
static CRITICAL_SECTION	merged_lock;
static gboolean		lock_ready;

static HANDLE		sampler_thread;
static HANDLE		stop_event;

static gchar		summary[160];


static void
merge_one(GpuSample *dst, const GpuSample *src)
	{
	GPU_TAKE(dst->load,        src->load);
	GPU_TAKE(dst->mem_used,    src->mem_used);
	GPU_TAKE(dst->mem_total,   src->mem_total);
	GPU_TAKE(dst->temp,        src->temp);
	GPU_TAKE(dst->power,       src->power);
	GPU_TAKE(dst->power_limit, src->power_limit);
	GPU_TAKE(dst->fan,         src->fan);
	GPU_TAKE(dst->clock,       src->clock);
	}

static void
sample_once(void)
	{
	GpuSample	scratch[GPU_MAX];
	GpuSample	result[GPU_MAX];
	gint		i, n = gpu_adapter_count();
	gsize		b;

	for (i = 0; i < GPU_MAX; ++i)
		gpu_sample_clear(&result[i]);

	for (b = 0; b < N_BACKENDS; ++b)
		{
		if (!backends[b]->active)
			continue;

		for (i = 0; i < GPU_MAX; ++i)
			gpu_sample_clear(&scratch[i]);

		if (!backends[b]->sample(scratch))
			{
			/* A backend that reports a hard failure is retired
			|  rather than retried forever. */
			backends[b]->active = FALSE;
			continue;
			}

		for (i = 0; i < n; ++i)
			merge_one(&result[i], &scratch[i]);
		}

	/* DXGI is the last resort for total memory; it always knows. */
	for (i = 0; i < n; ++i)
		{
		if (result[i].mem_total < 0)
			result[i].mem_total = gpu_adapter(i)->vram_total_mb;
		}

	EnterCriticalSection(&merged_lock);
	memcpy(merged, result, sizeof(merged));
	merged_stamp = GetTickCount64();
	LeaveCriticalSection(&merged_lock);
	}

static DWORD WINAPI
sampler_main(LPVOID unused)
	{
	(void) unused;

	while (WaitForSingleObject(stop_event, SAMPLE_PERIOD_MS)
				== WAIT_TIMEOUT)
		sample_once();

	return 0;
	}


gboolean
gpu_core_init(void)
	{
	gsize	b;
	gint	i;
	gchar	*p = summary;
	gsize	left = sizeof(summary);

	if (!gpu_adapters_init())
		return FALSE;

	for (i = 0; i < GPU_MAX; ++i)
		gpu_sample_clear(&merged[i]);
	merged_stamp = 0;

	InitializeCriticalSection(&merged_lock);
	lock_ready = TRUE;

	summary[0] = '\0';
	for (b = 0; b < N_BACKENDS; ++b)
		{
		backends[b]->active = backends[b]->open
					? backends[b]->open() : FALSE;

		if (backends[b]->active)
			{
			gint	used = g_snprintf(p, left, "%s%s",
						(p == summary) ? "" : ", ",
						backends[b]->name);
			if (used > 0 && (gsize) used < left)
				{
				p += used;
				left -= (gsize) used;
				}
			}
		}

	if (summary[0] == '\0')
		g_strlcpy(summary, "none", sizeof(summary));

	stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (!stop_event)
		return FALSE;

	/* One synchronous pass so the panel is not blank for the first
	|  second; the rate style counters will still read zero until the
	|  thread has taken its second sample. */
	sample_once();

	sampler_thread = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
	if (!sampler_thread)
		{
		CloseHandle(stop_event);
		stop_event = NULL;
		return FALSE;
		}

	return TRUE;
	}

void
gpu_core_cleanup(void)
	{
	gsize	b;

	if (stop_event)
		SetEvent(stop_event);

	if (sampler_thread)
		{
		if (WaitForSingleObject(sampler_thread, 3000) != WAIT_OBJECT_0)
			TerminateThread(sampler_thread, 0);
		CloseHandle(sampler_thread);
		sampler_thread = NULL;
		}

	if (stop_event)
		{
		CloseHandle(stop_event);
		stop_event = NULL;
		}

	for (b = 0; b < N_BACKENDS; ++b)
		{
		if (backends[b]->close)
			backends[b]->close();
		backends[b]->active = FALSE;
		}

	gpu_adapters_cleanup();

	if (lock_ready)
		{
		DeleteCriticalSection(&merged_lock);
		lock_ready = FALSE;
		}
	}

void
gpu_core_read(GpuSample *out, gint *age_ms)
	{
	ULONGLONG	stamp, now;

	if (!lock_ready)
		{
		gint	i;

		for (i = 0; i < GPU_MAX; ++i)
			gpu_sample_clear(&out[i]);
		*age_ms = -1;
		return;
		}

	EnterCriticalSection(&merged_lock);
	memcpy(out, merged, sizeof(GpuSample) * GPU_MAX);
	stamp = merged_stamp;
	LeaveCriticalSection(&merged_lock);

	if (stamp == 0)
		{
		*age_ms = -1;
		return;
		}

	now = GetTickCount64();
	*age_ms = (now >= stamp) ? (gint) (now - stamp) : 0;
	}

const gchar *
gpu_core_backend_summary(void)
	{
	return summary;
	}
