/** @file 
 * This file controls XMMS2 mainloop.
 */

#include <glib.h>

#include "plugin.h"
#include "transport.h"
#include "decoder.h"
#include "config.h"
#include "config_xmms.h"
#include "playlist.h"
#include "unixsignal.h"
#include "util.h"
#include "medialib.h"


#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

static xmms_playlist_t *playlist;
static xmms_output_t *output;
static xmms_decoder_t *m_decoder;
static GMainLoop *mainloop;



void play_next (void);

void
eos_reached (xmms_object_t *object, gconstpointer data, gpointer userdata)
{
	XMMS_DBG ("eos_reached");

	XMMS_DBG ("closing transport");
	xmms_transport_close (xmms_decoder_transport_get (m_decoder));
	XMMS_DBG ("destroying decoder");
	xmms_decoder_destroy (m_decoder);
	m_decoder = NULL;
	XMMS_DBG ("playing next");
	play_next ();
	XMMS_DBG ("done");
}

void
mediainfo_changed (xmms_object_t *object, gconstpointer data, gpointer userdata)
{
	xmms_playlist_entry_t *entry;
	xmms_decoder_t *decoder = (xmms_decoder_t *)data;

	entry = xmms_playlist_entry_new (NULL);
	
	xmms_decoder_get_mediainfo (decoder, entry);

	xmms_playlist_entry_print (entry);

	xmms_playlist_entry_free (entry);

}

void
play_next (void)
{
	xmms_transport_t *transport;
	xmms_decoder_t *decoder;
	xmms_playlist_entry_t *entry;
	const gchar *mime;

	g_return_if_fail (m_decoder == NULL);
	
	entry = xmms_playlist_pop (playlist);
	if (!entry)
		exit (1);

	XMMS_DBG ("Playing %s", xmms_playlist_entry_get_uri (entry));
	
	transport = xmms_transport_open (xmms_playlist_entry_get_uri (entry));

	if (!transport) {
		play_next ();
		return;
	}
	
	mime = xmms_transport_mime_type_get (transport);
	if (mime) {
		XMMS_DBG ("mime-type: %s", mime);
		decoder = xmms_decoder_new (mime);
		if (!decoder) {
			xmms_transport_close (transport);
			xmms_playlist_entry_free (entry);
			play_next ();
			return;
		}
	} else {
		xmms_transport_close (transport);
		xmms_playlist_entry_free (entry);
		play_next (); /* FIXME */
		return;
	}

	xmms_object_connect (XMMS_OBJECT (decoder), "mediainfo-changed", mediainfo_changed, NULL);

	XMMS_DBG ("starting threads..");
	xmms_transport_start (transport);
	XMMS_DBG ("transport started");
	xmms_decoder_start (decoder, transport, output);
	XMMS_DBG ("output started");

	m_decoder = decoder;

	xmms_playlist_entry_free (entry);
	
}

#define MAX_CONFIGFILE_LEN 255

xmms_config_data_t *
parse_config ()
{
	xmms_config_data_t *config;
	gchar filename[MAX_CONFIGFILE_LEN];
	gchar configdir[MAX_CONFIGFILE_LEN];

	g_snprintf (filename, MAX_CONFIGFILE_LEN, "%s/.xmms2/xmms2.conf", g_get_home_dir ());
	g_snprintf (configdir, MAX_CONFIGFILE_LEN, "%s/.xmms2/", g_get_home_dir ());

	if (g_file_test (filename, G_FILE_TEST_EXISTS)) {
		config = xmms_config_init (filename);
		if (!config) {
			XMMS_DBG ("XMMS was unable to parse configfile %s", filename);
			exit (EXIT_FAILURE);
		}
		return config;
	} else {
		if (g_file_test (XMMS_CONFIG_SYSTEMWIDE, G_FILE_TEST_EXISTS)) {
			config = xmms_config_init (XMMS_CONFIG_SYSTEMWIDE);
			if (!config) {
				XMMS_DBG ("XMMS was unable to parse configfile %s", filename);
				exit (EXIT_FAILURE);
			}

			if (!g_file_test (configdir, G_FILE_TEST_IS_DIR)) {
				mkdir (configdir, 0755);
			}

			if (!xmms_config_save_to_file (config, filename)) {
				XMMS_DBG ("Could't write file %s!", filename);
				exit (EXIT_FAILURE);
			}

			return config;
		} else {
			XMMS_DBG ("XMMS was unable to find systemwide configfile %s", XMMS_CONFIG_SYSTEMWIDE);
			exit (EXIT_FAILURE);
		}
	}
	return NULL;
}


