/****************************************************************************
 * apps/wamr_sonde/host_api_nuttx.c
 *
 * Couche hote WAMR pour NuttX : transport abstrait + metriques + identite.
 *
 * PORTABILITE : la table native_symbols est STRICTEMENT identique (noms +
 * signatures) a la version Zephyr. Le MEME .wasm s'execute sans modification.
 * Seule l'IMPLEMENTATION differe (API POSIX/NuttX au lieu des API Zephyr).
 *
 * METRIQUES REELLES :
 *   - CPU        : /proc/cpuload            (charge globale, en %)
 *   - threads    : comptage des taches sous /proc (dossiers numeriques)
 *   - stack      : /proc/<pid>/stack        (taille + usage)
 *   - free heap  : /proc/meminfo            (colonne "free")
 *   - signal dBm : ioctl SIOCGIWSENS sur wlan0 (RSSI Wi-Fi)
 *   - batterie   : lecture ADC sur /dev/adc0 + pont diviseur (0 si USB/indispo)
 *   - uptime, bytes tx/rx, transport_errors, reset_count : deja reels.
 *   Metrique laissee a 0 (non exposee par NuttX) : tcp_retransmissions.
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
#include <dirent.h>
#include <ctype.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>

#ifdef CONFIG_WIRELESS_WAPI
#include <nuttx/wireless/wireless.h>
#endif

#include "wasm_export.h"

#include "host_api_nuttx.h"
#include "deploy_nuttx.h"

/****************************************************************************
 * Identite du noeud (surchargeable via Kconfig plus tard ; en dur ici).
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

/* Interface Wi-Fi (RSSI). */
#ifndef WAMR_SONDE_WLAN_IF
#define WAMR_SONDE_WLAN_IF "wlan0"
#endif

/****************************************************************************
 * Etat interne
 ****************************************************************************/

static char     g_ip[32];
static uint32_t g_port;
static uint32_t g_sock_timeout;
static int      g_fd = -1;

static uint32_t g_bytes_tx;
static uint32_t g_bytes_rx;
static uint32_t g_tx_errors;

/* Cycle de vie. */
static uint32_t g_reset_count;
/* update_count est tenu cote main.c (deploy_update_count). */

static uint64_t g_boot_us;

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

/* Lit un petit fichier texte du procfs dans buf. Retourne le nb d'octets lus. */
static int read_proc_file(const char *path, char *buf, size_t cap)
{
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -1;
    }
  int n = read(fd, buf, cap - 1);
  close(fd);
  if (n < 0)
    {
      return -1;
    }
  buf[n] = '\0';
  return n;
}

/* Extrait un pourcentage entier depuis une chaine du type "  12.3%". */
static uint32_t parse_percent(const char *s)
{
  /* saute les espaces, lit la partie entiere avant le point/pourcent */
  while (*s == ' ' || *s == '\t')
    {
      s++;
    }
  uint32_t val = 0;
  while (*s >= '0' && *s <= '9')
    {
      val = val * 10 + (uint32_t)(*s - '0');
      s++;
    }
  if (val > 100)
    {
      val = 100;
    }
  return val;
}

void host_nuttx_init(void *pool, size_t pool_size)
{
  g_pool_base = pool;
  g_pool_size = pool_size;
  g_boot_us   = now_us();
  g_reset_count++;
}

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
 * HOST FUNCTIONS — transport abstrait (TCP/POSIX)
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
  return 0;
}

