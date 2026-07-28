/* gkrellm-gpu.c -- vendor neutral GPU monitor plugin for GKrellM Win32.
|
|  The plugin proper: one chart plus one panel per adapter.  All the hardware
|  access lives behind gpu_core_read(); this file never talks to a driver.
|
|  Successor to gkrellm-nvidia, which spoke only to nvidia-smi.
*/

#include <gkrellm2/gkrellm.h>

#include "gpu-common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLUGIN_NAME		"GPU"
#define PLUGIN_CONFIG_KEYWORD	"gpu"
#define PLUGIN_PLACEMENT	MON_MAIL

#define MAX_GPUS		GPU_MAX

#define DEFAULT_PANEL_FORMAT	"GPU$i $t\xc2\xb0" "C $p" "W"

/* A reading older than this means the sampler thread has stopped feeding us
|  and the panel should fall back to "--" rather than show a frozen value. */
#define SAMPLE_STALE_MS		5000

/* ---- chart series -------------------------------------------------
|  Three line traces on one shared 0..100 scale:
|
|      load   percent, straight from the backend
|      temp   degrees Celsius against TEMP_FULL_SCALE
|      power  Watt against the board power limit, or against the highest
|             draw seen so far when no limit is reported
|
|  GKrellM blits the data layers in cd_list order, so a later series
|  overwrites an earlier one where the traces cross.  The indices ARE the
|  stacking order, back to front:
|
|      SERIES_POWER  drawn first  -> always in the background
|      SERIES_LOAD   drawn second
|      SERIES_TEMP   drawn last   -> always fully visible on top
|
|  Changing these three numbers restacks the chart; the colour, label and
|  value tables are all indexed by them.
*/
#define SERIES_POWER		0
#define SERIES_LOAD		1
#define SERIES_TEMP		2
#define N_SERIES		3

#define TEMP_FULL_SCALE		100	/* degrees C == full chart height */

/* GKrellM only alternates between the two theme data colours, so a third
|  trace would be indistinguishable from the first.  Fixed here instead. */
#define COLOR_LOAD		"#ffffff"
#define COLOR_TEMP		"#e01c1c"
#define COLOR_POWER		"#9a9a9a"

#define GRID_LOAD		"#6a6a6a"
#define GRID_TEMP		"#701010"
#define GRID_POWER		"#454545"

/* MAX_CHARTHEIGHT in the GKrellM core.  The data pixmaps are rendered once
|  at this height so a runtime chart resize never needs a re-render. */
#define DATA_PIXMAP_HEIGHT	200


typedef struct
	{
	GkrellmChart		*chart;
	GkrellmChartconfig	*chart_config;
	GkrellmChartdata	*cd[N_SERIES];

	GkrellmPanel		*panel;
	GkrellmKrell		*krell;
	GkrellmDecal		*decal;

	/* Divisor used to map Watt onto the 0..100 chart scale. */
	gint			power_scale;

	gchar			last_text[128];
	}
	GpuMon;

static GpuMon		gpu_mon[MAX_GPUS];
static gint		n_gpus;

static GdkPixmap	*series_pixmap[N_SERIES];
static GdkPixmap	*series_grid_pixmap[N_SERIES];

static const gchar	*series_color[N_SERIES] =
			{
			[SERIES_POWER]	= COLOR_POWER,
			[SERIES_LOAD]	= COLOR_LOAD,
			[SERIES_TEMP]	= COLOR_TEMP
			};
static const gchar	*series_grid_color[N_SERIES] =
			{
			[SERIES_POWER]	= GRID_POWER,
			[SERIES_LOAD]	= GRID_LOAD,
			[SERIES_TEMP]	= GRID_TEMP
			};

static gint		chart_style_id;
static gint		meter_style_id;

static gchar		*panel_format;
static GtkWidget	*format_entry;

static GkrellmMonitor	*mon_gpu;

static gboolean		cleanup_registered;


/* ------------------------------------------------------------------ */
/* Text formatting                                                    */
/* ------------------------------------------------------------------ */

