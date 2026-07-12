# PicoCalc x86

![image](splash.svg)

8086/80186 emulator for **ClockWorkPi PicoCalc** (https://www.clockworkpi.com/picocalc)
with Raspberry Pi Pico 2 board.

The project based on great 8086tiny project (https://github.com/adriancable/8086tiny) by Adrian Cable.

## Using
* https://github.com/elehobica/pico_fatfs for storage and disk access
    * https://elm-chan.org/fsw/ff/ upstream FatFs project
* https://github.com/polpo/rp2040-psram for PSRAM access
    * https://github.com/polpo/rp2040-psram/pull/15 using my PR for QPI mode

The project is targeting **Raspberry Pi Pico 2 RISC-V Hazard3** cores, no plans for ARM cores.
If someone wants to run on ARM cores it would be very easy to adapt it.

## Specs

### Video
CGA video adpater

   * text modes 80x25 and 40x25 (Passing all text from `CGA_COMP` except 8x8 font display text WIP)
   * grahic modes 4 and 6 (issues with text output during grahics WIP)

### Sound
   * PC Speaker

### Serial
   * Pico UART0 connected as COM1, BIOS interrupts(up to 9600) and direct mode working up to 57600 (tested with https://github.com/go4retro/tcpser for accessing telnet BBS via modem emulation on host PC).
   * XON/XOFF software flow control method must be used

### RAM
In stock config:
* available conventional **RAM is 417792(0x66000) bytes, or 408 KB**
* 16 KB video ram for CGA
* 16 KB for BIOS

### Performance
It's around ~1 MIPS, so like IMB PC AT or overpowered XT or PS/2 model 30.

## Getting Started

Firmware images are available only for Raspberry Pi Pico 2.

* Find prebuilt disk images with MS DOS 4.0 or SvarDOS and PicoCalc x86 firmware images in Releases (https://github.com/shtirlic/picocalc_x86/releases)

* Load uf2 firmware image via `picotool` or USB mass storage method.
* Format SD card with FAT32 file system, create `x86` dir in the root and put desired `hd.img` into it, so the resulting path on the SD Card is `/x86/hd.img`

### Disk Images

#### How to make your own `hd.img`

```
dd if=/dev/zero of=hd.img bs=1M count=256
```

mount new disk in `dosbox`, usage of `mount` or `imgmount` command depends on your dosbox version
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

### Serial / Modem / File Transfers


#### File Transfers

You can transfer files between PicoCalc x86 and host PC running terminal(zmodem)
or using `DDLINK` utility https://dunfield.themindfactory.com/dnld/DDLINK.ZIP by Dave Dunfield
running on dosbox and PicoCalc x86, it provides Norton Commander style interface
to transfer files between PCs.


dosbox as client
```
ddlink c=1,57600
```

PicoCalc x86 as server
```
ddlink /s c=1,57600
```
Transfer speed is around ~6KB/sec

#### Modem emulation

Use https://github.com/go4retro/tcpser for example on linux host

Example
```
./tcpser -d /dev/ttyUSB0 -s 57600 -l 7  -i "s0=1&k4e1" -n123=amnesiabbs.duckdns.org
```

This starts host modem emulation on `ttyUSB0` (PicoCalc Pico uart port connected via USB-C)

Explanation of modem init command `"s0=1&k4e1"`:
>It will pick up the phone after one ring, enable XON/XOFF software flow control and enable
echo modem commands. Also adds speed dial for `123` number, so in terminal you could dial via `ATDT123` and then connect via emultion/proxy to the telnet `amnesiabbs.duckdns.org`
The current BBS list can be found here https://www.telnetbbsguide.com

For terminal I suggest using shareware Qmodem Lite 4.5 https://winworldpc.com/product/qmodem/45 or better alternative.


## Building

### Must
 * ClockworkPi PicoCalc with Raspberry Pi Pico 2 compatible board installed
 * Git
 * nasm (https://github.com/netwide-assembler/nasm/)
 * Cmake
 * pico-sdk (https://github.com/raspberrypi/pico-sdk) (/usr/share/pico-sdk)
 * riscv-none-elf gcc (https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack)

### Optional
 * dosbox-x (https://dosbox-x.com) or dosbox-staging (https://www.dosbox-staging.org)
 * Krita for svg splash
 * Pixelorama for font editing (https://pixelorama.org/)
 * LCD Image converter for splash/fonts/images to C headers files (https://lcd-image-converter.riuson.com)


```
git clone --recurse-submodules https://github.com/shtirlic/picocalc_x86
```

## Todo

 * [ ] Fix CGA text output in graphics mode for mode 4 and mode 6
 * [ ] Add good 8x8 font for 40 column text mode
 * [ ] Make screenshot function (hotkey) via saving on SD Card
 * [ ] Support PSRAM connected via QMI like Pimoroni Pico 2 (map memory to 640kb)
 * [ ] EMS 3.2 full testing and XMS on top of it
 * [x] UART passthrough to host PC as COM1 (modem etc)
 * [ ] Pico 2 W some network emulation
 * [ ] Pico 2 LED support
 * [ ] Support ctrl+f1 or alt+f1 and other keystrokes
 * [ ] MCGA? mode 13h 320x200 256-color mode via PSRAM
 * [ ] More hardware devices emulation
 * [ ] Add floppy support via fd.img
 * [ ] BIOS boot menu
 * [ ] Better emulation for disk subsystems
 * [ ] Enable bios override from SD card
 * [ ] Support backlights and power resets via https://git.jcsmith.fr/jackcartersmith/picocalc_BIOS
 * [ ] Pass all test for CGA comp https://github.com/MobyGamer/CGACompatibilityTester
