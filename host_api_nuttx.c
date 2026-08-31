/****************************************************************************
 * Couche hote WAMR pour NuttX : transport abstrait + metriques + identite
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "wasm_export.h"

#include "host_api_nuttx.h"

/****************************************************************************
 * Identite du noeud (surchargeable via Kconfig plus tard ; en dur ici pour
 * ce jalon). Ces chaines sont exposees au module WASM a l'execution.
 ****************************************************************************/

#ifndef WAMR_SONDE_DEVICE_NAME
#define WAMR_SONDE_DEVICE_NAME "nuttx-esp32s3"
#endif
#ifndef WAMR_SONDE_DEVICE_TYPE
#define WAMR_SONDE_DEVICE_TYPE "esp32s3"
#endif
#ifndef WAMR_SONDE_OS_NAME
#define WAMR_SONDE_OS_NAME "nuttx"
#endif

/****************************************************************************
 * Etat interne
 ****************************************************************************/

/* Cible TCP memorisee entre transport_connect() et le premier envoi. */
static char     g_ip[32];
static uint32_t g_port;
static uint32_t g_sock_timeout;
static int      g_fd = -1;

/* Compteurs de transmission (equivalents M4/M5/M6 Zephyr). */
static uint32_t g_bytes_tx;
static uint32_t g_bytes_rx;
static uint32_t g_tx_errors;

/* Cycle de vie. */
static uint32_t g_reset_count;
static uint32_t g_update_count;   /* mises a jour a chaud (jalon ulterieur) */

/* Base de temps pour uptime_ms. */
static uint64_t g_boot_us;

/* Pool WAMR (resolution de pointeur en dernier recours, comme Zephyr). */
static void   *g_pool_base;
static size_t  g_pool_size;

/****************************************************************************
 * Utilitaires
 ****************************************************************************/

static uint64_t now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void host_nuttx_init(void *pool, size_t pool_size)
{
  g_pool_base = pool;
  g_pool_size = pool_size;
  g_boot_us   = now_us();
  g_reset_count++;   /* une fois par demarrage, comme Zephyr */
}

/* Resolution pointeur WASM -> natif (identique a la logique Zephyr). */
static void *app_ptr(wasm_module_inst_t inst, uint32_t app_offset, uint32_t len)
{
  if (app_offset == 0 || len == 0)
    {
      return NULL;
    }
  if (wasm_runtime_validate_app_addr(inst, app_offset, len))
    {
      return wasm_runtime_addr_app_to_native(inst, app_offset);
    }
  uintptr_t v = (uintptr_t)app_offset;
  if (g_pool_base &&
      v >= (uintptr_t)g_pool_base &&
      v + len <= (uintptr_t)g_pool_base + g_pool_size)
    {
      return (void *)v;
    }
  return NULL;
}

/****************************************************************************
 * HOST FUNCTIONS — affichage
 ****************************************************************************/

static void h_print(wasm_exec_env_t e, char *msg, uint32_t len)
{
  (void)e;
  if (!msg || len == 0)
    {
      return;
    }
  char buf[192];
  uint32_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, msg, n);
  buf[n] = '\0';
  fputs(buf, stdout);
  fflush(stdout);
}

/****************************************************************************
 * HOST FUNCTIONS — transport abstrait (TCP/POSIX pour ce jalon)
 *
 * Modele identique a Zephyr : connect memorise la cible, le socket est ouvert
 * au premier envoi, un socket par trame (ouvert a l'envoi, ferme apres l'ACK).
 ****************************************************************************/

static int32_t h_transport_connect(wasm_exec_env_t e, uint32_t ip_ptr,
                                   uint32_t ip_len, uint32_t port,
                                   uint32_t timeout)
{
  wasm_module_inst_t inst = wasm_runtime_get_module_inst(e);
  const char *ip = (const char *)app_ptr(inst, ip_ptr, ip_len);

  uint32_t n = ip_len < sizeof(g_ip) - 1 ? ip_len : sizeof(g_ip) - 1;
  if (ip)
    {
      memcpy(g_ip, ip, n);
      g_ip[n] = '\0';
    }
  else
    {
      g_ip[0] = '\0';
    }
  g_port = port;
  g_sock_timeout = timeout;
  return 0;   /* liaison logique prete ; le socket s'ouvre au 1er envoi */
}

static int32_t h_transport_wait_ready(wasm_exec_env_t e, uint32_t timeout)
{
  (void)e;
  (void)timeout;
  /* Sous NuttX, le reseau est monte cote hote (voir main). Pret d'emblee. */
  return 0;
}

