# PicoCalc x86

8086/80186 emulator for ClockWorkPi PicoCalc (https://www.clockworkpi.com/picocalc)
with Raspberry Pi Pico 2 mcu board.

The project based on great 8086tiny (https://github.com/adriancable/8086tiny) by Adrian Cable.

## Using
* https://github.com/elehobica/pico_fatfs for storage and disk access
    * https://elm-chan.org/fsw/ff/ upstream FatFs project
* https://github.com/polpo/rp2040-psram for PSRAM access
    * https://github.com/polpo/rp2040-psram/pull/15 using my PR for QPI mode

The project is targeting Raspberry Pi Pico 2 RISC-V Hazard3 cores, no plans for ARM cores.
If someone wants to run on ARM cores it would be very easy to adapt it.

## Specs

### RAM
In stock config:
* available conventional **RAM is 417792(0x66000) bytes, or 408 KB**
* 16 KB video ram for CGA
* 16 KB for BIOS

### Performance
It's around ~1 MIPS, so like IMB PC AT or overpowered XT or PS/2 model 30.

## Getting Started

Firmware images are available only for Raspberry Pi Pico 2 for now.

* Load uf2 firmware image via `picotool` or USB mass storage method.
* Format SD card with FAT32 file system, create `x86` dir in the root and put desired `hd.img` into it, so the resulting path on the SD Card is `/x86/hd.img`

### How to make your own `hd.img`

```
dd if=/dev/zero of=hd.img bs=1M count=256
```

mount in `dosbox` like this

```
mount c hd.img -t hdd -fs none -size 512,63,8,1024
```

* Use `fdisk` to create partion table and make it bootable.
* Install the operating system.

Then you can mount your `hd.img` and transfer files in `dosbox` via:

Example: 63 sectors pert track, 8 heads, 1024 sectors, 512 bytes per sector
(63*8*1024*512) = 264241152 bytes so 256mb drive.

```
mount c hd.img -t hdd -fs fat -size 512,63,8,1024
```

* Transfer to SD card to `/x86/hd.img` location.

## Building

### Must
 * ClockworkPi PicoCalc with Raspberry Pi Pico 2 compatible board installed
 * Git
 * nasm (https://github.com/netwide-assembler/nasm/)
 * Cmake
 * pico-sdk (https://github.com/raspberrypi/pico-sdk) (/usr/share/pico-sdk)
 * riscv-none-elf gcc (https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack)

### Optional
 * dosbox-staging  (https://www.dosbox-staging.org/)
 * Krita for svg splash
 * Pixelorama for font editing (https://pixelorama.org/)
 * LCD Image converter for splash/fonts/images to C headers files (https://lcd-image-converter.riuson.com)


```
git clone --recurse-submodules https://github.com/shtirlic/picocalc_x86
```

## Todo

 * [ ] Fix CGA text output in graphics mode for mode 4 and mode 6
 * [ ] Add good 8x8 font for 40 column text mode
 * [ ] Support PSRAM connected via QMI like Pimoroni Pico 2 (map memory to 640kb)
 * [ ] EMS 3.2 full testing
 * [ ] Support ctrl+f1 or alt+f1 and other keystrokes
 * [ ] MCGA?
 * [ ] More hardware devices emulation
 * [ ] Add floppy support via fd.img
 * [ ] BIOS boot menu
 * [ ] Better emulation for disk subsystems
 * [ ] Enable bios override from SD card
 * [ ] Support backlights and power resets via https://git.jcsmith.fr/jackcartersmith/picocalc_BIOS
 * [ ] Pass all test for CGA comp https://github.com/MobyGamer/CGACompatibilityTester
