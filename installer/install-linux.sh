#!/usr/bin/env bash
#
# Installerer TheSuperHackers GeneralsGameCode (Zero Hour) build i en eksisterende
# Command & Conquer Generals - Zero Hour installation (Steam/Proton, Wine, Lutris).
#
# Indstillingerne (repo, build-tag m.m.) læses fra zh-install.conf ved siden af scriptet.
# Miljøvariabler går forud for config-filen.
#
# Brug:
#   ./install-linux.sh                      # Bruger zh-install.conf, finder Zero Hour-mappen automatisk
#   ./install-linux.sh "/sti/til/Zero Hour" # Angiv mappen manuelt
#   TAG=latest ./install-linux.sh           # Nyeste release i REPO (standard)
#   TAG=historical-2026-10-10-18449d8 ./install-linux.sh
#   REPO=TheSuperHackers/GeneralsGameCode TAG=weekly-2026-10-02 ./install-linux.sh  # Officiel build
#   INCLUDE_PDB=1 ./install-linux.sh        # Medtag debug-symboler (.pdb)
#   SKIP_GENLAUNCHER=1 ./install-linux.sh   # Spring GenLauncher-installation over
#   ZH_CONFIG=/sti/til/andet.conf ./install-linux.sh
#
set -euo pipefail

CONFIG_KEYS=(REPO TAG INSTALL_DIR INCLUDE_PDB SKIP_GENLAUNCHER)
CONFIG_FILE="${ZH_CONFIG:-$(dirname "$(readlink -f "$0")")/zh-install.conf}"
MARKER_FILE="INIZH.big"
STEAM_APP_ID="2732960"
GENLAUNCHER_MANIFEST="https://raw.githubusercontent.com/p0ls3r/GenLauncherModsData/master/ReposModificationDataZH3.yaml"
GENLAUNCHER_FALLBACK_URL="https://gen.insave.ovh/genlauncher/launcher/GenLauncher1010.zip"

info()  { printf '\e[1;34m[INFO]\e[0m %s\n' "$*"; }
warn()  { printf '\e[1;33m[ADVARSEL]\e[0m %s\n' "$*"; }
fail()  { printf '\e[1;31m[FEJL]\e[0m %s\n' "$*" >&2; exit 1; }

