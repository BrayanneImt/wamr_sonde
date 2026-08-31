/****************************************************************************
 * apps/wamr_sonde/wamr_sonde_main.c
 *
 * Firmware hote NuttX pour la sonde de supervision
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
#include <fcntl.h>
#include <errno.h>

#include "wasm_export.h"
#include "host_api_nuttx.h"

/****************************************************************************
 * Reglages (via Kconfig, valeurs de repli sinon)
 ****************************************************************************/

#ifndef CONFIG_WAMR_SONDE_UART_DEV
#define CONFIG_WAMR_SONDE_UART_DEV "/dev/console"
#endif
#ifndef CONFIG_WAMR_SONDE_WASM_MAX_KB
#define CONFIG_WAMR_SONDE_WASM_MAX_KB 32
#endif
#ifndef CONFIG_WAMR_SONDE_POOL_KB
#define CONFIG_WAMR_SONDE_POOL_KB 160
#endif
#ifndef CONFIG_WAMR_SONDE_STACK_KB
#define CONFIG_WAMR_SONDE_STACK_KB 8
#endif
#ifndef CONFIG_WAMR_SONDE_HEAP_KB
#define CONFIG_WAMR_SONDE_HEAP_KB 16
#endif

#define WASM_MAX_SIZE  (CONFIG_WAMR_SONDE_WASM_MAX_KB * 1024)
#define POOL_SIZE      (CONFIG_WAMR_SONDE_POOL_KB     * 1024)
#define STACK_SIZE     (CONFIG_WAMR_SONDE_STACK_KB    * 1024)
#define HEAP_SIZE      (CONFIG_WAMR_SONDE_HEAP_KB     * 1024)

/* Buffers statiques (pas d'allocation dynamique cote hote). */
static uint8_t g_wasm_buffer[WASM_MAX_SIZE];
static char    g_wamr_pool[POOL_SIZE] __attribute__((aligned(8)));

/****************************************************************************
 * Reception UART : protocole [taille(4 octets LE)][binaire]
 *
 * Lecture bloquante octet par octet sur le descripteur serie. Identique au
 * protocole Zephyr et compatible avec upload.py (aucun changement cote PC).
 ****************************************************************************/

static int read_full(int fd, uint8_t *buf, uint32_t n)
{
  uint32_t got = 0;
  while (got < n)
    {
      int r = read(fd, buf + got, n - got);
      if (r > 0)
        {
          got += (uint32_t)r;
        }
      else if (r < 0 && errno == EINTR)
        {
          continue;
        }
      else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
          usleep(1000);
          continue;
        }
      else
        {
          return -1;   /* EOF ou erreur */
        }
    }
  return 0;
}

/* Recoit un module complet dans g_wasm_buffer. Retourne la taille, ou 0. */
static uint32_t uart_receive_module(int fd)
{
  uint8_t hdr[4];
  printf("[uart] en attente d'un module .wasm...\n");

  if (read_full(fd, hdr, 4) != 0)
    {
      printf("[uart] echec de lecture de la taille\n");
      return 0;
    }

  uint32_t size = (uint32_t)hdr[0]        |
                  ((uint32_t)hdr[1] << 8) |
                  ((uint32_t)hdr[2] << 16)|
                  ((uint32_t)hdr[3] << 24);

  printf("[uart] taille annoncee = %u octets\n", (unsigned)size);
  if (size == 0 || size > WASM_MAX_SIZE)
    {
      printf("[uart] taille invalide (max %u)\n", (unsigned)WASM_MAX_SIZE);
      return 0;
    }

  if (read_full(fd, g_wasm_buffer, size) != 0)
    {
      printf("[uart] transfert incomplet\n");
      return 0;
    }

  printf("[uart] module recu (%u octets)\n", (unsigned)size);
  return size;
}

/****************************************************************************
 * Execution du module WASM (sequence identique a Zephyr).
 ****************************************************************************/

static void execute_wasm(uint8_t *wasm_data, uint32_t wasm_size)
{
  char error_buf[128];
  wasm_module_t       module   = NULL;
  wasm_module_inst_t  inst     = NULL;
  wasm_exec_env_t     exec_env = NULL;
  wasm_function_inst_t func    = NULL;

  printf("[wamr] chargement du module...\n");
  module = wasm_runtime_load(wasm_data, wasm_size, error_buf, sizeof(error_buf));
  if (!module)
    {
      printf("[wamr] LOAD ERROR: %s\n", error_buf);
      return;
    }
  printf("[wamr] module charge\n");

  inst = wasm_runtime_instantiate(module, STACK_SIZE, HEAP_SIZE,
                                  error_buf, sizeof(error_buf));
  if (!inst)
    {
      printf("[wamr] INSTANTIATE ERROR: %s\n", error_buf);
      wasm_runtime_unload(module);
      return;
    }
  printf("[wamr] instance creee\n");

  exec_env = wasm_runtime_create_exec_env(inst, STACK_SIZE);
  if (!exec_env)
    {
      printf("[wamr] echec de creation de l'exec_env\n");
      wasm_runtime_deinstantiate(inst);
      wasm_runtime_unload(module);
      return;
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
  printf(" Firmware hote WAMR sur NuttX — sonde SPIREC\n");
  printf("========================================================\n");

  /* 1. Initialisation WAMR avec pool memoire (comme Zephyr). */
  RuntimeInitArgs init_args;
  memset(&init_args, 0, sizeof(init_args));
  init_args.mem_alloc_type              = Alloc_With_Pool;
  init_args.mem_alloc_option.pool.heap_buf  = g_wamr_pool;
  init_args.mem_alloc_option.pool.heap_size = sizeof(g_wamr_pool);

  if (!wasm_runtime_full_init(&init_args))
    {
      printf("[wamr] echec d'initialisation du runtime\n");
      return -1;
    }
  printf("[wamr] runtime initialise (pool=%d Ko)\n", POOL_SIZE / 1024);

  /* 2. Etat hote + enregistrement des fonctions natives (module "env"). */
  host_nuttx_init(g_wamr_pool, sizeof(g_wamr_pool));
  if (!host_register_natives())
    {
      wasm_runtime_destroy();
      return -1;
    }

  /* 3. Ouverture du peripherique de reception UART. */
  int fd = open(CONFIG_WAMR_SONDE_UART_DEV, O_RDWR);
  if (fd < 0)
    {
      printf("[uart] impossible d'ouvrir %s (errno=%d)\n",
             CONFIG_WAMR_SONDE_UART_DEV, errno);
      wasm_runtime_destroy();
      return -1;
    }

  printf("===== DEPLOYMENT — SPIREC (NuttX) =====\n");
  printf("Protocole : 4 octets taille (LE) + binaire .wasm\n");
  printf("Taille max : %d octets\n", WASM_MAX_SIZE);
  printf("Peripherique : %s\n", CONFIG_WAMR_SONDE_UART_DEV);

  /* 4. Boucle : reception d'un module puis execution. */
  for (;;)
    {
      uint32_t size = uart_receive_module(fd);
      if (size > 0)
        {
          execute_wasm(g_wasm_buffer, size);
          printf("\n[boot] en attente d'un nouveau module...\n");
        }
      else
        {
          usleep(20000);
        }
    }

  close(fd);
  wasm_runtime_destroy();
  return 0;
}