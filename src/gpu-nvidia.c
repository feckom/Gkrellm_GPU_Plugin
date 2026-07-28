/* gpu-nvidia.c -- NVIDIA backend, driven by nvidia-smi.
|
|  nvidia-smi is the only one of the three vendor paths that is a process
|  rather than a library, so this backend keeps its own reader thread: a
|  single long lived child started with --loop=1 writes one CSV line per GPU
|  per second, and the thread parses those lines into a cache.  sample() then
|  just copies the cache, which keeps the core sampler free of blocking I/O.
|
|  The child is put into a kill-on-close job object so that it cannot outlive
|  GKrellM even if the plugin is unloaded uncleanly.
*/

#include "gpu-common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NV_MAX			GPU_MAX
#define SAMPLE_STALE_MS		5000

#define LOOP_QUERY \
	"--query-gpu=index,name,utilization.gpu," \
	"memory.used,memory.total,temperature.gpu,power.draw,power.limit," \
	"fan.speed,clocks.current.graphics " \
	"--format=csv,noheader,nounits --loop=1"

#define LOOP_FIELDS		10

static const DWORD	respawn_delay_ms[] = { 5000, 10000, 30000, 60000 };
#define N_RESPAWN_DELAYS \
	(sizeof(respawn_delay_ms) / sizeof(respawn_delay_ms[0]))

/* Cache written by the reader thread, read by sample(). */
typedef struct
	{
	GpuSample	s;
	gchar		name[GPU_NAME_LEN];
	ULONGLONG	stamp;
	}
	NvSlot;

static NvSlot		nv_slot[NV_MAX];
static CRITICAL_SECTION	nv_lock;
static gboolean		nv_lock_ready;

static gchar		nvsmi_path[MAX_PATH];
static HANDLE		job_object;
static HANDLE		reader_thread;
static HANDLE		stop_event;

/* Adapter index for each nvidia-smi index, resolved lazily once names are
|  known.  -1 means "not resolved yet". */
static gint		nv_to_adapter[NV_MAX];


/* ---- parsing ---- */

/* nvidia-smi prints plain integers with nounits, but power.draw and
|  power.limit come through with one or two decimals, and unsupported fields
|  arrive as [N/A] or [Not Supported]. */
static gint
parse_field(const gchar *s)
	{
	gdouble	d;
	gchar	*end = NULL;

	while (*s == ' ' || *s == '\t')
		++s;

	if (*s == '\0' || *s == '[' || *s == 'N')
		return GPU_NA;

	d = g_ascii_strtod(s, &end);
	if (end == s)
		return GPU_NA;
	if (d < 0.0)
		return GPU_NA;

	return (gint) (d + 0.5);
	}

static void
trim(gchar *s)
	{
	gchar	*p = s;
	gint	len;

	while (*p == ' ' || *p == '\t')
		++p;
	if (p != s)
		memmove(s, p, strlen(p) + 1);

	len = (gint) strlen(s);
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'
				|| s[len - 1] == '\r'))
		s[--len] = '\0';
	}

static void
publish_line(gchar *line)
	{
	gchar	*f[LOOP_FIELDS];
	gchar	*p;
	gint	i, idx;
	NvSlot	slot;

	p = line;
	for (i = 0; i < LOOP_FIELDS; ++i)
		{
		f[i] = p;
		p = strchr(p, ',');
		if (!p)
			{
			++i;
			break;
			}
		*p++ = '\0';
		}
	if (i < LOOP_FIELDS)
		return;

	idx = parse_field(f[0]);
	if (idx < 0 || idx >= NV_MAX)
		return;

	memset(&slot, 0, sizeof(slot));
	gpu_sample_clear(&slot.s);

	trim(f[1]);
	g_strlcpy(slot.name, f[1], GPU_NAME_LEN);

	slot.s.load        = parse_field(f[2]);
	slot.s.mem_used    = parse_field(f[3]);
	slot.s.mem_total   = parse_field(f[4]);
	slot.s.temp        = parse_field(f[5]);
	slot.s.power       = parse_field(f[6]);
	slot.s.power_limit = parse_field(f[7]);
	slot.s.fan         = parse_field(f[8]);
	slot.s.clock       = parse_field(f[9]);
	slot.stamp         = GetTickCount64();

	EnterCriticalSection(&nv_lock);
	nv_slot[idx] = slot;
	LeaveCriticalSection(&nv_lock);
	}

