/* gpu-intel.c -- Intel backend through the Level Zero sysman API
|  (ze_loader.dll, shipped with the Intel graphics driver).
|
|  Initialisation goes through zesInit() rather than the older
|  zeInit() + ZES_ENABLE_SYSMAN=1 dance.  zesInit does not require poking the
|  process environment, and it hands back real sysman device handles instead
|  of relying on the cast-a-core-handle trick, which Intel has since
|  documented as invalid.
|
|  Deliberately, this backend never calls any of the *GetProperties entry
|  points.  Those take structs whose first member is a stype enum that has to
|  match the loader's expectations exactly, and getting it wrong is a silent
|  ABI mismatch.  Everything here is derived from the *GetState/GetActivity
|  calls instead, which take either a plain scalar or a struct we can zero:
|
|      load   busiest engine group, same rule the PDH backend uses
|      temp   hottest of the reported sensors
|      power  differentiated energy counter
|      mem    memory module state
|
|  Every call is allowed to fail on its own; a board that reports nothing
|  simply falls through to the PDH numbers.
*/

#include "gpu-common.h"

#include <string.h>

#define ZE_RESULT_SUCCESS	0

typedef void *zes_driver_handle_t;
typedef void *zes_device_handle_t;
typedef void *zes_engine_handle_t;
typedef void *zes_temp_handle_t;
typedef void *zes_pwr_handle_t;
typedef void *zes_mem_handle_t;

/* Plain struct, no stype member. */
typedef struct
	{
	guint64	activeTime;	/* microseconds */
	guint64	timestamp;	/* microseconds */
	}
	zes_engine_stats_t;

/* These two do carry stype/pNext.  Zeroing them is accepted by the loader
|  for output structures; the fields we read follow the header. */
typedef struct
	{
	gint		stype;
	const void	*pNext;
	guint64		energy;		/* microjoules  */
	guint64		timestamp;	/* microseconds */
	}
	zes_power_energy_counter_t;

typedef struct
	{
	gint		stype;
	const void	*pNext;
	gint		health;
	guint64		free;		/* bytes */
	guint64		size;		/* bytes */
	}
	zes_mem_state_t;

typedef gint (*zesInit_t)(guint32);
typedef gint (*zesDriverGet_t)(guint32 *, zes_driver_handle_t *);
typedef gint (*zesDeviceGet_t)(zes_driver_handle_t, guint32 *,
				zes_device_handle_t *);
typedef gint (*zesDeviceEnumEngineGroups_t)(zes_device_handle_t, guint32 *,
				zes_engine_handle_t *);
typedef gint (*zesEngineGetActivity_t)(zes_engine_handle_t,
				zes_engine_stats_t *);
typedef gint (*zesDeviceEnumTemperatureSensors_t)(zes_device_handle_t,
				guint32 *, zes_temp_handle_t *);
typedef gint (*zesTemperatureGetState_t)(zes_temp_handle_t, gdouble *);
typedef gint (*zesDeviceEnumPowerDomains_t)(zes_device_handle_t, guint32 *,
				zes_pwr_handle_t *);
typedef gint (*zesPowerGetEnergyCounter_t)(zes_pwr_handle_t,
				zes_power_energy_counter_t *);
typedef gint (*zesDeviceEnumMemoryModules_t)(zes_device_handle_t, guint32 *,
				zes_mem_handle_t *);
typedef gint (*zesMemoryGetState_t)(zes_mem_handle_t, zes_mem_state_t *);

static HMODULE					ze_dll;
static zesInit_t				p_Init;
static zesDriverGet_t				p_DriverGet;
static zesDeviceGet_t				p_DeviceGet;
static zesDeviceEnumEngineGroups_t		p_EnumEngines;
static zesEngineGetActivity_t			p_EngineActivity;
static zesDeviceEnumTemperatureSensors_t	p_EnumTemps;
static zesTemperatureGetState_t			p_TempState;
static zesDeviceEnumPowerDomains_t		p_EnumPower;
static zesPowerGetEnergyCounter_t		p_PowerEnergy;
static zesDeviceEnumMemoryModules_t		p_EnumMemory;
static zesMemoryGetState_t			p_MemoryState;

#define MAX_ENGINES	32
#define MAX_TEMPS	8
#define MAX_POWER	4
#define MAX_MEMORY	4

typedef struct
	{
	zes_device_handle_t	dev;
	gint			adapter;	/* DXGI adapter index */

	zes_engine_handle_t	engine[MAX_ENGINES];
	guint32			n_engines;
	zes_engine_stats_t	prev_engine[MAX_ENGINES];
	gboolean		engine_primed;

	zes_temp_handle_t	temp[MAX_TEMPS];
	guint32			n_temps;

	zes_pwr_handle_t	power[MAX_POWER];
	guint32			n_power;
	guint64			prev_energy;
	guint64			prev_energy_ts;
	gboolean		power_primed;

	zes_mem_handle_t	memory[MAX_MEMORY];
	guint32			n_memory;
	}
	IntelDev;

