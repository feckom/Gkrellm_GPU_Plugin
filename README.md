# gkrellm-gpu

Univerzálny GPU monitor pre GKrellM 2.5.1 na Windows. Nástupca
`gkrellm-nvidia` — funguje na NVIDIA, AMD aj Intel, vrátane strojov
s viacerými kartami naraz (hybridný notebook: iGPU + dGPU).

## Architektúra

Kľúčová vec: **DXGI je jediný zoznam adaptérov.** Všetko ostatné sa naň
mapuje. Tým odpadá otravné párovanie kariet medzi vendor knižnicami,
ktoré si každá čísluje po svojom.

```
        DXGI  ──►  meno, PCI vendor ID, veľkosť VRAM, LUID
                        │
                        ▼
   ┌────────────────────────────────────────────┐
   │  gpu-core: 1 Hz vzorkovacie vlákno + merge │
   └────────────────────────────────────────────┘
       ▲          ▲          ▲             ▲
   nvidia-smi    ADL     Level Zero       PDH
    (NVIDIA)    (AMD)     (Intel)     (hocikto)
```

Backendy sa zlučujú v poradí priority — **vendor vždy prebije PDH.**
Pre každé pole vyhráva prvý backend, ktorý má reálne číslo; `-1` znamená
„toto neviem" a ide sa ďalej.

| backend | zdroj | load | VRAM | teplota | watty | fan | takt |
|---|---|---|:-:|:-:|:-:|:-:|:-:|
| nvidia-smi | proces, `--loop=1` | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ |
| ADL | `atiadlxx.dll` | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ |
| Level Zero | `ze_loader.dll` | ✔ | ✔ | ✔ | ✔ | ✗ | ✗ |
| PDH | perf. countery | ✔ | ✔ | ✗ | ✗ | ✗ | ✗ |

**PDH je poistka.** Je to ten istý zdroj, z ktorého číta Správca úloh —
funguje na každej karte bez ohľadu na výrobcu a bez SDK. Teplotu ani
príkon ale nedá nikdy; tie sa bez vendor knižnice zobrazia ako `--`.

### Prečo je všetko načítané dynamicky

`dxgi.dll`, `pdh.dll`, `atiadlxx.dll` aj `ze_loader.dll` sa ťahajú cez
`LoadLibrary` + `GetProcAddress`, nikdy ako import. Nie je to štýlová
preferencia: `compile.sh` kontroluje, či DLL neimportuje nič, čo nie je
vedľa `gkrellm.exe`, lebo Windows by taký plugin odmietol načítať **bez
akejkoľvek chybovej hlášky**. Statický import `atiadlxx.dll` by plugin
zabil na každom stroji bez AMD ovládača.

## Graf

Tri čiary na spoločnej pevnej škále 0–100. GKrellM má na graf len jednu
zvislú os, takže sa všetko prepočítava:

| čiara | farba | prepočet |
|---|---|---|
| Temp | červená `#e01c1c` | °C proti `TEMP_FULL_SCALE` (100 °C) |
| Load | biela `#ffffff` | percentá priamo |
| Power | sivá `#9a9a9a` | % z limitu príkonu |

Poradie v tabuľke je poradie vykresľovania spredu dozadu. Watty sú
najvzadu, teplota navrchu — indexy `SERIES_*` v `gkrellm-gpu.c` **sú**
to poradie, prečíslovaním sa graf preskladá a farby aj popisky idú
s nimi.

Ak karta nehlási limit príkonu, škáluje sa proti najvyššiemu odberu
videnému od štartu GKrellM.

## Formát panela

| token | | token | |
|---|---|---|---|
| `$n` | meno adaptéra | `$i` | index adaptéra |
| `$u` | záťaž % | `$t` | teplota °C |
| `$p` | príkon W | `$L` | limit príkonu W |
| `$f` | otáčky ventilátora % | `$c` | takt jadra MHz |
| `$m` | použitá pamäť MiB | `$M` | celková pamäť MiB |
| `$g` | použitá pamäť GiB | `$G` | celková pamäť GiB |
| `$$` | znak dolára | | |

Predvolené: `GPU$i $t°C $pW`. Čo karta nehlási, vypíše sa `--`.

## Build

```sh
./scripts/compile.sh --gkrellm-exe "/c/Program Files/GKrellM/gkrellm.exe" \
                     --source-tree ./work/gkrellm-2.5.1 \
                     --project . --out ./build
```

Rovnaké prostredie ako pri `gkrellm-nvidia` (MSYS2 MINGW64, 64-bit).
Výstup je `build/gkrellm-gpu.dll`.

## Čo overiť pri prvom spustení

Konfiguračná záložka pluginu vypisuje riadok **„Active data sources"** —
to je zoznam backendov, ktoré nabehli. Ak tam pri AMD alebo Intel karte
vidíš len `PDH`, vendor knižnica sa nenačítala alebo neprešla
enumerácia, a teplota s wattami ostanú prázdne.

### Známe krehké miesta

**ADL sensor ID.** `gpu-amd.c` číta `ADL2_New_QueryPMLogData_Get` a
indexuje pole senzorov konštantami `PMLOG_*`. Sú to čísla z ADL SDK,
teda ABI kontrakt s uzavretým ovládačom. Ak ich AMD niekedy preusporiada,
prejaví sa to ako vierohodne vyzerajúci nezmysel, nie ako pád — preto
sú v `amd_sample()` rozsahové kontroly (`clamp_or_na`), ktoré nezmysel
zahodia. AMD navyše `QueryPMLogData` označilo za deprecated v prospech
`ADL2_Overdrive8_PMLog_ShareMemory_Read`; zatiaľ funguje.

**Level Zero stype.** `gpu-intel.c` zámerne nevolá žiadne
`*GetProperties` — tie berú štruktúry, ktorých prvý člen je `stype` enum,
a netrafiť jeho hodnotu je tichý ABI nesúlad. Namiesto toho sa berie
najvyťaženejší engine a najteplejší senzor, čo properties nepotrebuje.

**Intel párovanie adaptérov.** Level Zero nedáva meno, na ktoré by sa
dalo napasovať DXGI, takže Intel karty sa mapujú poradím. Pri jednej
Intel karte v stroji je to spoľahlivé, pri dvoch nie.
