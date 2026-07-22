# Running Mac OS 9 on real Power Mac G3/G4 hardware

Background notes for the MacSurf wiki on sourcing, installing, and provisioning a
real Power Mac for OS 9 development. MacSurf's dev machine is a G3 iMac and its
community target is a Power Mac G4 on OS 9.2.2, so this covers representative
models, OS install, modern storage, networking, and file transfer. Facts below
were checked against the cited pages; folklore and unverified points are flagged.

## Representative models that boot OS 9 natively

- All Power Mac G3 models (Beige/Platinum and Blue & White) can boot Mac OS 9. Beige G3s shipped with Mac OS 8.0/8.1; Blue & White G3s shipped with 8.5.1/8.6. — https://everymac.com/systems/apple/powermac_g3/faq/power-mac-g3-boot-mac-os-9-x-support.html
- All Power Mac G4 models can boot Mac OS 9 *except* the "FireWire 800" models (the 1.0, 1.25 DP, and 1.42 DP FW800 variants), which boot only Mac OS X 10.2.3+ and run OS 9 apps in Classic only. — https://everymac.com/systems/apple/powermac_g4/faq/power-mac-g4-boot-macos-9-run-classic-applications.html
- The last Power Mac that can boot OS 9 natively is the Mirrored Drive Door (MDD) Power Mac G4 1.25 (2003), which shipped with 9.2.2 and X 10.2. — https://everymac.com/systems/apple/powermac_g4/faq/power-mac-g4-boot-macos-9-run-classic-applications.html
- G5 machines cannot boot OS 9 at all (no G5 support in the OS). — https://en.wikipedia.org/wiki/Mac_OS_9

## Installing OS 9.1 – 9.2.2

- Mac OS 9.2.2 (released December 2001) is the final release of OS 9 and of the classic Mac OS; it supports only PowerPC G3 and G4 processors. — https://en.wikipedia.org/wiki/Mac_OS_9
- 9.2.x updaters are *updaters, not full installers*: 9.2.1/9.2.2 require an existing 9.1+ install on the target drive. Practically, you install a full OS 9 (e.g. from a retail/restore CD or a 9.1 install), then apply the free 9.2.2 updater. — https://www.macintoshrepository.org/2605-mac-os-9-0-4-9-1-9-2-1-9-2-2-international-english-updaters
- Apple shipped free 9.0.4 / 9.1 / 9.2.1 / 9.2.2 updaters; any 9.0+ install can be brought to 9.2.2 with them. — https://lowendmac.com/2013/low-end-macs-compleat-guide-to-mac-os-9/

## Modern storage options

- The internal bus on these Macs is IDE/PATA (ATA-33 on Blue & White G3 and "Yikes" G4; ATA-66+ on later G4s). A CF-to-IDE or 2.5" IDE SSD adapter is a passive drop-in on this bus. — https://lowendmac.com/2010/sata-and-ssd-options-for-g3-and-g4-power-macs/
- Power Macs earlier than the Quicksilver G4 (mid-2001) have a 137 GB / 128 GiB ceiling on the internal IDE bus; larger drives only address ~128 GB without a third-party PCI IDE/SATA card. — https://lowendmac.com/2014/maximum-hard-drive-size/
- For SATA SSDs, the Sonnet Tempo Serial ATA PCI card is notable as one of the few SATA cards with OS 9 driver support (also bypasses the 128 GB onboard limit). — https://lowendmac.com/2010/sata-and-ssd-options-for-g3-and-g4-power-macs/
- SCSI2SD applies only to *SCSI* Macs (e.g. some Beige G3 SCSI setups or older systems); the mainstream G3/G4 internal disk is IDE, so CF/IDE or PATA-SSD adapters are the usual route, not SCSI2SD. — https://www.savagetaylor.com/2018/01/05/setting-up-your-vintage-classic-68k-macintosh-using-a-scsi2sd-adapter/

## Networking

