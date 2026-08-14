# MoveToPlay PCB Hardware

This directory contains the editable JLCEDA project sources for the MoveToPlay hardware.

## Design variants

| Hardware | Variant or revision | Source directory | Original export name |
| --- | --- | --- | --- |
| Blade Controller | Standard | `blade-controller/standard/source/` | `Blade_V1.0_Nromal.eprj2` |
| Blade Controller | Heart Rate | `blade-controller/heart-rate/source/` | `Blade_V2.0_HR(Heart Rate).eprj2` |
| Motion Tracker | Battery Top | `motion-tracker/battery-top/source/` | `Tracker_V1.0_SMT_Top-Battery.eprj2` |
| Motion Tracker | Battery Bottom | `motion-tracker/battery-bottom/source/` | `Tracker_V2.0_SMT_Botton-Battery.eprj2` |
| USB Dongle | Rev A | `usb-dongle/rev-a/source/` | `Dongle_SW1.eprj2` |
| USB Dongle | Rev B | `usb-dongle/rev-b/source/` | `Dongle_SW2.eprj2` |
| USB Dongle | Rev C | `usb-dongle/rev-c/source/` | `Dongle_SW3.eprj2` |
| Charging Dock | Rev A | `charging-dock/source/` | `充电底座.eprj2` |
| Heart-Rate Sensor | Rev A | `heart-rate-sensor/source/` | `Heart_Rate_V2.0.eprj2` |

The Dongle revision letters follow the chronological order of the original `SW1`, `SW2`, and `SW3` exports. Rev C is the newest exported design in this repository.

## Opening the projects

The `.eprj2` files are complete JLCEDA project databases. Open them with a compatible JLCEDA desktop editor. Public copies have local account metadata removed; schematic, PCB, library, and project-history data are unchanged.

## Manufacturing outputs

The current publication contains editable source projects. Before marking a hardware release as production-ready, add the corresponding outputs next to each variant:

```text
<variant>/
├─ source/          # Editable JLCEDA project
├─ fabrication/     # Gerber, drill, BOM, and pick-and-place files
├─ schematic/       # Schematic PDF
└─ images/          # Front/back PCB previews
```

Do not assume that an editable source snapshot is ready for fabrication. Use a tagged release whose README explicitly records the tested PCB revision and manufacturing parameters.
