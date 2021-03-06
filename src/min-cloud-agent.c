/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "string.h"

#include <gio/gio.h>
#include <gio/gunixoutputstream.h>
#include <gio/gfiledescriptorbased.h>
#define LIBSOUP_USE_UNSTABLE_REQUEST_API
#include <libsoup/soup.h>
#include <libsoup/soup-request-http.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "libgsystem.h"

#define MCA_KEY_INSTALLED_SUCCESS_ID "79d8b3e43cf9583435de95a9c20e24bc"
#define MCA_USERDATA_SUCCESS_ID      "51b4385a57d62c2db67c1a4a9de2776c"
#define MCA_USERDATA_EXECUTING_ID    "0b6c7d9e88478c8cb1f95e4f7da4ed2a"
#define MCA_USERDATA_FAILED_ID       "fbe69d44d6f0471b6d585bff2ba89cd0"
#define MCA_NOT_FOUND_ID             "7a7746250bdb1c11706883f0920e423c"
#define MCA_REQUEST_FAILED_ID        "ad5105612b8dd30800c2a29b12aabf3f"
#define MCA_TIMEOUT_ID               "d5684567f7d4843dac78ed23ee480163"

typedef enum {
  MCA_STATE_USER_DATA = 0,
  MCA_STATE_OPENSSH_KEY,
  MCA_STATE_DONE
} McaState;

typedef struct {
  gboolean running;
  gboolean metadata_available;
  GInetAddress *addr;
  GInetSocketAddress *addr_port;
  GNetworkMonitor *netmon;
  SoupSession *session;
  GCancellable *cancellable;
  GError *error;
  guint do_one_attempt_id;
  guint request_failure_count;

  GFile *done_stamp;
  GFile *userdata_done_stamp;
  GFile *authorized_keys_path;

  McaState state;
} MinCloudAgentApp;

static gboolean
handle_install_authorized_keys (MinCloudAgentApp      *self,
                                GInputStream               *instream,
                                GCancellable               *cancellable,
                                GError                    **error)
{
  gboolean ret = FALSE;
  int fd;
  guint i;
  gs_unref_object GOutputStream *outstream = NULL;
  gs_unref_object GDataInputStream *datain = NULL;
  gs_unref_ptrarray GPtrArray *lines = g_ptr_array_new_with_free_func (g_free);

  datain = g_data_input_stream_new (instream);

  while (TRUE)
    {
      gsize len;
      GError *temp_error = NULL;
      char *line = g_data_input_stream_read_line_utf8 (datain, &len, cancellable, &temp_error);
      if (temp_error != NULL)
        {
          g_propagate_error (error, temp_error);
          g_prefix_error (error, "Reading ssh keys: ");
          goto out;
        }
      if (!line)
        break;
      g_ptr_array_add (lines, line);
    }

  (void) g_input_stream_close ((GInputStream*)datain, NULL, NULL);

  outstream = (GOutputStream*)g_file_append_to (self->authorized_keys_path, 0, cancellable, error);
  if (!outstream)
    {
      g_prefix_error (error, "Appending to '%s': ",
                      gs_file_get_path_cached (self->authorized_keys_path));
      goto out;
    }

  fd = g_file_descriptor_based_get_fd ((GFileDescriptorBased*)outstream);
  if (fchmod (fd, 0600) != 0)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to chmod authorized_keys: %s",
                   g_strerror (errsv));
      goto out;
    }

  for (i = 0; i < lines->len; i++)
    {
      const char *line = lines->pdata[i];
      char nl[] = { '\n' };
      gsize bytes_written;
      if (!g_output_stream_write_all (outstream, line, strlen (line),
                                      &bytes_written,
                                      cancellable, error))
        goto out;
      if (!g_output_stream_write_all (outstream, nl, sizeof (nl),
                                      &bytes_written,
                                      cancellable, error))
        goto out;
    }

  if (!g_output_stream_flush (outstream, cancellable, error))
    goto out;

  gs_log_structured_print_id_v (MCA_KEY_INSTALLED_SUCCESS_ID,
                                "Successfully installed ssh key for '%s'",
                                "root");

  ret = TRUE;
 out:
  return ret;
}

