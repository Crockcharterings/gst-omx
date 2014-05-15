/*
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
 * Copyright (C) 2013, Collabora Ltd.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* FIXME 0.11: suppress warnings for deprecated API such as GStaticRecMutex
 * with newer GLib versions (>= 2.31.0) */
#define GLIB_DISABLE_DEPRECATION_WARNINGS

#include <gst/gst.h>
#include <string.h>

#include "gstomx.h"
#include "gstomxmpeg4videodec.h"
#include "gstomxh264dec.h"
#include "gstomxh263dec.h"
#include "gstomxwmvdec.h"
#include "gstomxmpeg4videoenc.h"
#include "gstomxh264enc.h"
#include "gstomxh263enc.h"
#include "gstomxaacenc.h"
#include "gstomxmpeg2videodec.h"
#include "gstomxvc1videodec.h"
#ifdef HAVE_HYBRIS
#include "hybris.h"
#include <dlfcn.h>
#endif
#include "HardwareAPI.h"

GST_DEBUG_CATEGORY (gstomx_debug);
#define GST_CAT_DEFAULT gstomx_debug

G_LOCK_DEFINE_STATIC (core_handles);
static GHashTable *core_handles;

GstOMXCore *
gst_omx_core_acquire (const gchar * filename)
{
  GstOMXCore *core;

  G_LOCK (core_handles);
  if (!core_handles)
    core_handles =
        g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  core = g_hash_table_lookup (core_handles, filename);
  if (!core) {
    core = g_slice_new0 (GstOMXCore);
    core->lock = g_mutex_new ();
    core->user_count = 0;
    g_hash_table_insert (core_handles, g_strdup (filename), core);

    core->module = g_module_open (filename, G_MODULE_BIND_LAZY);
    if (!core->module)
      goto load_failed;

    if (!g_module_symbol (core->module, "OMX_Init", (gpointer *) & core->init))
      goto symbol_error;
    if (!g_module_symbol (core->module, "OMX_Deinit",
            (gpointer *) & core->deinit))
      goto symbol_error;
    if (!g_module_symbol (core->module, "OMX_GetHandle",
            (gpointer *) & core->get_handle))
      goto symbol_error;
    if (!g_module_symbol (core->module, "OMX_FreeHandle",
            (gpointer *) & core->free_handle))
      goto symbol_error;

    GST_DEBUG ("Successfully loaded core '%s'", filename);
  }

  g_mutex_lock (core->lock);
  core->user_count++;
  if (core->user_count == 1) {
    OMX_ERRORTYPE err;

    err = core->init ();
    if (err != OMX_ErrorNone) {
      GST_ERROR ("Failed to initialize core '%s': 0x%08x", filename, err);
      g_mutex_unlock (core->lock);
      goto error;
    }

    GST_DEBUG ("Successfully initialized core '%s'", filename);
  }

  g_mutex_unlock (core->lock);
  G_UNLOCK (core_handles);

  return core;

load_failed:
  {
    GST_ERROR ("Failed to load module '%s': %s", filename, g_module_error ());
    goto error;
  }
symbol_error:
  {
    GST_ERROR ("Failed to locate required OpenMAX symbol in '%s': %s", filename,
        g_module_error ());
    g_module_close (core->module);
    core->module = NULL;
    goto error;
  }
error:
  {
    g_hash_table_remove (core_handles, filename);
    g_mutex_free (core->lock);
    g_slice_free (GstOMXCore, core);

    G_UNLOCK (core_handles);

    return NULL;
  }
}

GstOMXCore *
gst_omx_core_acquire_hybris (const gchar * filename)
{
  GstOMXCore *core;

  G_LOCK (core_handles);
  if (!core_handles)
    core_handles =
        g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  core = g_hash_table_lookup (core_handles, filename);
  if (!core) {
    core = g_slice_new0 (GstOMXCore);
    core->lock = g_mutex_new ();
    core->user_count = 0;
    g_hash_table_insert (core_handles, g_strdup (filename), core);
    core->module = NULL;
    core->hybris_module = android_dlopen (filename, RTLD_LAZY);
    if (!core->hybris_module)
      goto load_failed;

    core->init = android_dlsym (core->hybris_module, "OMX_Init");
    if (!core->init)
      goto symbol_error;
    core->deinit = android_dlsym (core->hybris_module, "OMX_Deinit");
    if (!core->deinit)
      goto symbol_error;
    core->get_handle = android_dlsym (core->hybris_module, "OMX_GetHandle");
    if (!core->get_handle)
      goto symbol_error;
    core->free_handle = android_dlsym (core->hybris_module, "OMX_FreeHandle");
    if (!core->free_handle)
      goto symbol_error;

    GST_DEBUG ("Successfully loaded core '%s'", filename);
  }

  g_mutex_lock (core->lock);
  core->user_count++;
  if (core->user_count == 1) {
    OMX_ERRORTYPE err;

    err = core->init ();
    if (err != OMX_ErrorNone) {
      GST_ERROR ("Failed to initialize core '%s': 0x%08x", filename, err);
      g_mutex_unlock (core->lock);
      goto error;
    }

    GST_DEBUG ("Successfully initialized core '%s'", filename);
  }

  g_mutex_unlock (core->lock);
  G_UNLOCK (core_handles);

  return core;

load_failed:
  {
    GST_ERROR ("Failed to load module '%s'", filename);
    goto error;
  }
symbol_error:
  {
    GST_ERROR ("Failed to locate required OpenMAX symbol in '%s'", filename);
    android_dlclose (core->hybris_module);
    core->module = NULL;
    core->hybris_module = NULL;
    goto error;
  }
error:
  {
    g_hash_table_remove (core_handles, filename);
    g_mutex_free (core->lock);
    g_slice_free (GstOMXCore, core);

    G_UNLOCK (core_handles);

    return NULL;
  }
}

void
gst_omx_core_release (GstOMXCore * core)
{
  g_return_if_fail (core != NULL);

  G_LOCK (core_handles);

  g_mutex_lock (core->lock);

  GST_DEBUG ("Releasing core %p", core);

  core->user_count--;
  if (core->user_count == 0) {
    GST_DEBUG ("Deinit core %p", core);
    core->deinit ();
  }

  g_mutex_unlock (core->lock);

  G_UNLOCK (core_handles);
}

/* NOTE: comp->messages_lock will be used */
static void
gst_omx_component_flush_messages (GstOMXComponent * comp)
{
  GstOMXMessage *msg;

  g_mutex_lock (comp->messages_lock);
  while ((msg = g_queue_pop_head (&comp->messages))) {
    g_slice_free (GstOMXMessage, msg);
  }
  g_mutex_unlock (comp->messages_lock);
}

/* NOTE: Call with comp->lock, comp->messages_lock will be used */
static void
gst_omx_component_handle_messages (GstOMXComponent * comp)
{
  GstOMXMessage *msg;

  g_mutex_lock (comp->messages_lock);

  while ((msg = g_queue_pop_head (&comp->messages))) {
    switch (msg->type) {
      case GST_OMX_MESSAGE_STATE_SET:{
        GST_DEBUG_OBJECT (comp->parent, "State change to %d finished",
            msg->content.state_set.state);
        comp->state = msg->content.state_set.state;
        if (comp->state == comp->pending_state)
          comp->pending_state = OMX_StateInvalid;
        break;
      }
      case GST_OMX_MESSAGE_FLUSH:{
        GstOMXPort *port = NULL;
        OMX_U32 index = msg->content.flush.port;

        port = gst_omx_component_get_port (comp, index);
        if (!port)
          break;

        GST_DEBUG_OBJECT (comp->parent, "Port %u flushed", port->index);

        if (port->flushing) {
          port->flushed = TRUE;
        } else {
          GST_ERROR_OBJECT (comp->parent, "Port %u was not flushing",
              port->index);
        }

        break;
      }
      case GST_OMX_MESSAGE_ERROR:{
        OMX_ERRORTYPE error = msg->content.error.error;

        if (error == OMX_ErrorNone)
          break;

        GST_ERROR_OBJECT (comp->parent, "Got error: %s (0x%08x)",
            gst_omx_error_to_string (error), error);

        /* We only set the first error ever from which
         * we can't recover anymore.
         */
        if (comp->last_error == OMX_ErrorNone)
          comp->last_error = error;
        g_cond_broadcast (comp->messages_cond);

        break;
      }
      case GST_OMX_MESSAGE_PORT_ENABLE:{
        GstOMXPort *port = NULL;
        OMX_U32 index = msg->content.port_enable.port;
        OMX_BOOL enable = msg->content.port_enable.enable;

        port = gst_omx_component_get_port (comp, index);
        if (!port)
          break;

        GST_DEBUG_OBJECT (comp->parent, "Port %u %s", port->index,
            (enable ? "enabled" : "disabled"));

        port->enabled_changed = TRUE;
        break;
      }
      case GST_OMX_MESSAGE_PORT_SETTINGS_CHANGED:{
        gint i, n;
        OMX_U32 index = msg->content.port_settings_changed.port;
        GList *outports = NULL, *l, *k;

        GST_DEBUG_OBJECT (comp->parent, "Settings changed (port %u)", index);

        /* FIXME: This probably can be done better */

        /* Now update the ports' states */
        n = (comp->ports ? comp->ports->len : 0);
        for (i = 0; i < n; i++) {
          GstOMXPort *port = g_ptr_array_index (comp->ports, i);

          if (index == OMX_ALL || index == port->index) {
            port->settings_cookie++;
            if (port->port_def.eDir == OMX_DirOutput)
              outports = g_list_prepend (outports, port);
          }
        }

        for (k = outports; k; k = k->next) {
          gboolean found = FALSE;

          for (l = comp->pending_reconfigure_outports; l; l = l->next) {
            if (l->data == k->data) {
              found = TRUE;
              break;
            }
          }

          if (!found)
            comp->pending_reconfigure_outports =
                g_list_prepend (comp->pending_reconfigure_outports, k->data);
        }

        if (comp->pending_reconfigure_outports)
          g_atomic_int_set (&comp->have_pending_reconfigure_outports, 1);

        g_list_free (outports);

        break;
      }
      case GST_OMX_MESSAGE_BUFFER_DONE:{
        GstOMXBuffer *buf = msg->content.buffer_done.buffer->pAppPrivate;
        GstOMXPort *port;
        GstOMXComponent *comp;

        port = buf->port;
        comp = port->comp;

        if (msg->content.buffer_done.empty) {
          /* Input buffer is empty again and can be used to contain new input */
          GST_DEBUG_OBJECT (comp->parent, "Port %u emptied buffer %p (%p)",
              port->index, buf, buf->omx_buf->pBuffer);

          /* XXX: Some OMX implementations don't reset nOffset
           * when the complete buffer is emptied but instead
           * only reset nFilledLen. We reset nOffset to 0
           * if nFilledLen == 0, which is safe to do because
           * the offset *must* be 0 if the buffer is not
           * filled at all.
           *
           * Seen in QCOM's OMX implementation.
           */
          if (buf->omx_buf->nFilledLen == 0)
            buf->omx_buf->nOffset = 0;

          /* Reset all flags, some implementations don't
           * reset them themselves and the flags are not
           * valid anymore after the buffer was consumed
           */
          buf->omx_buf->nFlags = 0;
        } else {
          /* Output buffer contains output now or
           * the port was flushed */
          GST_DEBUG_OBJECT (comp->parent, "Port %u filled buffer %p (%p)",
              port->index, buf, buf->omx_buf->pBuffer);
        }

        buf->used = FALSE;

        g_queue_push_tail (&port->pending_buffers, buf);

        break;
      }
      default:{
        g_assert_not_reached ();
        break;
      }
    }

    g_slice_free (GstOMXMessage, msg);
  }

  g_mutex_unlock (comp->messages_lock);
}

