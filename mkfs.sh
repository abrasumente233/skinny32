set -eu

mkdir -p build

hdd=fs.img

echo '[fs: 1/3] creating disk file'
dd if=/dev/zero of=$hdd bs=1M count=48 > /dev/null 2>&1

echo '[fs: 2/3] partitioning & formatting disk'
echo "n
p


t
b
a
w
"|fdisk $hdd > /dev/null 2>&1 ;mkfs.vfat -F 32 $hdd > /dev/null 2>&1

echo '[fs: 3/3] copying files to the disk'
mkdir -p fs
sudo mount $hdd fs/
sudo cp README fs/README.TXT
sudo umount fs/
