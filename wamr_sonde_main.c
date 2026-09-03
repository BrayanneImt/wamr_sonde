/****************************************************************************
 * apps/wamr_sonde/wamr_sonde_main.c
 *
 * Firmware hote NuttX pour la sonde de supervision SPIREC.
 *
 * RECEPTION RESEAU + MISE A JOUR A CHAUD
 * --------------------------------------
 * NuttX monte le Wi-Fi automatiquement au demarrage (NETINIT + WAPI + DHCP).
 * L'application ouvre un serveur TCP sur le port 5555, qui reste OUVERT en permanence :
 *   - il sert a la reception INITIALE du module (accept bloquant) ;
 *   - puis, pendant l'execution, a la detection d'une MISE A JOUR (accept non
 *     bloquant via deploy_try_stage, appele par host_poll_update).
 *
 * Hot-update :
 *   la sonde appelle host_poll_update -> deploy_try_stage ; si un nouveau .wasm
 *   arrive, il est recu dans un SECOND buffer (staging) et un drapeau est leve ;
 *   la sonde rend la main proprement (return) ; le firmware detruit l'instance,
 *   bascule staging -> courant (echange de pointeurs), incremente update_count,
 *   et execute le nouveau module. Jamais deux INSTANCES WASM simultanees.
 *
 * Le module .wasm est INCHANGE : meme binaire que sous Zephyr.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "wasm_export.h"
#include "host_api_nuttx.h"
#include "deploy_nuttx.h"

/****************************************************************************
 * Reglages (via Kconfig, valeurs de repli sinon)
 ****************************************************************************/

#ifndef CONFIG_WAMR_SONDE_WASM_MAX_KB
#define CONFIG_WAMR_SONDE_WASM_MAX_KB 12
#endif
#ifndef CONFIG_WAMR_SONDE_POOL_KB
#define CONFIG_WAMR_SONDE_POOL_KB 96
#endif
#ifndef CONFIG_WAMR_SONDE_STACK_KB
#define CONFIG_WAMR_SONDE_STACK_KB 8
#endif
#ifndef CONFIG_WAMR_SONDE_HEAP_KB
#define CONFIG_WAMR_SONDE_HEAP_KB 16
#endif

#define UPLOAD_TCP_PORT 5555
#define POLL_MS         50     /* sondage non bloquant d'un client (ms) */
#define RECV_TIMEOUT_S  15     /* delai max de reception d'un transfert */

#define WASM_MAX_SIZE  (CONFIG_WAMR_SONDE_WASM_MAX_KB * 1024)
#define POOL_SIZE      (CONFIG_WAMR_SONDE_POOL_KB     * 1024)
#define STACK_SIZE     (CONFIG_WAMR_SONDE_STACK_KB    * 1024)
#define HEAP_SIZE      (CONFIG_WAMR_SONDE_HEAP_KB     * 1024)

/* --- Deux buffers d'octets (module courant + module en attente) ---
 * On echange les pointeurs a chaque mise a jour : pas de copie. Seuls des
 * OCTETS coexistent, jamais deux instances WASM.
 */
static uint8_t g_buf_a[WASM_MAX_SIZE];
static uint8_t g_buf_b[WASM_MAX_SIZE];
static uint8_t *g_cur   = g_buf_a;   /* module en cours d'execution */
static uint8_t *g_stage = g_buf_b;   /* module recu, en attente     */
static uint32_t g_cur_size;
static uint32_t g_stage_size;
static volatile bool g_pending;      /* un module attend d'etre charge */
static uint32_t g_update_count;      /* nb de mises a jour a chaud     */

static char g_wamr_pool[POOL_SIZE] __attribute__((aligned(8)));

/* Socket d'ecoute, ouvert en permanence. */
static int g_listen_fd = -1;

/****************************************************************************
 * Reception reseau : protocole [taille(4 LE)][binaire]
 ****************************************************************************/

static int recv_all(int fd, uint8_t *buf, uint32_t n)
{
  uint32_t got = 0;
  while (got < n)
    {
      int r = recvfrom(fd, buf + got, n - got, 0, NULL, NULL);
      if (r > 0)
        {
          got += (uint32_t)r;
        }
      else if (r < 0 && errno == EINTR)
        {
          continue;
        }
      else
        {
          return -1;
        }
    }
  return 0;
}

