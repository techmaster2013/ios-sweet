#!/bin/bash
# HFS+ for WSL2: Microsoft's WSL kernel is built without CONFIG_HFSPLUS_FS, so
# mkiosroot.sh cannot loop-mount its image. Build hfsplus.ko for the running
# kernel and load it. Needs root for the apt install and insmod (wsl -u root).
#
#   wsl-hfsplus.sh build   [DIR]   clone the matching WSL kernel tag, build hfsplus.ko
#   wsl-hfsplus.sh load    [DIR]   insmod it (again after every WSL restart)
#
# The running kernel was built with GCC 13.2 and GCC plugins; with GCC 15 the
# generated config differs (counted_by, plugins) and the symbol CRCs with it:
# "hfsplus: disagrees about version of symbol module_layout". gcc-13 +
# gcc-13-plugin-dev reproduce the config, so the module loads without forcing.
# The tag's tree is a merge commit, so the release string gets a "+": pin it.
set -euo pipefail
cmd=${1:-}
dir=${2:-$HOME/pi5-build}
src=$dir/wsl-kernel
ko=$src/fs/hfsplus/hfsplus.ko
rel=$(uname -r)

case $cmd in
build)
	command -v gcc-13 >/dev/null || { echo "need: apt install gcc-13 gcc-13-plugin-dev bc bison flex libssl-dev libelf-dev dwarves" >&2; exit 1; }
	mkdir -p "$dir"
	if [[ ! -d $src ]]; then
		git clone -q --depth 1 --branch "linux-msft-wsl-${rel%%-*}" \
			https://github.com/microsoft/WSL2-Linux-Kernel.git "$src"
	fi
	cd "$src"
	K=(CC=gcc-13 HOSTCC=gcc-13 HOSTCXX=g++-13 LOCALVERSION=)
	zcat /proc/config.gz > .config
	./scripts/config --module CONFIG_HFSPLUS_FS
	make -s "${K[@]}" olddefconfig
	make -s -j"$(nproc)" "${K[@]}" vmlinux
	echo "$rel" > include/config/kernel.release
	sed -i "s/^#define UTS_RELEASE .*/#define UTS_RELEASE \"$rel\"/" include/generated/utsrelease.h
	cp vmlinux.symvers Module.symvers
	make -s -j"$(nproc)" "${K[@]}" M=fs/hfsplus modules
	modinfo "$ko" | grep '^vermagic'
	;;
load)
	grep -qw hfsplus /proc/filesystems || insmod "$ko"
	grep -w hfsplus /proc/filesystems
	;;
*)
	sed -n '2,9p' "$0"
	exit 2
	;;
esac
