#!/bin/sh
# drive_model_cases.sh — optical-drive identity normalization, validated across
# vendor families: (1) MODEL extraction from the product string, (2) VENDOR
# canonicalization to the community/brand name.
#
# Guards the strategy used by scripts/hw-smoke.sh (drive_model / vendor_canonical),
# which name capture artifacts <VENDOR>/<MODEL> and mos-v<ver>-<MODEL>-<rev>-… :
#   - the model is always the LAST whitespace token of the INQUIRY product string
#     (extract_drive_model = `awk 'NF { print $NF }'`);
#   - the vendor maps the OEM INQUIRY string to the brand (HL-DT-ST -> LG, etc.).
# Keep these copies in sync with hw-smoke.sh.
#
# Run: sh tests/drive_model_cases.sh   (exits non-zero on any failure)
#
# Handles:
#   product field only   "BD-RE WH16NS60"
#   full SCSI inquiry    "HL-DT-ST BD-RE WH16NS60"
#   SCSI-padded (16 B)   "BD-RE WH16NS60  "
#   no disc-type prefix  "DVD-E616P2"          (ASUS — entire string is the model)
#   space-separated type "DVD A DS8A5S"        (PLDS/HP)
#   space-separated type "DVD RW DW-D22A"      (Sony — not a typo for DVD-RW)
#   multi-hyphen model   "BD-RW BDR-XD08UMB-S" (Pioneer)

extract_drive_model() {
    printf '%s\n' "$1" | awk 'NF { print $NF }'
}

# Resolve an INQUIRY vendor string to the community/MakeMKV/AccurateRip name (kept
# in sync with scripts/hw-smoke.sh:vendor_canonical). Only the OEM joint-venture /
# brand-holder strings differ; real brands pass through.
vendor_canonical() {
    key="$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]' | tr -d ' ._-')"
    case "$key" in
        HLDTST)                       printf 'LG' ;;
        MATSHITA|MATSUSHITA)          printf 'Panasonic' ;;
        TSSTCORP|TOSHIBASAMSUNG)      printf 'Samsung' ;;
        PLDS|SLIMTYPE|PHILIPSLITEON)  printf 'Lite-On' ;;
        LITEON)                       printf 'Lite-On' ;;
        SONYNEC|SONYOPTIARC)          printf 'Optiarc' ;;
        *)                            printf '%s' "$1" ;;
    esac
}

# -- test runner ---------------------------------------------------------------

_PASS=0
_FAIL=0

_check() {
    _desc="$1"
    _input="$2"
    _want="$3"
    _got=$(extract_drive_model "$_input")
    if [ "$_got" = "$_want" ]; then
        printf 'PASS  %s\n' "$_desc"
        _PASS=$(( _PASS + 1 ))
    else
        printf 'FAIL  %-44s  got="%s"  want="%s"\n' "$_desc" "$_got" "$_want"
        _FAIL=$(( _FAIL + 1 ))
    fi
}

# -- LG / HITACHI-LG (HL-DT-ST) ------------------------------------------------
# Disc-type tokens seen: BD-RE, DVD+-RW, DVDRAM, DVD-ROM, DVD A
# Platform chip MT1959 is the LibreDrive-capable generation.
echo '--- LG / HITACHI-LG (HL-DT-ST) ---'
_check "WH16NS60 product-only"              "BD-RE WH16NS60"              "WH16NS60"
_check "WH16NS60 full SCSI inquiry"         "HL-DT-ST BD-RE WH16NS60"     "WH16NS60"
_check "WH16NS60 SCSI-padded (16 B field)"  "BD-RE WH16NS60  "             "WH16NS60"
_check "BU40N slim (MT1959, UHD official)"  "BD-RE BU40N"                  "BU40N"
_check "BH16NS55 (cross-flash base)"        "BD-RE BH16NS55"               "BH16NS55"
_check "WH14NS40 (UHD-friendly)"            "BD-RE WH14NS40"               "WH14NS40"
_check "WH16NS40 (UHD-friendly)"            "BD-RE WH16NS40"               "WH16NS40"
_check "BH14NS48 (top CD accuracy 99.73%)"  "BD-RE BH14NS48"               "BH14NS48"
_check "BH12NS38 (high CD accuracy)"        "BD-RE BH12NS38"               "BH12NS38"
_check "GT30N (slim DVD, literal DVD+-RW)"  "DVD+-RW GT30N"                "GT30N"
_check "GT51N (slim DVD)"                   "DVDRAM GT51N"                 "GT51N"
_check "GP10NB20 (ext slim, DVDRAM type)"   "DVDRAM GP10NB20"              "GP10NB20"
_check "BP60NB10 (ext slim USB, UHD)"       "BD-RE BP60NB10"               "BP60NB10"
_check "BP50NB40 (ext slim USB)"            "BD-RE BP50NB40"               "BP50NB40"
_check "BE16NU50 (ext slim USB)"            "BD-RE BE16NU50"               "BE16NU50"