int
main (int argc, char **argv)
{

	xmms_plugin_t *o_plugin;
	xmms_config_data_t *config;
	int opt;
	int verbose = 0;
	sigset_t signals;
	gchar *outname = NULL;

	memset (&signals, 0, sizeof (sigset_t));
        sigaddset (&signals, SIGHUP);
	sigaddset (&signals, SIGTERM);
	sigaddset (&signals, SIGINT);
	sigaddset (&signals, SIGSEGV);
	pthread_sigmask (SIG_BLOCK, &signals, NULL);

	
	if (argc < 2)
		exit (1);

	while (42) {
		opt = getopt (argc, argv, "vVo:");

		if (opt == -1)
			break;

		switch (opt) {
			case 'v':
				verbose++;
				break;

			case 'V':
				printf ("XMMS version %s\n", VERSION);
				exit (0);
				break;

			case 'o':
				outname = g_strdup (optarg);
				break;
				
		}
	}

	g_thread_init (NULL);
	if (!xmms_plugin_init ())
		return 1;

	playlist = xmms_playlist_init ();

	if (optind) {
		while (argv[optind]) {
			gchar nuri[XMMS_MAX_URI_LEN];
			if (!strchr (argv[optind], ':')) {
				XMMS_DBG ("No protocol, assuming file");
				if (argv[optind][0] == '/') {
					g_snprintf (nuri, XMMS_MAX_URI_LEN, "file://%s", argv[optind]);
				} else {
					gchar *cwd = g_get_current_dir ();
					g_snprintf (nuri, XMMS_MAX_URI_LEN, "file://%s/%s", cwd, argv[optind]);
					g_free (cwd);
				}
			} else {
				g_snprintf (nuri, XMMS_MAX_URI_LEN, "%s", argv[optind]);
			}
				
			XMMS_DBG ("Adding uri %s to playlist", nuri);
			xmms_playlist_add (playlist, xmms_playlist_entry_new (nuri), XMMS_PLAYLIST_APPEND);
			optind++;
		}
	}

	XMMS_DBG ("Playlist contains %d entries", xmms_playlist_entries (playlist));


	config = parse_config ();
	outname = xmms_config_value_as_string (xmms_config_value_lookup (config->core, "outputplugin"));
	XMMS_DBG ("output = %s", outname);

	if (!outname)
		outname = "oss";

	o_plugin = xmms_output_find_plugin (outname);
	g_return_val_if_fail (o_plugin, -1);
	output = xmms_output_open (o_plugin, config);
	g_return_val_if_fail (output, -1);

	xmms_object_connect (XMMS_OBJECT (output), "eos-reached", eos_reached, NULL);

	{
		xmms_plugin_t *a;
		xmms_medialib_t *medialib;

		a = xmms_medialib_find_plugin ("sqlite");
		if (a) {
			xmms_playlist_entry_t *find;

			find = xmms_playlist_entry_new (NULL);

			medialib = xmms_medialib_init (a);

			xmms_playlist_entry_set_prop (find, XMMS_ENTRY_PROPERTY_ARTIST, "*AP*");
			xmms_playlist_entry_set_prop (find, XMMS_ENTRY_PROPERTY_BITRATE, "128");
			xmms_medialib_search (medialib, find);

			xmms_playlist_entry_free (find);
		}

		
	}

	xmms_output_start (output);

	play_next ();

	xmms_signal_init (XMMS_OBJECT (output));

	mainloop = g_main_loop_new (NULL, FALSE);

        g_main_loop_run (mainloop);

	return 0;
}
