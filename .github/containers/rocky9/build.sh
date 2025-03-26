#!/bin/bash

set -e
set -x

## Basic variables

CURDIR=$(dirname $(readlink -f $0))
TOPDIR=$(git rev-parse --show-toplevel 2>/dev/null)

## RPMDIR=${TOPDIR}/contrib/packages/rpm/el9

## VERSION=$(grep '^set(VERSION ' ${TOPDIR}/CMakeLists.txt | sed 's@^set(VERSION \(.*\))@\1@')

## Prepare the build directory

## rm -rf ${CURDIR}/rpmbuild
## mkdir -p ${CURDIR}/rpmbuild/{BUILD,BUILDROOT,SRPMS,SOURCES,SPECS,RPMS}
## chmod a+w ${CURDIR}/rpmbuild/{BUILD,BUILDROOT,SRPMS,RPMS}
## [ -x /usr/sbin/selinuxenabled ] && /usr/sbin/selinuxenabled && chcon -Rt container_file_t ${CURDIR}/rpmbuild

## Copy over the packaging files

## cp ${RPMDIR}/SOURCES/* ${CURDIR}/rpmbuild/SOURCES
## cp ${RPMDIR}/SPECS/tigervnc.spec ${CURDIR}/rpmbuild/SPECS
## sed -i "s/@VERSION@/${VERSION}/" ${CURDIR}/rpmbuild/SPECS/tigervnc.spec

## Copy over the source code

## (cd ${TOPDIR} && git archive --prefix tigervnc-${VERSION}/ HEAD) | bzip2 > ${CURDIR}/rpmbuild/SOURCES/tigervnc-${VERSION}.tar.bz2

## Start the build

docker run --volume ${TOPDIR}:/home --interactive --rm qemu/${DOCKER} \
	bash -e -x -c "
 	ls -l
	sudo dnf -y install wget python3-tomli libjpeg-turbo-devel
	wget https://dl.rockylinux.org/pub/rocky/9.5/devel/source/tree/Packages/q/qemu-kvm-9.0.0-10.el9_5.2.src.rpm
	dnf builddep -y qemu-kvm-9.0.0-10.el9_5.2.src.rpm
	mkdir build
	cd build
	../configure --target-list=x86_64-softmmu --enable-slirp --enable-debug --audio-drv-list=pa
	make -j2
	"

mkdir -p ${CURDIR}/result
cp -av ${TOPDIR}/build/qemu-system* ${CURDIR}/result
