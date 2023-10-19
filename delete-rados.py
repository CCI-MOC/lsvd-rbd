#!/usr/bin/python3

import sys
import rados
import argparse

if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1].count(':') != 1:
        print("usage: %s pool:prefix\n" % sys.argv[0])
        sys.exit(1)

    pool, prefix = sys.argv[1].split(':')

    cluster = rados.Rados(conffile='')
    cluster.connect()
    if not cluster.pool_exists(pool):
        raise RuntimeError('Pool not found ' + pool)
    ioctx = cluster.open_ioctx(pool)
    for obj in ioctx.list_objects():
        if obj.key.startswith(prefix):
            ioctx.remove_object(obj.key)
            print('.', end='', flush=True)
    print('\ndone')