static OMX_ERRORTYPE
EventHandler (OMX_HANDLETYPE hComponent, OMX_PTR pAppData, OMX_EVENTTYPE eEvent,
    OMX_U32 nData1, OMX_U32 nData2, OMX_PTR pEventData)
{
  GstOMXComponent *comp = (GstOMXComponent *) pAppData;

  switch (eEvent) {
    case OMX_EventCmdComplete:
    {
      OMX_COMMANDTYPE cmd = (OMX_COMMANDTYPE) nData1;

      GST_DEBUG_OBJECT (comp->parent, "Command %d complete", cmd);

      switch (cmd) {
        case OMX_CommandStateSet:{
          GstOMXMessage *msg = g_slice_new (GstOMXMessage);

          msg->type = GST_OMX_MESSAGE_STATE_SET;
          msg->content.state_set.state = nData2;

          GST_DEBUG_OBJECT (comp->parent, "State change to %d finished",
              msg->content.state_set.state);

          g_mutex_lock (comp->messages_lock);
          g_queue_push_tail (&comp->messages, msg);
          g_cond_broadcast (comp->messages_cond);
          g_mutex_unlock (comp->messages_lock);
          break;
        }
        case OMX_CommandFlush:{
          GstOMXMessage *msg = g_slice_new (GstOMXMessage);

          msg->type = GST_OMX_MESSAGE_FLUSH;
          msg->content.flush.port = nData2;
          GST_DEBUG_OBJECT (comp->parent, "Port %u flushed",
              msg->content.flush.port);

          g_mutex_lock (comp->messages_lock);
          g_queue_push_tail (&comp->messages, msg);
          g_cond_broadcast (comp->messages_cond);
          g_mutex_unlock (comp->messages_lock);
          break;
        }
        case OMX_CommandPortEnable:
        case OMX_CommandPortDisable:{
          GstOMXMessage *msg = g_slice_new (GstOMXMessage);

          msg->type = GST_OMX_MESSAGE_PORT_ENABLE;
          msg->content.port_enable.port = nData2;
          msg->content.port_enable.enable = (cmd == OMX_CommandPortEnable);
          GST_DEBUG_OBJECT (comp->parent, "Port %u %s",
              msg->content.port_enable.port,
              (msg->content.port_enable.enable ? "enabled" : "disabled"));

          g_mutex_lock (comp->messages_lock);
          g_queue_push_tail (&comp->messages, msg);
          g_cond_broadcast (comp->messages_cond);
          g_mutex_unlock (comp->messages_lock);
          break;
        }
        default:
          break;
      }
      break;
    }
    case OMX_EventError:
    {
      GstOMXMessage *msg;

      /* Yes, this really happens... */
      if (nData1 == OMX_ErrorNone)
        break;

      msg = g_slice_new (GstOMXMessage);

      msg->type = GST_OMX_MESSAGE_ERROR;
      msg->content.error.error = nData1;
      GST_ERROR_OBJECT (comp->parent, "Got error: %s (0x%08x)",
          gst_omx_error_to_string (msg->content.error.error),
          msg->content.error.error);

      g_mutex_lock (comp->messages_lock);
      g_queue_push_tail (&comp->messages, msg);
      g_cond_broadcast (comp->messages_cond);
      g_mutex_unlock (comp->messages_lock);
      break;
    }
    case OMX_EventPortSettingsChanged:
    {
      GstOMXMessage *msg = g_slice_new (GstOMXMessage);
      OMX_U32 index;

      if (!(comp->hacks &
              GST_OMX_HACK_EVENT_PORT_SETTINGS_CHANGED_NDATA_PARAMETER_SWAP)) {
        index = nData1;
      } else {
        index = nData2;
      }


      if (index == 0
          && (comp->hacks &
              GST_OMX_HACK_EVENT_PORT_SETTINGS_CHANGED_PORT_0_TO_1))
        index = 1;


      msg->type = GST_OMX_MESSAGE_PORT_SETTINGS_CHANGED;
      msg->content.port_settings_changed.port = index;
      GST_DEBUG_OBJECT (comp->parent, "Settings changed (port index: %d)",
          msg->content.port_settings_changed.port);

      g_mutex_lock (comp->messages_lock);
      g_queue_push_tail (&comp->messages, msg);
      g_cond_broadcast (comp->messages_cond);
      g_mutex_unlock (comp->messages_lock);

      break;
    }
    case OMX_EventPortFormatDetected:
    case OMX_EventBufferFlag:
    default:
      break;
  }

  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
EmptyBufferDone (OMX_HANDLETYPE hComponent, OMX_PTR pAppData,
    OMX_BUFFERHEADERTYPE * pBuffer)
{
  GstOMXBuffer *buf;
  GstOMXComponent *comp;
  GstOMXMessage *msg;

  buf = pBuffer->pAppPrivate;
  if (!buf) {
    GST_ERROR ("Have unknown or deallocated buffer %p", pBuffer);
    return OMX_ErrorNone;
  }

  g_assert (buf->omx_buf == pBuffer);

  comp = buf->port->comp;

  msg = g_slice_new (GstOMXMessage);
  msg->type = GST_OMX_MESSAGE_BUFFER_DONE;
  msg->content.buffer_done.component = hComponent;
  msg->content.buffer_done.app_data = pAppData;
  msg->content.buffer_done.buffer = pBuffer;
  msg->content.buffer_done.empty = OMX_TRUE;

  GST_DEBUG_OBJECT (comp->parent, "Port %u emptied buffer %p (%p)",
      buf->port->index, buf, buf->omx_buf->pBuffer);

  g_mutex_lock (comp->messages_lock);
  g_queue_push_tail (&comp->messages, msg);
  g_cond_broadcast (comp->messages_cond);
  g_mutex_unlock (comp->messages_lock);

  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
FillBufferDone (OMX_HANDLETYPE hComponent, OMX_PTR pAppData,
    OMX_BUFFERHEADERTYPE * pBuffer)
{
  GstOMXBuffer *buf;
  GstOMXComponent *comp;
  GstOMXMessage *msg;

  buf = pBuffer->pAppPrivate;
  if (!buf) {
    GST_ERROR ("Have unknown or deallocated buffer %p", pBuffer);
    return OMX_ErrorNone;
  }

  g_assert (buf->omx_buf == pBuffer);

  comp = buf->port->comp;

  msg = g_slice_new (GstOMXMessage);
  msg->type = GST_OMX_MESSAGE_BUFFER_DONE;
  msg->content.buffer_done.component = hComponent;
  msg->content.buffer_done.app_data = pAppData;
  msg->content.buffer_done.buffer = pBuffer;
  msg->content.buffer_done.empty = OMX_FALSE;

  GST_DEBUG_OBJECT (comp->parent, "Port %u filled buffer %p (%p)",
      buf->port->index, buf, buf->omx_buf->pBuffer);

  g_mutex_lock (comp->messages_lock);
  g_queue_push_tail (&comp->messages, msg);
  g_cond_broadcast (comp->messages_cond);
  g_mutex_unlock (comp->messages_lock);

  return OMX_ErrorNone;
}

static OMX_CALLBACKTYPE callbacks =
    { EventHandler, EmptyBufferDone, FillBufferDone };

/* NOTE: Uses comp->lock and comp->messages_lock */
GstOMXComponent *
gst_omx_component_new (GstObject * parent, const gchar * core_name,
    const gchar * component_name, const gchar * component_role, guint64 hacks)
{
  OMX_ERRORTYPE err;
  GstOMXCore *core;
  GstOMXComponent *comp;

  if (hacks & GST_OMX_HACK_HYBRIS) {
#ifndef HAVE_HYBRIS
    GST_ERROR_OBJECT (parent,
        "hybris hack enabled but hybris support has not been compiled in");
    return NULL;
#else
    core = gst_omx_core_acquire_hybris (core_name);
#endif /* HAVE_HYBRIS */
  } else
    core = gst_omx_core_acquire (core_name);

  if (!core)
    return NULL;

  comp = g_slice_new0 (GstOMXComponent);
  comp->core = core;

  err =
      core->get_handle (&comp->handle, (OMX_STRING) component_name, comp,
      &callbacks);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (parent,
        "Failed to get component handle '%s' from core '%s': 0x%08x",
        component_name, core_name, err);
    gst_omx_core_release (core);
    g_slice_free (GstOMXComponent, comp);
    return NULL;
  }
  GST_DEBUG_OBJECT (parent,
      "Successfully got component handle %p (%s) from core '%s'", comp->handle,
      component_name, core_name);
  comp->parent = gst_object_ref (parent);
  comp->hacks = hacks;

  comp->ports = g_ptr_array_new ();
  comp->n_in_ports = 0;
  comp->n_out_ports = 0;

  comp->lock = g_mutex_new ();
  comp->messages_lock = g_mutex_new ();
  comp->messages_cond = g_cond_new ();

  g_mutex_init (&comp->resurrection_lock);

  g_queue_init (&comp->messages);
  comp->pending_state = OMX_StateInvalid;
  comp->last_error = OMX_ErrorNone;

  /* Set component role if any */
  if (component_role && !(hacks & GST_OMX_HACK_NO_COMPONENT_ROLE)) {
    OMX_PARAM_COMPONENTROLETYPE param;

    GST_OMX_INIT_STRUCT (&param);

    g_strlcpy ((gchar *) param.cRole, component_role, sizeof (param.cRole));
    err =
        gst_omx_component_set_parameter (comp,
        OMX_IndexParamStandardComponentRole, &param);

    GST_DEBUG_OBJECT (parent, "Setting component role to '%s': %s (0x%08x)",
        component_role, gst_omx_error_to_string (err), err);

    /* If setting the role failed this component is unusable */
    if (err != OMX_ErrorNone) {
      gst_omx_component_free (comp);
      return NULL;
    }
  }

  if (comp->hacks & GST_OMX_HACK_ANDROID_BUFFERS) {
    comp->gralloc = gst_gralloc_new ();
    if (!comp->gralloc) {
      GST_ERROR_OBJECT (parent, "Failed to initialize gralloc");
      gst_omx_component_free (comp);
      return NULL;
    }
  }

  OMX_GetState (comp->handle, &comp->state);

  g_mutex_lock (comp->lock);
  gst_omx_component_handle_messages (comp);
  g_mutex_unlock (comp->lock);

  return comp;
}

/* NOTE: Uses comp->messages_lock */
void
gst_omx_component_free (GstOMXComponent * comp)
{
  gint i, n;

  g_return_if_fail (comp != NULL);

  GST_DEBUG_OBJECT (comp->parent, "Unloading component %p", comp);

  if (comp->ports) {
    n = comp->ports->len;
    for (i = 0; i < n; i++) {
      GstOMXPort *port = g_ptr_array_index (comp->ports, i);

      gst_omx_port_deallocate_buffers (port);
      g_assert (port->buffers == NULL);
      g_assert (g_queue_get_length (&port->pending_buffers) == 0);

      g_slice_free (GstOMXPort, port);
    }
#if GLIB_CHECK_VERSION(2,22,0)
    g_ptr_array_unref (comp->ports);
#else
    g_ptr_array_free (comp->ports, TRUE);
#endif
    comp->ports = NULL;
  }

  comp->core->free_handle (comp->handle);
  gst_omx_core_release (comp->core);

  gst_omx_component_flush_messages (comp);

  g_mutex_clear (&comp->resurrection_lock);

  g_cond_free (comp->messages_cond);
  g_mutex_free (comp->messages_lock);
  g_mutex_free (comp->lock);

  gst_object_unref (comp->parent);

  if (comp->gralloc) {
    gst_gralloc_unref (comp->gralloc);
  }

  g_slice_free (GstOMXComponent, comp);
}

/* NOTE: Uses comp->lock and comp->messages_lock */
OMX_ERRORTYPE
gst_omx_component_set_state (GstOMXComponent * comp, OMX_STATETYPE state)
{
  OMX_STATETYPE old_state;
  OMX_ERRORTYPE err = OMX_ErrorNone;

  g_return_val_if_fail (comp != NULL, OMX_ErrorUndefined);

  g_mutex_lock (comp->lock);

  gst_omx_component_handle_messages (comp);

  old_state = comp->state;
  GST_DEBUG_OBJECT (comp->parent, "Setting state from %d to %d", old_state,
      state);

  if ((err = comp->last_error) != OMX_ErrorNone && state > old_state) {
    GST_ERROR_OBJECT (comp->parent, "Component in error state: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
    goto done;
  }

  if (old_state == state || comp->pending_state == state) {
    GST_DEBUG_OBJECT (comp->parent, "Component already in state %d", state);
    goto done;
  }

  comp->pending_state = state;

  /* Reset some things */
  if (old_state == OMX_StateExecuting && state < old_state) {
    g_atomic_int_set (&comp->have_pending_reconfigure_outports, 0);
    g_list_free (comp->pending_reconfigure_outports);
    comp->pending_reconfigure_outports = NULL;
    /* Notify all inports that are still waiting */
    g_mutex_lock (comp->messages_lock);
    g_cond_broadcast (comp->messages_cond);
    g_mutex_unlock (comp->messages_lock);
  }

  err = OMX_SendCommand (comp->handle, OMX_CommandStateSet, state, NULL);
  /* No need to check if anything has changed here */

done:
  gst_omx_component_handle_messages (comp);
  g_mutex_unlock (comp->lock);

  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (comp->parent,
        "Error setting state from %d to %d: %s (0x%08x)", old_state, state,
        gst_omx_error_to_string (err), err);
    gst_omx_component_set_last_error (comp, err);
  }
  return err;
}

/* NOTE: Uses comp->lock and comp->messages_lock */
OMX_STATETYPE
gst_omx_component_get_state (GstOMXComponent * comp, GstClockTime timeout)
{
  OMX_STATETYPE ret;
  GTimeVal *timeval, abstimeout;
  gboolean signalled = TRUE;

  g_return_val_if_fail (comp != NULL, OMX_StateInvalid);

  GST_DEBUG_OBJECT (comp->parent, "Getting state");

  g_mutex_lock (comp->lock);

  gst_omx_component_handle_messages (comp);

  ret = comp->state;
  if (comp->pending_state == OMX_StateInvalid)
    goto done;

  if (comp->last_error != OMX_ErrorNone) {
    GST_ERROR_OBJECT (comp->parent, "Component in error state: %s (0x%08x)",
        gst_omx_error_to_string (comp->last_error), comp->last_error);
    ret = OMX_StateInvalid;
    goto done;
  }

  if (timeout != GST_CLOCK_TIME_NONE) {
    glong add = timeout / (GST_SECOND / G_USEC_PER_SEC);

    if (add == 0)
      goto done;

    g_get_current_time (&abstimeout);
    g_time_val_add (&abstimeout, add);
    timeval = &abstimeout;
    GST_DEBUG_OBJECT (comp->parent, "Waiting for %ld us", add);
  } else {
    timeval = NULL;
    GST_DEBUG_OBJECT (comp->parent, "Waiting for signal");
  }

  gst_omx_component_handle_messages (comp);
  while (signalled && comp->last_error == OMX_ErrorNone
      && comp->pending_state != OMX_StateInvalid) {
    g_mutex_lock (comp->messages_lock);
    g_mutex_unlock (comp->lock);
    if (!g_queue_is_empty (&comp->messages)) {
      signalled = TRUE;
    }
    if (timeval == NULL) {
      g_cond_wait (comp->messages_cond, comp->messages_lock);
      signalled = TRUE;
    } else {
      signalled =
          g_cond_timed_wait (comp->messages_cond, comp->messages_lock,
          timeval);
    }
    g_mutex_unlock (comp->messages_lock);
    g_mutex_lock (comp->lock);
    if (signalled)
      gst_omx_component_handle_messages (comp);
  };

  if (signalled) {
    if (comp->last_error != OMX_ErrorNone) {
      GST_ERROR_OBJECT (comp->parent,
          "Got error while waiting for state change: %s (0x%08x)",
          gst_omx_error_to_string (comp->last_error), comp->last_error);
      ret = OMX_StateInvalid;
    } else if (comp->pending_state == OMX_StateInvalid) {
      /* State change finished and everything's fine */
      ret = comp->state;
    } else {
      ret = OMX_StateInvalid;
      g_assert_not_reached ();
    }
  } else {
    ret = OMX_StateInvalid;
    GST_WARNING_OBJECT (comp->parent, "Timeout while waiting for state change");
  }

done:
  g_mutex_unlock (comp->lock);

  /* If we waited and timed out this component is unusable now */
  if (!signalled)
    gst_omx_component_set_last_error (comp, OMX_ErrorTimeout);

  GST_DEBUG_OBJECT (comp->parent, "Returning state %d", ret);

  return ret;
}

GstOMXPort *
gst_omx_component_add_port (GstOMXComponent * comp, guint32 index)
{
  gint i, n;
  GstOMXPort *port;
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_ERRORTYPE err;

  g_return_val_if_fail (comp != NULL, NULL);

  /* Check if this port exists already */
  n = comp->ports->len;
  for (i = 0; i < n; i++) {
    port = g_ptr_array_index (comp->ports, i);
    g_return_val_if_fail (port->index != index, NULL);
  }

  GST_DEBUG_OBJECT (comp->parent, "Adding port %u", index);

  GST_OMX_INIT_STRUCT (&port_def);
  port_def.nPortIndex = index;

  err = gst_omx_component_get_parameter (comp, OMX_IndexParamPortDefinition,
      &port_def);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (comp->parent, "Failed to add port %u: %s (0x%08x)", index,
        gst_omx_error_to_string (err), err);
    return NULL;
  }

  /* We should immediately enable Android native buffers */
  if (comp->hacks & GST_OMX_HACK_ANDROID_BUFFERS
      && port_def.eDir == OMX_DirOutput) {
    OMX_INDEXTYPE extension;
    struct EnableAndroidNativeBuffersParams param;
    struct GetAndroidNativeBufferUsageParams usage_param;

    GST_DEBUG_OBJECT (comp->parent, "Trying to enable Android native buffers");

    err = OMX_GetExtensionIndex (comp->handle,
        (OMX_STRING) "OMX.google.android.index.enableAndroidNativeBuffers2",
        &extension);

    if (err != OMX_ErrorNone) {
      GST_ERROR_OBJECT (comp->parent,
          "Failed to get Android native buffers extension index on port %u: %s (0x%08x)",
          index, gst_omx_error_to_string (err), err);
      return NULL;
    }

    GST_OMX_INIT_STRUCT (&param);
    param.nPortIndex = index;
    param.enable = OMX_TRUE;

    err = gst_omx_component_set_parameter (comp, extension, &param);

    if (err != OMX_ErrorNone) {
      GST_ERROR_OBJECT (comp->parent, "Failed to enable extension: %s (0x%08x)",
          gst_omx_error_to_string (err), err);

      return NULL;
    }

    /* Now we need to get the new port definition as it might have changed. */
    err = gst_omx_component_get_parameter (comp, OMX_IndexParamPortDefinition,
        &port_def);
    if (err != OMX_ErrorNone) {
      GST_ERROR_OBJECT (comp->parent, "Failed to add port %u: %s (0x%08x)",
          index, gst_omx_error_to_string (err), err);
      return NULL;
    }

    /* Now get usage */
    err = OMX_GetExtensionIndex (comp->handle,
        (OMX_STRING) "OMX.google.android.index.getAndroidNativeBufferUsage",
        &extension);

    if (err != OMX_ErrorNone) {
      GST_ERROR_OBJECT (comp->parent,
          "Failed to get buffer usage extension index on port %u: %s (0x%08x)",
          index, gst_omx_error_to_string (err), err);
      return NULL;
    }

    GST_OMX_INIT_STRUCT (&usage_param);
    usage_param.nPortIndex = index;

    err = gst_omx_component_get_parameter (comp, extension, &param);

    if (err != OMX_ErrorNone) {
      GST_ERROR_OBJECT (comp->parent, "Failed to get usage: %s (0x%08x)",
          gst_omx_error_to_string (err), err);

      return NULL;
    }

    comp->android_buffer_usage = usage_param.nUsage;

    GST_DEBUG_OBJECT (comp->parent, "Enabled Android native buffers");
  }

  port = g_slice_new0 (GstOMXPort);
  port->comp = comp;
  port->index = index;

  port->port_def = port_def;

  g_queue_init (&port->pending_buffers);
  port->resurrection_cookie = 1;
  port->flushing = TRUE;
  port->flushed = FALSE;
  port->settings_changed = FALSE;
  port->enabled_changed = FALSE;

  if (port->port_def.eDir == OMX_DirInput)
    comp->n_in_ports++;
  else
    comp->n_out_ports++;

  g_ptr_array_add (comp->ports, port);

  return port;
}