static void
consume(gchar *buf, gsize buf_size, gsize *fill, const gchar *data, DWORD len)
	{
	DWORD	i;

	for (i = 0; i < len; ++i)
		{
		gchar	c = data[i];

		if (c == '\n')
			{
			buf[*fill] = '\0';
			if (*fill > 0)
				publish_line(buf);
			*fill = 0;
			}
		else if (*fill + 1 < buf_size)
			{
			buf[*fill] = c;
			*fill += 1;
			}
		else
			*fill = 0;	/* Pathologically long line: drop. */
		}
	}


/* ---- process plumbing ---- */

static gboolean
locate_nvidia_smi(void)
	{
	gchar	buf[MAX_PATH];
	UINT	n;
	DWORD	len;

	n = GetSystemDirectoryA(buf, sizeof(buf));
	if (n > 0 && n < sizeof(buf))
		{
		g_snprintf(nvsmi_path, sizeof(nvsmi_path),
				"%s\\nvidia-smi.exe", buf);
		if (GetFileAttributesA(nvsmi_path) != INVALID_FILE_ATTRIBUTES)
			return TRUE;
		}

	len = GetEnvironmentVariableA("ProgramFiles", buf, sizeof(buf));
	if (len > 0 && len < sizeof(buf))
		{
		g_snprintf(nvsmi_path, sizeof(nvsmi_path),
			"%s\\NVIDIA Corporation\\NVSMI\\nvidia-smi.exe", buf);
		if (GetFileAttributesA(nvsmi_path) != INVALID_FILE_ATTRIBUTES)
			return TRUE;
		}

	len = SearchPathA(NULL, "nvidia-smi.exe", NULL,
				sizeof(nvsmi_path), nvsmi_path, NULL);
	if (len > 0 && len < sizeof(nvsmi_path))
		return TRUE;

	nvsmi_path[0] = '\0';
	return FALSE;
	}

static void
create_job_object(void)
	{
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION	info;

	job_object = CreateJobObjectA(NULL, NULL);
	if (!job_object)
		return;

	memset(&info, 0, sizeof(info));
	info.BasicLimitInformation.LimitFlags =
			JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

	if (!SetInformationJobObject(job_object,
				JobObjectExtendedLimitInformation,
				&info, sizeof(info)))
		{
		CloseHandle(job_object);
		job_object = NULL;
		}
	}

static gboolean
spawn_nvidia_smi(const gchar *args, HANDLE *read_pipe, HANDLE *proc)
	{
	SECURITY_ATTRIBUTES	sa;
	STARTUPINFOA		si;
	PROCESS_INFORMATION	pi;
	HANDLE			rd = NULL, wr = NULL;
	gchar			cmdline[MAX_PATH + 512];
	BOOL			ok;

	*read_pipe = NULL;
	*proc = NULL;

	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	if (!CreatePipe(&rd, &wr, &sa, 64 * 1024))
		return FALSE;

	/* The read end must not be inherited by the child, otherwise the pipe
	|  never signals EOF when the child exits. */
	if (!SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0))
		{
		CloseHandle(rd);
		CloseHandle(wr);
		return FALSE;
		}

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdOutput = wr;
	si.hStdError = wr;
	si.hStdInput = INVALID_HANDLE_VALUE;

	memset(&pi, 0, sizeof(pi));

	g_snprintf(cmdline, sizeof(cmdline), "\"%s\" %s", nvsmi_path, args);

	ok = CreateProcessA(nvsmi_path, cmdline, NULL, NULL,
			TRUE,
			CREATE_NO_WINDOW | CREATE_SUSPENDED,
			NULL, NULL, &si, &pi);

	CloseHandle(wr);

	if (!ok)
		{
		CloseHandle(rd);
		return FALSE;
		}

	/* Into the job before the child runs a single instruction. */
	if (job_object)
		AssignProcessToJobObject(job_object, pi.hProcess);

	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);

	*read_pipe = rd;
	*proc = pi.hProcess;
	return TRUE;
	}

