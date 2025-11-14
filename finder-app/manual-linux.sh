#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e # Exit on error
set -u # Treat unset variables as an error

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

# if the number of arguments is less than 1 (i.e., none were passed).
if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

# create output directory if it does not exist
mkdir -p ${OUTDIR}

export PATH=/bin:/usr/bin:/usr/local/bin:/opt/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH

sudo rm -rf ${OUTDIR}/* #clean output directory

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; # build kernel only if doesn't exit yet.
then 
    cd "linux-stable"
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here - 1.c
    echo "Building the Linux Kernel"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper #deep clean
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig #default config the board
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all #build a kernel image
    #make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} modules #build kernel modules
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs #build device tree blobs
fi

echo "Adding the Image in outdir/Image"
sudo cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm -rf ${OUTDIR}/rootfs
fi

mkdir -p ${OUTDIR}/rootfs
cd "${OUTDIR}/rootfs"
mkdir -p bin dev etc home lib lib64 proc sbin sys tmp usr var
mkdir -p usr/bin usr/lib usr/sbin
mkdir -p var/log

echo "---------------- Busybox ------------------------------------"
cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
    #git clone git://busybox.net/busybox.git # this one fails sometimes on the remote server
    git clone https://github.com/mirror/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}

    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} distclean
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
else
    cd busybox
fi

make CONFIG_PREFIX=${OUTDIR}/rootfs ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} install 
echo "done"

echo "---------------- resolve Library dependencies----------------"
cd "${OUTDIR}/rootfs"
${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library"
${CROSS_COMPILE}gcc -print-sysroot

cp -v $(${CROSS_COMPILE}gcc --print-sysroot)/lib/ld-linux-aarch64.so.1 ${OUTDIR}/rootfs/lib/ # program interpreter must be in lib!!!!
cp -v $(${CROSS_COMPILE}gcc --print-sysroot)/lib64/libm.so.6 ${OUTDIR}/rootfs/lib64/
cp -v $(${CROSS_COMPILE}gcc --print-sysroot)/lib64/libc.so.6 ${OUTDIR}/rootfs/lib64/
cp -v $(${CROSS_COMPILE}gcc --print-sysroot)/lib64/libresolv.so.2 ${OUTDIR}/rootfs/lib64

echo "done"

echo "---------------- make devices -------------------------------"
cd "${OUTDIR}/rootfs"
sudo mknod -m 666 dev/null c 1 3 # null device
sudo mknod -m 666 dev/console c 5 1 # console device
echo "done"

echo "---------------- Clean and build the writer utility----------"
cd "${FINDER_APP_DIR}"
make CROSS_COMPILE=${CROSS_COMPILE} clean
make CROSS_COMPILE=${CROSS_COMPILE} all
echo "done"

echo "---------------- copy finder app ----------------------------"
cp -v ${FINDER_APP_DIR}/writer ${OUTDIR}/rootfs/home/

cp -v ${FINDER_APP_DIR}/finder.sh ${OUTDIR}/rootfs/home/
chmod +x ${OUTDIR}/rootfs/home/finder.sh
cp -v ${FINDER_APP_DIR}/finder-test.sh ${OUTDIR}/rootfs/home/
chmod +x ${OUTDIR}/rootfs/home/finder-test.sh

mkdir -p ${OUTDIR}/rootfs/home/conf/
cp -v ${FINDER_APP_DIR}/conf/username.txt ${OUTDIR}/rootfs/home/conf/
cp -v ${FINDER_APP_DIR}/conf/assignment.txt ${OUTDIR}/rootfs/home/conf/

cp -v ${FINDER_APP_DIR}/autorun-qemu.sh ${OUTDIR}/rootfs/home/
echo "done"

echo "---------------- create initramfs.cpio.gz -------------------"
# Create a standalone initramfs and outdir/initramfs.cpio.gz file based on the contents of the staging directory tree.
cd "${OUTDIR}/rootfs"
#remove existing initramfs.cpio.gz if it exists
if [ -f ${OUTDIR}/initramfs.cpio.gz ]; then
    sudo rm ${OUTDIR}/initramfs.cpio.gz
fi
if [ -f ${OUTDIR}/initramfs.cpio ]; then
    sudo rm ${OUTDIR}/initramfs.cpio
fi

#chown -R root:root *
sudo find . | cpio -H newc -ov --owner root:root > ${OUTDIR}/initramfs.cpio
sudo gzip -f ${OUTDIR}/initramfs.cpio
echo "done"

echo "---------------- All Done -----------------------------------"

echo "---------------- Start QEMU Terminal ------------------------"
cd "${OUTDIR}"
sudo ${FINDER_APP_DIR}/start-qemu-app.sh