/* Expand the user format string.
|     $n  adapter name       $i  adapter index
|     $u  GPU load %         $t  temperature C
|     $p  power draw W       $L  power limit W
|     $f  fan speed %        $c  core clock MHz
|     $m  memory used MiB    $M  memory total MiB
|     $g  memory used GiB (one decimal)
|     $G  memory total GiB (one decimal)
|     $$  a literal dollar sign
|  Unavailable values expand to "--".
*/
static void
format_sample(gchar *dst, gsize dst_size, gint idx, const GpuSample *s,
			gboolean fresh)
	{
	const gchar	*f = panel_format ? panel_format : DEFAULT_PANEL_FORMAT;
	const GpuAdapter *ad = gpu_adapter(idx);
	gsize		o = 0;
	gchar		num[64];

	while (*f && o + 1 < dst_size)
		{
		if (*f != '$')
			{
			dst[o++] = *f++;
			continue;
			}

		++f;
		num[0] = '\0';

		switch (*f)
			{
			case 'n':
				if (ad)
					g_strlcpy(num, ad->name, sizeof(num));
				break;
			case 'i':
				g_snprintf(num, sizeof(num), "%d", idx);
				break;
			case 'u':
				if (fresh && s->load >= 0)
					g_snprintf(num, sizeof(num), "%d",
							s->load);
				break;
			case 't':
				if (fresh && s->temp >= 0)
					g_snprintf(num, sizeof(num), "%d",
							s->temp);
				break;
			case 'p':
				if (fresh && s->power >= 0)
					g_snprintf(num, sizeof(num), "%d",
							s->power);
				break;
			case 'L':
				if (fresh && s->power_limit >= 0)
					g_snprintf(num, sizeof(num), "%d",
							s->power_limit);
				break;
			case 'f':
				if (fresh && s->fan >= 0)
					g_snprintf(num, sizeof(num), "%d",
							s->fan);
				break;
			case 'c':
				if (fresh && s->clock >= 0)
					g_snprintf(num, sizeof(num), "%d",
							s->clock);
				break;
			case 'm':
				if (fresh && s->mem_used >= 0)
					g_snprintf(num, sizeof(num), "%d",
							s->mem_used);
				break;
			case 'M':
				if (s->mem_total >= 0)
					g_snprintf(num, sizeof(num), "%d",
							s->mem_total);
				break;
			case 'g':
				if (fresh && s->mem_used >= 0)
					g_snprintf(num, sizeof(num), "%.1f",
						(gdouble) s->mem_used / 1024.0);
				break;
			case 'G':
				if (s->mem_total >= 0)
					g_snprintf(num, sizeof(num), "%.1f",
						(gdouble) s->mem_total / 1024.0);
				break;
			case '$':
				num[0] = '$';
				num[1] = '\0';
				break;
			case '\0':
				continue;
			default:
				/* Unknown token: emit it verbatim so a typo is
				|  visible rather than silently swallowed. */
				num[0] = '$';
				num[1] = *f;
				num[2] = '\0';
				break;
			}

		if (num[0] == '\0')
			g_strlcpy(num, "--", sizeof(num));

		g_strlcpy(dst + o, num, dst_size - o);
		o += strlen(dst + o);
		++f;
		}

	dst[o < dst_size ? o : dst_size - 1] = '\0';
	}


/* ------------------------------------------------------------------ */
/* Chart drawing                                                      */
/* ------------------------------------------------------------------ */

/* Create (or resize) a pixmap filled with one flat colour.  GKrellM clips
|  the data layer through a 1 bit bitmap, so a plain colour block is all a
|  data layer source pixmap has to be. */
static void
make_solid_pixmap(GdkPixmap **pm, const gchar *spec, gint w, gint h)
	{
	GtkWidget	*top = gkrellm_get_top_window();
	GdkGC		*gc;
	GdkColor	color;
	gint		w_old = 0, h_old = 0;

	if (!pm || !top || !top->window || w < 1 || h < 1)
		return;

	if (*pm)
		{
		gdk_drawable_get_size(*pm, &w_old, &h_old);
		if (w_old == w && h_old == h)
			return;
		g_object_unref(G_OBJECT(*pm));
		*pm = NULL;
		}

	*pm = gdk_pixmap_new(top->window, w, h, -1);
	if (!*pm)
		return;

	if (!gdk_color_parse(spec, &color))
		{
		color.red = color.green = color.blue = 0xffff;
		color.pixel = 0;
		}

	gc = gdk_gc_new(*pm);
	gdk_gc_set_rgb_fg_color(gc, &color);
	gdk_draw_rectangle(*pm, gc, TRUE, 0, 0, w, h);
	g_object_unref(G_OBJECT(gc));
	}

