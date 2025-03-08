#!/bin/bash

# Binary removing function
uninstall_binary() {
    if [ -f "/usr/local/bin/$1" ]; then
        echo "Deleting $1..."
        sudo rm "/usr/local/bin/$1"
        echo "$1 has been deleted."
    else
        echo "$1 is not installed."
    fi
}

# function to remove PAM configuration
remove_pam_config() {
    if [ -f "/etc/pam.d/$1" ]; then
        echo "PAM configuration being deleted for $1..."
        sudo rm "/etc/pam.d/$1"
        echo "PAM configuration of $1 deleted."
    else
        echo "PAM configuration of $1 not found."
    fi
}

# deleting binaries
uninstall_binary "kaylock"

# Supprimer la configuration PAM
remove_pam_config "kaylock"

echo "Uninstallation done."
echo "If you have set up shortcuts in KDE using kaylock, you have to delete them manually."
