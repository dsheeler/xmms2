/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003	Peter Alm, Tobias Rundstr�m, Anders Gustafsson
 * 
 *  PLUGINS ARE NOT CONSIDERED TO BE DERIVED WORK !!!
 * 
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *                   
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */




/** @file 
 * Control transport plugins.
 *
 * This file is responsible for the transportlayer.
 */

#include "xmmspriv/xmms_transport.h"
#include "xmmspriv/xmms_ringbuf.h"
#include "xmmspriv/xmms_plugin.h"
#include "xmmspriv/xmms_medialib.h"
#include "xmms/xmms_transportplugin.h"
#include "xmms/xmms_object.h"
#include "xmms/xmms_log.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>

#include <sys/stat.h>

/*
 * Static function prototypes
 */

static void xmms_transport_destroy (xmms_object_t *object);
static xmms_plugin_t *xmms_transport_plugin_find (const gchar *url);
static gpointer xmms_transport_thread (gpointer data);
static gint xmms_transport_read_direct (xmms_transport_t *transport, gchar *buffer, guint len, xmms_error_t *error);

/*
 * Type definitions
 */

/** 
 * @defgroup Transport Transport
 * @ingroup XMMSServer
 * @brief Responsible to read encoded data from source.
 *
 * The transport is responsible for reading encoded data from 
 * a source. The data will be put in ringbuffer that the decoder 
 * reads from.
 *
 * @{
 */

/** this is the main transport struct. */
struct xmms_transport_St {
	/** Object for emiting signals */
	xmms_object_t object;
	xmms_plugin_t *plugin; /**< The plugin used as media. */

	/** 
	 * The entry that are transported.
	 * The url will be extracted from this
	 * upon open 
	 */
	xmms_medialib_entry_t entry;

	GMutex *mutex;
	GCond *cond;
	GThread *thread;

	/** This is true if we are currently buffering. */
	gboolean running;

	/**
	 * Put the source data in this buffer
	 */
	xmms_ringbuf_t *buffer;

	/** Private plugin data */
	gpointer plugin_data;

	
	/**
	 * in order to avoid a lot of buffer kills when
	 * opening a file (many decoders need to seek a lot
	 * when opening a file). We don't start buffering until
	 * we read from the buffer twice in a row. If we seek
	 * we reset the numread to 0.
	 */
	gint numread; 	
	gboolean want_buffering;
	gboolean is_buffering;

	guint64 seek_to;

	/** Number of buffer underruns */
	guint32 buffer_underruns;

	/** Error status for when we're buffering */
	xmms_error_t status;

	/** Current position in the stream. Only incremented on reads, not
	 * on peek calls.
	 */
	guint64 current_position; 	

	/** Used for linereading */
	struct {
		gchar buf[XMMS_TRANSPORT_MAX_LINE_SIZE];
		gchar *bufend;
	} lr;
};

/** @} */

/** 
 * @defgroup TransportPlugin TransportPlugin
 * @ingroup XMMSPlugin
 * @{
 *
 * These functions can be used from a transport plugin.
 */

/**
 * Get a transport's private data.
 *
 * @returns Pointer to private data.
 */
gpointer
xmms_transport_private_data_get (xmms_transport_t *transport)
{
	gpointer ret;
	g_return_val_if_fail (transport, NULL);

	ret = transport->plugin_data;

	return ret;
}

/**
 * Set transport private data
 *
 * @param transport the transport to store the pointer in.
 * @param data pointer to private data.
 */
void
xmms_transport_private_data_set (xmms_transport_t *transport, gpointer data)
{
	transport->plugin_data = data;
}

/** 
 * Get the current URL from the transport.
 */
const gchar *
xmms_transport_url_get (const xmms_transport_t *const transport)
{
	const gchar *ret;
	xmms_medialib_session_t *session;

	g_return_val_if_fail (transport, NULL);

	session = xmms_medialib_begin ();

	ret =  xmms_medialib_entry_property_get_str (session, transport->entry,
	                                             XMMS_MEDIALIB_ENTRY_PROPERTY_URL);

	xmms_medialib_end (session);

	return ret;
}

