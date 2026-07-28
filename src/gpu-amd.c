/* gpu-amd.c -- AMD backend through ADL (atiadlxx.dll).
|
|  ADL rather than ADLX: ADL is a flat C API designed for GetProcAddress,
|  which is exactly what a MinGW built plugin wants.  ADLX is primarily a C++
|  SDK and its C wrapper drags in a good deal more machinery.
|
|  Two data paths, tried in order per adapter:
|
|    1. ADL2_New_QueryPMLogData_Get, the modern sensor block.  Available from
|       Vega (GCN family AI) onwards and by far the richest source.
|    2. Overdrive5 activity plus temperature, for older boards.
|
|  Sensor indices and struct layouts below were taken from the ADL SDK as
|  mirrored by LibreHardwareMonitor.  They are stable but they ARE an ABI
|  contract with a closed source DLL, so if a future driver reshuffles them
|  the symptom will be plausible-looking nonsense rather than a crash.  The
|  sanity clamps in amd_sample() exist for exactly that reason.
*/

#include "gpu-common.h"

#include <string.h>

#define ADL_OK				0
#define ADL_MAX_PATH			256
#define ADL_PMLOG_MAX_SENSORS		256
#define ADL_VENDOR_ID			0x1002

/* ADLPMLogSensors -- verified against the ADL SDK enumeration. */
#define PMLOG_CLK_GFXCLK		1
#define PMLOG_TEMPERATURE_EDGE		8
#define PMLOG_FAN_PERCENTAGE		15
#define PMLOG_SOC_POWER			17
#define PMLOG_INFO_ACTIVITY_GFX		19
#define PMLOG_ASIC_POWER		23
#define PMLOG_TEMPERATURE_HOTSPOT	27
#define PMLOG_TEMPERATURE_GFX		28
#define PMLOG_GFX_POWER			30
#define PMLOG_SSTOTAL_POWERLIMIT	47
#define PMLOG_BOARD_POWER		73

typedef void *ADL_CONTEXT_HANDLE;
typedef void *(__stdcall *ADL_MAIN_MALLOC_CALLBACK)(int);

typedef struct
	{
	int	iSize;
	int	iAdapterIndex;
	char	strUDID[ADL_MAX_PATH];
	int	iBusNumber;
	int	iDeviceNumber;
	int	iFunctionNumber;
	int	iVendorID;
	char	strAdapterName[ADL_MAX_PATH];
	char	strDisplayName[ADL_MAX_PATH];
	int	iPresent;
	int	iExist;
	char	strDriverPath[ADL_MAX_PATH];
	char	strDriverPathExt[ADL_MAX_PATH];
	char	strPNPString[ADL_MAX_PATH];
	int	iOSDisplayIndex;
	}
	AdapterInfo;

typedef struct
	{
	int	supported;
	int	value;
	}
	ADLSingleSensorData;

typedef struct
	{
	int			size;
	ADLSingleSensorData	sensors[ADL_PMLOG_MAX_SENSORS];
	}
	ADLPMLogDataOutput;

typedef struct
	{
	int	iSize;
	int	iEngineClock;		/* 10 kHz units    */
	int	iMemoryClock;
	int	iVddc;
	int	iActivityPercent;
	int	iCurrentPerformanceLevel;
	int	iCurrentBusSpeed;
	int	iCurrentBusLanes;
	int	iMaximumBusLanes;
	int	iReserved;
	}
	ADLPMActivity;

typedef struct
	{
	int	iSize;
	int	iTemperature;		/* millidegrees C  */
	}
	ADLTemperature;

typedef struct
	{
	long long	iMemorySize;	/* bytes */
	char		strMemoryType[ADL_MAX_PATH];
	long long	iMemoryBandwidth;
	long long	iHyperMemorySize;
	long long	iInvisibleMemorySize;
	long long	iVisibleMemorySize;
	long long	iVramVendorRevId;
	long long	iMemoryBandwidthX2;
	long long	iMemoryBitRateX2;
	}
	ADLMemoryInfoX4;

