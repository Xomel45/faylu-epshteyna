#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════════════
# deploy.sh — упаковка и развёртывание артефактов сборки faylu-epshteyna
# ══════════════════════════════════════════════════════════════════════════════
#
# Использование:
#   ./deploy.sh beta                  — сырой ELF → builds/beta/
#   ./deploy.sh release               — все платформы → builds/releases/VERSION-*/
#   ./deploy.sh release linux         — .tar.gz → builds/releases/VERSION-linux/
#   ./deploy.sh release win           — .exe → builds/releases/VERSION-windows/
#
# Опции:
#   --build     Пересобрать проект перед деплоем (через CMake)
#   --clean     Удалить целевую директорию перед копированием
#
# Примеры:
#   ./deploy.sh beta --build              Собрать и задеплоить beta
#   ./deploy.sh release linux --clean     .tar.gz поверх очищенной директории
#   ./deploy.sh release --build --clean   Полный цикл: сборка + релиз всех платформ
# ══════════════════════════════════════════════════════════════════════════════

set -euo pipefail

# ── Цвета и вывод ─────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

log()    { echo -e "${BLUE}[DEPLOY]${NC} $*"; }
ok()     { echo -e "${GREEN}[  OK  ]${NC} $*"; }
warn()   { echo -e "${YELLOW}[ WARN ]${NC} $*"; }
fail()   { echo -e "${RED}[ERROR ]${NC} $*" >&2; exit 1; }
header() { echo -e "\n${BOLD}${CYAN}══ $* ══${NC}"; }
rule()   { printf "${CYAN}%.0s─${NC}" {1..60}; echo; }

# ── Всегда запускаемся из корня проекта ───────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ── Константы путей ───────────────────────────────────────────────────────────
APP_NAME="faylu-epshteyna"
BUILD_LINUX="build"                                # выход linux cmake
BUILD_WIN="build-win"                              # выход windows cmake (cross)
BUILDS_DIR="builds"                                # корень артефактов