/* Recoit un module complet (deja accepte) dans `dst`. Retourne la taille, ou 0. */
static uint32_t receive_into(int cfd, uint8_t *dst, const char *tag)
{
  struct timeval tv;
  tv.tv_sec  = RECV_TIMEOUT_S;
  tv.tv_usec = 0;
  setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  uint8_t hdr[4];
  if (recv_all(cfd, hdr, 4) != 0)
    {
      printf("[net] %s : taille non recue\n", tag);
      return 0;
    }
  uint32_t size = (uint32_t)hdr[0]        |
                  ((uint32_t)hdr[1] << 8) |
                  ((uint32_t)hdr[2] << 16)|
                  ((uint32_t)hdr[3] << 24);

  if (size == 0 || size > WASM_MAX_SIZE)
    {
      printf("[net] %s : taille invalide %u (max %u)\n",
             tag, (unsigned)size, (unsigned)WASM_MAX_SIZE);
      return 0;
    }

  if (recv_all(cfd, dst, size) != 0)
    {
      printf("[net] %s : transfert incomplet\n", tag);
      return 0;
    }

  send(cfd, "OK", 2, 0);
  printf("[net] %s : module recu (%u octets)\n", tag, (unsigned)size);
  return size;
}

/* Ouvre le socket d'ecoute TCP (permanent). Retourne 0 si OK, -1 sinon. */
static int upload_listen_init(uint16_t port)
{
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    {
      printf("[net] creation du socket d'ecoute echouee (errno=%d)\n", errno);
      return -1;
    }

  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons(port);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      printf("[net] bind sur le port %u echoue (errno=%d)\n", port, errno);
      close(fd);
      return -1;
    }
  if (listen(fd, 1) < 0)
    {
      printf("[net] listen echoue (errno=%d)\n", errno);
      close(fd);
      return -1;
    }

  g_listen_fd = fd;
  printf("[net] serveur d'upload en ecoute sur le port TCP %u\n", port);
  return 0;
}

/* Reception INITIALE : accept BLOQUANT, module recu dans g_cur. Retourne la
 * taille, ou 0. */
static uint32_t upload_receive_initial(void)
{
  struct sockaddr_in cli;
  socklen_t clen = sizeof(cli);

  int cfd = accept(g_listen_fd, (struct sockaddr *)&cli, &clen);
  if (cfd < 0)
    {
      return 0;
    }
  printf("[net] client connecte (%s)\n", inet_ntoa(cli.sin_addr));

  uint32_t size = receive_into(cfd, g_cur, "initial");
  close(cfd);
  if (size > 0)
    {
      g_cur_size = size;
    }
  return size;
}

/****************************************************************************
 * Contrat de deploiement (declare dans deploy_nuttx.h, appele par host_api).
 *
 * deploy_try_stage : accept NON bloquant (via poll) ; si un client se presente,
 * recoit le module dans g_stage et leve g_pending.
 ****************************************************************************/

int deploy_try_stage(void)
{
  if (g_pending)
    {
      return 1;   /* deja un module en attente : on n'en recoit pas un 2e */
    }
  if (g_listen_fd < 0)
    {
      return 0;
    }

  /* Un client est-il en attente ? (sondage court, non bloquant) */
  struct pollfd pfd;
  pfd.fd     = g_listen_fd;
  pfd.events = POLLIN;
  int pr = poll(&pfd, 1, POLL_MS);
  if (pr <= 0 || !(pfd.revents & POLLIN))
    {
      return 0;   /* aucun client */
    }

  struct sockaddr_in cli;
  socklen_t clen = sizeof(cli);
  int cfd = accept(g_listen_fd, (struct sockaddr *)&cli, &clen);
  if (cfd < 0)
    {
      return 0;
    }
  printf("[net] client connecte pour MAJ (%s)\n", inet_ntoa(cli.sin_addr));

  uint32_t size = receive_into(cfd, g_stage, "maj");
  close(cfd);
  if (size == 0)
    {
      return 0;
    }

  g_stage_size = size;
  g_pending    = true;
  return 1;
}

int deploy_pending(void)
{
  return g_pending ? 1 : 0;
}

uint32_t deploy_update_count(void)
{
  return g_update_count;
}

/****************************************************************************
 * Execution du module WASM (sequence identique a Zephyr).
 * Retourne true si le module a ete charge (execute), false si le CHARGEMENT a
 * echoue (module invalide) -> repli.
 ****************************************************************************/

