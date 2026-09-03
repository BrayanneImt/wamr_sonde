# wamr_sonde — Firmware hôte NuttX pour la sonde SPIREC

Application NuttX qui héberge WAMR **en bibliothèque**, reçoit un module `.wasm`
**par réseau (TCP)** et l'exécute avec la table de fonctions natives de la sonde
SPIREC. Le Wi-Fi est monté automatiquement au démarrage par NuttX.

**Le même `.wasm` que sous Zephyr, sans aucune modification.** Seule la couche
hôte diffère (API POSIX/NuttX au lieu des API Zephyr) ; la table
`native_symbols` est identique nom‑pour‑nom, ce qui garantit la portabilité
binaire du module.

## Prérequis

- NuttX + nuttx-apps clonés dans `~/nuttxspace/` (branche master).
- Toolchain Xtensa ESP32‑S3 installée (`xtensa-esp32s3-elf-gcc`).
- `esptool.py` ≥ 4.8.0 dans le PATH (créer le lien `esptool.py` → `esptool`
  si le paquet ne fournit que `esptool` ; version apt 4.7.0 trop ancienne,
  passer par `pipx install esptool`).
- Le module `metrics.wasm` produit par le projet `zephyr_rust_metrics2`
  (`./build_wasm.sh`). C'est exactement le même binaire que sous Zephyr.
- Un point d'accès Wi‑Fi (le PC en hotspot) en **10.42.0.x**, sur lequel tourne
  le collecteur `server.py`. Le `.wasm` vise `10.42.0.1:8080` (adresse du PC).

## Installation de l'application

Copier le dossier `wamr_sonde/` dans `~/nuttxspace/apps/` :

```bash
cp -r wamr_sonde ~/nuttxspace/apps/
```

L'app est auto‑détectée par le build NuttX (présence de `Makefile`, `Make.defs`,
`Kconfig`).

## Configuration, compilation et flash

### 1. Partir du profil de carte Wi‑Fi

Le profil `wifi` fournit la pile réseau TCP/IP, le driver Wi‑Fi ESP32‑S3, le
client DHCP et l'outil `wapi` — indispensables à la réception réseau.

```bash
cd ~/nuttxspace/nuttx
make distclean
./tools/configure.sh esp32s3-devkit:wifi
```

### 2. Réglages dans `make menuconfig`

```bash
make menuconfig
```

Activer et régler, dans l'ordre :

**a) WAMR** (la bibliothèque dont dépend l'app) —
`Application Configuration → Interpreters → WAMR` :
- cocher **WAMR** ;
- sous‑menu `Enable interpreter →` : choisir le **fast interpreter**
  (`INTERPRETERS_WAMR_FAST=y`) ;
- cocher **Enable built‑in libc** (`INTERPRETERS_WAMR_LIBC_BUILTIN=y`) ;
- laisser AOT et WASI désactivés.

**b) Montée Wi‑Fi automatique au démarrage** —
`Application Configuration → Network Utilities → Network Initialization
(NETUTILS_NETINIT)` :
- activer **NETINIT** ;
- activer l'acquisition **DHCP** (`NETINIT_DHCPC=y`) — sinon l'IP reste figée
  sur un mauvais sous‑réseau ;
- sous‑menu **WAPI Configuration** : renseigner
  - SSID = `a26nguep-hotspot` (`NETINIT_WAPI_SSID`),
  - passphrase = `123456789` (`NETINIT_WAPI_PASSPHRASE`),
  - sécurité **WPA2** / chiffrement **CCMP**.

> Astuce : dans menuconfig, la touche `/` ouvre la recherche (taper
> `NETINIT_DHCPC`, `WAMR_SONDE_POOL`, etc. pour sauter à l'option).

**c) L'application** —
`Application Configuration → Sonde SPIREC (hôte WAMR portable)` :
- l'activer (elle n'apparaît qu'une fois WAMR activé, car elle en dépend) ;
- régler la mémoire pour tenir avec le Wi‑Fi (DRAM comptée sur ESP32‑S3) :
  - **Taille du pool memoire WAMR (Ko)** = `96`,
  - **Taille max du module .wasm accepte (Ko)** = `12`.

### 3. Vérifier la configuration puis compiler

```bash
grep -iE "WAMR_SONDE_POOL_KB|WAMR_SONDE_WASM_MAX_KB|NETINIT_DHCPC|NETINIT_WAPI_SSID" .config

make -j$(nproc)
make flash ESPTOOL_PORT=/dev/ttyUSB0
```

Vérifier au passage que `dram0_0_seg` reste **sous 100 %** dans le rapport
mémoire du build (sinon réduire encore le pool).

## Exécution

La réception du `.wasm` se fait **par réseau (TCP)**. NuttX monte le Wi-Fi
automatiquement au démarrage (NETINIT + WAPI + DHCP) ; l'application suppose donc
le réseau prêt et ouvre un serveur TCP sur le port **5555**.

1. Vérifier que la carte est bien sur le réseau (IP en 10.42.0.x) :

```bash
picocom -b 115200 /dev/ttyUSB0
nsh> ifconfig          # doit montrer wlan0 avec une IP 10.42.0.x
nsh> wamr_sonde        # lance l'hôte ; la console reste libre pour les logs
```

La sonde affiche :

```
 Firmware hote WAMR sur NuttX — sonde SPIREC (reseau)
[wamr] runtime initialise (pool=96 Ko)
[wamr] 25 symboles natifs enregistres
[net] serveur d'upload en ecoute sur le port TCP 5555
```

2. **Lancer le collecteur** sur le PC (version CBOR), à l'écoute sur 8080 :

```bash
python3 server.py        # écoute 0.0.0.0:8080
```

3. Depuis le PC, pousser le `.wasm` par réseau (protocole identique à Zephyr) :

```bash
python3 upload_net.py <ip_carte> metrics.wasm
# ex. : python3 upload_net.py 10.42.0.10 metrics.wasm
```

## Résultat attendu

Côté carte :

```
[net] client connecte (10.42.0.1)
[net] taille annoncee = 6850 octets
[net] module recu par reseau (6850 octets)
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

La sonde tourne alors sa boucle PADRE et émet ses trames CBOR par TCP vers
10.42.0.1:8080. Elles apparaissent dans `server.py`, exactement comme sous
Zephyr — c'est la preuve que le même `.wasm` supervise depuis NuttX.

## État et limites

- **Réception réseau (TCP 5555)** : opérationnelle. Le Wi‑Fi est monté
  automatiquement par NuttX au démarrage ; l'émission des métriques se fait par
  TCP vers `10.42.0.1:8080`.
- **Métriques système partielles** : `uptime`, `free_heap`, `bytes_tx/rx`,
  `transport_errors`, `reset_count` sont réelles ; `cpu`, `stack`,
  `active_threads`, `signal`, retransmissions renvoient 0 pour l'instant (à
  raffiner avec les API NuttX). PADRE fonctionne quand même (émission surtout
  sur heartbeat, faute de signal CPU).
- **Pas encore de mise à jour à chaud** : `host_poll_update` renvoie 0 ; la
  relève d'un nouveau module se fait en relançant `wamr_sonde` (ou après un
  nouvel upload une fois la sonde terminée). Le hot‑update (staging + drapeau,
  modèle Zephyr) est le jalon suivant.
- **Mémoire** : sur ESP32‑S3, le Wi‑Fi de NuttX est gourmand en DRAM. Le pool
  WAMR (96 Ko) et le buffer `.wasm` (12 Ko) sont dimensionnés pour tenir ;
  réduire encore le pool si le build déborde `dram0_0_seg`.