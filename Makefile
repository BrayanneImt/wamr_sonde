############################################################################
# apps/wamr_sonde/Makefile
#
# Application hote NuttX pour la sonde SPIREC (WAMR en bibliotheque).
#
# STRATEGIE A : on ne RECOMPILE PAS WAMR (deja compile par
# apps/interpreters/wamr dans libapps.a). On ajoute seulement les chemins
# d'en-tetes de l'API WAMR pour que nos appels compilent ; l'editeur de liens
# resout les symboles depuis libapps.a.
#
# Les chemins ci-dessous sont repris du wamr.mk officiel, prefixes par le
# chemin relatif vers le dossier WAMR (../interpreters/wamr).
#
# SPDX-License-Identifier: Apache-2.0
############################################################################

include $(APPDIR)/Make.defs

# --- Racine des sources WAMR telechargees par apps/interpreters/wamr ---
WAMR_DIR    := $(APPDIR)/interpreters/wamr/wamr
CORE_ROOT   := $(WAMR_DIR)/core
IWASM_ROOT  := $(WAMR_DIR)/core/iwasm
SHARED_ROOT := $(WAMR_DIR)/core/shared

# --- En-tetes de l'API WAMR (identiques a ceux du wamr.mk) ---
CFLAGS += -I$(CORE_ROOT)
CFLAGS += -I$(IWASM_ROOT)/include
CFLAGS += -I$(IWASM_ROOT)/common
CFLAGS += -I$(IWASM_ROOT)/interpreter
CFLAGS += -I$(SHARED_ROOT)/include
CFLAGS += -I$(SHARED_ROOT)/platform/include
CFLAGS += -I$(SHARED_ROOT)/platform/nuttx
CFLAGS += -I$(SHARED_ROOT)/utils
CFLAGS += -I$(SHARED_ROOT)/mem-alloc

# --- Macros de fonctionnalites WAMR ---
# wasm_export.h est l'API publique stable ; ces macros alignent les quelques
# blocs conditionnels de l'en-tete sur la configuration reelle de WAMR
# (interpreteur rapide, libc builtin, pas d'AOT/JIT/WASI/GC).
CFLAGS += -DWASM_ENABLE_INTERP=1
CFLAGS += -DWASM_ENABLE_FAST_INTERP=1
CFLAGS += -DWASM_ENABLE_AOT=0
CFLAGS += -DWASM_ENABLE_JIT=0
CFLAGS += -DWASM_ENABLE_LIBC_BUILTIN=1
CFLAGS += -DWASM_ENABLE_LIBC_WASI=0
CFLAGS += -DWASM_ENABLE_MULTI_MODULE=0
CFLAGS += -DWASM_ENABLE_SHARED_MEMORY=0
CFLAGS += -DWASM_ENABLE_THREAD_MGR=0
CFLAGS += -DWASM_ENABLE_GC=0

# --- Application NuttX ---
PROGNAME  = $(CONFIG_WAMR_SONDE_PROGNAME)
PRIORITY  = $(CONFIG_WAMR_SONDE_PRIORITY)
STACKSIZE = $(CONFIG_WAMR_SONDE_STACKSIZE)
MODULE    = $(CONFIG_WAMR_SONDE)

# Point d'entree + couche hote (fonctions natives POSIX/NuttX).
MAINSRC = wamr_sonde_main.c
CSRCS   = host_api_nuttx.c

include $(APPDIR)/Application.mk
