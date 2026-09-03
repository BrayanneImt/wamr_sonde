# wamr_sonde — Firmware hôte NuttX pour la sonde SPIREC

Application NuttX qui héberge **WAMR en bibliothèque**, reçoit un module `.wasm`
**par réseau (TCP)**, l'exécute, et le **met à jour à chaud** — avec des
**métriques système réelles** lues via le procfs, l'ADC et le Wi-Fi de NuttX.

**Le même `.wasm` que sous Zephyr, sans aucune modification.** Seule la couche
hôte diffère (API POSIX/NuttX au lieu des API Zephyr) ; la table
`native_symbols` est identique nom‑pour‑nom, ce qui garantit la portabilité
binaire du module.

---

## 1. Prérequis

- NuttX + nuttx-apps clonés dans `~/nuttxspace/` (branche master).
- Toolchain Xtensa ESP32‑S3 (`xtensa-esp32s3-elf-gcc`, GCC 14.2).
- `esptool.py` ≥ 4.8.0 dans le PATH.
  - Sur Ubuntu 24 : le paquet apt fournit 4.7.0 (trop ancienne) et la commande
    `esptool` sans le `.py`. Installer une version récente avec
    `pipx install esptool`, puis créer le lien
    `ln -sf ~/.local/bin/esptool ~/.local/bin/esptool.py`.
- Le module `metrics.wasm` produit par `zephyr_rust_metrics2` (`./build_wasm.sh`,
  cible `wasm32v1-none`). C'est exactement le même binaire que sous Zephyr.
- Un point d'accès Wi‑Fi (le PC en hotspot) en **10.42.0.x**, exécutant le
  collecteur `server.py` (version CBOR) sur le port **8080**. Le `.wasm` vise
  `10.42.0.1:8080` (adresse du PC).

---

## 2. Installation de l'application

```bash
cp -r wamr_sonde ~/nuttxspace/apps/
```

L'app est auto‑détectée (présence de `Makefile`, `Make.defs`, `Kconfig`). Elle
dépend de WAMR (`depends on INTERPRETERS_WAMR`) : elle n'apparaît dans le
menuconfig qu'une fois WAMR activé.

---

## 3. Configuration complète (toutes les options activées)

### 3.1 Partir du profil de carte Wi‑Fi

Le profil `wifi` fournit la pile réseau TCP/IP, le driver Wi‑Fi ESP32‑S3
(`ESPRESSIF_WIFI`), le client DHCP et l'outil `wapi`.

```bash
cd ~/nuttxspace/nuttx
make distclean
./tools/configure.sh esp32s3-devkit:wifi
```

### 3.2 Réglages `make menuconfig`

Astuce : la touche `/` ouvre la recherche (taper le symbole pour y sauter).

**a) WAMR** — `Application Configuration → Interpreters → WAMR` :
- cocher **WAMR** ;
- `Enable interpreter →` : **fast interpreter** (`INTERPRETERS_WAMR_FAST=y`) ;
- **Enable built‑in libc** (`INTERPRETERS_WAMR_LIBC_BUILTIN=y`) ;
- AOT et WASI **désactivés**.

**b) Montée Wi‑Fi automatique au démarrage** —
`Application Configuration → Network Utilities → Network Initialization` :
- **NETINIT** activé ;
- **DHCP** activé (`NETINIT_DHCPC=y`) — indispensable, sinon l'IP reste figée
  sur un mauvais sous‑réseau (l'IP statique par défaut est en 10.0.0.x) ;
- **WAPI Configuration** :
  - SSID = `a26nguep-hotspot` (`NETINIT_WAPI_SSID`),
  - passphrase = `123456789` (`NETINIT_WAPI_PASSPHRASE`),
  - sécurité **WPA2** (`NETINIT_WAPI_AUTHWPA_WPA2`), chiffrement **CCMP**.

**c) Mesure de charge CPU** (pour la métrique CPU réelle) —
`RTOS Features → Performance Monitoring` (ou recherche `SCHED_CPULOAD`) :
- passer de `SCHED_CPULOAD_NONE` à **`SCHED_CPULOAD_SYSCLK`** ;
- activer **`SCHED_INSTRUMENTATION`**.