typedef int (*ADL2_Main_Control_Create_t)(ADL_MAIN_MALLOC_CALLBACK, int,
				ADL_CONTEXT_HANDLE *);
typedef int (*ADL2_Main_Control_Destroy_t)(ADL_CONTEXT_HANDLE);
typedef int (*ADL2_Adapter_NumberOfAdapters_Get_t)(ADL_CONTEXT_HANDLE, int *);
typedef int (*ADL2_Adapter_AdapterInfo_Get_t)(ADL_CONTEXT_HANDLE,
				AdapterInfo *, int);
typedef int (*ADL2_New_QueryPMLogData_Get_t)(ADL_CONTEXT_HANDLE, int,
				ADLPMLogDataOutput *);
typedef int (*ADL2_Adapter_DedicatedVRAMUsage_Get_t)(ADL_CONTEXT_HANDLE, int,
				int *);
typedef int (*ADL2_Adapter_MemoryInfoX4_Get_t)(ADL_CONTEXT_HANDLE, int,
				ADLMemoryInfoX4 *);
typedef int (*ADL2_Overdrive5_CurrentActivity_Get_t)(ADL_CONTEXT_HANDLE, int,
				ADLPMActivity *);
typedef int (*ADL2_Overdrive5_Temperature_Get_t)(ADL_CONTEXT_HANDLE, int, int,
				ADLTemperature *);

static HMODULE					adl_dll;
static ADL_CONTEXT_HANDLE			adl_ctx;

static ADL2_Main_Control_Create_t		p_Create;
static ADL2_Main_Control_Destroy_t		p_Destroy;
static ADL2_Adapter_NumberOfAdapters_Get_t	p_NumAdapters;
static ADL2_Adapter_AdapterInfo_Get_t		p_AdapterInfo;
static ADL2_New_QueryPMLogData_Get_t		p_PMLog;
static ADL2_Adapter_DedicatedVRAMUsage_Get_t	p_VramUsage;
static ADL2_Adapter_MemoryInfoX4_Get_t		p_MemInfo;
static ADL2_Overdrive5_CurrentActivity_Get_t	p_OD5Activity;
static ADL2_Overdrive5_Temperature_Get_t	p_OD5Temp;

/* One entry per physical AMD board, already mapped onto the DXGI list. */
typedef struct
	{
	int	adl_index;
	gint	adapter;	/* index into the DXGI adapter list */
	gint	mem_total_mb;
	}
	AmdBoard;

static AmdBoard	board[GPU_MAX];
static gint	n_boards;


static void * __stdcall
adl_malloc(int size)
	{
	return malloc((size_t) size);
	}

/* Read one PMLog sensor, or GPU_NA when the board does not support it. */
static gint
sensor(const ADLPMLogDataOutput *d, gint id)
	{
	if (id < 0 || id >= ADL_PMLOG_MAX_SENSORS)
		return GPU_NA;
	if (!d->sensors[id].supported)
		return GPU_NA;
	return d->sensors[id].value;
	}

/* First supported sensor from a NULL terminated preference list. */
static gint
sensor_any(const ADLPMLogDataOutput *d, const gint *ids)
	{
	gint	i, v;

	for (i = 0; ids[i] >= 0; ++i)
		{
		v = sensor(d, ids[i]);
		if (v >= 0)
			return v;
		}
	return GPU_NA;
	}

static gint
clamp_or_na(gint v, gint lo, gint hi)
	{
	if (v < lo || v > hi)
		return GPU_NA;
	return v;
	}