# Læser NØGLE=værdi fra config-filen. Filen køres ikke som kode, og en værdi sættes kun
# hvis variablen ikke allerede er sat i miljøet.
load_config() {
    local file="$1" line key value known k
    [[ -f "$file" ]] || return 0
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        [[ "$line" =~ ^[[:space:]]*(#|$) ]] && continue
        if [[ ! "$line" =~ ^[[:space:]]*([A-Z_]+)[[:space:]]*=(.*)$ ]]; then
            warn "Ugyldig linje i $file: $line"
            continue
        fi
        key="${BASH_REMATCH[1]}"
        value="${BASH_REMATCH[2]}"
        value="${value#"${value%%[![:space:]]*}"}"
        value="${value%"${value##*[![:space:]]}"}"
        if [[ "$value" =~ ^\"(.*)\"$ || "$value" =~ ^\'(.*)\'$ ]]; then
            value="${BASH_REMATCH[1]}"
        fi
        known=0
        for k in "${CONFIG_KEYS[@]}"; do
            [[ "$k" == "$key" ]] && known=1
        done
        if [[ "$known" != "1" ]]; then
            warn "Ukendt nøgle i $file: $key"
            continue
        fi
        if [[ -z "${!key+x}" ]]; then
            printf -v "$key" '%s' "$value"
        fi
    done < "$file"
}

load_config "$CONFIG_FILE"

# Standardværdier hvis hverken miljø eller config-fil angiver dem.
REPO="${REPO:-Tricky12321/CnC_Generals_Zero_Hour}"
TAG="${TAG:-latest}"
INSTALL_DIR="${INSTALL_DIR:-}"
INCLUDE_PDB="${INCLUDE_PDB:-0}"
SKIP_GENLAUNCHER="${SKIP_GENLAUNCHER:-0}"

require_tool() {
    command -v "$1" >/dev/null 2>&1
}

is_zh_dir() {
    [[ -n "$1" && -d "$1" && -f "$1/$MARKER_FILE" ]]
}

# Finder alle Steam-biblioteker (inkl. ekstra biblioteker fra libraryfolders.vdf)
steam_libraries() {
    local roots=(
        "$HOME/.local/share/Steam"
        "$HOME/.steam/steam"
        "$HOME/.steam/root"
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
        "$HOME/snap/steam/common/.local/share/Steam"
    )
    local root vdf
    for root in "${roots[@]}"; do
        [[ -d "$root/steamapps" ]] && echo "$root"
        vdf="$root/steamapps/libraryfolders.vdf"
        if [[ -f "$vdf" ]]; then
            grep -E '^\s*"path"' "$vdf" | sed -E 's/^\s*"path"\s*"(.*)"\s*$/\1/' | sed 's/\\\\/\//g'
        fi
    done | awk '!seen[$0]++'
}

find_zh_dir() {
    local lib name candidate
    local steam_names=(
        "Command & Conquer Generals - Zero Hour"
        "Command and Conquer Generals Zero Hour"
        "Command & Conquer Generals Zero Hour"
    )
    while IFS= read -r lib; do
        # Steam-manifestet angiver den præcise installationsmappe
        local manifest="$lib/steamapps/appmanifest_${STEAM_APP_ID}.acf"
        if [[ -f "$manifest" ]]; then
            name="$(grep -E '^\s*"installdir"' "$manifest" | sed -E 's/^\s*"installdir"\s*"(.*)"\s*$/\1/')"
            candidate="$lib/steamapps/common/$name"
            if is_zh_dir "$candidate"; then
                echo "$candidate"
                return 0
            fi
        fi
        for name in "${steam_names[@]}"; do
            candidate="$lib/steamapps/common/$name"
            if is_zh_dir "$candidate"; then
                echo "$candidate"
                return 0
            fi
        done
    done < <(steam_libraries)

    # Wine/Lutris-præfikser: søg efter markørfilen i almindelige placeringer
    local search_roots=("$HOME/.wine" "$HOME/Games" "$HOME/.local/share/lutris" "$HOME/.local/share/bottles")
    local root found
    for root in "${search_roots[@]}"; do
        [[ -d "$root" ]] || continue
        found="$(find "$root" -maxdepth 8 -type f -name "$MARKER_FILE" 2>/dev/null | head -n 1 || true)"
        if [[ -n "$found" ]]; then
            dirname "$found"
            return 0
        fi
    done
    return 1
}

download_to_stdout() {
    if require_tool curl; then
        curl -fsL --retry 3 "$1"
    elif require_tool wget; then
        wget -qO- "$1"
    else
        return 1
    fi
}

download() {
    local url="$1" out="$2"
    if require_tool curl; then
        curl -fL --retry 3 --progress-bar -o "$out" "$url"
    elif require_tool wget; then
        wget -q --show-progress -O "$out" "$url"
    else
        fail "Hverken curl eller wget er installeret."
    fi
}

# Finder tagget på den nyeste release i REPO med en Zero Hour-zip (generalszh-<tag>.zip).
# GitHub returnerer releases nyeste først, også pre-releases.
resolve_latest_tag() {
    local json tags
    json="$(download_to_stdout "https://api.github.com/repos/${REPO}/releases?per_page=30")" || return 0
    tags="$(printf '%s\n' "$json" \
        | grep -oE '"browser_download_url"[[:space:]]*:[[:space:]]*"[^"]*"' \
        | sed -nE 's#.*/releases/download/([^/"]+)/generalszh-\1\.zip"$#\1#p')" || return 0
    printf '%s\n' "${tags%%$'\n'*}"
}

extract() {
    local zip="$1" dest="$2"
    if require_tool unzip; then
        unzip -q -o "$zip" -d "$dest"
    elif require_tool bsdtar; then
        bsdtar -xf "$zip" -C "$dest"
    elif require_tool python3; then
        python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$zip" "$dest"
    else
        fail "Kan ikke udpakke: installer unzip, bsdtar eller python3."
    fi
}

# Installerer GenLauncher.exe i spilmappen, hvis den ikke allerede findes
install_genlauncher() {
    local zh_dir="$1" work="$2"

    if [[ "$SKIP_GENLAUNCHER" == "1" ]]; then
        return 0
    fi
    if [[ -f "$zh_dir/GenLauncher.exe" ]]; then
        info "GenLauncher er allerede installeret - springer over."
        return 0
    fi

    # Download-linket til nyeste version står i GenLauncher's eget manifest
    local url
    url="$(download_to_stdout "$GENLAUNCHER_MANIFEST" 2>/dev/null | grep -E '^DownloadLink:' | sed -E 's/^DownloadLink:\s*//' | tr -d '\r' || true)"
    if [[ -z "$url" ]]; then
        warn "Kunne ikke læse GenLauncher-manifestet - bruger fast link."
        url="$GENLAUNCHER_FALLBACK_URL"
    fi

    info "Downloader GenLauncher fra $url ..."
    if ! download "$url" "$work/genlauncher.zip"; then
        warn "GenLauncher-download fejlede - springer over."
        return 0
    fi

    mkdir -p "$work/genlauncher"
    extract "$work/genlauncher.zip" "$work/genlauncher"
    if [[ ! -f "$work/genlauncher/GenLauncher.exe" ]]; then
        warn "GenLauncher.exe mangler i arkivet - springer over."
        return 0
    fi

    cp -f "$work/genlauncher/GenLauncher.exe" "$zh_dir/GenLauncher.exe"
    info "GenLauncher installeret: $zh_dir/GenLauncher.exe"
    warn "GenLauncher kræver .NET Framework 4.8 i Proton-præfikset: protontricks $STEAM_APP_ID dotnet48"
}

main() {
    local zh_dir="${1:-$INSTALL_DIR}"

    if [[ -f "$CONFIG_FILE" ]]; then
        info "Config: $CONFIG_FILE"
    else
        warn "Ingen config-fil ($CONFIG_FILE) - bruger standardværdier."
    fi
    if [[ "$TAG" == "latest" ]]; then
        TAG="$(resolve_latest_tag)"
        [[ -n "$TAG" ]] || fail "Kunne ikke finde nyeste release med en Zero Hour-zip i $REPO (GitHub utilgængelig?). Angiv TAG i zh-install.conf."
        info "Nyeste release: $TAG"
    fi
    ASSET="generalszh-${TAG}.zip"
    URL="https://github.com/${REPO}/releases/download/${TAG}/${ASSET}"
    info "Build: $REPO ($TAG)"

    if [[ -n "$zh_dir" ]]; then
        is_zh_dir "$zh_dir" || fail "'$zh_dir' ligner ikke en Zero Hour-mappe ($MARKER_FILE mangler)."
    else
        info "Leder efter Zero Hour-installation..."
        zh_dir="$(find_zh_dir)" || fail "Zero Hour blev ikke fundet. Angiv mappen: $0 \"/sti/til/Zero Hour\""
    fi
    info "Zero Hour-mappe: $zh_dir"
    [[ -w "$zh_dir" ]] || fail "Ingen skriveadgang til '$zh_dir'."

    local work
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT

    info "Downloader $ASSET ..."
    download "$URL" "$work/$ASSET" || fail "Download fejlede: $URL"

    info "Pakker ud..."
    mkdir -p "$work/extracted"
    extract "$work/$ASSET" "$work/extracted"
    [[ -f "$work/extracted/generalszh.exe" ]] || fail "generalszh.exe mangler i arkivet."

    local backup_dir="$zh_dir/_backup_superhackers_$(date +%Y-%m-%d_%H%M%S)"
    local file name installed=0
    for file in "$work/extracted"/*; do
        [[ -f "$file" ]] || continue
        name="$(basename "$file")"
        if [[ "$INCLUDE_PDB" != "1" && "$name" == *.pdb ]]; then
            continue
        fi
        if [[ -f "$zh_dir/$name" ]]; then
            mkdir -p "$backup_dir"
            cp -p "$zh_dir/$name" "$backup_dir/$name"
        fi
        cp -f "$file" "$zh_dir/$name"
        installed=$((installed + 1))
    done

    echo "$REPO $TAG" > "$zh_dir/superhackers-version.txt"

    install_genlauncher "$zh_dir" "$work"

    info "Installeret $installed filer fra $REPO ($TAG)."
    warn "Alle spillere i et LAN-spil skal køre samme build ($TAG), ellers kommer der mismatch."
    [[ -d "$backup_dir" ]] && info "Backup af overskrevne filer: $backup_dir"
    echo
    info "Start spillet med: $zh_dir/generalszh.exe"
    info "Steam (Proton): Højreklik spillet -> Egenskaber -> Startindstillinger, og indsæt:"
    echo "      bash -c 'exec \"\${@/[Gg]enerals.exe/generalszh.exe}\"' -- %command%"
    info "Eksisterende env-vars/argumenter bevares, fx:"
    echo "      WINEDLLOVERRIDES=\"d3d8=n,b;d3d9=n,b;\" bash -c 'exec \"\${@/[Gg]enerals.exe/generalszh.exe}\"' -- %command% -proxy dxvk8.dll"
    warn "Virker det ikke, kan generalszh.exe tilføjes som 'Non-Steam game' og køres med Proton."
}

main "$@"