/* Must run after gkrellm_chart_create() (the chart width is only known then)
|  and before gkrellm_add_chartdata(), which copies the grid pixmap pointer
|  by value. */
static void
render_series_pixmaps(void)
	{
	gint	w = gkrellm_chart_width();
	gint	i;

	for (i = 0; i < N_SERIES; ++i)
		{
		make_solid_pixmap(&series_pixmap[i], series_color[i],
					w, DATA_PIXMAP_HEIGHT);
		make_solid_pixmap(&series_grid_pixmap[i], series_grid_color[i],
					w, 1);
		}
	}

static void
draw_gpu_chart(GpuMon *g)
	{
	gkrellm_draw_chartdata(g->chart);
	gkrellm_draw_chart_to_screen(g->chart);
	}

static void
cb_chart_draw(GkrellmChart *cp, gpointer data)
	{
	(void) cp;
	draw_gpu_chart((GpuMon *) data);
	}

static gint
expose_event(GtkWidget *widget, GdkEventExpose *ev)
	{
	gint	i;

	for (i = 0; i < n_gpus; ++i)
		{
		GpuMon	*g = &gpu_mon[i];

		if (g->chart && widget == g->chart->drawing_area)
			gdk_draw_drawable(widget->window,
				gkrellm_draw_GC(1), g->chart->pixmap,
				ev->area.x, ev->area.y,
				ev->area.x, ev->area.y,
				ev->area.width, ev->area.height);
		else if (g->panel && widget == g->panel->drawing_area)
			gdk_draw_drawable(widget->window,
				gkrellm_draw_GC(1), g->panel->pixmap,
				ev->area.x, ev->area.y,
				ev->area.x, ev->area.y,
				ev->area.width, ev->area.height);
		}

	return FALSE;
	}


/* ------------------------------------------------------------------ */
/* Update                                                             */
/* ------------------------------------------------------------------ */

static void
update_plugin(void)
	{
	GkrellmTicks	*tk = gkrellm_ticks();
	GpuSample	snap[GPU_MAX];
	gint		age_ms;
	gint		i, j;
	gboolean	fresh;

	if (!tk->second_tick)
		return;

	gpu_core_read(snap, &age_ms);
	fresh = (age_ms >= 0 && age_ms < SAMPLE_STALE_MS);

	for (i = 0; i < n_gpus; ++i)
		{
		GpuMon		*g = &gpu_mon[i];
		GpuSample	*s = &snap[i];
		gchar		text[128];
		gint		v[N_SERIES];

		if (!g->chart)
			continue;

		/* Load is already a percentage. */
		v[SERIES_LOAD] = (fresh && s->load >= 0) ? s->load : 0;

		/* Temperature against a fixed full scale. */
		v[SERIES_TEMP] = (fresh && s->temp >= 0)
			? (gint) ((gint64) s->temp * 100 / TEMP_FULL_SCALE)
			: 0;

		/* Power against the board limit.  Boards that do not report a
		|  limit fall back to the largest draw seen so far, so the
		|  trace stays meaningful, just relative to the observed peak. */
		if (fresh && s->power_limit > 0)
			g->power_scale = s->power_limit;
		else if (fresh && s->power > g->power_scale)
			g->power_scale = s->power;

		if (fresh && s->power >= 0 && g->power_scale > 0)
			v[SERIES_POWER] = (gint) ((gint64) s->power * 100
						/ g->power_scale);
		else
			v[SERIES_POWER] = 0;

		for (j = 0; j < N_SERIES; ++j)
			v[j] = CLAMP(v[j], 0, 100);

		/* The argument order must match the order the series were
		|  added to the chart, which is the SERIES_* index order. */
		gkrellm_store_chartdata(g->chart, 0,
					(gulong) v[0],
					(gulong) v[1],
					(gulong) v[2]);
		draw_gpu_chart(g);

		if (g->panel)
			{
			format_sample(text, sizeof(text), i, s, fresh);

			if (strcmp(text, g->last_text))
				{
				g_strlcpy(g->last_text, text,
						sizeof(g->last_text));
				gkrellm_draw_decal_text(g->panel, g->decal,
							text, -1);
				}

			gkrellm_update_krell(g->panel, g->krell,
				(fresh && s->temp >= 0) ? (gulong) s->temp : 0);
			gkrellm_draw_panel_layers(g->panel);
			}
		}
	}


