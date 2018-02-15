#!/bin/bash
#
# Swift Kernel Build Script 
# Coded by BlackMesa/TaifAljaloo/AnanJaser @2018
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
echo " ....building Swift kernel started ...."

# For testing only
# SW_TC=/home/elite/android/toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-

# Init Fields
SW_DIR=$(pwd)
SW_DATE=$(date +%Y%m%d)
SW_TC=/home/taif/Desktop/kernel/toolchain/bin/aarch64-linux-android-
SW_JOBS=5
SW_VERSION=v1.0

# Device specific Fields
SW_CONFG=exynos7885-jackpotlte_eur_defconfig
SW_ARCH=arm64
SW_VARIANT=A530F
SW_DTB="exynos7885-jackpotlte_eur_open_00 exynos7885-jackpotlte_eur_open_01 exynos7885-jackpotlte_eur_open_02 exynos7885-jackpotlte_eur_open_03 exynos7885-jackpotlte_eur_open_04 exynos7885-jackpotlte_eur_open_05 exynos7885-jackpotlte_eur_open_06 exynos7885-jackpotlte_eur_open_07"
SW_ANDROID=n


# cleaning source
export CROSS_COMPILE=$SW_TC
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
echo "******************source cleaning started***********************"
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
make clean && make mrproper
	rm -r -f $SW_DIR/swift/dtb.img
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
echo "******************source cleaning is done***********************"
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"

# building zimage 

echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
echo "******************buidling zimage started***********************"
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
export ANDROID_MAJOR_VERSION=$SW_ANDROID
export $SW_ARCH
export LOCALVERSION=-Swift_Kernel-$SW_VERSION-$SW_VARIANT-$SW_DATE
make  $SW_CONFG
make -j$SW_JOBS
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
echo "******************buidling zimage is done***********************"
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"

#  building DTB 

echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
echo "******************buidling DTB started***********************"
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"

for dts in $SW_DTB; do
	$SW_TC-cpp -nostdinc -undef -x assembler-with-cpp -I        $SW_DIR/include $SW_DIR/arch/arm64/boot/dts/exynos/${dts}.dts > ${dts}.dts
	$SW_DIR/scripts/dtc/dtc -p 0 -i $SW_DIR/arch/arm64/boot/dts/exynos -O dtb -o ${dts}.dtb ${dts}.dts
done
$SW_DIR/tools/dtbtool/dtbtool -o $SW_DIR/swift/dtb.img
rm -f $SW_DIR/*.dtb
rm -f $SW_DIR/*.dts

echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
echo "******************buidling DTB is done***********************"
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"

# packing boot.img
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
echo "******************Packing boot.img***********************"
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
cp -rf $SW_DIR/sw-tools/ramdisks/ramdisk/. $SW_DIR/sw-tools/AIK-Linux/ramdisk
cp -rf $SW_DIR/sw-tools/ramdisks/split_img/. $SW_DIR/sw-tools/AIK-Linux/split_img
cp $SW_DIR/arch/arm64/boot/Image $SW_DIR/sw-tools/AIK-Linux/split_img/boot.img-zImage
cp $SW_DIR/swift/dtb.img $SW_DIR/sw-tools/AIK-Linux/split_img/boot.img-dtb
$SW_DIR/sw-tools/AIK-Linux/repackimg.sh --nosudo
cp $SW_DIR/sw-tools/AIK-Linux/image-new.img $SW_DIR/swift/boot_A530F.img
$SW_DIR/sw-tools/AIK-Linux/cleanup.sh --nosudo
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
echo "******************packing boot.img is done**********************"
echo "****************************************************************"
echo "****************************************************************"
echo "****************************************************************"