static bool execute_wasm(uint8_t *wasm_data, uint32_t wasm_size)
{
  char error_buf[128];
  wasm_module_t        module   = NULL;
  wasm_module_inst_t   inst     = NULL;
  wasm_exec_env_t      exec_env = NULL;
  wasm_function_inst_t func     = NULL;
  bool loaded = false;

  printf("[wamr] chargement du module...\n");
  module = wasm_runtime_load(wasm_data, wasm_size, error_buf, sizeof(error_buf));
  if (!module)
    {
      printf("[wamr] LOAD ERROR: %s\n", error_buf);
      return false;
    }
  loaded = true;
  printf("[wamr] module charge\n");

  inst = wasm_runtime_instantiate(module, STACK_SIZE, HEAP_SIZE,
                                  error_buf, sizeof(error_buf));
  if (!inst)
    {
      printf("[wamr] INSTANTIATE ERROR: %s\n", error_buf);
      wasm_runtime_unload(module);
      return loaded;
    }
  printf("[wamr] instance creee\n");

  exec_env = wasm_runtime_create_exec_env(inst, STACK_SIZE);
  if (!exec_env)
    {
      printf("[wamr] echec de creation de l'exec_env\n");
      wasm_runtime_deinstantiate(inst);
      wasm_runtime_unload(module);
      return loaded;
    }

  func = wasm_runtime_lookup_function(inst, "main");
  if (!func)
    {
      func = wasm_runtime_lookup_function(inst, "_start");
    }
  if (!func)
    {
      printf("[wamr] point d'entree introuvable (main/_start)\n");
    }
  else
    {
      printf("[wamr] execution du module...\n");
      if (!wasm_runtime_call_wasm(exec_env, func, 0, NULL))
        {
          printf("[wamr] EXCEPTION: %s\n", wasm_runtime_get_exception(inst));
        }
      else
        {
          printf("[wamr] execution terminee\n");
        }
    }

  wasm_runtime_destroy_exec_env(exec_env);
  wasm_runtime_deinstantiate(inst);
  wasm_runtime_unload(module);
  return loaded;
}

/* Execute g_cur ; a la sortie de la sonde, si un module est en attente, bascule
 * dessus (echange de pointeurs) et recommence. Sinon, rend la main. */
static void run_with_hot_update(void)
{
  bool keep = true;
  while (keep)
    {
      bool loaded = execute_wasm(g_cur, g_cur_size);

      if (g_pending)
        {
          uint8_t *tmp = g_cur;
          g_cur   = g_stage;
          g_stage = tmp;
          g_cur_size = g_stage_size;
          g_pending  = false;
          g_update_count++;
          printf("\n[deploy] MISE A JOUR A CHAUD #%u (%u octets)\n",
                 (unsigned)g_update_count, (unsigned)g_cur_size);
        }
      else if (!loaded)
        {
          printf("[deploy] module invalide, retour en attente\n");
          keep = false;
        }
      else
        {
          printf("\n[boot] en attente d'un nouveau module...\n");
          keep = false;
        }
    }
}

/****************************************************************************
 * Point d'entree
 ****************************************************************************/

int main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  printf("\n");
  printf("========================================================\n");
  printf(" Firmware hote WAMR sur NuttX — sonde SPIREC (hot-update)\n");
  printf("========================================================\n");

  RuntimeInitArgs init_args;
  memset(&init_args, 0, sizeof(init_args));
  init_args.mem_alloc_type                  = Alloc_With_Pool;
  init_args.mem_alloc_option.pool.heap_buf  = g_wamr_pool;
  init_args.mem_alloc_option.pool.heap_size = sizeof(g_wamr_pool);

  if (!wasm_runtime_full_init(&init_args))
    {
      printf("[wamr] echec d'initialisation du runtime\n");
      return -1;
    }
  printf("[wamr] runtime initialise (pool=%d Ko)\n", POOL_SIZE / 1024);

  host_nuttx_init(g_wamr_pool, sizeof(g_wamr_pool));
  if (!host_register_natives())
    {
      wasm_runtime_destroy();
      return -1;
    }

  if (upload_listen_init(UPLOAD_TCP_PORT) != 0)
    {
      wasm_runtime_destroy();
      return -1;
    }

  printf("===== DEPLOYMENT — SPIREC (NuttX / reseau + hot-update) =====\n");
  printf("Reception : TCP port %d  [taille(4 LE) + binaire]\n", UPLOAD_TCP_PORT);
  printf("Taille max : %d octets\n", WASM_MAX_SIZE);
  printf("Pousser / mettre a jour depuis le PC :\n");
  printf("  python3 upload_net.py <ip_carte> metrics.wasm\n");

  /* Boucle : attendre le PREMIER module, puis executer avec hot-update. */
  for (;;)
    {
      uint32_t size = upload_receive_initial();
      if (size > 0)
        {
          run_with_hot_update();
        }
      /* upload_receive_initial bloque sur accept() : pas de busy-wait. */
    }

  close(g_listen_fd);
  wasm_runtime_destroy();
  return 0;
}