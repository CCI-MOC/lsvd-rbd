#!/bin/bash

read -p "Provide NVMe device path: " devpath
UUID="$(blkid "$devpath" | cut -d \" -f2)"
[ ! $UUID ] && echo "$devpath not found" && exit 1
read -p "Provide IP address to bind to: " ipaddr
read -p "Provide port number to bind to: " portnum
read -p "Provide NVMe subsystem name (default: $(basename $devpath)): " nqnname
[ ! $nqnname ] && nqnname="$(basename $devpath)"

tee "$nqnname.conf" > /dev/null <<END
{
    "hosts": [],
    "ports": [
        {
            "addr": {
                "adrfam": "ipv4",
                "traddr": "$ipaddr",
                "treq": "not specified",
                "trsvcid": "$portnum",
                "trtype": "tcp"
            },
            "ana_groups": [
            {
                "ana": {
                    "state": "optimized"
                },
                "grpid": 1
            }
            ],
            "param": {
                "inline_data_size": "16384",
                "pi_enable": "0"
            },
            "portid": 1,
            "referrals": [],
            "subsystems": [
                "$nqnname"
            ]
        }
    ],
    "subsystems": [
        {
            "allowed_hosts": [],
            "attr": {
                "allow_any_host": "1",
                "cntlid_max": "65519",
                "cntlid_min": "1",
                "model": "Linux",
                "pi_enable": "0",
            },
            "namespaces": [
                {
                    "ana_grpid": 1,
                    "device": {
                        "path": "$devpath",
                        "uuid": "$UUID"
                    },
                    "enable": 1,
                    "nsid": 1
                }
            ],
            "nqn": "$nqnname"
        }
    ]
}
END

echo "Configuration written to $nqnname.conf. To enable the configuration: nvmetcli restore $nqnname.conf"