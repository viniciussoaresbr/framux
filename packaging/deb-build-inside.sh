#!/bin/sh
# Roda dentro do contêiner (ver build-deb.sh), com o código em /w e a saída
# em /out: framux e framux-layer amd64 pelo dpkg-buildpackage, e
# framux-layer i386 compilado com gcc -m32 sobre o pacote amd64.
set -eu
export DEBIAN_FRONTEND=noninteractive
. /etc/os-release
case $ID in
  debian) suffix=deb$VERSION_ID ;;
  ubuntu) suffix=ubuntu$VERSION_ID ;;
  *) echo "distro não suportada: $ID" >&2; exit 1 ;;
esac

apt-get update -qq
apt-get install -y -qq --no-install-recommends dpkg-dev >/dev/null

cd /w
cp -r packaging/debian debian
version=$(dpkg-parsechangelog -S Version)~$suffix
sed -i "1s/(.*) [a-z]*;/($version) $VERSION_CODENAME;/" debian/changelog
apt-get build-dep -y -qq --no-install-recommends . >/dev/null

dpkg-buildpackage -b -us -uc

# --- framux-layer i386 ---
# Mesmo conteúdo do amd64 (inclusive /usr/share/doc, que o Multi-Arch: same
# exige idêntico), com a camada, o shim e o manifesto de 32 bits.
eval "$(dpkg-buildflags --export=sh)"
meson setup /b32 --cross-file cross/i686-linux.ini --prefix=/usr --libdir=lib/i386-linux-gnu \
  --buildtype=plain -Dlayer_only=true -Dtests=false
meson compile -C /b32
DESTDIR=/s32 meson install -C /b32 --no-rebuild

amd64=$(ls ../framux-layer_*_amd64.deb)
dpkg-deb -R "$amd64" /p32
rm -rf /p32/usr/lib/x86_64-linux-gnu /p32/usr/share/vulkan/implicit_layer.d/framux_x86_64.json
mkdir -p /p32/usr/lib/i386-linux-gnu
cp -a /s32/usr/lib/i386-linux-gnu/framux /p32/usr/lib/i386-linux-gnu/
cp /s32/usr/share/vulkan/implicit_layer.d/framux_i686.json /p32/usr/share/vulkan/implicit_layer.d/
strip --strip-unneeded --remove-section=.comment /p32/usr/lib/i386-linux-gnu/framux/*.so

# libstdc++ e libgcc são estáticas: só a glibc entra nas dependências.
glibc=$(objdump -T /p32/usr/lib/i386-linux-gnu/framux/*.so | grep -o 'GLIBC_[0-9.]*' | cut -d_ -f2 | sort -uV | tail -1)
sed -i -e 's/^Architecture: .*/Architecture: i386/' \
  -e "s/^Depends: .*/Depends: libc6 (>= $glibc)/" \
  -e "s/^Installed-Size: .*/Installed-Size: $(du -sk --exclude=DEBIAN /p32 | cut -f1)/" /p32/DEBIAN/control
(cd /p32 && find usr -type f -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums)
dpkg-deb --root-owner-group -Zxz --build /p32 "../framux-layer_${version}_i386.deb"

# Nome do arquivo sem o "~" (o GitHub o troca nos anexos das releases):
# framux_0.4.6-1~deb13_amd64.deb → framux_0.4.6-1_debian13_amd64.deb.
for f in ../framux_*.deb ../framux-layer_*.deb; do
  cp "$f" "/out/$(basename "$f" | sed "s/~${suffix}_/_${ID}${VERSION_ID}_/")"
done
