#!/usr/bin/env python3

import rados, sys

pool_name = sys.argv[1]
img_name = sys.argv[2]

if pool_name == '' or img_name == '':
	print('Usage: remove_objs.py <pool_name> <image_name>')
	exit(1)

print(f'Removing all objects from pool {pool_name} with prefix {img_name}')

cluster = rados.Rados(conffile='/etc/ceph/ceph.conf')
cluster.connect()

ioctx = cluster.open_ioctx(pool_name)

num = 0
removed = 0

for obj in ioctx.list_objects():
	num += 1
	name = obj.key

	if name.startswith(img_name):
		obj.remove()
		removed += 1

	if num % 1000 == 0:
		print('+', end='', flush=True)

print(f'\nRemoved {removed}/{num} objects')

ioctx.close()