static IntelDev	dev_list[GPU_MAX];
static gint	n_devs;


static gboolean
load_symbols(void)
	{
	ze_dll = LoadLibraryA("ze_loader.dll");
	if (!ze_dll)
		return FALSE;

	p_Init = (zesInit_t) GetProcAddress(ze_dll, "zesInit");
	p_DriverGet = (zesDriverGet_t) GetProcAddress(ze_dll, "zesDriverGet");
	p_DeviceGet = (zesDeviceGet_t) GetProcAddress(ze_dll, "zesDeviceGet");
	p_EnumEngines = (zesDeviceEnumEngineGroups_t)
		GetProcAddress(ze_dll, "zesDeviceEnumEngineGroups");
	p_EngineActivity = (zesEngineGetActivity_t)
		GetProcAddress(ze_dll, "zesEngineGetActivity");
	p_EnumTemps = (zesDeviceEnumTemperatureSensors_t)
		GetProcAddress(ze_dll, "zesDeviceEnumTemperatureSensors");
	p_TempState = (zesTemperatureGetState_t)
		GetProcAddress(ze_dll, "zesTemperatureGetState");
	p_EnumPower = (zesDeviceEnumPowerDomains_t)
		GetProcAddress(ze_dll, "zesDeviceEnumPowerDomains");
	p_PowerEnergy = (zesPowerGetEnergyCounter_t)
		GetProcAddress(ze_dll, "zesPowerGetEnergyCounter");
	p_EnumMemory = (zesDeviceEnumMemoryModules_t)
		GetProcAddress(ze_dll, "zesDeviceEnumMemoryModules");
	p_MemoryState = (zesMemoryGetState_t)
		GetProcAddress(ze_dll, "zesMemoryGetState");

	/* zesInit plus device enumeration is the hard requirement; the
	|  individual sensor groups are all optional. */
	return (p_Init && p_DriverGet && p_DeviceGet);
	}

static void
enum_resources(IntelDev *d)
	{
	guint32	n;

	if (p_EnumEngines && p_EngineActivity)
		{
		n = 0;
		if (p_EnumEngines(d->dev, &n, NULL) == ZE_RESULT_SUCCESS
				&& n > 0)
			{
			if (n > MAX_ENGINES)
				n = MAX_ENGINES;
			if (p_EnumEngines(d->dev, &n, d->engine)
					== ZE_RESULT_SUCCESS)
				d->n_engines = n;
			}
		}

	if (p_EnumTemps && p_TempState)
		{
		n = 0;
		if (p_EnumTemps(d->dev, &n, NULL) == ZE_RESULT_SUCCESS && n > 0)
			{
			if (n > MAX_TEMPS)
				n = MAX_TEMPS;
			if (p_EnumTemps(d->dev, &n, d->temp)
					== ZE_RESULT_SUCCESS)
				d->n_temps = n;
			}
		}

	if (p_EnumPower && p_PowerEnergy)
		{
		n = 0;
		if (p_EnumPower(d->dev, &n, NULL) == ZE_RESULT_SUCCESS && n > 0)
			{
			if (n > MAX_POWER)
				n = MAX_POWER;
			if (p_EnumPower(d->dev, &n, d->power)
					== ZE_RESULT_SUCCESS)
				d->n_power = n;
			}
		}

	if (p_EnumMemory && p_MemoryState)
		{
		n = 0;
		if (p_EnumMemory(d->dev, &n, NULL) == ZE_RESULT_SUCCESS
				&& n > 0)
			{
			if (n > MAX_MEMORY)
				n = MAX_MEMORY;
			if (p_EnumMemory(d->dev, &n, d->memory)
					== ZE_RESULT_SUCCESS)
				d->n_memory = n;
			}
		}
	}


static gboolean
intel_open(void)
	{
	zes_driver_handle_t	drivers[8];
	zes_device_handle_t	devices[GPU_MAX];
	guint32			n_drivers, n_devices, i, j;
	gint			ordinal = 0;
	gboolean		has_intel = FALSE;

	for (i = 0; i < (guint32) gpu_adapter_count(); ++i)
		{
		if (gpu_adapter((gint) i)->vendor_id == GPU_VENDOR_INTEL)
			has_intel = TRUE;
		}
	if (!has_intel)
		return FALSE;

	if (!load_symbols())
		goto fail;

	if (p_Init(0) != ZE_RESULT_SUCCESS)
		goto fail;

	n_drivers = 0;
	if (p_DriverGet(&n_drivers, NULL) != ZE_RESULT_SUCCESS
			|| n_drivers == 0)
		goto fail;
	if (n_drivers > 8)
		n_drivers = 8;
	if (p_DriverGet(&n_drivers, drivers) != ZE_RESULT_SUCCESS)
		goto fail;

	for (i = 0; i < n_drivers && n_devs < GPU_MAX; ++i)
		{
		n_devices = 0;
		if (p_DeviceGet(drivers[i], &n_devices, NULL)
					!= ZE_RESULT_SUCCESS
				|| n_devices == 0)
			continue;
		if (n_devices > GPU_MAX)
			n_devices = GPU_MAX;
		if (p_DeviceGet(drivers[i], &n_devices, devices)
					!= ZE_RESULT_SUCCESS)
			continue;

		for (j = 0; j < n_devices && n_devs < GPU_MAX; ++j)
			{
			IntelDev	*d = &dev_list[n_devs];

			memset(d, 0, sizeof(*d));
			d->dev = devices[j];

			/* Level Zero only ever enumerates Intel devices here,
			|  and gives no name we could match on, so ordinal
			|  matching within the Intel adapters is all we have. */
			d->adapter = gpu_adapter_match(NULL, GPU_VENDOR_INTEL,
						ordinal++);
			if (d->adapter < 0)
				continue;

			enum_resources(d);
			++n_devs;
			}
		}

	if (n_devs < 1)
		goto fail;

	return TRUE;

fail:
	if (ze_dll)
		FreeLibrary(ze_dll);
	ze_dll = NULL;
	n_devs = 0;
	return FALSE;
	}

