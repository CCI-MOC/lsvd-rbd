#!/bin/bash

DIST=$(( lsb_release -ds || cat /etc/*release || uname -om ) 2>/dev/null | head -n1)

if [[ $DIST ~= .*"Ubuntu".* || $DIST ~= .*"Debian".* ]]; then
    echo "Ubuntu/Debian detected"
    sudo apt install -y nvme-cli python3-configshell-fb

elif [[ $DIST ~= .*"Fedora".* ]]; then
    echo "Fedora detected"
    sudo dnf -y nvme-cli python3-configshell
else
    echo "Unsupport platform. Please manually install nvmecli and configshell-fb"
fi

echo "Installing nvmetcli..."
git clone git://git.infradead.org/users/hch/nvmetcli.git && cd nvmetcli && python3 setup.py install