static int32_t h_transport_wait_ready(wasm_exec_env_t e, uint32_t timeout)
{
  (void)e;
  (void)timeout;
  return 0;   /* reseau monte cote hote (NETINIT) : pret d'emblee */
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

  int received = recvfrom(g_fd, buf, buf_len, 0, NULL, NULL);
  if (received > 0)
    {
      g_bytes_rx += (uint32_t)received;
    }
  else if (received < 0)
    {
      g_tx_errors++;
    }
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
 * HOST FUNCTIONS — mise a jour a chaud (hot-update)
 ****************************************************************************/

static int32_t h_poll_update(wasm_exec_env_t e)
{
  (void)e;
  deploy_try_stage();
  return (int32_t)deploy_pending();
}

static uint32_t h_update_count(wasm_exec_env_t e)
{
  (void)e;
  return deploy_update_count();
}

/****************************************************************************
 * HOST FUNCTIONS — metriques
 ****************************************************************************/

/* M1 — CPU (%) : /proc/cpuload renvoie une ligne du type "  12.3%". */
static uint32_t h_cpu_usage(wasm_exec_env_t e)
{
  (void)e;
  char buf[32];
  if (read_proc_file("/proc/cpuload", buf, sizeof(buf)) > 0)
    {
      return parse_percent(buf);
    }
  return 0;
}

/* M2 — Free heap (octets) : colonne "free" de /proc/meminfo (2e valeur). */
static uint32_t h_free_heap(wasm_exec_env_t e)
{
  (void)e;
  char buf[256];
  if (read_proc_file("/proc/meminfo", buf, sizeof(buf)) <= 0)
    {
      return (uint32_t)g_pool_size;   /* repli */
    }

  /* Format :
   *       total       used       free  ...
   *      142600      74536      68064  ...
   * On saute la 1re ligne (en-tete), puis on lit la 3e colonne de la 2e ligne.
   */
  char *nl = strchr(buf, '\n');
  if (!nl)
    {
      return (uint32_t)g_pool_size;
    }
  char *p = nl + 1;

  /* lire total (1), used (2), free (3) */
  unsigned long vals[3] = {0, 0, 0};
  int idx = 0;
  while (idx < 3 && *p)
    {
      while (*p == ' ' || *p == '\t')
        {
          p++;
        }
      if (*p < '0' || *p > '9')
        {
          break;
        }
      unsigned long v = 0;
      while (*p >= '0' && *p <= '9')
        {
          v = v * 10 + (unsigned long)(*p - '0');
          p++;
        }
      vals[idx++] = v;
    }
  if (idx >= 3)
    {
      return (uint32_t)vals[2];   /* free */
    }
  return (uint32_t)g_pool_size;
}

/* M3 — Uptime (ms). */
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

/* M7 — occupation de pile (%) de la tache courante : /proc/self/stack.
 * Format attendu (colonnes) : StackBase StackSize StackUsed ...
 * On calcule used*100/size. Si indisponible, 0.
 */
static uint32_t h_stack_usage_pct(wasm_exec_env_t e)
{
  (void)e;
  char buf[256];
  if (read_proc_file("/proc/self/stack", buf, sizeof(buf)) <= 0)
    {
      return 0;
    }

  /* /proc/self/stack donne des lignes "Nom:  valeur". On cherche
   * "StackSize" et "StackUsed" (noms selon la version de NuttX). */
  unsigned long size = 0;
  unsigned long used = 0;
  char *line = buf;
  while (line && *line)
    {
      char *nl = strchr(line, '\n');
      if (nl)
        {
          *nl = '\0';
        }

      if (strstr(line, "StackSize"))
        {
          char *c = strchr(line, ':');
          if (c)
            {
              size = strtoul(c + 1, NULL, 10);
            }
        }
      else if (strstr(line, "StackUsed") || strstr(line, "StackUsage"))
        {
          char *c = strchr(line, ':');
          if (c)
            {
              used = strtoul(c + 1, NULL, 10);
            }
        }

      line = nl ? nl + 1 : NULL;
    }

  if (size > 0 && used <= size)
    {
      return (uint32_t)((used * 100UL) / size);
    }
  return 0;
}

/* M9 — signal dBm : RSSI Wi-Fi via ioctl SIOCGIWSENS sur wlan0. */
static int32_t h_signal_dbm(wasm_exec_env_t e)
{
  (void)e;
#ifdef CONFIG_WIRELESS_WAPI
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    {
      return 0;
    }

  struct iwreq req;
  memset(&req, 0, sizeof(req));
  strncpy(req.ifr_name, WAMR_SONDE_WLAN_IF, IFNAMSIZ - 1);

  int32_t dbm = 0;
  if (ioctl(sock, SIOCGIWSENS, (unsigned long)&req) >= 0)
    {
      /* sensibilite renvoyee dans req.u.sens.value ; signe selon la version. */
      int v = (int)req.u.sens.value;
      dbm = (v > 0) ? -v : v;   /* normalise en dBm negatif */
    }
  close(sock);
  return dbm;
#else
  return 0;
#endif
}

static uint32_t h_reset_count(wasm_exec_env_t e)
{
  (void)e;
  return g_reset_count;
}

/* M11 — threads actifs : compte les dossiers numeriques sous /proc. */
static uint32_t h_active_threads(wasm_exec_env_t e)
{
  (void)e;
  DIR *d = opendir("/proc");
  if (!d)
    {
      return 1;
    }
  uint32_t count = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL)
    {
      /* un dossier de tache = nom entierement numerique */
      const char *n = ent->d_name;
      if (*n == '\0')
        {
          continue;
        }
      bool numeric = true;
      for (const char *p = n; *p; p++)
        {
          if (*p < '0' || *p > '9')
            {
              numeric = false;
              break;
            }
        }
      if (numeric)
        {
          count++;
        }
    }
  closedir(d);
  return (count > 0) ? count : 1;
}

