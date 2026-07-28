/* gpu-common.h -- shared types for the gkrellm-gpu backends.
|
|  Every backend fills a GpuSample array indexed the same way as the adapter
|  list returned by gpu_adapters().  A field a backend cannot supply is left
|  at GPU_NA, and the core merges the backends in priority order so that a
|  vendor library always wins over the generic PDH counters.
*/

#ifndef GPU_COMMON_H
#define GPU_COMMON_H

#include <windows.h>
#include <glib.h>

#define GPU_MAX                 8
#define GPU_NAME_LEN            96

/* Every numeric field uses -1 for "this source cannot tell me". */
#define GPU_NA                  (-1)

#define GPU_VENDOR_NVIDIA       0x10DE
#define GPU_VENDOR_AMD          0x1002
#define GPU_VENDOR_INTEL        0x8086

typedef struct
	{
	gint	load;		/* percent                              */
	gint	mem_used;	/* MiB                                  */
	gint	mem_total;	/* MiB                                  */
	gint	temp;		/* degrees C                            */
	gint	power;		/* Watt                                 */
	gint	power_limit;	/* Watt                                 */
	gint	fan;		/* percent                              */
	gint	clock;		/* MHz, core                            */
	}
	GpuSample;

typedef struct
	{
	gchar	name[GPU_NAME_LEN];
	guint	vendor_id;	/* PCI vendor id, 0 if unknown          */
	guint64	luid;		/* DXGI adapter LUID, packed hi<<32|lo  */
	gint	vram_total_mb;	/* from DXGI dedicated video memory     */
	}
	GpuAdapter;

typedef struct _GpuBackend GpuBackend;

struct _GpuBackend
	{
	const gchar	*name;

	/* Called once at startup.  Returns FALSE if the backend is not usable
	|  on this machine (library missing, no matching adapter, unsupported
	|  Windows build); the core then simply skips it. */
	gboolean	(*open)(void);

	/* Fill out[0 .. gpu_adapter_count()-1].  The array arrives already
	|  reset to GPU_NA, so a backend only writes what it knows.  Returning
	|  FALSE marks the backend dead for the rest of the session. */
	gboolean	(*sample)(GpuSample *out);

	void		(*close)(void);

	gboolean	active;
	};

/* ---- adapter enumeration (gpu-adapter.c) ---- */

gboolean		gpu_adapters_init(void);
void			gpu_adapters_cleanup(void);
gint			gpu_adapter_count(void);
const GpuAdapter	*gpu_adapter(gint i);

/* Helper used by the vendor backends to line their own device ordering up
|  with the DXGI adapter list: match on name first, then fall back to the
|  nth adapter carrying the given PCI vendor id.  Returns -1 on no match. */
gint			gpu_adapter_match(const gchar *name, guint vendor_id,
					gint vendor_ordinal);

/* ---- backends ---- */

extern GpuBackend	gpu_backend_nvidia;
extern GpuBackend	gpu_backend_amd;
extern GpuBackend	gpu_backend_intel;
extern GpuBackend	gpu_backend_pdh;

/* ---- core (gpu-core.c) ---- */

gboolean		gpu_core_init(void);
void			gpu_core_cleanup(void);

/* Copy the most recent merged reading for every adapter.  age_ms is set to
|  the age of the reading in milliseconds, or -1 if nothing has arrived yet. */
void			gpu_core_read(GpuSample *out, gint *age_ms);

/* Space separated list of the backends that came up, for the info tab. */
const gchar		*gpu_core_backend_summary(void);

/* ---- small shared helpers ---- */

#define GPU_TAKE(dst, src) \
	do { if ((dst) < 0 && (src) >= 0) (dst) = (src); } while (0)

static inline void
gpu_sample_clear(GpuSample *s)
	{
	s->load = s->mem_used = s->mem_total = GPU_NA;
	s->temp = s->power = s->power_limit = GPU_NA;
	s->fan = s->clock = GPU_NA;
	}

#endif	/* GPU_COMMON_H */