static gboolean
amd_open(void)
	{
	AdapterInfo	*info = NULL;
	gint		i, j, n_adl = 0;
	gboolean	has_amd = FALSE;
	gint		ordinal = 0;

	for (i = 0; i < gpu_adapter_count(); ++i)
		{
		if (gpu_adapter(i)->vendor_id == GPU_VENDOR_AMD)
			has_amd = TRUE;
		}
	if (!has_amd)
		return FALSE;

	adl_dll = LoadLibraryA("atiadlxx.dll");
	if (!adl_dll)
		adl_dll = LoadLibraryA("atiadlxy.dll");	/* 32 bit fallback */
	if (!adl_dll)
		return FALSE;

	p_Create = (ADL2_Main_Control_Create_t)
		GetProcAddress(adl_dll, "ADL2_Main_Control_Create");
	p_Destroy = (ADL2_Main_Control_Destroy_t)
		GetProcAddress(adl_dll, "ADL2_Main_Control_Destroy");
	p_NumAdapters = (ADL2_Adapter_NumberOfAdapters_Get_t)
		GetProcAddress(adl_dll, "ADL2_Adapter_NumberOfAdapters_Get");
	p_AdapterInfo = (ADL2_Adapter_AdapterInfo_Get_t)
		GetProcAddress(adl_dll, "ADL2_Adapter_AdapterInfo_Get");
	p_PMLog = (ADL2_New_QueryPMLogData_Get_t)
		GetProcAddress(adl_dll, "ADL2_New_QueryPMLogData_Get");
	p_VramUsage = (ADL2_Adapter_DedicatedVRAMUsage_Get_t)
		GetProcAddress(adl_dll, "ADL2_Adapter_DedicatedVRAMUsage_Get");
	p_MemInfo = (ADL2_Adapter_MemoryInfoX4_Get_t)
		GetProcAddress(adl_dll, "ADL2_Adapter_MemoryInfoX4_Get");
	p_OD5Activity = (ADL2_Overdrive5_CurrentActivity_Get_t)
		GetProcAddress(adl_dll, "ADL2_Overdrive5_CurrentActivity_Get");
	p_OD5Temp = (ADL2_Overdrive5_Temperature_Get_t)
		GetProcAddress(adl_dll, "ADL2_Overdrive5_Temperature_Get");

	if (!p_Create || !p_Destroy || !p_NumAdapters || !p_AdapterInfo)
		goto fail;

	if (p_Create(adl_malloc, 1, &adl_ctx) != ADL_OK || !adl_ctx)
		goto fail;

	if (p_NumAdapters(adl_ctx, &n_adl) != ADL_OK || n_adl < 1)
		goto fail;

	info = g_try_malloc0(sizeof(AdapterInfo) * (gsize) n_adl);
	if (!info)
		goto fail;

	if (p_AdapterInfo(adl_ctx, info, (int)(sizeof(AdapterInfo) * n_adl))
				!= ADL_OK)
		goto fail;

	/* ADL reports one entry per adapter/display combination, so several
	|  entries share a PCI address.  Keep the first index for each. */
	for (i = 0; i < n_adl && n_boards < GPU_MAX; ++i)
		{
		gboolean	dup = FALSE;

		if (info[i].iVendorID != ADL_VENDOR_ID)
			continue;
		if (!info[i].iExist)
			continue;

		for (j = 0; j < i; ++j)
			{
			if (info[j].iVendorID == ADL_VENDOR_ID
				&& info[j].iExist
				&& info[j].iBusNumber == info[i].iBusNumber
				&& info[j].iDeviceNumber == info[i].iDeviceNumber
				&& info[j].iFunctionNumber
						== info[i].iFunctionNumber)
				{
				dup = TRUE;
				break;
				}
			}
		if (dup)
			continue;

		board[n_boards].adl_index = info[i].iAdapterIndex;
		board[n_boards].adapter = gpu_adapter_match(
					info[i].strAdapterName,
					GPU_VENDOR_AMD, ordinal);
		board[n_boards].mem_total_mb = GPU_NA;

		if (p_MemInfo)
			{
			ADLMemoryInfoX4	mi;

			memset(&mi, 0, sizeof(mi));
			if (p_MemInfo(adl_ctx, info[i].iAdapterIndex, &mi)
						== ADL_OK
					&& mi.iMemorySize > 0)
				board[n_boards].mem_total_mb = (gint)
					(mi.iMemorySize / (1024 * 1024));
			}

		++ordinal;
		if (board[n_boards].adapter >= 0)
			++n_boards;
		}

	g_free(info);

	if (n_boards < 1)
		goto fail;

	return TRUE;

fail:
	g_free(info);
	if (adl_ctx && p_Destroy)
		p_Destroy(adl_ctx);
	adl_ctx = NULL;
	if (adl_dll)
		FreeLibrary(adl_dll);
	adl_dll = NULL;
	n_boards = 0;
	return FALSE;
	}