/* M12 — retransmissions TCP : non expose simplement par NuttX (niveau 3). */
static uint32_t h_tcp_retransmissions(wasm_exec_env_t e)
{
  (void)e;
  return 0;
}

/* M13 — batterie (mV) : lecture ADC sur /dev/adc0.
 *
 * Lit le canal ADC cable sur la tension batterie, convertit la valeur brute en
 * millivolts, puis applique le facteur du pont diviseur de la carte.
 *
 * NOTE MATERIELLE (Heltec WiFi LoRa 32 v3) : la tension batterie arrive sur
 * GPIO1 (ADC1 canal 0) via un pont diviseur (~x4.9 sur cette carte), et le
 * circuit de mesure doit etre active en mettant GPIO37 a l'etat BAS. Le
 * controle de GPIO37 n'est PAS fait ici (il depend d'un driver GPIO cote
 * board) : si la carte requiert cette activation, la lecture peut etre nulle
 * tant que GPIO37 n'est pas pilote. Voir README (section Batterie).
 *
 * Comportement : renvoie 0 si l'ADC n'est pas disponible ou la lecture echoue
 * (ex. alimentation USB sans batterie). 0 desactive le gating energetique de
 * PADRE (pas de mode survie force), exactement comme sous Zephyr.
 */

/* Facteur du pont diviseur : Vbatt = Vadc * BATT_DIVIDER_NUM / BATT_DIVIDER_DEN.
 * Ajuster selon la carte. Heltec v3 : ~4.9 (approxime ici par 490/100). */
#ifndef BATT_DIVIDER_NUM
#define BATT_DIVIDER_NUM 490
#endif
#ifndef BATT_DIVIDER_DEN
#define BATT_DIVIDER_DEN 100
#endif

/* Canal ADC de la batterie (index dans la sequence lue sur /dev/adc0). */
#ifndef BATT_ADC_CHANNEL
#define BATT_ADC_CHANNEL 0
#endif

/* Reference ADC (mV) pour convertir la valeur brute. La resolution effective
 * depend de la config ADC ; on suppose 12 bits (0..4095) sur 3100 mV par
 * defaut. Ajuster si la calibration differe. */
#ifndef BATT_ADC_VREF_MV
#define BATT_ADC_VREF_MV 3100
#endif
#ifndef BATT_ADC_MAX_RAW
#define BATT_ADC_MAX_RAW 4095
#endif

static uint32_t h_battery_mv(wasm_exec_env_t e)
{
  (void)e;

  int fd = open("/dev/adc0", O_RDONLY);
  if (fd < 0)
    {
      return 0;   /* ADC indisponible -> batterie non mesuree */
    }

  /* Declenche une conversion si le driver l'exige (ioctl ANIOC_TRIGGER).
   * Ignore l'erreur : certains drivers convertissent en continu. */
#ifdef ANIOC_TRIGGER
  ioctl(fd, ANIOC_TRIGGER, 0);
#endif

  /* Lit un lot d'echantillons ; chaque echantillon = {channel, data}. */
  struct adc_msg_s sample[8];
  ssize_t n = read(fd, sample, sizeof(sample));
  close(fd);

  if (n < (ssize_t)sizeof(struct adc_msg_s))
    {
      return 0;   /* rien lu */
    }

  size_t count = (size_t)n / sizeof(struct adc_msg_s);
  int32_t raw = -1;
  for (size_t i = 0; i < count; i++)
    {
      if (sample[i].am_channel == BATT_ADC_CHANNEL)
        {
          raw = (int32_t)sample[i].am_data;
          break;
        }
    }
  if (raw < 0)
    {
      /* canal non trouve : prend le premier echantillon par defaut */
      raw = (int32_t)sample[0].am_data;
    }
  if (raw < 0)
    {
      return 0;
    }

  /* Conversion brute -> mV a l'ADC, puis compensation du pont diviseur. */
  uint32_t v_adc = (uint32_t)((int64_t)raw * BATT_ADC_VREF_MV / BATT_ADC_MAX_RAW);
  uint32_t v_batt = v_adc * BATT_DIVIDER_NUM / BATT_DIVIDER_DEN;
  return v_batt;
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
  return copy_id(buf, cap, "wifi");
}

/****************************************************************************
 * TABLE DES SYMBOLES NATIFS (identique a Zephyr et au contrat ffi.rs).
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