GstOMXPort *
gst_omx_component_get_port (GstOMXComponent * comp, guint32 index)
{
  gint i, n;

  n = comp->ports->len;
  for (i = 0; i < n; i++) {
    GstOMXPort *tmp = g_ptr_array_index (comp->ports, i);

    if (tmp->index == index)
      return tmp;
  }
  return NULL;
}

void
gst_omx_component_trigger_settings_changed (GstOMXComponent * comp,
    guint32 port_index)
{
  g_return_if_fail (comp != NULL);

  /* Reverse hacks */
  if (port_index == 1
      && (comp->hacks & GST_OMX_HACK_EVENT_PORT_SETTINGS_CHANGED_PORT_0_TO_1))
    port_index = 0;

  if (!(comp->hacks &
          GST_OMX_HACK_EVENT_PORT_SETTINGS_CHANGED_NDATA_PARAMETER_SWAP)) {
    EventHandler (comp->handle, comp, OMX_EventPortSettingsChanged, port_index,
        0, NULL);
  } else {
    EventHandler (comp->handle, comp, OMX_EventPortSettingsChanged, 0,
        port_index, NULL);
  }
}

/* NOTE: Uses comp->lock and comp->messages_lock */
void
gst_omx_component_set_last_error (GstOMXComponent * comp, OMX_ERRORTYPE err)
{
  g_return_if_fail (comp != NULL);

  if (err == OMX_ErrorNone)
    return;

  g_mutex_lock (comp->lock);
  GST_ERROR_OBJECT (comp->parent, "Setting last error: %s (0x%08x)",
      gst_omx_error_to_string (err), err);
  /* We only set the first error ever from which
   * we can't recover anymore.
   */
  if (comp->last_error == OMX_ErrorNone)
    comp->last_error = err;
  g_mutex_unlock (comp->lock);

  g_mutex_lock (comp->messages_lock);
  g_cond_broadcast (comp->messages_cond);
  g_mutex_unlock (comp->messages_lock);
}