static gboolean
amd_sample(GpuSample *out)
	{
	static const gint	temp_pref[] =
		{ PMLOG_TEMPERATURE_EDGE, PMLOG_TEMPERATURE_GFX,
		  PMLOG_TEMPERATURE_HOTSPOT, -1 };
	static const gint	power_pref[] =
		{ PMLOG_BOARD_POWER, PMLOG_ASIC_POWER, PMLOG_GFX_POWER,
		  PMLOG_SOC_POWER, -1 };

	ADLPMLogDataOutput	d;
	gint			b, idx;
	gboolean		got_pmlog;

	if (!adl_ctx)
		return FALSE;

	for (b = 0; b < n_boards; ++b)
		{
		GpuSample	*s;

		idx = board[b].adapter;
		if (idx < 0 || idx >= gpu_adapter_count())
			continue;
		s = &out[idx];

		got_pmlog = FALSE;
		if (p_PMLog)
			{
			memset(&d, 0, sizeof(d));
			if (p_PMLog(adl_ctx, board[b].adl_index, &d) == ADL_OK)
				got_pmlog = TRUE;
			}

		if (got_pmlog)
			{
			s->load  = clamp_or_na(
					sensor(&d, PMLOG_INFO_ACTIVITY_GFX),
					0, 100);
			s->temp  = clamp_or_na(sensor_any(&d, temp_pref),
					0, 130);
			s->power = clamp_or_na(sensor_any(&d, power_pref),
					0, 1000);
			s->fan   = clamp_or_na(
					sensor(&d, PMLOG_FAN_PERCENTAGE),
					0, 100);
			s->clock = clamp_or_na(sensor(&d, PMLOG_CLK_GFXCLK),
					0, 10000);
			s->power_limit = clamp_or_na(
					sensor(&d, PMLOG_SSTOTAL_POWERLIMIT),
					1, 1000);
			}
		else
			{
			/* Pre-Vega path. */
			if (p_OD5Activity)
				{
				ADLPMActivity	act;

				memset(&act, 0, sizeof(act));
				act.iSize = sizeof(act);
				if (p_OD5Activity(adl_ctx, board[b].adl_index,
							&act) == ADL_OK)
					{
					s->load = clamp_or_na(
						act.iActivityPercent, 0, 100);
					s->clock = clamp_or_na(
						act.iEngineClock / 100,
						0, 10000);
					}
				}

			if (p_OD5Temp)
				{
				ADLTemperature	t;

				memset(&t, 0, sizeof(t));
				t.iSize = sizeof(t);
				if (p_OD5Temp(adl_ctx, board[b].adl_index, 0,
							&t) == ADL_OK)
					s->temp = clamp_or_na(
						t.iTemperature / 1000, 0, 130);
				}
			}

		if (p_VramUsage)
			{
			int	mb = 0;

			if (p_VramUsage(adl_ctx, board[b].adl_index, &mb)
						== ADL_OK && mb > 0)
				s->mem_used = mb;
			}

		if (board[b].mem_total_mb > 0)
			s->mem_total = board[b].mem_total_mb;
		}

	return TRUE;
	}

static void
amd_close(void)
	{
	if (adl_ctx && p_Destroy)
		p_Destroy(adl_ctx);
	adl_ctx = NULL;

	if (adl_dll)
		FreeLibrary(adl_dll);
	adl_dll = NULL;

	n_boards = 0;
	}

GpuBackend	gpu_backend_amd =
	{
	"ADL",
	amd_open,
	amd_sample,
	amd_close,
	FALSE
	};