/* ------------------------------------------------------------------ */
/* Create                                                             */
/* ------------------------------------------------------------------ */

static void
create_plugin(GtkWidget *vbox, gint first_create)
	{
	static gchar		*series_label[N_SERIES] =
				{
				[SERIES_POWER]	= N_("Power"),
				[SERIES_LOAD]	= N_("Load"),
				[SERIES_TEMP]	= N_("Temp")
				};
	GkrellmStyle		*style;
	GkrellmTextstyle	*ts;
	gint			i, j;

	for (i = 0; i < n_gpus; ++i)
		{
		GpuMon	*g = &gpu_mon[i];

		if (first_create)
			{
			g->chart = gkrellm_chart_new0();
			g->panel = gkrellm_panel_new0();
			g->power_scale = 0;
			g->last_text[0] = '\0';
			}

		/* ---- chart ---- */
		gkrellm_set_chart_height_default(g->chart, 30);
		gkrellm_chart_create(vbox, mon_gpu, g->chart,
					&g->chart_config);

		render_series_pixmaps();

		for (j = 0; j < N_SERIES; ++j)
			{
			g->cd[j] = gkrellm_add_chartdata(g->chart,
					&series_pixmap[j],
					series_grid_pixmap[j],
					_(series_label[j]));
			if (!g->cd[j])
				continue;

			/* Instantaneous readings, not monotonically
			|  increasing counters, so the automatic delta
			|  calculation must be switched off. */
			gkrellm_monotonic_chartdata(g->cd[j], FALSE);

			/* A line trace only as the default; the user may still
			|  switch a series back to a filled impulse trace in the
			|  chart config window. */
			gkrellm_set_chartdata_draw_style_default(g->cd[j],
						CHARTDATA_LINE);

			gkrellm_set_chartdata_flags(g->cd[j],
						CHARTDATA_ALLOW_HIDE
						| CHARTDATA_NO_CONFIG_SPLIT
						| CHARTDATA_NO_CONFIG_INVERTED);
			}

		/* Four grids of 25 give a fixed 0..100 scale.  Forced on every
		|  create, not only the first, because all three series are
		|  pre-scaled to that range: an auto scale would make the traces
		|  meaningless relative to one another. */
		gkrellm_set_chartconfig_auto_grid_resolution(g->chart_config,
					FALSE);
		gkrellm_set_chartconfig_grid_resolution(g->chart_config, 25);
		gkrellm_set_chartconfig_fixed_grids(g->chart_config, 4);
		gkrellm_set_chartconfig_flags(g->chart_config,
					NO_CONFIG_AUTO_GRID_RESOLUTION
					| NO_CONFIG_FIXED_GRIDS);

		gkrellm_alloc_chartdata(g->chart);
		gkrellm_set_draw_chart_function(g->chart, cb_chart_draw, g);

		/* ---- panel ---- */
		style = gkrellm_meter_style(meter_style_id);
		ts = gkrellm_meter_textstyle(meter_style_id);

		g->krell = gkrellm_create_krell(g->panel,
				gkrellm_krell_meter_piximage(meter_style_id),
				style);
		gkrellm_set_krell_full_scale(g->krell, 100, 1);	/* deg C */

		g->decal = gkrellm_create_decal_text(g->panel, "8888",
					ts, style, -1, -1, -1);

		/* No panel label: gkrellm_panel_configure() always centres it
		|  in the top text row, exactly where the decal sits, and the
		|  two would overlap.  The adapter is identified by the $i or
		|  $n token in the panel format instead. */
		gkrellm_panel_configure(g->panel, NULL, style);
		gkrellm_panel_create(vbox, mon_gpu, g->panel);

		g->last_text[0] = '\0';

		if (first_create)
			{
			g_signal_connect(G_OBJECT(g->chart->drawing_area),
					"expose_event",
					G_CALLBACK(expose_event), NULL);
			g_signal_connect(G_OBJECT(g->panel->drawing_area),
					"expose_event",
					G_CALLBACK(expose_event), NULL);
			}
		}
	}