/* NOTE: Uses comp->lock and comp->messages_lock */
OMX_ERRORTYPE
gst_omx_component_get_last_error (GstOMXComponent * comp)
{
  OMX_ERRORTYPE err;

  g_return_val_if_fail (comp != NULL, OMX_ErrorUndefined);

  g_mutex_lock (comp->lock);
  gst_omx_component_handle_messages (comp);
  err = comp->last_error;
  g_mutex_unlock (comp->lock);

  GST_DEBUG_OBJECT (comp->parent, "Returning last error: %s (0x%08x)",
      gst_omx_error_to_string (err), err);

  return err;
}

const gchar *
gst_omx_component_get_last_error_string (GstOMXComponent * comp)
{
  g_return_val_if_fail (comp != NULL, NULL);

  return gst_omx_error_to_string (gst_omx_component_get_last_error (comp));
}

/* comp->lock must be unlocked while calling this */
OMX_ERRORTYPE
gst_omx_component_get_parameter (GstOMXComponent * comp, OMX_INDEXTYPE index,
    gpointer param)
{
  OMX_ERRORTYPE err;

  g_return_val_if_fail (comp != NULL, OMX_ErrorUndefined);
  g_return_val_if_fail (param != NULL, OMX_ErrorUndefined);

  GST_DEBUG_OBJECT (comp->parent, "Getting parameter at index 0x%08x", index);
  err = OMX_GetParameter (comp->handle, index, param);
  GST_DEBUG_OBJECT (comp->parent, "Got parameter at index 0x%08x: %s (0x%08x)",
      index, gst_omx_error_to_string (err), err);

  return err;
}

/* comp->lock must be unlocked while calling this */
OMX_ERRORTYPE
gst_omx_component_set_parameter (GstOMXComponent * comp, OMX_INDEXTYPE index,
    gpointer param)
{
  OMX_ERRORTYPE err;

  g_return_val_if_fail (comp != NULL, OMX_ErrorUndefined);
  g_return_val_if_fail (param != NULL, OMX_ErrorUndefined);

  GST_DEBUG_OBJECT (comp->parent, "Setting parameter at index 0x%08x", index);
  err = OMX_SetParameter (comp->handle, index, param);
  GST_DEBUG_OBJECT (comp->parent, "Set parameter at index 0x%08x: %s (0x%08x)",
      index, gst_omx_error_to_string (err), err);

  return err;
}

/* comp->lock must be unlocked while calling this */
OMX_ERRORTYPE
gst_omx_component_get_config (GstOMXComponent * comp, OMX_INDEXTYPE index,
    gpointer config)
{
  OMX_ERRORTYPE err;

  g_return_val_if_fail (comp != NULL, OMX_ErrorUndefined);
  g_return_val_if_fail (config != NULL, OMX_ErrorUndefined);

  GST_DEBUG_OBJECT (comp->parent, "Getting configuration at index 0x%08x",
      index);
  err = OMX_GetConfig (comp->handle, index, config);
  GST_DEBUG_OBJECT (comp->parent, "Got parameter at index 0x%08x: %s (0x%08x)",
      index, gst_omx_error_to_string (err), err);

  return err;
}

/* comp->lock must be unlocked while calling this */
OMX_ERRORTYPE
gst_omx_component_set_config (GstOMXComponent * comp, OMX_INDEXTYPE index,
    gpointer config)
{
  OMX_ERRORTYPE err;

  g_return_val_if_fail (comp != NULL, OMX_ErrorUndefined);
  g_return_val_if_fail (config != NULL, OMX_ErrorUndefined);

  GST_DEBUG_OBJECT (comp->parent, "Setting configuration at index 0x%08x",
      index);
  err = OMX_SetConfig (comp->handle, index, config);
  GST_DEBUG_OBJECT (comp->parent, "Set parameter at index 0x%08x: %s (0x%08x)",
      index, gst_omx_error_to_string (err), err);

  return err;
}

void
gst_omx_port_get_port_definition (GstOMXPort * port,
    OMX_PARAM_PORTDEFINITIONTYPE * port_def)
{
  GstOMXComponent *comp;

  g_return_if_fail (port != NULL);

  comp = port->comp;

  GST_OMX_INIT_STRUCT (port_def);
  port_def->nPortIndex = port->index;

  gst_omx_component_get_parameter (comp, OMX_IndexParamPortDefinition,
      port_def);
}

gboolean
gst_omx_port_update_port_definition (GstOMXPort * port,
    OMX_PARAM_PORTDEFINITIONTYPE * port_def)
{
  OMX_ERRORTYPE err = OMX_ErrorNone;
  GstOMXComponent *comp;

  g_return_val_if_fail (port != NULL, FALSE);

  comp = port->comp;

  if (port_def)
    err =
        gst_omx_component_set_parameter (comp, OMX_IndexParamPortDefinition,
        port_def);
  gst_omx_component_get_parameter (comp, OMX_IndexParamPortDefinition,
      &port->port_def);

  GST_DEBUG_OBJECT (comp->parent, "Updated port %u definition: %s (0x%08x)",
      port->index, gst_omx_error_to_string (err), err);

  return (err == OMX_ErrorNone);
}

/* NOTE: Uses comp->lock and comp->messages_lock */
GstOMXAcquireBufferReturn
gst_omx_port_acquire_buffer (GstOMXPort * port, GstOMXBuffer ** buf)
{
  GstOMXAcquireBufferReturn ret = GST_OMX_ACQUIRE_BUFFER_ERROR;
  GstOMXComponent *comp;
  OMX_ERRORTYPE err;
  GstOMXBuffer *_buf = NULL;

  g_return_val_if_fail (port != NULL, GST_OMX_ACQUIRE_BUFFER_ERROR);
  g_return_val_if_fail (buf != NULL, GST_OMX_ACQUIRE_BUFFER_ERROR);

  *buf = NULL;

  comp = port->comp;

  g_mutex_lock (comp->lock);
  GST_DEBUG_OBJECT (comp->parent, "Acquiring buffer from port %u", port->index);

retry:
  gst_omx_component_handle_messages (comp);

  /* Check if the component is in an error state */
  if ((err = comp->last_error) != OMX_ErrorNone) {
    GST_ERROR_OBJECT (comp->parent, "Component is in error state: %s",
        gst_omx_error_to_string (err));
    ret = GST_OMX_ACQUIRE_BUFFER_ERROR;
    goto done;
  }

  /* Check if the port is flushing */
  if (port->flushing) {
    ret = GST_OMX_ACQUIRE_BUFFER_FLUSHING;
    goto done;
  }

  /* If this is an input port and at least one of the output ports
   * needs to be reconfigured, we wait until all output ports are
   * reconfigured. Afterwards this port is reconfigured if required
   * or buffers are returned to be filled as usual.
   */
  if (port->port_def.eDir == OMX_DirInput) {
    if (g_atomic_int_get (&comp->have_pending_reconfigure_outports)) {
      gst_omx_component_handle_messages (comp);
      while (g_atomic_int_get (&comp->have_pending_reconfigure_outports) &&
          (err = comp->last_error) == OMX_ErrorNone && !port->flushing) {
        GST_DEBUG_OBJECT (comp->parent,
            "Waiting for output ports to reconfigure");
        g_mutex_lock (comp->messages_lock);
        g_mutex_unlock (comp->lock);
        if (g_queue_is_empty (&comp->messages))
          g_cond_wait (comp->messages_cond, comp->messages_lock);
        g_mutex_unlock (comp->messages_lock);
        g_mutex_lock (comp->lock);
        gst_omx_component_handle_messages (comp);
      }
      goto retry;
    }

    /* Only if this port needs to be reconfigured too notify
     * the caller about it */
    if (port->settings_cookie != port->configured_settings_cookie) {
      ret = GST_OMX_ACQUIRE_BUFFER_RECONFIGURE;
      port->settings_changed = TRUE;
      goto done;
    }
  }

  /* If we have an output port that needs to be reconfigured
   * and it still has buffers pending for the old configuration
   * we first return them.
   * NOTE: If buffers for this configuration arrive later
   * we have to drop them... */
  if (port->port_def.eDir == OMX_DirOutput &&
      port->settings_cookie != port->configured_settings_cookie) {
    if (!g_queue_is_empty (&port->pending_buffers)) {
      GST_DEBUG_OBJECT (comp->parent,
          "Output port %u needs reconfiguration but has buffers pending",
          port->index);
      _buf = g_queue_pop_head (&port->pending_buffers);
      if (_buf->settings_cookie == port->settings_cookie) {
        g_queue_push_head (&port->pending_buffers, _buf);

        ret = GST_OMX_ACQUIRE_BUFFER_RECONFIGURE;
        port->settings_changed = TRUE;
      } else {
        ret = GST_OMX_ACQUIRE_BUFFER_OK;
        _buf->settings_cookie = port->settings_cookie;
        }
      goto done;
    }

    ret = GST_OMX_ACQUIRE_BUFFER_RECONFIGURE;
    port->settings_changed = TRUE;
    goto done;
  }

  if (port->settings_changed) {
    GST_DEBUG_OBJECT (comp->parent,
        "Port %u has settings changed, need new caps", port->index);
    ret = GST_OMX_ACQUIRE_BUFFER_RECONFIGURED;
    port->settings_changed = FALSE;
    goto done;
  }

  /* 
   * At this point we have no error or flushing port
   * and a properly configured port.
   *
   */

  /* If the queue is empty we wait until a buffer
   * arrives, an error happens, the port is flushing
   * or the port needs to be reconfigured.
   */
  gst_omx_component_handle_messages (comp);
  if (g_queue_is_empty (&port->pending_buffers)) {
    GST_DEBUG_OBJECT (comp->parent, "Queue of port %u is empty", port->index);
    g_mutex_lock (comp->messages_lock);
    g_mutex_unlock (comp->lock);
    if (g_queue_is_empty (&comp->messages))
      g_cond_wait (comp->messages_cond, comp->messages_lock);
    g_mutex_unlock (comp->messages_lock);
    g_mutex_lock (comp->lock);
    gst_omx_component_handle_messages (comp);

    /* And now check everything again and maybe get a buffer */
    goto retry;
  } else {
    GST_DEBUG_OBJECT (comp->parent, "Port %u has pending buffers", port->index);
    _buf = g_queue_pop_head (&port->pending_buffers);
    ret = GST_OMX_ACQUIRE_BUFFER_OK;
    goto done;
  }

  g_assert_not_reached ();
  goto retry;

done:
  if (_buf && _buf->native_buffer) {
    gst_buffer_ref (GST_BUFFER (_buf->native_buffer));
    gst_object_ref (comp->parent);
  }

  g_mutex_unlock (comp->lock);

  if (_buf) {
    g_assert (_buf == _buf->omx_buf->pAppPrivate);
    *buf = _buf;
  }

  GST_DEBUG_OBJECT (comp->parent, "Acquired buffer %p (%p) from port %u: %d",
      _buf, (_buf ? _buf->omx_buf->pBuffer : NULL), port->index, ret);

  return ret;
}