# -- Pioneer -------------------------------------------------------------------
# Pioneer reports BD-RW (not BD-RE) even for rewritable drives — intentional quirk.
# LibreDrive cut-off: firmware dates after Feb 2023 cannot be downgraded.
echo '--- Pioneer ---'
_check "BDR-S09 product-only"               "BD-RW BDR-S09"                "BDR-S09"
_check "BDR-S09 full SCSI inquiry"          "PIONEER BD-RW BDR-S09"        "BDR-S09"
_check "BDR-208M (high CD accuracy)"        "BD-RW BDR-208M"               "BDR-208M"
_check "BDR-S11JX"                          "BD-RW BDR-S11JX"              "BDR-S11JX"
_check "BDR-212DBK (LG chip, flashable)"    "BD-RW BDR-212DBK"             "BDR-212DBK"
_check "BDR-212V  (LG chip, flashable)"     "BD-RW BDR-212V"               "BDR-212V"
_check "BDR-XD07 (ext slim USB)"            "BD-RW BDR-XD07"               "BDR-XD07"
_check "BDR-XD07UHD (ext slim, UHD)"        "BD-RW BDR-XD07UHD"            "BDR-XD07UHD"
_check "BDR-XD08UMB-S (multi-hyphen model)" "BD-RW BDR-XD08UMB-S"          "BDR-XD08UMB-S"
_check "BDR-XS07UHD (slot-load, UHD)"       "BD-RW BDR-XS07UHD"            "BDR-XS07UHD"
_check "BDR-S13UBK (current flagship)"      "BD-RW BDR-S13UBK"             "BDR-S13UBK"
_check "BDR-S13U-X (premium flagship)"      "BD-RW BDR-S13U-X"             "BDR-S13U-X"
_check "BDR-UD03 (ext slim)"                "BD-RW BDR-UD03"               "BDR-UD03"
_check "DVR-221 (DVD-RW type)"              "DVD-RW DVR-221"               "DVR-221"
_check "DVR-K17LF (slim DVD)"               "DVD-RW DVR-K17LF"             "DVR-K17LF"

# -- ASUS ----------------------------------------------------------------------
# BW-16D1HT is the last widely-available new UHD-flashable drive as of 2025.
# The DVD-E6xx series have no disc-type prefix — the product string IS the model.
echo '--- ASUS ---'
_check "BW-16D1HT (UHD-friendly, in-prod)"  "BD-RE BW-16D1HT"              "BW-16D1HT"
_check "BW-16D1HT full inquiry"             "ASUS BD-RE BW-16D1HT"         "BW-16D1HT"
_check "BC-12B1ST (BD-ROM reader)"          "BD-ROM BC-12B1ST"             "BC-12B1ST"
_check "DVD-E616P2 (no prefix, 1 token)"    "DVD-E616P2"                   "DVD-E616P2"
_check "DVD-E616A  (no prefix, 1 token)"    "DVD-E616A"                    "DVD-E616A"