static int ensure_socket(void)
{
  if (g_fd >= 0)
    {
      return g_fd;
    }

  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    {
      g_tx_errors++;
      return -1;
    }

  struct timeval tv;
  tv.tv_sec  = g_sock_timeout;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons((uint16_t)g_port);
  if (inet_pton(AF_INET, g_ip, &addr.sin_addr) != 1)
    {
      close(fd);
      g_tx_errors++;
      return -1;
    }
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      close(fd);
      g_tx_errors++;
      return -1;
    }
  g_fd = fd;
  return g_fd;
}

static int32_t h_transport_send(wasm_exec_env_t e, int32_t handle,
                                uint32_t buf_ptr, uint32_t buf_len)
{
  (void)handle;
  wasm_module_inst_t inst = wasm_runtime_get_module_inst(e);
  const uint8_t *buf = (const uint8_t *)app_ptr(inst, buf_ptr, buf_len);
  if (!buf)
    {
      return -1;
    }

  int fd = ensure_socket();
  if (fd < 0)
    {
      return -1;
    }

  uint32_t total = 0;
  while (total < buf_len)
    {
      int n = send(fd, buf + total, buf_len - total, 0);
      if (n <= 0)
        {
          g_tx_errors++;
          close(g_fd);
          g_fd = -1;
          return (total > 0) ? (int32_t)total : -1;
        }
      total += (uint32_t)n;
    }
  g_bytes_tx += total;
  return (int32_t)total;
}

static int32_t h_transport_recv(wasm_exec_env_t e, int32_t handle,
                                uint32_t buf_ptr, uint32_t buf_len)
{
  (void)handle;
  wasm_module_inst_t inst = wasm_runtime_get_module_inst(e);
  uint8_t *buf = (uint8_t *)app_ptr(inst, buf_ptr, buf_len);
  if (!buf || g_fd < 0)
    {
      return -1;
    }

  int received = recv(g_fd, buf, buf_len, 0);
  if (received > 0)
    {
      g_bytes_rx += (uint32_t)received;
    }
  else if (received < 0)
    {
      g_tx_errors++;
    }
  /* Un socket par trame, comme Zephyr : on ferme apres l'ACK. */
  close(g_fd);
  g_fd = -1;
  return received;
}

static void h_transport_close(wasm_exec_env_t e, int32_t handle)
{
  (void)e;
  (void)handle;
  if (g_fd >= 0)
    {
      close(g_fd);
      g_fd = -1;
    }
}

static void h_sleep(wasm_exec_env_t e, uint32_t secs)
{
  (void)e;
  if (secs > 0)
    {
      sleep(secs);
    }
}

/****************************************************************************
 * HOST FUNCTIONS — mise a jour a chaud (jalon ulterieur)
 * Pour ce jalon, aucune mise a jour n'est detectee : la sonde tourne jusqu'au
 * reset. host_poll_update renvoie donc toujours 0.
 ****************************************************************************/

static int32_t h_poll_update(wasm_exec_env_t e)
{
  (void)e;
  return 0;   /* pas de hot-update sur ce jalon NuttX */
}

static uint32_t h_update_count(wasm_exec_env_t e)
{
  (void)e;
  return g_update_count;
}

/****************************************************************************
 * HOST FUNCTIONS — metriques
 *
 * Pleinement implementees : uptime, free_heap, bytes_tx/rx, transport_errors,
 * reset_count. Neutres (0) pour ce jalon : cpu, stack, active_threads, signal,
 * tcp_retransmissions, battery.
 ****************************************************************************/

static uint32_t h_cpu_usage(wasm_exec_env_t e)
{
  (void)e;
  return 0;   /* JALON : a raffiner (NuttX /proc ou API scheduler) */
}

static uint32_t h_free_heap(wasm_exec_env_t e)
{
  (void)e;
  /* Approximation portable : taille du pool WAMR. Raffinable via mallinfo(). */
  return (uint32_t)g_pool_size;
}

static uint32_t h_uptime_ms(wasm_exec_env_t e)
{
  (void)e;
  uint64_t d = now_us() - g_boot_us;
  return (uint32_t)((d / 1000ULL) & 0xFFFFFFFFULL);
}

static uint32_t h_bytes_tx(wasm_exec_env_t e)
{
  (void)e;
  return g_bytes_tx;
}

static uint32_t h_bytes_rx(wasm_exec_env_t e)
{
  (void)e;
  return g_bytes_rx;
}

static uint32_t h_transport_errors(wasm_exec_env_t e)
{
  (void)e;
  return g_tx_errors;
}

static uint32_t h_stack_usage_pct(wasm_exec_env_t e)
{
  (void)e;
  return 0;   /* JALON : a raffiner */
}

static int32_t h_signal_dbm(wasm_exec_env_t e)
{
  (void)e;
  return 0;   /* JALON : a raffiner (RSSI Wi-Fi NuttX) */
}