static gboolean
handle_userdata_script (MinCloudAgentApp      *self,
                        GInputStream               *instream,
                        GCancellable               *cancellable,
                        GError                    **error)
{
  gboolean ret = FALSE;
  gs_free char *tmppath = g_strdup ("/var/tmp/userdata-script.XXXXXX");
  gs_unref_object GOutputStream *outstream = NULL;
  int fd;

  fd = g_mkstemp_full (tmppath, O_WRONLY, 0700);
  if (fd == -1)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create temporary userdata script: %s",
                   g_strerror (errsv));
      goto out;
    }
  outstream = g_unix_output_stream_new (fd, TRUE);

  if (0 > g_output_stream_splice ((GOutputStream*)outstream, instream,
                                  G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET |
                                  G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                  self->cancellable, error))
    goto out;

  gs_log_structured_print_id_v (MCA_USERDATA_EXECUTING_ID,
                                "Executing user data script");

  if (!gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                      cancellable, error,
                                      tmppath,
                                      NULL))
    {
      gs_log_structured_print_id_v (MCA_USERDATA_FAILED_ID,
                                    "User data script failed");
      goto out;
    }

  gs_log_structured_print_id_v (MCA_USERDATA_SUCCESS_ID,
                                "User data script suceeded");
  
  if (!g_file_replace_contents (self->userdata_done_stamp, "done\n", 5,
                                NULL, FALSE, 0,
                                NULL, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  (void) unlink (tmppath);
  return ret;
}

static gboolean
do_one_attempt (gpointer user_data)
{
  GError *local_error = NULL;
  MinCloudAgentApp *self = user_data;
  gs_free char *uri_str = NULL;
  gs_unref_object SoupRequest *request = NULL;
  gs_unref_object GInputStream *instream = NULL;
  gs_unref_object GFileOutputStream *outstream = NULL;
  gs_unref_object GFile *authorized_keys_path = NULL;
  gs_unref_object SoupMessage *msg = NULL;
  SoupURI *uri = NULL;
  const int max_request_failures = 5;
  const char *state_description = NULL;

  /* Skip over already completed states */
 again:
  switch (self->state)
    {
    case MCA_STATE_USER_DATA:
      if (g_file_query_exists (self->userdata_done_stamp, NULL))
        {
          self->state++;
          goto again;
        }
      break;
    case MCA_STATE_OPENSSH_KEY:
      if (g_file_query_exists (self->authorized_keys_path, NULL))
        {
          self->state++;
          goto again;
        }
      break;
    case MCA_STATE_DONE:
      goto out;
    }

  uri = soup_uri_new (NULL);
  soup_uri_set_scheme (uri, "http");
  {
    gs_free char *addr_str = g_inet_address_to_string (self->addr);
    soup_uri_set_host (uri, addr_str);
  }
  soup_uri_set_port (uri, g_inet_socket_address_get_port (self->addr_port));
  switch (self->state)
    {
    case MCA_STATE_USER_DATA:
      soup_uri_set_path (uri, "/2009-04-04/user-data");
      state_description = "user-data";
      break;
    case MCA_STATE_OPENSSH_KEY:
      soup_uri_set_path (uri, "/2009-04-04/meta-data/public-keys/0/openssh-key");
      state_description = "openssh-key";
      break;
    case MCA_STATE_DONE:
      g_assert_not_reached ();
    }

  uri_str = soup_uri_to_string (uri, FALSE);
  g_print ("Requesting '%s'...\n", uri_str);

  request = soup_session_request_uri (self->session, uri, &local_error);
  soup_uri_free (uri);
  if (!request)
    goto out;

  instream = soup_request_send (request, NULL, &local_error);
  if (!instream)
    goto out;

  msg = soup_request_http_get_message ((SoupRequestHTTP*) request);
  if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
      switch (msg->status_code)
        {
        case 404:
        case 410:
          {
            gs_log_structured_print_id_v (MCA_NOT_FOUND_ID, "No %s found", state_description);
            g_clear_error (&local_error);
            /* Note fallthrough to out, where we'll advance to the
               next state */
            goto out;
          }
        default:
          /* Don't actually set the error, we will just continue to
           * the next phase.
           */
          gs_log_structured_print_id_v (MCA_TIMEOUT_ID,
                                        "Error fetching %s: %u %s",
                                        uri_str,
                                        msg->status_code,
                                        soup_status_get_phrase (msg->status_code));
          goto out;
        }
    }

  switch (self->state)
    {
    case MCA_STATE_USER_DATA:
      if (!handle_userdata_script (self, instream, self->cancellable,
                                   &local_error))
        goto out;
      break;
    case MCA_STATE_OPENSSH_KEY:
      if (!handle_install_authorized_keys (self, instream, self->cancellable,
                                           &local_error))
        goto out;
      break;
    default:
      g_assert_not_reached ();
    }

  g_assert (self->state != MCA_STATE_DONE);
  self->state++;
  self->request_failure_count = 0;

 out:
  if (local_error)
    {
      self->request_failure_count++;
      if (self->request_failure_count >= max_request_failures)
        {
          g_error_free (local_error);
          gs_log_structured_print_id_v (MCA_TIMEOUT_ID,
                                        "Reached maximum failed attempts (%u) to fetch metadata",
                                        self->request_failure_count);
          self->do_one_attempt_id = 0;
          self->running = FALSE;
        }
      else
        {
          gs_log_structured_print_id_v (MCA_REQUEST_FAILED_ID,
                                        "Request failed (count: %u): %s", self->request_failure_count,
                                        local_error->message);
          g_error_free (local_error);
          self->do_one_attempt_id = g_timeout_add_seconds (self->request_failure_count,
                                                           do_one_attempt, self);
        }
    }
  else
    {
      /* If we advanced in state, schedule the next callback in an
       * idle so we're consistently scheduled out of an idle.
       */
      if (self->state != MCA_STATE_DONE)
        self->do_one_attempt_id = g_idle_add (do_one_attempt, self);
      else
        {
          self->do_one_attempt_id = 0;
          self->running = FALSE;
        }
    }

  return FALSE;
}