# -- Panasonic / MATSHITA ------------------------------------------------------
# Slim laptop form factor; decades-long AccurateRip dataset.
# UJ-812 has been consistently high-accuracy since the early 2000s.
# DVDRAM vs DVD-RAM: Panasonic uses DVD-RAM (hyphenated), LG uses DVDRAM.
# Region/variant suffix (ASW, AFW, ...) is part of the model — do not strip.
echo '--- Panasonic / MATSHITA ---'
_check "UJ-812 (DVD-RAM type, hyphen)"      "DVD-RAM UJ-812"               "UJ-812"
_check "UJ-85J (DVD-R type)"                "DVD-R UJ-85J"                 "UJ-85J"
_check "UJ160  (BD-CMB type)"               "BD-CMB UJ160"                 "UJ160"
_check "UJ172  (BD-CMB type)"               "BD-CMB UJ172"                 "UJ172"
_check "UJ262  (BD-MLT type)"               "BD-MLT UJ262"                 "UJ262"
_check "UJ240AFW (BD-MLT, AFW variant)"     "BD-MLT UJ240AFW"              "UJ240AFW"
_check "UJ8A0ASW (DVDRAM, ASW variant)"     "DVDRAM UJ8A0ASW"              "UJ8A0ASW"
_check "UJ8G6  (DVD-RAM)"                   "DVD-RAM UJ8G6"                "UJ8G6"
_check "UJ8HC  (DVD-RAM)"                   "DVD-RAM UJ8HC"                "UJ8HC"
_check "UJ892  (DVD+-RW)"                   "DVD+-RW UJ892"                "UJ892"
_check "UJ898  (DVD-R)"                     "DVD-R UJ898"                  "UJ898"

# -- Samsung / TSSTcorp --------------------------------------------------------
# TSSTcorp = Toshiba Samsung Storage Technology. Vendor strings vary:
# "SAMSUNG", "TSSTcorp", or "HL-DT-ST" for rebadged units.
# BDDVDW / CDDVDW / DVDWBD are Samsung-specific composite type tokens.
echo '--- Samsung / TSSTcorp ---'
_check "SE-506BB product-only"              "BDDVDW SE-506BB"              "SE-506BB"
_check "SE-506BB full SCSI inquiry"         "TSSTcorp BDDVDW SE-506BB"     "SE-506BB"
_check "SH-216DB (CDDVDW type)"            "CDDVDW SH-216DB"              "SH-216DB"
_check "TS-H353C (DVD-ROM type)"            "DVD-ROM TS-H353C"             "TS-H353C"
_check "TS-LB23D (DVDWBD type)"            "DVDWBD TS-LB23D"              "TS-LB23D"
_check "SE-218CN (CDDVDW)"                 "CDDVDW SE-218CN"              "SE-218CN"
_check "SN-506AB (slim BD)"                "BDDVDW SN-506AB"              "SN-506AB"

# -- Philips / Lite-On (PLDS) --------------------------------------------------
# "DVD A" is a real two-word disc-type token used by PLDS and HP slim drives.
echo '--- Philips / Lite-On (PLDS) ---'
_check "DU-8A5HH (DVD+-RW)"                "DVD+-RW DU-8A5HH"             "DU-8A5HH"
_check "DA8A6SH  (DVD-RW)"                 "DVD-RW DA8A6SH"               "DA8A6SH"
_check "DS8A5S   (DVD A two-word type)"    "DVD A DS8A5S"                 "DS8A5S"
_check "DS8A9SH  (DVD-RW)"                 "DVD-RW DS8A9SH"               "DS8A9SH"
_check "DA8AESH  (DVD A)"                  "DVD A DA8AESH"                "DA8AESH"
_check "iHAS324  (DVD A)"                  "DVD A iHAS324"                "iHAS324"
_check "iHES212  (BD reader)"              "BD-RE iHES212"                "iHES212"
_check "SOHR-5239V (CD-RW)"               "CD-RW SOHR-5239V"             "SOHR-5239V"

