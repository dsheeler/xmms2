/**
 * @file
 * Equalizer-effect
 */

#include "xmms/xmms.h"
#include "xmms/plugin.h"
#include "xmms/effect.h"
#include "xmms/util.h"
#include "xmms/config_xmms.h"
#include "xmms/object.h"
#include "xmms/signal_xmms.h"

#include <math.h>
#include <glib.h>
#include <stdlib.h>

static void xmms_eq_init (xmms_effect_t *effect);
static void xmms_eq_deinit (xmms_effect_t *effect);
static void xmms_eq_samplerate_set (xmms_effect_t *effect, guint rate);
static void xmms_eq_process (xmms_effect_t *effect, gchar *buf, guint len);

typedef struct xmms_eq_filter_St {
	gdouble a[3];
	gdouble b[3];
	gdouble rinput[2], routput[2]; /* right channel memory */
	gdouble linput[2], loutput[2]; /* other channel memory */
} xmms_eq_filter_t;

#define XMMS_EQ_BANDS 10
#define XMMS_EQ_Q 1.4142

typedef struct xmms_eq_priv_St {
	xmms_eq_filter_t filters[XMMS_EQ_BANDS];
	gdouble gains[XMMS_EQ_BANDS];
	xmms_config_value_t *configvals[XMMS_EQ_BANDS];
	guint rate;
} xmms_eq_priv_t;

