/****************************************************************************
 * apps/wamr_sonde/deploy_nuttx.h
 *
 * Contrat de mise a jour a chaud (hot-update) cote NuttX.
 *
 * Ces fonctions sont DEFINIES dans wamr_sonde_main.c (qui possede le socket
 * d'ecoute, les deux buffers et le compteur) et APPELEES par host_api_nuttx.c
 * (qui les expose au module WASM via host_poll_update / host_metric_update_count).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_WAMR_SONDE_DEPLOY_NUTTX_H
#define __APPS_WAMR_SONDE_DEPLOY_NUTTX_H

#include <stdint.h>

/* Sonde le reseau (accept NON bloquant sur le socket d'ecoute 5555) et, si un
 * module complet arrive, le place dans le buffer de staging et leve le drapeau.
 * Retourne 1 si un module est desormais en attente, 0 sinon.
 */
int deploy_try_stage(void);

/* Retourne 1 si un module recu attend d'etre charge, 0 sinon. */
int deploy_pending(void);

/* Nombre de mises a jour a chaud effectuees depuis le demarrage (metrique,
 * distincte de reset_count qui ne compte que les redemarrages materiels).
 */
uint32_t deploy_update_count(void);

#endif /* __APPS_WAMR_SONDE_DEPLOY_NUTTX_H */