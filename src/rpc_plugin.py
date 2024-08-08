from spdk.rpc.client import print_json

def spdk_rpc_plugin_initialize(subparsers):
    def bdev_lsvd_create(args):
        print_json(args.client.call('bdev_lsvd_create', {
            'name': args.name,
            'pool_name': args.pool_name,
            'cfg_path': args.cfg
        }))

    p = subparsers.add_parser('bdev_lsvd_create', help='Create a bdev with LSVD backend')
    p.add_argument('pool_name', help='Name of the ceph pool')
    p.add_argument('name', help='Name of the lsvd disk image')
    p.add_argument('-c', '--cfg', help='Path to config file', required=False)
    p.set_defaults(func=bdev_lsvd_create)

    def bdev_lsvd_delete(args):
        print_json(args.client.call('bdev_lsvd_delete', {
            'name': args.name
        }))

    p = subparsers.add_parser('bdev_lsvd_delete', help='Delete a lsvd bdev')
    p.add_argument('name', help='Name of the lsvd disk image')
    p.set_defaults(func=bdev_lsvd_delete)



