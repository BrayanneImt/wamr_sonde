# wamr_sonde — Firmware hôte NuttX pour la sonde SPIREC

Application NuttX qui héberge WAMR **en bibliothèque**, reçoit un module `.wasm`
par UART et l'exécute avec la table de fonctions natives de la sonde SPIREC.

**Le même `.wasm` que sous Zephyr, sans aucune modification.** Seule la couche
hôte diffère (API POSIX/NuttX au lieu des API Zephyr) ; la table
`native_symbols` est identique nom‑pour‑nom, ce qui garantit la portabilité
binaire du module.

## Prérequis

- NuttX + nuttx-apps clonés dans `~/nuttxspace/` (branche master).
- Toolchain Xtensa ESP32‑S3 installée (`xtensa-esp32s3-elf-gcc`).
- `esptool.py` ≥ 4.8.0 dans le PATH.
- **WAMR activé** dans NuttX : `Application Configuration → Interpreters → WAMR`
  avec l'**interpréteur rapide** (`INTERPRETERS_WAMR_FAST=y`) et la **libc
  builtin** (`INTERPRETERS_WAMR_LIBC_BUILTIN=y`). C'est ce qui compile la
  bibliothèque WAMR dont dépend cette application.
- Le module `metrics.wasm` produit par le projet `zephyr_rust_metrics2`
  (`./build_wasm.sh`). C'est exactement le même binaire que sous Zephyr.

## Installation de l'application

Copier le dossier `wamr_sonde/` dans `~/nuttxspace/apps/` :

```bash
cp -r wamr_sonde ~/nuttxspace/apps/
```

L'app est auto‑détectée par le build NuttX (présence de `Makefile`, `Make.defs`,
`Kconfig`).

## Activation et compilation

```bash
cd ~/nuttxspace/nuttx
make menuconfig
#   Application Configuration → Interpreters → WAMR : FAST + LIBC builtin (déjà fait)
#   Application Configuration → Sonde SPIREC (hôte WAMR portable) : activer
make -j$(nproc)
make flash ESPTOOL_PORT=/dev/ttyUSB0
```

## Exécution

1. Ouvrir la console et lancer l'application :

```bash
picocom -b 115200 /dev/ttyUSB0
nsh> wamr_sonde
```

La sonde affiche sa bannière puis attend le module :

```
 Firmware hote WAMR sur NuttX — sonde SPIREC
[wamr] runtime initialise (pool=160 Ko)
[wamr] 24 symboles natifs enregistres
[uart] en attente d'un module .wasm...
```

2. Depuis le PC, pousser le `.wasm` par le **même** protocole que sous Zephyr :

```bash
python3 upload.py --port /dev/ttyUSB0 --file metrics.wasm
```

## Résultat attendu

Côté carte :

```
[uart] taille annoncee = 6850 octets
[uart] module recu (6850 octets)
[wamr] chargement du module...
[wamr] module charge
[wamr] instance creee
[wamr] execution du module...
================================================
 Collecteur WASM multi-transport (PADRE + CBOR)
================================================
Device    : nuttx-esp32s3
OS        : nuttx
Transport : wifi
...
```

La sonde tourne alors sa boucle PADRE et tente d'émettre ses trames CBOR par TCP.

## Limites de ce jalon

- **Transport UART pour la réception** du `.wasm` ; le réseau (upload TCP) sera
  ajouté ensuite, comme sous Zephyr.
- **Métriques système partielles** : `uptime`, `free_heap`, `bytes_tx/rx`,
  `transport_errors`, `reset_count` sont réelles ; `cpu`, `stack`,
  `active_threads`, `signal`, retransmissions renvoient 0 pour l'instant (à
  raffiner avec les API NuttX). PADRE fonctionne quand même (émission surtout
  sur heartbeat).
- **Pas de mise à jour à chaud** sur ce jalon (`host_poll_update` renvoie 0).
- **Transport TCP** : l'émission suppose le Wi‑Fi monté côté hôte (jalon
  réseau à venir). Sur ce premier jalon, l'objectif est que le module **charge
  et s'exécute** sous NuttX.

Voir `GUIDE.md` pour le détail du fonctionnement et le dépannage.