# -- HP (rebadged Lite-On / Panasonic) ----------------------------------------
echo '--- HP ---'
_check "SU-208CB (CDDVDW, Lite-On OEM)"    "CDDVDW SU-208CB"              "SU-208CB"
_check "SU-208FB (CDDVDW)"                 "CDDVDW SU-208FB"              "SU-208FB"
_check "GUE1N    (DVD A, top slim 2022)"   "DVD A GUE1N"                  "GUE1N"
_check "SU208GB  (DVDRW)"                  "DVDRW SU208GB"                "SU208GB"
_check "DU8A5SH  (DVD A)"                  "DVD A DU8A5SH"                "DU8A5SH"
_check "GHB0N    (DVD-RAM, Panasonic OEM)" "DVD-RAM GHB0N"                "GHB0N"

# -- Sony / Optiarc ------------------------------------------------------------
# Sony uses "DVD RW" (space, no hyphen) — distinct from "DVD-RW".
# This is the only family that does this; do not normalise it away.
echo '--- Sony / Optiarc ---'
_check "DW-D22A product-only (DVD RW)"     "DVD RW DW-D22A"               "DW-D22A"
_check "DW-D22A full SCSI inquiry"         "SONY DVD RW DW-D22A"          "DW-D22A"

# -- Plextor (legacy) ----------------------------------------------------------
# Plextor uses CD-R (not CD-RW) even for rewritable drives in some product IDs.
# DVDR (no hyphen) is Plextor's DVD type token.
echo '--- Plextor ---'
_check "PX-W1610A (CD-R type)"             "CD-R PX-W1610A"               "PX-W1610A"
_check "PX-716A   (DVDR type)"             "DVDR PX-716A"                 "PX-716A"
_check "PX-760A   (DVDR type)"             "DVDR PX-760A"                 "PX-760A"

# -- TEAC ----------------------------------------------------------------------
# Some TEAC drives report no disc-type prefix at all.
echo '--- TEAC ---'
_check "DV-W28EA (bare model, no prefix)"  "DV-W28EA"                     "DV-W28EA"

# -- empty / degenerate --------------------------------------------------------
echo '--- degenerate ---'
_check "empty string -> empty"             ""                            ""
_check "whitespace only -> empty"          "   "                         ""

# -- vendor canonicalization (INQUIRY string -> community/brand name) ----------
# AccurateRip / MakeMKV / forums all use the brand, never the OEM joint-venture
# string. Only these differ; real brands pass through unchanged.
_vcheck() {
    _got=$(vendor_canonical "$2")
    if [ "$_got" = "$3" ]; then
        printf 'PASS  vendor %s\n' "$1"; _PASS=$(( _PASS + 1 ))
    else
        printf 'FAIL  vendor %-38s  got="%s"  want="%s"\n' "$1" "$_got" "$3"; _FAIL=$(( _FAIL + 1 ))
    fi
}
echo '--- vendor canonicalization ---'
_vcheck "HL-DT-ST -> LG"          "HL-DT-ST"   "LG"
_vcheck "MATSHITA -> Panasonic"   "MATSHITA"   "Panasonic"
_vcheck "TSSTcorp -> Samsung"     "TSSTcorp"   "Samsung"
_vcheck "PLDS -> Lite-On"         "PLDS"       "Lite-On"
_vcheck "Slimtype -> Lite-On"     "Slimtype"   "Lite-On"
_vcheck "LITEON -> Lite-On"       "LITEON"     "Lite-On"
_vcheck "Sony NEC -> Optiarc"     "Sony NEC"   "Optiarc"
_vcheck "LG passes through"       "LG"         "LG"
_vcheck "PIONEER passes through"  "PIONEER"    "PIONEER"
_vcheck "ASUS passes through"     "ASUS"       "ASUS"
_vcheck "Pioneer passes through"  "Pioneer"    "Pioneer"
_vcheck "Lite-On passes through"  "Lite-On"    "Lite-On"
_vcheck "TEAC passes through"     "TEAC"       "TEAC"

# -- summary -------------------------------------------------------------------
printf '\n%d passed, %d failed\n' "$_PASS" "$_FAIL"
[ "$_FAIL" -eq 0 ]
