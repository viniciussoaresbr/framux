#!/bin/sh
# Gera os .deb em contêineres podman (sem sudo), um conjunto por distro:
#   packaging/build-deb.sh                 # debian13, ubuntu24.04 e ubuntu26.04
#   packaging/build-deb.sh ubuntu24.04     # só uma
# Saída em deb/: framux_<v>_<distro>_amd64.deb, framux-layer_<v>_<distro>_amd64.deb
# e framux-layer_<v>_<distro>_i386.deb (ex.: framux_0.4.6-1_ubuntu24.04_amd64.deb;
# dentro do pacote, a versão é 0.4.6-1~ubuntu24.04).
set -eu
cd "$(dirname "$0")/.."

mkdir -p deb
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Arquivos do repositório, inclusive os ainda não commitados (sem os ignorados).
git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf "$work/framux.tar"

for target in ${*:-debian13 ubuntu24.04 ubuntu26.04}; do
  case $target in
    debian13) image=docker.io/library/debian:trixie ;;
    ubuntu24.04) image=docker.io/library/ubuntu:24.04 ;;
    ubuntu26.04) image=docker.io/library/ubuntu:26.04 ;;
    *) echo "alvo desconhecido: $target (debian13, ubuntu24.04, ubuntu26.04)" >&2; exit 1 ;;
  esac
  echo "==> $target"
  podman run --rm --security-opt label=disable -v "$work:/in:ro" -v "$PWD/deb:/out" "$image" \
    sh -c 'mkdir /w && tar -xf /in/framux.tar -C /w && sh /w/packaging/deb-build-inside.sh'
done
ls -1 deb/*.deb