static DWORD WINAPI
reader_main(LPVOID unused)
	{
	gchar	line[512];
	gchar	raw[8192];
	gsize	fill = 0;
	gint	fail_count = 0;

	(void) unused;

	while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0)
		{
		HANDLE	rd = NULL, proc = NULL;
		DWORD	delay;

		if (spawn_nvidia_smi(LOOP_QUERY, &rd, &proc))
			{
			fail_count = 0;
			fill = 0;

			for (;;)
				{
				DWORD	got = 0;

				if (WaitForSingleObject(stop_event, 0)
						== WAIT_OBJECT_0)
					break;

				if (!ReadFile(rd, raw, sizeof(raw), &got, NULL)
						|| got == 0)
					break;

				consume(line, sizeof(line), &fill, raw, got);
				}

			TerminateProcess(proc, 0);
			WaitForSingleObject(proc, 2000);
			CloseHandle(proc);
			CloseHandle(rd);
			}
		else if (fail_count < (gint) N_RESPAWN_DELAYS - 1)
			++fail_count;

		if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0)
			break;

		/* Wait, then try again, so the backend recovers by itself
		|  after a driver restart or a TDR event. */
		delay = respawn_delay_ms[fail_count];
		if (WaitForSingleObject(stop_event, delay) == WAIT_OBJECT_0)
			break;

		if (fail_count < (gint) N_RESPAWN_DELAYS - 1)
			++fail_count;
		}

	return 0;
	}


/* ---- backend interface ---- */

static gboolean
nvidia_open(void)
	{
	gint	i;
	gboolean has_nvidia = FALSE;

	for (i = 0; i < gpu_adapter_count(); ++i)
		{
		if (gpu_adapter(i)->vendor_id == GPU_VENDOR_NVIDIA)
			has_nvidia = TRUE;
		nv_to_adapter[i] = -1;
		}
	for (; i < NV_MAX; ++i)
		nv_to_adapter[i] = -1;

	if (!has_nvidia)
		return FALSE;
	if (!locate_nvidia_smi())
		return FALSE;

	for (i = 0; i < NV_MAX; ++i)
		{
		gpu_sample_clear(&nv_slot[i].s);
		nv_slot[i].stamp = 0;
		nv_slot[i].name[0] = '\0';
		}

	InitializeCriticalSection(&nv_lock);
	nv_lock_ready = TRUE;

	create_job_object();

	stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (!stop_event)
		return FALSE;

	reader_thread = CreateThread(NULL, 0, reader_main, NULL, 0, NULL);
	if (!reader_thread)
		{
		CloseHandle(stop_event);
		stop_event = NULL;
		return FALSE;
		}

	return TRUE;
	}

static gboolean
nvidia_sample(GpuSample *out)
	{
	NvSlot		snap[NV_MAX];
	ULONGLONG	now;
	gint		i, idx, ordinal = 0;

	if (!nv_lock_ready)
		return FALSE;

	EnterCriticalSection(&nv_lock);
	memcpy(snap, nv_slot, sizeof(snap));
	LeaveCriticalSection(&nv_lock);

	now = GetTickCount64();

	for (i = 0; i < NV_MAX; ++i)
		{
		if (snap[i].stamp == 0)
			continue;

		/* Resolve the nvidia-smi ordinal to a DXGI adapter once the
		|  board name has actually arrived. */
		if (nv_to_adapter[i] < 0)
			{
			nv_to_adapter[i] = gpu_adapter_match(snap[i].name,
						GPU_VENDOR_NVIDIA, ordinal);
			}
		++ordinal;

		idx = nv_to_adapter[i];
		if (idx < 0 || idx >= gpu_adapter_count())
			continue;

		if (now < snap[i].stamp
				|| (now - snap[i].stamp) >= SAMPLE_STALE_MS)
			continue;

		out[idx] = snap[i].s;
		}

	return TRUE;
	}

static void
nvidia_close(void)
	{
	if (stop_event)
		SetEvent(stop_event);

	if (reader_thread)
		{
		if (WaitForSingleObject(reader_thread, 3000) != WAIT_OBJECT_0)
			TerminateThread(reader_thread, 0);
		CloseHandle(reader_thread);
		reader_thread = NULL;
		}

	if (stop_event)
		{
		CloseHandle(stop_event);
		stop_event = NULL;
		}

	/* Closing the job kills any nvidia-smi that is somehow still alive. */
	if (job_object)
		{
		CloseHandle(job_object);
		job_object = NULL;
		}

	if (nv_lock_ready)
		{
		DeleteCriticalSection(&nv_lock);
		nv_lock_ready = FALSE;
		}
	}

GpuBackend	gpu_backend_nvidia =
	{
	"nvidia-smi",
	nvidia_open,
	nvidia_sample,
	nvidia_close,
	FALSE
	};