# ── Чтение версии из CMakeLists.txt ──────────────────────────────────────────
# Берём из строки вида: project(faylu-epshteyna VERSION 5.1 LANGUAGES C)
get_version() {
    local v
    v=$(sed -n 's/^project([^ ]* VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -1)
    echo "${v:-unknown}"
}
VERSION=$(get_version)

# ── Парсинг аргументов ────────────────────────────────────────────────────────
MODE="${1:-help}"
PLATFORM="${2:-both}"
DO_BUILD=false
DO_CLEAN=false

for arg in "$@"; do
    [[ "$arg" == "--build" ]] && DO_BUILD=true
    [[ "$arg" == "--clean" ]] && DO_CLEAN=true
done

# ── Вспомогательные функции ───────────────────────────────────────────────────

file_size() { du -h "$1" 2>/dev/null | cut -f1 || echo "?"; }

git_hash() { git rev-parse --short HEAD 2>/dev/null || echo "unknown"; }

write_build_info() {
    local dir="$1"
    printf "version=%s\nbuilt=%s\ncommit=%s\nplatform=%s\n" \
        "$VERSION" \
        "$(date '+%Y-%m-%d %H:%M:%S')" \
        "$(git_hash)" \
        "${2:-linux}" \
        > "$dir/build-info.txt"
}

# Определяет семейство дистрибутива по /etc/os-release.
# Возвращает: pkg | deb | rpm | unknown
detect_pkg_format() {
    local id="" id_like=""
    if [[ -f /etc/os-release ]]; then
        id=$(. /etc/os-release && echo "${ID:-}")
        id_like=$(. /etc/os-release && echo "${ID_LIKE:-}")
    fi
    local combined="${id} ${id_like}"

    local arch_re='arch|manjaro|endeavouros|garuda|artix|parabola|cachyos|blackarch'
    arch_re+='|archlabs|archcraft|arcolinux|archman|archstrike|bluestar|crystal'
    arch_re+='|ctlos|hyperbola|kaos|librewish|obarun|rebornos|anarchy|axyl|snal'
    arch_re+='|steamos'

    local deb_re='debian|ubuntu|linuxmint|raspbian|raspios|kali|elementary|zorin'
    deb_re+='|popos|pop_os|backbox|parrot|tails|deepin|mx|antix|devuan|pureos'
    deb_re+='|sparky|lmde|bunsenlabs|crunchbang|bodhi|mobian|armbian|siduction'
    deb_re+='|solydxk|trisquel|netrunner|nitrux|regolith|q4os|peppermint|lite'
    deb_re+='|neon|kubuntu|lubuntu|xubuntu|endless|astra|knoppix|wubuntu'

    local rpm_re='fedora|rhel|centos|rocky|almalinux|opensuse|suse|oracle|scientific'
    rpm_re+='|springdale|eurolinux|clearos|cloudlinux|mageia|pclinuxos|openmandriva'
    rpm_re+='|rosa|nobara|ultramarine|bazzite|aurora|bluefin|coreos|qubes|asahi'
    rpm_re+='|turbolinux|vine|alt|mandriva|centos-stream|circle'

    if   echo "$combined" | grep -qiE "$arch_re"; then echo "pkg"
    elif echo "$combined" | grep -qiE "$deb_re";  then echo "deb"
    elif echo "$combined" | grep -qiE "$rpm_re";  then echo "rpm"
    else echo "unknown"
    fi
}

# make_zip <src> <out.zip>
make_zip() {
    local src="$1" zip_out="$2"
    local abs_zip
    abs_zip="$(realpath -m "$zip_out")"
    log "ZIP: $(basename "$abs_zip")..."
    rm -f "$abs_zip"
    if [[ -d "$src" ]]; then
        (cd "$(dirname "$src")" && zip -9 -r "$abs_zip" "$(basename "$src")")
    else
        (cd "$(dirname "$src")" && zip -9 "$abs_zip" "$(basename "$src")")
    fi
    ok "  + $(basename "$abs_zip")  ($(file_size "$abs_zip"))"
}

copy_file() {
    local src="$1" dst="$2" label="$3"
    if [[ -f "$src" ]]; then
        cp "$src" "$dst"
        ok "  + ${label:-$(basename "$src")}"
        return 0
    else
        warn "  ? Не найден: ${label:-$(basename "$src")}"
        return 1
    fi
}

ensure_builds_tree() {
    mkdir -p "$BUILDS_DIR/beta"
    mkdir -p "$BUILDS_DIR/releases"
}

# Безопасное удаление — только внутри BUILDS_DIR.
safe_clean() {
    local target="$1"
    local abs_builds abs_target
    abs_builds="$(realpath -m "$BUILDS_DIR")"
    abs_target="$(realpath -m "$target")"

    if [[ "$abs_target" != "$abs_builds/"* && "$abs_target" != "$abs_builds" ]]; then
        fail "safe_clean: цель '${target}' (→ ${abs_target}) находится вне ${abs_builds} — отказ в удалении!"
    fi

    log "Очистка $target ..."
    rm -rf "${target:?}"
}

# ── Сборка (опциональная) ─────────────────────────────────────────────────────

build_linux() {
    header "Сборка Linux (Release)"
    cmake -B "$BUILD_LINUX" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD_LINUX" --parallel "$(( $(nproc) - 2 ))"
    ok "Linux бинарник собран: $BUILD_LINUX/$APP_NAME"
}

build_windows() {
    header "Сборка Windows (cross-compile MinGW)"
    cmake -B "$BUILD_WIN" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
    cmake --build "$BUILD_WIN" --parallel "$(( $(nproc) - 2 ))"
    ok "Windows .exe собран: $BUILD_WIN/$APP_NAME.exe"
}

# ── РЕЖИМ: Beta ───────────────────────────────────────────────────────────────
# Назначение: быстрая проверка, итерации.
# Артефакт:   сырой ELF.
# Путь:       builds/beta/faylu-epshteyna
deploy_beta() {
    header "Beta Deploy → ${BUILDS_DIR}/beta/"
    ensure_builds_tree

    if $DO_BUILD || [[ ! -f "$BUILD_LINUX/$APP_NAME" ]]; then
        build_linux
    fi

    local dest="${BUILDS_DIR}/beta"
    $DO_CLEAN && { safe_clean "$dest"; mkdir -p "$dest"; }

    cp "$BUILD_LINUX/$APP_NAME" "$dest/$APP_NAME"
    chmod +x "$dest/$APP_NAME"
    write_build_info "$dest" "linux-beta"

    rule
    ok "Beta готов!"
    echo "  Путь:    ${BOLD}$dest/$APP_NAME${NC}"
    echo "  Размер:  $(file_size "$dest/$APP_NAME")"
    echo "  Версия:  $VERSION  |  commit: $(git_hash)"
    echo ""
    echo "  Запуск:"
    echo "    ./$dest/$APP_NAME --url <URL>"
    echo ""
}

# ── РЕЖИМ: Release Linux (.tar.gz) ───────────────────────────────────────────
# Назначение: финальный дистрибутив для Linux.
# Артефакт:   faylu-epshteyna-VERSION-linux-x86_64.tar.gz
# Путь:       builds/releases/VERSION-linux/
deploy_release_linux() {
    header "Release Linux → ${BUILDS_DIR}/releases/${VERSION}-linux/"
    ensure_builds_tree

    if $DO_BUILD || [[ ! -f "$BUILD_LINUX/$APP_NAME" ]]; then
        build_linux
    fi

    local dest="${BUILDS_DIR}/releases/${VERSION}-linux"
    local tarball_name="${APP_NAME}-${VERSION}-linux-x86_64.tar.gz"

    $DO_CLEAN && { safe_clean "$dest"; }
    mkdir -p "$dest"

    cp "$BUILD_LINUX/$APP_NAME" "$dest/$APP_NAME"
    chmod +x "$dest/$APP_NAME"
    write_build_info "$dest" "linux"

    tar -czf "$dest/$tarball_name" -C "$dest" "$APP_NAME" build-info.txt

    rule
    ok "Release Linux готов!"
    echo "  Директория:  ${BOLD}$dest/${NC}"
    echo "  Tarball:     $tarball_name  ($(file_size "$dest/$tarball_name"))"
    echo "  Версия:      $VERSION  |  commit: $(git_hash)"
    echo ""
}

# ── РЕЖИМ: Release Windows (.exe) ────────────────────────────────────────────
# Назначение: финальный дистрибутив для Windows.
# Артефакт:   статический .exe (никаких DLL не нужно) + .zip
# Путь:       builds/releases/VERSION-windows/
deploy_release_windows() {
    header "Release Windows → ${BUILDS_DIR}/releases/${VERSION}-windows/"
    ensure_builds_tree

    if $DO_BUILD || [[ ! -f "$BUILD_WIN/$APP_NAME.exe" ]]; then
        build_windows
    fi

    local dest="${BUILDS_DIR}/releases/${VERSION}-windows"

    $DO_CLEAN && { safe_clean "$dest"; }
    mkdir -p "$dest"

    copy_file "$BUILD_WIN/$APP_NAME.exe" "$dest/$APP_NAME.exe" "$APP_NAME.exe"
    write_build_info "$dest" "windows"

    cat > "$dest/README.txt" << EOF
faylu-epshteyna v${VERSION} — Windows Release
===============================================

Требования / Requirements:
  - Windows 10 / 11 (x86_64)

Запуск / Launch:
  faylu-epshteyna.exe --url <URL>

Опции:
  --url <URL>        URL видео
  --output <DIR>     папка для сохранения
  --quality <N>      ограничить качество (напр. 720)
  --no-proxy         прямое подключение
  --system-proxy     системный прокси (HTTP_PROXY/HTTPS_PROXY из env)
  --user-agent <UA>  кастомный User-Agent

Версия:  ${VERSION}
Сборка:  $(date '+%Y-%m-%d')
Коммит:  $(git_hash)
EOF
    ok "  + README.txt"

    make_zip "$dest" "${BUILDS_DIR}/releases/${VERSION}-windows.zip"

    rule
    ok "Release Windows готов!"
    echo "  Директория:  ${BOLD}$dest/${NC}"
    echo "  ZIP:         ${VERSION}-windows.zip  ($(file_size "${BUILDS_DIR}/releases/${VERSION}-windows.zip"))"
    echo "  .exe:        $APP_NAME.exe  ($(file_size "$dest/$APP_NAME.exe"))"
    echo "  (статический бинарник — DLL не нужны)"
    echo ""
}

# ── РЕЖИМ: Release Linux (.pkg.tar.zst — Arch Linux) ─────────────────────────
# Назначение: нативный пакет для Arch Linux / pacman.
# Артефакт:   faylu-epshteyna-VERSION-1-x86_64.pkg.tar.zst
# Путь:       builds/releases/VERSION-linux/
# Установка:  sudo pacman -U faylu-epshteyna-VERSION-1-x86_64.pkg.tar.zst
deploy_release_pkg() {
    header "Release .pkg.tar.zst → ${BUILDS_DIR}/releases/${VERSION}-linux/"
    ensure_builds_tree

    if ! command -v zstd &>/dev/null; then
        fail "zstd не найден. Установите: sudo pacman -S zstd"
    fi

    if $DO_BUILD || [[ ! -f "$BUILD_LINUX/$APP_NAME" ]]; then
        build_linux
    fi

    local dest="${BUILDS_DIR}/releases/${VERSION}-linux"
    local pkgver="${VERSION}-1"
    local pkg_name="${APP_NAME}-${pkgver}-x86_64.pkg.tar.zst"
    local staging="${BUILD_LINUX}/pkg-staging"

    $DO_CLEAN && { safe_clean "$dest"; }
    mkdir -p "$dest"

    rm -rf "$staging"
    mkdir -p "$staging/usr/bin"

    cp "$BUILD_LINUX/$APP_NAME" "$staging/usr/bin/"
    chmod 755 "$staging/usr/bin/$APP_NAME"
    ok "  + usr/bin/$APP_NAME"

    local installed_size
    installed_size=$(du -sb "$staging" | cut -f1)

    cat > "$staging/.PKGINFO" << PKGINFO_EOF
pkgname = ${APP_NAME}
pkgver = ${pkgver}
pkgdesc = Загрузчик видео с нативными экстракторами и fallback на yt-dlp
url = https://github.com/xomel45/${APP_NAME}
builddate = $(date +%s)
packager = xomel45 <xom.xom.zip@gmail.com>
size = ${installed_size}
arch = x86_64
depend = curl
depend = openssl
PKGINFO_EOF
    ok "  + .PKGINFO  (depends: curl, openssl)"

    tar --zstd -cf "$dest/$pkg_name" -C "$staging" .PKGINFO usr
    rm -rf "$staging"

    rule
    ok "Release .pkg.tar.zst готов!"
    echo "  Директория:  ${BOLD}$dest/${NC}"
    echo "  Пакет:       $pkg_name"
    echo "  Размер:      $(file_size "$dest/$pkg_name")"
    echo "  Версия:      $VERSION  |  commit: $(git_hash)"
    echo ""
    echo "  Установка:"
    echo "    sudo pacman -U $dest/$pkg_name"
    echo ""
}

# ── РЕЖИМ: Release Linux (.deb) ───────────────────────────────────────────────
# Назначение: пакет для Debian / Ubuntu / Mint.
# Артефакт:   faylu-epshteyna_VERSION_amd64.deb
# Путь:       builds/releases/VERSION-linux/
# Требует:    dpkg-deb  (пакет dpkg-dev)
deploy_release_deb() {
    header "Release .deb → ${BUILDS_DIR}/releases/${VERSION}-linux/"
    ensure_builds_tree

    if ! command -v dpkg-deb &>/dev/null; then
        fail "dpkg-deb не найден. Установите: sudo pacman -S dpkg  /  sudo apt install dpkg-dev"
    fi

    if $DO_BUILD || [[ ! -f "$BUILD_LINUX/$APP_NAME" ]]; then
        build_linux
    fi

    local dest="${BUILDS_DIR}/releases/${VERSION}-linux"
    local deb_name="${APP_NAME}_${VERSION}_amd64.deb"
    local pkg_dir="${BUILD_LINUX}/deb-staging"

    $DO_CLEAN && { safe_clean "$dest"; }
    mkdir -p "$dest"

    rm -rf "$pkg_dir"
    mkdir -p "$pkg_dir/DEBIAN"
    mkdir -p "$pkg_dir/usr/bin"

    local installed_kb
    installed_kb=$(du -sk "$BUILD_LINUX/$APP_NAME" | cut -f1)

    cat > "$pkg_dir/DEBIAN/control" << CTRL_EOF
Package: ${APP_NAME}
Version: ${VERSION}
Section: net
Priority: optional
Architecture: amd64
Installed-Size: ${installed_kb}
Depends: libcurl4, libssl3 | libssl1.1
Maintainer: xomel45 <xom.xom.zip@gmail.com>
Homepage: https://github.com/xomel45/${APP_NAME}
Description: Загрузчик видео с нативными экстракторами и fallback на yt-dlp
 faylu-epshteyna скачивает видео на зашифрованный/съёмный диск.
 Поддерживает: pornhub, vk, xvideos, xhamster, xnxx, eporner, redtube,
 tiktok, bunkr, thothub и другие сайты. Fallback на yt-dlp.
CTRL_EOF

    cp "$BUILD_LINUX/$APP_NAME" "$pkg_dir/usr/bin/"
    chmod 755 "$pkg_dir/usr/bin/$APP_NAME"
    ok "  + usr/bin/$APP_NAME"

    find "$pkg_dir" -type d -exec chmod 755 {} \;

    dpkg-deb --build --root-owner-group "$pkg_dir" "$dest/$deb_name"
    rm -rf "$pkg_dir"

    rule
    ok "Release .deb готов!"
    echo "  Директория:  ${BOLD}$dest/${NC}"
    echo "  Пакет:       $deb_name"
    echo "  Размер:      $(file_size "$dest/$deb_name")"
    echo "  Версия:      $VERSION  |  commit: $(git_hash)"
    echo ""
    echo "  Установка:"
    echo "    sudo dpkg -i $deb_name"
    echo "    sudo apt-get install -f   # если нужно подтянуть зависимости"
    echo ""
}

# ── РЕЖИМ: Release Linux (.rpm) ───────────────────────────────────────────────
# Назначение: пакет для Fedora / RHEL / openSUSE.
# Артефакт:   faylu-epshteyna-VERSION-1.x86_64.rpm
# Путь:       builds/releases/VERSION-linux/
# Требует:    rpmbuild  (пакет rpm-build / rpm-tools)
deploy_release_rpm() {
    header "Release .rpm → ${BUILDS_DIR}/releases/${VERSION}-linux/"
    ensure_builds_tree

    if ! command -v rpmbuild &>/dev/null; then
        fail "rpmbuild не найден. Установите: sudo pacman -S rpm-tools  /  sudo dnf install rpm-build  /  sudo apt install rpm"
    fi

    if $DO_BUILD || [[ ! -f "$BUILD_LINUX/$APP_NAME" ]]; then
        build_linux
    fi

    local dest="${BUILDS_DIR}/releases/${VERSION}-linux"
    local rpm_version="${VERSION//-/_}"
    local rpm_name="${APP_NAME}-${rpm_version}-1.x86_64.rpm"
    local rpm_topdir="${BUILD_LINUX}/rpm-staging"
    local buildroot="${rpm_topdir}/BUILDROOT/${APP_NAME}-${rpm_version}-1.x86_64"

    $DO_CLEAN && { safe_clean "$dest"; }
    mkdir -p "$dest"

    rm -rf "$rpm_topdir"
    mkdir -p "${rpm_topdir}"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
    mkdir -p "$buildroot/usr/bin"

    cp "$BUILD_LINUX/$APP_NAME" "$buildroot/usr/bin/"
    chmod 755 "$buildroot/usr/bin/$APP_NAME"
    ok "  + usr/bin/$APP_NAME"

    local files_list
    files_list=$(find "$buildroot" \( -type f -o -type l \) \
        | sed "s|${buildroot}||" | sort)

    local changelog_date
    changelog_date=$(LC_ALL=C date '+%a %b %d %Y')

    cat > "${rpm_topdir}/SPECS/${APP_NAME}.spec" << SPEC_EOF
Name:           ${APP_NAME}
Version:        ${rpm_version}
Release:        1
Summary:        Загрузчик видео с нативными экстракторами и fallback на yt-dlp
License:        Proprietary
URL:            https://github.com/xomel45/${APP_NAME}
BuildArch:      x86_64
Requires:       libcurl >= 7.0, openssl-libs
%define __spec_install_pre %{nil}
%define _unpackaged_files_terminate_build 0

%description
faylu-epshteyna скачивает видео на зашифрованный/съёмный диск.
Поддерживает: pornhub, vk, xvideos, xhamster, xnxx, eporner, redtube,
tiktok, bunkr, thothub и другие сайты. Fallback на yt-dlp.

%build
# pre-built binary

%install
# buildroot already populated externally

%files
$(echo "$files_list")

%changelog
* ${changelog_date} xomel45 <xom.xom.zip@gmail.com> - ${rpm_version}-1
- Release ${VERSION}
SPEC_EOF

    rpmbuild -bb \
        --nodeps \
        --define "_topdir $(realpath "$rpm_topdir")" \
        --buildroot "$(realpath "$buildroot")" \
        "${rpm_topdir}/SPECS/${APP_NAME}.spec"

    local created_rpm
    created_rpm=$(find "${rpm_topdir}/RPMS" -name "*.rpm" | head -1 || true)

    if [[ -z "$created_rpm" || ! -f "$created_rpm" ]]; then
        fail ".rpm не создан! Проверь вывод rpmbuild выше."
    fi

    cp "$created_rpm" "$dest/$rpm_name"
    rm -rf "$rpm_topdir"

    rule
    ok "Release .rpm готов!"
    echo "  Директория:  ${BOLD}$dest/${NC}"
    echo "  Пакет:       $rpm_name"
    echo "  Размер:      $(file_size "$dest/$rpm_name")"
    echo "  Версия:      $VERSION  |  commit: $(git_hash)"
    echo ""
    echo "  Установка:"
    echo "    sudo rpm -i $rpm_name                 # RPM-based"
    echo "    sudo dnf install ./$rpm_name          # Fedora / RHEL"
    echo ""
}

# ── Вывод помощи ─────────────────────────────────────────────────────────────
show_help() {
    echo ""
    echo -e "${BOLD}${CYAN}deploy.sh${NC} — упаковщик артефактов faylu-epshteyna v${BOLD}${VERSION}${NC}"
    rule
    echo ""
    echo -e "  ${BOLD}Использование:${NC}"
    echo "    ./deploy.sh beta                  Сырой ELF → builds/beta/"
    echo "    ./deploy.sh release               Linux + Windows → builds/releases/VERSION-*/"
    echo "    ./deploy.sh release linux         .tar.gz → builds/releases/${VERSION}-linux/"
    echo "    ./deploy.sh release pkg/arch      .pkg.tar.zst (Arch/pacman)"
    echo "    ./deploy.sh release deb/debian    .deb"
    echo "    ./deploy.sh release rpm/rh        .rpm"
    echo "    ./deploy.sh release my            Авто: пакет для текущего дистрибутива"
    echo "    ./deploy.sh release linux-all     Все Linux форматы"
    echo "    ./deploy.sh release win           .exe → builds/releases/${VERSION}-windows/"
    echo "    ./deploy.sh release all           Всё: tar.gz+pkg+deb+rpm + Windows.zip"
    echo ""
    echo -e "  ${BOLD}Опции:${NC}"
    echo "    --build     Пересобрать перед деплоем (через CMake)"
    echo "    --clean     Удалить целевые директории перед сборкой"
    echo ""
    echo -e "  ${BOLD}Примеры:${NC}"
    echo "    ./deploy.sh beta --build"
    echo "    ./deploy.sh release linux --clean"
    echo "    ./deploy.sh release linux-all --build"
    echo "    ./deploy.sh release all --build --clean"
    echo ""
    echo -e "  ${BOLD}Требования для Linux-пакетов:${NC}"
    echo "    .pkg.tar.zst   — zstd      (sudo pacman -S zstd)"
    echo "    .deb           — dpkg-deb  (sudo apt install dpkg-dev)"
    echo "    .rpm           — rpmbuild  (sudo dnf install rpm-build)"
    echo ""
    echo -e "  ${BOLD}Структура вывода:${NC}"
    echo "    builds/"
    echo "    ├── beta/"
    echo "    │   ├── faylu-epshteyna"
    echo "    │   └── build-info.txt"
    echo "    └── releases/"
    echo "        ├── ${VERSION}-linux/"
    echo "        │   ├── faylu-epshteyna-${VERSION}-linux-x86_64.tar.gz"
    echo "        │   ├── faylu-epshteyna-${VERSION}-1-x86_64.pkg.tar.zst"
    echo "        │   ├── faylu-epshteyna_${VERSION}_amd64.deb"
    echo "        │   ├── faylu-epshteyna-${VERSION}-1.x86_64.rpm"
    echo "        │   └── build-info.txt"
    echo "        └── ${VERSION}-windows/"
    echo "            ├── faylu-epshteyna.exe  (статический, DLL не нужны)"
    echo "            ├── README.txt"
    echo "            └── build-info.txt"
    echo ""
}

# ── Точка входа ───────────────────────────────────────────────────────────────
case "$MODE" in
    beta)
        deploy_beta
        ;;
    release)
        case "$PLATFORM" in
            linux)
                deploy_release_linux
                ;;
            win|windows)
                deploy_release_windows
                ;;
            pkg|arch|Arch)
                deploy_release_pkg
                ;;
            deb|debian|Debian)
                deploy_release_deb
                ;;
            rpm|rh|RH|red-hat|Red-Hat|red_hat|Red_Hat)
                deploy_release_rpm
                ;;
            my)
                fmt=$(detect_pkg_format)
                case "$fmt" in
                    pkg) log "Определён дистрибутив: Arch-семейство → .pkg.tar.zst"; deploy_release_pkg ;;
                    deb) log "Определён дистрибутив: Debian-семейство → .deb";        deploy_release_deb ;;
                    rpm) log "Определён дистрибутив: RPM-семейство → .rpm";           deploy_release_rpm ;;
                    *)   fail "Не удалось определить семейство дистрибутива. Укажи формат вручную: pkg / deb / rpm" ;;
                esac
                ;;
            linux-all)
                if $DO_CLEAN; then
                    ensure_builds_tree
                    safe_clean "${BUILDS_DIR}/releases/${VERSION}-linux"
                    DO_CLEAN=false
                fi
                deploy_release_linux
                deploy_release_pkg
                deploy_release_deb
                deploy_release_rpm
                ;;
            all)
                if $DO_CLEAN; then
                    ensure_builds_tree
                    safe_clean "${BUILDS_DIR}/releases/${VERSION}-linux"
                    safe_clean "${BUILDS_DIR}/releases/${VERSION}-windows"
                    rm -f "${BUILDS_DIR}/releases/${VERSION}-windows.zip"
                    DO_CLEAN=false
                fi
                deploy_release_linux
                deploy_release_pkg
                deploy_release_deb
                deploy_release_rpm
                deploy_release_windows
                ;;
            both|--build|--clean|*)
                deploy_release_linux
                deploy_release_windows
                ;;
        esac
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        show_help
        fail "Неизвестная команда: '$MODE'"
        ;;
esac