/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

static void
save_plugin_config(FILE *f)
	{
	gint	i;

	fprintf(f, "%s format %s\n", PLUGIN_CONFIG_KEYWORD,
			panel_format ? panel_format : DEFAULT_PANEL_FORMAT);

	for (i = 0; i < n_gpus; ++i)
		{
		gchar	sub[32];

		g_snprintf(sub, sizeof(sub), "chart%d", i);
		gkrellm_save_chartconfig(f, gpu_mon[i].chart_config,
					PLUGIN_CONFIG_KEYWORD, sub);
		}
	}

static void
load_plugin_config(gchar *arg)
	{
	gchar	config[32], item[CFG_BUFSIZE];
	gint	n;

	n = sscanf(arg, "%31s %[^\n]", config, item);
	if (n != 2)
		return;

	if (!strcmp(config, "format"))
		gkrellm_dup_string(&panel_format, item);
	else if (!strncmp(config, "chart", 5))
		{
		gint	i = atoi(config + 5);

		if (i >= 0 && i < MAX_GPUS)
			gkrellm_load_chartconfig(&gpu_mon[i].chart_config,
						item, N_SERIES);
		}
	}

static void
apply_plugin_config(void)
	{
	const gchar	*s;
	gint		i;

	if (!format_entry)
		return;

	s = gtk_entry_get_text(GTK_ENTRY(format_entry));
	gkrellm_dup_string(&panel_format, (gchar *) s);

	for (i = 0; i < n_gpus; ++i)
		gpu_mon[i].last_text[0] = '\0';
	}

static gchar	*plugin_info_text[] =
{
N_("<b>GPU monitor\n\n"),
N_("Monitors every display adapter in the machine, whichever vendor made "
   "it.  Adapters come from DXGI; the readings are merged from whichever "
   "of the following sources are present:\n\n"),
N_("\tnvidia-smi\tNVIDIA, everything\n"),
N_("\tADL\tAMD, everything\n"),
N_("\tLevel Zero\tIntel, everything\n"),
N_("\tPDH\tany vendor, load and memory only\n\n"),
N_("A vendor source always wins over PDH, which fills the gaps for boards "
   "with no vendor library installed.  Temperature and power are simply not "
   "available from PDH, and show as <b>--\n\n"),
N_("<b>Panel format\n"),
N_("\t$n\tadapter name\n"),
N_("\t$i\tadapter index\n"),
N_("\t$u\tGPU load, percent\n"),
N_("\t$t\ttemperature, degrees C\n"),
N_("\t$p\tpower draw, Watt\n"),
N_("\t$L\tpower limit, Watt\n"),
N_("\t$f\tfan speed, percent\n"),
N_("\t$c\tcore clock, MHz\n"),
N_("\t$m\tmemory used, MiB\n"),
N_("\t$M\tmemory total, MiB\n"),
N_("\t$g\tmemory used, GiB\n"),
N_("\t$G\tmemory total, GiB\n"),
N_("\t$$\ta literal dollar sign\n\n"),
N_("Values a particular board does not report are shown as <b>--\n\n"),
N_("<b>Chart\n"),
N_("Three line traces share one 0 to 100 scale, listed front to back:\n"),
N_("\tred\ttemperature, degrees Celsius (100 = full scale)\n"),
N_("\twhite\tGPU load, percent\n"),
N_("\tgrey\tpower draw, percent of the board power limit\n\n"),
N_("Power is drawn first so that it always stays behind the other two "
   "traces where they cross.\n\n"),
N_("Boards that do not report a power limit are scaled against the highest "
   "draw seen since GKrellM was started.\n\n"),
N_("Right click the chart to hide individual traces or to switch one back "
   "to a filled trace.\n")
};

