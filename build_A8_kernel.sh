#!/bin/bash
#
echo " ....building Swift kernel started ...."
# Init Fields
SW_DIR=/home/taif/Desktop/kernel/kernel
SW_DTB="exynos7885-jackpotlte_eur_open_00 exynos7885-jackpotlte_eur_open_01 exynos7885-jackpotlte_eur_open_02 exynos7885-jackpotlte_eur_open_03 exynos7885-jackpotlte_eur_open_04 exynos7885-jackpotlte_eur_open_05 exynos7885-jackpotlte_eur_open_06 exynos7885-jackpotlte_eur_open_07"

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
	