**d) Procfs** (pour CPU, mémoire, pile, threads) —
normalement déjà actif dans le profil `wifi` : vérifier `CONFIG_FS_PROCFS=y`.
Aucune entrée à exclure (ni CPUINFO, ni PROCESS, ni MEMINFO).

**e) ADC** (pour la métrique batterie) —
`Device Drivers → Analog Device(ADC/DAC) Support` :
- activer **`CONFIG_ADC`** ;
- activer le **driver ADC ESP32‑S3** et le **canal** câblé sur la batterie
  (Heltec v3 : ADC1 canal 0 = GPIO1). Un `/dev/adc0` doit apparaître.

**f) L'application** —
`Application Configuration → Sonde SPIREC (hôte WAMR portable)` :
- l'activer ;
- **mémoire** (la DRAM de l'ESP32‑S3 est comptée avec le Wi‑Fi) :
  - **Pool WAMR (Ko)** = `112` (doit contenir les 64 Ko de mémoire linéaire du
    module + métadonnées + tas/pile d'exécution ; **96 Ko est trop juste** et
    provoque `allocate linear memory failed`) ;
  - **Taille max .wasm (Ko)** = `12` (le module fait ~7 Ko ; ce buffer existe
    **en double** à cause du hot‑update).

### 3.3 Vérifier puis compiler

```bash
grep -iE "WAMR_SONDE_POOL_KB|WAMR_SONDE_WASM_MAX_KB|NETINIT_DHCPC|NETINIT_WAPI_SSID|SCHED_CPULOAD_SYSCLK|FS_PROCFS=|CONFIG_ADC=" .config

make -j$(nproc)
make flash ESPTOOL_PORT=/dev/ttyUSB0
```

> **Mémoire** : vérifier que `dram0_0_seg` reste **sous 100 %** dans le rapport
> de build. Avec Wi‑Fi + WAMR + hot‑update + ADC, on est autour de 95 % — très
> serré. Si un `INSTANTIATE ERROR: allocate linear memory failed` apparaît,
> remonter le pool (≥112 Ko) ; si le lien déborde, réduire le buffer `.wasm`
> ou désactiver l'ADC tant que la batterie n'est pas exploitée.

---

## 4. Exécution

La réception du `.wasm` se fait **par réseau (TCP 5555)**. NuttX monte le Wi‑Fi
automatiquement au démarrage ; l'application suppose le réseau prêt.

1. Vérifier que la carte est sur le réseau (IP en 10.42.0.x) :

```bash
picocom -b 115200 /dev/ttyUSB0
nsh> ifconfig          # wlan0 avec une IP 10.42.0.x
nsh> wamr_sonde        # démarre l'hôte ; la console reste libre pour les logs
```

Affichage attendu :

```
 Firmware hote WAMR sur NuttX — sonde SPIREC (hot-update)
[wamr] runtime initialise (pool=112 Ko)
[wamr] 25 symboles natifs enregistres
[net] serveur d'upload en ecoute sur le port TCP 5555
```

2. Lancer le collecteur sur le PC (version CBOR), à l'écoute sur 8080 :

```bash
python3 server.py
```

3. Pousser le `.wasm` par réseau (protocole identique à Zephyr) :

```bash
python3 upload_net.py <ip_carte> metrics.wasm
# ex. : python3 upload_net.py 10.42.0.71 metrics.wasm
```

### Mise à jour à chaud

Sans reset, pousser à nouveau un `.wasm` avec `upload_net.py`. La sonde détecte
la mise à jour (~1 s, pendant son sommeil fragmenté), rend la main, et le
firmware bascule sur le nouveau module :

```
[net] client connecte pour MAJ (10.42.0.1)
[net] maj : module recu (7085 octets)
[deploy] MISE A JOUR A CHAUD #1 (7085 octets)
```

Côté serveur, `update_count` s'incrémente (distinct de `reset_count`).

---

## 5. Métriques implémentées

| Métrique              | Source NuttX                          | État        |
|-----------------------|---------------------------------------|-------------|
| `cpu_usage_pct`       | `/proc/cpuload`                       | réelle      |
| `free_heap_bytes`     | `/proc/meminfo` (colonne *free*)      | réelle      |
| `stack_usage_pct`     | `/proc/self/stack`                    | réelle      |
| `active_threads`      | comptage des tâches sous `/proc`      | réelle      |
| `signal_dbm`          | ioctl `SIOCGIWSENS` sur `wlan0`       | réelle      |
| `uptime_ms`           | `clock_gettime(CLOCK_MONOTONIC)`      | réelle      |
| `bytes_tx/rx`, `errors`| compteurs de transport               | réels       |
| `reset_count`         | compteur interne                      | réel        |
| `update_count`        | compteur de hot‑update                | réel        |
| `battery_mv`          | ADC `/dev/adc0` + pont diviseur       | conditionnel|
| `coap_retransmissions`| —                                     | 0 (voir §6) |

---

## 6. Points spécifiques et limites

### Batterie (ADC)

`battery_mv` lit `/dev/adc0`, convertit la valeur brute en mV, puis applique le
**pont diviseur** de la carte (constantes `BATT_DIVIDER_NUM/DEN`,
`BATT_ADC_VREF_MV`, `BATT_ADC_MAX_RAW`, `BATT_ADC_CHANNEL` dans
`host_api_nuttx.c`, à ajuster selon la carte).

**Spécificité Heltec WiFi LoRa 32 v3** : la tension batterie arrive sur GPIO1
(ADC1 canal 0) via un pont diviseur (~x4.9), et le circuit de mesure s'active en
mettant **GPIO37 à l'état bas**. Ce pilotage de GPIO37 n'est pas fait dans le
code (il dépend d'un driver GPIO board) : sans lui, la lecture peut rester
nulle. La validation réelle nécessite une **batterie physiquement branchée** ;
sur alimentation USB, `battery_mv` vaut 0, ce qui désactive le gating
énergétique de PADRE (comportement identique à Zephyr sans ADC batterie).

### Retransmissions TCP

`coap_retransmissions` reste à 0 : NuttX **n'expose pas** ces statistiques par
socket dans cette configuration (`/proc/net` ne contient que `wlan0`, sans
compteurs TCP). Contrairement à Zephyr qui les fournit via `net_mgmt`. C'est une
différence entre OS que la couche hôte absorbe : la sonde `.wasm` reste
identique, seule la richesse des métriques disponibles varie selon l'OS.

### Réception

**TCP uniquement** (port 5555), pas d'UART : sous NuttX la console `nsh` occupe
l'UART. Le `.wasm` arrive par réseau, la console reste libre pour les logs.

### Mémoire

Wi‑Fi + WAMR + hot‑update (double buffer) + ADC saturent la DRAM de l'ESP32‑S3
(~95 %). Dimensionnement retenu : pool WAMR 112 Ko, buffer `.wasm` 12 Ko. C'est
un résultat en soi : il documente le coût réel de l'approche sur cible
contrainte.

---

## 7. Résumé des ajouts par rapport au profil `wifi` de base

1. **WAMR** activé (fast interp + libc builtin) → bibliothèque WAMR.
2. **NETINIT + DHCP + WAPI** → montée Wi‑Fi automatique au boot.
3. **SCHED_CPULOAD_SYSCLK + SCHED_INSTRUMENTATION** → métrique CPU réelle.
4. **ADC** (`CONFIG_ADC` + driver ESP32‑S3) → métrique batterie.
5. **Application `wamr_sonde`** : hôte WAMR, réception TCP, hot‑update,
   métriques procfs/Wi‑Fi/ADC.