/** 
 * Get the current #xmms_medialib_entry_t from the transport.
 */
xmms_medialib_entry_t
xmms_transport_medialib_entry_get (const xmms_transport_t *const transport)
{
	g_return_val_if_fail (transport, 0);
	return transport->entry;
}

/** @} */

/** 
 * @addtogroup Transport Transport
 *
 * @{
 */

/**
 * Return the #xmms_plugin_t for this transport.
 */

xmms_plugin_t *
xmms_transport_plugin_get (const xmms_transport_t *transport)
{
	g_return_val_if_fail (transport, NULL);

	return transport->plugin;
}

/**
 * Wether this transport is a local one.
 * 
 * A local transport is file / socket / fd
 * Remote transports are http / ftp.
 * This is decided by the plugin with the property 
 * XMMS_PLUGIN_PROPERTY_LOCAL
 *
 * @param transport the transport structure.
 * @returns TRUE if this transport plugin has XMMS_PLUGIN_PROPERTY_LOCAL
 */
gboolean
xmms_transport_islocal (xmms_transport_t *transport)
{
	g_return_val_if_fail (transport, FALSE);

	return xmms_plugin_properties_check (transport->plugin, XMMS_PLUGIN_PROPERTY_LOCAL);
}

/**
 * This method can be called to check if the current plugin supports
 * seeking. It will check that the plugin implements
 * XMMS_PLUGIN_METHOD_SEEK.
 *
 * @returns a gboolean wheter this plugin can do seeking or not.
 */
gboolean
xmms_transport_can_seek (xmms_transport_t *transport)
{
	g_return_val_if_fail (transport, FALSE);

	return !!xmms_plugin_method_get (transport->plugin,
	                                 XMMS_PLUGIN_METHOD_SEEK);
}

gboolean
xmms_transport_isstream (xmms_transport_t *transport)
{
	g_return_val_if_fail (transport, FALSE);

	return xmms_plugin_properties_check (transport->plugin,
	                                     XMMS_PLUGIN_PROPERTY_STREAM);
}

/**
 * Initialize a new #xmms_transport_t structure. This structure has to
 * be dereffed by #xmms_object_unref to be freed. 
 *
 * To be able to read form this transport you'll have to call
 * #xmms_transport_open after you created a structure with
 * this function.
 *
 * @returns A newly allocated #xmms_transport_t
 */
xmms_transport_t *
xmms_transport_new ()
{
	xmms_transport_t *transport;
	xmms_config_property_t *val;

	val = xmms_config_lookup ("transport.buffersize");

	transport = xmms_object_new (xmms_transport_t, xmms_transport_destroy);
	transport->mutex = g_mutex_new ();
	transport->cond = g_cond_new ();
	transport->buffer = xmms_ringbuf_new (xmms_config_property_get_int (val));
	transport->seek_to = -1;
	transport->lr.bufend = &transport->lr.buf[0];
	
	return transport;
}

/**
 * Determines the size of the ring buffer used by the transport.
 *
 * @returns the size of the ring buffer used by the transport.
 */
guint
xmms_transport_buffersize (xmms_transport_t *transport)
{
	guint ret;

	g_return_val_if_fail (transport, FALSE);

	g_mutex_lock (transport->mutex);
	ret = xmms_ringbuf_size (transport->buffer);
	g_mutex_unlock (transport->mutex);

	return ret;
}

/**
 * Make the transport ready for buffering and reading.
 * It will take the entry URL and pass it to all transport
 * plugins and let them decide if they can handle this URL
 * or not. When the it finds a plugin that claims to handle
 * it, the plugins open method will be called.
 *
 * @returns TRUE if a suitable plugin is found and the plugins
 * open method is successfull, otherwise FALSE.
 */