static gdouble freqs[XMMS_EQ_BANDS] = { 31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

/**
 * Calculate a filter with specified gain and relative center frequence
 */
static void
xmms_eq_calc_filter (xmms_eq_filter_t *filter, gdouble gain, gdouble relfreq)
{
	gdouble omega, sn, cs, alpha, temp1, temp2, A;
	
	A = sqrt (gain);
	omega = 2 * M_PI * relfreq;

	sn = sin (omega);
	cs = cos (omega);
	alpha = sn / (2.0 * XMMS_EQ_Q);
	temp1 = alpha * A;
	temp2 = alpha / A;

	filter->a[0] = 1.0 / (1.0 + temp2);
	filter->a[1] = (-2.0 * cs) * filter->a[0];
	filter->a[2] = (1.0 - temp2) * filter->a[0];
	filter->b[0] = (1.0 + temp1) * filter->a[0];
	filter->b[1] = filter->a[1];
	filter->b[2] = (1.0 - temp1) * filter->a[0];

}


xmms_plugin_t *
xmms_plugin_get (void)
{
	xmms_plugin_t *plugin;

	plugin = xmms_plugin_new (XMMS_PLUGIN_TYPE_EFFECT, "equalizer",
					    "Equalizer effect " XMMS_VERSION,
					    "Equalizer effect");


	xmms_plugin_info_add (plugin, "URL", "http://www.xmms.org/");
	xmms_plugin_info_add (plugin, "Author", "XMMS Team");

	xmms_plugin_method_add (plugin, XMMS_PLUGIN_METHOD_INIT, xmms_eq_init);
	xmms_plugin_method_add (plugin, XMMS_PLUGIN_METHOD_DEINIT, xmms_eq_deinit);
	xmms_plugin_method_add (plugin, XMMS_PLUGIN_METHOD_SAMPLERATE_SET, xmms_eq_samplerate_set);
	xmms_plugin_method_add (plugin, XMMS_PLUGIN_METHOD_PROCESS, xmms_eq_process);

	return plugin;
}

static void
xmms_eq_configval_changed (xmms_object_t *object, gconstpointer data, gpointer userdata)
{
	xmms_config_value_t *val = (xmms_config_value_t *)object;
	xmms_effect_t *effect = userdata;
	xmms_eq_priv_t *priv;
	const gchar *newval = data;
	gchar *name;
	gint i;

	priv = xmms_effect_plugin_data_get (effect);

	g_return_if_fail (priv);

	name = xmms_config_value_name_get (val);

	XMMS_DBG ("configval changed! %s => %s", name, newval);

	i = atoi (name+4);

	XMMS_DBG ("changing filter #%d", i);

	priv->gains[i] = atof (newval);

	xmms_eq_calc_filter (&priv->filters[i], 
			     priv->gains[i], ((gdouble)freqs[i])/((gdouble)priv->rate));

}

static void
xmms_eq_init (xmms_effect_t *effect) {
	xmms_eq_priv_t *priv;
	gint i;

	priv = g_new0 (xmms_eq_priv_t, 1);

	g_return_if_fail (priv);

	xmms_effect_plugin_data_set (effect, priv);

	for (i = 0; i < XMMS_EQ_BANDS; i++) {
		gchar buf[20];
		snprintf (buf, 20, "gain%d", i);
		priv->configvals[i] = xmms_effect_config_value_get (effect, g_strdup (buf), "1.0");
		g_return_if_fail (priv->configvals[i]);
		xmms_object_connect (XMMS_OBJECT (priv->configvals[i]),
				     XMMS_SIGNAL_CONFIG_VALUE_CHANGE,
				     xmms_eq_configval_changed, effect);

		priv->gains[i] = atof (xmms_config_value_as_string (priv->configvals[i]));
	}

}

static void
xmms_eq_deinit (xmms_effect_t *effect) {
	
}

static void
xmms_eq_samplerate_set (xmms_effect_t *effect, guint rate)
{
	gint i;
	xmms_eq_priv_t *priv = xmms_effect_plugin_data_get (effect);

	g_return_if_fail (priv);

	for (i=0; i<XMMS_EQ_BANDS; i++) {
		xmms_eq_calc_filter (&priv->filters[i], 
				     priv->gains[i], ((gdouble)freqs[i])/((gdouble)rate));
	}

	priv->rate = rate;

	XMMS_DBG ("calculatin' filter!");

}

static void
xmms_eq_process (xmms_effect_t *effect, gchar *buf, guint len)
{
	gint i;
	xmms_eq_priv_t *priv = xmms_effect_plugin_data_get (effect);

	g_return_if_fail (priv);

	while (len) {
		gdouble r,l,tmp;
		gint16 *samples = (gint16 *)buf;

		r = ((gdouble)samples[0]) / ((1<<15) - 1);
		l = ((gdouble)samples[1]) / ((1<<15) - 1);

		for (i=0; i<XMMS_EQ_BANDS; i++) {
			tmp = (priv->filters[i].b[0] * r) + 
				(priv->filters[i].b[1] * priv->filters[i].rinput[0]) +
				(priv->filters[i].b[2] * priv->filters[i].rinput[1]) -
				(priv->filters[i].a[1] * priv->filters[i].routput[0])-
				(priv->filters[i].a[2] * priv->filters[i].routput[1]);
			priv->filters[i].routput[1] = priv->filters[i].routput[0];
			priv->filters[i].routput[0] = tmp;
			priv->filters[i].rinput[1] = priv->filters[i].rinput[0];
			priv->filters[i].rinput[0] = r;
			r = tmp;

			tmp = (priv->filters[i].b[0] * l) + 
				(priv->filters[i].b[1] * priv->filters[i].linput[0]) +
				(priv->filters[i].b[2] * priv->filters[i].linput[1]) -
				(priv->filters[i].a[1] * priv->filters[i].loutput[0])-
				(priv->filters[i].a[2] * priv->filters[i].loutput[1]);
			priv->filters[i].loutput[1] = priv->filters[i].loutput[0];
			priv->filters[i].loutput[0] = tmp;
			priv->filters[i].linput[1] = priv->filters[i].linput[0];
			priv->filters[i].linput[0] = l;
			l = tmp;
			
		}

		r = CLAMP (r, -1.0, 1.0);
		l = CLAMP (l, -1.0, 1.0);

		samples[0] = r * ((1<<15) - 1);
		samples[1] = l * ((1<<15) - 1);
		
		len -= 4;
		buf += 4;
	}
}
