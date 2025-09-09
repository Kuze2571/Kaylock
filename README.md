 # Kaylock

This tool is an attempt to recreate a simple screen locker as xtrlock in KDE Plasma on Wayland.

## Prerequisites

You will need to install the following libraries from PAM & Wayland :

```bash
sudo apt install build-essential wayland-protocols libwayland-dev libpam0g-dev pkg-config libxkbcommon-dev libcairo2-dev libpango1.0-dev
```

## Installation

1. Clone this repository and navigate to the src folder:

```bash
git clone
cd kaylock/src
```

2. You can proceed to the compilation:
```bash
make
sudo make install
```

This command will:
- Install the executable in `/usr/local/bin/kaylock`
- Create the PAM configuration file `/etc/pam.d/kaylock`

## Usage

To lock your screen, simply proceed by typing the following command:

```bash
kaylock
```

To unlock, type your session password and press ENTER.

However, to test the program you can use the debug mode which includes a bypass in case the program wouldnt unlock:
```bash
kaylock --debug
```

In debug mode, the keys pressed will be shown in the terminal.
To unlock the screen using the bypass method, type 'debug123'.

## Shortcut in KDE Plasma

To configure a shortcut in KDE Plasma:

1. Open the system settings
2. Go to the shortcuts section
3. Click on "Add"
4. Name it as "Kaylock" for example
5. In the "Command" field, type `kaylock`
6. Assign a keyboard shortcut, for example Ctrl+Alt+L
7. Click on "Apply"

## Uninstall

To uninstall the program, you can use the `uninstall.sh` script:

```bash
chmod +x uninstall.sh
./uninstall.sh
```

This will erase the binaries and remove the PAM configuration.
You will still have toremove the shortcuts manually.