gboolean
xmms_transport_open (xmms_transport_t *transport, xmms_medialib_entry_t entry)
{
	xmms_transport_open_method_t init_method;
	xmms_transport_lmod_method_t lmod_method;
	xmms_medialib_session_t *session;
	xmms_plugin_t *plugin;
	gboolean res = FALSE;
	gchar *url = NULL;

	g_return_val_if_fail (entry, FALSE);
	g_return_val_if_fail (transport, FALSE);

	session = xmms_medialib_begin ();

	url = xmms_medialib_entry_property_get_str (session, entry,
	                                            XMMS_MEDIALIB_ENTRY_PROPERTY_URL);

	xmms_log_info ("Opening url '%s'", url);

	if (!xmms_medialib_decode_url (url)) {
		xmms_log_error ("Illegal encoding in url");
		goto out;
	}

	plugin = xmms_transport_plugin_find (url);
	if (!plugin)
		goto out;

	xmms_log_info ("Using plugin: %s", xmms_plugin_name_get (plugin));

	transport->plugin = plugin;
	transport->entry = entry;

	init_method = xmms_plugin_method_get (plugin, XMMS_PLUGIN_METHOD_INIT);
	g_assert (init_method);

	if (!init_method (transport, url))
		goto out;

	lmod_method = xmms_plugin_method_get (plugin, XMMS_PLUGIN_METHOD_LMOD);
	if (lmod_method) {
		guint lmod;
		lmod = lmod_method (transport);
		xmms_medialib_entry_property_set_int (session, transport->entry,
		                                      XMMS_MEDIALIB_ENTRY_PROPERTY_LMOD, lmod);
	}

	res = TRUE;
 out:
	xmms_medialib_end (session);
	if (url)
		g_free (url);
	return res;
}

/**
 * Tell the transport to start buffer, this is normaly done
 * after you read twice from the buffer
 */
void
xmms_transport_buffering_start (xmms_transport_t *transport)
{
	g_mutex_lock (transport->mutex);
	transport->want_buffering = TRUE;
	g_cond_signal (transport->cond);
	g_mutex_unlock (transport->mutex);
}

/**
 * Read #len bytes into buffer.
 *
 * This function reads from the transport thread buffer, if you want to
 * read more then currently are buffered, it will wait for you. Does not
 * guarantee that all bytes are read, may return less bytes.
 *
 * @param transport transport to read from.
 * @param buffer where to store read data.
 * @param len number of bytes to read.
 * @param error a #xmms_error_t structure that can hold errors.
 * @returns number of bytes actually read, or -1 on error.
 *
 */
gint
xmms_transport_read (xmms_transport_t *transport, gchar *buffer, guint len, xmms_error_t *error)
{
	gint ret;

	g_return_val_if_fail (transport, -1);
	g_return_val_if_fail (buffer, -1);
	g_return_val_if_fail (len > 0, -1);
	g_return_val_if_fail (error, -1);

	g_mutex_lock (transport->mutex);
	if (!transport->want_buffering) { /* Unbuffered read */
		ret = xmms_transport_read_direct (transport, buffer, len, error);
		if (transport->running && transport->numread++ > 1) {
			XMMS_DBG ("Let's start buffering");
			transport->want_buffering = TRUE;
			g_cond_signal (transport->cond);
		}
	} else { /* Buffered read */
		len = CLAMP (len, 1, xmms_ringbuf_size (transport->buffer));

		ret = xmms_ringbuf_read_wait (transport->buffer, buffer, len, transport->mutex);
		
		if (ret < len) {
			transport->buffer_underruns ++;
		}

		if (ret == 0 && xmms_ringbuf_iseos (transport->buffer)) {
			xmms_error_set (error,
			                transport->status.code,
			                transport->status.message);
			ret = transport->status.code == XMMS_ERROR_EOS ? 0 : -1;
		}
	}

	if (ret > 0)
		transport->current_position += ret;

	g_mutex_unlock (transport->mutex);

	return ret;
}