static void
create_plugin_tab(GtkWidget *tab_vbox)
	{
	GtkWidget	*tabs, *vbox, *hbox, *text, *label;
	gchar		buf[256];
	gsize		i;

	tabs = gtk_notebook_new();
	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(tabs), GTK_POS_TOP);
	gtk_box_pack_start(GTK_BOX(tab_vbox), tabs, TRUE, TRUE, 0);

	/* ---- Options ---- */
	vbox = gkrellm_gtk_framed_notebook_page(tabs, _("Options"));

	hbox = gtk_hbox_new(FALSE, 4);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 4);

	label = gtk_label_new(_("Panel text format"));
	gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

	format_entry = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(format_entry),
		panel_format ? panel_format : DEFAULT_PANEL_FORMAT);
	gtk_box_pack_start(GTK_BOX(hbox), format_entry, TRUE, TRUE, 0);

	g_snprintf(buf, sizeof(buf), _("Active data sources: %s"),
			gpu_core_backend_summary());
	label = gtk_label_new(buf);
	gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
	gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 4);

	/* ---- Info ---- */
	vbox = gkrellm_gtk_framed_notebook_page(tabs, _("Info"));
	text = gkrellm_gtk_scrolled_text_view(vbox, NULL,
				GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

	for (i = 0; i < sizeof(plugin_info_text) / sizeof(gchar *); ++i)
		gkrellm_gtk_text_view_append(text, _(plugin_info_text[i]));
	}


/* ------------------------------------------------------------------ */
/* Monitor structure and entry point                                  */
/* ------------------------------------------------------------------ */

static GkrellmMonitor	plugin_mon =
	{
	PLUGIN_NAME,		/* Name, for config tab                 */
	0,			/* Id, 0 if a plugin                    */
	create_plugin,		/* The create function                  */
	update_plugin,		/* The update function                  */
	create_plugin_tab,	/* The config tab create function       */
	apply_plugin_config,	/* Apply the config function            */

	save_plugin_config,	/* Save user config                     */
	load_plugin_config,	/* Load user config                     */
	PLUGIN_CONFIG_KEYWORD,	/* config keyword                       */

	NULL,			/* Undefined 2                          */
	NULL,			/* Undefined 1                          */
	NULL,			/* Undefined 0                          */

	PLUGIN_PLACEMENT,	/* Insert plugin before this monitor    */
	NULL,			/* Handle if a plugin, filled by GKrellM*/
	NULL			/* Path if a plugin, filled by GKrellM  */
	};

GKRELLM_EXPORT GkrellmMonitor *
gkrellm_init_plugin(void)
	{
	/* Probe synchronously.  No GUI exists yet, and on a machine with no
	|  usable source this costs one DXGI enumeration. */
	if (!gpu_core_init())
		{
		gpu_core_cleanup();
		return NULL;		/* Silently not loaded. */
		}

	n_gpus = gpu_adapter_count();
	if (n_gpus <= 0 || n_gpus > MAX_GPUS)
		{
		gpu_core_cleanup();
		return NULL;
		}

	if (!cleanup_registered)
		{
		atexit(gpu_core_cleanup);
		cleanup_registered = TRUE;
		}

	mon_gpu = &plugin_mon;

	chart_style_id = gkrellm_add_chart_style(&plugin_mon, "gpu");
	meter_style_id = gkrellm_add_meter_style(&plugin_mon, "gpu");
	(void) chart_style_id;

	if (!panel_format)
		gkrellm_dup_string(&panel_format, DEFAULT_PANEL_FORMAT);

	return &plugin_mon;
	}