static void
recheck_metadata_reachability (MinCloudAgentApp *self)
{
  gboolean available = g_network_monitor_can_reach (self->netmon,
                                                   (GSocketConnectable*)self->addr_port,
                                                   NULL,
                                                   NULL);
  if (available == self->metadata_available)
    return;

  self->metadata_available = available;

  if (!self->metadata_available)
    {
      if (self->do_one_attempt_id)
        {
          g_source_remove (self->do_one_attempt_id);
          self->do_one_attempt_id = 0;
        }
      return;
    }
  else if (self->do_one_attempt_id == 0)
    {
      g_assert (self->metadata_available);
      g_print ("Determined metadata service is reachable\n");
      self->do_one_attempt_id = g_timeout_add_seconds (1, do_one_attempt, self);
    }
}

static void
on_network_changed (GNetworkMonitor  *monitor,
                   gboolean          available,
                   gpointer          user_data)
{
  MinCloudAgentApp *self = user_data;
  recheck_metadata_reachability (self);
}

static gboolean
prepare_root_ssh (GCancellable   *cancellable,
                  GError        **error)
{
  gboolean ret = FALSE;
  GFile *root_ssh_path = g_file_new_for_path ("/root/.ssh");

  if (!g_file_query_exists (root_ssh_path, NULL))
    {
      if (!g_file_make_directory (root_ssh_path, cancellable, error))
        goto out;

      if (!gs_file_chmod (root_ssh_path, 0700, cancellable, error))
        goto out;

      /* Ignore errors here to be simple, otherwise we'd have to link
       * to libselinux etc.
       */
      (void) gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                            cancellable, error,
                                            "restorecon",
                                            gs_file_get_path_cached (root_ssh_path),
                                            NULL);
    }

  ret = TRUE;
 out:
  return ret;
}

int
main (int argc, char **argv)
{
  MinCloudAgentApp selfstruct = { 0, };
  MinCloudAgentApp *self = &selfstruct;
  GCancellable *cancellable = NULL;
  const char *src_address;
  guint srcport = 80;

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  if (!prepare_root_ssh (cancellable, &self->error))
    goto out;

  src_address = g_getenv ("MIN_CLOUDAGENT_ADDRESS");
  if (!src_address)
    src_address = "169.254.169.254";

  {
    const char *srcport_str = g_getenv ("MIN_CLOUDAGENT_PORT");
    if (srcport_str)
      srcport = (guint) g_ascii_strtoull (srcport_str, NULL, 10);
  }

  self->addr = g_inet_address_new_from_string (src_address);
  self->addr_port = (GInetSocketAddress*)g_inet_socket_address_new (self->addr, srcport);
  self->netmon = g_network_monitor_get_default ();
  self->cancellable = cancellable;
  self->running = TRUE;
  self->session = soup_session_sync_new_with_options (SOUP_SESSION_USER_AGENT, "min-cloud-agent",
                                                      SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
                                                      SOUP_SESSION_USE_THREAD_CONTEXT, TRUE,
                                                       NULL);
  self->authorized_keys_path = g_file_new_for_path ("/root/.ssh/authorized_keys");
  self->userdata_done_stamp = g_file_new_for_path (MIN_CLOUD_AGENT_STATEDIR "/userdata.stamp");
  self->done_stamp = g_file_new_for_path (MIN_CLOUD_AGENT_STATEDIR "/done.stamp");

  if (g_file_query_exists (self->done_stamp, NULL))
    goto out;

  g_signal_connect (self->netmon, "network-changed",
                   G_CALLBACK (on_network_changed),
                   self);
  recheck_metadata_reachability (self);

  while (self->running && self->error == NULL)
    g_main_context_iteration (NULL, TRUE);
  if (self->error)
    goto out;

  if (!g_file_replace_contents (self->done_stamp, "done\n", 5,
                                NULL, FALSE, 0,
                                NULL, cancellable, &self->error))
    goto out;

 out:
  if (self->error)
    {
      g_printerr ("error: %s\n", self->error->message);
      return 1;
    }
  return 0;
}