gint
xmms_transport_peek (xmms_transport_t *transport, gchar *buffer,
                     guint len, xmms_error_t *error)
{
	gint ret;

	g_return_val_if_fail (transport, -1);
	g_return_val_if_fail (buffer, -1);
	g_return_val_if_fail (len > 0, -1);
	g_return_val_if_fail (error, -1);

	g_mutex_lock (transport->mutex);

	if (len > xmms_ringbuf_size (transport->buffer)) {
		xmms_error_set (error, XMMS_ERROR_INVAL,
		                "transport buffer too small");
		g_mutex_unlock (transport->mutex);

		return -1;
	}

	if (xmms_ringbuf_iseos (transport->buffer)) {
		xmms_error_set (error, transport->status.code,
		                transport->status.message);
		ret = (transport->status.code == XMMS_ERROR_EOS) ? 0 : -1;

		g_mutex_unlock (transport->mutex);

		return ret;
	}

	/* make sure we are buffering */
	if (!transport->want_buffering) {
		transport->want_buffering = TRUE;
		g_cond_signal (transport->cond);
	}

	ret = xmms_ringbuf_peek_wait (transport->buffer, buffer, len,
	                              transport->mutex);

	if (ret < len) {
		transport->buffer_underruns++;
	}

	g_mutex_unlock (transport->mutex);

	return ret;
}

/**
 * Read line.
 *
 * Reads one line from the transport. The length of the line can be up
 * to XMMS_TRANSPORT_MAX_LINE_SIZE bytes. Should not be mixed with
 * calls to xmms_transport_read.
 *
 * @param transport transport to read from.
 * @param line buffer to store the line in,
 *             must be atleast XMMS_TRANSPORT_MAX_LINE_SIZE bytes.
 * @param error a #xmms_error_t structure that can hold errors.
 * @returns the line or NULL on EOF or error.
 */
gchar *
xmms_transport_read_line (xmms_transport_t *transport, gchar *line, xmms_error_t *err)
{
	gchar *p;
	
	g_return_val_if_fail (transport, NULL);

	p = strchr (transport->lr.buf, '\n');
	
	if (!p) {
		gint l, r;

		l = (XMMS_TRANSPORT_MAX_LINE_SIZE - 1) - (transport->lr.bufend - transport->lr.buf);
		if (l) {
			r = xmms_transport_read (transport, transport->lr.bufend, l, err);
			if (r < 0) {
				return NULL;
			}
			transport->lr.bufend += r;
		}
		if (transport->lr.bufend == transport->lr.buf)
			return NULL;

		*(transport->lr.bufend) = '\0';

		p = strchr (transport->lr.buf, '\n');
		if (!p) {
			p = transport->lr.bufend;
		}
	}
	if (p > transport->lr.buf && *(p-1) == '\r') {
		*(p-1) = '\0';
	} else {
		*p = '\0';
	}

	strcpy (line, transport->lr.buf);
	memmove (transport->lr.buf, p + 1, transport->lr.bufend - p);
	transport->lr.bufend -= (p - transport->lr.buf) + 1;
	*transport->lr.bufend = '\0';
	return line;

}

/**
 * 
 * Seek to a specific offset in a transport. Emulates the behaviour of
 * lseek. Buffering is disabled after a seek (automatically enabled
 * after two reads).
 *
 * The whence parameter should be one of:
 * @li @c XMMS_TRANSPORT_SEEK_SET Sets position to offset from start of file
 * @li @c XMMS_TRANSPORT_SEEK_END Sets position to offset from end of file
 * @li @c XMMS_TRANSPORT_SEEK_CUR Sets position to offset from current position
 *
 * @param transport the transport to modify
 * @param offset offset in bytes
 * @param whence se above
 * @returns new position, or -1 on error
 * 
 */
gint
xmms_transport_seek (xmms_transport_t *transport, gint offset, gint whence)
{
	guint64 seek_to, size;

	g_return_val_if_fail (transport, -1);

	if (!xmms_transport_can_seek (transport))
		return -1;

	g_mutex_lock (transport->mutex);

	switch (whence) {
	case XMMS_TRANSPORT_SEEK_CUR:
		seek_to = transport->current_position + offset;
		break;
	case XMMS_TRANSPORT_SEEK_END:
		size = xmms_transport_size (transport);
		if (size == -1) {
			g_mutex_unlock (transport->mutex);
			return -1;
		}
		seek_to = size + offset;
		break;
	case XMMS_TRANSPORT_SEEK_SET:
		seek_to = offset;
		break;
	}

	transport->seek_to = seek_to;
	transport->current_position = seek_to;

	/* stop buffering kthx */
	transport->want_buffering = FALSE;
	while (transport->is_buffering) {
		/* abort pending writes */
		xmms_ringbuf_set_eos (transport->buffer, TRUE);
		g_cond_wait (transport->cond, transport->mutex);
	}
	transport->numread = 0;
	xmms_ringbuf_set_eos (transport->buffer, FALSE);

	g_mutex_unlock (transport->mutex);

	return seek_to;
}

