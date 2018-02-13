#!/bin/bash
#
echo " ....building Swift kernel started ...."

export CROSS_COMPILE=/home/taif/Desktop/kernel/toolchain/bin/aarch64-linux-android-


# building zimage 

echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
echo "******************buidling zimage started***********************"
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
export ANDROID_MAJOR_VERSION=n
make ARCH=arm64 exynos7885-jackpotlte_eur_defconfig
make ARCH=arm64