static uint32_t h_reset_count(wasm_exec_env_t e)
{
  (void)e;
  return g_reset_count;
}

static uint32_t h_active_threads(wasm_exec_env_t e)
{
  (void)e;
  return 1;   /* JALON : a raffiner */
}

static uint32_t h_tcp_retransmissions(wasm_exec_env_t e)
{
  (void)e;
  return 0;   /* JALON : a raffiner */
}

static uint32_t h_battery_mv(wasm_exec_env_t e)
{
  (void)e;
  return 0;   /* pas de mesure batterie : gating energetique inactif */
}

/****************************************************************************
 * HOST FUNCTIONS — identite (resolue a l'execution)
 ****************************************************************************/

static int32_t copy_id(char *buf, uint32_t cap, const char *val)
{
  if (!buf)
    {
      return -1;
    }
  size_t len = strlen(val);
  if (cap < len)
    {
      return -1;
    }
  memcpy(buf, val, len);
  return (int32_t)len;
}

static int32_t h_get_device_name(wasm_exec_env_t e, char *buf, uint32_t cap)
{
  (void)e;
  return copy_id(buf, cap, WAMR_SONDE_DEVICE_NAME);
}

static int32_t h_get_device_type(wasm_exec_env_t e, char *buf, uint32_t cap)
{
  (void)e;
  return copy_id(buf, cap, WAMR_SONDE_DEVICE_TYPE);
}

static int32_t h_get_os_name(wasm_exec_env_t e, char *buf, uint32_t cap)
{
  (void)e;
  return copy_id(buf, cap, WAMR_SONDE_OS_NAME);
}

static int32_t h_get_transport_name(wasm_exec_env_t e, char *buf, uint32_t cap)
{
  (void)e;
  return copy_id(buf, cap, "wifi");   /* transport TCP/Wi-Fi pour ce jalon */
}

/****************************************************************************
 * TABLE DES SYMBOLES NATIFS
 * IDENTIQUE (noms + signatures) a la version Zephyr et au contrat ffi.rs.
 ****************************************************************************/

static NativeSymbol g_native_symbols[] =
{
  { "host_print",                 h_print,                 "(*~)",    NULL },

  { "host_transport_connect",     h_transport_connect,     "(iiii)i", NULL },
  { "host_transport_wait_ready",  h_transport_wait_ready,  "(i)i",    NULL },
  { "host_transport_send",        h_transport_send,        "(iii)i",  NULL },
  { "host_transport_recv",        h_transport_recv,        "(iii)i",  NULL },
  { "host_transport_close",       h_transport_close,       "(i)",     NULL },
  { "host_sleep",                 h_sleep,                 "(i)",     NULL },

  { "host_poll_update",           h_poll_update,           "()i",     NULL },
  { "host_metric_update_count",   h_update_count,          "()i",     NULL },

  { "host_metric_cpu_usage",         h_cpu_usage,         "()i", NULL },
  { "host_metric_free_heap",         h_free_heap,         "()i", NULL },
  { "host_metric_uptime_ms",         h_uptime_ms,         "()i", NULL },
  { "host_metric_bytes_tx",          h_bytes_tx,          "()i", NULL },
  { "host_metric_bytes_rx",          h_bytes_rx,          "()i", NULL },
  { "host_metric_transport_errors",  h_transport_errors,  "()i", NULL },
  { "host_metric_stack_usage_pct",   h_stack_usage_pct,   "()i", NULL },
  { "host_metric_signal_dbm",        h_signal_dbm,        "()i", NULL },
  { "host_metric_reset_count",       h_reset_count,       "()i", NULL },
  { "host_metric_active_threads",       h_active_threads,       "()i", NULL },
  { "host_metric_tcp_retransmissions",  h_tcp_retransmissions,  "()i", NULL },
  { "host_metric_battery_mv",           h_battery_mv,           "()i", NULL },

  { "host_get_device_name",       h_get_device_name,       "(*~)i", NULL },
  { "host_get_device_type",       h_get_device_type,       "(*~)i", NULL },
  { "host_get_os_name",           h_get_os_name,           "(*~)i", NULL },
  { "host_get_transport_name",    h_get_transport_name,    "(*~)i", NULL },
};

bool host_register_natives(void)
{
  uint32_t n = (uint32_t)(sizeof(g_native_symbols) /
                          sizeof(g_native_symbols[0]));
  if (!wasm_runtime_register_natives("env", g_native_symbols, n))
    {
      printf("[wamr] echec d'enregistrement des symboles natifs\n");
      return false;
    }
  printf("[wamr] %u symboles natifs enregistres\n", (unsigned)n);
  return true;
}