/* NOTE: Uses comp->lock and comp->messages_lock */
OMX_ERRORTYPE
gst_omx_port_release_buffer (GstOMXPort * port, GstOMXBuffer * buf)
{
  GstOMXComponent *comp;
  GstNativeBuffer *native_buffer;
  OMX_ERRORTYPE err = OMX_ErrorNone;

  g_return_val_if_fail (port != NULL, OMX_ErrorUndefined);
  g_return_val_if_fail (buf != NULL, OMX_ErrorUndefined);
  g_return_val_if_fail (buf->port == port, OMX_ErrorUndefined);

  comp = port->comp;

  g_mutex_lock (comp->lock);

  GST_DEBUG_OBJECT (comp->parent, "Releasing buffer %p (%p) to port %u",
      buf, buf->omx_buf->pBuffer, port->index);

  native_buffer = buf->native_buffer;

  gst_omx_component_handle_messages (comp);

#if 0
  if (port->port_def.eDir == OMX_DirInput) {
    /* Reset all flags, some implementations don't
     * reset them themselves and the flags are not
     * valid anymore after the buffer was consumed
     */
    buf->omx_buf->nFlags = 0;
  }
#endif

  if ((err = comp->last_error) != OMX_ErrorNone) {
    GST_ERROR_OBJECT (comp->parent, "Component is in error state: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
    g_queue_push_tail (&port->pending_buffers, buf);
    g_mutex_lock (comp->messages_lock);
    g_cond_broadcast (comp->messages_cond);
    g_mutex_unlock (comp->messages_lock);
    goto done;
  }

  if (port->flushing) {
    GST_DEBUG_OBJECT (comp->parent, "Port %u is flushing, not releasing buffer",
        port->index);
    g_queue_push_tail (&port->pending_buffers, buf);
    g_mutex_lock (comp->messages_lock);
    g_cond_broadcast (comp->messages_cond);
    g_mutex_unlock (comp->messages_lock);
    goto done;
  }

  g_assert (buf == buf->omx_buf->pAppPrivate);

  /* FIXME: What if the settings cookies don't match? */

  buf->used = TRUE;

  if (port->port_def.eDir == OMX_DirInput) {
    err = OMX_EmptyThisBuffer (comp->handle, buf->omx_buf);
  } else {
    err = OMX_FillThisBuffer (comp->handle, buf->omx_buf);
  }
  GST_DEBUG_OBJECT (comp->parent, "Released buffer %p to port %u: %s (0x%08x)",
      buf, port->index, gst_omx_error_to_string (err), err);

done:
  gst_omx_component_handle_messages (comp);
  g_mutex_unlock (comp->lock);

  if (native_buffer) {
    gst_buffer_unref (GST_BUFFER (native_buffer));
    gst_object_unref (comp->parent);
  }

  if (err != OMX_ErrorNone)
    gst_omx_component_set_last_error (comp, err);

  return err;
}

/* NOTE: Uses comp->lock and comp->messages_lock */
OMX_ERRORTYPE
gst_omx_port_set_flushing (GstOMXPort * port, gboolean flush)
{
  GstOMXComponent *comp;
  OMX_ERRORTYPE err = OMX_ErrorNone;

  g_return_val_if_fail (port != NULL, OMX_ErrorUndefined);

  comp = port->comp;

  g_mutex_lock (comp->lock);

  GST_DEBUG_OBJECT (comp->parent, "Setting port %d to %sflushing",
      port->index, (flush ? "" : "not "));

  gst_omx_component_handle_messages (comp);

  if (! !flush == ! !port->flushing) {
    GST_DEBUG_OBJECT (comp->parent, "Port %u was %sflushing already",
        port->index, (flush ? "" : "not "));
    goto done;
  }

  if ((err = comp->last_error) != OMX_ErrorNone) {
    GST_ERROR_OBJECT (comp->parent, "Component is in error state: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
    goto done;
  }

  if (comp->state != OMX_StateIdle && comp->state != OMX_StateExecuting) {

    GST_DEBUG_OBJECT (comp->parent, "Component is in wrong state: %d",
        comp->state);
    err = OMX_ErrorUndefined;

    goto done;
  }

  port->flushing = flush;
  if (flush) {
    GTimeVal abstimeout, *timeval;
    gboolean signalled;
    OMX_ERRORTYPE last_error;

    g_mutex_lock (comp->messages_lock);
    g_cond_broadcast (comp->messages_cond);
    g_mutex_unlock (comp->messages_lock);

    /* Now flush the port */
    port->flushed = FALSE;

    err = OMX_SendCommand (comp->handle, OMX_CommandFlush, port->index, NULL);

    if (err != OMX_ErrorNone) {
      GST_ERROR_OBJECT (comp->parent,
          "Error sending flush command to port %u: %s (0x%08x)", port->index,
          gst_omx_error_to_string (err), err);
      goto done;
    }

    if ((err = comp->last_error) != OMX_ErrorNone) {
      GST_ERROR_OBJECT (comp->parent,
          "Component is in error state: %s (0x%08x)",
          gst_omx_error_to_string (err), err);
      goto done;
    }

    if (! !port->flushing != ! !flush) {
      GST_ERROR_OBJECT (comp->parent, "Another flush happened in the meantime");
      goto done;
    }

    g_get_current_time (&abstimeout);
    g_time_val_add (&abstimeout, 5 * G_USEC_PER_SEC);
    timeval = &abstimeout;
    GST_DEBUG_OBJECT (comp->parent, "Waiting for 5s");

    /* Retry until timeout or until an error happend or
     * until all buffers were released by the component and
     * the flush command completed */
    signalled = TRUE;
    last_error = OMX_ErrorNone;
    gst_omx_component_handle_messages (comp);
    while (signalled && last_error == OMX_ErrorNone && !port->flushed
        && port->buffers->len > g_queue_get_length (&port->pending_buffers)) {
      g_mutex_lock (comp->messages_lock);
      g_mutex_unlock (comp->lock);
      if (!g_queue_is_empty (&comp->messages))
        signalled = TRUE;
      else
        signalled =
            g_cond_timed_wait (comp->messages_cond, comp->messages_lock,
            timeval);
      g_mutex_unlock (comp->messages_lock);
      g_mutex_lock (comp->lock);

      if (signalled)
        gst_omx_component_handle_messages (comp);

      last_error = comp->last_error;
    }
    port->flushed = FALSE;

    GST_DEBUG_OBJECT (comp->parent, "Port %d flushed", port->index);
    if (last_error != OMX_ErrorNone) {
      GST_ERROR_OBJECT (comp->parent,
          "Got error while flushing port %u: %s (0x%08x)", port->index,
          gst_omx_error_to_string (last_error), last_error);
      err = last_error;
      goto done;
    } else if (!signalled) {
      GST_ERROR_OBJECT (comp->parent, "Timeout while flushing port %u",
          port->index);
      err = OMX_ErrorTimeout;
      goto done;
    }
  } else {
    if (port->port_def.eDir == OMX_DirOutput && port->buffers) {
      GstOMXBuffer *buf;

      GQueue *pending = g_queue_copy (&port->pending_buffers);
      g_queue_clear (&port->pending_buffers);

      /* Enqueue all buffers for the component to fill */
      while ((buf = g_queue_pop_head (pending))) {
        if (!buf)
          continue;

        g_assert (!buf->used);

        /* Reset all flags, some implementations don't
         * reset them themselves and the flags are not
         * valid anymore after the buffer was consumed
         */
        buf->omx_buf->nFlags = 0;

        err = OMX_FillThisBuffer (comp->handle, buf->omx_buf);

        if (err != OMX_ErrorNone) {
          buf->used = FALSE;
          g_queue_push_tail (&port->pending_buffers, buf);

          GST_ERROR_OBJECT (comp->parent,
              "Failed to pass buffer %p (%p) to port %u: %s (0x%08x)", buf,
              buf->omx_buf->pBuffer, port->index,
              gst_omx_error_to_string (err), err);
          goto error;
        }
        GST_DEBUG_OBJECT (comp->parent, "Passed buffer %p (%p) to component",
            buf, buf->omx_buf->pBuffer);
      }

      g_queue_free (pending);
    }
  }

done:
  GST_DEBUG_OBJECT (comp->parent, "Set port %u to %sflushing: %s (0x%08x)",
      port->index, (flush ? "" : "not "), gst_omx_error_to_string (err), err);
  gst_omx_component_handle_messages (comp);
  g_mutex_unlock (comp->lock);

  return err;

error:
  {
    g_mutex_unlock (comp->lock);
    gst_omx_component_set_last_error (comp, err);
    g_mutex_lock (comp->lock);
    goto done;
  }
}

/* NOTE: Uses comp->lock and comp->messages_lock */
gboolean
gst_omx_port_is_flushing (GstOMXPort * port)
{
  GstOMXComponent *comp;
  gboolean flushing;

  g_return_val_if_fail (port != NULL, FALSE);

  comp = port->comp;

  g_mutex_lock (comp->lock);
  gst_omx_component_handle_messages (port->comp);
  flushing = port->flushing;
  g_mutex_unlock (comp->lock);

  GST_DEBUG_OBJECT (comp->parent, "Port %u is flushing: %d", port->index,
      flushing);

  return flushing;
}

static gboolean
gst_omx_resurrect_buffer (void *data, GstNativeBuffer * buffer)
{
  GstOMXBuffer *buf = (GstOMXBuffer *) data;
  gboolean resurrect = FALSE;

  g_mutex_lock (&buf->port->comp->resurrection_lock);
  resurrect = buf->resurrection_cookie == buf->port->resurrection_cookie;
  buf->resurrection_cookie = 0;
  g_mutex_unlock (&buf->port->comp->resurrection_lock);

  /* Return buffers with a current resurrection cookie to the decoder, otherwise
   * the decoder no longer references the buffer so destroy it. */
  if (resurrect) {
    GST_DEBUG_OBJECT (buf->port->comp->parent, "resurrecting buffer %p (%p)", buf,
        buf->omx_buf->pBuffer);

    gst_buffer_ref (GST_BUFFER (buffer));

    /* increment the reference count gst_omx_port_release buffer is going to
     * decrement. */
    gst_buffer_ref (GST_BUFFER (buffer));

    gst_omx_port_release_buffer (buf->port, buf);
  } else {
      buffer_handle_t *handle;
      GstGralloc *gralloc;

      GST_DEBUG_OBJECT (buf->port->comp->parent, "destroy buffer %p (%p)", buf,
          buf->omx_buf);

      gralloc = gst_native_buffer_get_gralloc (buffer);
      handle = gst_native_buffer_get_handle (buffer);
      gst_gralloc_free (gralloc, *handle);

    g_slice_free (GstOMXBuffer, buf);
  }

  return resurrect;
}

/* NOTE: Must be called while holding comp->lock, uses comp->messages_lock */
static OMX_ERRORTYPE
gst_omx_port_allocate_buffers_unlocked (GstOMXPort * port)
{
  GstOMXComponent *comp;
  OMX_ERRORTYPE err = OMX_ErrorNone;
  gint i, n;

  g_assert (!port->buffers || port->buffers->len == 0);

  comp = port->comp;

  gst_omx_component_handle_messages (port->comp);
  if ((err = comp->last_error) != OMX_ErrorNone) {
    GST_ERROR_OBJECT (comp->parent, "Component in error state: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
    goto done;
  }

  /* Update the port definition to check if we need more
   * buffers after the port configuration was done and to
   * update the buffer size
   */
  gst_omx_component_get_parameter (comp, OMX_IndexParamPortDefinition,
      &port->port_def);

  /* If the configured, actual number of buffers is less than
   * the minimal number of buffers required, use the minimal
   * number of buffers
   */
  if (port->port_def.nBufferCountActual < port->port_def.nBufferCountMin) {
    port->port_def.nBufferCountActual = port->port_def.nBufferCountMin;
    err = gst_omx_component_set_parameter (comp, OMX_IndexParamPortDefinition,
        &port->port_def);
    gst_omx_component_get_parameter (comp, OMX_IndexParamPortDefinition,
        &port->port_def);
  }

  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (comp->parent,
        "Failed to configure number of buffers of port %u: %s (0x%08x)",
        port->index, gst_omx_error_to_string (err), err);
    goto error;
  }

  n = port->port_def.nBufferCountActual;
  GST_DEBUG_OBJECT (comp->parent,
      "Allocating %d buffers of size %u for port %u", n,
      port->port_def.nBufferSize, port->index);

  if (!port->buffers)
    port->buffers = g_ptr_array_sized_new (n);

  for (i = 0; i < n; i++) {
    GstOMXBuffer *buf;

    buf = g_slice_new0 (GstOMXBuffer);
    buf->port = port;
    buf->used = FALSE;
    buf->settings_cookie = port->settings_cookie;
    buf->resurrection_cookie = 0;
    g_ptr_array_add (port->buffers, buf);

    if (port->port_def.eDir == OMX_DirOutput
        && comp->hacks & GST_OMX_HACK_ANDROID_BUFFERS) {
      int format = port->port_def.format.video.eColorFormat;
      int width = port->port_def.format.video.nFrameWidth;
      int height = port->port_def.format.video.nFrameHeight;
      int stride = 0;

      buf->android_handle =
          gst_gralloc_allocate (comp->gralloc, width, height, format,
          comp->android_buffer_usage, &stride);
      if (!buf->android_handle) {
        err = OMX_ErrorUndefined;
      } else {
        err =
            OMX_UseBuffer (comp->handle, &buf->omx_buf, port->index, buf,
            port->port_def.nBufferSize, (OMX_U8 *) buf->android_handle);

        if (err == OMX_ErrorNone) {
          buf->native_buffer =
              gst_native_buffer_new (buf->android_handle, comp->gralloc,
              port->port_def.format.video.nFrameWidth,
              port->port_def.format.video.nFrameHeight,
              stride, comp->android_buffer_usage,
              port->port_def.format.video.eColorFormat);

          gst_native_buffer_set_finalize_callback (buf->native_buffer,
              gst_omx_resurrect_buffer, buf);
        } else {
            gst_gralloc_free (comp->gralloc, buf->android_handle);
            buf->android_handle = NULL;
        }
      }
    } else {
      err =
          OMX_AllocateBuffer (comp->handle, &buf->omx_buf, port->index, buf,
          port->port_def.nBufferSize);
    }

    if (err != OMX_ErrorNone) {
      buf->omx_buf = NULL;
      GST_ERROR_OBJECT (comp->parent,
          "Failed to allocate buffer for port %u: %s (0x%08x)", port->index,
          gst_omx_error_to_string (err), err);
      goto error;
    }

    GST_DEBUG_OBJECT (comp->parent, "Allocated buffer %p (%p)", buf,
        buf->omx_buf->pBuffer);

    g_assert (buf->omx_buf->pAppPrivate == buf);

    /* In the beginning all buffers are not owned by the component */
    g_queue_push_tail (&port->pending_buffers, buf);
  }

  gst_omx_component_handle_messages (comp);

done:
  GST_DEBUG_OBJECT (comp->parent, "Allocated buffers for port %u: %s (0x%08x)",
      port->index, gst_omx_error_to_string (err), err);

  return err;

error:
  {
    g_mutex_unlock (comp->lock);
    gst_omx_component_set_last_error (comp, err);
    g_mutex_lock (comp->lock);
    goto done;
  }
}

/* NOTE: Uses comp->lock and comp->messages_lock */
OMX_ERRORTYPE
gst_omx_port_allocate_buffers (GstOMXPort * port)
{
  OMX_ERRORTYPE err;

  g_return_val_if_fail (port != NULL, OMX_ErrorUndefined);

  g_mutex_lock (port->comp->lock);
  err = gst_omx_port_allocate_buffers_unlocked (port);
  g_mutex_unlock (port->comp->lock);

  return err;
}

/* NOTE: Must be called while holding comp->lock, uses comp->messages_lock */
static OMX_ERRORTYPE
gst_omx_port_deallocate_buffers_unlocked (GstOMXPort * port)
{
  GstOMXComponent *comp;
  OMX_ERRORTYPE err = OMX_ErrorNone;
  gint i, n;

  comp = port->comp;

  GST_DEBUG_OBJECT (comp->parent, "Deallocating buffers of port %u",
      port->index);

  gst_omx_component_handle_messages (port->comp);

  if (!port->buffers) {
    GST_DEBUG_OBJECT (comp->parent, "No buffers allocated for port %u",
        port->index);
    goto done;
  }

  if ((err = comp->last_error) != OMX_ErrorNone) {
    GST_ERROR_OBJECT (comp->parent, "Component in error state: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
    /* We still try to deallocate all buffers */
  }

  /* We only allow deallocation of buffers after they
   * were all released from the port, either by flushing
   * the port or by disabling it.
   */
  n = port->buffers->len;
  for (i = 0; i < n; i++) {
    GstOMXBuffer *buf = g_ptr_array_index (port->buffers, i);
    OMX_ERRORTYPE tmp = OMX_ErrorNone;

    if (buf->used)
      GST_ERROR_OBJECT (comp->parent,
          "Trying to free used buffer %p of port %u", buf, port->index);

    /* omx_buf can be NULL if allocation failed earlier
     * and we're just shutting down
     *
     * errors do not cause exiting this loop because we want
     * to deallocate as much as possible.
     */
    if (buf->omx_buf) {
      g_assert (buf == buf->omx_buf->pAppPrivate);
      buf->omx_buf->pAppPrivate = NULL;
      GST_DEBUG_OBJECT (comp->parent, "Deallocating buffer %p (%p)", buf,
          buf->omx_buf->pBuffer);

      tmp = OMX_FreeBuffer (comp->handle, port->index, buf->omx_buf);
      buf->omx_buf = NULL;

      if (tmp != OMX_ErrorNone) {
        GST_ERROR_OBJECT (comp->parent,
            "Failed to deallocate buffer %d of port %u: %s (0x%08x)", i,
            port->index, gst_omx_error_to_string (tmp), tmp);
        if (err == OMX_ErrorNone)
          err = tmp;
      }
    }

    /* Free regular buffers now.  Native buffers are unreffed and will
     * free the GstOMXBuffer when their ref count reaches zero. */
    if (buf->native_buffer) {
        gst_buffer_unref (GST_BUFFER (buf->native_buffer));
    } else {
      g_slice_free (GstOMXBuffer, buf);
    }
  }
  g_queue_clear (&port->pending_buffers);
#if GLIB_CHECK_VERSION(2,22,0)
  g_ptr_array_unref (port->buffers);
#else
  g_ptr_array_free (port->buffers, TRUE);
#endif
  port->buffers = NULL;

  gst_omx_component_handle_messages (comp);

done:
  GST_DEBUG_OBJECT (comp->parent, "Deallocated buffers of port %u: %s (0x%08x)",
      port->index, gst_omx_error_to_string (err), err);

  return err;
}

/* NOTE: Uses comp->lock and comp->messages_lock */
OMX_ERRORTYPE
gst_omx_port_deallocate_buffers (GstOMXPort * port)
{
  OMX_ERRORTYPE err;

  g_return_val_if_fail (port != NULL, OMX_ErrorUndefined);

  g_mutex_lock (port->comp->lock);
  err = gst_omx_port_deallocate_buffers_unlocked (port);
  g_mutex_unlock (port->comp->lock);

  return err;
}

/* NOTE: Must be called while holding comp->lock, uses comp->messages_lock */
static OMX_ERRORTYPE
gst_omx_port_set_enabled_unlocked (GstOMXPort * port, gboolean enabled)
{
  GstOMXComponent *comp;
  OMX_ERRORTYPE err = OMX_ErrorNone;
  GTimeVal abstimeout, *timeval;
  gboolean signalled;
  OMX_ERRORTYPE last_error;

  comp = port->comp;

  gst_omx_component_handle_messages (comp);

  if ((err = comp->last_error) != OMX_ErrorNone) {
    GST_ERROR_OBJECT (comp->parent, "Component in error state: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
    goto done;
  }

  GST_DEBUG_OBJECT (comp->parent, "Setting port %u to %s", port->index,
      (enabled ? "enabled" : "disabled"));

  /* Check if the port is already enabled/disabled first */
  gst_omx_component_get_parameter (comp, OMX_IndexParamPortDefinition,
      &port->port_def);
  if (! !port->port_def.bEnabled == ! !enabled)
    goto done;

  port->enabled_changed = FALSE;

  if (!enabled) {
    /* This is also like flushing, i.e. all buffers are returned
     * by the component and no new buffers should be passed to
     * the component anymore */
    port->flushing = TRUE;
  }

  if (enabled)
    err =
        OMX_SendCommand (comp->handle, OMX_CommandPortEnable, port->index,
        NULL);
  else
    err =
        OMX_SendCommand (comp->handle, OMX_CommandPortDisable,
        port->index, NULL);

  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (comp->parent,
        "Failed to send enable/disable command to port %u: %s (0x%08x)",
        port->index, gst_omx_error_to_string (err), err);
    goto error;
  }

  if ((err = comp->last_error) != OMX_ErrorNone) {
    GST_ERROR_OBJECT (comp->parent, "Component in error state: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
    goto done;
  }

  if (port->enabled_changed) {
    GST_ERROR_OBJECT (comp->parent, "Port enabled/disabled in the meantime");
    goto done;
  }

  if (port->buffers) {
    gint i, n = port->buffers->len;
    g_mutex_lock (&comp->resurrection_lock);
    for (i = 0; i < n; i++) {
      GstOMXBuffer *buf = g_ptr_array_index (port->buffers, i);

      // Return any pushed buffers to the pending queue for cleanup of
      // their buffer headers.
      if (buf->resurrection_cookie > 0) {
        gst_buffer_ref (GST_BUFFER (buf->native_buffer));
        g_queue_push_tail (&port->pending_buffers, buf);
      }
    }
    // Increment the resurrection cookie, any buffers with the old
    // cookie will be deleted when their reference count reaches zero
    // instead of being resurrected as normal.
    port->resurrection_cookie += 1;
    g_mutex_unlock (&comp->resurrection_lock);
  }

  g_get_current_time (&abstimeout);
  g_time_val_add (&abstimeout, 5 * G_USEC_PER_SEC);
  timeval = &abstimeout;
  GST_DEBUG_OBJECT (comp->parent, "Waiting for 5s");

  /* First wait until all buffers are released by the port */
  signalled = TRUE;
  last_error = OMX_ErrorNone;
  gst_omx_component_handle_messages (comp);
  while (signalled && last_error == OMX_ErrorNone && (port->buffers
          && port->buffers->len >
          g_queue_get_length (&port->pending_buffers))) {
    g_mutex_lock (comp->messages_lock);
    g_mutex_unlock (comp->lock);
    if (!g_queue_is_empty (&comp->messages))
      signalled = TRUE;
    else
      signalled =
          g_cond_timed_wait (comp->messages_cond, comp->messages_lock,
          timeval);
    g_mutex_unlock (comp->messages_lock);
    g_mutex_lock (comp->lock);
    if (signalled)
      gst_omx_component_handle_messages (comp);
    last_error = comp->last_error;
  }

  if (last_error != OMX_ErrorNone) {
    err = last_error;
    GST_ERROR_OBJECT (comp->parent,
        "Got error while waiting for port %u to release all buffers: %s (0x%08x)",
        port->index, gst_omx_error_to_string (err), err);
    goto done;
  } else if (!signalled) {
    GST_ERROR_OBJECT (comp->parent,
        "Timeout waiting for port %u to release all buffers", port->index);
    err = OMX_ErrorTimeout;
    goto error;
  }

  /* Allocate/deallocate all buffers for the port to finish
   * the enable/disable command */
  if (enabled) {
    /* If allocation fails this component can't really be used anymore */
    if ((err = gst_omx_port_allocate_buffers_unlocked (port)) != OMX_ErrorNone) {
      goto error;
    }
  } else {
    /* If deallocation fails this component can't really be used anymore */
    if ((err =
            gst_omx_port_deallocate_buffers_unlocked (port)) != OMX_ErrorNone) {
      goto error;
    }
  }

  /* And now wait until the enable/disable command is finished */
  signalled = TRUE;
  last_error = OMX_ErrorNone;
  gst_omx_component_get_parameter (comp, OMX_IndexParamPortDefinition,
      &port->port_def);
  gst_omx_component_handle_messages (comp);
  while (signalled && last_error == OMX_ErrorNone
      && (! !port->port_def.bEnabled != ! !enabled || !port->enabled_changed)) {
    g_mutex_lock (comp->messages_lock);
    g_mutex_unlock (comp->lock);
    if (!g_queue_is_empty (&comp->messages))
      signalled = TRUE;
    else
      signalled =
          g_cond_timed_wait (comp->messages_cond, comp->messages_lock,
          timeval);
    g_mutex_unlock (comp->messages_lock);
    g_mutex_lock (comp->lock);
    if (signalled)
      gst_omx_component_handle_messages (comp);
    last_error = comp->last_error;
    gst_omx_component_get_parameter (comp, OMX_IndexParamPortDefinition,
        &port->port_def);
  }
  port->enabled_changed = FALSE;

  if (!signalled) {
    GST_ERROR_OBJECT (comp->parent,
        "Timeout waiting for port %u to be %s", port->index,
        (enabled ? "enabled" : "disabled"));
    err = OMX_ErrorTimeout;
    goto error;
  } else if (last_error != OMX_ErrorNone) {
    GST_ERROR_OBJECT (comp->parent,
        "Got error while waiting for port %u to be %s: %s (0x%08x)",
        port->index, (enabled ? "enabled" : "disabled"),
        gst_omx_error_to_string (err), err);
    err = last_error;
  } else {
    if (enabled)
      port->flushing = FALSE;

    /* If everything went fine and we have an output port we
     * should provide all newly allocated buffers to the port
     */
    if (enabled && port->port_def.eDir == OMX_DirOutput) {
      GstOMXBuffer *buf;

      /* Enqueue all buffers for the component to fill */
      while ((buf = g_queue_pop_head (&port->pending_buffers))) {
        if (!buf)
          continue;

        g_assert (!buf->used);

        /* Reset all flags, some implementations don't
         * reset them themselves and the flags are not
         * valid anymore after the buffer was consumed
         */
        buf->omx_buf->nFlags = 0;

        err = OMX_FillThisBuffer (comp->handle, buf->omx_buf);

        if (err != OMX_ErrorNone) {
          GST_ERROR_OBJECT (comp->parent,
              "Failed to pass buffer %p (%p) to port %u: %s (0x%08x)", buf,
              buf->omx_buf->pBuffer, port->index, gst_omx_error_to_string (err),
              err);
          goto error;
        }
        GST_DEBUG_OBJECT (comp->parent, "Passed buffer %p (%p) to component",
            buf, buf->omx_buf->pBuffer);
      }
    }
  }

  gst_omx_component_handle_messages (comp);

done:
  GST_DEBUG_OBJECT (comp->parent, "Port %u is %s%s: %s (0x%08x)", port->index,
      (err == OMX_ErrorNone ? "" : "not "),
      (enabled ? "enabled" : "disabled"), gst_omx_error_to_string (err), err);

  return err;

error:
  {
    g_mutex_unlock (comp->lock);
    gst_omx_component_set_last_error (comp, err);
    g_mutex_lock (comp->lock);
    goto done;
  }
}

/* NOTE: Uses comp->lock and comp->messages_lock */
OMX_ERRORTYPE
gst_omx_port_set_enabled (GstOMXPort * port, gboolean enabled)
{
  OMX_ERRORTYPE err;

  g_return_val_if_fail (port != NULL, OMX_ErrorUndefined);

  g_mutex_lock (port->comp->lock);
  err = gst_omx_port_set_enabled_unlocked (port, enabled);
  g_mutex_unlock (port->comp->lock);

  return err;
}

gboolean
gst_omx_port_is_enabled (GstOMXPort * port)
{
  GstOMXComponent *comp;
  gboolean enabled;

  g_return_val_if_fail (port != NULL, FALSE);

  comp = port->comp;

  gst_omx_component_get_parameter (comp, OMX_IndexParamPortDefinition,
      &port->port_def);
  enabled = ! !port->port_def.bEnabled;

  GST_DEBUG_OBJECT (comp->parent, "Port %u is enabled: %d", port->index,
      enabled);

  return enabled;
}

/* NOTE: Uses comp->lock and comp->messages_lock */
OMX_ERRORTYPE
gst_omx_port_reconfigure (GstOMXPort * port)
{
  GstOMXComponent *comp;
  OMX_ERRORTYPE err = OMX_ErrorNone;

  g_return_val_if_fail (port != NULL, OMX_ErrorUndefined);

  comp = port->comp;

  g_mutex_lock (comp->lock);
  GST_DEBUG_OBJECT (comp->parent, "Reconfiguring port %u", port->index);

  gst_omx_component_handle_messages (comp);

  if (!port->settings_changed)
    goto done;

  if ((err = comp->last_error) != OMX_ErrorNone)
    goto done;

  /* Disable and enable the port. This already takes
   * care of deallocating and allocating buffers.
   */
  err = gst_omx_port_set_enabled_unlocked (port, FALSE);
  if (err != OMX_ErrorNone)
    goto done;

  err = gst_omx_port_set_enabled_unlocked (port, TRUE);
  if (err != OMX_ErrorNone)
    goto done;

  port->configured_settings_cookie = port->settings_cookie;

  /* If this is an output port, notify all input ports
   * that might wait for us to reconfigure in
   * acquire_buffer()
   */
  if (port->port_def.eDir == OMX_DirOutput) {
    GList *l;

    for (l = comp->pending_reconfigure_outports; l; l = l->next) {
      if (l->data == (gpointer) port) {
        comp->pending_reconfigure_outports =
            g_list_delete_link (comp->pending_reconfigure_outports, l);
        break;
      }
    }
    if (!comp->pending_reconfigure_outports) {
      g_atomic_int_set (&comp->have_pending_reconfigure_outports, 0);
      g_mutex_lock (comp->messages_lock);
      g_cond_broadcast (comp->messages_cond);
      g_mutex_unlock (comp->messages_lock);
    }
  }

done:
  GST_DEBUG_OBJECT (comp->parent, "Reconfigured port %u: %s (0x%08x)",
      port->index, gst_omx_error_to_string (err), err);

  g_mutex_unlock (comp->lock);

  return err;
}

/* NOTE: Uses comp->lock and comp->messages_lock */
OMX_ERRORTYPE
gst_omx_port_manual_reconfigure (GstOMXPort * port, gboolean start)
{
  GstOMXComponent *comp;
  OMX_ERRORTYPE err = OMX_ErrorNone;

  g_return_val_if_fail (port != NULL, OMX_ErrorUndefined);

  comp = port->comp;

  g_mutex_lock (comp->lock);

  GST_DEBUG_OBJECT (comp->parent, "Manual reconfigure of port %u %s",
      port->index, (start ? "start" : "stsop"));

  gst_omx_component_handle_messages (comp);

  if ((err = comp->last_error) != OMX_ErrorNone)
    goto done;

  if (start)
    port->settings_cookie++;
  else
    port->configured_settings_cookie = port->settings_cookie;

  if (port->port_def.eDir == OMX_DirOutput) {
    GList *l;

    if (start) {
      for (l = comp->pending_reconfigure_outports; l; l = l->next) {
        if (l->data == (gpointer) port)
          break;
      }

      if (!l) {
        comp->pending_reconfigure_outports =
            g_list_prepend (comp->pending_reconfigure_outports, port);
        g_atomic_int_set (&comp->have_pending_reconfigure_outports, 1);
      }
    } else {
      for (l = comp->pending_reconfigure_outports; l; l = l->next) {
        if (l->data == (gpointer) port) {
          comp->pending_reconfigure_outports =
              g_list_delete_link (comp->pending_reconfigure_outports, l);
          break;
        }
      }
      if (!comp->pending_reconfigure_outports) {
        g_atomic_int_set (&comp->have_pending_reconfigure_outports, 0);
        g_mutex_lock (comp->messages_lock);
        g_cond_broadcast (comp->messages_cond);
        g_mutex_unlock (comp->messages_lock);
      }
    }
  }


done:
  GST_DEBUG_OBJECT (comp->parent, "Manual reconfigure of port %u: %s (0x%08x)",
      port->index, gst_omx_error_to_string (err), err);

  g_mutex_unlock (comp->lock);

  return err;
}

GQuark gst_omx_element_name_quark = 0;

static GType (*types[]) (void) = {
gst_omx_mpeg4_video_dec_get_type, gst_omx_h264_dec_get_type,
      gst_omx_h263_dec_get_type, gst_omx_wmv_dec_get_type,
      gst_omx_mpeg4_video_enc_get_type, gst_omx_h264_enc_get_type,
      gst_omx_h263_enc_get_type, gst_omx_aac_enc_get_type,
      gst_omx_mpeg2_video_dec_get_type, gst_omx_vc1_video_dec_get_type};

static GKeyFile *config = NULL;
GKeyFile *
gst_omx_get_configuration (void)
{
  return config;
}

const gchar *
gst_omx_error_to_string (OMX_ERRORTYPE err)
{
  switch (err) {
    case OMX_ErrorNone:
      return "None";
    case OMX_ErrorInsufficientResources:
      return "Insufficient resources";
    case OMX_ErrorUndefined:
      return "Undefined";
    case OMX_ErrorInvalidComponentName:
      return "Invalid component name";
    case OMX_ErrorComponentNotFound:
      return "Component not found";
    case OMX_ErrorInvalidComponent:
      return "Invalid component";
    case OMX_ErrorBadParameter:
      return "Bad parameter";
    case OMX_ErrorNotImplemented:
      return "Not implemented";
    case OMX_ErrorUnderflow:
      return "Underflow";
    case OMX_ErrorOverflow:
      return "Overflow";
    case OMX_ErrorHardware:
      return "Hardware";
    case OMX_ErrorInvalidState:
      return "Invalid state";
    case OMX_ErrorStreamCorrupt:
      return "Stream corrupt";
    case OMX_ErrorPortsNotCompatible:
      return "Ports not compatible";
    case OMX_ErrorResourcesLost:
      return "Resources lost";
    case OMX_ErrorNoMore:
      return "No more";
    case OMX_ErrorVersionMismatch:
      return "Version mismatch";
    case OMX_ErrorNotReady:
      return "Not ready";
    case OMX_ErrorTimeout:
      return "Timeout";
    case OMX_ErrorSameState:
      return "Same state";
    case OMX_ErrorResourcesPreempted:
      return "Resources preempted";
    case OMX_ErrorPortUnresponsiveDuringAllocation:
      return "Port unresponsive during allocation";
    case OMX_ErrorPortUnresponsiveDuringDeallocation:
      return "Port unresponsive during deallocation";
    case OMX_ErrorPortUnresponsiveDuringStop:
      return "Port unresponsive during stop";
    case OMX_ErrorIncorrectStateTransition:
      return "Incorrect state transition";
    case OMX_ErrorIncorrectStateOperation:
      return "Incorrect state operation";
    case OMX_ErrorUnsupportedSetting:
      return "Unsupported setting";
    case OMX_ErrorUnsupportedIndex:
      return "Unsupported index";
    case OMX_ErrorBadPortIndex:
      return "Bad port index";
    case OMX_ErrorPortUnpopulated:
      return "Port unpopulated";
    case OMX_ErrorComponentSuspended:
      return "Component suspended";
    case OMX_ErrorDynamicResourcesUnavailable:
      return "Dynamic resources unavailable";
    case OMX_ErrorMbErrorsInFrame:
      return "Macroblock errors in frame";
    case OMX_ErrorFormatNotDetected:
      return "Format not detected";
    case OMX_ErrorContentPipeOpenFailed:
      return "Content pipe open failed";
    case OMX_ErrorContentPipeCreationFailed:
      return "Content pipe creation failed";
    case OMX_ErrorSeperateTablesUsed:
      return "Separate tables used";
    case OMX_ErrorTunnelingUnsupported:
      return "Tunneling unsupported";
    default:
      if (err >= (guint32) OMX_ErrorKhronosExtensions
          && err < (guint32) OMX_ErrorVendorStartUnused) {
        return "Khronos extension error";
      } else if (err >= (guint32) OMX_ErrorVendorStartUnused
          && err < (guint32) OMX_ErrorMax) {
        return "Vendor specific error";
      } else {
        return "Unknown error";
      }
  }
}

guint64
gst_omx_parse_hacks (gchar ** hacks)
{
  guint64 hacks_flags = 0;

  if (!hacks)
    return 0;

  while (*hacks) {
    if (g_str_equal (*hacks,
            "event-port-settings-changed-ndata-parameter-swap"))
      hacks_flags |=
          GST_OMX_HACK_EVENT_PORT_SETTINGS_CHANGED_NDATA_PARAMETER_SWAP;
    else if (g_str_equal (*hacks, "event-port-settings-changed-port-0-to-1"))
      hacks_flags |= GST_OMX_HACK_EVENT_PORT_SETTINGS_CHANGED_PORT_0_TO_1;
    else if (g_str_equal (*hacks, "video-framerate-integer"))
      hacks_flags |= GST_OMX_HACK_VIDEO_FRAMERATE_INTEGER;
    else if (g_str_equal (*hacks, "syncframe-flag-not-used"))
      hacks_flags |= GST_OMX_HACK_SYNCFRAME_FLAG_NOT_USED;
    else if (g_str_equal (*hacks, "no-component-reconfigure"))
      hacks_flags |= GST_OMX_HACK_NO_COMPONENT_RECONFIGURE;
    else if (g_str_equal (*hacks, "no-empty-eos-buffer"))
      hacks_flags |= GST_OMX_HACK_NO_EMPTY_EOS_BUFFER;
    else if (g_str_equal (*hacks, "drain-may-not-return"))
      hacks_flags |= GST_OMX_HACK_DRAIN_MAY_NOT_RETURN;
    else if (g_str_equal (*hacks, "no-component-role"))
      hacks_flags |= GST_OMX_HACK_NO_COMPONENT_ROLE;
    else if (g_str_equal (*hacks, "hybris"))
      hacks_flags |= GST_OMX_HACK_HYBRIS;
    else if (g_str_equal (*hacks, "android-native-buffers"))
      hacks_flags |= GST_OMX_HACK_ANDROID_BUFFERS;
    else if (g_str_equal (*hacks, "implicit-format-change"))
      hacks_flags |= GST_OMX_HACK_IMPLICIT_FORMAT_CHANGE;
    else
      GST_WARNING ("Unknown hack: %s", *hacks);
    hacks++;
  }

  return hacks_flags;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  gboolean ret = FALSE;
  GError *err = NULL;
  gchar **config_dirs;
  gchar **elements;
  gchar *env_config_dir;
  const gchar *user_config_dir;
  const gchar *const *system_config_dirs;
  gint i, j;
  gsize n_elements;
  static const gchar *config_name[] = { "gstomx.conf", NULL };
  static const gchar *env_config_name[] = { "GST_OMX_CONFIG_DIR", NULL };

  GST_DEBUG_CATEGORY_INIT (gstomx_debug, "omx", 0, "gst-omx");

  gst_omx_element_name_quark =
      g_quark_from_static_string ("gst-omx-element-name");

  /* Read configuration file gstomx.conf from the preferred
   * configuration directories */
  env_config_dir = g_strdup (g_getenv (*env_config_name));
  user_config_dir = g_get_user_config_dir ();
  system_config_dirs = g_get_system_config_dirs ();
  config_dirs =
      g_new (gchar *, g_strv_length ((gchar **) system_config_dirs) + 3);

  i = 0;
  j = 0;
  if (env_config_dir)
    config_dirs[i++] = (gchar *) env_config_dir;
  config_dirs[i++] = (gchar *) user_config_dir;
  while (system_config_dirs[j])
    config_dirs[i++] = (gchar *) system_config_dirs[j++];
  config_dirs[i++] = NULL;

  gst_plugin_add_dependency (plugin, env_config_name,
      (const gchar **) (config_dirs + (env_config_dir ? 1 : 0)), config_name,
      GST_PLUGIN_DEPENDENCY_FLAG_NONE);

  config = g_key_file_new ();
  if (!g_key_file_load_from_dirs (config, *config_name,
          (const gchar **) config_dirs, NULL, G_KEY_FILE_NONE, &err)) {
    GST_ERROR ("Failed to load configuration file: %s", err->message);
    g_error_free (err);
    goto done;
  }

  /* Initialize all types */
  for (i = 0; i < G_N_ELEMENTS (types); i++)
    types[i] ();

  elements = g_key_file_get_groups (config, &n_elements);
  for (i = 0; i < n_elements; i++) {
    GTypeQuery type_query;
    GTypeInfo type_info = { 0, };
    GType type, subtype;
    gchar *type_name, *core_name, *component_name;
    gint rank;

    GST_DEBUG ("Registering element '%s'", elements[i]);

    err = NULL;
    if (!(type_name =
            g_key_file_get_string (config, elements[i], "type-name", &err))) {
      GST_ERROR
          ("Unable to read 'type-name' configuration for element '%s': %s",
          elements[i], err->message);
      g_error_free (err);
      continue;
    }

    type = g_type_from_name (type_name);
    if (type == G_TYPE_INVALID) {
      GST_ERROR ("Invalid type name '%s' for element '%s'", type_name,
          elements[i]);
      g_free (type_name);
      continue;
    }
    if (!g_type_is_a (type, GST_TYPE_ELEMENT)) {
      GST_ERROR ("Type '%s' is no GstElement subtype for element '%s'",
          type_name, elements[i]);
      g_free (type_name);
      continue;
    }
    g_free (type_name);

    /* And now some sanity checking */
    err = NULL;
    if (!(core_name =
            g_key_file_get_string (config, elements[i], "core-name", &err))) {
      GST_ERROR
          ("Unable to read 'core-name' configuration for element '%s': %s",
          elements[i], err->message);
      g_error_free (err);
      continue;
    }
    if (!g_file_test (core_name, G_FILE_TEST_IS_REGULAR)) {
      GST_ERROR ("Core '%s' does not exist for element '%s'", core_name,
          elements[i]);
      g_free (core_name);
      continue;
    }
    g_free (core_name);

    err = NULL;
    if (!(component_name =
            g_key_file_get_string (config, elements[i], "component-name",
                &err))) {
      GST_ERROR
          ("Unable to read 'component-name' configuration for element '%s': %s",
          elements[i], err->message);
      g_error_free (err);
      continue;
    }
    g_free (component_name);

    err = NULL;
    rank = g_key_file_get_integer (config, elements[i], "rank", &err);
    if (err != NULL) {
      GST_ERROR ("No rank set for element '%s': %s", elements[i], err->message);
      g_error_free (err);
      continue;
    }

    /* And now register the type, all other configuration will
     * be handled by the type itself */
    g_type_query (type, &type_query);
    memset (&type_info, 0, sizeof (type_info));
    type_info.class_size = type_query.class_size;
    type_info.instance_size = type_query.instance_size;
    type_name = g_strdup_printf ("%s-%s", g_type_name (type), elements[i]);
    if (g_type_from_name (type_name) != G_TYPE_INVALID) {
      GST_ERROR ("Type '%s' already exists for element '%s'", type_name,
          elements[i]);
      g_free (type_name);
      continue;
    }
    subtype = g_type_register_static (type, type_name, &type_info, 0);
    g_free (type_name);
    g_type_set_qdata (subtype, gst_omx_element_name_quark,
        g_strdup (elements[i]));
    ret |= gst_element_register (plugin, elements[i], rank, subtype);
  }
  g_strfreev (elements);

done:
  g_free (env_config_dir);
  g_free (config_dirs);

  return ret;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "openmax",
    "GStreamer OpenMAX Plug-ins",
    plugin_init,
    PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
