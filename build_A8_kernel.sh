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
# Init Fields
SW_DIR=/home/taif/Desktop/kernel/kernel
SW_DTB="exynos7885-jackpotlte_eur_open_00 exynos7885-jackpotlte_eur_open_01 exynos7885-jackpotlte_eur_open_02 exynos7885-jackpotlte_eur_open_03 exynos7885-jackpotlte_eur_open_04 exynos7885-jackpotlte_eur_open_05 exynos7885-jackpotlte_eur_open_06 exynos7885-jackpotlte_eur_open_07"

export CROSS_COMPILE=/home/taif/Desktop/kernel/toolchain/bin/aarch64-linux-android-
# cleaning source 
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
export ANDROID_MAJOR_VERSION=n
make ARCH=arm64 exynos7885-jackpotlte_eur_defconfig
make ARCH=arm64
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
	/home/taif/Desktop/kernel/toolchain/bin/aarch64-linux-android-cpp -nostdinc -undef -x assembler-with-cpp -I        $SW_DIR/include $SW_DIR/arch/arm64/boot/dts/exynos/${dts}.dts > ${dts}.dts
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

# remove some useless files
cd /home/taif/Desktop/kernel/kernel/sw-tools/AIK-Linux

sudo ./cleanup.sh

	