/**
 * Obtain the current value of the stream position indicator for transport
 * 
 * @returns current position in bytes.
 */
guint64
xmms_transport_tell (xmms_transport_t *transport)
{
	guint64 ret;

	g_return_val_if_fail (transport, -1); 

	g_mutex_lock (transport->mutex);
	ret = transport->current_position;
	g_mutex_unlock (transport->mutex);

	return ret;
}

/**
 * Query the transport to check wheter it's EOFed or
 * not.
 * @returns TRUE if the stream is EOFed.
 */
gboolean
xmms_transport_iseos (xmms_transport_t *transport)
{
	gboolean ret = FALSE;

	g_return_val_if_fail (transport, FALSE);

	g_mutex_lock (transport->mutex);

	if (transport->want_buffering) {
		ret = xmms_ringbuf_iseos (transport->buffer);
	} else if (xmms_error_iserror (&transport->status)) {
		ret = TRUE;
	}

	g_mutex_unlock (transport->mutex);
	return ret;
}

/**
 * Get the total size in bytes of the transports source.
 * @returns size of the media, or -1 if it can't be determined.
 */
guint64
xmms_transport_size (xmms_transport_t *transport)
{
	xmms_transport_size_method_t size;
	g_return_val_if_fail (transport, -1);

	size = xmms_plugin_method_get (transport->plugin, XMMS_PLUGIN_METHOD_SIZE);
	g_return_val_if_fail (size, -1);

	return size (transport);
}

/**
 * Get the plugin that was used to instantiate this transport.
 */
xmms_plugin_t *
xmms_transport_get_plugin (const xmms_transport_t *transport)
{
	g_return_val_if_fail (transport, NULL);

	return transport->plugin;
}

/** @} */

/**
  * Start the transport thread.
  * This should be called to make the transport start buffer.
  *
  * @internal
  */

void
xmms_transport_start (xmms_transport_t *transport)
{
	g_return_if_fail (transport);

	transport->running = TRUE;
	xmms_object_ref (transport);
	transport->thread = g_thread_create (xmms_transport_thread, transport, TRUE, NULL); 
}


/**
  * Tell the transport to stop buffering.
  * You still have to deref the object to free memory.
  *
  * @internal
  */

void
xmms_transport_stop (xmms_transport_t *transport)
{
	g_return_if_fail (transport);

	if (transport->thread) {
		g_mutex_lock (transport->mutex);
		transport->running = FALSE;
		xmms_ringbuf_set_eos (transport->buffer, TRUE);
		g_cond_signal (transport->cond);
		g_mutex_unlock (transport->mutex);
		g_thread_join (transport->thread);
		transport->thread = NULL;
	}
}

gboolean
xmms_transport_plugin_verify (xmms_plugin_t *plugin)
{
	g_return_val_if_fail (plugin, FALSE);

	return xmms_plugin_has_methods (plugin,
	                                XMMS_PLUGIN_METHOD_INIT,
	                                XMMS_PLUGIN_METHOD_READ,
	                                NULL);
}

/*
 * Static functions
 */

/**
 * Destroy function. Called when all references to the 
 * #xmms_transport_t is gone. Will free up memory and 
 * close sockets.
 */
static void
xmms_transport_destroy (xmms_object_t *object)
{
	xmms_transport_t *transport = (xmms_transport_t *)object;
	xmms_transport_close_method_t close_method;

	close_method = xmms_plugin_method_get (transport->plugin, XMMS_PLUGIN_METHOD_CLOSE);
	
	if (close_method)
		close_method (transport);

	xmms_object_unref (transport->plugin);

	xmms_ringbuf_destroy (transport->buffer);
	g_cond_free (transport->cond);
	g_mutex_free (transport->mutex);
}

