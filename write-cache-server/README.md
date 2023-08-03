# Write cache server

Set of small scripts to enable NVMe-over-TCP on the write cache server. This is
done using [`nvmetcli`](http://git.infradead.org/users/hch/nvmetcli.git).
Alternatively, this can also be achieved through [`SPDK`](https://spdk.io/)
with local block device (or ramdisk) target.

## Installation

If using Ubuntu/Debian/Fedora:

```
$ sh install.sh
```

Otherwise, you may need to install `nvmecli` and `configshell-fb` manually
before running `install.sh`.

## Configuration

Simply run the following and provide required fields:

``` $ sh setup.sh ```

Once ran, you can open the device to access by `nvmetcli restore
config-name.conf`.