static gboolean
intel_sample(GpuSample *out)
	{
	gint	i;
	guint32	k;

	if (n_devs < 1)
		return FALSE;

	for (i = 0; i < n_devs; ++i)
		{
		IntelDev	*d = &dev_list[i];
		GpuSample	*s;

		if (d->adapter < 0 || d->adapter >= gpu_adapter_count())
			continue;
		s = &out[d->adapter];

		/* ---- load: busiest engine group ---- */
		if (d->n_engines > 0)
			{
			gdouble	best = -1.0;

			for (k = 0; k < d->n_engines; ++k)
				{
				zes_engine_stats_t	now;
				guint64			d_act, d_ts;

				memset(&now, 0, sizeof(now));
				if (p_EngineActivity(d->engine[k], &now)
						!= ZE_RESULT_SUCCESS)
					continue;

				if (d->engine_primed
					&& now.timestamp > d->prev_engine[k].timestamp
					&& now.activeTime >= d->prev_engine[k].activeTime)
					{
					d_act = now.activeTime
						- d->prev_engine[k].activeTime;
					d_ts = now.timestamp
						- d->prev_engine[k].timestamp;

					if (d_ts > 0)
						{
						gdouble pct = 100.0
							* (gdouble) d_act
							/ (gdouble) d_ts;
						if (pct > best)
							best = pct;
						}
					}

				d->prev_engine[k] = now;
				}

			d->engine_primed = TRUE;

			if (best >= 0.0)
				{
				if (best > 100.0)
					best = 100.0;
				s->load = (gint) (best + 0.5);
				}
			}

		/* ---- temperature: hottest reported sensor ---- */
		if (d->n_temps > 0)
			{
			gdouble	hottest = -1.0;

			for (k = 0; k < d->n_temps; ++k)
				{
				gdouble	t = 0.0;

				if (p_TempState(d->temp[k], &t)
						== ZE_RESULT_SUCCESS
						&& t > hottest)
					hottest = t;
				}

			if (hottest > 0.0 && hottest < 130.0)
				s->temp = (gint) (hottest + 0.5);
			}

		/* ---- power: differentiated energy counter ---- */
		if (d->n_power > 0)
			{
			zes_power_energy_counter_t	e;

			memset(&e, 0, sizeof(e));
			if (p_PowerEnergy(d->power[0], &e) == ZE_RESULT_SUCCESS)
				{
				if (d->power_primed
					&& e.timestamp > d->prev_energy_ts
					&& e.energy >= d->prev_energy)
					{
					guint64	d_e = e.energy
							- d->prev_energy;
					guint64	d_t = e.timestamp
							- d->prev_energy_ts;

					/* microjoule per microsecond is watt. */
					if (d_t > 0)
						{
						gdouble w = (gdouble) d_e
							/ (gdouble) d_t;
						if (w >= 0.0 && w < 1000.0)
							s->power =
							(gint)(w + 0.5);
						}
					}

				d->prev_energy = e.energy;
				d->prev_energy_ts = e.timestamp;
				d->power_primed = TRUE;
				}
			}

		/* ---- memory ---- */
		if (d->n_memory > 0)
			{
			guint64	total = 0, freemem = 0;

			for (k = 0; k < d->n_memory; ++k)
				{
				zes_mem_state_t	st;

				memset(&st, 0, sizeof(st));
				if (p_MemoryState(d->memory[k], &st)
						== ZE_RESULT_SUCCESS)
					{
					total += st.size;
					freemem += st.free;
					}
				}

			if (total > 0)
				{
				s->mem_total = (gint)
					(total / (1024 * 1024));
				s->mem_used = (gint)
					((total - freemem) / (1024 * 1024));
				}
			}
		}

	return TRUE;
	}

static void
intel_close(void)
	{
	if (ze_dll)
		FreeLibrary(ze_dll);
	ze_dll = NULL;
	n_devs = 0;
	}

GpuBackend	gpu_backend_intel =
	{
	"Level Zero",
	intel_open,
	intel_sample,
	intel_close,
	FALSE
	};