/**
 * Find a transportplugin for this URL.
 *
 * @internal
 */
static xmms_plugin_t *
xmms_transport_plugin_find (const gchar *url)
{
	GList *list, *node;
	xmms_plugin_t *plugin = NULL;
	xmms_transport_can_handle_method_t can_handle;

	g_return_val_if_fail (url, NULL);

	list = xmms_plugin_list_get (XMMS_PLUGIN_TYPE_TRANSPORT);
	
	for (node = list; node; node = g_list_next (node)) {
		plugin = node->data;
		XMMS_DBG ("Trying plugin: %s", xmms_plugin_shortname_get (plugin));
		can_handle = xmms_plugin_method_get (plugin, XMMS_PLUGIN_METHOD_CAN_HANDLE);
		
		if (!can_handle)
			continue;

		if (can_handle (url)) {
			xmms_object_ref (plugin);
			break;
		}
	}
	if (!node)
		plugin = NULL;

	if (list)
		xmms_plugin_list_destroy (list);

	return plugin;
}

static gpointer
xmms_transport_thread (gpointer data)
{
	xmms_transport_t *transport = data;
	gchar buffer[4096];
	xmms_error_t error;
	gint ret;

	g_return_val_if_fail (transport, NULL);
	
	xmms_error_reset (&error);

	g_mutex_lock (transport->mutex);
	while (transport->running) {

		if (!transport->want_buffering) {
			xmms_ringbuf_clear (transport->buffer);
			transport->is_buffering = FALSE;
			g_cond_signal (transport->cond);
			g_cond_wait (transport->cond, transport->mutex);
			continue;
		} 

		transport->is_buffering = TRUE;

		ret = xmms_transport_read_direct (transport, buffer,
		                                  sizeof(buffer), &error);

		if (!transport->running)
			break;

		/* if our buffering service isn't wanted anymore,
		   throw the read stuff away */
		if (!transport->want_buffering)
			continue;

		if (ret > 0) {
			xmms_ringbuf_write_wait (transport->buffer, buffer, ret, transport->mutex);
		} else {
			transport->is_buffering = FALSE;
			xmms_ringbuf_set_eos (transport->buffer, TRUE);
			xmms_error_set (&transport->status, error.code, error.message);
			g_cond_wait (transport->cond, transport->mutex);
		}
	}
	g_mutex_unlock (transport->mutex);

	XMMS_DBG ("xmms_transport_thread: cleaning up");
	
	xmms_object_unref (transport);
	
	return NULL;
}

static void
xmms_transport_seek_exec (xmms_transport_t *transport)
{
	xmms_transport_seek_method_t seek_method;
	gint ret;

	seek_method = xmms_plugin_method_get (transport->plugin,
	                                      XMMS_PLUGIN_METHOD_SEEK);

	/* this function must not be called if xmms_transport_can_seek()
	 * returns FALSE
	 */
	g_assert (seek_method);
	g_assert (transport->seek_to != -1);

	ret = seek_method (transport, transport->seek_to,
	                   XMMS_TRANSPORT_SEEK_SET);
	if (ret != transport->seek_to) {
		XMMS_DBG ("Seeking failed, hell will break loose!");
	}

	transport->seek_to = -1;
}

static gint
xmms_transport_read_direct (xmms_transport_t *transport, gchar *buffer, guint len, xmms_error_t *error)
{
	xmms_transport_read_method_t read_method;
	gint ret;

	/* has a seek been queued? */
	if (transport->seek_to != -1) {
		xmms_transport_seek_exec (transport);
	}

	read_method = xmms_plugin_method_get (transport->plugin, XMMS_PLUGIN_METHOD_READ);
	g_assert (read_method);
	
	ret = read_method (transport, buffer, len, error);

	if (ret == -1)
		xmms_error_set (&transport->status, error->code, error->message);

	return ret;
}
