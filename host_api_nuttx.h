/****************************************************************************
 * Interface de la couche hote NuttX (transport + metriques + identite).
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_WAMR_SONDE_HOST_API_NUTTX_H
#define __APPS_WAMR_SONDE_HOST_API_NUTTX_H

#include <stdbool.h>
#include <stddef.h>

/* Initialise l'etat hote (pool WAMR pour resolution de pointeur, base de temps,
 * compteur de reset). A appeler apres wasm_runtime_full_init et avant tout
 * chargement de module. */
void host_nuttx_init(void *pool, size_t pool_size);

/* Enregistre la table native_symbols aupres de WAMR (module "env").
 * A appeler avant wasm_runtime_load. Retourne true en cas de succes. */
bool host_register_natives(void);

#endif /* __APPS_WAMR_SONDE_HOST_API_NUTTX_H */
