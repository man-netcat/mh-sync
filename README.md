# MH-Sync v1.0

Cross-platform Monster Hunter save sync between **Nintendo 3DS** and **Nintendo Switch** over local Wi-Fi. No PC or online service required.

---

## Features

- **Direct 3DS ↔ Switch sync** — transfers your save over your local network
- **Automatic conversion** — saves are converted between 3DS and Switch formats transparently
- **Safety backup** — the 3DS backs up your existing save before overwriting (It is recommended to manually back up your save with Checkpoint)
- **Persistent IP** — enter the Switch's IP once, it's saved for next time
- **Direction choice on 3DS** — you choose send or receive, with a confirmation step to prevent mistakes
- **Write confirmation** — the receiver confirms the save was written successfully
- **Auto-exit** — both consoles close automatically after sync

## Supported Games

- **3DS:** Monster Hunter XX (JPN), Monster Hunter Generations (USA), Monster Hunter X / Cross (JPN)
- **Switch:** Monster Hunter Generations Ultimate, Monster Hunter XX (JPN)

---

## How It Works

1. Launch the app on both consoles — the Switch acts as a server, the 3DS as a client
2. Enter the Switch's IP on the 3DS (once, it's saved)
3. Choose the sync direction on the 3DS and confirm
4. The save is transferred and converted automatically
5. Both consoles show the result and close

---

## Installation

- **3DS** with custom firmware (Luma3DS + boot9strap)
- **Switch** with custom firmware (Atmosphère), running in application mode (hold R while launching a game, then open hbmenu)

Copy the files to your SD card:

| File | Destination |
|---|---|
| `mh-sync-3ds.3dsx` | `sdmc:/3ds/mhsync/` |
| `mh-sync-switch.nro` | `sdmc:/switch/` |

Launch each from the Homebrew Launcher on the respective console. Both consoles need to be on the same network.

---

## Usage

### Switch
1. Launch `mh-sync-switch.nro`
2. Note the IP address displayed on screen
3. Press **A** to start waiting for the 3DS
4. Press **B** to exit

### 3DS
1. Launch `mh-sync-3ds.3dsx`
2. Press **X** to enter the Switch's IP address
3. Press **A** to start syncing
4. Choose **Send from 3DS to Switch** or **Receive from Switch to 3DS**, then confirm
5. Wait for the transfer — both consoles will close when done

---

## Building

### With Docker (recommended)

```bash
# Pull the images (one-time)
docker pull devkitpro/devkitarm
docker pull devkitpro/devkita64

# Build both targets
make
# or individually: make 3ds / make switch
```

Output: `build/3ds/mh-sync-3ds.3dsx` and `build/switch/mh-sync-switch.nro`.

### Without Docker

Requires [devkitPro](https://devkitpro.org/) with devkitARM + libctru (3DS) and devkitA64 + libnx (Switch).

```bash
export DEVKITARM=/opt/devkitpro/devkitARM
export DEVKITPRO=/opt/devkitpro

make 3ds
make switch
```

## Credits

Save conversion is based on reverse-engineering by the **MHXXSaveEditor** project, the Python converter by Alexander-Lancellott, and save data research from the [GBAtemp community](https://gbatemp.net/threads/mhxx-mhgu-save-manager.668510/).

- **Checkpoint** (FlagBrew) — reference for 3DS save handling
- **MHXXSaveEditor** — reverse-engineering of the save format
- **Alexander-Lancellott** — original Python save converter
- **GBAtemp community** — save data research and documentation
