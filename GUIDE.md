# Guide — Firmware hôte NuttX (wamr_sonde)

Ce guide explique comment l'application fonctionne, pourquoi elle est conçue
ainsi, et comment diagnostiquer les problèmes. Il complète le `README.md`
(prise en main rapide).

## 1. Principe : un `.wasm` portable, deux hôtes

La sonde de supervision est un module WebAssembly (`metrics.wasm`) compilé une
seule fois. Elle n'appelle jamais l'OS directement : elle passe par un
**contrat de fonctions hôtes** (`host_print`, `host_transport_*`,
`host_metric_*`, `host_get_*`, `host_sleep`, `host_poll_update`), importé du
module `env`.

Chaque OS fournit sa propre **implémentation** de ce contrat :

| Contrat (identique)      | Zephyr                    | NuttX (ici)             |
|--------------------------|---------------------------|-------------------------|
| `host_sleep`             | `k_sleep`                 | `sleep()`               |
| `host_transport_send`    | `zsock_send`              | `send()` (POSIX)        |
| `host_metric_uptime_ms`  | `k_uptime_get`            | `clock_gettime`         |
| `host_metric_cpu_usage`  | `k_thread_runtime_stats`  | 0 (jalon)               |
| identité                 | Kconfig Zephyr            | constantes NuttX        |

La table `native_symbols` a **les mêmes noms et signatures** des deux côtés.
C'est la condition de la portabilité : WAMR résout les imports du `.wasm` contre
cette table, quel que soit l'OS.

## 2. Architecture de l'application

Trois fichiers de code :

- `wamr_sonde_main.c` : le point d'entrée. Initialise WAMR avec un **pool
  mémoire** (`Alloc_With_Pool`, comme Zephyr), enregistre les natives, reçoit
  le `.wasm` par UART, puis le charge/instancie/exécute.
- `host_api_nuttx.c` : la couche hôte. Contient la table `native_symbols` et
  l'implémentation POSIX/NuttX de chaque fonction.
- `host_api_nuttx.h` : l'interface (init + enregistrement).

Et trois fichiers de build : `Kconfig` (options), `Make.defs` (enregistrement),
`Makefile` (chemins d'en-têtes WAMR — **stratégie A** : on ne recompile pas
WAMR, on référence ses en-têtes ; le linker résout les symboles depuis
`libapps.a`).

## 3. Cycle d'exécution

```
open(UART) → wasm_runtime_full_init(pool)
           → host_register_natives("env", …)
           → boucle :
                uart_receive_module()   [taille(4 LE) + binaire]
                wasm_runtime_load()
                wasm_runtime_instantiate()
                wasm_runtime_create_exec_env()
                wasm_runtime_call_wasm("main")
```

C'est la transposition exacte du `main.c` Zephyr. Chaque étape logge son
résultat, ce qui permet de localiser un échec sans ambiguïté.

## 4. Protocole d'upload (inchangé)

Identique à Zephyr : `[taille : 4 octets little-endian][binaire .wasm]`. Le
script `upload.py` du projet fonctionne **sans modification**. C'est voulu : la
chaîne de déploiement PC est commune aux deux OS.

## 5. Pourquoi ce modèle (et pas `iwasm`)

NuttX fournit une commande `iwasm` qui exécute un `.wasm` depuis un **fichier**,
via WASI. On ne l'utilise pas car :

- la sonde importe des fonctions hôtes **maison** (`env.host_*`) que `iwasm`
  standard ne fournit pas ;
- WASI n'expose pas proprement le transport ni les métriques système voulus ;
- passer par `iwasm` imposerait de **réécrire** la sonde, cassant la
  portabilité binaire.

En hébergeant WAMR **en bibliothèque** et en enregistrant nos natives, on garde
le **même `.wasm`** sur les deux OS. C'est le cœur de la démonstration.

## 6. Dépannage

**« impossible d'ouvrir /dev/console »** : ajuster `WAMR_SONDE_UART_DEV` dans
menuconfig vers le bon périphérique série de la carte.

**Le chargement échoue (`LOAD ERROR`)** : vérifier que le `.wasm` est bien celui
produit pour `wasm32v1-none` (WebAssembly 1.0), que WAMR refuse le reference-
types. C'est le même prérequis que sous Zephyr.

**`instantiate ERROR` mémoire** : augmenter `WAMR_SONDE_POOL_KB` (le pool doit
contenir la mémoire linéaire de 64 Ko + les métadonnées). Réduire si la RAM
manque, mais 160 Ko est un bon défaut sur ESP32-S3.

**Symboles natifs non résolus au chargement** : signe que la table
`native_symbols` diverge du contrat `ffi.rs` de la sonde. Les noms et signatures
doivent être identiques à la version Zephyr. Vérifier qu'aucune fonction n'a été
oubliée (24 symboles attendus).

**La compilation ne trouve pas `wasm_export.h`** : vérifier que WAMR est bien
activé (donc téléchargé dans `apps/interpreters/wamr/wamr/`) et que les chemins
`-I` du `Makefile` pointent vers ce dossier.

## 7. Suite (jalons à venir)

1. **Métriques NuttX réelles** : remplacer les retours 0 par les vraies valeurs
   (CPU via l'ordonnanceur NuttX, pile, threads, RSSI Wi-Fi).
2. **Upload réseau** : recevoir le `.wasm` par TCP (comme le module réseau
   Zephyr), en montant le Wi-Fi côté hôte au démarrage.
3. **Mise à jour à chaud** : implémenter `host_poll_update` (staging + drapeau)
   pour la relève sans reset, sur le modèle Zephyr.