- G3/G4 Power Macs have built-in Ethernet; OS 9's TCP/IP control panel can pull an address via DHCP, so they join a normal LAN/router like any other client. — https://www.macworld.com/article/227653/how-to-connect-an-old-power-macintosh-g3-and-other-vintage-macs-to-the-internet.html
- For wireless, an Ethernet-to-Wi-Fi bridge/adapter (configured via its own web UI) presents as plain Ethernet to the Mac. — https://www.macworld.com/article/227653/how-to-connect-an-old-power-macintosh-g3-and-other-vintage-macs-to-the-internet.html
- Modern HTTPS/TLS and heavy sites generally fail in OS 9-era browsers; the Macworld author notes anything more complex than google.com is "almost guaranteed to fail." (This is exactly the gap MacSurf + its TLS proxy / macTLS exist to close.) — https://www.macworld.com/article/227653/how-to-connect-an-old-power-macintosh-g3-and-other-vintage-macs-to-the-internet.html

## Getting files onto the machine

- USB works but the ports are USB 1.1 (slow); a thumb drive is "perfectly functional" if formatted so both ends can read it (HFS+/Mac OS Extended is the safe common format per the forum). Bus-powered external USB hard drives are unreliable due to insufficient USB 1.1 power. — https://tinkerdifferent.com/threads/easy-way-to-transfer-files-to-and-from-imac-g3.2028/
- FTP over Ethernet is a common path: run an FTP server on the modern machine and use an OS 9 client (Fetch, Transmit, or even IE 5.x). — https://tinkerdifferent.com/threads/easy-way-to-transfer-files-to-and-from-imac-g3.2028/
- AFP/AppleShare to a NAS or a netatalk-based server (e.g. A2SERVER) gives fast, drive-like transfers over Ethernet via OS 9's Network Browser; WebDAV (Goliath client) is an alternative. — https://mac-classic.com/articles/remote-file-storage-transfers-and-backups/ , https://tinkerdifferent.com/threads/easy-way-to-transfer-files-to-and-from-imac-g3.2028/

## Beginner gotchas / things that surprise people

- "9.2.2" is not a clean-install disc. You must lay down a full OS 9 first, then run the updater — a frequent first-timer trap. (Macintosh Repository updaters page)
- The FireWire 800 G4s look like normal G4s but cannot boot OS 9 — verify the exact model, not just "it's a G4." (everymac G4 page)
- The ~128 GB IDE ceiling on pre-Quicksilver machines silently truncates big drives; CF cards and modest SSDs sidestep this by being small enough. (Low End Mac max-drive-size)
- Mac metadata (type/creator, resource forks) is lost over plain FAT USB drives or naive transfers; HFS+ media and AFP/Mac-aware servers preserve it, which matters for launching Classic apps. (mac-classic AFP article; HFS+ recommendation in the TinkerDifferent thread)
- Software preserving the resource fork (e.g. BinHex .hqx, StuffIt .sit) is the conventional way to move Mac files through non-Mac channels — this matches MacSurf's own .hqx/.sit handoff workflow. (Not independently re-verified here; consistent with the AFP/forks discussion above.)

## Uncertain / not verified

- Exact OS 9 USB-mass-storage filesystem support (FAT16/FAT32 vs. HFS specifics) could not be confirmed from an authoritative OS 9-era source; the modern-Mac formatting pages that surfaced in search do not speak to OS 9. The reliable, source-backed advice is "USB 1.1, slow but works; format HFS+ for round-tripping with another Mac."
- SCSI2SD is included for completeness but is off the mainstream path for IDE-based G3/G4s; treat it as relevant only to genuinely SCSI machines.

## Sources

- https://everymac.com/systems/apple/powermac_g3/faq/power-mac-g3-boot-mac-os-9-x-support.html
- https://everymac.com/systems/apple/powermac_g4/faq/power-mac-g4-boot-macos-9-run-classic-applications.html
- https://en.wikipedia.org/wiki/Mac_OS_9
- https://www.macintoshrepository.org/2605-mac-os-9-0-4-9-1-9-2-1-9-2-2-international-english-updaters
- https://lowendmac.com/2013/low-end-macs-compleat-guide-to-mac-os-9/
- https://lowendmac.com/2010/sata-and-ssd-options-for-g3-and-g4-power-macs/
- https://lowendmac.com/2014/maximum-hard-drive-size/
- https://www.savagetaylor.com/2018/01/05/setting-up-your-vintage-classic-68k-macintosh-using-a-scsi2sd-adapter/
- https://www.macworld.com/article/227653/how-to-connect-an-old-power-macintosh-g3-and-other-vintage-macs-to-the-internet.html
- https://tinkerdifferent.com/threads/easy-way-to-transfer-files-to-and-from-imac-g3.2028/
- https://mac-classic.com/articles/remote-file-storage-transfers-and-backups/
